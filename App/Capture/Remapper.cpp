#ifndef GR_BUILD_DLL
#define GR_BUILD_DLL
#endif
#include "Bridge.h"
#include "ControlCodec.h"
#include "HidLayout.h"
#include "SnapshotNotification.h"
#include <cfgmgr32.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>

namespace {
using namespace Interceptor;
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
Remapper::SnapshotNotification snapshotNotification;
template <std::size_t N> void Text(wchar_t (&target)[N], const std::wstring &value) noexcept {
    const auto count = std::min(N - 1, value.size());
    std::copy_n(value.data(), count, target);
    target[count] = 0;
}
std::wstring Wide(const std::string &text) {
    if (text.empty())
        return {};
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!length)
        return L"Unspecified native error";
    std::wstring value(static_cast<std::size_t>(length), 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), value.data(), length);
    return value;
}
std::wstring Explain(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_SERVICE_DOES_NOT_EXIST:
        return L"Interceptor Gamepad Controller is not installed. Run its UI installer, then restart "
               L"Windows "
               L"if requested.";
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return L"Administrator rights are required to open the Interceptor control device. Use Inject "
               L"and "
               L"accept the Windows elevation prompt.";
    case IC_ERROR_TRIAL_EXPIRED:
        return L"The Interceptor two-hour demo has expired for this Windows boot. Restart Windows to "
               L"begin a "
               L"new trial session.";
    case ERROR_BUSY:
    case ERROR_SHARING_VIOLATION:
        return L"The capture or controller output queue is busy. Stop other capture applications; if "
               L"this "
               L"happened while forwarding, increase the macro tick interval.";
    case ERROR_NOT_SUPPORTED:
    case ERROR_REVISION_MISMATCH:
        return L"The installed Interceptor edition or ABI does not match GamePad Remapper. Install the "
               L"bundled Gamepad Controller edition.";
    case ERROR_SUCCESS_REBOOT_REQUIRED:
        return L"Windows must restart to finish associating the controller filters. Restart Windows, "
               L"then "
               L"inject again.";
    case ERROR_OPERATION_ABORTED:
        return L"The driver revoked capture. Input is now passed through. Check the driver demo, device "
               L"connection and diagnostics.";
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_NOT_READY:
        return L"A controller source became unavailable. Input is passed through. Reconnect it; capture "
               L"can "
               L"be injected again.";
    default:
        break;
    }
    wchar_t *raw = nullptr;
    const auto n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                      FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, error, 0, reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
    std::wstring text = n ? std::wstring(raw, n) : L"Windows error " + std::to_wstring(error);
    if (raw)
        LocalFree(raw);
    return text;
}
DWORD Probe(std::wstring &message) {
    try {
        const auto caps = Get_Capabilities();
        if (caps.abi_version != IC_VERSION || caps.input_mask != 0 || caps.controller_mask != 15) {
            message = Explain(ERROR_REVISION_MISMATCH);
            return ERROR_REVISION_MISMATCH;
        }
        if (!(caps.xusb_profiles & IC_PROFILE_XUSB_USB_FILTER)) {
            message = L"The loaded Interceptor Gamepad Controller driver uses an older XUSB backend. "
                      L"Run the bundled driver installer, restart Windows and reopen the installer "
                      L"until association is complete, then inject again.";
            return ERROR_REVISION_MISMATCH;
        }
        const auto trial = Get_Trial_Status();
        if (trial.state == IC_TRIAL_EXPIRED) {
            message = Explain(IC_ERROR_TRIAL_EXPIRED);
            return IC_ERROR_TRIAL_EXPIRED;
        }
        message =
            L"Interceptor Gamepad Controller is ready. Capture starts only when Inject is selected.";
        return 0;
    } catch (const std::system_error &error) {
        const auto code = static_cast<DWORD>(error.code().value());
        message = Explain(code);
        return code;
    } catch (const std::exception &error) {
        message = Wide(error.what());
        return ERROR_GEN_FAILURE;
    }
}

using Payload = std::variant<Controller_Hid_Input, Controller_Xinput_Input, Controller_XinputHid_Input,
                             Controller_GIP_Input, Controller_GIP_Raw_Input>;
