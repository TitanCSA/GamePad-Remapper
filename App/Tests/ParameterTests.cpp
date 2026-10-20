#include "../Core/Script.h"
#include "../Core/VisualDocument.h"
#include <climits>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>

namespace {
int checks = 0;
void Check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
void Reject(const std::string &source) {
    bool rejected = false;
    try {
        gpr::ScriptCompiler::Compile(source);
    } catch (const gpr::ScriptException &e) {
        rejected = e.Line > 0 && e.Column > 0;
    }
    Check(rejected, "Invalid parameter declarations need a source-positioned compiler error.");
}
const std::string Header = R"igc(ui {
 title("Controller settings"); description("Unicode: réglages 🎮; braces { }, // text");
 group("Sticks"); slider zone("Deadzone",0,99,10,"%"); help("Inner deadzone");
 toggle enabled("Enable mapping",true);
 select mode("Mode",7) {7:"Normal";42:"Invert";-3:"Quiet";};
 group("Timing"); number delay_ms("Delay",1,2000,80,1," ms");
})igc";
} // namespace
int main() {
    using namespace gpr;
    try {
        const auto program = ScriptCompiler::Compile(Header + R"igc(
int calls, initial;
init { initial = zone; }
main { calls++; set_val(LX,enabled ? zone : 0); set_val(LY,mode); set_val(VALUE_1,calls); set_val(VALUE_2,initial); }
)igc");
        const auto &ui = program->Interface();
        Check(ui.Title == "Controller settings" && ui.Parameters.size() == 4,
              "UI metadata preserves declaration order.");
        Check(ui.Parameters[0].Group == "Sticks" && ui.Parameters[0].Help == "Inner deadzone" &&
                  ui.Parameters[0].Unit == "%",
              "Group, help and units survive compilation.");
        Check(ui.Parameters[2].Choices[1].Value == 42 && ui.Parameters[3].Group == "Timing",
              "Selections use declared values, not list indices.");
        auto values = ui.Defaults();
        auto vm = program->CreateInstance();
        ControlFrame f;
        f.Available.fill(true);
        vm->Execute(f, 0, 0);
        Check(f.Values[0] == 10 && f.Values[1] == 7 && f.Values[208] == 1 && f.Values[209] == 10,
              "Default UI values are readable in init/main.");
        for (int i = 1; i <= 10000; ++i) {
            values[0] = i % 100;
            values[1] = (i & 1);
            values[2] = (i & 1) ? 42 : -3;
            vm->SetParameters(values);
            vm->Execute(f, static_cast<std::int64_t>(i) * 1000, 1);
            Check(f.Values[0] == (values[1] ? values[0] : 0) && f.Values[1] == values[2] &&
                      f.Values[208] == i + 1 && f.Values[209] == 10,
                  "Hot settings update preserves globals/init state and reaches the next evaluation.");
        }
        auto bad = values;
        bad[0] = 100;
        bool invalid = false;
        try {
            vm->SetParameters(bad);
        } catch (const std::invalid_argument &) {
            invalid = true;
        }
        vm->Execute(f, 10001000, 1);
        Check(invalid && f.Values[0] == (values[1] ? values[0] : 0),
              "Invalid updates are atomic and retain previous values.");
        auto other = program->CreateInstance();
        other->Execute(f, 0, 0);
        Check(f.Values[0] == 10 && f.Values[208] == 1,
              "A new VM keeps independent state and script defaults.");
        auto macro = ScriptCompiler::Compile(
            "ui {slider power(\"Power\",0,100,20);} int edges; main "
            "{if(event_press(SOUTH)){edges++;combo_run(M);}set_val(VALUE_1,edges);} combo M "
            "{set_val(LX,power);wait(100);set_val(LY,power);wait(100);}");
        auto m = macro->CreateInstance();
        ControlFrame b;
        b.Available.fill(true);
        b.Physical[Controls::Parse("SOUTH")] = 100;
        m->Execute(b, 0, 0);
        auto p = macro->Interface().Defaults();
        p[0] = 70;
        m->SetParameters(p);
        m->Execute(b, 50000, 50);
        Check(b.Values[0] == 20 && b.Values[208] == 1,
              "Parameter edits keep waiting combo segments and do not retrigger physical press edges.");
        m->Execute(b, 100000, 50);
        Check(b.Values[1] == 70 && b.Values[208] == 1,
              "The next combo segment reads updated parameters.");
        for (const auto &source : {"ui {slider x(\"X\",100,0,0);} main {}",
                                   "ui {slider x(\"X\",0,100,30,7);} main {}",
                                   "ui {slider x(\"X\",0,100,0,0);} main {}",
                                   "ui {toggle flag(\"X\",2);} main {}",
                                   "ui {select mode(\"X\",1) {}; } main {}",
                                   "ui {select mode(\"X\",1) {1:\"A\";1:\"B\";};} main {}",
                                   "ui {select mode(\"X\",2) {1:\"A\";};} main {}",
                                   "ui {toggle x(\"X\",1);toggle x(\"Again\",0);} main {}",
                                   "ui {toggle LX(\"X\",1);} main {}",
                                   "ui {toggle get_val(\"X\",1);} main {}",
                                   "ui {toggle flag(\"X\",1);} main {flag=0;}",
                                   "ui {toggle flag(\"X\",1);} main {flag++;}",
                                   "ui {toggle flag(\"X\",1);} function f(flag) {} main {}",
                                   "function f(flag) {} ui {toggle flag(\"X\",1);} main {}",
                                   "ui {} ui {} main {}",
                                   "ui {help(\"No parameter\");} main {}",
                                   "ui {slider x(\"\",0,100,0);} main {}",
                                   "main {set_val(LX,\"5\");}",
                                   "\"main\" {}",
                                   "main {} \"<end>\"",
                                   "main { \"(\" 1); }",
                                   "ui {title(\"unterminated);} main {}",
                                   "ui {title(\"\\q\");} main {}"})
            Reject(source);
        std::string full = "ui {";
        for (int i = 0; i < 64; ++i)
            full += "toggle p" + std::to_string(i) + "(\"P\",1);";
        Check(
            ScriptCompiler::Compile(full + "} main {set_val(LX,p63);}")->Interface().Parameters.size() ==
                64,
            "Exactly 64 parameters are supported.");
        Reject(full + "toggle overflow(\"Too many\",0);} main {}");
        const auto extreme =
            ScriptCompiler::Compile("ui {number val(\"Integer\",-2147483648,2147483647,2147483647);} "
                                    "main {set_val(VALUE_1,val);}");
        auto ev = extreme->CreateInstance();
        auto e = extreme->Interface().Defaults();
        e[0] = INT_MIN;
        ev->SetParameters(e);
        ev->Execute(f, 0, 0);
        Check(f.Values[208] == INT_MIN, "Full integer range is validated without subtraction overflow.");
        const auto quoted =
            R"igc(ui {title("main { fake } // <igc-visual-v1>");description("define FAKE = 5; /* @igc-visual-call */ __igc_visual_apply();");slider level("Value \"quoted\"",0,100,20);} main {set_val(LX,level);})igc";
        auto visual = VisualDocument::Parse(quoted);
        visual.Rules.push_back({VisualRuleKind::Invert, "LY", "EAST", 0, true});
        const auto rebuilt = visual.BuildSource();
        Check(rebuilt.find("main { fake }") != std::string::npos && visual.Constants.empty(),
              "Visual editing never interprets UI string contents as IGC code or managed markers.");
        const auto roundtrip = VisualDocument::Parse(rebuilt).BuildSource();
        auto r = ScriptCompiler::Compile(roundtrip)->CreateInstance();
        f.Physical[1] = 30;
        r->Execute(f, 0, 0);
        Check(f.Values[0] == 20 && f.Values[1] == -30,
              "Custom UI survives visual edit/code roundtrip with correct behavior.");
        std::printf("Parameter checks passed: %d; 10,000 hot updates; no driver access\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Parameter failure: %s\n", error.what());
        return 1;
    }
}
