using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using GamePadRemapper.Core;

internal static class Program
{
    private static int checks, tests; private static string current;
    private static void Check(bool value, string detail) { checks++; if (!value) throw new Exception(current + ": " + detail); }
    private static ControlFrame Frame()
    {
        var result = new ControlFrame(); for (int i = 0; i < 256; i++) result.Available[i] = true; for (int i = 192; i < 208; i++) result.Physical[i] = -1; return result;
    }
    private static void Test(string name, Action action) { current = name; action(); tests++; Console.WriteLine("PASS " + name); }
    private static void CompileError(string source, string fragment = null)
    {
        try { ScriptCompiler.Compile(source); throw new Exception("Compile accepted invalid source: " + source); }
        catch (ScriptException error) { Check(error.Line > 0 && error.Column > 0, "valid source location"); if (fragment != null) Check(error.Message.Contains(fragment), error.Message); }
    }
    private static void RuntimeError(string source, string fragment)
    {
        var frame = Frame(); frame.Physical[0] = 31;
        try { ScriptCompiler.Compile(source).CreateInstance().Execute(frame, 0, 0); throw new Exception("Runtime accepted unsafe script"); }
        catch (ScriptException error) { Check(error.Message.Contains(fragment), error.Message); for (int i = 0; i < 256; i++) Check(frame.Values[i] == frame.Physical[i], "fault restored physical output " + i); }
    }
    private static ControlFrame Run(string source)
    { var frame = Frame(); ScriptCompiler.Compile(source).CreateInstance().Execute(frame, 0, 0); return frame; }
    private static string ScriptsPath(string[] args)
    {
        string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Scripts");
        for (int i = 0; i + 1 < args.Length; i++) if (args[i] == "--scripts") path = Path.GetFullPath(args[i + 1]);
        if (!Directory.Exists(path)) throw new DirectoryNotFoundException("Shipped scripts are missing: " + path + ". Supply --scripts path for a source-tree test run.");
        return path;
    }
    public static int Main(string[] args)
    {
        try
        {
            Test("identity and names", () =>
            {
                var frame = Frame(); var engine = ScriptCompiler.Compile("main { }").CreateInstance(); var random = new Random(7331);
                for (int round = 0; round < 256; round++)
                {
                    for (int i = 0; i < 256; i++) frame.Physical[i] = random.Next(-100, 101);
                    engine.Execute(frame, round * 1000L, 1); for (int i = 0; i < 256; i++) Check(frame.Values[i] == frame.Physical[i], "identity"); Check(engine.NextWakeMilliseconds == 0, "idle no timer");
                }
                for (int i = 0; i < 256; i++) Check(Controls.Parse(Controls.Names[i]) == i, "canonical names roundtrip");
                Check(Controls.Parse("PS4_CROSS") == 10 && Controls.Parse("XB1_A") == 10 && Controls.Parse("PS5_R2") == 5, "aliases"); Check(Controls.Parse("INVALID") == -1, "unknown control");
            });
            Test("operators and precedence", () =>
            {
                var frame = Run("main { set_val(VALUE_1, 2+3*4); set_val(VALUE_2, (2+3)*4); set_val(VALUE_3, 0x10 | 3 << 1); set_val(VALUE_4, ~0 & 255); set_val(VALUE_5, 10 % 3); set_val(VALUE_6, -7/2); set_val(VALUE_7, (2<3)+(3<=3)+(4>3)+(4>=4)+(3==3)+(4!=3)); }");
                int[] expected = { 14, 20, 22, 255, 1, -3, 6 }; for (int i = 0; i < expected.Length; i++) Check(frame.Values[208 + i] == expected[i], "operator result " + i);
            });
            Test("assignment side effects and short circuit", () =>
            {
                var frame = Run("int count; int list[4]; main { int a=2,b=4,x=0; list[x++] = a++; list[x++] += ++b; 0 && ++count; 1 || ++count; count += a ? 7 : 1/0; set_val(VALUE_1,list[0]); set_val(VALUE_2,list[1]); set_val(VALUE_3,x); set_val(VALUE_4,a); set_val(VALUE_5,count); a <<= 2; a |= 1; a ^= 3; a &= 12; set_val(VALUE_6,a); }");
                int[] expected = { 2, 5, 2, 3, 7, 12 }; for (int i = 0; i < expected.Length; i++) Check(frame.Values[208 + i] == expected[i], "side effect " + i);
            });
            Test("loops functions arrays and constants", () =>
            {
                var frame = Run("define SIZE = 2*3; const int table[] = {2,3,5}; int sum; int values[SIZE] = {1,2,3,4,5,6}; init { sum=4; } main { for(int i=0;i<SIZE;i++){ if(i==2) continue; sum+=values[i]; if(i==4) break; } int n=3; while(n){sum+=n;n--;} set_val(VALUE_1,sum); set_val(VALUE_2,factorial(6)); set_val(VALUE_3,table[2]); } function factorial(n) { if(n<=1) return 1; return n*factorial(n-1); }");
                Check(frame.Values[208] == 22, "for/while/break/continue"); Check(frame.Values[209] == 720, "recursive functions"); Check(frame.Values[210] == 5, "read-only table");
            });
            Test("instance state and reset", () =>
            {
                var program = ScriptCompiler.Compile("int count; init {count=40;} main { set_val(VALUE_1,++count); }"); var a = program.CreateInstance(); var b = program.CreateInstance(); var frame = Frame();
                a.Execute(frame, 1000, 1); Check(frame.Values[208] == 41, "init"); a.Execute(frame, 2000, 1); Check(frame.Values[208] == 42, "state persists"); b.Execute(frame, 2000, 1); Check(frame.Values[208] == 41, "device isolation"); a.Reset(); a.Execute(frame, 0, 1); Check(frame.Values[208] == 41, "reset");
            });
            Test("physical values edges and timing", () =>
            {
                var script = ScriptCompiler.Compile("main {set_val(VALUE_1,event_press(SOUTH));set_val(VALUE_2,event_release(SOUTH));set_val(VALUE_3,get_ptime(SOUTH));set_val(VALUE_4,get_lval(SOUTH));set_val(VALUE_5,get_rtime()); set_val(SOUTH,0);set_val(VALUE_6,get_ival(SOUTH));set_val(VALUE_7,get_val(SOUTH));set_val(VALUE_8,get_brtime(SOUTH));}").CreateInstance();
                var frame = Frame(); script.Execute(frame, 0, 0); frame.Physical[10] = 100; script.Execute(frame, 100000, 100);
                Check(frame.Values[208] == 1 && frame.Values[209] == 0 && frame.Values[210] == 0, "press edge"); Check(frame.Values[213] == 100 && frame.Values[214] == 0 && frame.Values[215] == 100, "physical independent of output");
                script.Execute(frame, 120000, 20); Check(frame.Values[208] == 0 && frame.Values[210] == 20 && frame.Values[211] == 100 && frame.Values[212] == 20, "held timer tick no duplicate edge");
                frame.Physical[10] = 0; script.Execute(frame, 130000, 10); Check(frame.Values[209] == 1, "release edge"); Check(script.NextWakeMilliseconds == 1, "time builtin schedules evaluation");
                script.Execute(frame, 160000, 30); Check(frame.Values[215] == 30, "release duration");
            });
            Test("remap transforms and bounds", () =>
            {
                var frame = Frame(); frame.Physical[0] = 55; frame.Physical[1] = -80; frame.Physical[10] = 100;
                var instance = ScriptCompiler.Compile("main { remap(SOUTH,EAST); deadzone(LX,10); sensitivity(LY,50); invert(LY); set_val(RX,900); set_val(LT,-5); set_val(HAT_1,9); }").CreateInstance(); instance.Execute(frame, 0, 0);
                Check(frame.Values[10] == 0 && frame.Values[11] == 100, "remap"); Check(frame.Values[0] == 50 && frame.Values[1] == 40, "deadzone/sensitivity/invert"); Check(frame.Values[2] == 100 && frame.Values[4] == 0 && frame.Values[192] == 7, "control bounds");
                instance = ScriptCompiler.Compile("main {swap(SOUTH,EAST);}").CreateInstance(); instance.Execute(frame, 0, 0); Check(frame.Values[11] == 100 && frame.Values[10] == 0, "swap");
            });
            Test("missing HID hats never create phantom edges", () =>
            {
                var engine = ScriptCompiler.Compile("main{set_val(VALUE_1,event_press(HAT_1));set_val(VALUE_2,get_ival(HAT_1));set_val(VALUE_3,get_lval(HAT_1));set_val(VALUE_4,is_available(HAT_1));}").CreateInstance();
                var frame = Frame(); frame.Available[192] = false; frame.Physical[192] = 0; engine.Execute(frame, 0, 0);
                Check(frame.Values[208] == 0 && frame.Values[209] == -1 && frame.Values[210] == -1 && frame.Values[211] == 0, "missing hat is unavailable neutral");
                frame.Available[192] = true; frame.Physical[192] = -1; engine.Execute(frame, 1000, 1); Check(frame.Values[208] == 0, "first real neutral hat has no press");
                frame.Physical[192] = 2; engine.Execute(frame, 2000, 1); Check(frame.Values[208] == 1 && frame.Values[209] == 2 && frame.Values[210] == -1, "real pressed hat edge");
                engine.Execute(frame, 3000, 1); Check(frame.Values[208] == 0 && frame.Values[210] == 2, "hat edge not repeated");
            });
            Test("combo hold release and deadlines", () =>
            {
                var instance = ScriptCompiler.Compile("main {if(event_press(SOUTH)) combo_run(Tap);} combo Tap {set_val(EAST,100);wait(20);set_val(EAST,0);wait(10);}").CreateInstance(); var frame = Frame(); frame.Physical[10] = 100;
                instance.Execute(frame, 0, 0); Check(frame.Values[11] == 100 && instance.NextWakeMilliseconds == 20, "first segment");
                frame.Physical[11] = 45; instance.Execute(frame, 10000, 10); Check(frame.Values[11] == 100 && instance.NextWakeMilliseconds == 10, "hold overrides newer input");
                instance.Execute(frame, 20000, 10); Check(frame.Values[11] == 0 && instance.NextWakeMilliseconds == 10, "second segment");
                instance.Execute(frame, 30000, 10); Check(frame.Values[11] == 45 && instance.NextWakeMilliseconds == 0, "completed macro restores current physical value");
            });
            Test("combo run stop restart isolation", () =>
            {
                var instance = ScriptCompiler.Compile("main { if(event_press(SOUTH)) combo_run(Tap); if(event_press(WEST)) combo_restart(Tap); if(event_press(NORTH)) combo_stop(Tap);set_val(VALUE_1,combo_running(Tap));} combo Tap {set_val(EAST,100);wait(50);}").CreateInstance(); var frame = Frame(); frame.Physical[10] = 100;
                instance.Execute(frame, 0, 0); Check(frame.Values[11] == 100 && frame.Values[208] == 1, "combo running"); frame.Physical[12] = 100; instance.Execute(frame, 20000, 20); Check(instance.NextWakeMilliseconds == 50, "restart extends"); frame.Physical[13] = 100; instance.Execute(frame, 30000, 10); Check(frame.Values[11] == 0 && instance.NextWakeMilliseconds == 0, "stop releases");
            });
            Test("combo loops and function calls", () =>
            {
                var engine = ScriptCompiler.Compile("main {if(event_press(SOUTH)) combo_run(Sequence);} function output(n){set_val(EAST,n);} combo Sequence {int i;for(i=1;i<=3;i++){output(i*20);wait(5);}}").CreateInstance(); var frame = Frame(); frame.Physical[10] = 100;
                engine.Execute(frame, 0, 0); Check(frame.Values[11] == 20, "combo first iteration"); engine.Execute(frame, 5000, 5); Check(frame.Values[11] == 40, "combo second iteration"); engine.Execute(frame, 10000, 5); Check(frame.Values[11] == 60, "combo third iteration"); engine.Execute(frame, 15000, 5); Check(frame.Values[11] == 0 && engine.NextWakeMilliseconds == 0, "combo final release");
            });
            Test("blocking and tick controls", () =>
            {
                var instance = ScriptCompiler.Compile("main {vm_tctrl(4);if(event_press(SOUTH))block(SOUTH,20);}").CreateInstance(); var frame = Frame(); frame.Physical[10] = 100;
                instance.Execute(frame, 0, 0); Check(frame.Values[10] == 0 && instance.NextWakeMilliseconds == 4, "block start"); instance.Execute(frame, 10000, 10); Check(frame.Values[10] == 0, "block continues"); instance.Execute(frame, 20000, 10); Check(frame.Values[10] == 100, "block expires");
                instance = ScriptCompiler.Compile("main {if(get_ptime(SOUTH)>=25 && get_val(SOUTH))set_val(EAST,100);}").CreateInstance(); instance.Execute(frame, 0, 0); instance.Execute(frame, 25000, 25); Check(frame.Values[11] == 100, "held threshold without input report");
            });
            Test("conditional timing avoids idle polling", () =>
            {
                var engine = ScriptCompiler.Compile("int enabled;main{if(event_press(VIEW))enabled=!enabled;if(enabled && get_ptime(SOUTH)>=25)set_val(EAST,100);}function unused(){return get_runtime();}").CreateInstance(); var frame = Frame();
                engine.Execute(frame, 0, 0); Check(engine.NextWakeMilliseconds == 0, "unreachable time builtin does not poll"); frame.Physical[16] = 100; engine.Execute(frame, 1000, 1); Check(engine.NextWakeMilliseconds == 1, "active time-dependent path schedules tick");
                frame.Physical[16] = 0; engine.Execute(frame, 2000, 1); frame.Physical[16] = 100; engine.Execute(frame, 3000, 1); Check(engine.NextWakeMilliseconds == 0, "inactive path releases periodic tick");
                engine = ScriptCompiler.Compile("main{if(event_press(SOUTH))block(SOUTH,5000);}").CreateInstance(); frame.Physical[10] = 100; engine.Execute(frame, 0, 0); Check(engine.NextWakeMilliseconds == 5000, "block uses one deadline rather than polling");
                engine = ScriptCompiler.Compile("init{vm_tctrl(10);}main{}").CreateInstance(); engine.Execute(frame, 0, 0); engine.Execute(frame, 10000, 10); Check(engine.NextWakeMilliseconds == 10, "explicit vm_tctrl keeps chosen periodic cadence");
            });
            Test("math bits deterministic random and pvars", () =>
            {
                var instance = ScriptCompiler.Compile("main {set_val(VALUE_1,abs(-32));set_val(VALUE_2,min(8,3)+max(8,3));set_val(VALUE_3,clamp(19,-3,8));set_val(VALUE_4,pow(3,5));set_val(VALUE_5,isqrt(99));set_val(VALUE_6,hypot(3,4));set_val(VALUE_7,set_bit(0,31,1));set_val(VALUE_8,get_bit(0x80000000,31));set_val(VALUE_9,get_pvar(0,0,100,50));set_pvar(0,70);set_val(VALUE_10,random(-10,10));}").CreateInstance(); var frame = Frame(); instance.Execute(frame, 0, 0);
                int[] expected = { 32, 11, 8, 243, 9, 5, int.MinValue, 1, 50 }; for (int i = 0; i < expected.Length; i++) Check(frame.Values[208 + i] == expected[i], "math result " + i);
                Check(frame.Values[217] >= -10 && frame.Values[217] <= 10, "random bounded"); instance.Execute(frame, 1000, 1); Check(frame.Values[216] == 70, "session pvar");
            });
            Test("additive output composition and saturated signed fields", () =>
            {
                var engine = ScriptCompiler.Compile("main{add_val(LX,30);add_val(LX,-10);add_val(LY,2147483647);add_val(RX,0x80000000);add_val(LT,0x80000000);add_val(VALUE_1,2147483647);add_val(VALUE_2,0x80000000);invert(VALUE_3);set_val(RY,90);add_val(RY,40);add_val(RY,-50);}").CreateInstance();
                var frame = Frame(); frame.Physical[0] = 10; frame.Physical[1] = -20; frame.Physical[2] = 100; frame.Physical[4] = 100; frame.Physical[208] = 100; frame.Physical[209] = -100; frame.Physical[210] = int.MinValue;
                engine.Execute(frame, 0, 0); Check(frame.Values[0] == 30 && frame.Values[1] == 100 && frame.Values[2] == -100, "axis additions saturate without int overflow");
                Check(frame.Values[4] == 0 && frame.Values[208] == int.MaxValue && frame.Values[209] == int.MinValue && frame.Values[210] == int.MaxValue, "trigger and signed generic-field bounds"); Check(frame.Values[3] == 50, "later additions compose against current bounded output");
                engine.Execute(frame, 4000, 4); Check(frame.Values[0] == 30, "new evaluation rebases to physical input without accumulating drift"); Check(engine.NextWakeMilliseconds == 0, "static additions do not poll");
                frame.Physical[0] = -60; engine.Execute(frame, 8000, 4); Check(frame.Values[0] == -40, "physical center changes are followed immediately");
            });
            Test("additive combo holds rebase and preserve ordered clipping", () =>
            {
                var engine = ScriptCompiler.Compile("main{if(event_press(SOUTH))combo_run(Offset);if(event_press(EAST))combo_stop(Offset);}combo Offset{add_val(LX,20);add_val(LY,80);add_val(LY,-80);set_val(RX,40);add_val(RX,15);add_val(RY,20);set_val(RY,-30);add_val(RY,5);wait(100);}").CreateInstance();
                var frame = Frame(); frame.Physical[0] = 10; frame.Physical[1] = 80; frame.Physical[2] = -60; frame.Physical[3] = 70; frame.Physical[10] = 100;
                engine.Execute(frame, 0, 0); Check(frame.Values[0] == 30, "new additive segment is applied exactly once"); Check(frame.Values[1] == 20 && frame.Values[2] == 55 && frame.Values[3] == -25, "ordered clipping and Set/Add composition on initial run");
                frame.Physical[0] = 30; frame.Physical[1] = -80; frame.Physical[2] = -10; frame.Physical[3] = 50; engine.Execute(frame, 10000, 10);
                Check(frame.Values[0] == 50, "held offset follows new physical center"); Check(frame.Values[1] == -80 && frame.Values[2] == 55 && frame.Values[3] == -25, "held affine transform matches sequential actions");
                frame.Physical[0] = 90; frame.Physical[1] = 80; engine.Execute(frame, 20000, 10); Check(frame.Values[0] == 100 && frame.Values[1] == 20, "held clipping is recomputed without drift");
                frame.Physical[11] = 100; engine.Execute(frame, 21000, 1); Check(frame.Values[0] == 90 && frame.Values[1] == 80 && frame.Values[2] == -10 && engine.NextWakeMilliseconds == 0, "stop releases every held action immediately");
                engine.Reset(); frame.Physical[10] = 100; frame.Physical[11] = 0; engine.Execute(frame, 0, 0); Check(frame.Values[0] == 100, "reset clears affine history and restarts cleanly"); engine.Execute(frame, 100000, 100); Check(frame.Values[0] == 90 && engine.NextWakeMilliseconds == 0, "deadline releases additive hold to current physical values");
                engine = ScriptCompiler.Compile("main{set_val(LX,50);if(event_press(SOUTH)){combo_run(First);combo_run(Second);}}combo First{add_val(LX,10);wait(20);}combo Second{add_val(LX,15);wait(20);}").CreateInstance(); engine.Execute(frame, 0, 0); Check(frame.Values[0] == 75, "separate combos compose against current output once"); engine.Execute(frame, 10000, 10); Check(frame.Values[0] == 75, "waiting combos retain the same main/combination ordering");
            });
            Test("additive combo transform matches bounded sequential reference", () =>
            {
                var random = new Random(8719); const int operations = 24; var values = new int[operations]; var isSet = new bool[operations];
                for (int sample = 0; sample < 150; sample++)
                {
                    int control = sample % 2 == 0 ? 0 : 208; int minimum = control == 0 ? -100 : int.MinValue, maximum = control == 0 ? 100 : int.MaxValue;
                    var source = new StringBuilder("main{if(event_press(SOUTH))combo_run(Offset);}combo Offset{");
                    for (int op = 0; op < operations; op++)
                    {
                        isSet[op] = random.Next(5) == 0; values[op] = op % 7 == 0 ? (op % 2 == 0 ? int.MaxValue : int.MinValue) : random.Next(-200, 201);
                        source.Append(isSet[op] ? "set_val(" : "add_val(").Append(control).Append(',').Append(values[op].ToString(CultureInfo.InvariantCulture)).Append(");");
                    }
                    source.Append("wait(100);}"); var engine = ScriptCompiler.Compile(source.ToString()).CreateInstance(); var frame = Frame(); frame.Physical[10] = 100;
                    for (int pass = 0; pass < 20; pass++)
                    {
                        int physical = control == 0 ? random.Next(-100, 101) : random.Next(-1000, 1001); frame.Physical[control] = physical; long expected = physical;
                        for (int op = 0; op < operations; op++) expected = Math.Max(minimum, Math.Min(maximum, isSet[op] ? values[op] : expected + values[op]));
                        engine.Execute(frame, pass * 1000L, pass == 0 ? 0 : 1); Check(frame.Values[control] == expected, "affine/sequential equivalence sample " + sample + " pass " + pass);
                    }
                }
            });
            Test("degree trigonometry interpolation and range validation", () =>
            {
                var frame = Run("main{set_val(VALUE_1,sin(0));set_val(VALUE_2,sin(90));set_val(VALUE_3,sin(180));set_val(VALUE_4,sin(270));set_val(VALUE_5,sin(-90));set_val(VALUE_6,cos(360));set_val(VALUE_7,cos(90));set_val(VALUE_8,sin(450,35));set_val(VALUE_9,sin(30,100));set_val(VALUE_10,cos(-180,-25));set_val(VALUE_11,sin(270,0x80000000));set_val(VALUE_12,sin(90,0x80000000));set_val(VALUE_13,cos(90,2147483647));set_val(VALUE_14,atan2(1,0));set_val(VALUE_15,atan2(0,-1));set_val(VALUE_16,atan2(-1,0));set_val(VALUE_17,atan2(1,1));set_val(VALUE_18,atan2(-1,-1));set_val(VALUE_19,atan2(0,0));set_val(VALUE_20,lerp(-20,80,25));set_val(VALUE_21,lerp(0x80000000,2147483647,100));set_val(VALUE_22,lerp(2147483647,0x80000000,100));set_val(VALUE_23,lerp(0x80000000,2147483647,50));}");
                int[] expected = { 0, 100, 0, -100, -100, 100, 0, 35, 50, 25, int.MaxValue, int.MinValue, 0, 90, 180, -90, 45, -135, 0, 5, int.MaxValue, int.MinValue, -1 };
                for (int i = 0; i < expected.Length; i++) Check(frame.Values[208 + i] == expected[i], "math extension result " + i + " got " + frame.Values[208 + i]);
                for (int degrees = -720; degrees <= 720; degrees += 15)
                {
                    frame = Run("main{set_val(VALUE_1,sin(" + degrees + ",70));set_val(VALUE_2,cos(" + degrees + ",70));}");
                    Check(Math.Abs(frame.Values[208] - 70 * Math.Sin(degrees * Math.PI / 180)) <= 0.500001, "sin angle sweep " + degrees);
                    Check(Math.Abs(frame.Values[209] - 70 * Math.Cos(degrees * Math.PI / 180)) <= 0.500001, "cos angle sweep " + degrees);
                }
                RuntimeError("main{set_val(VALUE_1,lerp(0,100,101));}", "lerp percentage"); RuntimeError("main{set_val(VALUE_1,lerp(0,100,-1));}", "lerp percentage");
                CompileError("main{sin();}"); CompileError("main{cos(1,2,3);}"); CompileError("main{add_val(LX);}"); CompileError("main{atan2(1);}");
            });
            Test("conditional tick cadence deadline composition and idle release", () =>
            {
                var engine = ScriptCompiler.Compile("main{if(get_ival(SOUTH)){tick(4);set_val(LX,sin(get_phase(1200)*360/1200,30));}}function unused(){tick(1);}").CreateInstance(); var frame = Frame();
                engine.Execute(frame, 0, 0); Check(engine.NextWakeMilliseconds == 0, "inactive branch requests no periodic work"); frame.Physical[10] = 100;
                engine.Execute(frame, 100000, 100); Check(engine.NextWakeMilliseconds == 4, "scoped tick overrides automatic clock cadence"); frame.Physical[10] = 0;
                engine.Execute(frame, 101000, 1); Check(engine.NextWakeMilliseconds == 0 && frame.Values[0] == frame.Physical[0], "release immediately removes cadence and transform");
                engine = ScriptCompiler.Compile("main{if(get_ival(SOUTH)){tick(10);tick(4);tick(8);if(event_press(SOUTH))block(EAST,2);}}").CreateInstance(); frame.Physical[10] = 100;
                engine.Execute(frame, 0, 0); Check(engine.NextWakeMilliseconds == 2, "earlier block deadline wins over requested cadence"); engine.Execute(frame, 2000, 2); Check(engine.NextWakeMilliseconds == 4, "fastest executed tick wins");
                engine = ScriptCompiler.Compile("init{vm_tctrl(10);}main{if(get_ival(SOUTH)){get_runtime();tick(4);}}").CreateInstance(); engine.Execute(frame, 0, 0); Check(engine.NextWakeMilliseconds == 4, "active scoped cadence"); frame.Physical[10] = 0; engine.Execute(frame, 4000, 4); Check(engine.NextWakeMilliseconds == 10, "legacy sticky cadence remains compatible");
                RuntimeError("main{tick(0);}", "tick interval"); RuntimeError("main{tick(21);}", "tick interval");
            });
            Test("monotonic wave quadrants phase offsets and long-running clock", () =>
            {
                var engine = ScriptCompiler.Compile("main{set_val(VALUE_1,wave_sin(1000,40));set_val(VALUE_2,wave_cos(1000,30));set_val(VALUE_3,wave_sin(1000,20,90));set_val(VALUE_4,get_phase(1000));tick(4);}").CreateInstance(); var frame = Frame();
                int[] sine = { 0, 40, 0, -40, 0 }, cosine = { 30, 0, -30, 0, 30 };
                for (int i = 0; i < 5; i++) { engine.Execute(frame, i * 250000L, i == 0 ? 0 : 250); Check(frame.Values[208] == sine[i] && frame.Values[209] == cosine[i], "wave quadrant " + i); Check(frame.Values[211] == i * 250 % 1000 && engine.NextWakeMilliseconds == 4, "phase and cadence " + i); }
                engine.Execute(frame, 2147483647000L + 250000, int.MaxValue); int phase = (int)((2147483647000L + 250000) / 1000 % 1000); Check(frame.Values[211] == phase, "phase does not saturate at legacy int32 runtime");
                engine.Execute(frame, long.MaxValue - 1000000, 1000); phase = (int)((long.MaxValue - 1000000) / 1000 % 1000); Check(frame.Values[211] == phase, "phase supports full positive monotonic clock"); Check(Math.Abs(frame.Values[208]) <= 40 && Math.Abs(frame.Values[209]) <= 30, "large clock never overflows waveform");
                engine.Reset(); engine.Execute(frame, 999000, 0); Check(frame.Values[208] == 0 && frame.Values[209] == 30 && frame.Values[210] == 20 && frame.Values[211] == 0, "wave reset starts at new origin");
                RuntimeError("main{wave_sin(0,30);}", "Wave period"); RuntimeError("main{wave_cos(86400001,30);}", "Wave period"); RuntimeError("main{get_phase(-1);}", "Phase period");
            });
            Test("double-tap uses distinct real presses and read-only same-frame checks", () =>
            {
                var engine = ScriptCompiler.Compile("main{set_val(VALUE_1,double_tap(SOUTH,250));set_val(VALUE_2,double_tap(SOUTH,250));}").CreateInstance(); var frame = Frame(); frame.Physical[10] = 100;
                engine.Execute(frame, 0, 0); Check(frame.Values[208] == 0 && frame.Values[209] == 0, "first press at startup is never a double-tap"); engine.Execute(frame, 50000, 50); Check(frame.Values[208] == 0, "held reports are not taps");
                frame.Physical[10] = 0; engine.Execute(frame, 60000, 10); frame.Physical[10] = 100; engine.Execute(frame, 250000, 190); Check(frame.Values[208] == 1 && frame.Values[209] == 1, "inclusive press-to-press window and stable repeated query"); Check(engine.NextWakeMilliseconds == 0, "double-tap is event-driven without idle timer");
                frame.Physical[10] = 0; engine.Execute(frame, 260000, 10); frame.Physical[10] = 100; engine.Execute(frame, 501000, 241); Check(frame.Values[208] == 0, "expired press window");
                frame.Available[10] = false; engine.Execute(frame, 510000, 9); frame.Available[10] = true; engine.Execute(frame, 520000, 10); Check(frame.Values[208] == 0, "unavailable control clears history");
                engine.Reset(); engine.Execute(frame, 0, 0); Check(frame.Values[208] == 0, "reset clears tap history");
                frame.Physical[10] = 0; engine.Execute(frame, 1000, 1); frame.Physical[10] = 100; engine.Execute(frame, 250001, 249); Check(frame.Values[208] == 0, "one microsecond outside the tap window cannot round back inside");
                RuntimeError("main{double_tap(SOUTH,0);}", "Double-tap window"); RuntimeError("main{double_tap(256,20);}", "identifier");
            });
            Test("shipped additive figure-eight follows moving physical center", () =>
            {
                string source = File.ReadAllText(Path.Combine(ScriptsPath(args), "07 - Additive Figure Eight.igc")); var engine = ScriptCompiler.Compile(source).CreateInstance(); var frame = Frame(); frame.Physical[0] = 10; frame.Physical[1] = -20;
                engine.Execute(frame, 0, 0); Check(frame.Values[0] == 10 && frame.Values[1] == -20 && engine.NextWakeMilliseconds == 0, "idle physical passthrough");
                frame.Physical[10] = frame.Physical[11] = 100; frame.Physical[2] = 51; engine.Execute(frame, 730000, 730); Check(frame.Values[0] == 10 && frame.Values[1] == -20 && engine.NextWakeMilliseconds == 4, "activation starts exactly at center at arbitrary time");
                engine.Execute(frame, 880000, 150); Check(frame.Values[0] == 31 && frame.Values[1] == 0, "one-eighth period adds diagonal around (10,-20)"); engine.Execute(frame, 1030000, 150); Check(frame.Values[0] == 40 && frame.Values[1] == -20, "quarter cycle X peak and Y center");
                frame.Physical[0] = -60; frame.Physical[1] = 70; engine.Execute(frame, 1030000, 0); Check(frame.Values[0] == -30 && frame.Values[1] == 70, "new physical center follows without restarting phase");
                frame.Physical[0] = 95; engine.Execute(frame, 1030000, 0); Check(frame.Values[0] == 100, "output clipping respects stick bounds"); frame.Physical[10] = 0; engine.Execute(frame, 1031000, 1); Check(frame.Values[0] == 95 && frame.Values[1] == 70 && engine.NextWakeMilliseconds == 0, "button release is immediate passthrough");
                frame.Physical[10] = 100; engine.Execute(frame, 1040000, 9); Check(frame.Values[0] == 95 && frame.Values[1] == 70, "reactivation resets orbit origin"); frame.Physical[2] = 50; engine.Execute(frame, 1041000, 1); Check(engine.NextWakeMilliseconds == 0 && frame.Values[0] == 95, "strict 50-percent threshold releases");
                frame.Physical[2] = 80; frame.Available[1] = false; engine.Execute(frame, 1042000, 1); Check(engine.NextWakeMilliseconds == 0, "missing second stick axis prevents unsafe transform");
            });
            Test("generic HID orbit availability and descriptor bindings", () =>
            {
                string source = File.ReadAllText(Path.Combine(ScriptsPath(args), "08 - Generic HID Orbit.igc")); var engine = ScriptCompiler.Compile(source).CreateInstance(); var frame = Frame();
                frame.Physical[32] = frame.Physical[33] = 100; frame.Physical[98] = 80; frame.Physical[96] = 10; frame.Physical[97] = -20; engine.Execute(frame, 0, 0); engine.Execute(frame, 400000, 400);
                Check(frame.Values[96] == 35 && frame.Values[97] == -20 && engine.NextWakeMilliseconds == 4, "generic descriptor axes receive additive orbit"); frame.Available[98] = false; engine.Execute(frame, 401000, 1); Check(frame.Values[96] == 10 && frame.Values[97] == -20 && engine.NextWakeMilliseconds == 0, "missing camera control releases orbit");
            });
            Test("compiler rejects unsafe and malformed programs", () =>
            {
                string[] invalid = { "", "main {", "main {} main{}", "main{wait(5);}", "main{break;}", "main{continue;}", "int a[0];main{}", "main{unknown=3;}", "main{set_val(SOUTH);}", "main{system(1);}", "main{get_val(\"SOUTH\");}", "main{int a; a[0]=1;}", "int a[3];main{a=1;}", "const int a[]={1};main{a[0]=2;}", "function f(a,a){} main{}", "define Z=1/0;main{}", "main{int a=1;int a=2;}", "int a[];main{}", "main{/*" };
                foreach (string source in invalid) CompileError(source);
                CompileError("main {set_val(VALUE_1," + new string('(', 150) + "1" + new string(')', 150) + ");}", "nesting");
                var expression = new StringBuilder("main {set_val(VALUE_1,1"); for (int i = 0; i < 2000; i++) expression.Append("+1"); expression.Append(");}"); CompileError(expression.ToString(), "nesting");
            });
            Test("runtime safety and fail-open output", () =>
            {
                RuntimeError("main{set_val(LX,99);while(1){}}", "budget"); RuntimeError("int a[2];main{set_val(LX,99);a[2]=1;}", "Array index"); RuntimeError("main{set_val(LX,99);set_val(VALUE_1,3/0);}", "Division"); RuntimeError("main{set_val(256,1);}", "identifier"); RuntimeError("main{f();}function f(){return f();}", "depth"); RuntimeError("main{set_val(VALUE_1,get_bit(2,32));}", "Bit index"); RuntimeError("main{if(1)combo_run(M);}combo M{wait(-1);}", "wait duration");
                var engine = ScriptCompiler.Compile("main{}").CreateInstance(); var frame = Frame(); engine.Execute(frame, 1000, 1); try { engine.Execute(frame, 999, 0); throw new Exception("time accepted"); } catch (ScriptException) { Check(true, "monotonic guard"); }
            });
            Test("visual source roundtrip and independent advanced logic", () =>
            {
                const string custom = "// custom stays intact\n define SCALE=80;\nint clicks;\nmain { if(event_press(SOUTH)){clicks++;} set_val(VALUE_1,clicks); }\nfunction original(v){ return v*2; }\n";
                var doc = VisualDocument.Parse(custom); Check(doc.HasAdvancedCode && doc.Constants.Count == 1, "advanced source retained"); doc.Constants[0].Value = 90;
                doc.Rules.Add(new VisualRule { Kind = VisualRuleKind.Remap, Source = "SOUTH", Target = "EAST" }); doc.Rules.Add(new VisualRule { Kind = VisualRuleKind.Invert, Source = "LY" });
                var macro = new VisualMacro { Name = "Tap", Trigger = "WEST" }; macro.Steps.Add(new VisualMacroStep { Control = "NORTH", Value = 100, DurationMilliseconds = 40 }); macro.Steps.Add(new VisualMacroStep { Control = "NORTH", Value = 0, DurationMilliseconds = 20 }); doc.Macros.Add(macro);
                string built = doc.BuildSource(); Check(built.Contains("function original(v){ return v*2; }") && built.Contains("define SCALE=90;"), "custom preserved and scalar patched");
                var parsed = VisualDocument.Parse(built); Check(parsed.Rules.Count == 2 && parsed.Macros.Count == 1 && parsed.Constants.Count == 1, "managed metadata roundtrip"); Check(!parsed.ManagedSectionModified, "valid managed hash");
                parsed.Rules[0].Target = "NORTH"; var engine = ScriptCompiler.Compile(parsed.BuildSource()).CreateInstance(); var frame = Frame(); frame.Physical[10] = 100; frame.Physical[1] = 50; engine.Execute(frame, 0, 0); Check(frame.Values[13] == 100 && frame.Values[1] == -50 && frame.Values[208] == 1, "visual plus custom output");
                string edited = built.Replace("invert(LY);", "invert(LX);"); var changed = VisualDocument.Parse(edited); Check(changed.ManagedSectionModified && changed.HasAdvancedCode && changed.Rules.Count == 0, "hand-edited managed code becomes advanced");
                string untouched = changed.BuildSource(); Check(untouched.Contains("invert(LX);"), "hand edits never overwritten"); ScriptCompiler.Compile(untouched);
                var newMacro = new VisualMacro { Name = "SecondTap", Trigger = "GUIDE" }; newMacro.Steps.Add(new VisualMacroStep { Control = "EAST", DurationMilliseconds = 30 }); changed.Macros.Add(newMacro);
                string withAdditionalMacro = changed.BuildSource(); Check(withAdditionalMacro.Contains("combo __igc_macro_1_0") && withAdditionalMacro.Contains("combo __igc_macro_0"), "new macro avoids preserved identifier collision"); ScriptCompiler.Compile(withAdditionalMacro);
            });
            Test("visual rules validation", () =>
            {
                var doc = VisualDocument.Parse("main{}"); doc.Rules.Add(new VisualRule { Kind = VisualRuleKind.Deadzone, Source = "LX", Value = 100 }); try { doc.BuildSource(); throw new Exception("invalid visual accepted"); } catch (ScriptException) { Check(true, "deadzone reject"); }
                doc = VisualDocument.Parse("main{}"); doc.Constants.Add(new VisualConstant { Name = "STRENGTH", Value = 50 }); var parsed = VisualDocument.Parse(doc.BuildSource()); Check(parsed.Constants.Count == 1 && parsed.Constants[0].Value == 50, "new defines roundtrip");
                doc = VisualDocument.Parse("// define IGNORED=4;\nmain{}"); Check(doc.Constants.Count == 0, "ignore comment fake constants");
            });
            Test("advanced visual macros and patterns", () => VisualAdvancedTests.Run(Check));
            Test("isolated parallel instances", () =>
            {
                var program = ScriptCompiler.Compile("int n;main{set_val(VALUE_1,++n);}"); Parallel.For(0, 16, device => { var instance = program.CreateInstance(); var frame = Frame(); for (int cycle = 1; cycle <= 2000; cycle++) { instance.Execute(frame, cycle * 1000L, 1); if (frame.Values[208] != cycle) throw new Exception("instance race"); } }); Check(true, "32,000 concurrent device frames");
            });
            Test("maximum bounded local call storage", () =>
            {
                var frame = Run("main{set_val(VALUE_1,accumulate(20));}function accumulate(n){int padding[240];padding[0]=n;if(n==0)return 0;return padding[0]+accumulate(n-1);}"); Check(frame.Values[208] == 210, "large local frames preserve caller data");
                frame = Run("function first(){return second();}function second(){return 42;}main{set_val(VALUE_1,first());}"); Check(frame.Values[208] == 42, "zero-local functions require no local heap array");
            });
            Test("bounded malformed-source fuzz", () =>
            {
                var random = new Random(633); string[] tokens = { "main", "{", "}", "int", "a", ";", "=", "1", "if", "(", ")", "wait", "[", "]", "+", "function", "combo", "return", "for", "define" };
                for (int i = 0; i < 3000; i++) { var source = new StringBuilder(); int count = random.Next(1, 80); for (int n = 0; n < count; n++) source.Append(tokens[random.Next(tokens.Length)]).Append(' '); try { ScriptCompiler.Compile(source.ToString()); } catch (ScriptException) { } Check(true, "bounded malformed fuzz " + i); }
            });
            Test("shipped script compilation and execution", () =>
            {
                string[] files = Directory.GetFiles(ScriptsPath(args), "*.igc"); Array.Sort(files, StringComparer.Ordinal); Check(files.Length >= 9, "all shipped examples are present");
                foreach (string file in files)
                {
                    var instance = ScriptCompiler.Compile(File.ReadAllText(file)).CreateInstance(); var frame = Frame();
                    frame.Physical[0] = 35; frame.Physical[1] = -50; frame.Physical[4] = 80; frame.Physical[5] = 70; frame.Physical[10] = 100; frame.Physical[14] = 100; frame.Physical[32] = 100; frame.Physical[96] = 25; frame.Physical[97] = -60;
                    instance.Execute(frame, 0, 0); instance.Execute(frame, 5000, 5); frame.Physical[10] = frame.Physical[32] = 0; instance.Execute(frame, 500000, 495); instance.Reset();
                    Check(true, "compile and three-frame execution: " + Path.GetFileName(file));
                }
            });
            string benchmarks = Benchmark(), advancedBenchmarks = BenchmarkAdvanced(args);
            string json = "{\"passed\":true,\"tests\":" + tests + ",\"checks\":" + checks + ",\"benchmark\":" + benchmarks + ",\"advanced_benchmark\":" + advancedBenchmarks + "}";
            for (int i = 0; i + 1 < args.Length; i++) if (args[i] == "--json") File.WriteAllText(args[i + 1], json, new UTF8Encoding(false));
            Console.WriteLine(json); return 0;
        }
        catch (Exception error) { Console.Error.WriteLine(error); return 1; }
    }
    private static string Benchmark()
    {
        const int batches = 101, perBatch = 1000;
        var instance = ScriptCompiler.Compile("define THRESHOLD=10; main{remap(SOUTH,EAST);invert(LY);deadzone(LX,THRESHOLD);sensitivity(RX,80);if(event_press(WEST))combo_run(Tap);}combo Tap{set_val(NORTH,100);wait(20);set_val(NORTH,0);wait(20);}").CreateInstance();
        var frame = Frame(); frame.Physical[0] = 35; frame.Physical[1] = -60; frame.Physical[2] = 50; frame.Physical[10] = 100; long time = 0;
        for (int i = 0; i < 10000; i++) { time += 1000; instance.Execute(frame, time, 1); }
        GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect(); int before0 = GC.CollectionCount(0), before1 = GC.CollectionCount(1), before2 = GC.CollectionCount(2);
        long[] samples = new long[batches]; var watch = new Stopwatch();
        for (int batch = 0; batch < batches; batch++) { watch.Restart(); for (int i = 0; i < perBatch; i++) { time += 1000; instance.Execute(frame, time, 1); } watch.Stop(); samples[batch] = watch.ElapsedTicks; }
        int collections = GC.CollectionCount(0) - before0 + GC.CollectionCount(1) - before1 + GC.CollectionCount(2) - before2;
        Array.Sort(samples); double factor = 1000000.0 / Stopwatch.Frequency / perBatch;
        Check(collections == 0, "no VM stress garbage collections");
        Console.WriteLine("Pure VM: median " + (samples[50] * factor).ToString("F3", CultureInfo.InvariantCulture) + " us/frame; p95 " + (samples[95] * factor).ToString("F3", CultureInfo.InvariantCulture) + "; max batch average " + (samples[100] * factor).ToString("F3", CultureInfo.InvariantCulture) + "; GCs " + collections);
        return "{\"scope\":\"pure VM; 101 batches of 1000 frames after 10000 warmup; not hardware latency\",\"frames\":101000,\"median_us\":" + (samples[50] * factor).ToString("F6", CultureInfo.InvariantCulture) + ",\"p95_us\":" + (samples[95] * factor).ToString("F6", CultureInfo.InvariantCulture) + ",\"maximum_batch_average_us\":" + (samples[100] * factor).ToString("F6", CultureInfo.InvariantCulture) + ",\"garbage_collections\":" + collections + "}";
    }
    private static string BenchmarkAdvanced(string[] args)
    {
        const int batches = 101, perBatch = 1000;
        var instance = ScriptCompiler.Compile(File.ReadAllText(Path.Combine(ScriptsPath(args), "07 - Additive Figure Eight.igc"))).CreateInstance(); var frame = Frame();
        frame.Physical[0] = 10; frame.Physical[1] = -20; frame.Physical[2] = 80; frame.Physical[10] = frame.Physical[11] = 100; long time = 0;
        for (int i = 0; i < 10000; i++) { time += 4000; instance.Execute(frame, time, 4); }
        long[] samples = new long[batches]; var watch = new Stopwatch(); GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
        int before0 = GC.CollectionCount(0), before1 = GC.CollectionCount(1), before2 = GC.CollectionCount(2);
        for (int batch = 0; batch < batches; batch++)
        {
            watch.Restart(); for (int i = 0; i < perBatch; i++) { time += 4000; instance.Execute(frame, time, 4); } watch.Stop(); samples[batch] = watch.ElapsedTicks;
            Check(frame.Values[0] >= -20 && frame.Values[0] <= 40 && frame.Values[1] >= -40 && frame.Values[1] <= 0 && instance.NextWakeMilliseconds == 4, "advanced orbit stress output stays centered and bounded");
        }
        int collections = GC.CollectionCount(0) - before0 + GC.CollectionCount(1) - before1 + GC.CollectionCount(2) - before2; Array.Sort(samples); double factor = 1000000.0 / Stopwatch.Frequency / perBatch;
        Check(collections == 0, "no advanced VM stress garbage collections");
        Console.WriteLine("Additive figure eight: median " + (samples[50] * factor).ToString("F3", CultureInfo.InvariantCulture) + " us/frame; p95 " + (samples[95] * factor).ToString("F3", CultureInfo.InvariantCulture) + "; max batch average " + (samples[100] * factor).ToString("F3", CultureInfo.InvariantCulture) + "; GCs " + collections);
        return "{\"scope\":\"pure VM additive figure-eight; 101 batches of 1000 frames after 10000 warmup; not hardware latency\",\"frames\":101000,\"median_us\":" + (samples[50] * factor).ToString("F6", CultureInfo.InvariantCulture) + ",\"p95_us\":" + (samples[95] * factor).ToString("F6", CultureInfo.InvariantCulture) + ",\"maximum_batch_average_us\":" + (samples[100] * factor).ToString("F6", CultureInfo.InvariantCulture) + ",\"garbage_collections\":" + collections + "}";
    }
}
