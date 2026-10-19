#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace gpr {
struct ControlFrame {
    std::array<int, 256> Physical{}, Values{};
    std::array<bool, 256> Available{};
    std::uint32_t DeviceKey = 0, Family = 0;
    void CopyPhysicalToOutput() noexcept { Values = Physical; }
};
struct Controls {
    static const std::array<std::string, 256> &Names();
    static int Parse(const std::string &name);
};
class ScriptException : public std::runtime_error {
  public:
    const int Line, Column;
    explicit ScriptException(const std::string &message, int line = 1, int column = 1);
    std::string Diagnostic() const;
};
namespace detail {
struct ProgramData;
}
class ScriptInstance;
class ScriptProgram final {
    std::shared_ptr<const detail::ProgramData> data_;
    explicit ScriptProgram(std::shared_ptr<const detail::ProgramData> data);
    friend struct ScriptCompiler;

  public:
    const std::string &Source() const noexcept;
    std::unique_ptr<ScriptInstance> CreateInstance() const;
};
struct ScriptCompiler {
    static std::shared_ptr<const ScriptProgram> Compile(const std::string &source);
};
class ScriptInstance final {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit ScriptInstance(std::shared_ptr<const detail::ProgramData> program);
    friend class ScriptProgram;

  public:
    ~ScriptInstance();
    ScriptInstance(const ScriptInstance &) = delete;
    ScriptInstance &operator=(const ScriptInstance &) = delete;
    void Execute(ControlFrame &frame, std::int64_t nowMicroseconds, int elapsedMilliseconds);
    void Reset();
    int NextWakeMilliseconds() const noexcept;
    int InstructionBudget() const noexcept;
    void SetInstructionBudget(int value);
};
} // namespace gpr
