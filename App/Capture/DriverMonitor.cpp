#define IC_SDK_INTERNAL
#include "DriverMonitor.h"
#include "DriverMonitorProtocol.h"
#include "ControlCodec.h"
#include "HidLayout.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>
#include <setupapi.h>
#include <hidsdi.h>

namespace gpr {
namespace {
class Handle final {
    HANDLE value_ = nullptr;
  public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE Get() const noexcept { return value_; }
    void Set(HANDLE value) {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
};
std::string Utf8(const wchar_t *text, std::size_t count) {
    if (!count) return {};
    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                                         static_cast<int>(count), nullptr, 0, nullptr, nullptr);
    if (!size) return "Invalid Unicode diagnostic";
    std::string result(static_cast<std::size_t>(size), 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(count),
                        result.data(), size, nullptr, nullptr);
    return result;
}
[[noreturn]] void Failure(const char *operation, DWORD code) {
    wchar_t message[2048]{};
    const auto count = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, code, 0, message, 2048, nullptr);
    auto detail = count ? Utf8(message, count) : "Windows error";
    while (!detail.empty() && (detail.back() == '\r' || detail.back() == '\n')) detail.pop_back();
    throw std::runtime_error(std::string(operation) + " (Win32 " + std::to_string(code) + "): " + detail);
}
void Require(bool condition, const char *operation) {
    if (!condition) Failure(operation, ERROR_INVALID_DATA);
}
class HidConsumer final {
    std::mutex gate_;
    Handle file_;
    PTP_IO io_ = nullptr;
    OVERLAPPED pending_{};
    std::vector<std::uint8_t> bytes_;
    bool stopped_ = false;
    std::atomic<DWORD> error_{0};
    void SubmitLocked() noexcept {
        if (stopped_) return;
        pending_ = {};
        StartThreadpoolIo(io_);
        if (!ReadFile(file_.Get(), bytes_.data(), static_cast<DWORD>(bytes_.size()), nullptr, &pending_)) {
            const auto code = GetLastError();
            if (code != ERROR_IO_PENDING) {
                CancelThreadpoolIo(io_); error_.store(code); stopped_ = true;
            }
        }
    }
    static void CALLBACK Complete(PTP_CALLBACK_INSTANCE, void *context, void *, ULONG result,
                                   ULONG_PTR transferred, PTP_IO) noexcept {
        auto &self = *static_cast<HidConsumer *>(context);
        std::lock_guard<std::mutex> lock(self.gate_);
        if (self.stopped_) return;
        if (result != ERROR_SUCCESS || !transferred || transferred > self.bytes_.size()) {
            self.error_.store(result ? result : ERROR_INVALID_DATA); self.stopped_ = true; return;
        }
        // The kernel already recorded this actual Windows READ completion.
        // Drain only: no decoder, allocation or speculative OUT publication.
        self.SubmitLocked();
    }
  public:
    explicit HidConsumer(const std::wstring &path) {
        file_.Set(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (file_.Get() == INVALID_HANDLE_VALUE) Failure("Open normal Windows HID output reader", GetLastError());
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (!HidD_GetPreparsedData(file_.Get(), &preparsed))
            Failure("Read normal Windows HID input-report capabilities", GetLastError());
        const std::unique_ptr<_HIDP_PREPARSED_DATA, decltype(&HidD_FreePreparsedData)> descriptor(
            preparsed, HidD_FreePreparsedData);
        Require(preparsed != nullptr, "Missing normal Windows HID parser data");
        HIDP_CAPS caps{};
        Require(HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS && caps.InputReportByteLength &&
                    caps.InputReportByteLength <= IC_HID_REPORT_MAX,
                "Invalid normal Windows HID input-report length");
        // The normal HID interface can sit above a translator. Its own report
        // length, rather than the raw source descriptor's length, sizes READ.
        bytes_.resize(caps.InputReportByteLength);
        io_ = CreateThreadpoolIo(file_.Get(), Complete, this, nullptr);
        if (!io_) Failure("Create normal Windows HID output reader", GetLastError());
        std::lock_guard<std::mutex> lock(gate_);
        SubmitLocked();
    }
    ~HidConsumer() {
        {
            std::lock_guard<std::mutex> lock(gate_);
            stopped_ = true;
            // Submission and cancellation share this lock. A completing read
            // cannot issue a new request after cancellation has begun.
            CancelIoEx(file_.Get(), &pending_);
        }
        // Keep callbacks enabled: cancellation is asynchronous, so the READ
        // must complete before its OVERLAPPED and buffer can be destroyed.
        // The callback observes stopped_ and cannot resubmit another request.
        WaitForThreadpoolIoCallbacks(io_, FALSE);
        CloseThreadpoolIo(io_);
    }
    DWORD Error() const noexcept { return error_.load(); }
};
std::wstring HidPath(const std::wstring &instance) {
    Require(!instance.empty() && instance.size() < 256, "Missing passive HID instance identity");
    GUID guid{}; HidD_GetHidGuid(&guid);
    const auto devices = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) Failure("Enumerate normal Windows HID interfaces", GetLastError());
    struct DeviceSet { HDEVINFO Value; ~DeviceSet() { SetupDiDestroyDeviceInfoList(Value); } } owner{devices};
    for (DWORD index = 0; index < IC_MAX_DEVICES * 4u; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{sizeof(interfaceData)};
        if (!SetupDiEnumDeviceInterfaces(devices, nullptr, &guid, index, &interfaceData)) {
            const auto code = GetLastError();
            if (code == ERROR_NO_MORE_ITEMS) break;
            Failure("Enumerate normal Windows HID interface", code);
        }
        DWORD count = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &count, nullptr);
        Require(count >= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) && count <= 65536,
                "Invalid normal Windows HID interface path size");
        std::vector<std::uint8_t> bytes(count);
        auto detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(bytes.data());
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA device{sizeof(device)};
        if (!SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, count, nullptr, &device))
            Failure("Read normal Windows HID interface path", GetLastError());
        wchar_t candidate[256]{};
        if (!SetupDiGetDeviceInstanceIdW(devices, &device, candidate, 256, nullptr)) continue;
        if (CompareStringOrdinal(candidate, -1, instance.c_str(), -1, TRUE) == CSTR_EQUAL)
            return detail->DevicePath;
    }
    Failure("Find exact normal Windows HID interface for passive telemetry", ERROR_NOT_FOUND);
}
class WindowsTransport final : public MonitorTransport {
    Handle control_;
    std::vector<std::uint8_t> buffer_;
    std::unordered_map<std::uint64_t, std::unique_ptr<HidConsumer>> consumers_;
    void Consumer(MonitorMetadata &metadata) {
        if ((metadata.Info.type != IC_TYPE_CONTROLLER_HID && metadata.Info.type != IC_TYPE_CONTROLLER_XINPUTHID) ||
            !(metadata.Flags & IC_MONITOR_SOURCE_READY)) return;
        const auto key = Remapper::SourceKey(metadata.Info.type, metadata.Info.instance_token);
        auto found = consumers_.find(key);
        if (found != consumers_.end() && found->second->Error()) {
            // A readiness event after D0 resume can refer to the same FDO.
            // Retire a stopped drainer before making one bounded fresh attempt.
            consumers_.erase(found); found = consumers_.end();
        }
        if (found == consumers_.end()) {
            try {
                auto reader = std::make_unique<HidConsumer>(HidPath(metadata.Instance));
                found = consumers_.emplace(key, std::move(reader)).first;
            } catch (const std::exception &error) {
                metadata.ConsumerError = error.what(); return;
            }
        }
        if (const auto error = found->second->Error())
            metadata.ConsumerError = "Normal Windows HID output reader stopped (Win32 " + std::to_string(error) +
                                     "). OUT remains unknown until a real Windows input read completes.";
    }
    DWORD Io(DWORD code, void *input, DWORD inputBytes, void *output, DWORD outputBytes,
             const char *operation) {
        DWORD returned = 0;
        if (!DeviceIoControl(control_.Get(), code, input, inputBytes, output, outputBytes, &returned, nullptr))
            Failure(operation, GetLastError());
        Require(returned <= outputBytes, "Telemetry returned an oversized response");
        return returned;
    }
    MonitorMetadata Metadata(IC_MONITOR_DEVICE_REQUEST request) {
        std::vector<std::uint8_t> bytes(sizeof(IC_MONITOR_DEVICE_RESULT) + 65536);
        const auto count = Io(IOCTL_IC_MONITOR_DEVICE, &request, sizeof(request), bytes.data(),
                              static_cast<DWORD>(bytes.size()), "Read passive controller metadata");
        Require(count >= sizeof(IC_MONITOR_DEVICE_RESULT), "Truncated passive controller metadata");
        IC_MONITOR_DEVICE_RESULT result{};
        std::memcpy(&result, bytes.data(), sizeof(result));
        const auto appended = request.flags & IC_MONITOR_PREPARSED ? result.descriptor_bytes : 0u;
        Require(result.version == IC_MONITOR_VERSION && result.size == sizeof(result) &&
                    result.descriptor_bytes <= 65536 && count == sizeof(result) + appended,
                "Invalid passive controller metadata");
        Require(result.info.type == request.type && result.info.instance_token &&
                    ((request.flags & IC_MONITOR_BY_ORDINAL) || result.info.instance_token == request.instance_token),
                "Passive controller metadata identity changed");
        MonitorMetadata metadata;
        metadata.Info = result.info;
        metadata.Flags = result.flags;
        std::size_t characters = 0;
        while (characters < 256 && result.instance[characters]) ++characters;
        Require(characters < 256, "Unterminated passive controller instance identity");
        metadata.Instance.assign(reinterpret_cast<const wchar_t *>(result.instance), characters);
        metadata.Preparsed.assign(bytes.begin() + sizeof(result), bytes.begin() + count);
        Consumer(metadata);
        return metadata;
    }
  public:
    // Drain normal HID reads before closing the observer control handle. The
    // driver owns and retains the independent physical source underneath them.
    ~WindowsTransport() override { consumers_.clear(); }
    MonitorCapabilities Open(HANDLE changedEvent) override {
        control_.Set(CreateFileW(IC_SDK_DOS_DEVICE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (control_.Get() == INVALID_HANDLE_VALUE) Failure("Open passive controller monitor", GetLastError());
        IC_MONITOR_CAPABILITIES caps{};
        const auto count = Io(IOCTL_IC_MONITOR_QUERY, nullptr, 0, &caps, sizeof(caps),
                              "Query passive controller monitor; install the updated driver if unsupported");
        Require(count == sizeof(caps) && caps.size == sizeof(caps) && caps.version == IC_MONITOR_VERSION &&
                    caps.frame_bytes == sizeof(IC_MONITOR_FRAME) && caps.capacity == IC_MONITOR_CAPACITY &&
                    caps.timestamp_frequency,
                "Invalid passive monitor capabilities");
        IC_MONITOR_SUBSCRIBE request{IC_MONITOR_VERSION, 0,
                                    static_cast<std::uint64_t>(reinterpret_cast<ULONG_PTR>(changedEvent))};
        Io(IOCTL_IC_MONITOR_SUBSCRIBE, &request, sizeof(request), nullptr, 0, "Subscribe to passive controller reports");
        // The first subscriber allocates a fresh ring and advances session.
        // A pre-subscription query may legitimately return zero or the prior
        // retired ring's session. Pin the session only after our observer owns it.
        IC_MONITOR_CAPABILITIES active{};
        const auto activeBytes = Io(IOCTL_IC_MONITOR_QUERY, nullptr, 0, &active, sizeof(active),
                                     "Query subscribed passive controller monitor");
        Require(activeBytes == sizeof(active) && active.size == sizeof(active) &&
                    active.version == caps.version && active.frame_bytes == caps.frame_bytes &&
                    active.capacity == caps.capacity && active.timestamp_frequency == caps.timestamp_frequency && active.session,
                "Passive monitor capabilities changed while subscribing");
        buffer_.resize(sizeof(IC_MONITOR_READ_RESULT) + IC_MONITOR_MAX_READ * sizeof(IC_MONITOR_FRAME));
        return {active.session, active.timestamp_frequency, active.capacity};
    }
    MonitorBatch Read(unsigned maximum) override {
        Require(maximum && maximum <= IC_MONITOR_MAX_READ, "Invalid passive monitor read bound");
        IC_MONITOR_READ request{IC_MONITOR_VERSION, maximum};
        const auto count = Io(IOCTL_IC_MONITOR_READ, &request, sizeof(request), buffer_.data(),
                              static_cast<DWORD>(buffer_.size()), "Read passive controller reports");
        Require(count >= sizeof(IC_MONITOR_READ_RESULT), "Truncated passive monitor read");
        IC_MONITOR_READ_RESULT result{};
        std::memcpy(&result, buffer_.data(), sizeof(result));
        Require(result.version == IC_MONITOR_VERSION && result.frame_bytes == sizeof(IC_MONITOR_FRAME) &&
                    result.capacity == IC_MONITOR_CAPACITY && result.count <= maximum &&
                    count == sizeof(result) + result.count * sizeof(IC_MONITOR_FRAME),
                "Invalid passive monitor read");
        MonitorBatch batch;
        batch.Session = result.session;
        batch.NextSequence = result.next_sequence;
        batch.Dropped = result.dropped;
        batch.Frames.reserve(result.count);
        for (unsigned index = 0; index < result.count; ++index) {
            IC_MONITOR_FRAME frame{};
            std::memcpy(&frame, buffer_.data() + sizeof(result) + index * sizeof(frame), sizeof(frame));
            Require(!frame.reserved && frame.length <= IC_HID_REPORT_MAX,
                    "Invalid passive controller frame bounds");
            MonitorFrame value;
            value.Sequence = frame.sequence; value.Timestamp = frame.timestamp;
            value.Family = frame.type; value.Instance = frame.instance_token;
            value.Direction = frame.direction; value.Flags = frame.flags; value.Status = frame.status;
            value.Native = frame.state;
            value.Raw.assign(frame.report, frame.report + frame.length);
            batch.Frames.push_back(std::move(value));
        }
        return batch;
    }
    MonitorMetadata Device(unsigned family, unsigned instance, bool preparsed) override {
        return Metadata({IC_MONITOR_VERSION, family, instance, preparsed ? IC_MONITOR_PREPARSED : 0u});
    }
    bool Enumerate(unsigned family, unsigned ordinal, MonitorMetadata &metadata) override {
        IC_MONITOR_DEVICE_REQUEST request{IC_MONITOR_VERSION, family, ordinal, IC_MONITOR_BY_ORDINAL};
        std::vector<std::uint8_t> bytes(sizeof(IC_MONITOR_DEVICE_RESULT));
        DWORD count = 0;
        if (!DeviceIoControl(control_.Get(), IOCTL_IC_MONITOR_DEVICE, &request, sizeof(request),
                              bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) {
            const auto code = GetLastError();
            if (code == ERROR_NO_MORE_ITEMS || code == ERROR_NOT_FOUND || code == ERROR_DEVICE_NOT_CONNECTED)
                return false;
            Failure("Enumerate passive controller metadata", code);
        }
        Require(count == sizeof(IC_MONITOR_DEVICE_RESULT), "Invalid enumerated passive controller metadata");
        IC_MONITOR_DEVICE_RESULT result{};
        std::memcpy(&result, bytes.data(), sizeof(result));
        Require(result.version == IC_MONITOR_VERSION && result.size == sizeof(result) && result.descriptor_bytes <= 65536 &&
                    result.info.type == family && result.info.instance_token,
                "Invalid enumerated passive controller identity");
        metadata.Info = result.info;
        metadata.Flags = result.flags;
        std::size_t characters = 0;
        while (characters < 256 && result.instance[characters]) ++characters;
        Require(characters < 256, "Unterminated enumerated controller instance identity");
        metadata.Instance.assign(reinterpret_cast<const wchar_t *>(result.instance), characters);
        metadata.Preparsed.clear();
        Consumer(metadata);
        return true;
    }
    void Removed(unsigned family, unsigned instance) override { consumers_.erase(Remapper::SourceKey(family, instance)); }
};
} // namespace

struct DriverMonitor::Impl {
    struct Lane {
        std::array<int, 256> Values{};
        std::array<std::uint8_t, 256> Known{};
        Interceptor::Hid_Decoded_Input Decoded;
        std::uint64_t Sequence = 0, Timestamp = 0;
        std::uint32_t Status = 0;
        std::vector<std::uint8_t> Raw;
    };
    struct Source {
        ServiceDevice Device;
        Remapper::HidSlots Slots;
        std::shared_ptr<const Interceptor::Detail::Hid_Codec> Decoder;
        std::array<std::vector<unsigned>, 256> ReportSlots;
        std::vector<unsigned> Relative;
        std::array<bool, 256> RelativeKnown{};
        Lane Input, Output;
        bool Metadata = false, Seeded = false, Removed = false;
        std::string ConsumerError;
        std::chrono::steady_clock::time_point Retry{};
    };
    std::function<void()> changed;
    Factory factory;
    std::mutex lifetime, published;
    Handle stop, ready;
    std::thread thread;
    std::atomic<bool> alive{false};
    MonitorSnapshot snapshot;
    std::unordered_map<std::uint64_t, Source> sources;
    std::uint32_t nextKey = 1;
    std::uint64_t frames = 0, dropped = 0, lastSequence = 0, nextSequence = 0;

    Impl(std::function<void()> notification, Factory transport)
        : changed(std::move(notification)), factory(std::move(transport)) {}
    void Notify() noexcept { try { if (changed) changed(); } catch (...) {} }
    void Publish(bool running, const std::string &status, const std::string &error = {}) {
        std::vector<ServiceDevice> devices;
        devices.reserve(sources.size());
        for (auto &item : sources) {
            auto &source = item.second;
            auto &device = source.Device;
            device.PhysicalValid = device.OutputValid = false;
            for (unsigned i = 0; i < 256; ++i) {
                device.InputAvailable[i] = source.Input.Known[i] != 0;
                device.OutputAvailable[i] = source.Output.Known[i] != 0;
                device.Available[i] = device.Available[i] || device.InputAvailable[i] || device.OutputAvailable[i];
                device.PhysicalValid |= device.InputAvailable[i];
                device.OutputValid |= device.OutputAvailable[i];
            }
            device.Values = source.Input.Values; device.OutputValues = source.Output.Values;
            device.InputSequence = source.Input.Sequence; device.OutputSequence = source.Output.Sequence;
            device.InputTimestamp = source.Input.Timestamp; device.OutputTimestamp = source.Output.Timestamp;
            device.InputStatus = source.Input.Status; device.OutputStatus = source.Output.Status;
            device.InputRaw = source.Input.Raw; device.OutputRaw = source.Output.Raw;
            devices.push_back(device);
        }
        std::sort(devices.begin(), devices.end(), [](const auto &a, const auto &b) { return a.Key < b.Key; });
        {
            std::lock_guard<std::mutex> lock(published);
            snapshot.Running = running; snapshot.Status = status; snapshot.Error = error;
            snapshot.Frames = frames; snapshot.Dropped = dropped;
            snapshot.Devices = std::move(devices);
        }
        Notify();
    }
    Source &Find(unsigned family, unsigned instance, std::uint64_t session) {
        Require(family >= IC_TYPE_CONTROLLER_HID && family <= IC_TYPE_CONTROLLER_GIP && instance,
                "Invalid passive controller source");
        const auto identity = Remapper::SourceKey(family, instance);
        auto found = sources.find(identity);
        if (found == sources.end()) {
            Require(sources.size() < IC_MAX_DEVICES * 4u, "Passive monitor source bound exceeded");
            found = sources.try_emplace(identity).first;
            auto &device = found->second.Device;
            device.Key = nextKey++; device.Family = family; device.Telemetry = true;
            device.Name = "Controller " + std::to_string(family) + ":" + std::to_string(instance);
            device.Token = "monitor:" + std::to_string(session) + ":" + std::to_string(family) + ":" + std::to_string(instance);
            device.Diagnostic = "Passive driver telemetry. Waiting for observed input and output reports.";
        }
        return found->second;
    }
    void Metadata(Source &source, const MonitorMetadata &metadata) {
        const auto &info = metadata.Info;
        Require(info.type == source.Device.Family && info.instance_token,
                "Invalid passive metadata family");
        std::size_t length = 0;
        while (length < 128 && info.name[length]) ++length;
        if (length) source.Device.Name = Utf8(reinterpret_cast<const wchar_t *>(info.name), length);
        source.ConsumerError = metadata.ConsumerError;
        if (!metadata.Preparsed.empty()) {
            source.Decoder = Interceptor::Detail::Create_Hid_Decoder(metadata.Preparsed);
            source.Seeded = Remapper::SeedHidLayout(source.Slots, metadata.Preparsed);
        }
        source.Metadata = source.Device.Family == IC_TYPE_CONTROLLER_XUSB ||
                          source.Device.Family == IC_TYPE_CONTROLLER_GIP || static_cast<bool>(source.Decoder);
    }
    void RefreshMetadata(Source &source, MonitorTransport &transport, unsigned instance, bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && (now < source.Retry || source.Metadata)) return;
        source.Retry = now + std::chrono::seconds(1);
        try {
            const bool hid = source.Device.Family == IC_TYPE_CONTROLLER_HID ||
                             source.Device.Family == IC_TYPE_CONTROLLER_XINPUTHID;
            Metadata(source, transport.Device(source.Device.Family, instance, hid));
        } catch (const std::system_error &error) {
            source.Device.Diagnostic = "Passive report observed; controller metadata/parser unavailable (Win32 " +
                std::to_string(error.code().value()) + "). Undecoded controls remain unknown.";
        } catch (const std::exception &error) {
            source.Device.Diagnostic = "Passive report observed; controller metadata/descriptor is not ready. "
                                       "Undecoded controls remain unknown. ";
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, error.what(), -1, nullptr, 0))
                source.Device.Diagnostic += error.what();
        }
    }
    void Invalidate() {
        for (auto &item : sources) {
            item.second.Input.Known.fill(0); item.second.Output.Known.fill(0);
            item.second.Device.Diagnostic = "Telemetry reports were lost. Unobserved fields remain unknown until fresh reports arrive.";
        }
    }
    void Fault(const char *error) noexcept {
        try {
            Invalidate();
            Publish(false, "Passive controller monitor unavailable.", error);
        } catch (...) {
            try { std::lock_guard<std::mutex> lock(published); snapshot.Running = false; } catch (...) {}
            Notify();
        }
    }
    void Frame(const MonitorFrame &frame, MonitorTransport &transport, std::uint64_t session) {
        Require(frame.Sequence && frame.Sequence > lastSequence && frame.Raw.size() <= IC_HID_REPORT_MAX &&
                    frame.Direction <= IC_MONITOR_OUT &&
                    !(frame.Flags & ~(IC_MONITOR_REMOVED | IC_MONITOR_ERROR | IC_MONITOR_NATIVE_VALID |
                                      IC_MONITOR_GUIDE_VALID | IC_MONITOR_RAW)),
                "Invalid passive frame sequence or shape");
        lastSequence = frame.Sequence;
        auto &source = Find(frame.Family, frame.Instance, session);
        ++frames;
        if (frame.Flags & IC_MONITOR_REMOVED) {
            transport.Removed(frame.Family, frame.Instance);
            source.Removed = true;
            source.Input.Known.fill(0); source.Output.Known.fill(0);
            source.Device.Diagnostic = "Controller disconnected. Previous bytes are retained for diagnostics; live values are unknown.";
            return;
        }
        if (source.Removed) {
            // Completions already queued before PnP removal can arrive later.
            // Retain those authentic bytes for diagnosis without reopening the
            // source or presenting a disconnected FDO as live again.
            if (frame.Direction == IC_MONITOR_IN || frame.Direction == IC_MONITOR_OUT) {
                auto &lane = frame.Direction == IC_MONITOR_IN ? source.Input : source.Output;
                lane.Sequence = frame.Sequence; lane.Timestamp = frame.Timestamp;
                lane.Status = frame.Status; lane.Raw = frame.Raw;
            }
            source.Device.Diagnostic = "Controller disconnected. A late completion was observed; live values remain unknown.";
            return;
        }
        if (frame.Direction == IC_MONITOR_DEVICE) {
            if ((frame.Flags & IC_MONITOR_ERROR) || frame.Status) {
                source.Input.Known.fill(0); source.Output.Known.fill(0);
                source.Input.Status = source.Output.Status = frame.Status;
                std::ostringstream diagnostic;
                diagnostic << "Driver telemetry reported a controller transport failure, status 0x"
                           << std::hex << frame.Status << std::dec << " (device event " << frame.Sequence
                           << "). IN and OUT remain unknown until fresh successful reports arrive.";
                source.Device.Diagnostic = diagnostic.str();
            } else {
                RefreshMetadata(source, transport, frame.Instance, true);
            }
            return;
        }
        RefreshMetadata(source, transport, frame.Instance, false);
        auto &lane = frame.Direction == IC_MONITOR_IN ? source.Input : source.Output;
        lane.Sequence = frame.Sequence; lane.Timestamp = frame.Timestamp; lane.Status = frame.Status; lane.Raw = frame.Raw;
        Remapper::ClearRelative(lane.Values, source.Relative);
        if ((frame.Flags & IC_MONITOR_ERROR) || frame.Status) {
            lane.Known.fill(0);
            std::ostringstream diagnostic;
            diagnostic << "Driver telemetry reported transport status 0x" << std::hex << frame.Status
                       << ". This direction is unknown until a successful report arrives.";
            source.Device.Diagnostic = diagnostic.str();
            return;
        }
        if (frame.Family == IC_TYPE_CONTROLLER_XUSB || frame.Family == IC_TYPE_CONTROLLER_GIP) {
            const bool gip = frame.Family == IC_TYPE_CONTROLLER_GIP;
            if (frame.Flags & IC_MONITOR_NATIVE_VALID) {
                Require(!frame.Native.reserved && frame.Native.left_trigger <= (gip ? 1023u : 255u) &&
                            frame.Native.right_trigger <= (gip ? 1023u : 255u),
                        "Invalid observed native gamepad range");
                const auto guide = lane.Values[20]; const auto guideKnown = lane.Known[20];
                Remapper::ReadNative(frame.Native, gip, lane.Values, lane.Known);
                lane.Values[20] = guide; lane.Known[20] = guideKnown;
            }
            if (frame.Flags & IC_MONITOR_GUIDE_VALID) {
                lane.Values[20] = (frame.Native.extra_buttons & 1) ? 100 : 0;
                lane.Known[20] = 1;
            }
            source.Device.Diagnostic = "Passive native reports: IN is physical; OUT was copied into a Windows input completion. "
                                       "Guide and ordinary GIP controls are observed independently.";
            return;
        }
        if (!source.Decoder || frame.Raw.empty()) return;
        const auto reportId = frame.Raw[0];
        for (auto slot : source.ReportSlots[reportId]) lane.Known[slot] = 0;
        Interceptor::Detail::Decode_Hid_Input(source.Decoder, lane.Decoded, frame.Raw.data(),
                                             static_cast<std::uint32_t>(frame.Raw.size()), frame.Instance);
        source.Slots.Read(lane.Decoded, lane.Values, lane.Known);
        auto &fields = source.ReportSlots[reportId];
        const auto remember = [&](const Remapper::FieldKey &key, bool relative) {
            const auto slot = source.Slots.Find(key);
            if (slot >= 256) return;
            source.Device.Available[slot] = true;
            if (std::find(fields.begin(), fields.end(), slot) == fields.end()) fields.push_back(slot);
            if (relative && !source.RelativeKnown[slot]) {
                source.RelativeKnown[slot] = true; source.Relative.push_back(slot);
            }
        };
        for (const auto &button : lane.Decoded.state.buttons) remember(Remapper::Key(button), false);
        const std::vector<Interceptor::Hid_Value> *lists[]{&lane.Decoded.state.axes, &lane.Decoded.state.triggers,
                                                         &lane.Decoded.state.hats, &lane.Decoded.state.values};
        for (unsigned kind = 0; kind < 4; ++kind)
            for (const auto &value : *lists[kind]) remember(Remapper::Key(value, kind + 1), value.relative);
        source.Device.Diagnostic = "Descriptor-driven passive HID reports; repeated usages and report IDs remain distinct. "
                                   "IN/OUT validity is tracked independently per control.";
        if (!source.Seeded) source.Device.Diagnostic += " Descriptor slot ordering could not be preseeded.";
        if (lane.Decoded.decode_error || !lane.Decoded.decode_complete)
            source.Device.Diagnostic += " Partial decoding (Windows " + std::to_string(lane.Decoded.decode_error) +
                                        "); undecoded fields remain unknown and raw bytes are retained.";
        if (source.Slots.truncated || !lane.Decoded.state.binary_values.empty())
            source.Device.Diagnostic += " Extra/wide fields are preserved in the raw report.";
        if (!source.ConsumerError.empty()) source.Device.Diagnostic += " " + source.ConsumerError;
    }
    void Run() noexcept {
        try {
            auto transport = factory ? factory() : std::make_unique<WindowsTransport>();
            Require(static_cast<bool>(transport), "Passive transport factory returned null");
            const auto caps = transport->Open(ready.Get());
            Require(caps.Session && caps.TimestampFrequency && caps.Capacity == IC_MONITOR_CAPACITY,
                    "Invalid passive transport capabilities");
            { std::lock_guard<std::mutex> lock(published); snapshot.TimestampFrequency = caps.TimestampFrequency; }
            for (unsigned family = IC_TYPE_CONTROLLER_HID; family <= IC_TYPE_CONTROLLER_GIP; ++family)
                for (unsigned ordinal = 1; ordinal <= IC_MAX_DEVICES; ++ordinal) {
                    if (WaitForSingleObject(stop.Get(), 0) == WAIT_OBJECT_0) break;
                    MonitorMetadata metadata;
                    if (!transport->Enumerate(family, ordinal, metadata)) break;
                    auto &source = Find(family, metadata.Info.instance_token, caps.Session);
                    Metadata(source, metadata);
                }
            Publish(true, "Passive controller monitor active; no script capture is required.");
            while (WaitForSingleObject(stop.Get(), 0) != WAIT_OBJECT_0) {
                const auto batch = transport->Read(IC_MONITOR_MAX_READ);
                Require(batch.Session == caps.Session && batch.Frames.size() <= IC_MONITOR_MAX_READ &&
                            batch.Dropped >= dropped && batch.NextSequence >= nextSequence,
                        "Passive monitor session changed or cursor was invalid");
                nextSequence = batch.NextSequence;
                const bool lost = batch.Dropped != dropped;
                if (lost) { dropped = batch.Dropped; Invalidate(); }
                for (const auto &frame : batch.Frames) Frame(frame, *transport, caps.Session);
                if (lost || !batch.Frames.empty()) Publish(true, "Passive controller monitor active.");
                if (batch.Frames.size() == IC_MONITOR_MAX_READ) continue;
                HANDLE events[]{stop.Get(), ready.Get()};
                const auto result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                if (result == WAIT_OBJECT_0) break;
                if (result != WAIT_OBJECT_0 + 1) Failure("Wait for passive controller telemetry", GetLastError());
            }
            Publish(false, "Passive controller monitor stopped.");
        } catch (const std::exception &error) {
            Fault(error.what());
        } catch (...) {
            Fault("Unexpected passive monitor failure.");
        }
    }
};
DriverMonitor::DriverMonitor(std::function<void()> changed, Factory factory)
    : impl_(std::make_unique<Impl>(std::move(changed), std::move(factory))) {}
