#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "../Core/VisualDocument.h"
#include "../ThirdParty/json.hpp"
#include "VisualFixtures.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

namespace gpr::tests {
namespace {
using Json = nlohmann::json;
int checks = 0, groups = 0;
std::string current;
void Check(const bool value, const std::string &detail) {
    ++checks;
    if (!value)
        throw std::runtime_error(current + ": " + detail);
}
void Test(const std::string &name, const std::function<void()> &test) {
    current = name;
    test();
    ++groups;
    std::cout << "PASS " << name << '\n';
}
template <class F> void Reject(F &&action, const std::string &detail) {
    try {
        action();
    } catch (const ScriptException &) {
        Check(true, detail);
        return;
    }
    throw std::runtime_error(current + ": accepted invalid input: " + detail);
}
ControlFrame Frame() {
    ControlFrame frame;
    frame.Available.fill(true);
    for (std::size_t i = 192; i < 208; ++i)
        frame.Physical[i] = -1;
    return frame;
}
std::unique_ptr<ScriptInstance> Engine(const VisualMacro &macro) {
    auto document = VisualDocument::Parse("main{}");
    document.Macros.push_back(macro);
    return ScriptCompiler::Compile(document.BuildSource())->CreateInstance();
}
VisualMacro Sequence(const VisualMacroActivation activation) {
    VisualMacro result;
    result.Name = "Tap";
    result.Activation = activation;
    VisualCondition condition;
    condition.Kind = VisualConditionKind::Held;
    condition.Control = "SOUTH";
    result.Condition = condition;
    VisualMacroStep step;
    step.Control = "NORTH";
    step.Value = 100;
    step.DurationMilliseconds = 20;
    result.Steps.push_back(step);
    return result;
}
void Replace(std::string &source, const std::string &before, const std::string &after) {
    const auto offset = source.find(before);
    if (offset == std::string::npos)
        throw std::runtime_error("Test replacement missing: " + before);
    source.replace(offset, before.size(), after);
}
std::string Encode(const std::string &text) {
    constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t i = 0; i < text.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(text[i]),
                       b = i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0u,
                       c = i + 2 < text.size() ? static_cast<unsigned char>(text[i + 2]) : 0u;
        result += table[a >> 2];
        result += table[((a & 3) << 4) | (b >> 4)];
        result += i + 1 < text.size() ? table[((b & 15) << 2) | (c >> 6)] : '=';
        result += i + 2 < text.size() ? table[c & 63] : '=';
    }
    return result;
}
std::string Hash(const std::string &content) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Test SHA initialization failed");
    std::array<UCHAR, 32> hash{};
    const NTSTATUS status =
        BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(content.data())),
                   static_cast<ULONG>(content.size()), hash.data(), static_cast<ULONG>(hash.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0)
        throw std::runtime_error("Test SHA failed");
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const auto byte : hash) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
std::string RawMetadata(const std::string &json) {
    const std::string content = "// @document " + Encode(json) + "\n";
    return "main{}\n// <igc-visual-v1>\n// @hash " + Hash(content) + "\n" + content + "// </igc-visual-v1>\n";
}
std::string Payload(const Json &macro = nullptr, const Json &rule = nullptr, const Json &constant = nullptr) {
    return RawMetadata(Json{{"Version", 2},
                            {"Rules", rule.is_null() ? Json::array() : Json::array({rule})},
                            {"Macros", macro.is_null() ? Json::array() : Json::array({macro})},
                            {"Constants", constant.is_null() ? Json::array() : Json::array({constant})}}
                           .dump());
}
void Stable(const std::string &source, const int repeats) {
    std::string expected = source;
    for (int i = 0; i < repeats; ++i) {
        const std::string actual = VisualDocument::Parse(expected).BuildSource();
        Check(actual == expected, "roundtrip changed source at pass " + std::to_string(i));
        expected = actual;
    }
}
} // namespace