struct Report {
    unsigned id = 0;
    Payload physical;
    std::array<std::uint8_t, 256> controls{};
    std::array<int, 256> lastOutput{};
    std::vector<unsigned> relative;
};
struct Source {
    std::uint32_t key = 0, family = 0, id = 0;
    std::string token;
    std::wstring name, diagnostic;
    Remapper::HidSlots slots;
    std::array<int, 256> physical{}, output{};
    std::array<int, 256> observed{}, forwarded{};
    bool physicalValid = false, outputValid = false;
    std::array<std::uint8_t, 256> available{}, outputKnown{};
    std::vector<Report> reports;
    std::vector<unsigned> relative;
    std::array<bool, 256> relativeKnown{};
    Time deadline = Time::max();
    bool layoutAttempted = false, layoutSeeded = false;
};
class Service {
    GR_Callback callback_;
    void *user_;
    HANDLE wake_ = nullptr, timer_ = nullptr;
    PTP_WAIT timerWait_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<unsigned> reasons_{0};
    std::mutex publishedMutex_, startMutex_;
    std::condition_variable started_;
    bool startComplete_ = false;
    DWORD startError_ = 0;
    GR_Snapshot snapshot_{};
    std::vector<GR_Device> deviceSnapshot_;
    std::unordered_map<std::uint64_t, std::unique_ptr<Source>> sources_;
    std::uint32_t nextKey_ = 1;
    std::uint64_t received_ = 0, forwarded_ = 0, changed_ = 0, timerOutputs_ = 0, faults_ = 0,
                  measured_ = 0;
    double processingUs_ = 0;
    bool snapshotDirty_ = false;
    Time armedDeadline_ = Time::max();
    bool preciseTimer_ = true;
    static void CALLBACK Timer(PTP_CALLBACK_INSTANCE, void *context, PTP_WAIT, TP_WAIT_RESULT) noexcept {
        auto &self = *static_cast<Service *>(context);
        self.reasons_.fetch_or(1, std::memory_order_release);
        SetEvent(self.wake_);
    }
    std::wstring status_ = L"Starting capture", error_;
    void Forget(Source &source) noexcept {
        try {
            (void)callback_(source.key, source.family, source.physical.data(), source.available.data(),
                            source.output.data(), 3, user_);
        } catch (...) {
        }
    }
    void Publish(bool force = false) {
        if (!force && !snapshotDirty_)
            return;
        std::vector<GR_Device> devices;
        devices.reserve(sources_.size());
        for (const auto &entry : sources_) {
            const auto &source = *entry.second;
            GR_Device device{};
            device.size = sizeof(device);
            device.key = source.key;
            device.family = source.family;
            device.flags = (source.physicalValid ? GR_PHYSICAL_VALID : 0) |
                           (source.outputValid ? GR_OUTPUT_VALID : 0);
            Text(device.name, source.name);
            Text(device.token, Wide(source.token));
            Text(device.diagnostic, source.diagnostic);
            std::copy(source.observed.begin(), source.observed.end(), device.values);
            for (std::size_t i = 0; i < source.available.size(); ++i)
                device.available[i] = source.available[i] | (source.outputKnown[i] ? 2 : 0);
            std::copy(source.forwarded.begin(), source.forwarded.end(), device.output_values);
            devices.push_back(device);
        }
        std::sort(devices.begin(), devices.end(),
                  [](const auto &a, const auto &b) { return a.key < b.key; });
        std::lock_guard<std::mutex> lock(publishedMutex_);
        snapshot_.size = sizeof(snapshot_);
        snapshot_.version = 2;
        snapshot_.running = !stopping_.load() && error_.empty();
        snapshot_.device_count = static_cast<std::uint32_t>(devices.size());
        snapshot_.received = received_;
        snapshot_.forwarded = forwarded_;
        snapshot_.changed = changed_;
        snapshot_.timer_outputs = timerOutputs_;
        snapshot_.faults = faults_;
        snapshot_.processing_microseconds =
            measured_ ? processingUs_ / static_cast<double>(measured_) : 0;
        Text(snapshot_.status, status_);
        Text(snapshot_.error, error_);
        deviceSnapshot_ = std::move(devices);
        snapshotDirty_ = false;
        snapshotNotification.Changed();
    }
    Source &GetSource(const Input_Event &input) {
        const auto family = static_cast<unsigned>(input.type);
        const auto key = Remapper::SourceKey(family, input.device);
        if (sources_.find(key) == sources_.end() && sources_.size() >= 256)
            throw std::runtime_error(
                "The capture source limit of 256 was exceeded. Capture was released; "
                "physical input passes through.");
        auto &slot = sources_[key];
        const std::string fallback = input.device_info
                                         ? std::string{}
                                         : std::to_string(family) + ":" + std::to_string(input.device);
        const auto &token = input.device_info ? input.device_info->token : fallback;
        if (!slot || slot->token != token) {
            if (slot)
                Forget(*slot);
            slot = std::make_unique<Source>();
            slot->family = family;
            slot->id = input.device;
            slot->key = nextKey_++;
            slot->token = token;
            slot->name = input.device_info
                             ? std::wstring(reinterpret_cast<const wchar_t *>(input.device_info->name))
                             : L"Controller";
            slot->reports.reserve(4);
        }
        return *slot;
    }
    void RegisterInitialDevices(const Context_Info &context) {
        for (const auto &device : context.devices) {
            const auto identity = Remapper::SourceKey(device.type, device.id);
            auto source = std::make_unique<Source>();
            source->family = device.type;
            source->id = device.id;
            source->key = nextKey_++;
            source->token = device.token;
            source->name = std::wstring(reinterpret_cast<const wchar_t *>(device.name));
            source->diagnostic = L"Capture is active. Waiting for the first physical report to expose "
                                 L"this source's controls.";
            source->reports.reserve(4);
            sources_.emplace(identity, std::move(source));
        }
    }
    template <class T> static unsigned ReportId(const T &input) noexcept {
        if constexpr (std::is_base_of_v<Hid_Decoded_Input, T>)
            return input.length ? input.report[0] : 0;
        else
            return 0;
    }
    template <class T> void Read(Source &source, const T &input, Report &report) {
        report.controls.fill(0);
        if constexpr (std::is_base_of_v<Hid_Decoded_Input, T>) {
            if (!source.layoutAttempted) {
                source.layoutAttempted = true;
                try {
                    const auto descriptor = Get_descriptor(source.token);
                    if (descriptor)
                        source.layoutSeeded = Remapper::SeedHidLayout(source.slots, descriptor.data);
                } catch (const std::system_error &) {
                }
            }
            source.slots.Read(input, source.physical, source.available);
            for (const auto &button : input.state.buttons) {
                const auto slot = source.slots.Find(Remapper::Key(button));
                if (slot < 256)
                    report.controls[slot] = 1;
            }
            const std::vector<Hid_Value> *lists[]{&input.state.axes, &input.state.triggers,
                                                  &input.state.hats, &input.state.values};
            for (unsigned i = 0; i < 4; ++i)
                for (const auto &value : *lists[i]) {
                    const auto slot = source.slots.Find(Remapper::Key(value, i + 1));
                    if (slot < 256)
                        report.controls[slot] = 1;
                }
            source.diagnostic =
                L"Descriptor-driven HID: use BUTTON_n, AXIS_n, TRIGGER_n, HAT_n and VALUE_n. "
                L"Console face positions are not inferred.";
            if (!source.layoutSeeded)
                source.diagnostic +=
                    L" Descriptor layout could not be preseeded; slot order is local to this capture.";
            if (input.decode_error || !input.decode_complete)
                source.diagnostic += L" Some fields could not be decoded (Windows " +
                                     std::to_wstring(input.decode_error) +
                                     L"); unknown bytes are passed through.";
            if (source.slots.truncated)
                source.diagnostic +=
                    L" Script control limits reached; additional controls remain unchanged.";
            if (!input.state.binary_values.empty())
                source.diagnostic += L" Wide binary fields are preserved unchanged.";
        } else {
            constexpr bool gip =
                std::is_same_v<T, Controller_GIP_Input> || std::is_same_v<T, Controller_GIP_Raw_Input>;
            const auto &state = [&]() -> const auto & {
                if constexpr (std::is_same_v<T, Controller_GIP_Raw_Input>)
                    return input.input.state;
                else
                    return input.state;
            }();
            Remapper::ReadNative(state, gip, source.physical, source.available);
            report.controls = source.available;
            source.diagnostic =
                gip ? L"Native GIP standard controls; opaque physical report extensions and transport "
                      L"sequencing are retained."
                    : L"Native XUSB standard controls; unedited fields retain their exact physical "
                      L"values.";
        }
    }
    template <class T> static void Apply(Source &source, T &input) {
        if constexpr (std::is_base_of_v<Hid_Decoded_Input, T>)
            source.slots.Write(input, source.physical, source.output);
        else {
            constexpr bool gip =
                std::is_same_v<T, Controller_GIP_Input> || std::is_same_v<T, Controller_GIP_Raw_Input>;
            auto &state = [&]() -> auto & {
                if constexpr (std::is_same_v<T, Controller_GIP_Raw_Input>)
                    return input.input.state;
                else
                    return input.state;
            }();
            Remapper::WriteNative(state, gip, source.physical, source.output);
        }
    }
    template <class T> static void Accepted(Source &source, const T &input) {
        if constexpr (std::is_base_of_v<Hid_Decoded_Input, T>)
            source.slots.ReadForwarded(input, source.physical, source.output, source.forwarded,
                                       &source.outputKnown);
        else {
            constexpr bool gip = std::is_same_v<T, Controller_GIP_Input> ||
                                 std::is_same_v<T, Controller_GIP_Raw_Input>;
            const auto &state = [&]() -> const auto & {
                if constexpr (std::is_same_v<T, Controller_GIP_Raw_Input>)
                    return input.input.state;
                else
                    return input.state;
            }();
            Remapper::ReadNative(state, gip, source.forwarded, source.available);
            source.outputKnown = source.available;
        }
        source.outputValid = true;
    }
    static void AcceptedEvent(Source &source, const Input_Event &input) {
        if (auto hid = input.Get<Controller_Hid_Input>())
            Accepted(source, *hid);
        else if (auto xusb = input.Get<Controller_Xinput_Input>())
            Accepted(source, *xusb);
        else if (auto xih = input.Get<Controller_XinputHid_Input>())
            Accepted(source, *xih);
        else if (auto raw = input.Get<Controller_GIP_Raw_Input>())
            Accepted(source, *raw);
        else if (auto gip = input.Get<Controller_GIP_Input>())
            Accepted(source, *gip);
    }
    bool Execute(Source &source, unsigned flags) {
        snapshotDirty_ = true;
        source.observed = source.physical;
        Remapper::ClearRelative(source.forwarded, source.relative);
        source.output = source.physical;
        const int next = callback_(source.key, source.family, source.physical.data(),
                                   source.available.data(), source.output.data(), flags, user_);
        if (next < 0) {
            source.output = source.physical;
            return false;
        }
        for (unsigned i = 0; i < 256; ++i)
            if (!source.available[i])
                source.output[i] = source.physical[i];
        source.deadline = next ? Clock::now() + std::chrono::milliseconds(next) : Time::max();
        return true;
    }
    static bool Different(const Report &report, const std::array<int, 256> &a,
                          const std::array<int, 256> &b) noexcept {
        for (unsigned i = 0; i < 256; ++i)
            if (report.controls[i] && a[i] != b[i])
                return true;
        return false;
    }
    static void RequireSend(const Send_Result &sent) {
        if (sent.error || sent.accepted != 1)
            throw std::system_error(sent.error ? sent.error : ERROR_WRITE_FAULT, std::system_category(),
                                    "Forward controller report");
    }
    template <class T> bool Physical(Source &source, T &input) {
        Remapper::ClearRelative(source.physical, source.relative);
        const auto reportId = ReportId(input);
        auto position = std::find_if(source.reports.begin(), source.reports.end(),
                                     [&](const auto &report) { return report.id == reportId; });
        if (position == source.reports.end()) {
            source.reports.emplace_back();
            position = source.reports.end() - 1;
            position->id = reportId;
        }
        auto &report = *position;
        report.physical = input;
        Read(source, input, report);
        source.physicalValid = true;
        if constexpr (std::is_base_of_v<Hid_Decoded_Input, T>) {
            source.slots.PrepareSynthetic(std::get<T>(report.physical), report.relative);
            for (const auto slot : report.relative)
                if (!source.relativeKnown[slot]) {
                    source.relativeKnown[slot] = true;
                    source.relative.push_back(slot);
                }
        }
        report.lastOutput = source.physical;
        const bool good = Execute(source, 0);
        if (good) {
            if (Different(report, source.physical, source.output))
                ++changed_;
            Apply(source, input);
        }
        report.lastOutput = source.output;
        Remapper::ClearRelative(report.lastOutput, report.relative);
        Remapper::ClearRelative(source.physical, report.relative);
        Remapper::ClearRelative(source.output, report.relative);
        return good;
    }
    void RestoreAndReset(bool applyNew) {
        for (auto &entry : sources_) {
            auto &source = *entry.second;
            source.deadline = Time::max();
            Remapper::ClearRelative(source.physical, source.relative);
            Remapper::ClearRelative(source.observed, source.relative);
            Remapper::ClearRelative(source.forwarded, source.relative);
            source.output = source.physical;
            for (auto &report : source.reports) {
                if (!Different(report, report.lastOutput, source.physical))
                    continue;
                std::visit([&](const auto &input) {
                    RequireSend(Send_Input(input));
                    Accepted(source, input);
                }, report.physical);
                ++forwarded_;
                report.lastOutput = source.physical;
            }
            if (applyNew && !Execute(source, 2))
                throw std::runtime_error(
                    "The script runtime rejected the configuration reset. Capture has stopped.");
            if (applyNew)
                SendScheduled(source);
        }
    }
    void SendScheduled(Source &source) {
        for (auto &report : source.reports) {
            if (!Different(report, report.lastOutput, source.output))
                continue;
            // Reuse one scratch per type: after warmup, assignment reuses HID
            // vector capacities. Templates retain absolute physical data and
            // neutral relative scalars, whose original movement was sent once.
            std::visit(
                [&](const auto &physical) {
                    using T = std::decay_t<decltype(physical)>;
                    thread_local T edited;
                    edited = physical;
                    Apply(source, edited);
                    RequireSend(Send_Input(edited));
                    Accepted(source, edited);
                },
                report.physical);
            report.lastOutput = source.output;
            Remapper::ClearRelative(report.lastOutput, report.relative);
            ++forwarded_;
            ++changed_;
            ++timerOutputs_;
        }
    }
    void ArmTimer() {
        // Only script deadlines need a timer. UI snapshots are published after
        // forwarding the current batch and never delay the final quiet update.
        auto earliest = Time::max();
        for (const auto &item : sources_)
            earliest = std::min(earliest, item.second->deadline);
        if (earliest == armedDeadline_)
            return;
        armedDeadline_ = earliest;
        if (earliest == Time::max()) {
            CancelWaitableTimer(timer_);
            SetThreadpoolWait(timerWait_, nullptr, nullptr);
            return;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::nanoseconds>(earliest - Clock::now()).count();
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(1, (remaining + 99) / 100);
        if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
            throw std::system_error(GetLastError(), std::system_category(), "Arm macro timer");
        SetThreadpoolWait(timerWait_, timer_, nullptr);
    }
    void CompleteStart(DWORD error) noexcept {
        std::lock_guard<std::mutex> lock(startMutex_);
        if (startComplete_)
            return;
        startError_ = error;
        startComplete_ = true;
        started_.notify_all();
    }
    void Run() noexcept {
        DWORD startError = ERROR_GEN_FAILURE;
        try {
            const auto initialized = Init_AllDevices();
            if (initialized.error) {
                std::string detail = "Initialize installed controller filters";
                if (!initialized.error_detail.empty())
                    detail += ": " + initialized.error_detail;
                if (!initialized.error_device.empty()) {
                    const auto &device = initialized.error_device;
                    const int length = WideCharToMultiByte(CP_UTF8, 0, device.data(),
                        static_cast<int>(device.size()), nullptr, 0, nullptr, nullptr);
                    if (length > 0) {
                        std::string instance(static_cast<std::size_t>(length), 0);
                        WideCharToMultiByte(CP_UTF8, 0, device.data(), static_cast<int>(device.size()),
                                            instance.data(), length, nullptr, nullptr);
                        detail += " [" + instance + "]";
                    }
                }
                throw std::system_error(initialized.error, std::system_category(),
                                        detail);
            }
            if (initialized.reboot_required)
                throw std::system_error(ERROR_SUCCESS_REBOOT_REQUIRED, std::system_category(),
                                        "Controller filters require a restart");
            RegisterInitialDevices(Get_Context());
            status_ = L"Injected · waiting for controller input";
            if (!preciseTimer_)
                status_ += L" · standard Windows timer fallback";
            Publish(true);
            CompleteStart(0);
            while (!stopping_.load(std::memory_order_acquire)) {
                const unsigned reason = reasons_.exchange(0, std::memory_order_acq_rel);
                if (reason & 1)
                    armedDeadline_ = Time::max();
                if (reason & 2)
                    RestoreAndReset(true);
                else if (reason & 4)
                    for (auto &entry : sources_) {
                        auto &source = *entry.second;
                        Remapper::ClearRelative(source.physical, source.relative);
                        if (!Execute(source, 4))
                            throw std::runtime_error(
                                "The script failed after a parameter change; capture stopped.");
                        SendScheduled(source);
                    }
                const auto now = Clock::now();
                for (auto &entry : sources_)
                    if (entry.second->deadline <= now) {
                        auto &source = *entry.second;
                        if (!Execute(source, 1))
                            throw std::runtime_error("The script runtime failed during a scheduled "
                                                     "macro. Capture has stopped.");
                        SendScheduled(source);
                    }
                Publish();
                ArmTimer();
                bool scriptFailed = false;
                {
                    auto inputs = Get_Input_With_Timeout(INFINITE, wake_);
                    for (auto &input : inputs) {
                        if (stopping_.load())
                            break;
                        const auto begin = Clock::now();
                        auto &source = GetSource(input);
                        ++received_;
                        bool good = true;
                        if (auto hid = input.Get<Controller_Hid_Input>())
                            good = Physical(source, *hid);
                        else if (auto xusb = input.Get<Controller_Xinput_Input>())
                            good = Physical(source, *xusb);
                        else if (auto xih = input.Get<Controller_XinputHid_Input>())
                            good = Physical(source, *xih);
                        else if (auto raw = input.Get<Controller_GIP_Raw_Input>())
                            good = Physical(source, *raw);
                        else if (auto gip = input.Get<Controller_GIP_Input>())
                            good = Physical(source, *gip);
                        // A callback fault must forward this physical report first,
                        // then release the entire capture to restore passthrough.
                        RequireSend(Send_Input(input));
                        AcceptedEvent(source, input);
                        ++forwarded_;
                        if (good)
                            SendScheduled(source);
                        processingUs_ +=
                            std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
                        ++measured_;
                        if (!good) {
                            scriptFailed = true;
                            break;
                        }
                    }
                }
                if (scriptFailed)
                    throw std::runtime_error(
                        "The script runtime failed. The physical report was forwarded "
                        "and capture has stopped.");
            }
            status_ = L"Stopped · physical input passes through";
        } catch (const std::system_error &error) {
            startError = static_cast<DWORD>(error.code().value());
            ++faults_;
            error_ =
                Explain(startError) + L" (" + std::to_wstring(startError) + L")\n" + Wide(error.what());
            status_ = L"Capture stopped · physical input passes through";
        } catch (const std::exception &error) {
            ++faults_;
            error_ = Wide(error.what());
            status_ = L"Script stopped · physical input passes through";
        } catch (...) {
            ++faults_;
            error_ = L"Unexpected capture failure. Physical input passes through.";
            status_ = L"Capture stopped";
        }
        stopping_.store(true);
        SetThreadpoolWait(timerWait_, nullptr, nullptr);
        CancelWaitableTimer(timer_);
        // Release synthetic held state even when no further physical report is
        // generated. A revoked/disconnected capture may reject restoration;
        // closing its handle is the final driver-owned passthrough guarantee.
        try {
            RestoreAndReset(false);
        } catch (...) {
            for (auto &entry : sources_)
                entry.second->outputValid = false;
        }
        try {
            Destroy_Context();
        } catch (...) {
        }
        for (auto &entry : sources_)
            entry.second->deadline = Time::max();
        for (auto &entry : sources_)
            Forget(*entry.second);
        try {
            Publish(true);
        } catch (...) {
        }
        // A failed Start must expose its final ABI/status before waking the
        // caller, which immediately reads this snapshot to explain the failure.
        CompleteStart(startError);
    }

  public:
    Service(GR_Callback callback, void *user) : callback_(callback), user_(user) {
        snapshot_.size = sizeof(snapshot_);
        snapshot_.version = 2;
        Text(snapshot_.status, status_);
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake_)
            throw std::system_error(GetLastError(), std::system_category());
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
        if (!timer_) {
            preciseTimer_ = false;
            timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
        if (!timer_) {
            CloseHandle(wake_);
            throw std::system_error(GetLastError(), std::system_category());
        }
        timerWait_ = CreateThreadpoolWait(Timer, this, nullptr);
        if (!timerWait_) {
            CloseHandle(timer_);
            CloseHandle(wake_);
            throw std::system_error(GetLastError(), std::system_category());
        }
    }
    ~Service() {
        Stop();
        SetThreadpoolWait(timerWait_, nullptr, nullptr);
        WaitForThreadpoolWaitCallbacks(timerWait_, TRUE);
        CloseThreadpoolWait(timerWait_);
        CloseHandle(timer_);
        CloseHandle(wake_);
    }
    DWORD Start() {
        thread_ = std::thread([this] { Run(); });
        std::unique_lock<std::mutex> lock(startMutex_);
        started_.wait(lock, [this] { return startComplete_; });
        return startError_;
    }
    void Stop() noexcept {
        stopping_.store(true, std::memory_order_release);
        SetEvent(wake_);
        if (thread_.joinable())
            thread_.join();
    }
    void Wake(unsigned reason = 2) noexcept {
        reasons_.fetch_or(reason, std::memory_order_release);
        SetEvent(wake_);
    }
    void Snapshot(GR_Snapshot &output) {
        std::lock_guard<std::mutex> lock(publishedMutex_);
        output = snapshot_;
    }
    bool Device(unsigned index, GR_Device &output) {
        std::lock_guard<std::mutex> lock(publishedMutex_);
        if (index >= deviceSnapshot_.size())
            return false;
        output = deviceSnapshot_[index];
        return true;
    }
};
std::mutex apiMutex;
std::unique_ptr<Service> service;
} // namespace

