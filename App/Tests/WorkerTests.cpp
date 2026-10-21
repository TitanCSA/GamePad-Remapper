#define IC_SDK_INTERNAL
#include "Interceptor.h"
#include "../Capture/Bridge.h"
#include "../Capture/Service.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <variant>

// The production capture worker/service use this deterministic SDK transport.
// The passive monitor is not started by this fixture; no host driver or HID
// interface is opened.
namespace fixture {
using namespace Interceptor;
using Packet = std::variant<Controller_Hid_Input, Controller_Xinput_Input,
                            Controller_XinputHid_Input, Controller_GIP_Input,
                            Controller_GIP_Raw_Input>;
std::mutex gate;
std::condition_variable changed;
std::deque<Packet> queued;
std::vector<Packet> sent;
std::vector<Device> devices;
Init_Result initialized;
std::atomic<unsigned> profiles{7}, contexts{0}, closes{0}, rejected{0};
std::atomic<DWORD> sendError{0};
std::atomic<bool> rejectAll{false};
HANDLE ready = nullptr;
unsigned checks = 0;
void Check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
template <class T> unsigned Family() {
    if constexpr (std::is_same_v<T, Controller_Hid_Input>) return IC_TYPE_CONTROLLER_HID;
    else if constexpr (std::is_same_v<T, Controller_Xinput_Input>) return IC_TYPE_CONTROLLER_XUSB;
    else if constexpr (std::is_same_v<T, Controller_XinputHid_Input>) return IC_TYPE_CONTROLLER_XINPUTHID;
    else return IC_TYPE_CONTROLLER_GIP;
}
template <class T> auto &Wire(T &input) {
    if constexpr (std::is_same_v<std::remove_cv_t<T>, Controller_GIP_Raw_Input>) return input.input;
    else return input;
}
Device DeviceFor(unsigned family, unsigned id) {
    Device device;
    device.id = id;
    device.type = family;
    device.token = "test-capture-" + std::to_string(family) + ":" + std::to_string(id);
    const wchar_t name[] = L"Isolated worker fixture";
    std::copy(std::begin(name), std::end(name), device.name);
    return device;
}
template <class T> Send_Result Send(const T &input) {
    const auto error = rejectAll.load() ? ERROR_WRITE_FAULT : sendError.exchange(0);
    if (error) {
        ++rejected;
        return {0, error};
    }
    std::lock_guard<std::mutex> lock(gate);
    sent.emplace_back(input);
    changed.notify_all();
    return {1, ERROR_SUCCESS};
}
template <class T> void Queue(T input) {
    std::lock_guard<std::mutex> lock(gate);
    queued.emplace_back(std::move(input));
    SetEvent(ready);
}
std::size_t SentCount() {
    std::lock_guard<std::mutex> lock(gate);
    return sent.size();
}
template <class Predicate> void Await(Predicate predicate, const char *reason) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw std::runtime_error(reason);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(true, reason);
}
std::vector<Packet> Sent() {
    std::lock_guard<std::mutex> lock(gate);
    return sent;
}
void Reset(std::vector<Device> initial = {}) {
    std::lock_guard<std::mutex> lock(gate);
    queued.clear();
    sent.clear();
    devices = std::move(initial);
    initialized = {};
    initialized.ready = true;
    sendError.store(0);
    rejectAll.store(false);
    ResetEvent(ready);
}
} // namespace fixture

