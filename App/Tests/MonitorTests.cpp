// Compile the actual SDK parser with an input-I/O tripwire. All monitor I/O is
// provided by the event-backed fixture; accidental context/trial calls fail.
#include <Windows.h>
#include <atomic>
std::atomic<unsigned> forbiddenIo{0};
volatile ULONG_PTR forbiddenHandle = static_cast<ULONG_PTR>(-1);
volatile BOOL forbiddenResult = FALSE;
volatile DWORD forbiddenBytes = 0;
__declspec(noinline) HANDLE WINAPI ForbiddenOpen(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE) {
    ++forbiddenIo; SetLastError(ERROR_ACCESS_DENIED); return reinterpret_cast<HANDLE>(forbiddenHandle);
}
__declspec(noinline) BOOL WINAPI ForbiddenIo(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD returned, LPOVERLAPPED) {
    ++forbiddenIo; if (returned) *returned = forbiddenBytes;
    SetLastError(ERROR_ACCESS_DENIED); return forbiddenResult;
}
#define CreateFileW ForbiddenOpen
#define DeviceIoControl ForbiddenIo
#include "../../Interceptor/Interceptor.cpp"
#undef CreateFileW
#undef DeviceIoControl
#include "MonitorFixture.h"
#include <cstdio>

namespace reader_fixture {
bool controlMode = false, wireActive = false;
std::uint64_t wireSession = 0;
unsigned wireQueries = 0, wireSubscriptions = 0;
HANDLE wireEvent = nullptr;
struct Io {
    std::mutex Gate;
    PTP_WIN32_IO_CALLBACK Callback = nullptr;
    void *Context = nullptr;
    OVERLAPPED *Request = nullptr;
    bool Pending = false, Cancelled = false, ImmediateError = false, CancelledCallbacks = false;
    unsigned Reads = 0, ReadsAfterCancel = 0, Closed = 0, Waits = 0, ImmediateCancels = 0;
    unsigned ReadBytes = 0, PreparsedFreed = 0;
    unsigned NormalLength = 8;
    std::thread Completing;
} io;
std::uint8_t preparsed[1024]{};
BOOLEAN __stdcall Preparsed(HANDLE, PHIDP_PREPARSED_DATA *data) {
    *data = reinterpret_cast<PHIDP_PREPARSED_DATA>(preparsed); return TRUE;
}
BOOLEAN __stdcall FreePreparsed(PHIDP_PREPARSED_DATA) { ++io.PreparsedFreed; return TRUE; }
NTSTATUS __stdcall ReportCaps(PHIDP_PREPARSED_DATA data, PHIDP_CAPS caps) {
    if (data != reinterpret_cast<PHIDP_PREPARSED_DATA>(preparsed)) return HidP_GetCaps(data, caps);
    *caps = {}; caps->InputReportByteLength = static_cast<USHORT>(io.NormalLength); return HIDP_STATUS_SUCCESS;
}
HANDLE WINAPI Open(LPCWSTR, DWORD access, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD flags, HANDLE) {
    if (access != GENERIC_READ || (flags != FILE_FLAG_OVERLAPPED && !(controlMode && flags == FILE_ATTRIBUTE_NORMAL))) {
        SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE;
    }
    return CreateEventW(nullptr, TRUE, FALSE, nullptr);
}
BOOL WINAPI ControlIo(HANDLE, DWORD code, LPVOID input, DWORD, LPVOID output, DWORD bytes,
                      LPDWORD returned, LPOVERLAPPED) {
    if (!controlMode) { ++forbiddenIo; SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    *returned = 0;
    if (code == IOCTL_IC_MONITOR_QUERY && bytes >= sizeof(IC_MONITOR_CAPABILITIES)) {
        auto &caps = *static_cast<IC_MONITOR_CAPABILITIES *>(output);
        caps = {sizeof(caps), IC_MONITOR_VERSION, sizeof(IC_MONITOR_FRAME), IC_MONITOR_CAPACITY, 10000000, wireSession};
        *returned = sizeof(caps); ++wireQueries; return TRUE;
    }
    if (code == IOCTL_IC_MONITOR_SUBSCRIBE) {
        const auto &request = *static_cast<IC_MONITOR_SUBSCRIBE *>(input);
        if (!wireActive) ++wireSession;
        wireActive = true; wireEvent = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(request.event_handle));
        ++wireSubscriptions; return TRUE;
    }
    if (code == IOCTL_IC_MONITOR_READ && bytes >= sizeof(IC_MONITOR_READ_RESULT)) {
        auto &result = *static_cast<IC_MONITOR_READ_RESULT *>(output);
        result = {IC_MONITOR_VERSION, sizeof(IC_MONITOR_FRAME), 0, IC_MONITOR_CAPACITY, wireSession, 1, 0};
        *returned = sizeof(result); return TRUE;
    }
    SetLastError(ERROR_INVALID_FUNCTION); return FALSE;
}
PTP_IO WINAPI Create(HANDLE, PTP_WIN32_IO_CALLBACK callback, PVOID context, PTP_CALLBACK_ENVIRON) {
    std::lock_guard<std::mutex> lock(io.Gate);
    io.Callback = callback; io.Context = context; return reinterpret_cast<PTP_IO>(&io);
}
void WINAPI Start(PTP_IO) {}
void WINAPI CancelNotification(PTP_IO) { ++io.ImmediateCancels; }
BOOL WINAPI Read(HANDLE, LPVOID, DWORD bytes, LPDWORD, LPOVERLAPPED request) {
    std::lock_guard<std::mutex> lock(io.Gate);
    ++io.Reads;
    io.ReadBytes = bytes;
    if (io.Cancelled) ++io.ReadsAfterCancel;
    if (io.ImmediateError) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    io.Pending = true; io.Request = request; SetLastError(ERROR_IO_PENDING); return FALSE;
}
BOOL WINAPI Cancel(HANDLE, LPOVERLAPPED) {
    std::lock_guard<std::mutex> lock(io.Gate); io.Cancelled = true; return TRUE;
}
void Complete(ULONG status) {
    PTP_WIN32_IO_CALLBACK callback = nullptr; void *context = nullptr; OVERLAPPED *request = nullptr;
    {
        std::lock_guard<std::mutex> lock(io.Gate);
        if (!io.Pending) return;
        callback = io.Callback; context = io.Context; request = io.Request; io.Pending = false;
    }
    callback(nullptr, context, request, status, status ? 0 : 4, reinterpret_cast<PTP_IO>(&io));
}
void WINAPI Wait(PTP_IO, BOOL cancelCallbacks) {
    if (io.Completing.joinable()) io.Completing.join();
    ++io.Waits; io.CancelledCallbacks = cancelCallbacks != FALSE;
    // Cancelling queued notifications does not establish that the Windows
    // READ has completed. Only the non-cancelling wait drains that lifetime.
    if (!cancelCallbacks) Complete(ERROR_OPERATION_ABORTED);
}
void WINAPI Close(PTP_IO) {
    std::lock_guard<std::mutex> lock(io.Gate); ++io.Closed; io.Callback = nullptr; io.Context = nullptr;
}
void Reset(bool immediateError = false) {
    std::lock_guard<std::mutex> lock(io.Gate);
    io.Callback = nullptr; io.Context = nullptr; io.Request = nullptr;
    io.Pending = io.Cancelled = io.CancelledCallbacks = false; io.ImmediateError = immediateError;
    io.Reads = io.ReadsAfterCancel = io.Closed = io.Waits = io.ImmediateCancels = 0;
    io.ReadBytes = io.PreparsedFreed = 0;
    io.NormalLength = 8;
}
} // namespace reader_fixture

