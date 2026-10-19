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
                string scripts = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Scripts");
                for (int i = 0; i + 1 < args.Length; i++) if (args[i] == "--scripts") scripts = Path.GetFullPath(args[i + 1]);
                if (!Directory.Exists(scripts)) throw new DirectoryNotFoundException("Shipped scripts are missing: " + scripts + ". Supply --scripts path for a source-tree test run.");
                string[] files = Directory.GetFiles(scripts, "*.igc"); Array.Sort(files, StringComparer.Ordinal); Check(files.Length >= 6, "all shipped examples are present");
                foreach (string file in files)
                {
                    var instance = ScriptCompiler.Compile(File.ReadAllText(file)).CreateInstance(); var frame = Frame();
                    frame.Physical[0] = 35; frame.Physical[1] = -50; frame.Physical[4] = 80; frame.Physical[5] = 70; frame.Physical[10] = 100; frame.Physical[14] = 100; frame.Physical[32] = 100; frame.Physical[96] = 25; frame.Physical[97] = -60;
                    instance.Execute(frame, 0, 0); instance.Execute(frame, 5000, 5); frame.Physical[10] = frame.Physical[32] = 0; instance.Execute(frame, 500000, 495); instance.Reset();
                    Check(true, "compile and three-frame execution: " + Path.GetFileName(file));
                }
            });
            string benchmarks = Benchmark();
            string json = "{\"passed\":true,\"tests\":" + tests + ",\"checks\":" + checks + ",\"benchmark\":" + benchmarks + "}";
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
}