namespace Interceptor {
namespace Detail {
std::shared_ptr<const Hid_Codec> Create_Hid_Decoder(std::vector<std::uint8_t>) {
    throw std::runtime_error("Unexpected passive HID decoding in capture-worker fixture");
}
void Decode_Hid_Input(const std::shared_ptr<const Hid_Codec> &, Hid_Decoded_Input &,
                      const std::uint8_t *, std::uint32_t, std::uint32_t) {
    throw std::runtime_error("Unexpected passive HID decoding in capture-worker fixture");
}
// This friend is implemented only in the isolated test binary. The real SDK
// Automatic_State/transport is neither compiled nor linked into this target.
struct Automatic_State {
    template <class T> static Input_Event Event(T &input) {
        Input_Event event;
        event.type = static_cast<Device_Type>(fixture::Family<T>());
        const auto &wire = fixture::Wire(input);
        event.device = wire.device;
        event.sequence = wire.sequence;
        event.timestamp = wire.timestamp;
        event.payload_ = &input;
        const auto found = std::find_if(fixture::devices.begin(), fixture::devices.end(),
            [&](const Device &device) {
                return device.type == static_cast<unsigned>(event.type) && device.id == event.device;
            });
        event.device_info = found == fixture::devices.end() ? nullptr : &*found;
        return event;
    }
    static Input_Batch Batch(std::vector<Input_Event> &events) {
        Input_Batch batch;
        batch.inputs_ = {events.data(), events.size()};
        return batch;
    }
};
} // namespace Detail
void Input_Batch::Release() noexcept { inputs_ = {}; }
Input_Batch::~Input_Batch() { Release(); }
Input_Batch::Input_Batch(Input_Batch &&other) noexcept : inputs_(other.inputs_) { other.inputs_ = {}; }
Input_Batch &Input_Batch::operator=(Input_Batch &&other) noexcept {
    if (this != &other) { inputs_ = other.inputs_; other.inputs_ = {}; }
    return *this;
}
IC_CAPABILITIES Get_Capabilities() {
    IC_CAPABILITIES caps{};
    caps.abi_version = IC_VERSION;
    caps.controller_mask = 15;
    caps.xusb_profiles = fixture::profiles;
    return caps;
}
IC_TRIAL_STATUS Get_Trial_Status() { return {}; }
Init_Result Init_AllDevices() { return fixture::initialized; }
Context_Info Get_Context() {
    ++fixture::contexts;
    Context_Info context;
    context.active = true;
    context.devices = fixture::devices;
    return context;
}
void Destroy_Context() { ++fixture::closes; }
Descriptor_Result Get_descriptor(const std::string &) {
    Descriptor_Result descriptor;
    descriptor.applicable = false;
    return descriptor;
}
Input_Batch Get_Input_With_Timeout(DWORD timeout, HANDLE stopEvent) {
    HANDLE events[]{stopEvent, fixture::ready};
    const auto result = WaitForMultipleObjects(2, events, FALSE, timeout);
    if (result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT)
        return {};
    if (result != WAIT_OBJECT_0 + 1)
        throw std::system_error(GetLastError(), std::system_category(), "Isolated input wait");
    thread_local std::vector<fixture::Packet> flight;
    thread_local std::vector<Input_Event> batch;
    flight.clear();
    batch.clear();
    {
        std::lock_guard<std::mutex> lock(fixture::gate);
        while (!fixture::queued.empty()) {
            flight.push_back(std::move(fixture::queued.front()));
            fixture::queued.pop_front();
        }
        ResetEvent(fixture::ready);
    }
    for (auto &packet : flight)
        batch.push_back(std::visit([](auto &input) { return Detail::Automatic_State::Event(input); }, packet));
    return Detail::Automatic_State::Batch(batch);
}
Send_Result Send_Input(Input_Event &input) {
    if (auto p = input.Get<Controller_Hid_Input>()) return fixture::Send(*p);
    if (auto p = input.Get<Controller_Xinput_Input>()) return fixture::Send(*p);
    if (auto p = input.Get<Controller_XinputHid_Input>()) return fixture::Send(*p);
    if (auto p = input.Get<Controller_GIP_Raw_Input>()) return fixture::Send(*p);
    if (auto p = input.Get<Controller_GIP_Input>()) return fixture::Send(*p);
    return {0, ERROR_INVALID_DATA};
}
#define FIXTURE_SEND(Type) \
Send_Result Detail::SendAuto(Detail::Input_View<const Type> inputs) { \
    Send_Result total; \
    for (const auto &input : inputs) { \
        const auto result = fixture::Send(input); \
        total.accepted += result.accepted; \
        if (result.error) { total.error = result.error; break; } \
    } \
    return total; \
}
FIXTURE_SEND(Controller_Hid_Input)
FIXTURE_SEND(Controller_Xinput_Input)
FIXTURE_SEND(Controller_XinputHid_Input)
FIXTURE_SEND(Controller_GIP_Input)
FIXTURE_SEND(Controller_GIP_Raw_Input)
#undef FIXTURE_SEND
} // namespace Interceptor