// Exercise the production reader's cancellation/reissue lifetime without any
// HID file. Its fake file is an event; completions execute the real callback.
#define CreateFileW reader_fixture::Open
#define DeviceIoControl reader_fixture::ControlIo
#define CreateThreadpoolIo reader_fixture::Create
#define StartThreadpoolIo reader_fixture::Start
#define CancelThreadpoolIo reader_fixture::CancelNotification
#define ReadFile reader_fixture::Read
#define CancelIoEx reader_fixture::Cancel
#define WaitForThreadpoolIoCallbacks reader_fixture::Wait
#define CloseThreadpoolIo reader_fixture::Close
#define HidD_GetPreparsedData reader_fixture::Preparsed
#define HidD_FreePreparsedData reader_fixture::FreePreparsed
#define HidP_GetCaps reader_fixture::ReportCaps
#include "../Capture/DriverMonitor.cpp"
#undef CreateFileW
#undef DeviceIoControl
#undef CreateThreadpoolIo
#undef StartThreadpoolIo
#undef CancelThreadpoolIo
#undef ReadFile
#undef CancelIoEx
#undef WaitForThreadpoolIoCallbacks
#undef CloseThreadpoolIo
#undef HidD_GetPreparsedData
#undef HidD_FreePreparsedData
#undef HidP_GetCaps

