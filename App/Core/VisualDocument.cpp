#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "VisualDocument.h"
#include "../ThirdParty/json.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace gpr {
namespace {
using Json = nlohmann::json;
constexpr std::string_view Begin = "// <igc-visual-v1>", End = "// </igc-visual-v1>";
constexpr std::string_view CallPrefix = "/* @igc-visual-call */ ";
constexpr const char *NL = "\r\n";
constexpr std::size_t MaxSource = 524288, MaxJson = 262144;
const std::regex IntegerDefines(
    R"rx(\bdefine\s+((?!__igc_)[A-Za-z_][A-Za-z_0-9]{0,79})\s*=\s*(-?(?:0[xX][0-9a-fA-F]+|[0-9]+))\s*;)rx");

[[noreturn]] void Fail(const std::string &message) { throw ScriptException(message); }
bool Starts(const std::string &text, const std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}
bool Space(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }
std::string Trim(const std::string &text) {
    std::size_t a = 0, b = text.size();
    while (a < b && Space(text[a]))
        ++a;
    while (b > a && Space(text[b - 1]))
        --b;
    return text.substr(a, b - a);
}
void TrimEnd(std::string &text) {
    while (!text.empty() && Space(text.back()))
        text.pop_back();
}
void ReplaceAll(std::string &text, const std::string_view before, const std::string_view after) {
    std::size_t offset = 0;
    while ((offset = text.find(before, offset)) != std::string::npos) {
        text.replace(offset, before.size(), after);
        offset += after.size();
    }
}
void CheckUtf8(const std::string &text) {
    if (text.size() > MaxSource)
        Fail("A script may contain at most 512 KiB of UTF-8 text.");
    if (!text.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0) == 0)
        Fail("The script contains invalid UTF-8 text.");
}
std::string MaskComments(const std::string &source) {
    std::string result = source;
    bool line = false, block = false;
    for (std::size_t i = 0; i < source.size(); ++i) {
        const char c = source[i], next = i + 1 < source.size() ? source[i + 1] : '\0';
        if (!line && !block && c == '/' && next == '/') {
            line = true;
            result[i] = ' ';
        } else if (!line && !block && c == '/' && next == '*') {
            block = true;
            result[i] = ' ';
        } else if (line) {
            if (c == '\r' || c == '\n')
                line = false;
            else
                result[i] = ' ';
        } else if (block) {
            if (c == '*' && next == '/') {
                result[i] = result[i + 1] = ' ';
                ++i;
                block = false;
            } else if (c != '\r' && c != '\n')
                result[i] = ' ';
        }
    }
    return result;
}
std::string Hash(std::string text) {
    ReplaceAll(text, "\r\n", "\n");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        Fail("Could not initialize the script integrity hash.");
    std::array<UCHAR, 32> digest{};
    const NTSTATUS status =
        BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(text.data()),
                   static_cast<ULONG>(text.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0)
        Fail("Could not calculate the script integrity hash.");
    constexpr char Hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(64);
    for (const UCHAR byte : digest) {
        result.push_back(Hex[byte >> 4]);
        result.push_back(Hex[byte & 15]);
    }
    return result;
}
std::string Base64(const std::string &input) {
    constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((input.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const auto a = static_cast<unsigned char>(input[i]);
        const unsigned b = i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0u,
                       c = i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0u;
        output.push_back(Alphabet[a >> 2]);
        output.push_back(Alphabet[((a & 3u) << 4) | (b >> 4)]);
        output.push_back(i + 1 < input.size() ? Alphabet[((b & 15u) << 2) | (c >> 6)] : '=');
        output.push_back(i + 2 < input.size() ? Alphabet[c & 63u] : '=');
    }
    return output;
}
std::string Unbase64(const std::string &input) {
    if (input.size() > 349528 || input.size() % 4 != 0)
        Fail("Visual metadata is too large or has invalid base64 encoding.");
    auto digit = [](const char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    std::string output;
    output.reserve(input.size() / 4 * 3);
    for (std::size_t i = 0; i < input.size(); i += 4) {
        const int a = digit(input[i]), b = digit(input[i + 1]),
                  c = input[i + 2] == '=' ? 0 : digit(input[i + 2]),
                  d = input[i + 3] == '=' ? 0 : digit(input[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (input[i + 2] == '=' && input[i + 3] != '=') ||
            ((input[i + 2] == '=' || input[i + 3] == '=') && i + 4 != input.size()))
            Fail("Malformed visual metadata base64 encoding.");
        output.push_back(static_cast<char>((a << 2) | (b >> 4)));
        if (input[i + 2] != '=')
            output.push_back(static_cast<char>(((b & 15) << 4) | (c >> 2)));
        if (input[i + 3] != '=')
            output.push_back(static_cast<char>(((c & 3) << 6) | d));
    }
    if (output.size() > MaxJson)
        Fail("Visual metadata is too large.");
    return output;
}
bool TryInteger(std::string text, int &value) {
    const bool negative = !text.empty() && text[0] == '-';
    if (negative)
        text.erase(0, 1);
    const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    if (hex)
        text.erase(0, 2);
    std::uint32_t number = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number, hex ? 16 : 10);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return false;
    if (negative)
        number = 0u - number;
    std::int32_t signedValue = 0;
    std::memcpy(&signedValue, &number, sizeof(number));
    value = signedValue;
    return true;
}
int SignedInteger(const std::string &text) {
    const std::size_t start = !text.empty() && text[0] == '+' ? 1u : 0u;
    int result = 0;
    const auto parsed = std::from_chars(text.data() + start, text.data() + text.size(), result);
    if (start == text.size() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        Fail("Malformed visual integer metadata.");
    return result;
}
std::string Control(const std::string &name) {
    const int id = Controls::Parse(name);
    if (id < 0 || (id >= 22 && id < 32))
        Fail("Unknown control '" + name + "'.");
    return Controls::Names()[static_cast<std::size_t>(id)];
}
std::string Name(const std::string &name) {
    auto letter = [](const char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    if (name.empty() || name.size() > 80 || !letter(name[0]) || Starts(name, "__igc_"))
        Fail("Use a valid name (letters, digits and underscores; no __igc_ prefix).");
    for (const char c : name)
        if (!letter(c) && (c < '0' || c > '9'))
            Fail("Use a valid name (letters, digits and underscores; no __igc_ prefix).");
    return name;
}
template <class T> bool EnumValid(const T value, const T last) {
    return static_cast<int>(value) >= 0 && static_cast<int>(value) <= static_cast<int>(last);
}
void CheckFields(const Json &value, std::initializer_list<const char *> allowed) {
    if (!value.is_object())
        Fail("A visual metadata object has the wrong type.");
    for (auto field = value.begin(); field != value.end(); ++field)
        if (std::none_of(allowed.begin(), allowed.end(),
                         [&](const char *name) { return field.key() == name; }))
            Fail("Unknown visual metadata field '" + field.key().substr(0, 80) + "'.");
}
int ReadInt(const Json &value, const char *key, const int fallback) {
    const auto field = value.find(key);
    if (field == value.end())
        return fallback;
    if (!field->is_number_integer())
        Fail("A visual metadata value has the wrong type.");
    if (field->is_number_unsigned()) {
        const auto number = field->get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>((std::numeric_limits<int>::max)()))
            Fail("A visual metadata integer is out of range.");
        return static_cast<int>(number);
    }
    const auto number = field->get<std::int64_t>();
    if (number < (std::numeric_limits<int>::min)() || number > (std::numeric_limits<int>::max)())
        Fail("A visual metadata integer is out of range.");
    return static_cast<int>(number);
}
bool ReadBool(const Json &value, const char *key, const bool fallback) {
    const auto field = value.find(key);
    if (field == value.end())
        return fallback;
    if (!field->is_boolean())
        Fail("A visual metadata value has the wrong type.");
    return field->get<bool>();
}
std::string ReadString(const Json &value, const char *key, const std::string &fallback,
                       const bool nullable = false) {
    const auto field = value.find(key);
    if (field == value.end())
        return fallback;
    if (nullable && field->is_null())
        return fallback;
    if (!field->is_string())
        Fail("A visual metadata value has the wrong type.");
    return field->get<std::string>();
}
template <class T> T ReadEnum(const Json &value, const char *key, const T fallback, const T last) {
    const auto result = static_cast<T>(ReadInt(value, key, static_cast<int>(fallback)));
    if (!EnumValid(result, last))
        Fail("Invalid visual metadata enumeration.");
    return result;
}
void CheckArray(const Json &value, const std::size_t maximum) {
    if (!value.is_array() || value.size() > maximum)
        Fail("A visual metadata list has the wrong type or exceeds its item limit.");
}
Json ParseJson(const std::string &text) {
    if (text.size() > MaxJson)
        Fail("Visual metadata is too large.");
    std::vector<std::set<std::string>> keys;
    auto callback = [&](const int depth, const Json::parse_event_t event, Json &parsed) {
        if (depth > 48)
            Fail("Visual metadata exceeds its nesting limit.");
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        else if (event == Json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
            Fail("Duplicate visual metadata field.");
        else if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        return true;
    };
    try {
        return Json::parse(text, callback);
    } catch (const Json::exception &) {
        Fail("Malformed visual document metadata.");
    }
}
void ValidateExpression(const std::string &expression) {
    if (Trim(expression).empty() || expression.size() > 4096)
        Fail("A custom condition needs an expression of at most 4,096 characters.");
    if (expression.find_first_of(";{}") != std::string::npos || expression.find("//") != std::string::npos ||
        expression.find("/*") != std::string::npos)
        Fail("A custom condition must be one expression, without statements or comments.");
    int parentheses = 0, brackets = 0;
    for (const char c : expression) {
        if (c == '(')
            ++parentheses;
        else if (c == ')' && --parentheses < 0)
            Fail("Unbalanced condition parentheses.");
        if (c == '[')
            ++brackets;
        else if (c == ']' && --brackets < 0)
            Fail("Unbalanced condition brackets.");
    }
    if (parentheses != 0 || brackets != 0)
        Fail("Unbalanced condition expression.");
}
void ValidateCondition(const VisualCondition &condition, const int depth, int &nodes) {
    if (depth > 12 || ++nodes > 128)
        Fail("Conditions exceed 12 nested levels or 128 nodes.");
    if (!EnumValid(condition.Kind, VisualConditionKind::Expression))
        Fail("Invalid visual condition.");
    if (condition.Kind == VisualConditionKind::All || condition.Kind == VisualConditionKind::Any ||
        condition.Kind == VisualConditionKind::Not) {
        const std::size_t maximum = condition.Kind == VisualConditionKind::Not ? 1u : 16u;
        if (condition.Children.empty() || condition.Children.size() > maximum)
            Fail("ALL/ANY need 1-16 children; NOT needs exactly one child.");
        for (const auto &child : condition.Children)
            ValidateCondition(child, depth + 1, nodes);
    } else {
        if (!condition.Children.empty())
            Fail("Only ALL, ANY and NOT conditions may contain children.");
        if (condition.Kind == VisualConditionKind::Expression)
            ValidateExpression(condition.Expression);
        else {
            Control(condition.Control);
            if ((condition.Kind == VisualConditionKind::Compare ||
                 condition.Kind == VisualConditionKind::AbsoluteCompare) &&
                condition.Operator != ">" && condition.Operator != ">=" && condition.Operator != "<" &&
                condition.Operator != "<=" && condition.Operator != "==" && condition.Operator != "!=")
                Fail("Choose >, >=, <, <=, == or != for an axis comparison.");
            if ((condition.Kind == VisualConditionKind::HeldFor ||
                 condition.Kind == VisualConditionKind::DoubleTap) &&
                (condition.Milliseconds < 1 || condition.Milliseconds > 60000))
                Fail("Held/double-tap time must be between 1 and 60,000 ms.");
        }
    }
}
void ValidatePattern(const VisualAxisPattern &pattern) {
    if (!EnumValid(pattern.Shape, VisualPatternShape::Sine) ||
        !EnumValid(pattern.Blend, VisualBlend::Replace) ||
        !EnumValid(pattern.Center, VisualPatternCenter::Zero))
        Fail("Invalid axis pattern.");
    const std::string x = Control(pattern.TargetX), y = Control(pattern.TargetY);
    const int ix = Controls::Parse(x), iy = Controls::Parse(y);
    if (!(ix <= 3 || (ix >= 96 && ix < 160)) || !(iy <= 3 || (iy >= 96 && iy < 160)) || x == y)
        Fail("A motion pattern needs two different signed axes, such as LX and LY.");
    if (pattern.AmplitudeX < 0 || pattern.AmplitudeX > 100 || pattern.AmplitudeY < 0 ||
        pattern.AmplitudeY > 100)
        Fail("Pattern amplitudes must be between 0 and 100 percent.");
    if (pattern.PeriodMilliseconds < 1 || pattern.PeriodMilliseconds > 3600000)
        Fail("Pattern period must be between 1 and 3,600,000 ms.");
    if (pattern.TickMilliseconds < 1 || pattern.TickMilliseconds > 20)
        Fail("Pattern update interval must be between 1 and 20 ms.");
    if (pattern.Blend == VisualBlend::Add && pattern.Center != VisualPatternCenter::DynamicPhysical)
        Fail("Add blending follows the current output; select the live center, or use Replace for a "
             "fixed/zero center.");
}
void ValidateRows(const VisualDocument &document) {
    if (document.Rules.size() > 256 || document.Constants.size() > 256 || document.Macros.size() > 32)
        Fail("Visual document exceeds 256 rules/constants or 32 macros.");
    std::set<std::string> names;
    for (const auto &item : document.Constants)
        if (!names.insert(Name(item.Name)).second)
            Fail("Duplicate constant name '" + item.Name + "'.");
    for (const auto &rule : document.Rules) {
        if (!EnumValid(rule.Kind, VisualRuleKind::Block))
            Fail("Invalid visual rule.");
        Control(rule.Source);
        Control(rule.Target);
        if (rule.Kind == VisualRuleKind::Deadzone && (rule.Value < 0 || rule.Value > 99))
            Fail("Deadzone must be between 0 and 99 percent.");
        if (rule.Kind == VisualRuleKind::Sensitivity && (rule.Value < 0 || rule.Value > 1000))
            Fail("Sensitivity must be between 0 and 1000 percent.");
    }
    names.clear();
    for (const auto &macro : document.Macros) {
        if (!names.insert(Name(macro.Name)).second)
            Fail("Duplicate macro name '" + macro.Name + "'.");
        Control(macro.Trigger);
        if (!EnumValid(macro.Kind, VisualMacroKind::AxisPattern) ||
            !EnumValid(macro.Activation, VisualMacroActivation::Toggle))
            Fail("Invalid macro activation or kind.");
        ValidatePattern(macro.Pattern);
        if (macro.Steps.size() > 256)
            Fail("A visual macro may contain at most 256 steps.");
        for (const auto &step : macro.Steps) {
            Control(step.Control);
            if (!EnumValid(step.Blend, VisualBlend::Replace))
                Fail("Invalid macro step blend.");
            if (step.DurationMilliseconds < 1 || step.DurationMilliseconds > 60000)
                Fail("A macro step duration must be between 1 and 60,000 ms.");
        }
        if (macro.Condition) {
            int nodes = 0;
            ValidateCondition(*macro.Condition, 0, nodes);
        }
    }
}
VisualCondition ReadCondition(const Json &value, const int depth, int &nodes) {
    if (depth > 12 || ++nodes > 128)
        Fail("Conditions exceed 12 nested levels or 128 nodes.");
    CheckFields(value, {"Kind", "Control", "Operator", "Value", "Milliseconds", "Expression", "Children"});
    VisualCondition result;
    result.Kind = ReadEnum(value, "Kind", result.Kind, VisualConditionKind::Expression);
    result.Control = ReadString(value, "Control", result.Control);
    result.Operator = ReadString(value, "Operator", result.Operator);
    result.Value = ReadInt(value, "Value", result.Value);
    result.Milliseconds = ReadInt(value, "Milliseconds", result.Milliseconds);
    result.Expression = ReadString(value, "Expression", "", true);
    const auto children = value.find("Children");
    if (children != value.end()) {
        CheckArray(*children, 16);
        for (const auto &child : *children)
            result.Children.push_back(ReadCondition(child, depth + 1, nodes));
    }
    return result;
}
VisualAxisPattern ReadPattern(const Json &value) {
    CheckFields(value, {"TargetX", "TargetY", "Shape", "AmplitudeX", "AmplitudeY", "PeriodMilliseconds",
                        "TickMilliseconds", "Blend", "Center"});
    VisualAxisPattern result;
    result.TargetX = ReadString(value, "TargetX", result.TargetX);
    result.TargetY = ReadString(value, "TargetY", result.TargetY);
    result.Shape = ReadEnum(value, "Shape", result.Shape, VisualPatternShape::Sine);
    result.AmplitudeX = ReadInt(value, "AmplitudeX", result.AmplitudeX);
    result.AmplitudeY = ReadInt(value, "AmplitudeY", result.AmplitudeY);
    result.PeriodMilliseconds = ReadInt(value, "PeriodMilliseconds", result.PeriodMilliseconds);
    result.TickMilliseconds = ReadInt(value, "TickMilliseconds", result.TickMilliseconds);
    result.Blend = ReadEnum(value, "Blend", result.Blend, VisualBlend::Replace);
    result.Center = ReadEnum(value, "Center", result.Center, VisualPatternCenter::Zero);
    return result;
}
void ReadPayload(const std::string &encoded, VisualDocument &document) {
    const Json value = ParseJson(Unbase64(encoded));
    CheckFields(value, {"Version", "Rules", "Constants", "Macros"});
    if (ReadInt(value, "Version", 0) != 2 || !value.contains("Rules") || !value.contains("Constants") ||
        !value.contains("Macros"))
        Fail("Invalid visual document metadata version or limits.");
    CheckArray(value["Rules"], 256);
    CheckArray(value["Constants"], 256);
    CheckArray(value["Macros"], 32);
    for (const auto &item : value["Rules"]) {
        CheckFields(item, {"Kind", "Source", "Target", "Value", "Enabled"});
        VisualRule rule;
        rule.Kind = ReadEnum(item, "Kind", rule.Kind, VisualRuleKind::Block);
        rule.Source = ReadString(item, "Source", rule.Source);
        rule.Target =
            ReadString(item, "Target",
                       item.contains("Target") && item["Target"].is_null() ? "SOUTH" : rule.Target, true);
        rule.Value = ReadInt(item, "Value", rule.Value);
        rule.Enabled = ReadBool(item, "Enabled", rule.Enabled);
        document.Rules.push_back(std::move(rule));
    }
    for (const auto &item : value["Constants"]) {
        CheckFields(item, {"Name", "Value"});
        VisualConstant constant;
        constant.Name = ReadString(item, "Name", "");
        constant.Value = ReadInt(item, "Value", 0);
        document.Constants.push_back(std::move(constant));
    }
    for (const auto &item : value["Macros"]) {
        CheckFields(item,
                    {"Name", "Trigger", "Enabled", "Condition", "Activation", "Kind", "Pattern", "Steps"});
        VisualMacro macro;
        macro.Name = ReadString(item, "Name", macro.Name);
        macro.Trigger = ReadString(item, "Trigger", macro.Trigger);
        macro.Enabled = ReadBool(item, "Enabled", macro.Enabled);
        macro.Kind = ReadEnum(item, "Kind", macro.Kind, VisualMacroKind::AxisPattern);
        macro.Activation = ReadEnum(item, "Activation", macro.Activation, VisualMacroActivation::Toggle);
        if (item.contains("Condition") && !item["Condition"].is_null()) {
            int nodes = 0;
            macro.Condition = ReadCondition(item["Condition"], 0, nodes);
        }
        if (item.contains("Pattern")) {
            if (item["Pattern"].is_null()) {
                if (macro.Kind == VisualMacroKind::AxisPattern)
                    Fail("An axis pattern is missing its movement settings.");
            } else
                macro.Pattern = ReadPattern(item["Pattern"]);
        }
        if (item.contains("Steps")) {
            CheckArray(item["Steps"], 256);
            for (const auto &raw : item["Steps"]) {
                CheckFields(raw, {"Control", "Value", "DurationMilliseconds", "Blend"});
                VisualMacroStep step;
                step.Control = ReadString(raw, "Control", step.Control);
                step.Value = ReadInt(raw, "Value", step.Value);
                step.DurationMilliseconds = ReadInt(raw, "DurationMilliseconds", step.DurationMilliseconds);
                step.Blend = ReadEnum(raw, "Blend", step.Blend, VisualBlend::Replace);
                macro.Steps.push_back(std::move(step));
            }
        }
        document.Macros.push_back(std::move(macro));
    }
}
Json ConditionJson(const VisualCondition &condition) {
    Json children = Json::array();
    for (const auto &child : condition.Children)
        children.push_back(ConditionJson(child));
    return Json{{"Kind", static_cast<int>(condition.Kind)}, {"Control", condition.Control},
                {"Operator", condition.Operator},           {"Value", condition.Value},
                {"Milliseconds", condition.Milliseconds},   {"Expression", condition.Expression},
                {"Children", std::move(children)}};
}
Json PatternJson(const VisualAxisPattern &pattern) {
    return Json{{"TargetX", pattern.TargetX},
                {"TargetY", pattern.TargetY},
                {"Shape", static_cast<int>(pattern.Shape)},
                {"AmplitudeX", pattern.AmplitudeX},
                {"AmplitudeY", pattern.AmplitudeY},
                {"PeriodMilliseconds", pattern.PeriodMilliseconds},
                {"TickMilliseconds", pattern.TickMilliseconds},
                {"Blend", static_cast<int>(pattern.Blend)},
                {"Center", static_cast<int>(pattern.Center)}};
}
std::string MetadataJson(const VisualDocument &document) {
    Json rules = Json::array(), constants = Json::array(), macros = Json::array();
    for (const auto &rule : document.Rules)
        rules.push_back(Json{{"Kind", static_cast<int>(rule.Kind)},
                             {"Source", rule.Source},
                             {"Target", rule.Target},
                             {"Value", rule.Value},
                             {"Enabled", rule.Enabled}});
    for (const auto &constant : document.Constants)
        if (constant.OriginalStart < 0)
            constants.push_back(Json{{"Name", constant.Name}, {"Value", constant.Value}});
    for (const auto &macro : document.Macros) {
        Json steps = Json::array();
        for (const auto &step : macro.Steps)
            steps.push_back(Json{{"Control", step.Control},
                                 {"Value", step.Value},
                                 {"DurationMilliseconds", step.DurationMilliseconds},
                                 {"Blend", static_cast<int>(step.Blend)}});
        macros.push_back(
            Json{{"Name", macro.Name},
                 {"Trigger", macro.Trigger},
                 {"Enabled", macro.Enabled},
                 {"Kind", static_cast<int>(macro.Kind)},
                 {"Activation", static_cast<int>(macro.Activation)},
                 {"Condition", macro.Condition ? ConditionJson(*macro.Condition) : Json(nullptr)},
                 {"Pattern", PatternJson(macro.Pattern)},
                 {"Steps", std::move(steps)}});
    }
    // nlohmann::json uses an ordered std::map for object keys. Arrays retain action order.
    try {
        std::string result = Json{
            {"Version", 2},
            {"Rules", std::move(rules)},
            {"Constants", std::move(constants)},
            {"Macros",
             std::move(macros)}}.dump();
        // Match the preceding release's JavaScriptSerializer escaping as well as its
        // canonical key order, so simply visiting visual view does not rewrite a file.
        ReplaceAll(result, "'", "\\u0027");
        ReplaceAll(result, "<", "\\u003c");
        ReplaceAll(result, ">", "\\u003e");
        ReplaceAll(result, "&", "\\u0026");
        ReplaceAll(result, "\xC2\x85", "\\u0085");
        ReplaceAll(result, "\xE2\x80\xA8", "\\u2028");
        ReplaceAll(result, "\xE2\x80\xA9", "\\u2029");
        if (result.size() > MaxJson)
            Fail("Visual metadata exceeds its size limit.");
        return result;
    } catch (const Json::exception &) {
        Fail("Visual metadata contains invalid UTF-8 text.");
    }
}
std::string RemoveManagedSection(std::string source) {
    const std::size_t start = source.find(Begin), finish = source.find(End);
    if ((start == std::string::npos) != (finish == std::string::npos) ||
        (start != std::string::npos && finish < start))
        Fail("Visual section markers are incomplete.");
    if (start != std::string::npos)
        source.erase(start, finish + End.size() - start);
    const std::string call = R"rx(/\* @igc-visual-call \*/ __igc_visual_apply(?:_[0-9]+)?\(\);)rx";
    source = std::regex_replace(source, std::regex("\\r?\\n    " + call + "\\r?\\n"), "");
    return std::regex_replace(source, std::regex(call), "");
}
const std::array<const char *, 5> RuleNames{"Remap", "Invert", "Deadzone", "Sensitivity", "Block"};
VisualRuleKind LegacyRuleKind(const std::string &value) {
    for (std::size_t i = 0; i < RuleNames.size(); ++i)
        if (value == RuleNames[i])
            return static_cast<VisualRuleKind>(i);
    const auto result = static_cast<VisualRuleKind>(SignedInteger(value));
    if (!EnumValid(result, VisualRuleKind::Block))
        Fail("Malformed visual rule metadata.");
    return result;
}
bool IsHat(const std::string &control) {
    const int id = Controls::Parse(control);
    return id >= 192 && id < 208;
}
std::string ConditionSource(const VisualCondition &condition) {
    if (condition.Kind == VisualConditionKind::All || condition.Kind == VisualConditionKind::Any) {
        std::string result;
        for (const auto &child : condition.Children) {
            if (!result.empty())
                result += condition.Kind == VisualConditionKind::All ? " && " : " || ";
            result += "(" + ConditionSource(child) + ")";
        }
        return result;
    }
    if (condition.Kind == VisualConditionKind::Not)
        return "!(" + ConditionSource(condition.Children[0]) + ")";
    if (condition.Kind == VisualConditionKind::Expression)
        return "(" + condition.Expression + ")";
    const std::string control = Control(condition.Control), available = "is_available(" + control + ")",
                      physical = "get_ival(" + control + ")";
    switch (condition.Kind) {
    case VisualConditionKind::Held:
        return available + " && " + physical + (IsHat(control) ? " >= 0" : " != 0");
    case VisualConditionKind::Pressed:
        return available + " && event_press(" + control + ")";
    case VisualConditionKind::Released:
        return available + " && event_release(" + control + ")";
    case VisualConditionKind::Compare:
        return available + " && " + physical + " " + condition.Operator + " " +
               std::to_string(condition.Value);
    case VisualConditionKind::AbsoluteCompare:
        return available + " && abs(" + physical + ") " + condition.Operator + " " +
               std::to_string(condition.Value);
    case VisualConditionKind::HeldFor:
        return available + " && " + physical + (IsHat(control) ? " >= 0" : " != 0") + " && get_ptime(" +
               control + ") >= " + std::to_string(condition.Milliseconds);
    case VisualConditionKind::DoubleTap:
        return available + " && double_tap(" + control + ", " + std::to_string(condition.Milliseconds) + ")";
    default:
        Fail("Unsupported visual condition.");
    }
}
void EmitPattern(const VisualMacro &macro, const std::string &state, const std::string &condition,
                 const std::string &lifetime, std::ostringstream &definitions, std::ostringstream &body,
                 std::ostringstream &combos) {
    const auto &pattern = macro.Pattern;
    const std::string x = Control(pattern.TargetX), y = Control(pattern.TargetY),
                      period = std::to_string(pattern.PeriodMilliseconds);
    definitions << "int " << state << "_was, " << state << "_origin, " << state << "_phase, " << state
                << "_angle, " << state << "_x, " << state << "_y;" << NL;
    body << "    " << state << "_was = " << state << "_active;" << NL << "    " << state
         << "_condition = " << condition << ';' << NL;
    const std::string enter = state + "_condition && !" + state + "_last",
                      exit = "!" + state + "_condition && " + state + "_last";
    switch (macro.Activation) {
    case VisualMacroActivation::OnEnter:
        body << "    if (" << enter << ") { " << state << "_active = 1; " << state
             << "_was = 0; combo_restart(" << lifetime << "); }" << NL;
        break;
    case VisualMacroActivation::WhileTrue:
        body << "    " << state << "_active = " << state << "_condition;" << NL;
        break;
    case VisualMacroActivation::OnExit:
        body << "    if (" << exit << ") { " << state << "_active = 1; " << state
             << "_was = 0; combo_restart(" << lifetime << "); }" << NL;
        break;
    case VisualMacroActivation::Toggle:
        body << "    if (" << enter << ") " << state << "_active = !" << state << "_active;" << NL;
        break;
    }
    const bool once = macro.Activation == VisualMacroActivation::OnEnter ||
                      macro.Activation == VisualMacroActivation::OnExit;
    if (once)
        combos << "combo " << lifetime << " { wait(" << period << "); " << state << "_active = 0; }" << NL;
    body << "    if (!is_available(" << x << ") || !is_available(" << y << ")) " << state << "_active = 0;"
         << NL;
    if (once)
        body << "    if (!" << state << "_active) combo_stop(" << lifetime << ");" << NL;
    body << "    if (" << state << "_active && !" << state << "_was) {" << NL << "        " << state
         << "_origin = get_phase(" << period << ");" << NL;
    if (pattern.Center == VisualPatternCenter::Activation)
        body << "        " << state << "_x = get_ival(" << x << ");" << NL << "        " << state
             << "_y = get_ival(" << y << ");" << NL;
    body << "    }" << NL << "    if (" << state << "_active) {" << NL << "        tick("
         << pattern.TickMilliseconds << ");" << NL;
    body << "        " << state << "_phase = (get_phase(" << period << ") - " << state << "_origin + "
         << period << ") % " << period << ';' << NL;
    body << "        " << state << "_angle = " << state << "_phase * 360 / " << period << ';' << NL;
    const std::string dx = "sin(" + state + "_angle, " + std::to_string(pattern.AmplitudeX) + ")";
    const std::string dy =
        pattern.Shape == VisualPatternShape::Sine
            ? "0"
            : (pattern.Shape == VisualPatternShape::Circle ? "cos(" + state + "_angle, "
                                                           : "sin(" + state + "_angle * 2, ") +
                  std::to_string(pattern.AmplitudeY) + ")";
    if (pattern.Blend == VisualBlend::Add)
        body << "        add_val(" << x << ", " << dx << ");" << NL << "        add_val(" << y << ", " << dy
             << ");" << NL;
    else {
        const std::string cx = pattern.Center == VisualPatternCenter::DynamicPhysical ? "get_ival(" + x + ")"
                               : pattern.Center == VisualPatternCenter::Activation    ? state + "_x"
                                                                                      : "0";
        const std::string cy = pattern.Center == VisualPatternCenter::DynamicPhysical ? "get_ival(" + y + ")"
                               : pattern.Center == VisualPatternCenter::Activation    ? state + "_y"
                                                                                      : "0";
        body << "        set_val(" << x << ", " << cx << " + " << dx << ");" << NL << "        set_val(" << y
             << ", " << cy << " + " << dy << ");" << NL;
    }
    body << "    }" << NL << "    " << state << "_last = " << state << "_condition;" << NL;
}
} // namespace

VisualDocument VisualDocument::Parse(const std::string &source) {
    CheckUtf8(source);
    const std::size_t begin = source.find(Begin), end = source.find(End);
    if ((begin == std::string::npos) != (end == std::string::npos) ||
        (begin != std::string::npos && end < begin))
        Fail("Visual section markers are incomplete.");
    if (begin != std::string::npos && (source.find(Begin, begin + Begin.size()) != std::string::npos ||
                                       source.find(End, end + End.size()) != std::string::npos))
        Fail("Invalid or duplicate visual section markers.");
    bool modified = false;
    if (begin != std::string::npos) {
        std::size_t hashStart = begin + Begin.size();
        while (hashStart < end && (source[hashStart] == '\r' || source[hashStart] == '\n'))
            ++hashStart;
        const std::size_t hashEnd = source.find('\n', hashStart);
        const std::string line = hashEnd != std::string::npos && hashEnd < end
                                     ? Trim(source.substr(hashStart, hashEnd - hashStart))
                                     : "";
        const std::string content = hashEnd != std::string::npos && hashEnd < end
                                        ? source.substr(hashEnd + 1, end - hashEnd - 1)
                                        : "";
        modified = !Starts(line, "// @hash ") || line.substr(9) != Hash(content);
    }
    std::string preserved = source;
    if (modified) {
        ReplaceAll(preserved, Begin, "// Advanced code (edited visual section)");
        ReplaceAll(preserved, End, "// End advanced code");
        ReplaceAll(preserved, CallPrefix, "/* Custom visual call */ ");
    } else
        preserved = RemoveManagedSection(std::move(preserved));
    VisualDocument document(std::move(preserved));
    document.ManagedSectionModified = modified;
    if (begin != std::string::npos && !modified) {
        std::istringstream lines(source.substr(begin, end - begin));
        std::string raw;
        bool payload = false;
        std::size_t current = std::string::npos;
        while (std::getline(lines, raw)) {
            const std::string line = Trim(raw);
            if (!Starts(line, "// @"))
                continue;
            std::istringstream fieldStream(line.substr(4));
            std::vector<std::string> fields;
            std::string field;
            while (fieldStream >> field)
                fields.push_back(field);
            if (fields.empty() || fields[0] == "hash")
                continue;
            if (fields[0] == "document" && fields.size() == 2) {
                if (payload || !document.Rules.empty() || !document.Macros.empty() ||
                    !document.Constants.empty())
                    Fail("Duplicate visual document metadata.");
                ReadPayload(fields[1], document);
                payload = true;
            } else if (payload)
                Fail("Mixed visual document metadata is not supported.");
            else if (fields[0] == "rule" && fields.size() == 6) {
                VisualRule rule;
                rule.Kind = LegacyRuleKind(fields[1]);
                rule.Source = fields[2];
                rule.Target = fields[3];
                rule.Value = SignedInteger(fields[4]);
                const int enabled = SignedInteger(fields[5]);
                if (enabled < 0 || enabled > 1)
                    Fail("Malformed visual rule metadata.");
                rule.Enabled = enabled == 1;
                document.Rules.push_back(std::move(rule));
            } else if (fields[0] == "macro" && fields.size() == 4) {
                VisualMacro macro;
                macro.Name = fields[1];
                macro.Trigger = fields[2];
                const int enabled = SignedInteger(fields[3]);
                if (enabled < 0 || enabled > 1)
                    Fail("Malformed visual macro metadata.");
                macro.Enabled = enabled == 1;
                current = document.Macros.size();
                document.Macros.push_back(std::move(macro));
            } else if (fields[0] == "step" && fields.size() == 4 && current != std::string::npos) {
                VisualMacroStep step;
                step.Control = fields[1];
                step.Value = SignedInteger(fields[2]);
                step.DurationMilliseconds = SignedInteger(fields[3]);
                document.Macros[current].Steps.push_back(std::move(step));
            } else if (fields[0] == "constant" && fields.size() == 3) {
                VisualConstant constant;
                constant.Name = fields[1];
                constant.Value = SignedInteger(fields[2]);
                document.Constants.push_back(std::move(constant));
            } else
                Fail("Unknown or malformed visual metadata. Edit this file in code view to repair its "
                     "managed section.");
            if (document.Rules.size() > 256 || document.Macros.size() > 32 ||
                document.Constants.size() > 256 ||
                (current != std::string::npos && document.Macros[current].Steps.size() > 256))
                Fail("Visual document exceeds 256 rules/constants, 32 macros or 256 steps per macro.");
        }
    }
    const std::string masked = MaskComments(document.original_);
    for (auto match = std::sregex_iterator(masked.begin(), masked.end(), IntegerDefines);
         match != std::sregex_iterator(); ++match) {
        int value = 0;
        if (!TryInteger((*match)[2].str(), value))
            continue;
        VisualConstant constant;
        constant.Name = constant.OriginalName = (*match)[1].str();
        constant.Value = value;
        constant.OriginalStart = std::distance(masked.cbegin(), (*match)[2].first);
        constant.OriginalLength = static_cast<std::size_t>((*match)[2].length());
        document.Constants.push_back(std::move(constant));
        if (document.Constants.size() > 256)
            Fail("A visual document may contain at most 256 constants.");
    }
    ValidateRows(document);
    std::string remaining = std::regex_replace(masked, IntegerDefines, "");
    remaining = std::regex_replace(remaining, std::regex(R"rx(\bmain\s*\{\s*\})rx"), "");
    document.HasAdvancedCode = !Trim(remaining).empty();
    return document;
}
VisualMacro VisualDocument::CreateFigureEightPreset() {
    VisualCondition condition;
    condition.Kind = VisualConditionKind::All;
    VisualCondition south;
    south.Kind = VisualConditionKind::Held;
    south.Control = "SOUTH";
    condition.Children.push_back(std::move(south));
    VisualCondition east;
    east.Kind = VisualConditionKind::Held;
    east.Control = "EAST";
    condition.Children.push_back(std::move(east));
    VisualCondition camera;
    camera.Kind = VisualConditionKind::Compare;
    camera.Control = "RX";
    camera.Operator = ">";
    camera.Value = 50;
    condition.Children.push_back(std::move(camera));
    VisualMacro macro;
    macro.Name = "Infinity";
    macro.Kind = VisualMacroKind::AxisPattern;
    macro.Activation = VisualMacroActivation::WhileTrue;
    macro.Condition = std::move(condition);
    return macro;
}
std::vector<std::string> VisualDocument::GetWarnings() const {
    std::vector<std::string> result;
    if (ManagedSectionModified)
        result.push_back("Hand-edited generated code is preserved as advanced code; its old visual controls "
                         "are no longer applied.");
    for (const auto &macro : Macros)
        if (macro.Enabled && macro.Kind == VisualMacroKind::AxisPattern) {
            if (macro.Pattern.Blend == VisualBlend::Add)
                result.push_back(macro.Name +
                                 ": additive movement is clamped at the controller's axis limits.");
            if (macro.Pattern.Center == VisualPatternCenter::Activation)
                result.push_back(
                    macro.Name +
                    ": the center is captured on activation; later stick movement does not move it.");
        }
    return result;
}
std::string VisualDocument::BuildSource() const {
    ValidateRows(*this);
    std::string advanced = original_;
    std::vector<const VisualConstant *> replacements;
    for (const auto &item : Constants)
        if (item.OriginalStart >= 0) {
            if (item.Name != item.OriginalName)
                Fail("A constant in custom code cannot be renamed in visual view; rename it and its "
                     "references in code view.");
            const auto start = static_cast<std::size_t>(item.OriginalStart);
            if (start > advanced.size() || item.OriginalLength > advanced.size() - start)
                Fail("Invalid preserved constant source offset.");
            replacements.push_back(&item);
        }
    std::sort(replacements.begin(), replacements.end(),
              [](const auto a, const auto b) { return a->OriginalStart > b->OriginalStart; });
    for (const auto item : replacements)
        advanced.replace(static_cast<std::size_t>(item->OriginalStart), item->OriginalLength,
                         std::to_string(item->Value));
    const std::string masked = MaskComments(advanced);
    std::string macroPrefix = "__igc_macro_";
    unsigned generation = 0;
    while (std::regex_search(masked, std::regex("\\b" + macroPrefix + "[0-9]+\\b")))
        macroPrefix = "__igc_macro_" + std::to_string(++generation) + "_";
    std::string statePrefix = "__igc_visual_state_";
    generation = 0;
    while (masked.find(statePrefix) != std::string::npos)
        statePrefix = "__igc_visual_state_" + std::to_string(++generation) + "_";
    std::ostringstream definitions, body, combos;
    // Use the classic locale even if the application changes the user's display locale.
    definitions.imbue(std::locale::classic());
    body.imbue(std::locale::classic());
    combos.imbue(std::locale::classic());
    for (const auto &item : Constants)
        if (item.OriginalStart < 0)
            definitions << "define " << item.Name << " = " << item.Value << ';' << NL;
    for (const auto &rule : Rules) {
        const std::string control = Control(rule.Source), target = Control(rule.Target);
        if (!rule.Enabled)
            continue;
        switch (rule.Kind) {
        case VisualRuleKind::Remap:
            body << "    remap(" << control << ", " << target << ");" << NL;
            break;
        case VisualRuleKind::Invert:
            body << "    invert(" << control << ");" << NL;
            break;
        case VisualRuleKind::Deadzone:
            body << "    deadzone(" << control << ", " << rule.Value << ");" << NL;
            break;
        case VisualRuleKind::Sensitivity:
            body << "    sensitivity(" << control << ", " << rule.Value << ");" << NL;
            break;
        case VisualRuleKind::Block:
            body << "    set_val(" << control << ", " << (IsHat(control) ? -1 : 0) << ");" << NL;
            break;
        }
    }
    for (std::size_t m = 0; m < Macros.size(); ++m) {
        const auto &macro = Macros[m];
        if (!macro.Enabled)
            continue;
        if (macro.Kind == VisualMacroKind::Sequence && macro.Steps.empty())
            Fail("Macro '" + macro.Name + "' needs at least one timed step.");
        const std::string combo = macroPrefix + std::to_string(m), state = statePrefix + std::to_string(m);
        std::string condition = macro.Condition ? ConditionSource(*macro.Condition)
                                                : "event_press(" + Control(macro.Trigger) + ")";
        definitions << "int " << state << "_last, " << state << "_condition, " << state << "_active;" << NL;
        if (macro.Kind == VisualMacroKind::AxisPattern) {
            condition = "is_available(" + Control(macro.Pattern.TargetX) + ") && is_available(" +
                        Control(macro.Pattern.TargetY) + ") && (" + condition + ")";
            EmitPattern(macro, state, condition, combo, definitions, body, combos);
        } else {
            body << "    " << state << "_condition = " << condition << ';' << NL;
            const std::string enter = state + "_condition && !" + state + "_last",
                              exit = "!" + state + "_condition && " + state + "_last";
            switch (macro.Activation) {
            case VisualMacroActivation::OnEnter:
                body << "    if (" << enter << ") combo_run(" << combo << ");" << NL;
                break;
            case VisualMacroActivation::OnExit:
                body << "    if (" << exit << ") combo_run(" << combo << ");" << NL;
                break;
            case VisualMacroActivation::WhileTrue:
                body << "    " << state << "_active = " << state << "_condition;" << NL;
                break;
            case VisualMacroActivation::Toggle:
                body << "    if (" << enter << ") " << state << "_active = !" << state << "_active;" << NL;
                break;
            }
            const bool repeat = macro.Activation == VisualMacroActivation::WhileTrue ||
                                macro.Activation == VisualMacroActivation::Toggle;
            if (repeat)
                body << "    if (" << state << "_active) combo_run(" << combo << "); else combo_stop("
                     << combo << ");" << NL;
            body << "    " << state << "_last = " << state << "_condition;" << NL;
            combos << "combo " << combo << " {" << NL;
            if (repeat)
                combos << "    while (" << state << "_active) {" << NL;
            for (const auto &step : macro.Steps)
                combos << "    " << (step.Blend == VisualBlend::Add ? "add_val(" : "set_val(")
                       << Control(step.Control) << ", " << step.Value << "); wait("
                       << step.DurationMilliseconds << ");" << NL;
            if (repeat)
                combos << "    }" << NL;
            combos << '}' << NL;
        }
    }
    std::string helper = "__igc_visual_apply";
    generation = 0;
    while (std::regex_search(masked, std::regex("\\b" + helper + "\\b")))
        helper = "__igc_visual_apply_" + std::to_string(++generation);
    const std::string call = std::string(CallPrefix) + helper + "();";
    const std::string maskedMain = MaskComments(advanced);
    std::smatch mainMatch;
    if (!std::regex_search(maskedMain, mainMatch, std::regex(R"rx(\bmain\s*\{)rx")))
        advanced += std::string(NL) + "main {" + NL + "    " + call + NL + "}" + NL;
    else {
        std::size_t finish = static_cast<std::size_t>(mainMatch.position() + mainMatch.length());
        int nesting = 1;
        for (; finish < maskedMain.size(); ++finish) {
            if (maskedMain[finish] == '{')
                ++nesting;
            else if (maskedMain[finish] == '}' && --nesting == 0)
                break;
        }
        if (finish == maskedMain.size())
            Fail("The main block is incomplete.");
        advanced.insert(finish, std::string(NL) + "    " + call + NL);
    }
    const std::string content = "// @document " + Base64(MetadataJson(*this)) + NL + definitions.str() +
                                "function " + helper + "() {" + NL + body.str() + "}" + NL + combos.str();
    TrimEnd(advanced);
    std::string result = advanced + NL + NL + std::string(Begin) + NL + "// @hash " + Hash(content) + NL +
                         content + std::string(End) + NL;
    CheckUtf8(result);
    static_cast<void>(ScriptCompiler::Compile(result));
    return result;
}
} // namespace gpr