void __cdecl GR_SetSnapshotWindow(HWND window, UINT message) {
    snapshotNotification.Attach(window, message);
}
BOOL __cdecl GR_AcknowledgeSnapshot(HWND window, ULONG_PTR generation) {
    return snapshotNotification.Acknowledge(window, generation) ? TRUE : FALSE;
}
void __cdecl GR_MonitorSnapshotChanged() { snapshotNotification.Changed(); }

std::uint32_t __cdecl GR_Probe(wchar_t *message, std::uint32_t characters) {
    try {
        std::wstring text;
        const auto code = Probe(text);
        if (message && characters) {
            const auto n = std::min<std::size_t>(characters - 1, text.size());
            std::copy_n(text.data(), n, message);
            message[n] = 0;
        }
        return code;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}
std::uint32_t __cdecl GR_Start(GR_Callback callback, void *user) {
    if (!callback)
        return ERROR_INVALID_PARAMETER;
    try {
        std::lock_guard<std::mutex> lock(apiMutex);
        if (service) {
            GR_Snapshot previous{};
            service->Snapshot(previous);
            if (previous.running)
                return ERROR_BUSY;
            service.reset();
        }
        std::wstring message;
        const auto probe = Probe(message);
        if (probe)
            return probe;
        auto candidate = std::make_unique<Service>(callback, user);
        const auto error = candidate->Start();
        service = std::move(candidate);
        return error;
    } catch (const std::bad_alloc &) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}
void __cdecl GR_Stop() {
    try {
        std::lock_guard<std::mutex> lock(apiMutex);
        if (service)
            service->Stop();
    } catch (...) {
    }
}
void __cdecl GR_Wake() {
    try {
        std::lock_guard<std::mutex> lock(apiMutex);
        if (service)
            service->Wake();
    } catch (...) {
    }
}
void __cdecl GR_ParametersChanged() {
    try {
        std::lock_guard<std::mutex> lock(apiMutex);
        if (service)
            service->Wake(4);
    } catch (...) {
    }
}
std::uint32_t __cdecl GR_GetSnapshot(GR_Snapshot *snapshot) {
    if (!snapshot || snapshot->size != sizeof(GR_Snapshot))
        return ERROR_INVALID_PARAMETER;
    try {
        std::lock_guard<std::mutex> lock(apiMutex);
        if (service)
            service->Snapshot(*snapshot);
        else {
            *snapshot = {};
            snapshot->size = sizeof(*snapshot);
            snapshot->version = 2;
            Text(snapshot->status, L"Ready · no configuration injected");
        }
        return 0;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}
std::uint32_t __cdecl GR_GetDevice(std::uint32_t index, GR_Device *device) {
    if (!device || device->size != sizeof(GR_Device))
        return ERROR_INVALID_PARAMETER;
    try {
        std::lock_guard<std::mutex> lock(apiMutex);
        return service && service->Device(index, *device) ? 0 : ERROR_NO_MORE_ITEMS;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}