namespace {
unsigned checks = 0;
void Check(bool condition, const char *reason) {
    ++checks;
    if (!condition) throw std::runtime_error(reason);
}
template <class Predicate> void Await(Predicate predicate, const char *reason) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= until) throw std::runtime_error(reason);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(true, reason);
}
const gpr::ServiceDevice &Find(const gpr::MonitorSnapshot &snapshot, unsigned family) {
    for (const auto &device : snapshot.Devices) if (device.Family == family) return device;
    throw std::runtime_error("Expected passive source not found");
}
// Two independent X usages in different reports/collections plus two buttons.
// Only the HID parser boundary is synthetic: the SDK decoder, dynamic slot
// mapping, IN/OUT state reconstruction and monitor thread are production code.
NTSTATUS __stdcall Caps(PHIDP_PREPARSED_DATA, PHIDP_CAPS caps) {
    *caps = {}; caps->InputReportByteLength = 4; caps->NumberInputValueCaps = 2;
    caps->NumberInputButtonCaps = 1; return HIDP_STATUS_SUCCESS;
}
NTSTATUS __stdcall Values(HIDP_REPORT_TYPE, PHIDP_VALUE_CAPS values, PUSHORT count, PHIDP_PREPARSED_DATA) {
    if (*count < 2) return HIDP_STATUS_BUFFER_TOO_SMALL;
    for (unsigned i = 0; i < 2; ++i) {
        values[i] = {}; auto &cap = values[i]; cap.UsagePage = 1; cap.NotRange.Usage = 0x30;
        cap.NotRange.DataIndex = static_cast<USHORT>(i); cap.ReportID = static_cast<UCHAR>(i + 1);
        cap.LinkCollection = static_cast<USHORT>(i); cap.BitSize = 8; cap.ReportCount = 1;
        cap.IsAbsolute = TRUE; cap.LogicalMax = 255;
    }
    *count = 2; return HIDP_STATUS_SUCCESS;
}
NTSTATUS __stdcall Buttons(HIDP_REPORT_TYPE, PHIDP_BUTTON_CAPS buttons, PUSHORT count, PHIDP_PREPARSED_DATA) {
    if (!*count) return HIDP_STATUS_BUFFER_TOO_SMALL;
    buttons[0] = {}; auto &cap = buttons[0]; cap.UsagePage = 9; cap.ReportID = 1; cap.IsRange = TRUE;
    cap.Range.UsageMin = 1; cap.Range.UsageMax = 2; cap.Range.DataIndexMin = 2; cap.Range.DataIndexMax = 3;
    *count = 1; return HIDP_STATUS_SUCCESS;
}
ULONG __stdcall MaxData(HIDP_REPORT_TYPE, PHIDP_PREPARSED_DATA) { return 4; }
NTSTATUS __stdcall Data(HIDP_REPORT_TYPE, PHIDP_DATA data, PULONG count, PHIDP_PREPARSED_DATA,
                         PCHAR raw, ULONG length) {
    if (length != 4 || (raw[0] != 1 && raw[0] != 2)) return HIDP_STATUS_INVALID_REPORT_LENGTH;
    if (*count < 3) return HIDP_STATUS_BUFFER_TOO_SMALL;
    data[0] = {}; data[0].DataIndex = static_cast<USHORT>(raw[0] - 1);
    data[0].RawValue = static_cast<unsigned char>(raw[1]);
    ULONG used = 1;
    if (raw[0] == 1)
        for (unsigned button = 0; button < 2; ++button)
            if (raw[2] & (1u << button)) {
                data[used] = {}; data[used].DataIndex = static_cast<USHORT>(button + 2);
                data[used++].On = TRUE;
            }
    *count = used; return HIDP_STATUS_SUCCESS;
}
void ConfigureParser() {
    auto &parser = Interceptor::Detail::hidParser;
    parser.caps = Caps; parser.values = Values; parser.buttons = Buttons; parser.data = Data;
    parser.maxData = MaxData;
}
void ParserBounds() {
    using namespace Interceptor;
    for (const auto size : {std::size_t{0}, std::size_t{65537}}) {
        bool rejected = false;
        try { (void)Detail::Create_Hid_Decoder(std::vector<std::uint8_t>(size)); }
        catch (const std::system_error &error) { rejected = error.code().value() == ERROR_INVALID_DATA; }
        Check(rejected, "The pure passive decoder rejects unbounded or absent parser data before any I/O");
    }
    Hid_Decoded_Input input;
    input.state.axes.push_back({});
    const std::uint8_t raw[]{1, 0, 0, 0};
    Detail::Decode_Hid_Input({}, input, raw, IC_HID_REPORT_MAX + 1, 1);
    Check(input.state.axes.empty() && input.decode_error == ERROR_INVALID_DATA && !input.decode_complete,
          "An oversized passive report clears stale decoded state without reading beyond its buffer");
}
void PassiveStates() {
    using namespace monitor_fixture;
    auto state = std::make_shared<State>();
    state->Metadata = {Metadata(3, 1, true), Metadata(4, 1), Metadata(5, 1, true), Metadata(6, 1)};
    std::atomic<unsigned> notifications{0};
    gpr::DriverMonitor monitor([&] { ++notifications; }, Factory(state));
    monitor.Start();
    monitor.Start();
    Await([&] { return monitor.Snapshot().Running; }, "Passive monitor must start without capture");
    auto snapshot = monitor.Snapshot();
    Check(snapshot.Requested && snapshot.Devices.size() == 4 && snapshot.TimestampFrequency == 10000000,
          "Read-only enumeration and QPC frequency must be available without a script");
    Check(state->Opens == 1, "Repeated start while asynchronous setup is pending must remain idempotent");
    for (const auto &device : snapshot.Devices)
        Check(device.Telemetry && !device.PhysicalValid && !device.OutputValid,
              "Enumerated controllers must remain unknown before any observed reports");
    Await([&] { return state->Reads > 0; }, "The initial telemetry read must finish");
    const auto reads = state->Reads.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    Check(state->Reads == reads, "Quiet monitoring must block on the event instead of polling");

    state->Push(Native(4, 1, IC_MONITOR_IN, -32768));
    Await([&] { return monitor.Snapshot().Frames == 1; }, "A signalled IN report must wake the monitor");
    snapshot = monitor.Snapshot();
    Check(Find(snapshot, 4).Values[0] == -100 && Find(snapshot, 4).InputAvailable[0] &&
              !Find(snapshot, 4).OutputAvailable[0] && !Find(snapshot, 4).OutputValid,
          "IN cannot fabricate an OUT observation or zero-valued accepted output");
    state->Push(Native(4, 1, IC_MONITOR_OUT, 32767));
    auto guide = Native(6, 1, IC_MONITOR_IN, 0);
    guide.Flags = IC_MONITOR_GUIDE_VALID; guide.Native.extra_buttons = 1;
    state->Push(guide);
    state->Push(Native(6, 1, IC_MONITOR_IN, 12000));
    state->Push(Hid(3, 1, IC_MONITOR_IN, 1, 255, 1));
    state->Push(Hid(3, 1, IC_MONITOR_OUT, 1, 0, 0));
    state->Push(Hid(3, 1, IC_MONITOR_IN, 2, 128));
    state->Push(Hid(5, 1, IC_MONITOR_IN, 1, 0, 2));
    state->Push(Hid(5, 1, IC_MONITOR_OUT, 1, 255, 1));
    Await([&] { return monitor.Snapshot().Frames == 9; }, "All native/HID telemetry directions must decode");
    snapshot = monitor.Snapshot();
    const auto &xusb = Find(snapshot, 4);
    Check(xusb.Values[0] == -100 && xusb.OutputValues[0] == 100 && xusb.OutputAvailable[0] &&
              xusb.InputSequence != xusb.OutputSequence && xusb.InputRaw.size() == 20,
          "XUSB retains distinct actual IN/OUT states, sequences and raw reports");
    const auto &gip = Find(snapshot, 6);
    Check(gip.Values[20] == 100 && gip.InputAvailable[20] && gip.InputAvailable[0] && !gip.OutputValid,
          "Ordinary GIP reports cannot erase an independently observed Guide press");
    for (const auto family : {3u, 5u}) {
        const auto &hid = Find(snapshot, family);
        Check(hid.InputAvailable[96] && hid.OutputAvailable[96] && hid.InputRaw.size() == 4 &&
                  hid.Values[96] != hid.OutputValues[96] && !hid.Available[0],
              "Generic HID/XInputHID use descriptor slots without guessing console LX positions");
    }
    const auto &hid = Find(snapshot, 3);
    Check(hid.InputAvailable[97] && !hid.OutputAvailable[97] && hid.Values[96] == 100 &&
              hid.OutputValues[96] == -100 && hid.Values[32] == 100 && hid.OutputValues[32] == 0,
          "Different reports/collections reconstruct independently and preserve unknown OUT fields");

    { std::lock_guard<std::mutex> lock(state->Gate); state->Dropped = 7; }
    state->Push(Hid(3, 1, IC_MONITOR_IN, 2, 0));
    Await([&] { return monitor.Snapshot().Dropped == 7; }, "Overflow loss must be published");
    snapshot = monitor.Snapshot();
    Check(!Find(snapshot, 3).InputAvailable[96] && Find(snapshot, 3).InputAvailable[97] &&
              !Find(snapshot, 3).OutputAvailable[96] && Find(snapshot, 3).Available[96] &&
              !Find(snapshot, 4).PhysicalValid,
          "Lost reports invalidate unseen per-report state instead of retaining possible stuck buttons");
    auto removed = Native(4, 1, IC_MONITOR_DEVICE, 0); removed.Flags = IC_MONITOR_REMOVED;
    state->Push(removed);
    Await([&] { return monitor.Snapshot().Frames == 11; }, "A removal record must wake the monitor");
    Check(Find(monitor.Snapshot(), 4).Diagnostic.find("disconnected") != std::string::npos,
          "Disconnected controller state must be explicit");
    const auto metadataBeforeLate = state->Queries.load();
    state->Push(Native(4, 1, IC_MONITOR_OUT, 32767));
    Await([&] { return monitor.Snapshot().Frames == 12; }, "Late Windows completions must remain diagnosable");
    snapshot = monitor.Snapshot();
    Check(!Find(snapshot, 4).PhysicalValid && !Find(snapshot, 4).OutputValid &&
              !Find(snapshot, 4).OutputAvailable[0] && state->Queries == metadataBeforeLate &&
              Find(snapshot, 4).Diagnostic.find("late completion") != std::string::npos,
          "A late OUT cannot reopen metadata or resurrect a disconnected controller's state");
    const auto beforeStop = std::chrono::steady_clock::now();
    monitor.Stop();
    Check(!monitor.Snapshot().Running && state->Closes == 1 && notifications >= 5 &&
              std::chrono::steady_clock::now() - beforeStop < std::chrono::seconds(1),
          "Stopping a quiet monitor signals its wait and joins/releases its observer promptly");
    monitor.Stop();
    Check(state->Closes == 1, "Monitor stop must be idempotent");
}
void DeviceTransportFailure() {
    using namespace monitor_fixture;
    auto state = std::make_shared<State>();
    state->Metadata = {Metadata(4, 1), Metadata(6, 1)};
    gpr::DriverMonitor monitor([] {}, Factory(state));
    monitor.Start();
    Await([&] { return monitor.Snapshot().Running; }, "Device failure fixture starts without capture");
    state->Push(Native(4, 1, IC_MONITOR_IN, -32768));
    state->Push(Native(4, 1, IC_MONITOR_OUT, 32767));
    state->Push(Native(6, 1, IC_MONITOR_IN, 12000));
    Await([&] { return monitor.Snapshot().Frames == 3; }, "Both directions are observed before the transport fails");
    const auto before = Find(monitor.Snapshot(), 4);
    auto failure = Native(4, 1, IC_MONITOR_DEVICE, 0);
    failure.Flags = IC_MONITOR_ERROR; failure.Status = 0xc000009d;
    state->Push(failure);
    Await([&] { return monitor.Snapshot().Frames == 4; }, "A device-wide transport error wakes the passive monitor");
    auto snapshot = monitor.Snapshot();
    const auto &failed = Find(snapshot, 4);
    Check(snapshot.Running && snapshot.Error.empty() && !failed.PhysicalValid && !failed.OutputValid &&
              failed.InputStatus == failure.Status && failed.OutputStatus == failure.Status &&
              failed.Diagnostic.find("c000009d") != std::string::npos && Find(snapshot, 6).PhysicalValid,
          "A DEVICE error invalidates both source directions, diagnoses its status, and leaves other controllers active");
    Check(failed.InputSequence == before.InputSequence && failed.OutputSequence == before.OutputSequence &&
              failed.InputRaw == before.InputRaw && failed.OutputRaw == before.OutputRaw,
          "A transport failure preserves previous real report bytes and cannot manufacture an IN or OUT completion");
    auto ready = Native(4, 1, IC_MONITOR_DEVICE, 0); ready.Flags = 0;
    state->Push(ready);
    Await([&] { return monitor.Snapshot().Frames == 5; }, "Readiness can return after a device transport failure");
    Check(!Find(monitor.Snapshot(), 4).PhysicalValid && !Find(monitor.Snapshot(), 4).OutputValid,
          "A successful readiness event does not revalidate stale report values");
    state->Push(Native(4, 1, IC_MONITOR_IN, 123));
    Await([&] { return monitor.Snapshot().Frames == 6; }, "A fresh physical report recovers its own direction");
    snapshot = monitor.Snapshot();
    Check(Find(snapshot, 4).PhysicalValid && !Find(snapshot, 4).OutputValid &&
              !Find(snapshot, 4).InputStatus && Find(snapshot, 4).OutputStatus == failure.Status,
          "Recovery of IN cannot manufacture recovery of OUT after a DEVICE error");
    monitor.Stop();
}
void FailuresAndLifetime() {
    using namespace monitor_fixture;
    auto state = std::make_shared<State>();
    state->OpenError = "Open passive controller monitor (Win32 5): acc\xc3\xa8s refus\xc3\xa9";
    gpr::DriverMonitor monitor([] {}, Factory(state));
    monitor.Start();
    Await([&] { return !monitor.Snapshot().Error.empty(); }, "Access failure must be published asynchronously");
    Check(!monitor.Snapshot().Running && monitor.Snapshot().Requested && state->Opens == 1,
          "A failed observer must not silently retry or create a capture");
    { std::lock_guard<std::mutex> lock(state->Gate); state->OpenError.clear(); }
    monitor.Start();
    Await([&] { return monitor.Snapshot().Running; }, "Explicit retry can replace a failed observer");
    auto malformed = Native(4, 1, IC_MONITOR_IN, 0); malformed.Raw.resize(1025);
    state->Push(std::move(malformed));
    Await([&] { return !monitor.Snapshot().Running; }, "Oversized telemetry must stop safely");
    Check(monitor.Snapshot().Error.find("shape") != std::string::npos && state->Closes == 1,
          "Malformed telemetry retains its exact failing stage and closes its handle");
    monitor.Stop();

    for (unsigned round = 0; round < 8; ++round) {
        auto concurrent = std::make_shared<State>();
        concurrent->Metadata.push_back(Metadata(4, 1));
        gpr::DriverMonitor active([] {}, Factory(concurrent));
        active.Start();
        Await([&] { return active.Snapshot().Running; }, "Repeated observer lifetime must start");
        for (unsigned index = 0; index < 320; ++index)
            concurrent->Push(Native(4, 1, index & 1 ? IC_MONITOR_OUT : IC_MONITOR_IN,
                                     static_cast<int>(index % 32768)));
        Await([&] { return active.Snapshot().Frames == 320; }, "Bounded reads must drain multi-batch bursts");
        Check(active.Snapshot().Dropped == 0 && active.Snapshot().Error.empty(),
              "Concurrent snapshot readers must not corrupt multi-batch monitor state");
        active.Stop();
        Check(concurrent->Opens == 1 && concurrent->Closes == 1,
              "Each observer lifetime must close exactly its own transport");
    }
}
void ReaderLifetime() {
    using namespace reader_fixture;
    Reset();
    {
        gpr::HidConsumer reader(L"isolated-fixture");
        Check(io.Reads == 1 && io.ReadBytes == 8 && io.PreparsedFreed == 1 && !reader.Error(),
              "A normal reader sizes its one bounded READ from its own HID capabilities and frees parser data");
        Complete(ERROR_SUCCESS);
        Check(io.Reads == 2, "A successful read completion reissues without decoding or allocating reports");
    }
    Check(io.ReadsAfterCancel == 0 && io.Waits == 1 && io.Closed == 1 && !io.Pending && !io.CancelledCallbacks,
          "Reader shutdown waits for actual READ completion without cancelling its terminal callback");
    for (unsigned race = 0; race < 64; ++race) {
        Reset();
        {
            gpr::HidConsumer reader(L"isolated-fixture");
            io.Completing = std::thread([] { Complete(ERROR_SUCCESS); });
        }
        Check(io.ReadsAfterCancel == 0 && io.Closed == 1 && io.Waits == 1 && !io.Pending && !io.CancelledCallbacks,
              "Concurrent completion and cancellation cannot submit a read after stop or outlive the reader");
    }
    Reset(true);
    {
        gpr::HidConsumer reader(L"isolated-fixture");
        Check(reader.Error() == ERROR_ACCESS_DENIED && io.ImmediateCancels == 1,
              "Immediate ReadFile failure cancels its threadpool notification and keeps the exact error");
    }
    Check(io.Reads == 1 && io.Closed == 1 && io.ReadsAfterCancel == 0,
          "A failed reader cannot spin or reissue during destruction");
    for (const auto length : {0u, 1025u, 65535u}) {
        Reset(); io.NormalLength = length;
        bool invalid = false;
        try { gpr::HidConsumer reader(L"isolated-fixture"); }
        catch (const std::exception &error) {
            invalid = std::strstr(error.what(), "input-report length") != nullptr;
        }
        Check(invalid && io.Reads == 0 && io.PreparsedFreed == 1 && io.Closed == 0,
              "An invalid normal HID report length frees parser data without creating I/O or submitting a read");
    }
}
void WireSubscriptionSession() {
    using namespace reader_fixture;
    controlMode = true; wireActive = false; wireSession = 0; wireQueries = wireSubscriptions = 0;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(event != nullptr, "The wire fixture creates only a notification event");
    for (std::uint64_t expected = 1; expected <= 3; ++expected) {
        {
            gpr::WindowsTransport transport;
            const auto caps = transport.Open(event);
            const auto empty = transport.Read(IC_MONITOR_MAX_READ);
            Check(caps.Session == expected && empty.Session == expected && caps.TimestampFrequency == 10000000 &&
                      caps.Capacity == IC_MONITOR_CAPACITY && wireEvent == event,
                  "The actual Windows backend pins the post-subscription ring session on first boot and reopen");
        }
        // Model kernel FILE cleanup after the final observer closes. QUERY now
        // exposes the retired session; the next SUBSCRIBE creates its successor.
        wireActive = false;
    }
    Check(wireQueries == 6 && wireSubscriptions == 3,
          "Every observer validates the ABI before and after subscription without creating capture");
    CloseHandle(event); wireEvent = nullptr; controlMode = false;
}
} // namespace
int main() {
    try {
        ConfigureParser();
        ParserBounds();
        PassiveStates();
        DeviceTransportFailure();
        FailuresAndLifetime();
        ReaderLifetime();
        WireSubscriptionSession();
        Check(forbiddenIo == 0, "Passive monitoring must never open the SDK capture or query its trial");
        std::printf("Passive monitor checks passed: %u; actual decoder/worker, fake event transport, zero SDK input I/O\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Passive monitor failure after %u: %s\n", checks, error.what());
        return 1;
    }
}
