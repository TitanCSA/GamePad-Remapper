#pragma once
#include "../Core/Script.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gpr {
struct ServiceDevice {
    std::uint32_t Key = 0, Family = 0;
    std::string Name, Token, Diagnostic;
    std::array<int, 256> Values{}, OutputValues{};
    std::array<bool, 256> Available{};
};
struct ServiceSnapshot {
    std::string Status = "Ready", Error;
    bool Running = false;
    std::uint64_t Received = 0, Forwarded = 0, Changed = 0, TimerOutputs = 0, Faults = 0;
    double ProcessingMicroseconds = 0;
    std::vector<ServiceDevice> Devices;
};
// Only the capture worker executes a script instance. Published UI snapshots
// are detached, and replacing a program never races its current invocation.
class RemapperService final {
  public:
    RemapperService();
    ~RemapperService();
    RemapperService(const RemapperService &) = delete;
    RemapperService &operator=(const RemapperService &) = delete;
    void Inject(std::shared_ptr<const ScriptProgram> program);
    void Inject(std::shared_ptr<const ScriptProgram> program, const ParameterValues &values);
    bool TryUpdate(std::shared_ptr<const ScriptProgram> program);
    bool TryUpdate(std::shared_ptr<const ScriptProgram> program, const ParameterValues &values);
    bool SetParameters(const std::shared_ptr<const ScriptProgram> &program,
                       const ParameterValues &values);
    void Stop();
    bool IsRunning();
    ServiceSnapshot GetSnapshot();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
int RunLiveTest(const std::wstring &reportPath, int seconds);
} // namespace gpr
