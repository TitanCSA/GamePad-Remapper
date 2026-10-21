#include "Service.h"
#include "Bridge.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

namespace gpr {
namespace {
std::string Utf8(const wchar_t *text) {
    if (!text || !*text)
        return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return "Invalid Windows diagnostic text";
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, result.data(), length, nullptr,
                        nullptr);
    result.pop_back();
    return result;
}
// std::system_category().message() uses the process ANSI code page on MSVC.
// Keep the native error_code/category, but never append that byte string to a
// UTF-8 diagnostic consumed by the UI's strict UTF-8 conversion.
class WindowsFailure final : public std::system_error {
    std::string text_;

  public:
    WindowsFailure(std::uint32_t code, const std::string &operation)
        : std::system_error(static_cast<int>(code), std::system_category()) {
        wchar_t *raw = nullptr;
        const auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                              FORMAT_MESSAGE_IGNORE_INSERTS,
                                          nullptr, code, 0, reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
        const std::unique_ptr<wchar_t, void (*)(wchar_t *)> message(
            raw, [](wchar_t *value) { if (value) LocalFree(value); });
        auto detail = length ? Utf8(raw) : "Windows error " + std::to_string(code);
        while (!detail.empty() && (detail.back() == '\r' || detail.back() == '\n'))
            detail.pop_back();
        text_ = operation.empty() ? detail : operation + ": " + detail;
    }
    const char *what() const noexcept override { return text_.c_str(); }
};
GR_Snapshot NativeSnapshot() {
    GR_Snapshot result{};
    result.size = sizeof(result);
    const auto error = GR_GetSnapshot(&result);
    if (error)
        throw WindowsFailure(error, "Read capture status");
    if (result.version != 2)
        throw std::runtime_error("The capture engine version does not match this application.");
    return result;
}
} // namespace
struct RemapperService::Impl {
    struct ProgramSlot {
        std::shared_ptr<const ScriptProgram> Program;
        ParameterValues Values;
        std::uint64_t Generation;
        ProgramSlot(std::shared_ptr<const ScriptProgram> program, const ParameterValues &values,
                    std::uint64_t generation)
            : Program(std::move(program)), Values(values), Generation(generation) {}
    };
    struct RuntimeDevice {
        std::unique_ptr<ScriptInstance> Script;
        ControlFrame Frame;
        std::int64_t LastMicroseconds = 0;
    };
    std::mutex gate, errorGate, cacheGate;
    std::unordered_map<std::uint32_t, RuntimeDevice> devices;
    std::shared_ptr<const ProgramSlot> pending, executing;
    std::string startupMessage, callbackError;
    bool started = false;
    std::uint64_t generation = 0;
    ServiceSnapshot cached;

