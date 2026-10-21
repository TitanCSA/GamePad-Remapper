#pragma once
#include "../Core/Script.h"
#include <array>
#include <cstdint>
#include <memory>
#include <functional>
#include <string>
#include <vector>

namespace gpr {
class MonitorTransport;
struct ServiceDevice {
    std::uint32_t Key = 0, Family = 0;
    std::string Name, Token, Diagnostic;
    std::array<int, 256> Values{}, OutputValues{};
    std::array<bool, 256> Available{}, InputAvailable{}, OutputAvailable{};
    bool PhysicalValid = false, OutputValid = false;
    bool Telemetry = false;
    std::uint64_t InputSequence = 0, OutputSequence = 0;
    std::uint64_t InputTimestamp = 0, OutputTimestamp = 0;
    std::uint32_t InputStatus = 0, OutputStatus = 0;
    std::vector<std::uint8_t> InputRaw, OutputRaw;
};
struct ServiceSnapshot {
    std::string Status = "Ready", Error;
    bool Running = false;
    bool Monitoring = false;
    std::string MonitorStatus, MonitorError;
    std::uint64_t MonitorFrames = 0, MonitorDropped = 0, MonitorTimestampFrequency = 0;
    std::uint64_t Received = 0, Forwarded = 0, Changed = 0, TimerOutputs = 0, Faults = 0;
    double ProcessingMicroseconds = 0;
    std::vector<ServiceDevice> Devices;
};
// Only the capture worker executes a script instance. Published UI snapshots
// are detached, and replacing a program never races its current invocation.
class RemapperService final {
  public:
    explicit RemapperService(std::function<std::unique_ptr<MonitorTransport>()> monitorFactory = {});
    ~RemapperService();
    RemapperService(const RemapperService &) = delete;
    RemapperService &operator=(const RemapperService &) = delete;
    void Inject(std::shared_ptr<const ScriptProgram> program);
    void Inject(std::shared_ptr<const ScriptProgram> program, const ParameterValues &values);
    bool TryUpdate(std::shared_ptr<const ScriptProgram> program);
    bool TryUpdate(std::shared_ptr<const ScriptProgram> program, const ParameterValues &values);
    bool SetParameters(const std::shared_ptr<const ScriptProgram> &program,
                       const ParameterValues &values);
    // Read-only kernel telemetry. Neither starts capture nor consumes the demo.
    // Initialization errors are published in MonitorError by the async worker.
    void StartMonitoring();
    void StopMonitoring();
    void Stop();
    bool IsRunning();
    ServiceSnapshot GetSnapshot();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
int RunLiveTest(const std::wstring &reportPath, int seconds);
} // namespace gpr
