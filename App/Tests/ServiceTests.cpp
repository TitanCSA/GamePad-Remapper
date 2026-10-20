#include "../Capture/Service.h"
#include "../Capture/Bridge.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <atomic>
#include <thread>

// Deterministic callback-boundary test backend. This executable never links
// the SDK or real capture worker and cannot create a driver context.
namespace {
GR_Callback callback = nullptr;
void *callbackUser = nullptr;
GR_Snapshot snapshot{};
GR_Device deviceSnapshot{};
bool exposeDevice = false;
std::array<int, 256> lastOutput{};
int lastWake = 0;
std::uint32_t startError = 0;
int starts = 0, stops = 0, checks = 0, parameterWakes = 0;
void Check(bool condition, const char *reason) {
    ++checks;
    if (!condition)
        throw std::runtime_error(reason);
}
int Invoke(std::uint32_t key, int input = 20, std::uint32_t flags = 0) {
    std::array<int, 256> physical{}, output{};
    std::array<std::uint8_t, 256> available{};
    physical[0] = input;
    physical[1] = -13;
    available.fill(1);
    const auto next =
        callback(key, 6, physical.data(), available.data(), output.data(), flags, callbackUser);
    lastOutput = output;
    lastWake = next;
    if (next < 0) {
        snapshot.running = 0;
        ++snapshot.faults;
    }
    return output[0];
}
} // namespace
std::uint32_t __cdecl GR_Probe(wchar_t *message, std::uint32_t characters) {
    if (message && characters)
        wcsncpy_s(message, characters, L"Test capture backend", _TRUNCATE);
    return 0;
}
std::uint32_t __cdecl GR_Start(GR_Callback cb, void *user) {
    ++starts;
    if (snapshot.running)
        return ERROR_BUSY;
    snapshot = {};
    snapshot.size = sizeof(snapshot);
    snapshot.version = 2;
    exposeDevice = false;
    if (startError) {
        wcscpy_s(snapshot.error, L"A precise injected startup cause");
        return startError;
    }
    callback = cb;
    callbackUser = user;
    snapshot.running = 1;
    return 0;
}
void __cdecl GR_Stop() {
    ++stops;
    snapshot.running = 0;
    callback = nullptr;
    callbackUser = nullptr;
}
void __cdecl GR_Wake() {}
void __cdecl GR_ParametersChanged() { ++parameterWakes; }
std::uint32_t __cdecl GR_GetSnapshot(GR_Snapshot *output) {
    if (!output || output->size != sizeof(*output))
        return ERROR_INVALID_PARAMETER;
    *output = snapshot;
    output->size = sizeof(*output);
    output->version = 2;
    return 0;
}
std::uint32_t __cdecl GR_GetDevice(std::uint32_t index, GR_Device *output) {
    if (!output || output->size != sizeof(*output))
        return ERROR_INVALID_PARAMETER;
    if (!exposeDevice || index != 0)
        return ERROR_NO_MORE_ITEMS;
    *output = deviceSnapshot;
    return 0;
}
int main() {
    try {
        using namespace gpr;
        const auto counting = ScriptCompiler::Compile("int count; main { count++; set_val(LX,count); }");
        RemapperService service;
        Check(!service.IsRunning() && starts == 0, "Opening a service must not start capture.");
        Check(!service.TryUpdate(counting) && starts == 0, "Live apply cannot start an idle capture.");
        service.Inject(counting);
        Check(service.IsRunning(), "Explicit injection starts capture.");
        Check(Invoke(1) == 1 && Invoke(1) == 2 && Invoke(2) == 1,
              "Each source owns independent script state.");
        service.Inject(counting);
        Check(Invoke(1, 20, 2) == 1 && Invoke(2, 20, 2) == 1 && Invoke(1) == 2,
              "Reinjecting the same program resets once without dropping a previous reset source.");
        Check(service.TryUpdate(counting) && Invoke(1, 20, 2) == 1 && Invoke(2, 20, 2) == 1 &&
                  Invoke(1) == 2,
              "Live replacement of the same shared program also owns a fresh reset generation across "
              "sources.");
        const auto startsBeforeNull = starts;
        bool rejectedNull = false;
        try {
            service.Inject({});
        } catch (const std::invalid_argument &) {
            rejectedNull = true;
        }
        Check(rejectedNull && starts == startsBeforeNull && service.IsRunning() && Invoke(1) == 3,
              "Null injection cannot replace or disturb the active configuration.");
        rejectedNull = false;
        try {
            (void)service.TryUpdate({});
        } catch (const std::invalid_argument &) {
            rejectedNull = true;
        }
        Check(rejectedNull && service.IsRunning() && Invoke(1) == 4,
              "Null live replacement preserves active VM state.");
        Check(Invoke(1, 20, 3) == 0 && Invoke(1) == 1, "Forgetting a source releases its old VM.");
        const auto inverted = ScriptCompiler::Compile("main { set_val(LX,-get_ival(LX)); }");
        Check(service.TryUpdate(inverted) && Invoke(1, 37) == -37,
              "Live replacement reaches the next callback.");
        bool invalid = false;
        try {
            ScriptCompiler::Compile("main {");
        } catch (const ScriptException &) {
            invalid = true;
        }
        Check(invalid && service.IsRunning() && Invoke(1, 12) == -12,
              "A rejected compile cannot replace the active script.");
        {
            RemapperService second;
            bool busy = false;
            try {
                second.Inject(counting);
            } catch (const std::exception &) {
                busy = true;
            }
            Check(busy, "A second owner must not replace an active capture.");
        }
        Check(service.IsRunning() && Invoke(1, 12) == -12,
              "A failed second owner cannot stop the original capture.");
        deviceSnapshot = {};
        deviceSnapshot.size = sizeof(deviceSnapshot);
        deviceSnapshot.key = 73;
        deviceSnapshot.family = 3;
        deviceSnapshot.flags = GR_PHYSICAL_VALID | GR_OUTPUT_VALID;
        wcscpy_s(deviceSnapshot.name, L"Controller \u00e9");
        wcscpy_s(deviceSnapshot.token, L"capture-local-token");
        wcscpy_s(deviceSnapshot.diagnostic, L"Descriptor-driven test source");
        deviceSnapshot.available[96] = 3;
        deviceSnapshot.values[96] = -40;
        deviceSnapshot.output_values[96] = 30;
        exposeDevice = true;
        snapshot.device_count = 1;
        auto detached = service.GetSnapshot();
        Check(detached.Devices.size() == 1 && detached.Devices[0].Name == "Controller \xc3\xa9" &&
                  detached.Devices[0].Token == "capture-local-token" &&
                  detached.Devices[0].Available[96] && detached.Devices[0].Values[96] == -40 &&
                  detached.Devices[0].OutputValues[96] == 30 &&
                  detached.Devices[0].PhysicalValid && detached.Devices[0].OutputValid &&
                  detached.Devices[0].OutputAvailable[96],
              "Detached snapshots preserve UTF-8 identity, availability and physical/output arrays.");
        detached.Devices[0].Values[96] = 99;
        detached.Devices[0].OutputValues[96] = 99;
        detached.Devices[0].Name = "edited by consumer";
        const auto fresh = service.GetSnapshot();
        Check(fresh.Devices[0].Values[96] == -40 && fresh.Devices[0].OutputValues[96] == 30 &&
                  fresh.Devices[0].Name == "Controller \xc3\xa9",
              "A consumer cannot mutate the native snapshot or subsequent service snapshots.");
        deviceSnapshot.flags = GR_PHYSICAL_VALID;
        Check(service.GetSnapshot().Devices[0].PhysicalValid &&
                  !service.GetSnapshot().Devices[0].OutputValid,
              "An output rejected by the driver cannot masquerade as an accepted output.");
        deviceSnapshot.flags = GR_PHYSICAL_VALID | GR_OUTPUT_VALID;
        deviceSnapshot.available[32] = 1;
        deviceSnapshot.values[32] = 100;
        const auto partial = service.GetSnapshot();
        Check(partial.Devices[0].OutputAvailable[96] && partial.Devices[0].Available[32] &&
                  !partial.Devices[0].OutputAvailable[32],
              "A rejected new HID report cannot borrow output validity from another accepted report.");
        exposeDevice = false;
        snapshot.device_count = 0;
        service.Inject(ScriptCompiler::Compile(
            "main "
            "{set_val(VALUE_1,get_device());set_val(VALUE_2,get_controller());if(is_"
            "available(LX)){set_val(LX,50);tick(4);}}"));
        Check(Invoke(73) == 50 && lastWake == 4 && lastOutput[208] == 73 && lastOutput[209] == 6,
              "Callbacks propagate family, capture-local device identity and scoped scheduling.");
        service.Inject(
            ScriptCompiler::Compile("int zero; main { set_val(LY,99);set_val(LX,100/zero); }"));
        Check(Invoke(1, 28) == 28 && lastOutput[1] == -13,
              "A script fault forwards every original physical value after partial modifications.");
        const auto fault = service.GetSnapshot();
        Check(!fault.Running && fault.Faults == 1 &&
                  fault.Error.find("Division by zero") != std::string::npos,
              "Runtime faults publish the script cause and stop capture.");
        Check(!service.TryUpdate(counting), "Live apply cannot restart faulted capture.");
        service.Inject(counting);
        Check(Invoke(1) == 1, "Explicit injection recovers after a fault.");
        service.Stop();
        Check(!service.IsRunning() && !service.TryUpdate(counting),
              "Stop releases capture and blocks live restart.");
        const auto stopsBeforeIdle = stops;
        service.Stop();
        service.Stop();
        Check(stops == stopsBeforeIdle && !service.IsRunning(),
              "Stopping an idle owner is idempotent and cannot touch another capture.");
        startError = ERROR_NOT_READY;
        bool precise = false;
        try {
            service.Inject(counting);
        } catch (const std::exception &error) {
            precise = std::strstr(error.what(), "precise injected startup cause") != nullptr;
        }
        Check(precise && !service.IsRunning(), "Startup failures retain their actual published cause.");
        startError = 0;
        service.Inject(counting);
        Check(Invoke(1) == 1, "Startup failure leaves the service reusable.");
        service.Stop();
        const auto configurable =
            ScriptCompiler::Compile("ui {slider gain(\"Gain\",0,100,20);} int count; main "
                                    "{count++;set_val(LX,gain);set_val(VALUE_1,count);}");
        auto parameters = configurable->Interface().Defaults();
        Check(!service.SetParameters(configurable, parameters),
              "Editing parameters cannot start capture.");
        service.Inject(configurable, parameters);
        Check(Invoke(1) == 20 && lastOutput[208] == 1 && Invoke(2) == 20,
              "Defaults reach every source.");
        const auto startsBeforeParameters = starts;
        for (int i = 0; i < 10000; ++i) {
            parameters[0] = i % 101;
            Check(service.SetParameters(configurable, parameters) && Invoke(1, 20, 4) == parameters[0] &&
                      lastOutput[208] == i + 2,
                  "10,000 parameter updates preserve VM globals and capture ownership.");
        }
        Check(starts == startsBeforeParameters && parameterWakes == 10000,
              "Parameter edits wake evaluation without restarting capture.");
        Check(Invoke(2) == parameters[0] && lastOutput[208] == 2 && Invoke(3) == parameters[0] &&
                  lastOutput[208] == 1,
              "Existing and newly attached devices use the current settings with independent VM state.");
        Check(!service.SetParameters(counting, parameters),
              "An old or different script cannot update the active program's settings.");
        bool invalidParameters = false;
        parameters[0] = 101;
        try {
            service.SetParameters(configurable, parameters);
        } catch (const std::invalid_argument &) {
            invalidParameters = true;
        }
        Check(invalidParameters && service.IsRunning(),
              "Invalid settings leave the running program untouched.");
        service.Inject(configurable);
        Check(Invoke(1) == 20 && lastOutput[208] == 1,
              "Explicit reinjection still resets state even with the same program pointer.");
        std::atomic<bool> finished{false};
        std::atomic<int> concurrentFrames{0};
        std::atomic<bool> monotonic{true};
        std::thread worker([&] {
            int previousCount = 1;
            while (!finished.load(std::memory_order_acquire)) {
                const int output = Invoke(1);
                if (output < 0 || output > 100 || lastOutput[208] != ++previousCount)
                    monotonic.store(false);
                concurrentFrames.fetch_add(1, std::memory_order_release);
            }
        });
        try {
            while (concurrentFrames.load(std::memory_order_acquire) < 1000)
                std::this_thread::yield();
            for (int i = 0; i < 10000; ++i) {
                parameters[0] = i % 101;
                service.SetParameters(configurable, parameters);
            }
        } catch (...) {
            finished.store(true, std::memory_order_release);
            worker.join();
            throw;
        }
        finished.store(true, std::memory_order_release);
        worker.join();
        Check(monotonic.load() && concurrentFrames.load() >= 1000 && service.IsRunning(),
              "Concurrent parameter publication preserves whole valid snapshots and never resets the "
              "running VM.");
        service.Stop();
        std::printf("Service boundary checks passed: %d; fake backend, no driver access\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Service test failure: %s\n", error.what());
        return 1;
    }
}
