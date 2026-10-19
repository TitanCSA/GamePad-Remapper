#pragma once
#include "Script.h"
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gpr {
enum class VisualRuleKind { Remap, Invert, Deadzone, Sensitivity, Block };
enum class VisualConditionKind {
    All,
    Any,
    Not,
    Held,
    Pressed,
    Released,
    Compare,
    AbsoluteCompare,
    HeldFor,
    DoubleTap,
    Expression
};
enum class VisualMacroActivation { OnEnter, WhileTrue, OnExit, Toggle };
enum class VisualMacroKind { Sequence, AxisPattern };
enum class VisualPatternShape { FigureEight, Circle, Sine };
enum class VisualBlend { Add, Replace };
enum class VisualPatternCenter { DynamicPhysical, Activation, Zero };

struct VisualRule {
    VisualRuleKind Kind = VisualRuleKind::Remap;
    std::string Source = "SOUTH", Target = "EAST";
    int Value = 0;
    bool Enabled = true;
};
struct VisualConstant {
    std::string Name;
    int Value = 0;
    // Offsets belong to the preserved UTF-8 source; the visual editor never edits these.
    std::string OriginalName;
    std::ptrdiff_t OriginalStart = -1;
    std::size_t OriginalLength = 0;
};
struct VisualMacroStep {
    std::string Control = "SOUTH";
    int Value = 100, DurationMilliseconds = 100;
    VisualBlend Blend = VisualBlend::Replace;
};
struct VisualCondition {
    VisualConditionKind Kind = VisualConditionKind::Held;
    std::string Control = "SOUTH", Operator = ">";
    int Value = 50, Milliseconds = 250;
    std::string Expression;
    std::vector<VisualCondition> Children;
};
struct VisualAxisPattern {
    std::string TargetX = "LX", TargetY = "LY";
    VisualPatternShape Shape = VisualPatternShape::FigureEight;
    int AmplitudeX = 25, AmplitudeY = 25, PeriodMilliseconds = 1000, TickMilliseconds = 4;
    VisualBlend Blend = VisualBlend::Add;
    VisualPatternCenter Center = VisualPatternCenter::DynamicPhysical;
};
struct VisualMacro {
    std::string Name = "Macro", Trigger = "SOUTH";
    bool Enabled = true;
    std::optional<VisualCondition> Condition;
    VisualMacroActivation Activation = VisualMacroActivation::OnEnter;
    VisualMacroKind Kind = VisualMacroKind::Sequence;
    VisualAxisPattern Pattern;
    std::vector<VisualMacroStep> Steps;
};

class VisualDocument {
  public:
    std::vector<VisualRule> Rules;
    std::vector<VisualConstant> Constants;
    std::vector<VisualMacro> Macros;
    bool HasAdvancedCode = false, ManagedSectionModified = false;

    static VisualDocument Parse(const std::string &source);
    std::string BuildSource() const;
    static VisualMacro CreateFigureEightPreset();
    std::vector<std::string> GetWarnings() const;

  private:
    explicit VisualDocument(std::string source) : original_(std::move(source)) {}
    std::string original_;
};
} // namespace gpr