DriverMonitor::~DriverMonitor() { Stop(); }
void DriverMonitor::Start() {
    auto &self = *impl_;
    std::lock_guard<std::mutex> lifecycle(self.lifetime);
    if (self.thread.joinable()) {
        if (self.alive.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lock(self.published);
            if (self.snapshot.Error.empty()) return;
        }
        if (self.stop.Get()) SetEvent(self.stop.Get());
        self.thread.join();
    }
    self.stop.Set(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!self.stop.Get()) Failure("Create passive monitor stop event", GetLastError());
    self.ready.Set(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!self.ready.Get()) Failure("Create passive monitor report event", GetLastError());
    self.sources.clear(); self.nextKey = 1; self.frames = self.dropped = self.lastSequence = self.nextSequence = 0;
    { std::lock_guard<std::mutex> lock(self.published); self.snapshot = {}; self.snapshot.Requested = true;
      self.snapshot.Status = "Starting passive controller monitor."; }
    self.alive.store(true, std::memory_order_release);
    try {
        self.thread = std::thread([&self] { self.Run(); self.alive.store(false, std::memory_order_release); });
    } catch (...) { self.alive.store(false, std::memory_order_release); throw; }
}
void DriverMonitor::Stop() noexcept {
    auto &self = *impl_;
    std::lock_guard<std::mutex> lifecycle(self.lifetime);
    if (self.stop.Get()) SetEvent(self.stop.Get());
    if (self.thread.joinable()) self.thread.join();
}
MonitorSnapshot DriverMonitor::Snapshot() {
    std::lock_guard<std::mutex> lock(impl_->published);
    return impl_->snapshot;
}
} // namespace gpr
