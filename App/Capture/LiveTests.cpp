#include "Service.h"
#include "../Core/VisualDocument.h"
#include "../ThirdParty/json.hpp"
#include <Windows.h>
#include <psapi.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

namespace gpr {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
Json SnapshotJson(const ServiceSnapshot &value) {
    Json devices = Json::array();
    for (const auto &device : value.Devices)
        devices.push_back({{"Key", device.Key},
                           {"Family", device.Family},
                           {"Name", device.Name},
                           {"Token", device.Token},
                           {"Diagnostic", device.Diagnostic},
                           {"Values", device.Values},
                           {"OutputValues", device.OutputValues},
                           {"PhysicalValid", device.PhysicalValid},
                           {"OutputValid", device.OutputValid},
                           {"OutputAvailable", device.OutputAvailable},
                           {"Available", device.Available}});
    return {{"Running", value.Running},
            {"Status", value.Status},
            {"Error", value.Error},
            {"Received", value.Received},
            {"Forwarded", value.Forwarded},
            {"Changed", value.Changed},
            {"TimerOutputs", value.TimerOutputs},
            {"Faults", value.Faults},
            {"ProcessingMicroseconds", value.ProcessingMicroseconds},
            {"Devices", devices}};
}
void Require(bool condition, const char *reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
std::wstring Bios(const wchar_t *name) {
    wchar_t text[256]{};
    DWORD size = sizeof(text);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\BIOS", name, RRF_RT_REG_SZ, nullptr,
                     text, &size) != ERROR_SUCCESS)
        return {};
    return text;
}
double ProcessCpu() {
    FILETIME created{}, ended{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user))
        return 0;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    return static_cast<double>(k.QuadPart + u.QuadPart) / 10000000.0;
}
} // namespace
int RunLiveTest(const std::wstring &reportPath, int seconds) {
    const auto beginning = Clock::now();
    bool passed = false;
    const auto cpuBefore = ProcessCpu();
    bool initial = false, invalidPreserved = false, reinjected = false, updateRejected = false;
    bool gipPattern = false, hidPattern = false, quietScheduled = false, quietStopped = false;
    std::set<int> gipPoints, hidPoints;
    std::string failure;
    ServiceSnapshot active, swapped, faulted, stopped;
    try {
        wchar_t guard[8]{};
        Require(GetEnvironmentVariableW(L"GAMEPAD_REMAPPER_LAB", guard, 8) == 1 && guard[0] == L'1',
                "Live tests require explicit GAMEPAD_REMAPPER_LAB=1 in a dedicated test VM.");
        Require(Bios(L"SystemManufacturer") == L"Microsoft Corporation" &&
                    Bios(L"SystemProductName") == L"Virtual Machine",
                "The live runner is restricted to the Microsoft Hyper-V laboratory guest.");
        Require(seconds >= 16 && seconds <= 60, "Live tests require 16..60 seconds.");
        const auto script = ScriptCompiler::Compile(
            "main { if(event_press(SOUTH)) combo_run(Tap); if(event_press(BUTTON_1)) combo_run(HidTap); "
            "set_val(LX,-get_ival(LX)); set_val(AXIS_1,-get_ival(AXIS_1)); } "
            "combo Tap { set_val(EAST,100); wait(450); set_val(EAST,0); wait(35); } "
            "combo HidTap { set_val(BUTTON_2,100); wait(450); set_val(BUTTON_2,0); wait(35); }");
        RemapperService service;
        service.Inject(script);
        initial = service.IsRunning();
        auto stage = Clock::now();
        while (Clock::now() - stage < std::chrono::seconds(4)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            active = service.GetSnapshot();
        }
        Require(active.Running && active.Received && active.Changed && active.TimerOutputs && !active.Faults,
                "The fixture did not establish physical remapping and scheduled output.");
        try {
            ScriptCompiler::Compile("main {");
        } catch (const ScriptException &) {
            invalidPreserved = service.IsRunning();
        }
        auto visual = VisualDocument::Parse("main { }");
        visual.Macros.push_back(VisualDocument::CreateFigureEightPreset());
        auto hid = VisualDocument::CreateFigureEightPreset();
        hid.Name = "HidEight";
        hid.Pattern.TargetX = "AXIS_1";
        hid.Pattern.TargetY = "AXIS_2";
        VisualCondition condition;
        condition.Kind = VisualConditionKind::All;
        VisualCondition one;
        one.Control = "BUTTON_1";
        VisualCondition two;
        two.Control = "BUTTON_2";
        condition.Children = {one, two};
        hid.Condition = condition;
        visual.Macros.push_back(hid);
        const auto complex = ScriptCompiler::Compile(visual.BuildSource());
        Require(service.TryUpdate(complex), "Live update rejected running capture.");
        bool havePrevious = false;
        ServiceSnapshot previous;
        auto stableFrom = Clock::now();
        stage = stableFrom;
        while (Clock::now() - stage < std::chrono::seconds(seconds - 4)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            swapped = service.GetSnapshot();
            Require(swapped.Running && !swapped.Faults, "The compound pattern stopped capture.");
            bool anyActive = false, allRestored = true;
            for (const auto &device : swapped.Devices) {
                const bool gip = device.Family == 6;
                if (!gip && device.Family != 3)
                    continue;
                const int x = gip ? 0 : 96, y = gip ? 1 : 97;
                const bool triggered =
                    gip ? device.Values[10] > 0 && device.Values[11] > 0 && device.Values[2] > 50
                        : device.Values[32] > 0 && device.Values[33] > 0;
                anyActive |= triggered;
                const auto ox = device.OutputValues[x], oy = device.OutputValues[y];
                Require(device.PhysicalValid && device.OutputValid &&
                            device.OutputAvailable[x] && device.OutputAvailable[y],
                        "Pattern snapshots must contain observed input and accepted output, not pending values.");
                if (triggered) {
                    Require(std::abs(ox - device.Values[x]) <= 25 && std::abs(oy - device.Values[y]) <= 25,
                            "An additive pattern drifted away from its physical center.");
                    if (ox != device.Values[x] || oy != device.Values[y]) {
                        if (gip) {
                            gipPattern = true;
                            gipPoints.insert((ox + 100) * 201 + oy + 100);
                        } else {
                            hidPattern = true;
                            hidPoints.insert((ox + 100) * 201 + oy + 100);
                        }
                    }
                }
                allRestored &= ox == device.Values[x] && oy == device.Values[y];
            }
            if (!havePrevious || previous.Received != swapped.Received)
                stableFrom = Clock::now();
            else if (Clock::now() - stableFrom > std::chrono::milliseconds(180)) {
                quietScheduled |= anyActive && swapped.TimerOutputs > previous.TimerOutputs;
                quietStopped |= !anyActive && allRestored && swapped.TimerOutputs == previous.TimerOutputs;
            }
            previous = swapped;
            havePrevious = true;
        }
        Require(
            gipPattern && hidPattern && gipPoints.size() >= 5 && hidPoints.size() >= 5 && quietScheduled &&
                quietStopped,
            "Both HID/GIP additive patterns, quiet scheduling and physical restoration must be observed.");
        service.Inject(ScriptCompiler::Compile("int divisor; main { set_val(LX,100/divisor); }"));
        stage = Clock::now();
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            faulted = service.GetSnapshot();
        } while (faulted.Running && Clock::now() - stage < std::chrono::seconds(3));
        Require(!faulted.Running && faulted.Faults && !faulted.Error.empty(),
                "A runtime fault must release capture and explain its cause.");
        updateRejected = !service.TryUpdate(complex) && !service.IsRunning();
        service.Inject(ScriptCompiler::Compile("main {}"));
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        reinjected = service.IsRunning();
        service.Stop();
        stopped = service.GetSnapshot();
        updateRejected &= !service.TryUpdate(complex) && !service.IsRunning();
        passed = initial && invalidPreserved && reinjected && updateRejected && !stopped.Running &&
                 !stopped.Faults;
    } catch (const ScriptException &error) {
        failure = error.Diagnostic();
    } catch (const std::exception &error) {
        failure = error.what();
    }
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                         sizeof(memory));
    Json report = {{"Passed", passed},
                   {"Error", failure},
                   {"InitialCaptureActive", initial},
                   {"InvalidSourcePreservedCapture", invalidPreserved},
                   {"ReinjectAfterFault", reinjected},
                   {"LiveUpdateCannotRestartCapture", updateRejected},
                   {"GipAdditiveFigureEight", gipPattern},
                   {"HidAdditiveFigureEight", hidPattern},
                   {"GipDistinctPatternPoints", gipPoints.size()},
                   {"HidDistinctPatternPoints", hidPoints.size()},
                   {"ScheduledDuringQuietInput", quietScheduled},
                   {"RestoredAndIdleWhenConditionCeases", quietStopped},
                   {"Active", SnapshotJson(active)},
                   {"Swapped", SnapshotJson(swapped)},
                   {"Faulted", SnapshotJson(faulted)},
                   {"Stopped", SnapshotJson(stopped)},
                   {"ElapsedSeconds", std::chrono::duration<double>(Clock::now() - beginning).count()},
                   {"ProcessCpuSeconds", ProcessCpu() - cpuBefore},
                   {"PrivateBytes", memory.PrivateUsage},
                   {"Scope", "Native C++ compiler, VM, capture worker and signed driver with virtual HID/GIP "
                             "fixtures; no hardware latency claim."}};
    const std::filesystem::path path(reportPath);
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << report.dump(2);
    return passed ? 0 : 1;
}
} // namespace gpr