    Impl() {
        wchar_t text[1024]{};
        GR_Probe(text, 1024);
        startupMessage = Utf8(text);
        cached.Status = startupMessage;
    }
    void SetError(std::string text) {
        std::lock_guard<std::mutex> lock(errorGate);
        callbackError = std::move(text);
    }
    static int __cdecl Transform(std::uint32_t key, std::uint32_t family, const int *physical,
                                 const std::uint8_t *available, int *output, std::uint32_t flags,
                                 void *user) noexcept {
        auto &self = *static_cast<Impl *>(user);
        try {
            if (flags == 3) {
                self.devices.erase(key);
                return 0;
            }
            const auto current = std::atomic_load_explicit(&self.pending, std::memory_order_acquire);
            if (current != self.executing) {
                if (!current || !self.executing || current->Generation != self.executing->Generation)
                    self.devices.clear();
                else
                    for (auto &entry : self.devices)
                        entry.second.Script->SetParameters(current->Values);
                self.executing = current;
            }
            if (!current) {
                std::copy_n(physical, 256, output);
                return 0;
            }
            auto found = self.devices.find(key);
            if (found == self.devices.end()) {
                RuntimeDevice device;
                device.Script = current->Program->CreateInstance();
                device.Script->SetParameters(current->Values);
                found = self.devices.emplace(key, std::move(device)).first;
            }
            auto &device = found->second;
            auto &frame = device.Frame;
            frame.DeviceKey = key;
            frame.Family = family;
            std::copy_n(physical, 256, frame.Physical.begin());
            for (std::size_t i = 0; i < 256; ++i)
                frame.Available[i] = available[i] != 0;
            const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
            const int elapsed = device.LastMicroseconds == 0
                                    ? 0
                                    : static_cast<int>(std::clamp<std::int64_t>(
                                          (now - device.LastMicroseconds) / 1000, 0, INT_MAX));
            device.Script->Execute(frame, now, elapsed);
            device.LastMicroseconds = now;
            std::copy(frame.Values.begin(), frame.Values.end(), output);
            return device.Script->NextWakeMilliseconds();
        } catch (const ScriptException &error) {
            try {
                self.SetError(error.Diagnostic());
            } catch (...) {
            }
        } catch (const std::exception &error) {
            try {
                self.SetError(error.what());
            } catch (...) {
            }
        } catch (...) {
            try {
                self.SetError("Unexpected script runtime failure.");
            } catch (...) {
            }
        }
        std::copy_n(physical, 256, output);
        return -1;
    }
};
RemapperService::RemapperService() : impl_(std::make_unique<Impl>()) {}
RemapperService::~RemapperService() {
    try {
        Stop();
    } catch (...) {
    }
}
void RemapperService::Inject(std::shared_ptr<const ScriptProgram> program) {
    if (!program)
        throw std::invalid_argument("A compiled script is required.");
    const auto values = program->Interface().Defaults();
    Inject(std::move(program), values);
}
void RemapperService::Inject(std::shared_ptr<const ScriptProgram> program,
                             const ParameterValues &values) {
    if (!program)
        throw std::invalid_argument("A compiled script is required.");
    program->Interface().Validate(values);
    auto &self = *impl_;
    std::lock_guard<std::mutex> lock(self.gate);
    std::atomic_store_explicit(
        &self.pending,
        std::make_shared<const Impl::ProgramSlot>(std::move(program), values, ++self.generation),
        std::memory_order_release);
    self.SetError({});
    if (self.started && NativeSnapshot().running) {
        GR_Wake();
        return;
    }
    if (self.started) {
        GR_Stop();
        self.started = false;
    }
    self.devices.clear();
    self.executing.reset();
    wchar_t text[1024]{};
    auto error = GR_Probe(text, 1024);
    if (error)
        throw WindowsFailure(error, Utf8(text));
    error = GR_Start(Impl::Transform, &self);
    if (error) {
        const auto failed = NativeSnapshot();
        const auto detail = Utf8(failed.error);
        throw WindowsFailure(error, detail.empty() ? "Capture could not start" : detail);
    }
    self.started = true;
}
bool RemapperService::TryUpdate(std::shared_ptr<const ScriptProgram> program) {
    if (!program)
        throw std::invalid_argument("A compiled script is required.");
    const auto values = program->Interface().Defaults();
    return TryUpdate(std::move(program), values);
}
bool RemapperService::TryUpdate(std::shared_ptr<const ScriptProgram> program,
                                const ParameterValues &values) {
    if (!program)
        throw std::invalid_argument("A compiled script is required.");
    program->Interface().Validate(values);
    auto &self = *impl_;
    std::lock_guard<std::mutex> lock(self.gate);
    if (!self.started || !NativeSnapshot().running)
        return false;
    std::atomic_store_explicit(
        &self.pending,
        std::make_shared<const Impl::ProgramSlot>(std::move(program), values, ++self.generation),
        std::memory_order_release);
    self.SetError({});
    GR_Wake();
    return true;
}
bool RemapperService::SetParameters(const std::shared_ptr<const ScriptProgram> &program,
                                    const ParameterValues &values) {
    if (!program)
        throw std::invalid_argument("A compiled script is required.");
    program->Interface().Validate(values);
    auto &self = *impl_;
    std::lock_guard<std::mutex> lock(self.gate);
    const auto current = std::atomic_load_explicit(&self.pending, std::memory_order_acquire);
    if (!self.started || !current || current->Program != program || !NativeSnapshot().running)
        return false;
    std::atomic_store_explicit(
        &self.pending, std::make_shared<const Impl::ProgramSlot>(program, values, current->Generation),
        std::memory_order_release);
    GR_ParametersChanged();
    return true;
}
void RemapperService::Stop() {
    auto &self = *impl_;
    std::lock_guard<std::mutex> lock(self.gate);
    if (self.started) {
        GR_Stop();
        self.started = false;
    }
    self.devices.clear();
    self.executing.reset();
    std::atomic_store_explicit(&self.pending, std::shared_ptr<const Impl::ProgramSlot>{},
                               std::memory_order_release);
}
bool RemapperService::IsRunning() { return GetSnapshot().Running; }
ServiceSnapshot RemapperService::GetSnapshot() {
    auto &self = *impl_;
    std::unique_lock<std::mutex> lock(self.gate, std::try_to_lock);
    if (!lock) {
        std::lock_guard<std::mutex> cache(self.cacheGate);
        return self.cached;
    }
    ServiceSnapshot result;
    const auto native = NativeSnapshot();
    result.Running = native.running != 0;
    result.Status = self.started || native.received ? Utf8(native.status) : self.startupMessage;
    result.Error = Utf8(native.error);
    result.Received = native.received;
    result.Forwarded = native.forwarded;
    result.Changed = native.changed;
    result.TimerOutputs = native.timer_outputs;
    result.Faults = native.faults;
    result.ProcessingMicroseconds = native.processing_microseconds;
    {
        std::lock_guard<std::mutex> errors(self.errorGate);
        if (!self.callbackError.empty())
            result.Error = self.callbackError + (result.Error.empty() ? "" : "\n" + result.Error);
    }
    const auto count = std::min(native.device_count, 256u);
    result.Devices.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        GR_Device source{};
        source.size = sizeof(source);
        if (GR_GetDevice(i, &source))
            break;
        ServiceDevice device;
        device.Key = source.key;
        device.Family = source.family;
        device.Name = Utf8(source.name);
        device.Token = Utf8(source.token);
        device.Diagnostic = Utf8(source.diagnostic);
        device.PhysicalValid = (source.flags & GR_PHYSICAL_VALID) != 0;
        device.OutputValid = (source.flags & GR_OUTPUT_VALID) != 0;
        std::copy_n(source.values, 256, device.Values.begin());
        std::copy_n(source.output_values, 256, device.OutputValues.begin());
        for (std::size_t slot = 0; slot < 256; ++slot) {
            device.Available[slot] = (source.available[slot] & 1) != 0;
            device.OutputAvailable[slot] = (source.available[slot] & 2) != 0;
        }
        result.Devices.emplace_back(std::move(device));
    }
    {
        std::lock_guard<std::mutex> cache(self.cacheGate);
        self.cached = result;
    }
    return result;
}
} // namespace gpr
