#include "Script.h"
#include <unordered_map>

namespace gpr {
namespace {
std::string Upper(std::string s) {
    for (auto &c : s)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return s;
}
} // namespace
const std::array<std::string, 256> &Controls::Names() {
    static const auto names = [] {
        std::array<std::string, 256> n{};
        const char *canonical[]{"LX",   "LY",    "RX",    "RY",   "LT",    "RT",    "UP", "DOWN",
                                "LEFT", "RIGHT", "SOUTH", "EAST", "WEST",  "NORTH", "LB", "RB",
                                "VIEW", "MENU",  "LS",    "RS",   "GUIDE", "TOUCH"};
        for (int i = 0; i < 22; ++i)
            n[i] = canonical[i];
        for (int i = 22; i < 32; ++i)
            n[i] = "RESERVED_" + std::to_string(i);
        for (int i = 0; i < 64; ++i) {
            n[32 + i] = "BUTTON_" + std::to_string(i + 1);
            n[96 + i] = "AXIS_" + std::to_string(i + 1);
        }
        for (int i = 0; i < 32; ++i)
            n[160 + i] = "TRIGGER_" + std::to_string(i + 1);
        for (int i = 0; i < 16; ++i)
            n[192 + i] = "HAT_" + std::to_string(i + 1);
        for (int i = 0; i < 48; ++i)
            n[208 + i] = "VALUE_" + std::to_string(i + 1);
        return n;
    }();
    return names;
}
int Controls::Parse(const std::string &name) {
    static const auto lookup = [] {
        std::unordered_map<std::string, int> m;
        for (int i = 0; i < 256; ++i)
            m[Names()[i]] = i;
        const std::pair<const char *, int> aliases[]{
            {"A", 10},       {"B", 11},       {"X", 12},        {"Y", 13},     {"CROSS", 10},
            {"CIRCLE", 11},  {"SQUARE", 12},  {"TRIANGLE", 13}, {"L1", 14},    {"R1", 15},
            {"L2", 4},       {"R2", 5},       {"L3", 18},       {"R3", 19},    {"SHARE", 16},
            {"CREATE", 16},  {"OPTIONS", 17}, {"PS", 20},       {"START", 17}, {"BACK", 16},
            {"TOUCHPAD", 21}};
        for (const std::string prefix : {"", "XB1_", "XB360_", "PS4_", "PS5_"}) {
            for (int i = 0; i < 22; ++i)
                m[prefix + Names()[i]] = i;
            for (const auto &alias : aliases)
                m[prefix + alias.first] = alias.second;
        }
        return m;
    }();
    const auto found = lookup.find(Upper(name));
    return found == lookup.end() ? -1 : found->second;
}
ScriptException::ScriptException(const std::string &message, int line, int column)
    : std::runtime_error(message), Line(line), Column(column) {}
std::string ScriptException::Diagnostic() const {
    return "Line " + std::to_string(Line) + ", column " + std::to_string(Column) + ": " + what();
}
} // namespace gpr
