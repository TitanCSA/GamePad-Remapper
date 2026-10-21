#include "../Core/Script.h"
#include "CoreParityFixtures.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <vector>
#include <new>

namespace {
std::atomic<std::uint64_t> allocations{0};
}
void *operator new(std::size_t size) {
    if (void *p = std::malloc(size ? size : 1)) {
        allocations.fetch_add(1, std::memory_order_relaxed);
        return p;
    }
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace gpr;
std::uint64_t checks = 0;
int tests = 0;
void Check(bool value, const std::string &description) {
    ++checks;
    if (!value)
        throw std::runtime_error(description);
}
ControlFrame Frame() {
    ControlFrame f;
    f.Available.fill(true);
    for (int i = 192; i < 208; ++i)
        f.Physical[i] = -1;
    return f;
}
ControlFrame Run(const std::string &source) {
    auto f = Frame();
    auto vm = ScriptCompiler::Compile(source)->CreateInstance();
    vm->Execute(f, 0, 0);
    return f;
}
void CompileError(const std::string &source, const std::string &fragment = "") {
    bool rejected = false;
    try {
        (void)ScriptCompiler::Compile(source);
    } catch (const ScriptException &e) {
        rejected = true;
        Check(e.Line > 0 && e.Column > 0, "compile diagnostic location");
        Check(fragment.empty() || std::string(e.what()).find(fragment) != std::string::npos,
              "compile diagnostic: " + std::string(e.what()));
    }
    Check(rejected, "invalid script accepted: " + source);
}
void RuntimeError(const std::string &source, const std::string &fragment) {
    auto f = Frame();
    f.Physical[0] = 31;
    auto vm = ScriptCompiler::Compile(source)->CreateInstance();
    bool rejected = false;
    try {
        vm->Execute(f, 0, 0);
    } catch (const ScriptException &e) {
        rejected = true;
        Check(std::string(e.what()).find(fragment) != std::string::npos,
              "runtime diagnostic: " + std::string(e.what()));
        Check(f.Values == f.Physical, "runtime fail-open restore");
    }
    Check(rejected, "unsafe script accepted");
}
template <class F> void Test(const char *name, F action) {
    action();
    ++tests;
    std::cout << "PASS " << name << '\n';
}
std::string Read(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Missing script: " + path.u8string());
    return {std::istreambuf_iterator<char>(stream), {}};
}
std::uint32_t Next(std::uint32_t &state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
ControlFrame ParityFrame(int number, std::uint32_t &random) {
    ControlFrame f;
    f.DeviceKey = 0x80000000;
    f.Family = 6;
    for (int i = 0; i < 256; ++i) {
        f.Available[i] = Next(random) % 13 != 0;
        const auto n = Next(random);
        f.Physical[i] = i < 4 || (i >= 96 && i < 160) ? static_cast<int>(n % 201) - 100
                        : i >= 192 && i < 208         ? static_cast<int>(n % 9) - 1
                        : i >= 208                    ? static_cast<std::int32_t>(n)
                        : n % 2 == 0                  ? 0
                                                      : 100;
    }
    for (int i = 0; i < 22; ++i)
        f.Available[i] = true;
    f.Available[21] = number % 3 != 0;
    f.Physical[10] = number % 3 == 0 ? 0 : 100;
    f.Physical[11] = number % 4 == 0 ? 0 : 100;
    f.Physical[2] = number % 5 == 0 ? 50 : 80;
    return f;
}
void Word(std::uint64_t &hash, std::uint32_t n) {
    for (int b = 0; b < 4; ++b) {
        hash = (hash ^ static_cast<std::uint8_t>(n)) * 1099511628211ull;
        n >>= 8;
    }
}
std::uint64_t Hash(const ControlFrame &f, int wake) {
    std::uint64_t h = 1469598103934665603ull;
    for (const int v : f.Values)
        Word(h, static_cast<std::uint32_t>(v));
    Word(h, static_cast<std::uint32_t>(wake));
    return h;
}
} // namespace
int main(int argc, char **argv) {
    using namespace gpr;
    std::string jsonPath, scripts;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--json")
            jsonPath = argv[++i];
        else if (std::string(argv[i]) == "--scripts")
            scripts = argv[++i];
    }
    try {
        Test("identity controls and aliases", [] {
            auto f = Frame();
            auto vm = ScriptCompiler::Compile("main{}")->CreateInstance();
            std::mt19937 random(7331);
            for (int round = 0; round < 256; ++round) {
                for (auto &value : f.Physical)
                    value = static_cast<int>(random() % 201) - 100;
                vm->Execute(f, round * 1000LL, 1);
                for (int i = 0; i < 256; ++i)
                    Check(f.Values[i] == f.Physical[i], "identity");
                Check(vm->NextWakeMilliseconds() == 0, "idle no polling");
            }
            for (int i = 0; i < 256; ++i)
                Check(Controls::Parse(Controls::Names()[i]) == i, "control identity");
            Check(Controls::Parse("PS4_CROSS") == 10 && Controls::Parse("XB1_A") == 10 &&
                      Controls::Parse("ps5_r2") == 5,
                  "aliases");
        });
        Test("operators arrays functions and wrapping", [] {
            const auto f = Run(
                "define SIZE=6;const int t[]={2,3,5};int a[SIZE]={1,2,3,4,5,6};main{int n=0;for(int "
                "i=0;i<SIZE;i++){if(i==2)continue;n+=a[i];if(i==4)break;}while(n<20)n++;set_val(VALUE_1,"
                "n);set_val(VALUE_2,factorial(6));set_val(VALUE_3,t[2]);set_val(VALUE_4,2147483647+1);"
                "set_val(VALUE_5,-0x80000000);set_val(VALUE_6,0x80000000/"
                "-1);set_val(VALUE_7,0x80000000%-1);set_val(VALUE_8,0x80000000>>31);set_val(VALUE_9,1<<"
                "31);set_val(VALUE_10,1<<33);}function factorial(n){return n<=1?1:n*factorial(n-1);}");
            const int expected[]{20, 720, 5, INT_MIN, INT_MIN, INT_MIN, 0, -1, INT_MIN, 2};
            for (int i = 0; i < 10; ++i)
                Check(f.Values[208 + i] == expected[i], "operator/function result " + std::to_string(i));
        });
        Test("assignment ordering and short circuit", [] {
            const auto f = Run(
                "int count;int list[4];main{int "
                "a=2,b=4,x=0;list[x++]=a++;list[x++]+=++b;0&&++count;1||++count;count+=a?7:1/"
                "0;set_val(VALUE_1,list[0]);set_val(VALUE_2,list[1]);set_val(VALUE_3,x);set_val(VALUE_4,"
                "a);set_val(VALUE_5,count);a<<=2;a|=1;a^=3;a&=12;set_val(VALUE_6,a);}");
            const int e[]{2, 5, 2, 3, 7, 12};
            for (int i = 0; i < 6; ++i)
                Check(f.Values[208 + i] == e[i], "side effects");
        });
        Test("state reset and independent ownership", [] {
            const auto p = ScriptCompiler::Compile("int n;init{n=40;}main{set_val(VALUE_1,++n);}");
            auto a = p->CreateInstance(), b = p->CreateInstance();
            auto f = Frame();
            a->Execute(f, 1000, 1);
            Check(f.Values[208] == 41, "init");
            a->Execute(f, 2000, 1);
            Check(f.Values[208] == 42, "persistent global");
            b->Execute(f, 2000, 1);
            Check(f.Values[208] == 41, "independent instance");
            a->Reset();
            a->Execute(f, 0, 0);
            Check(f.Values[208] == 41, "reset");
        });
        Test("edges timing double tap and invalid clock history", [] {
            auto vm =
                ScriptCompiler::Compile(
                    "main{set_val(VALUE_1,event_press(SOUTH));set_val(VALUE_2,event_release(SOUTH));"
                    "set_val(VALUE_3,double_tap(SOUTH,250));set_val(VALUE_4,get_ptime(SOUTH));}")
                    ->CreateInstance();
            auto f = Frame();
            vm->Execute(f, 0, 0);
            f.Physical[10] = 100;
            vm->Execute(f, 1000, 1);
            Check(f.Values[208] == 1 && f.Values[210] == 0, "first press");
            vm->Execute(f, 2000, 1);
            Check(f.Values[208] == 0, "held no edge");
            f.Physical[10] = 0;
            vm->Execute(f, 50000, 48);
            Check(f.Values[209] == 1, "release");
            f.Physical[10] = 100;
            vm->Execute(f, 251000, 201);
            Check(f.Values[210] == 1, "tap inclusive exact window");
            f.Physical[10] = 0;
            vm->Execute(f, 252000, 1);
            f.Physical[10] = 100;
            vm->Execute(f, 501001, 249);
            Check(f.Values[210] == 0, "tap outside window");
            vm = ScriptCompiler::Compile("main{set_val(VALUE_1,event_press(SOUTH));}")->CreateInstance();
            f = Frame();
            vm->Execute(f, 1000, 0);
            f.Physical[10] = 100;
            for (const auto t : {500LL, 750LL, 999LL}) {
                bool rejected = false;
                try {
                    vm->Execute(f, t, 0);
                } catch (const ScriptException &) {
                    rejected = true;
                }
                Check(rejected, "retrograde remains rejected");
            }
            vm->Execute(f, 1001, 0);
            Check(f.Values[208] == 1, "invalid clock cannot consume real edge");
        });
        Test("combo holds stops additive composition and self restart", [] {
            auto vm =
                ScriptCompiler::Compile("main{if(event_press(SOUTH))combo_run(T);}combo "
                                        "T{add_val(LX,20);add_val(LY,80);add_val(LY,-80);set_val(EAST,"
                                        "100);wait(20);set_val(EAST,0);wait(10);}")
                    ->CreateInstance();
            auto f = Frame();
            f.Physical[0] = 10;
            f.Physical[1] = 80;
            f.Physical[10] = 100;
            vm->Execute(f, 0, 0);
            Check(f.Values[0] == 30 && f.Values[1] == 20 && vm->NextWakeMilliseconds() == 20,
                  "initial additive clip");
            f.Physical[0] = 30;
            f.Physical[1] = -80;
            vm->Execute(f, 10000, 10);
            Check(f.Values[0] == 50 && f.Values[1] == -80, "held offset tracks physical");
            vm->Execute(f, 30000, 20);
            Check(f.Values[0] == 30 && f.Values[11] == 0, "next segment releases old offset");
            vm = ScriptCompiler::Compile(
                     "main{if(event_press(SOUTH))combo_run(Self);}combo "
                     "Self{set_val(EAST,100);wait(1);restart();set_val(LX,99);}function "
                     "restart(){set_val(LY,combo_restart(Self));}")
                     ->CreateInstance();
            for (int i = 0; i < 3000; ++i) {
                vm->Execute(f, i * 1000LL, 1);
                Check(f.Values[11] == 100 && f.Values[0] == 30 && vm->NextWakeMilliseconds() == 1,
                      "self restart stack bounded");
            }
            vm = ScriptCompiler::Compile("main{combo_run(Self);}combo "
                                         "Self{set_val(LX,99);combo_stop(Self);set_val(LY,99);wait(1);}")
                     ->CreateInstance();
            vm->Execute(f, 0, 0);
            Check(f.Values[0] == 30 && f.Values[1] == -80 && vm->NextWakeMilliseconds() == 0,
                  "self stop cancels continuation");
        });
        Test("math all new functions and full int ranges", [] {
            const auto f = Run(
                "main{set_val(VALUE_1,sin(90));set_val(VALUE_2,cos(90));set_val(VALUE_3,sin(270,"
                "0x80000000));set_val(VALUE_4,atan2(-1,-1));set_val(VALUE_5,lerp(0x80000000,2147483647,"
                "50));set_val(VALUE_6,pow(3,5));set_val(VALUE_7,isqrt(99));set_val(VALUE_8,hypot(3,4));"
                "set_val(VALUE_9,set_bit(0,31,1));set_val(VALUE_10,get_bit(0x80000000,31));set_val("
                "VALUE_"
                "11,abs(0x80000000));set_val(VALUE_12,random(-2147483647,2147483647));}");
            const int e[]{100, 0, INT_MAX, -135, -1, 243, 9, 5, INT_MIN, 1, INT_MAX};
            for (int i = 0; i < 11; ++i)
                Check(f.Values[208 + i] == e[i], "math result");
            for (int deg = -720; deg <= 720; deg += 15) {
                const auto v = Run("main{set_val(VALUE_1,sin(" + std::to_string(deg) +
                                   ",70));set_val(VALUE_2,cos(" + std::to_string(deg) + ",70));}");
                Check(std::abs(v.Values[208] - 70 * std::sin(deg * 3.141592653589793 / 180)) <= 0.500001,
                      "sine sweep");
                Check(std::abs(v.Values[209] - 70 * std::cos(deg * 3.141592653589793 / 180)) <= 0.500001,
                      "cosine sweep");
            }
        });
        Test("conditional timers long clock phase and bounds", [] {
            auto vm =
                ScriptCompiler::Compile(
                    "main{if(get_ival(SOUTH)){tick(4);set_val(VALUE_1,wave_sin(1000,40));set_val(VALUE_"
                    "2,wave_cos(1000,30));set_val(VALUE_3,get_phase(1000));}} ")
                    ->CreateInstance();
            auto f = Frame();
            vm->Execute(f, 0, 0);
            Check(vm->NextWakeMilliseconds() == 0, "inactive path idle");
            f.Physical[10] = 100;
            vm->Execute(f, 250000, 250);
            Check(f.Values[208] == 40 && f.Values[209] == 0 && f.Values[210] == 250 &&
                      vm->NextWakeMilliseconds() == 4,
                  "wave quadrant scoped cadence");
            vm->Execute(f, INT64_MAX - 1000000, 1000);
            Check(f.Values[210] == (INT64_MAX - 1000000) / 1000 % 1000, "full monotonic clock phase");
            f.Physical[10] = 0;
            vm->Execute(f, INT64_MAX - 999999, 0);
            Check(vm->NextWakeMilliseconds() == 0, "release drops tick");
        });
        Test("availability transforms and control limits", [] {
            auto f = Frame();
            f.Physical[10] = 100;
            f.Available[21] = false;
            auto vm =
                ScriptCompiler::Compile("main{remap(SOUTH,TOUCH);swap(SOUTH,TOUCH);set_val(RX,900);set_"
                                        "val(LT,-5);set_val(HAT_1,9);}")
                    ->CreateInstance();
            vm->Execute(f, 0, 0);
            Check(f.Values[10] == 100 && f.Values[2] == 100 && f.Values[4] == 0 && f.Values[192] == 7,
                  "missing endpoints and bounds");
            const auto r = Run("main{set_val(LX,90);add_val(LX,2147483647);set_val(VALUE_1,0x80000000);"
                               "invert(VALUE_1);}");
            Check(r.Values[0] == 100 && r.Values[208] == INT_MAX, "saturated additions and invert");
        });
        Test("compiler rejects unsafe forms and preserves Unicode", [] {
            for (const auto *source :
                 {"", "main{", "main{}main{}", "main{wait(1);}", "main{break;}", "main{continue;}",
                  "int a[-1]={1};main{}", "function main(a){}", "combo main{wait(1);}",
                  "main{int a=1;int a=2;}", "main{unknown=3;}", "main{set_val(SOUTH);}",
                  "main{system(1);}", "main{}combo T{set_val(LX,wait(5));}",
                  "const int a[]={1};main{a[0]=2;}", "main{for(wait(5);0;){}}"})
                CompileError(source);
            const auto f = Run(u8"int κόσμος=7;main{int 漢字=5;set_val(VALUE_1,κόσμος+漢字);}");
            Check(f.Values[208] == 12, "UTF-8 identifiers");
            CompileError("main{set_val(VALUE_1," + std::string(150, '(') + "1" + std::string(150, ')') +
                             ");}",
                         "nesting");
        });
        Test("runtime failures restore every physical value", [] {
            RuntimeError("main{set_val(LX,99);while(1){}}", "budget");
            RuntimeError("int a[2];main{a[2]=1;}", "Array index");
            RuntimeError("main{set_val(VALUE_1,3/0);}", "Division");
            RuntimeError("main{f();}function f(){return f();}", "depth");
            RuntimeError("main{combo_run(Self);}combo Self{combo_restart(Self);}", "budget");
            RuntimeError("main{tick(0);}", "tick interval");
            RuntimeError("main{wave_sin(0,30);}", "Wave period");
            RuntimeError("main{double_tap(SOUTH,0);}", "Double-tap window");
        });
        Test("parallel independent script instances", [] {
            const auto p = ScriptCompiler::Compile("int n;main{set_val(VALUE_1,++n);}");
            std::atomic<bool> failed{false};
            std::vector<std::thread> workers;
            for (int i = 0; i < 16; ++i)
                workers.emplace_back([&] {
                    auto vm = p->CreateInstance();
                    auto f = Frame();
                    for (int n = 1; n <= 2000; ++n) {
                        vm->Execute(f, n * 1000LL, 1);
                        if (f.Values[208] != n)
                            failed = true;
                    }
                });
            for (auto &t : workers)
                t.join();
            Check(!failed, "32,000 isolated concurrent frames");
        });
        Test("physical history release duration and missing hats", [] {
            auto vm =
                ScriptCompiler::Compile(
                    "main{set_val(VALUE_1,event_press(HAT_1));set_val(VALUE_2,get_ival(HAT_1));set_val("
                    "VALUE_3,get_lval(HAT_1));set_val(VALUE_4,is_available(HAT_1));set_val(VALUE_5,get_"
                    "brtime(SOUTH));set_val(VALUE_6,get_rtime());}")
                    ->CreateInstance();
            auto f = Frame();
            f.Available[192] = false;
            f.Physical[192] = 0;
            vm->Execute(f, 0, 0);
            Check(f.Values[208] == 0 && f.Values[209] == -1 && f.Values[210] == -1 && f.Values[211] == 0,
                  "missing hat is neutral without phantom edge");
            f.Available[192] = true;
            f.Physical[192] = -1;
            vm->Execute(f, 1000, 1);
            Check(f.Values[208] == 0, "new neutral hat has no edge");
            f.Physical[192] = 2;
            f.Physical[10] = 100;
            vm->Execute(f, 100000, 99);
            Check(f.Values[208] == 1 && f.Values[209] == 2 && f.Values[210] == -1 &&
                      f.Values[212] == 100 && f.Values[213] == 99,
                  "hat and saved release duration");
            f.Physical[10] = 0;
            vm->Execute(f, 130000, 30);
            vm->Execute(f, 160000, 30);
            Check(f.Values[212] == 30, "released duration advances");
        });
        Test("combo ordering explicit restart stop loops and cleanup", [] {
            auto vm = ScriptCompiler::Compile(
                          "main{if(event_press(SOUTH))combo_run(T);if(event_press(WEST))combo_"
                          "restart(T);if(event_press(NORTH))combo_stop(T);set_val(VALUE_1,"
                          "combo_running(T));}combo T{set_val(EAST,100);wait(50);}")
                          ->CreateInstance();
            auto f = Frame();
            f.Physical[10] = 100;
            vm->Execute(f, 0, 0);
            Check(f.Values[11] == 100 && f.Values[208] == 1, "combo state");
            f.Physical[12] = 100;
            vm->Execute(f, 20000, 20);
            Check(vm->NextWakeMilliseconds() == 50, "restart extends deadline");
            f.Physical[13] = 100;
            vm->Execute(f, 30000, 10);
            Check(f.Values[11] == 0 && vm->NextWakeMilliseconds() == 0, "stop releases");
            vm = ScriptCompiler::Compile(
                     "main{if(event_press(SOUTH)){combo_run(First);combo_run(Second);}}"
                     "combo First{set_val(EAST,100);wait(100);}combo "
                     "Second{wait(10);combo_stop_all();set_val(LX,99);}")
                     ->CreateInstance();
            vm->Execute(f, 0, 0);
            vm->Execute(f, 10000, 10);
            Check(vm->NextWakeMilliseconds() == 1 && f.Values[0] == 0,
                  "cross-combo quiet cleanup requested");
            vm->Execute(f, 11000, 1);
            Check(f.Values[11] == 0 && vm->NextWakeMilliseconds() == 0, "cross-combo cleanup goes idle");
            vm = ScriptCompiler::Compile("main{if(event_press(SOUTH))combo_run(Sequence);}function "
                                         "output(n){set_val(EAST,n);}combo Sequence{int "
                                         "i;for(i=1;i<=3;i++){output(i*20);wait(5);}}")
                     ->CreateInstance();
            for (int n = 0; n < 4; ++n) {
                vm->Execute(f, n * 5000LL, 5);
                Check(f.Values[11] == (n < 3 ? (n + 1) * 20 : 0), "combo locals survive yielding loops");
            }
        });
        Test("block deadlines scoped ticks and persistent legacy cadence", [] {
            auto f = Frame();
            f.Physical[10] = 100;
            auto vm = ScriptCompiler::Compile("main{if(event_press(SOUTH))block(SOUTH,5000);}")
                          ->CreateInstance();
            vm->Execute(f, 0, 0);
            Check(f.Values[10] == 0 && vm->NextWakeMilliseconds() == 5000, "block uses one deadline");
            vm->Execute(f, 5000000, 5000);
            Check(f.Values[10] == 100 && vm->NextWakeMilliseconds() == 0,
                  "block expiration restores without polling");
            vm = ScriptCompiler::Compile(
                     "init{vm_tctrl(10);}main{if(get_ival(SOUTH)){tick(10);tick(4);tick("
                     "8);get_runtime();if(event_press(SOUTH))block(EAST,2);}}")
                     ->CreateInstance();
            vm->Execute(f, 0, 0);
            Check(vm->NextWakeMilliseconds() == 2, "earlier block deadline wins");
            vm->Execute(f, 2000, 2);
            Check(vm->NextWakeMilliseconds() == 4, "fastest scoped tick wins");
            f.Physical[10] = 0;
            vm->Execute(f, 4000, 2);
            Check(vm->NextWakeMilliseconds() == 10, "legacy cadence remains sticky");
        });
        Test("persistent values deterministic random and native identifiers", [] {
            const auto p = ScriptCompiler::Compile(
                "main{set_val(VALUE_1,get_pvar(0,0,100,50));set_pvar(0,70);set_val(VALUE_2,rand(-"
                "2147483647,"
                "2147483647));set_val(VALUE_3,get_controller());set_val(VALUE_4,get_device());}");
            auto a = p->CreateInstance(), b = p->CreateInstance();
            auto f = Frame(), g = Frame();
            f.Family = g.Family = 6;
            f.DeviceKey = g.DeviceKey = 0x80000000;
            for (int i = 0; i < 100; ++i) {
                a->Execute(f, i * 1000LL, 1);
                b->Execute(g, i * 1000LL, 1);
                Check(f.Values == g.Values, "per-instance deterministic random");
                Check(f.Values[208] == (i ? 70 : 50) && f.Values[210] == 6 && f.Values[211] == INT_MIN,
                      "session state and full device key");
            }
            a->Reset();
            a->Execute(f, 0, 0);
            Check(f.Values[208] == 50, "reset clears session pvars");
        });
        Test("ordered affine combo clipping reference property", [] {
            std::mt19937 random(8719);
            std::array<int, 24> values{};
            std::array<bool, 24> set{};
            for (int sample = 0; sample < 150; ++sample) {
                const int control = sample % 2 == 0 ? 0 : 208, low = control == 0 ? -100 : INT_MIN,
                          high = control == 0 ? 100 : INT_MAX;
                std::string code = "main{if(event_press(SOUTH))combo_run(T);}combo T{";
                for (int op = 0; op < 24; ++op) {
                    set[op] = random() % 5 == 0;
                    values[op] = op % 7 == 0 ? (op % 2 == 0 ? INT_MAX : INT_MIN)
                                             : static_cast<int>(random() % 401) - 200;
                    code += (set[op] ? "set_val(" : "add_val(") + std::to_string(control) + "," +
                            std::to_string(values[op]) + ");";
                }
                code += "wait(100);}";
                auto vm = ScriptCompiler::Compile(code)->CreateInstance();
                auto f = Frame();
                f.Physical[10] = 100;
                for (int pass = 0; pass < 20; ++pass) {
                    f.Physical[control] = control == 0 ? static_cast<int>(random() % 201) - 100
                                                       : static_cast<int>(random() % 2001) - 1000;
                    std::int64_t expected = f.Physical[control];
                    for (int op = 0; op < 24; ++op)
                        expected = std::clamp<std::int64_t>(set[op] ? values[op] : expected + values[op],
                                                            low, high);
                    vm->Execute(f, pass * 1000LL, 1);
                    Check(f.Values[control] == expected, "ordered affine vs sequential clamps");
                }
            }
        });
        Test("remap preserves destination input", [] {
            for (const auto &names : std::vector<std::pair<std::string, std::string>>{
                     {"A", "B"}, {"CROSS", "CIRCLE"}, {"BUTTON_1", "BUTTON_2"}}) {
                const auto a = Controls::Parse(names.first), b = Controls::Parse(names.second);
                auto vm = ScriptCompiler::Compile("main{remap(" + names.first + "," + names.second + ");}")
                              ->CreateInstance();
                std::int64_t now = 0;
                for (const auto state : std::vector<std::array<int, 3>>{
                         {0, 0, 0}, {100, 0, 100}, {100, 100, 100}, {0, 100, 100}, {0, 0, 0},
                         {0, 100, 100}, {100, 100, 100}, {100, 0, 100}, {0, 0, 0},
                         {35, 80, 80}, {80, 35, 80}}) {
                    auto f = Frame();
                    f.Physical[a] = state[0];
                    f.Physical[b] = state[1];
                    vm->Execute(f, now += 4000, 4);
                    Check(f.Values[a] == 0 && f.Values[b] == state[2],
                          names.first + " remap preserves physical target and both release orders");
                    Check(f.Physical[a] == state[0] && f.Physical[b] == state[1],
                          "remapping never changes recorded physical input");
                    Check(vm->NextWakeMilliseconds() == 0, "button remap adds no timed polling");
                }
                for (const bool sourceMissing : {false, true}) {
                    auto f = Frame();
                    f.Physical[a] = 100;
                    f.Physical[b] = 100;
                    f.Available[sourceMissing ? a : b] = false;
                    vm->Execute(f, now += 4000, 4);
                    Check(f.Values[a] == 100 && f.Values[b] == 100,
                          "missing remap endpoints cannot consume either input");
                }
            }
            auto vm = ScriptCompiler::Compile("main{remap(SOUTH,EAST);remap(WEST,EAST);}")->CreateInstance();
            for (const int a : {0, 100})
                for (const int b : {0, 100})
                    for (const int x : {0, 100}) {
                        auto f = Frame();
                        f.Physical[10] = a;
                        f.Physical[11] = b;
                        f.Physical[12] = x;
                        vm->Execute(f, 0, 0);
                        Check(f.Values[10] == 0 && f.Values[12] == 0 &&
                                  f.Values[11] == (a || b || x ? 100 : 0),
                              "multiple button sources combine without cancelling one another");
                    }
            auto f = Frame();
            f.Physical[10] = 100;
            ScriptCompiler::Compile("main{remap(SOUTH,SOUTH);}")->CreateInstance()->Execute(f, 0, 0);
            Check(f.Values[10] == 100, "remapping to the same control leaves it usable");
            f = Frame();
            f.Physical[5] = 85;
            f.Physical[4] = 25;
            ScriptCompiler::Compile("main{remap(LT,RT);}")->CreateInstance()->Execute(f, 0, 0);
            Check(f.Values[4] == 0 && f.Values[5] == 85, "trigger remapping preserves the stronger target");
            f = Frame();
            f.Physical[0] = 0;
            f.Physical[2] = -45;
            ScriptCompiler::Compile("main{remap(LX,RX);}")->CreateInstance()->Execute(f, 0, 0);
            Check(f.Values[2] == -45, "a centered source axis leaves the target axis usable");
            f.Physical[0] = -30;
            ScriptCompiler::Compile("main{remap(LX,RX);}")->CreateInstance()->Execute(f, 0, 0);
            Check(f.Values[0] == 0 && f.Values[2] == -30, "an active signed source keeps its direction");
            f = Frame();
            f.Physical[193] = 4;
            ScriptCompiler::Compile("main{remap(HAT_1,HAT_2);}")->CreateInstance()->Execute(f, 0, 0);
            Check(f.Values[192] == -1 && f.Values[193] == 4,
                  "a neutral source hat cannot erase the target direction");
            f.Physical[192] = 0;
            ScriptCompiler::Compile("main{remap(HAT_1,HAT_2);}")->CreateInstance()->Execute(f, 0, 0);
            Check(f.Values[192] == -1 && f.Values[193] == 0, "hat north is active even though its value is zero");
            auto earlier = Run("main{set_val(EAST,100);remap(SOUTH,EAST);}");
            Check(earlier.Values[11] == 100, "an idle source preserves an earlier target modification");
            vm = ScriptCompiler::Compile("init{combo_run(Map);} main{} combo Map{remap(SOUTH,EAST);wait(50);}")
                     ->CreateInstance();
            f = Frame();
            f.Physical[11] = 100;
            vm->Execute(f, 0, 0);
            Check(f.Values[11] == 100, "an inactive remap inside a combo preserves target input");
            f.Physical[11] = 0;
            vm->Execute(f, 10000, 10);
            Check(f.Values[11] == 0, "an inactive combo remap cannot hold the physical target button");
        });
        Test("control-specific transform saturation", [] {
            auto f = Frame();
            f.Physical[0] = 55;
            f.Physical[1] = -80;
            f.Physical[10] = 100;
            auto vm = ScriptCompiler::Compile(
                          "main{remap(SOUTH,EAST);deadzone(LX,10);sensitivity(LY,50);invert(LY);}")
                          ->CreateInstance();
            vm->Execute(f, 0, 0);
            Check(f.Values[10] == 0 && f.Values[11] == 100 && f.Values[0] == 50 && f.Values[1] == 40,
                  "remap/deadzone/sensitivity/invert");
            vm = ScriptCompiler::Compile(
                     "main{add_val(LX,30);add_val(LX,-10);add_val(LY,2147483647);add_val(RX,0x80000000);"
                     "add_"
                     "val(LT,0x80000000);add_val(VALUE_1,2147483647);add_val(VALUE_2,0x80000000);invert("
                     "VALUE_3);set_val(RY,90);add_val(RY,40);add_val(RY,-50);}")
                     ->CreateInstance();
            f.Physical[0] = 10;
            f.Physical[1] = -20;
            f.Physical[2] = 100;
            f.Physical[4] = 100;
            f.Physical[208] = 100;
            f.Physical[209] = -100;
            f.Physical[210] = INT_MIN;
            vm->Execute(f, 0, 0);
            Check(f.Values[0] == 30 && f.Values[1] == 100 && f.Values[2] == -100 && f.Values[4] == 0 &&
                      f.Values[208] == INT_MAX && f.Values[209] == INT_MIN && f.Values[210] == INT_MAX &&
                      f.Values[3] == 50,
                  "all signed and normalized saturation cases");
            vm->Execute(f, 4000, 4);
            Check(f.Values[0] == 30 && vm->NextWakeMilliseconds() == 0,
                  "no additive drift or idle polling");
        });
        Test("bounded locals arity blocks reserved names and all argument checks", [] {
            const auto f =
                Run("main{set_val(VALUE_1,accumulate(20));}function accumulate(n){int "
                    "padding[240];padding[0]=n;if(n==0)return 0;return padding[0]+accumulate(n-1);}");
            Check(f.Values[208] == 210, "large recursive caller frames");
            Check(Run("function first(){return second();}function second(){return "
                      "42;}main{set_val(VALUE_1,first());}")
                          .Values[208] == 42,
                  "forward zero-local functions");
            for (const auto *k : {"define", "const", "int", "main", "init", "combo", "function", "if",
                                  "else", "while", "for", "break", "continue", "return", "wait"}) {
                CompileError("int " + std::string(k) + ";main{}");
                CompileError("function " + std::string(k) + "(){}main{}");
            }
            std::string blocks = "main{}";
            for (int i = 0; i < 94; ++i)
                blocks += "function f" + std::to_string(i) + "(){}";
            (void)ScriptCompiler::Compile(blocks);
            CompileError(blocks + "function overflow(){}", "At most 96");
            for (const auto *invalid : {"sin()", "cos(1,2,3)", "add_val(LX)", "atan2(1)"})
                CompileError("main{" + std::string(invalid) + ";}");
            for (const auto *call :
                 {"lerp(0,100,101)", "lerp(0,100,-1)", "tick(21)", "get_phase(-1)", "get_bit(0,32)",
                  "set_bit(0,0,2)", "get_pvar(64,0,1,0)", "set_pvar(64,1)", "get_pvar(0,1,0,0)",
                  "pow(2,32)", "isqrt(-1)", "random(1,0)", "deadzone(LX,100)", "sensitivity(LX,1001)",
                  "set_val(256,1)", "block(LX,-1)"}) {
                auto v = Frame();
                auto vm = ScriptCompiler::Compile("main{" + std::string(call) + ";}")->CreateInstance();
                bool rejected = false;
                try {
                    vm->Execute(v, 0, 0);
                } catch (const ScriptException &) {
                    rejected = true;
                }
                Check(rejected && v.Values == v.Physical, "builtin argument failure is fail-open");
            }
        });
        Test("bounded malformed syntax and very large expression teardown", [] {
            std::mt19937 random(633);
            const char *tokens[]{"main", "{",        "}",     "int",    "a",    ";",     "=",
                                 "1",    "if",       "(",     ")",      "wait", "[",     "]",
                                 "+",    "function", "combo", "return", "for",  "define"};
            for (int i = 0; i < 3000; ++i) {
                std::string text;
                const unsigned count = random() % 79 + 1;
                for (unsigned n = 0; n < count; ++n)
                    text += std::string(tokens[random() % 20]) + " ";
                try {
                    (void)ScriptCompiler::Compile(text);
                } catch (const ScriptException &) {
                }
                Check(true, "bounded malformed fuzz");
            }
            std::string flat = "main{set_val(VALUE_1,1";
            for (int i = 0; i < 30000; ++i)
                flat += "+1";
            CompileError(flat + ");}", "nesting");
            CompileError(std::string(524289, ' '), "512 KiB");
        });
        Test("single instance rejects simultaneous execute or reset", [] {
            auto vm = ScriptCompiler::Compile("main{while(1){}}")->CreateInstance();
            vm->SetInstructionBudget(100000);
            std::atomic<int> collisions{0};
            std::atomic<bool> begin{false};
            auto worker = [&] {
                auto f = Frame();
                while (!begin.load(std::memory_order_acquire))
                    std::this_thread::yield();
                for (int n = 0; n < 40; ++n) {
                    try {
                        vm->Execute(f, 0, 0);
                    } catch (const std::logic_error &) {
                        ++collisions;
                    } catch (const ScriptException &) {
                    }
                }
            };
            std::thread first(worker), second(worker);
            begin.store(true, std::memory_order_release);
            first.join();
            second.join();
            Check(collisions > 0, "concurrent VM access is rejected");
            vm->Reset();
            Check(vm->InstructionBudget() == 100000 && vm->NextWakeMilliseconds() == 0,
                  "instance resets after concurrent rejection");
        });
        Test("frozen managed VM differential parity", [] {
            int index = 0;
            for (const auto &item : CoreParityCases) {
                std::shared_ptr<const ScriptProgram> p;
                try {
                    p = ScriptCompiler::Compile(item.Source);
                } catch (const ScriptException &) {
                }
                Check(static_cast<bool>(p) == item.Compiles,
                      "reference compile parity case " + std::to_string(index));
                if (p) {
                    auto vm = p->CreateInstance();
                    std::uint32_t random = 633;
                    std::int64_t time = 0;
                    for (int n = 0; n < 16; ++n) {
                        const int ms = n == 0 ? 0 : n % 4 == 0 ? 173 : 4;
                        time += ms * 1000LL;
                        auto f = ParityFrame(n, random);
                        bool fault = false;
                        try {
                            vm->Execute(f, time, ms);
                        } catch (const ScriptException &) {
                            fault = true;
                        }
                        Check(fault == item.Faults[n], "reference fault parity case " +
                                                           std::to_string(index) + " frame " +
                                                           std::to_string(n));
                        Check(Hash(f, vm->NextWakeMilliseconds()) == item.Hashes[n],
                              "reference output/timer parity case " + std::to_string(index) + " frame " +
                                  std::to_string(n));
                    }
                }
                ++index;
            }
        });
        if (!scripts.empty()) {
            Test("shipped additive figure-eight geometry and moving center", [&] {
                auto vm = ScriptCompiler::Compile(
                              Read(std::filesystem::u8path(scripts) / "07 - Additive Figure Eight.igc"))
                              ->CreateInstance();
                auto f = Frame();
                f.Physical[0] = 10;
                f.Physical[1] = -20;
                vm->Execute(f, 0, 0);
                Check(f.Values[0] == 10 && f.Values[1] == -20 && vm->NextWakeMilliseconds() == 0,
                      "idle physical center");
                f.Physical[10] = f.Physical[11] = 100;
                f.Physical[2] = 51;
                vm->Execute(f, 730000, 730);
                Check(f.Values[0] == 10 && f.Values[1] == -20 && vm->NextWakeMilliseconds() == 4,
                      "arbitrary-time activation starts at center");
                vm->Execute(f, 880000, 150);
                Check(f.Values[0] == 31 && f.Values[1] == 0, "eighth-cycle diagonal");
                vm->Execute(f, 1030000, 150);
                Check(f.Values[0] == 40 && f.Values[1] == -20, "quarter-cycle peak");
                f.Physical[0] = -60;
                f.Physical[1] = 70;
                vm->Execute(f, 1030000, 0);
                Check(f.Values[0] == -30 && f.Values[1] == 70, "moving physical center");
                f.Physical[0] = 95;
                vm->Execute(f, 1030000, 0);
                Check(f.Values[0] == 100, "clip at valid axis bound");
                f.Physical[10] = 0;
                vm->Execute(f, 1031000, 1);
                Check(f.Values[0] == 95 && f.Values[1] == 70 && vm->NextWakeMilliseconds() == 0,
                      "condition release returns to idle");
                f.Physical[10] = 100;
                vm->Execute(f, 1040000, 9);
                Check(f.Values[0] == 95 && f.Values[1] == 70, "reactivation resets origin");
                f.Physical[2] = 50;
                vm->Execute(f, 1041000, 1);
                Check(vm->NextWakeMilliseconds() == 0, "strict threshold release");
                f.Physical[2] = 80;
                f.Available[1] = false;
                vm->Execute(f, 1042000, 1);
                Check(vm->NextWakeMilliseconds() == 0, "missing second axis blocks motion");
            });
            Test("shipped generic HID orbit uses descriptor bindings", [&] {
                auto vm = ScriptCompiler::Compile(
                              Read(std::filesystem::u8path(scripts) / "08 - Generic HID Orbit.igc"))
                              ->CreateInstance();
                auto f = Frame();
                f.Physical[32] = f.Physical[33] = 100;
                f.Physical[98] = 80;
                f.Physical[96] = 10;
                f.Physical[97] = -20;
                vm->Execute(f, 0, 0);
                vm->Execute(f, 400000, 400);
                Check(f.Values[96] == 35 && f.Values[97] == -20 && vm->NextWakeMilliseconds() == 4,
                      "generic descriptor orbit");
                f.Available[98] = false;
                vm->Execute(f, 401000, 1);
                Check(f.Values[96] == 10 && f.Values[97] == -20 && vm->NextWakeMilliseconds() == 0,
                      "unavailable generic camera releases motion");
            });
            Test("all shipped IGC scripts compile and execute", [&] {
                int count = 0;
                for (const auto &entry :
                     std::filesystem::directory_iterator(std::filesystem::u8path(scripts)))
                    if (entry.path().extension() == ".igc") {
                        auto vm = ScriptCompiler::Compile(Read(entry.path()))->CreateInstance();
                        auto f = Frame();
                        f.Physical[0] = 35;
                        f.Physical[1] = -50;
                        f.Physical[2] = 80;
                        f.Physical[10] = f.Physical[11] = 100;
                        vm->Execute(f, 0, 0);
                        vm->Execute(f, 5000, 5);
                        f.Physical[10] = 0;
                        vm->Execute(f, 500000, 495);
                        vm->Reset();
                        ++count;
                        Check(true, "shipped script");
                    }
                Check(count == 10, "ten shipped scripts, including a custom script interface");
            });
        }
        auto vm = ScriptCompiler::Compile(
                      "main{remap(SOUTH,EAST);invert(LY);deadzone(LX,10);sensitivity(RX,80);if("
                      "get_ival(LB)){add_val(LX,sin(get_phase(1200)*360/1200,30));tick(4);}}")
                      ->CreateInstance();
        auto f = Frame();
        f.Physical[0] = 35;
        f.Physical[1] = -60;
        f.Physical[2] = 50;
        f.Physical[10] = f.Physical[14] = 100;
        for (int i = 0; i < 10000; ++i)
            vm->Execute(f, i * 4000LL, 4);
        const auto start = std::chrono::steady_clock::now();
        const auto before = allocations.load();
        bool bounded = true;
        for (int i = 0; i < 202000; ++i) {
            vm->Execute(f, (i + 10000) * 4000LL, 4);
            bounded &= f.Values[0] >= -100 && f.Values[0] <= 100 && vm->NextWakeMilliseconds() == 4;
        }
        const auto vmAllocations = allocations.load() - before;
        const double average =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() /
            202000;
        Check(bounded, "202,000 stress frames remain bounded");
        Check(vmAllocations == 0, "VM hot path must allocate no memory");
        std::ostringstream json;
        json << "{\"passed\":true,\"tests\":" << tests << ",\"checks\":" << checks
             << ",\"stress_frames\":202000,\"vm_allocations\":" << vmAllocations
             << ",\"reference_cases\":" << std::size(CoreParityCases) << ",\"average_us\":" << average
             << ",\"scope\":\"native VM software cost, not hardware latency\"}";
        if (!jsonPath.empty()) {
            std::ofstream file(std::filesystem::u8path(jsonPath));
            file << json.str();
            if (!file)
                throw std::runtime_error("Cannot write test JSON.");
        }
        std::cout << json.str() << '\n';
        return 0;
    } catch (const ScriptException &e) {
        std::cerr << e.Diagnostic() << '\n';
        return 1;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