namespace {
using namespace fixture;
using namespace gpr;
template <class T> T Native(unsigned device, std::uint64_t sequence) {
    T input{};
    auto &wire = Wire(input);
    wire.device = device;
    wire.sequence = sequence;
    wire.timestamp = 0x123456789ull;
    wire.state.left_x = -12001;
    wire.state.left_y = 933;
    wire.state.buttons = 0x1001;
    wire.state.extra_buttons = 0x80;
    wire.state.packet = 0xaabbccdd;
    return input;
}
template <class T> T Hid(unsigned device, std::uint64_t sequence) {
    T input;
    input.device = device;
    input.sequence = sequence;
    input.timestamp = 0x987654321ull;
    input.length = 6;
    input.report[0] = 3;
    input.report[5] = 0x5a;
    Hid_Value axis;
    axis.usage_page = 1;
    axis.usage = 0x30;
    axis.report_id = 3;
    axis.bit_size = 16;
    axis.minimum = -32768;
    axis.maximum = 32767;
    axis.normalized_valid = true;
    axis.normalized = -0.4;
    input.state.axes.push_back(axis);
    input.decode_complete = true;
    return input;
}
void StartupFailures() {
    Reset();
    profiles = 3;
    const auto before = contexts.load();
    wchar_t message[1024]{};
    Check(GR_Probe(message, 1024) == ERROR_REVISION_MISMATCH &&
              std::wstring(message).find(L"older XUSB backend") != std::wstring::npos && contexts == before,
          "An older upper-filter backend must be rejected before creating capture");
    profiles = 7;
    initialized.error = ERROR_INVALID_DATA;
    initialized.error_detail = "Read device filter properties";
    initialized.error_device = L"HID\\fixture-\u00e9";
    RemapperService service;
    bool precise = false;
    try {
        service.Inject(ScriptCompiler::Compile("main {}"));
    } catch (const std::exception &error) {
        const std::string text = error.what();
        precise = text.find("Read device filter properties") != std::string::npos &&
                  text.find("HID\\fixture-\xc3\xa9") != std::string::npos;
    }
    Check(precise && !service.IsRunning() && contexts == before,
          "Failed initialization must identify its actual operation/device and never create capture");
}
void AllTransports() {
    Reset({DeviceFor(3, 1), DeviceFor(4, 1), DeviceFor(5, 1), DeviceFor(6, 1), DeviceFor(6, 2)});
    RemapperService service;
    service.Inject(ScriptCompiler::Compile("main { set_val(LX,-get_ival(LX)); "
                                           "set_val(AXIS_1,-get_ival(AXIS_1)); }"));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    auto snapshot = service.GetSnapshot();
    Check(snapshot.Running && snapshot.Devices.size() == 5 && snapshot.Received == 0 && SentCount() == 0,
          "A quiet startup must remain active without inventing physical or output reports");
    for (const auto &device : snapshot.Devices)
        Check(!device.PhysicalValid && !device.OutputValid, "No initial report means unknown state");
    auto hid = Hid<Controller_Hid_Input>(1, 101);
    auto xusb = Native<Controller_Xinput_Input>(1, 102);
    auto xih = Hid<Controller_XinputHid_Input>(1, 103);
    auto gip = Native<Controller_GIP_Input>(1, 104);
    auto raw = Native<Controller_GIP_Raw_Input>(2, 105);
    raw.report_length = 50;
    for (unsigned i = 0; i < raw.report_length; ++i) raw.report[i] = static_cast<std::uint8_t>(i ^ 0xa5);
    Queue(hid); Queue(xusb); Queue(xih); Queue(gip); Queue(raw);
    Await([&] { return service.GetSnapshot().Received == 5; }, "All five payloads must be processed");
    const auto packets = Sent();
    Check(packets.size() == 5, "Each physical packet must be forwarded exactly once");
    for (const auto &packet : packets)
        std::visit([&](const auto &input) {
            using T = std::decay_t<decltype(input)>;
            const auto &wire = Wire(input);
            Check(wire.device != 0 && wire.sequence >= 101 && wire.sequence <= 105,
                  "Forwarding retains its original physical source and sequence");
            if constexpr (std::is_base_of_v<Hid_Decoded_Input, T>) {
                Check(input.state.axes[0].normalized == 0.4 && input.report[5] == 0x5a,
                      "HID and XInputHID edits retain unrelated opaque bytes");
            } else {
                Check(wire.state.left_x > 0 && wire.state.left_y == 933 &&
                          wire.state.extra_buttons == 0x80 && wire.state.packet == 0xaabbccdd,
                      "Native XUSB/GIP edits retain all unrelated native fields");
                if constexpr (std::is_same_v<T, Controller_GIP_Raw_Input>)
                    Check(input.report_length == 50 && !std::memcmp(input.report, raw.report, 64),
                          "Raw GIP keeps its full physical envelope and extension");
            }
        }, packet);
    snapshot = service.GetSnapshot();
    for (const auto &device : snapshot.Devices)
        Check(device.PhysicalValid && device.OutputValid && device.Token.find("test-capture-") == 0,
              "Physical and accepted-output snapshots retain exact capture-local identities");
    const auto beforeClose = closes.load();
    service.Stop();
    Check(closes == beforeClose + 1 && !service.IsRunning(), "Stop releases the capture exactly once");
    const auto restored = Sent();
    Check(restored.size() == 10, "Stop must restore each edited transport without new physical input");
    for (std::size_t i = 5; i < restored.size(); ++i)
        std::visit([&](const auto &input) {
            using T = std::decay_t<decltype(input)>;
            if constexpr (std::is_base_of_v<Hid_Decoded_Input, T>)
                Check(input.state.axes[0].normalized == -0.4, "HID physical axis restored on stop");
            else Check(Wire(input).state.left_x == -12001, "Native exact physical axis restored on stop");
        }, restored[i]);
}
void IdleMacrosAndFailure() {
    Reset({DeviceFor(4, 9)});
    RemapperService service;
    service.Inject(ScriptCompiler::Compile("int started; main { if(!started) { started=1; combo_run(Tap); } } "
                                           "combo Tap { set_val(EAST,100); wait(30); "
                                           "set_val(EAST,0); wait(30); }"));
    auto physical = Native<Controller_Xinput_Input>(9, 701);
    Queue(physical);
    Await([&] { return service.GetSnapshot().TimerOutputs >= 1; },
          "A macro release must be sent while the physical controller is idle");
    const auto macro = Sent();
    Check(macro.size() >= 2 && (std::get<Controller_Xinput_Input>(macro.front()).state.buttons & 0x2000) &&
              !(std::get<Controller_Xinput_Input>(macro.back()).state.buttons & 0x2000),
          "The timed macro press and release reach the low-level Send_Input boundary");
    for (const auto &packet : macro) {
        const auto &input = std::get<Controller_Xinput_Input>(packet);
        Check(input.device == 9 && input.sequence == 701 && input.state.left_x == physical.state.left_x,
              "Idle macros reuse their original captured source/sequence and preserve physical axes");
    }
    Check(service.GetSnapshot().Received == 1, "An idle macro must not invent a second physical report");
    service.Stop();

    Reset({DeviceFor(4, 9)});
    service.Inject(ScriptCompiler::Compile("int divisor; main {set_val(LX,100);set_val(LY,1/divisor);}"));
    Queue(physical);
    Await([&] { return !service.IsRunning(); }, "A script exception must release capture");
    const auto fault = service.GetSnapshot();
    const auto fallback = Sent();
    Check(fault.Faults == 1 && fault.Error.find("Division by zero") != std::string::npos &&
              fallback.size() == 1 &&
              std::get<Controller_Xinput_Input>(fallback[0]).state.left_x == physical.state.left_x,
          "Partial script edits cannot leak after a runtime fault; the original packet is forwarded");
    service.Stop();

    Reset({DeviceFor(4, 9)});
    service.Inject(ScriptCompiler::Compile("main { set_val(LX,100); }"));
    const auto beforeClose = closes.load();
    sendError = ERROR_WRITE_FAULT;
    Queue(physical);
    Await([&] { return !service.IsRunning(); }, "A rejected output must release capture");
    const auto rejectedSnapshot = service.GetSnapshot();
    const auto recovery = Sent();
    Check(closes == beforeClose + 1 && rejectedSnapshot.Faults == 1 &&
              rejectedSnapshot.Error.find("Forward controller report") != std::string::npos &&
              rejectedSnapshot.Changed == 1 && recovery.size() == 1 &&
              std::get<Controller_Xinput_Input>(recovery[0]).state.left_x == physical.state.left_x,
          "A failed Send_Input records its precise stage and restores physical input before close");
    service.Stop();

    Reset({DeviceFor(4, 9)});
    service.Inject(ScriptCompiler::Compile("main { set_val(LX,100); }"));
    const auto beforeUnrecoverable = closes.load();
    rejectAll = true;
    Queue(physical);
    Await([&] { return !service.IsRunning(); }, "An unrecoverable Send_Input failure must release capture");
    const auto unavailable = service.GetSnapshot();
    Check(closes == beforeUnrecoverable + 1 && unavailable.Faults == 1 && SentCount() == 0 &&
              unavailable.Devices.size() == 1 && unavailable.Devices[0].PhysicalValid &&
              !unavailable.Devices[0].OutputValid,
          "When both output and restoration fail, the final output is unknown and capture still closes");
    service.Stop();
}
void BurstForwarding() {
    Reset({DeviceFor(3, 1), DeviceFor(4, 1), DeviceFor(5, 1), DeviceFor(6, 1), DeviceFor(6, 2)});
    RemapperService service;
    service.Inject(ScriptCompiler::Compile("main {}"));
    constexpr unsigned batches = 40, perFamily = 64;
    const auto before = std::chrono::steady_clock::now();
    for (unsigned batch = 0; batch < batches; ++batch) {
        for (unsigned i = 0; i < perFamily; ++i) {
            const auto sequence = static_cast<std::uint64_t>(batch * perFamily + i + 1001);
            Queue(Hid<Controller_Hid_Input>(1, sequence));
            Queue(Native<Controller_Xinput_Input>(1, sequence));
            Queue(Hid<Controller_XinputHid_Input>(1, sequence));
            Queue(Native<Controller_GIP_Input>(1, sequence));
            Queue(Native<Controller_GIP_Raw_Input>(2, sequence));
        }
        Await([&] { return service.GetSnapshot().Received == (batch + 1) * perFamily * 5; },
              "Every transport must survive sustained mixed input bursts");
    }
    service.Stop();
    const auto snapshot = service.GetSnapshot();
    const auto packets = Sent();
    Check(!snapshot.Running && snapshot.Received == batches * perFamily * 5 &&
              snapshot.Forwarded == snapshot.Received && snapshot.Changed == 0 && snapshot.Faults == 0 &&
              packets.size() == snapshot.Received,
          "Mixed bursts must be forwarded exactly once without synthetic edits or lost reports");
    for (std::size_t i = 0; i < packets.size(); ++i)
        std::visit([&](const auto &input) {
            using T = std::decay_t<decltype(input)>;
            Check(Wire(input).sequence == i / 5 + 1001,
                  "Burst forwarding must preserve per-source physical ordering");
            if constexpr (std::is_base_of_v<Hid_Decoded_Input, T>)
                Check(input.state.axes[0].normalized == -0.4 && input.report[5] == 0x5a,
                      "A no-op HID script preserves its exact decoded state and opaque data");
            else
                Check(Wire(input).state.left_x == -12001 && Wire(input).state.packet == 0xaabbccdd,
                      "A no-op native script preserves exact physical state and protocol metadata");
        }, packets[i]);
    std::printf("Mixed transport burst: %llu reports, %.3f s; isolated boundary, not hardware latency\n",
                static_cast<unsigned long long>(snapshot.Received),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count());
}
} // namespace

int main() {
    fixture::ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!fixture::ready) return 1;
    try {
        StartupFailures();
        AllTransports();
        IdleMacrosAndFailure();
        BurstForwarding();
        GR_Stop();
        CloseHandle(fixture::ready);
        std::printf("Production worker checks passed: %u; isolated transport, no driver access\n", fixture::checks);
        return 0;
    } catch (const std::exception &error) {
        GR_Stop();
        CloseHandle(fixture::ready);
        std::fprintf(stderr, "Production worker check failed after %u: %s\n", fixture::checks, error.what());
        return 1;
    }
}