int RunVisualTests() {
    checks = groups = 0;
    Test("managed v1/v2 source and hash compatibility", [] {
        auto simple = VisualDocument::Parse(std::string(ManagedSimple));
        Check(!simple.ManagedSectionModified && !simple.HasAdvancedCode && simple.Rules.size() == 1 &&
                  simple.Rules[0].Value == 8,
              "managed simple metadata accepted with original SHA");
        Check(simple.BuildSource() == ManagedSimple,
              "unchanged managed simple source remains byte-for-byte identical");
        auto complex = VisualDocument::Parse(std::string(ManagedComplex));
        Check(!complex.ManagedSectionModified && complex.HasAdvancedCode && complex.Macros.size() == 2 &&
                  complex.Constants.size() == 1,
              "all managed complex metadata imported without being discarded");
        Check(complex.Macros[0].Condition && complex.Macros[0].Condition->Children.size() == 3 &&
                  complex.Macros[1].Steps[0].Blend == VisualBlend::Add && complex.Constants[0].Value == 35,
              "conditions, additive steps and constants preserved");
        const std::string rebuilt = complex.BuildSource();
        Check(rebuilt == ManagedComplex,
              "unchanged managed complex source and JSON escapes remain byte-for-byte identical");
        Check(rebuilt.find("// UTF-8 advanced source: é, caméra.") != std::string::npos &&
                  rebuilt.find("function original(v){return v*2;}") != std::string::npos,
              "advanced UTF-8 comments and functions preserved exactly");
        auto engine = ScriptCompiler::Compile(rebuilt)->CreateInstance();
        auto frame = Frame();
        engine->Execute(frame, 0, 0);
        Check(frame.Values[208] == 6, "managed Unicode custom constant still executes");
        Stable(rebuilt, 30);
        auto legacy = VisualDocument::Parse(std::string(LegacyV1));
        Check(!legacy.ManagedSectionModified && legacy.Macros.size() == 1 && !legacy.Macros[0].Condition &&
                  legacy.Macros[0].Name == "Legacy",
              "v1 remains an event-press trigger");
        auto upgraded = VisualDocument::Parse(legacy.BuildSource());
        Check(upgraded.Macros[0].Steps[0].Value == 100 && !upgraded.ManagedSectionModified,
              "v1 upgrades to v2 without semantic loss");
    });
    Test("compound additive figure eight and live center", [] {
        auto document = VisualDocument::Parse("main{}");
        document.Macros.push_back(VisualDocument::CreateFigureEightPreset());
        const std::string source = document.BuildSource();
        auto parsed = VisualDocument::Parse(source);
        Check(parsed.Macros[0].Pattern.Blend == VisualBlend::Add &&
                  parsed.Macros[0].Pattern.Center == VisualPatternCenter::DynamicPhysical,
              "preset uses current output");
        auto engine = ScriptCompiler::Compile(source)->CreateInstance();
        auto frame = Frame();
        frame.Physical[0] = 10;
        frame.Physical[1] = -20;
        engine->Execute(frame, 0, 0);
        Check(engine->NextWakeMilliseconds() == 0 && frame.Values[0] == 10, "false gate has no idle polling");
        frame.Physical[10] = frame.Physical[11] = 100;
        frame.Physical[2] = 50;
        engine->Execute(frame, 1000, 1);
        Check(engine->NextWakeMilliseconds() == 0, "threshold is strictly above 50");
        frame.Physical[2] = 51;
        engine->Execute(frame, 10000, 9);
        Check(frame.Values[0] == 10 && frame.Values[1] == -20 && engine->NextWakeMilliseconds() == 4,
              "phase starts at zero around 10/-20");
        engine->Execute(frame, 135000, 125);
        Check(frame.Values[0] == 28 && frame.Values[1] == 5, "infinity lobe adds to physical center");
        frame.Physical[0] = -40;
        frame.Physical[1] = 30;
        engine->Execute(frame, 260000, 125);
        Check(frame.Values[0] == -15 && frame.Values[1] == 30,
              "moving physical center rebases without drift");
        frame.Physical[10] = 0;
        engine->Execute(frame, 300000, 40);
        Check(frame.Values[0] == -40 && frame.Values[1] == 30 && engine->NextWakeMilliseconds() == 0,
              "release restores both axes immediately");
        frame.Physical[10] = 100;
        engine->Execute(frame, 350000, 50);
        Check(frame.Values[0] == -40 && frame.Values[1] == 30, "reactivation resets phase");
        frame.Available[1] = false;
        engine->Execute(frame, 360000, 10);
        Check(frame.Values[0] == -40 && engine->NextWakeMilliseconds() == 0,
              "missing target axis disables pattern without polling");
        frame.Available[1] = true;
        frame.Physical[0] = frame.Physical[1] = 90;
        engine->Execute(frame, 400000, 40);
        engine->Execute(frame, 525000, 125);
        Check(frame.Values[0] == 100 && frame.Values[1] == 100, "axis limits saturate safely");
        Check(!document.GetWarnings().empty(), "additive limit is visible in warnings");
    });
    Test("pattern shapes centers composition and long-running clock", [] {
        auto document = VisualDocument::Parse("main{sensitivity(LX,50);}");
        document.Macros.push_back(VisualDocument::CreateFigureEightPreset());
        auto engine = ScriptCompiler::Compile(document.BuildSource())->CreateInstance();
        auto frame = Frame();
        frame.Physical[0] = 20;
        frame.Physical[2] = frame.Physical[10] = frame.Physical[11] = 100;
        engine->Execute(frame, 0, 0);
        engine->Execute(frame, 250000, 250);
        Check(frame.Values[0] == 35, "live Add composes after custom transforms");
        auto macro = VisualDocument::CreateFigureEightPreset();
        macro.Pattern.Center = VisualPatternCenter::Activation;
        macro.Pattern.Blend = VisualBlend::Replace;
        engine = Engine(macro);
        frame = Frame();
        frame.Physical[0] = 10;
        frame.Physical[1] = -20;
        frame.Physical[2] = frame.Physical[10] = frame.Physical[11] = 100;
        engine->Execute(frame, 0, 0);
        frame.Physical[0] = frame.Physical[1] = 60;
        engine->Execute(frame, 125000, 125);
        Check(frame.Values[0] == 28 && frame.Values[1] == 5, "activation center remains anchored");
        for (const auto shape :
             {VisualPatternShape::FigureEight, VisualPatternShape::Circle, VisualPatternShape::Sine}) {
            macro = VisualDocument::CreateFigureEightPreset();
            macro.Pattern.Shape = shape;
            macro.Pattern.Center = VisualPatternCenter::Zero;
            macro.Pattern.Blend = VisualBlend::Replace;
            engine = Engine(macro);
            frame = Frame();
            frame.Physical[2] = frame.Physical[10] = frame.Physical[11] = 100;
            engine->Execute(frame, 0, 0);
            engine->Execute(frame, 250000, 250);
            Check(frame.Values[0] == 25 && frame.Values[1] == 0, "all motion shapes reach cardinal point");
        }
        macro = VisualDocument::CreateFigureEightPreset();
        macro.Pattern.PeriodMilliseconds = 3600000;
        engine = Engine(macro);
        frame = Frame();
        frame.Physical[2] = frame.Physical[10] = frame.Physical[11] = 100;
        engine->Execute(frame, 0, 0);
        engine->Execute(frame, 3599999000LL, 1);
        Check(frame.Values[0] >= -1 && frame.Values[0] <= 0,
              "one-hour phase multiplication does not overflow");
        frame.Physical[10] = 0;
        engine->Execute(frame, 2592000000000LL, 1);
        frame.Physical[10] = 100;
        engine->Execute(frame, 2592000001000LL, 1);
        engine->Execute(frame, 2592900001000LL, 1);
        Check(frame.Values[0] == 25 && frame.Values[1] == 0, "phase survives 30-day instance uptime");
    });
    Test("one-cycle motion edge pulses retriggers and lifetime cleanup", [] {
        for (const auto activation : {VisualMacroActivation::OnEnter, VisualMacroActivation::OnExit}) {
            auto macro = VisualDocument::CreateFigureEightPreset();
            macro.Activation = activation;
            VisualCondition condition;
            condition.Kind = activation == VisualMacroActivation::OnEnter ? VisualConditionKind::Pressed
                                                                          : VisualConditionKind::Held;
            macro.Condition = condition;
            auto engine = Engine(macro);
            auto frame = Frame();
            engine->Execute(frame, 0, 0);
            frame.Physical[10] = 100;
            engine->Execute(frame, 1000, 1);
            if (activation == VisualMacroActivation::OnExit)
                Check(engine->NextWakeMilliseconds() == 0, "exit waits for release");
            frame.Physical[10] = 0;
            engine->Execute(frame, 2000, 1);
            const std::int64_t started = activation == VisualMacroActivation::OnEnter ? 1000 : 2000;
            engine->Execute(frame, started + 250000, 249);
            Check(frame.Values[0] == 25 && engine->NextWakeMilliseconds() == 4,
                  "edge pulse plays a full cycle");
            engine->Execute(frame, started + 1000000, 750);
            engine->Execute(frame, started + 1004000, 4);
            Check(frame.Values[0] == 0 && frame.Values[1] == 0 && engine->NextWakeMilliseconds() == 0,
                  "one-cycle lifetime releases final output and timer");
            engine->Execute(frame, started + 1200000, 196);
            Check(engine->NextWakeMilliseconds() == 0, "one-shot never restarts itself");
        }
        auto macro = VisualDocument::CreateFigureEightPreset();
        macro.Activation = VisualMacroActivation::OnEnter;
        VisualCondition pressed;
        pressed.Kind = VisualConditionKind::Pressed;
        macro.Condition = pressed;
        auto engine = Engine(macro);
        auto frame = Frame();
        engine->Execute(frame, 0, 0);
        frame.Physical[10] = 100;
        engine->Execute(frame, 1000, 1);
        frame.Physical[10] = 0;
        engine->Execute(frame, 2000, 1);
        frame.Physical[10] = 100;
        engine->Execute(frame, 101000, 99);
        Check(frame.Values[0] == 0 && frame.Values[1] == 0, "retrigger restarts phase");
        engine->Execute(frame, 1051000, 950);
        Check(engine->NextWakeMilliseconds() == 4, "retrigger replaces old deadline");
        engine->Execute(frame, 1101000, 50);
        engine->Execute(frame, 1105000, 4);
        Check(engine->NextWakeMilliseconds() == 0, "replacement lifetime expires");
        frame.Physical[10] = 0;
        engine->Execute(frame, 1200000, 95);
        frame.Physical[10] = 100;
        engine->Execute(frame, 1300000, 100);
        engine->Execute(frame, 2592000000000LL, 1);
        engine->Execute(frame, 2592000004000LL, 4);
        Check(engine->NextWakeMilliseconds() == 0,
              "64-bit lifetime expires after a 30-day delayed evaluation");
    });
    Test("all sequence activations additive steps and condition leaves", [] {
        for (const auto activation : {VisualMacroActivation::OnEnter, VisualMacroActivation::WhileTrue,
                                      VisualMacroActivation::OnExit, VisualMacroActivation::Toggle}) {
            auto engine = Engine(Sequence(activation));
            auto frame = Frame();
            engine->Execute(frame, 0, 0);
            frame.Physical[10] = 100;
            engine->Execute(frame, 1000, 1);
            Check(frame.Values[13] == (activation == VisualMacroActivation::OnExit ? 0 : 100),
                  "sequence activation mode");
            engine->Execute(frame, 21000, 20);
            Check(frame.Values[13] == (activation == VisualMacroActivation::WhileTrue ||
                                               activation == VisualMacroActivation::Toggle
                                           ? 100
                                           : 0),
                  "repetition mode");
            frame.Physical[10] = 0;
            engine->Execute(frame, 22000, 1);
            Check(frame.Values[13] == (activation == VisualMacroActivation::OnExit ||
                                               activation == VisualMacroActivation::Toggle
                                           ? 100
                                           : 0),
                  "sequence release semantics");
            if (activation == VisualMacroActivation::Toggle) {
                frame.Physical[10] = 100;
                engine->Execute(frame, 23000, 1);
                Check(frame.Values[13] == 0 && engine->NextWakeMilliseconds() == 0,
                      "second toggle releases current hold immediately");
            }
        }
        auto macro = Sequence(VisualMacroActivation::OnEnter);
        macro.Steps[0].Control = "LX";
        macro.Steps[0].Value = 20;
        macro.Steps[0].Blend = VisualBlend::Add;
        auto engine = Engine(macro);
        auto frame = Frame();
        frame.Physical[10] = 100;
        frame.Physical[0] = 10;
        engine->Execute(frame, 0, 0);
        Check(frame.Values[0] == 30, "additive step begins relative to existing value");
        frame.Physical[0] = -40;
        engine->Execute(frame, 10000, 10);
        Check(frame.Values[0] == -20, "additive step hold follows new physical base");
        for (int kind = static_cast<int>(VisualConditionKind::Held);
             kind <= static_cast<int>(VisualConditionKind::Expression); ++kind) {
            macro = Sequence(VisualMacroActivation::OnEnter);
            macro.Condition->Kind = static_cast<VisualConditionKind>(kind);
            macro.Condition->Expression = "is_available(RX) && abs(get_ival(RX))>50";
            auto doc = VisualDocument::Parse("main{}");
            doc.Macros.push_back(macro);
            auto parsed = VisualDocument::Parse(doc.BuildSource());
            Check(parsed.Macros[0].Condition->Kind == macro.Condition->Kind &&
                      parsed.Macros[0].Condition->Expression == macro.Condition->Expression,
                  "all leaf kinds roundtrip");
        }
        macro = Sequence(VisualMacroActivation::OnEnter);
        macro.Condition->Kind = VisualConditionKind::DoubleTap;
        macro.Condition->Milliseconds = 250;
        engine = Engine(macro);
        frame = Frame();
        frame.Physical[10] = 100;
        engine->Execute(frame, 0, 0);
        Check(frame.Values[13] == 0 && engine->NextWakeMilliseconds() == 0,
              "double tap does not fire at startup");
        frame.Physical[10] = 0;
        engine->Execute(frame, 50000, 50);
        frame.Physical[10] = 100;
        engine->Execute(frame, 100000, 50);
        Check(frame.Values[13] == 100, "second real tap fires");
        macro.Condition->Kind = VisualConditionKind::HeldFor;
        macro.Condition->Milliseconds = 40;
        engine = Engine(macro);
        frame = Frame();
        engine->Execute(frame, 0, 0);
        Check(engine->NextWakeMilliseconds() == 0, "held threshold does not poll while released");
        frame.Physical[10] = 100;
        engine->Execute(frame, 1000, 1);
        engine->Execute(frame, 41000, 40);
        Check(frame.Values[13] == 100, "held threshold works without another input packet");
        VisualCondition any;
        any.Kind = VisualConditionKind::Any;
        VisualCondition compare;
        compare.Kind = VisualConditionKind::AbsoluteCompare;
        compare.Control = "RX";
        compare.Value = 90;
        any.Children.push_back(compare);
        VisualCondition negation;
        negation.Kind = VisualConditionKind::Not;
        VisualCondition west;
        west.Control = "WEST";
        negation.Children.push_back(west);
        any.Children.push_back(negation);
        macro = Sequence(VisualMacroActivation::OnEnter);
        macro.Condition = any;
        engine = Engine(macro);
        frame = Frame();
        engine->Execute(frame, 0, 0);
        Check(frame.Values[13] == 100, "nested ANY and NOT combine correctly");
        macro.Condition = VisualCondition{};
        macro.Condition->Kind = VisualConditionKind::Expression;
        macro.Condition->Expression = "get_ival(SOUTH) && (get_ival(RX)>50 || get_ival(RY)<-50)";
        engine = Engine(macro);
        frame = Frame();
        frame.Physical[10] = 100;
        frame.Physical[3] = -60;
        engine->Execute(frame, 0, 0);
        Check(frame.Values[13] == 100, "arbitrary nested expression condition");
    });
    Test("advanced source constants edits and generated symbol ownership", [] {
        const std::string longName = "A" + std::string(99, 'b');
        const std::string custom = "// Keep this exact comment.\ndefine Aé=1;\ndefine " + longName +
                                   "=2;\ndefine __igc_visual_state_0_active=3;\nmain{set_val(VALUE_1,Aé+" +
                                   longName +
                                   "+__igc_visual_state_0_active);}\nfunction original(v){return v*2;}";
        auto document = VisualDocument::Parse(custom);
        Check(document.Constants.empty() && document.HasAdvancedCode,
              "nonvisual identifier names remain advanced source");
        document.Macros.push_back(VisualDocument::CreateFigureEightPreset());
        const std::string source = document.BuildSource();
        Check(source.find("define Aé=1;") != std::string::npos &&
                  source.find("function original(v){return v*2;}") != std::string::npos,
              "UTF-8 source preserved");
        auto engine = ScriptCompiler::Compile(source)->CreateInstance();
        auto frame = Frame();
        engine->Execute(frame, 0, 0);
        Check(frame.Values[208] == 6, "all custom identifiers execute after visual composition");
        document =
            VisualDocument::Parse("define FIRST=1;\ndefine SECOND=20;\nmain{set_val(VALUE_1,FIRST+SECOND);}");
        Check(document.Constants.size() == 2, "multiple source constants discovered");
        document.Constants[0].Value = 30000;
        document.Constants[1].Value = -7;
        engine = ScriptCompiler::Compile(document.BuildSource())->CreateInstance();
        engine->Execute(frame, 0, 0);
        Check(frame.Values[208] == 29993,
              "multiple original byte offsets replace backwards without corrupting source");
        document.Constants[0].Name = "RENAMED";
        Reject([&] { document.BuildSource(); }, "custom constant rename is refused losslessly");
        document = VisualDocument::Parse(
            "int __igc_macro_0;int __igc_visual_state_0_active;main{}function __igc_visual_apply(){}");
        document.Macros.push_back(Sequence(VisualMacroActivation::OnEnter));
        Check(document.BuildSource().find("combo __igc_macro_1_0") != std::string::npos,
              "macro helper avoids advanced symbol collision");
        document = VisualDocument::Parse("main{}");
        VisualRule rule;
        rule.Kind = VisualRuleKind::Invert;
        rule.Source = "LY";
        document.Rules.push_back(rule);
        std::string edited = document.BuildSource();
        Replace(edited, "invert(LY);", "invert(LX);");
        auto changed = VisualDocument::Parse(edited);
        Check(changed.ManagedSectionModified && changed.HasAdvancedCode && changed.Rules.empty(),
              "hand-edited managed code becomes advanced");
        Check(changed.BuildSource().find("invert(LX);") != std::string::npos &&
                  !changed.GetWarnings().empty(),
              "hand edits are never replaced by stale metadata");
    });
    Test("canonical metadata and repeated source idempotence", [] {
        for (const std::string &input : {std::string("main{}"), std::string(), std::string("// Comment only"),
                                         std::string("main {\r\n    // Custom closing indentation.\r\n    }"),
                                         std::string("define N=7;\nmain{set_val(VALUE_1,N);\n\t}\n")}) {
            auto document = VisualDocument::Parse(input);
            VisualRule rule;
            rule.Kind = VisualRuleKind::Deadzone;
            rule.Source = "LX";
            rule.Value = 8;
            document.Rules.push_back(rule);
            Stable(document.BuildSource(), 100);
        }
        auto document = VisualDocument::Parse("main{}");
        VisualRule rule;
        rule.Kind = VisualRuleKind::Deadzone;
        rule.Source = "LX";
        rule.Value = 8;
        document.Rules.push_back(rule);
        const std::string source = document.BuildSource();
        const std::string canonical =
            R"json({"Constants":[],"Macros":[],"Rules":[{"Enabled":true,"Kind":2,"Source":"LX","Target":"EAST","Value":8}],"Version":2})json";
        Check(source.find("// @document " + Encode(canonical)) != std::string::npos,
              "metadata object keys have ordinal order");
        Stable(source, 1000);
        for (
            const std::string &json :
            {std::string(
                 R"json({"Version":2,"Rules":[{"Kind":2,"Source":"LX","Target":"EAST","Value":8,"Enabled":true}],"Macros":[],"Constants":[]})json"),
             std::string(
                 R"json({"Macros":[],"Constants":[],"Version":2,"Rules":[{"Value":8,"Target":"EAST","Enabled":true,"Source":"LX","Kind":2}]})json")})
            Check(VisualDocument::Parse(RawMetadata(json)).BuildSource() == source,
                  "different metadata key orders produce exactly the same source and hash");
        document = VisualDocument::Parse("main{}");
        VisualRule second;
        second.Kind = VisualRuleKind::Invert;
        second.Source = "LY";
        document.Rules.push_back(second);
        document.Rules.push_back(rule);
        const auto parsed = VisualDocument::Parse(document.BuildSource());
        Check(parsed.Rules[0].Kind == VisualRuleKind::Invert &&
                  parsed.Rules[1].Kind == VisualRuleKind::Deadzone,
              "array action order is never sorted");
    });
    Test("strict metadata schema scalar types and malformed imports", [] {
        for (const std::string &value :
             {std::string("2147483648"), std::string("-2147483649"), std::string("\"NaN\""),
              std::string("{\"foo\":1}"), std::string("null"), std::string("[]"), std::string("true"),
              std::string("1.5"), std::string("\"12\""), std::string("1e999")})
            Reject(
                [&] {
                    VisualDocument::Parse(RawMetadata("{\"Version\":2,\"Rules\":[],\"Macros\":[],"
                                                      "\"Constants\":[{\"Name\":\"CUSTOM\",\"Value\":" +
                                                      value + "}]}"));
                },
                "metadata has no scalar coercion or escaped numeric conversion exception");
        for (
            const std::string &value :
            {std::string(R"json({"Version":"2","Rules":[],"Macros":[],"Constants":[]})json"),
             std::string(R"json({"Version":2,"Rules":{},"Macros":[],"Constants":[]})json"),
             std::string(R"json({"Version":2,"Rules":[],"Macros":[],"Constants":[],"Unknown":1})json"),
             std::string(R"json({"Version":2,"Rules":[{"Enabled":"false"}],"Macros":[],"Constants":[]})json"),
             std::string(R"json({"Version":2,"Rules":[{"Kind":"Remap"}],"Macros":[],"Constants":[]})json"),
             std::string(R"json({"Version":2,"Version":2,"Rules":[],"Macros":[],"Constants":[]})json"),
             std::string(
                 R"json({"Version":2,"Rules":[],"Macros":[],"Constants":[{"__type":"System.IO.FileInfo","Name":"CUSTOM","Value":1}]})json")})
            Reject([&] { VisualDocument::Parse(RawMetadata(value)); },
                   "unknown fields duplicate keys and incorrect JSON types fail safely");
        for (const int bound : {(std::numeric_limits<int>::min)(), (std::numeric_limits<int>::max)()}) {
            const auto parsed =
                VisualDocument::Parse(Payload(nullptr, nullptr, Json{{"Name", "CUSTOM"}, {"Value", bound}}));
            Check(parsed.Constants[0].Value == bound, "signed integer metadata boundary is lossless");
        }
        Reject([] { VisualDocument::Parse(Payload(Json{{"Kind", 1}, {"Pattern", nullptr}})); },
               "missing motion settings rejected before UI");
        const auto sequence = VisualDocument::Parse(Payload(Json{{"Pattern", nullptr}}));
        Check(sequence.Macros[0].Pattern.TargetX == "LX", "dormant null sequence pattern defaults safely");
        for (const Json &value :
             {Json{{"Kind", 999}}, Json{{"Activation", 999}}, Json{{"Pattern", Json{{"Shape", 999}}}},
              Json{{"Pattern", Json{{"TargetX", "SOUTH"}}}},
              Json{{"Pattern", Json{{"PeriodMilliseconds", 0}}}},
              Json{{"Condition", Json{{"Kind", 6}, {"Operator", "call()"}}}},
              Json{{"Condition", Json{{"Children", nullptr}}}}, Json{{"Trigger", "unknown"}},
              Json{{"Name", "bad name"}}, Json{{"Steps", nullptr}}, Json{{"Steps", Json::array({nullptr})}},
              Json{{"Steps", Json::array({Json{{"DurationMilliseconds", 0}}})}}})
            Reject([&] { VisualDocument::Parse(Payload(value)); },
                   "invalid macro fields rejected before rendering");
        Reject([] { VisualDocument::Parse(Payload(nullptr, Json{{"Kind", 999}})); },
               "invalid rule enumeration");
        Reject([] { VisualDocument::Parse(Payload(nullptr, Json{{"Kind", 2}, {"Value", 100}})); },
               "invalid rule range");
        Reject([] { VisualDocument::Parse(Payload(nullptr, nullptr, Json{{"Name", "invalid name"}})); },
               "invalid constant name");
        std::string bad = std::string(ManagedSimple);
        Replace(bad, "// <igc-visual-v1>", "// <igc-visual-v1>\n// <igc-visual-v1>");
        Reject([&] { VisualDocument::Parse(bad); }, "duplicate section markers");
        Reject([] { VisualDocument::Parse("// <igc-visual-v1>"); }, "incomplete section markers");
        Reject([] { VisualDocument::Parse(std::string(524289, ' ')); }, "source-size limit");
        Reject([] { VisualDocument::Parse(std::string("main{}") + static_cast<char>(0xFF)); },
               "invalid UTF-8 source");
    });
    Test("visual limits expression safety and disabled row preservation", [] {
        auto document = VisualDocument::Parse("main{}");
        auto macro = VisualDocument::CreateFigureEightPreset();
        document.Macros.push_back(macro);
        for (int i = 0; i < 14; ++i) {
            VisualCondition parent;
            parent.Kind = VisualConditionKind::Not;
            parent.Children.push_back(*document.Macros[0].Condition);
            document.Macros[0].Condition = std::move(parent);
        }
        Reject([&] { document.BuildSource(); }, "deep condition graph rejected before recursion overflow");
        for (const std::string &expression :
             {std::string("1);set_val(LX,100);if(1"), std::string("1){}main{"), std::string("/*comment"),
              std::string("(1"), std::string("[1"), std::string("1//comment")}) {
            document = VisualDocument::Parse("main{}");
            macro = Sequence(VisualMacroActivation::OnEnter);
            macro.Condition->Kind = VisualConditionKind::Expression;
            macro.Condition->Expression = expression;
            document.Macros.push_back(macro);
            Reject([&] { document.BuildSource(); }, "condition cannot inject statements or comments");
        }
        for (const int period : {0, -1, 3600001, (std::numeric_limits<int>::max)()}) {
            document = VisualDocument::Parse("main{}");
            macro = VisualDocument::CreateFigureEightPreset();
            macro.Pattern.PeriodMilliseconds = period;
            document.Macros.push_back(macro);
            Reject([&] { document.BuildSource(); }, "unsafe motion period rejected");
        }
        document = VisualDocument::Parse("main{}");
        macro = VisualDocument::CreateFigureEightPreset();
        macro.Pattern.TargetY = "LX";
        document.Macros.push_back(macro);
        Reject([&] { document.BuildSource(); }, "two distinct signed axes required");
        document.Macros[0].Pattern.TargetY = "LY";
        document.Macros[0].Pattern.Center = VisualPatternCenter::Activation;
        Reject([&] { document.BuildSource(); }, "ambiguous live-add and fixed-center rejected");
        document = VisualDocument::Parse("main{}");
        VisualMacro empty;
        empty.Enabled = false;
        document.Macros.push_back(empty);
        auto parsed = VisualDocument::Parse(document.BuildSource());
        Check(!parsed.Macros[0].Enabled && parsed.Macros[0].Steps.empty(),
              "disabled unfinished sequence stays editable");
        parsed.Macros[0].Enabled = true;
        Reject([&] { parsed.BuildSource(); }, "enabled sequence needs a timed step");
        document = VisualDocument::Parse("main{}");
        for (int i = 0; i < 257; ++i)
            document.Rules.push_back(VisualRule{});
        Reject([&] { document.BuildSource(); }, "rule limit");
        Json many = Json::array();
        for (int i = 0; i < 257; ++i)
            many.push_back(Json::object());
        Reject(
            [&] {
                VisualDocument::Parse(RawMetadata(Json{
                    {"Version", 2}, {"Rules", many}, {"Macros", Json::array()}, {"Constants", Json::array()}}
                                                      .dump()));
            },
            "import item limits checked before UI allocation");
    });
    Test("continuous complex motion stress has no progressive drift", [] {
        auto engine = Engine(VisualDocument::CreateFigureEightPreset());
        auto frame = Frame();
        frame.Physical[2] = frame.Physical[10] = frame.Physical[11] = 100;
        for (int i = 0; i < 2000; ++i) {
            frame.Physical[0] = i % 101 - 50;
            frame.Physical[1] = 50 - i % 101;
            engine->Execute(frame, static_cast<std::int64_t>(i) * 4000, 4);
            const int angle = (i * 4 % 1000) * 360 / 1000;
            const double dx = std::sin(angle * (3.14159265358979323846 / 180)) * 25,
                         dy = std::sin((angle * 2 % 360) * (3.14159265358979323846 / 180)) * 25;
            Check(std::abs(frame.Values[0] - frame.Physical[0] - dx) <= 0.500001 &&
                      std::abs(frame.Values[1] - frame.Physical[1] - dy) <= 0.500001 &&
                      engine->NextWakeMilliseconds() == 4,
                  "live base and clamped waveform remain deterministic");
        }
    });
    return checks;
}
} // namespace gpr::tests

#ifndef GPR_TEST_NO_MAIN
int main(const int argc, char **argv) {
    try {
        const int checks = gpr::tests::RunVisualTests();
        const std::string report = "{\"passed\":true,\"checks\":" + std::to_string(checks) + "}";
        for (int i = 1; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--json") {
                std::ofstream output(argv[++i], std::ios::binary);
                if (!output || !(output << report))
                    throw std::runtime_error("Cannot write visual test report");
            }
        std::cout << report << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#endif
