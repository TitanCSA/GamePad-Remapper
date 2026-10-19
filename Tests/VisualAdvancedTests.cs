using System;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;
using GamePadRemapper.Core;

internal static class VisualAdvancedTests
{
    private static ControlFrame Frame()
    {
        var frame = new ControlFrame(); for (int i = 0; i < 256; i++) frame.Available[i] = true;
        for (int i = 192; i < 208; i++) frame.Physical[i] = -1;
        return frame;
    }
    private static ScriptInstance Engine(VisualMacro macro)
    {
        var document = VisualDocument.Parse("main{}"); document.Macros.Add(macro);
        return ScriptCompiler.Compile(document.BuildSource()).CreateInstance();
    }
    private static VisualMacro Sequence(VisualMacroActivation activation)
    {
        var macro = new VisualMacro { Name = "Tap", Activation = activation, Condition = new VisualCondition { Kind = VisualConditionKind.Held, Control = "SOUTH" } };
        macro.Steps.Add(new VisualMacroStep { Control = "NORTH", Value = 100, DurationMilliseconds = 20 });
        return macro;
    }
    private static void Reject(Action operation, Action<bool, string> check, string message)
    {
        try { operation(); throw new Exception("Accepted invalid visual document: " + message); }
        catch (ScriptException) { check(true, message); }
    }
    private static string MetadataSource(object macro = null, object rule = null, object constant = null)
    {
        var payload = new Dictionary<string, object> { { "Version", 2 }, { "Macros", macro == null ? new object[0] : new[] { macro } }, { "Rules", rule == null ? new object[0] : new[] { rule } }, { "Constants", constant == null ? new object[0] : new[] { constant } } };
        string content = "// @document " + Convert.ToBase64String(Encoding.UTF8.GetBytes(new JavaScriptSerializer().Serialize(payload))) + "\n";
        string hash; using (var sha = SHA256.Create()) hash = BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(content))).Replace("-", "");
        return "main{}\n// <igc-visual-v1>\n// @hash " + hash + "\n" + content + "// </igc-visual-v1>\n";
    }
    public static void Run(Action<bool, string> check)
    {
        var document = VisualDocument.Parse("main{}"); var macro = VisualDocument.CreateFigureEightPreset(); document.Macros.Add(macro);
        string source = document.BuildSource(); var parsed = VisualDocument.Parse(source);
        check(parsed.Macros.Count == 1 && parsed.Macros[0].Condition.Children.Count == 3 && parsed.Macros[0].Kind == VisualMacroKind.AxisPattern, "compound pattern metadata roundtrip");
        check(!parsed.ManagedSectionModified && !parsed.HasAdvancedCode, "managed source remains completely visual");
        check(parsed.Macros[0].Pattern.Blend == VisualBlend.Add && parsed.Macros[0].Pattern.Center == VisualPatternCenter.DynamicPhysical, "preset live additive default");
        var engine = ScriptCompiler.Compile(source).CreateInstance(); var frame = Frame(); frame.Physical[0] = 10; frame.Physical[1] = -20;
        engine.Execute(frame, 0, 0); check(frame.Values[0] == 10 && frame.Values[1] == -20 && engine.NextWakeMilliseconds == 0, "inactive pattern has no idle polling");
        frame.Physical[10] = frame.Physical[11] = 100; frame.Physical[2] = 50;
        engine.Execute(frame, 1000, 1); check(engine.NextWakeMilliseconds == 0, "strict greater-than 50 percent threshold");
        frame.Physical[2] = 51; engine.Execute(frame, 10000, 9); check(frame.Values[0] == 10 && frame.Values[1] == -20 && engine.NextWakeMilliseconds == 4, "activation starts at phase zero around existing 10/-20 center");
        engine.Execute(frame, 135000, 125); check(frame.Values[0] == 28 && frame.Values[1] == 5 && engine.NextWakeMilliseconds == 4, "infinity quarter-lobe shape added to base");
        frame.Physical[0] = -40; frame.Physical[1] = 30;
        engine.Execute(frame, 260000, 125); check(frame.Values[0] == -15 && frame.Values[1] == 30, "live movement rebases without offset accumulation");
        frame.Physical[10] = 0; engine.Execute(frame, 300000, 40); check(frame.Values[0] == -40 && frame.Values[1] == 30 && engine.NextWakeMilliseconds == 0, "releasing a chord stops both axes immediately");
        frame.Physical[10] = 100; engine.Execute(frame, 350000, 50); check(frame.Values[0] == -40 && frame.Values[1] == 30 && engine.NextWakeMilliseconds == 4, "reactivation resets phase rather than jumping to old phase");
        frame.Available[1] = false; engine.Execute(frame, 360000, 10); check(engine.NextWakeMilliseconds == 0 && frame.Values[0] == -40, "missing second stick axis stops safely without polling");
        frame.Available[1] = true; frame.Physical[0] = frame.Physical[1] = 90; engine.Execute(frame, 400000, 40); engine.Execute(frame, 525000, 125);
        check(frame.Values[0] == 100 && frame.Values[1] == 100, "additive output is clamped to valid signed-axis limits");

        document = VisualDocument.Parse("main { sensitivity(LX, 50); }"); document.Macros.Add(VisualDocument.CreateFigureEightPreset());
        engine = ScriptCompiler.Compile(document.BuildSource()).CreateInstance(); frame = Frame(); frame.Physical[0] = 20; frame.Physical[10] = frame.Physical[11] = frame.Physical[2] = 100;
        engine.Execute(frame, 0, 0); engine.Execute(frame, 250000, 250); check(frame.Values[0] == 35, "additive pattern composes with earlier custom transforms");

        macro = VisualDocument.CreateFigureEightPreset(); macro.Pattern.Center = VisualPatternCenter.Activation; macro.Pattern.Blend = VisualBlend.Replace;
        engine = Engine(macro); frame = Frame(); frame.Physical[0] = 10; frame.Physical[1] = -20; frame.Physical[10] = frame.Physical[11] = frame.Physical[2] = 100;
        engine.Execute(frame, 0, 0); frame.Physical[0] = 60; frame.Physical[1] = 60; engine.Execute(frame, 125000, 125); check(frame.Values[0] == 28 && frame.Values[1] == 5, "activation center stays anchored when physical stick moves");

        foreach (VisualPatternShape shape in Enum.GetValues(typeof(VisualPatternShape)))
        {
            macro = VisualDocument.CreateFigureEightPreset(); macro.Pattern.Shape = shape; macro.Pattern.Blend = VisualBlend.Replace; macro.Pattern.Center = VisualPatternCenter.Zero;
            engine = Engine(macro); frame = Frame(); frame.Physical[10] = frame.Physical[11] = frame.Physical[2] = 100;
            engine.Execute(frame, 0, 0); engine.Execute(frame, 250000, 250);
            check(frame.Values[0] == 25 && frame.Values[1] == 0, "motion shape reaches expected signed-axis cardinal point: " + shape);
        }
        macro = VisualDocument.CreateFigureEightPreset(); macro.Pattern.PeriodMilliseconds = 3600000;
        engine = Engine(macro); frame = Frame(); frame.Physical[10] = frame.Physical[11] = frame.Physical[2] = 100;
        engine.Execute(frame, 0, 0); engine.Execute(frame, 3599999000L, 1); check(frame.Values[0] <= 0 && frame.Values[0] >= -1, "one-hour phase multiplication stays inside Int32");
        frame.Physical[10] = 0; engine.Execute(frame, 2592000000000L, 1); frame.Physical[10] = 100; engine.Execute(frame, 2592000001000L, 1); engine.Execute(frame, 2592900001000L, 1);
        check(frame.Values[0] == 25 && frame.Values[1] == 0, "phase remains correct after 30 days of instance uptime");

        foreach (VisualMacroActivation activation in new[] { VisualMacroActivation.OnEnter, VisualMacroActivation.OnExit })
        {
            macro = VisualDocument.CreateFigureEightPreset(); macro.Activation = activation;
            macro.Condition = new VisualCondition { Kind = activation == VisualMacroActivation.OnEnter ? VisualConditionKind.Pressed : VisualConditionKind.Held, Control = "SOUTH" };
            engine = Engine(macro); frame = Frame(); engine.Execute(frame, 0, 0);
            frame.Physical[10] = 100; engine.Execute(frame, 1000, 1);
            if (activation == VisualMacroActivation.OnExit) check(engine.NextWakeMilliseconds == 0, "exit pattern remains idle before the release edge");
            frame.Physical[10] = 0; engine.Execute(frame, 2000, 1);
            long started = activation == VisualMacroActivation.OnEnter ? 1000 : 2000;
            engine.Execute(frame, started + 250000, 249);
            check(frame.Values[0] == 25 && engine.NextWakeMilliseconds == 4, "edge-triggered pattern completes movement after trigger pulse: " + activation);
            engine.Execute(frame, started + 1000000, 750);
            check(engine.NextWakeMilliseconds == 4, "one-shot lifetime deadline requests its final release tick: " + activation);
            engine.Execute(frame, started + 1004000, 4);
            check(frame.Values[0] == 0 && frame.Values[1] == 0 && engine.NextWakeMilliseconds == 0, "one-shot pattern ends and releases its periodic timer: " + activation);
            engine.Execute(frame, started + 1200000, 196); check(engine.NextWakeMilliseconds == 0, "completed one-shot never restarts itself: " + activation);
        }
        macro = VisualDocument.CreateFigureEightPreset(); macro.Activation = VisualMacroActivation.OnEnter;
        macro.Condition = new VisualCondition { Kind = VisualConditionKind.DoubleTap, Control = "SOUTH", Milliseconds = 250 };
        engine = Engine(macro); frame = Frame(); frame.Physical[10] = 100; engine.Execute(frame, 0, 0);
        frame.Physical[10] = 0; engine.Execute(frame, 50000, 50); frame.Physical[10] = 100; engine.Execute(frame, 100000, 50);
        frame.Physical[10] = 0; engine.Execute(frame, 101000, 1); engine.Execute(frame, 350000, 249);
        check(frame.Values[0] == 25 && engine.NextWakeMilliseconds == 4, "double-tap motion plays beyond its one-frame activation pulse");
        engine.Execute(frame, 1100000, 750); engine.Execute(frame, 1104000, 4); check(engine.NextWakeMilliseconds == 0, "double-tap motion completes one cycle and sleeps");

        macro = VisualDocument.CreateFigureEightPreset(); macro.Activation = VisualMacroActivation.OnEnter;
        macro.Condition = new VisualCondition { Kind = VisualConditionKind.Pressed, Control = "SOUTH" };
        engine = Engine(macro); frame = Frame(); engine.Execute(frame, 0, 0); frame.Physical[10] = 100; engine.Execute(frame, 1000, 1);
        frame.Physical[10] = 0; engine.Execute(frame, 2000, 1); frame.Physical[10] = 100; engine.Execute(frame, 101000, 99);
        check(frame.Values[0] == 0 && frame.Values[1] == 0, "retriggering a one-shot restarts its phase immediately");
        engine.Execute(frame, 1051000, 950); check(engine.NextWakeMilliseconds == 4, "retriggering extends lifetime past the first activation deadline");
        engine.Execute(frame, 1101000, 50); engine.Execute(frame, 1105000, 4); check(engine.NextWakeMilliseconds == 0, "retriggered one-shot stops at its replacement lifetime deadline");
        frame.Physical[10] = 0; engine.Execute(frame, 1200000, 95); frame.Physical[10] = 100; engine.Execute(frame, 1300000, 100);
        engine.Execute(frame, 2592000000000L, 1); engine.Execute(frame, 2592000004000L, 4);
        check(frame.Values[0] == 0 && frame.Values[1] == 0 && engine.NextWakeMilliseconds == 0, "one-shot lifetime expires safely after a 30-day delayed evaluation");

        foreach (VisualMacroActivation activation in Enum.GetValues(typeof(VisualMacroActivation)))
        {
            engine = Engine(Sequence(activation)); frame = Frame(); engine.Execute(frame, 0, 0); frame.Physical[10] = 100; engine.Execute(frame, 1000, 1);
            check(frame.Values[13] == (activation == VisualMacroActivation.OnExit ? 0 : 100), "sequence activation: " + activation);
            engine.Execute(frame, 21000, 20); check(frame.Values[13] == (activation == VisualMacroActivation.WhileTrue || activation == VisualMacroActivation.Toggle ? 100 : 0), "sequence repeats exactly while requested: " + activation);
            frame.Physical[10] = 0; engine.Execute(frame, 22000, 1);
            check(frame.Values[13] == (activation == VisualMacroActivation.OnExit || activation == VisualMacroActivation.Toggle ? 100 : 0), "release semantics: " + activation);
            if (activation == VisualMacroActivation.Toggle)
            {
                frame.Physical[10] = 100; engine.Execute(frame, 23000, 1); check(frame.Values[13] == 0 && engine.NextWakeMilliseconds == 0, "second toggle stops and releases holds immediately");
            }
            else if (activation == VisualMacroActivation.WhileTrue) check(engine.NextWakeMilliseconds == 0, "while-true sequence releases periodic wake on false gate");
        }

        macro = Sequence(VisualMacroActivation.OnEnter); macro.Condition = new VisualCondition { Kind = VisualConditionKind.DoubleTap, Control = "SOUTH", Milliseconds = 250 };
        engine = Engine(macro); frame = Frame(); frame.Physical[10] = 100; engine.Execute(frame, 0, 0); check(frame.Values[13] == 0 && engine.NextWakeMilliseconds == 0, "double tap first press never fires");
        frame.Physical[10] = 0; engine.Execute(frame, 50000, 50); frame.Physical[10] = 100; engine.Execute(frame, 100000, 50); check(frame.Values[13] == 100, "double tap second actual press fires");
        macro = Sequence(VisualMacroActivation.OnEnter); macro.Condition = new VisualCondition { Kind = VisualConditionKind.HeldFor, Control = "SOUTH", Milliseconds = 40 };
        engine = Engine(macro); frame = Frame(); engine.Execute(frame, 0, 0); check(engine.NextWakeMilliseconds == 0, "held duration does not poll when button is released");
        frame.Physical[10] = 100; engine.Execute(frame, 1000, 1); engine.Execute(frame, 41000, 40); check(frame.Values[13] == 100, "held duration fires at threshold without another physical report");

        var any = new VisualCondition { Kind = VisualConditionKind.Any };
        any.Children.Add(new VisualCondition { Kind = VisualConditionKind.Compare, Control = "RX", Operator = ">", Value = 90 });
        var not = new VisualCondition { Kind = VisualConditionKind.Not }; not.Children.Add(new VisualCondition { Kind = VisualConditionKind.Held, Control = "WEST" }); any.Children.Add(not);
        macro = Sequence(VisualMacroActivation.OnEnter); macro.Condition = any; document = VisualDocument.Parse("define CUSTOM=7; main {set_val(VALUE_1,CUSTOM);}"); document.Macros.Add(macro);
        source = document.BuildSource(); parsed = VisualDocument.Parse(source); check(parsed.Macros[0].Condition.Children[1].Kind == VisualConditionKind.Not && parsed.Constants[0].Value == 7 && parsed.HasAdvancedCode, "nested ANY/NOT roundtrip preserves custom code and constants");
        engine = ScriptCompiler.Compile(parsed.BuildSource()).CreateInstance(); frame = Frame(); engine.Execute(frame, 0, 0); check(frame.Values[13] == 100 && frame.Values[208] == 7, "nested negation executes alongside preserved custom logic");
        foreach (VisualConditionKind kind in Enum.GetValues(typeof(VisualConditionKind)))
        {
            if (kind == VisualConditionKind.All || kind == VisualConditionKind.Any || kind == VisualConditionKind.Not) continue;
            macro = Sequence(VisualMacroActivation.OnEnter); macro.Condition = new VisualCondition { Kind = kind, Expression = "is_available(RX) && abs(get_ival(RX)) > 50", Milliseconds = 100 };
            document = VisualDocument.Parse("main{}"); document.Macros.Add(macro); parsed = VisualDocument.Parse(document.BuildSource());
            check(parsed.Macros[0].Condition.Kind == kind && parsed.Macros[0].Condition.Expression == macro.Condition.Expression, "every leaf kind survives roundtrip: " + kind);
        }
        macro = Sequence(VisualMacroActivation.OnEnter); macro.Condition = new VisualCondition { Kind = VisualConditionKind.Expression, Expression = "get_ival(SOUTH) && (get_ival(RX)>50 || get_ival(RY)<-50)" };
        engine = Engine(macro); frame = Frame(); frame.Physical[10] = 100; frame.Physical[3] = -60; engine.Execute(frame, 0, 0); check(frame.Values[13] == 100, "custom expression supports compound nested trigger logic");

        string legacyContent = "// @macro Legacy SOUTH 1\n// @step NORTH 100 20\nfunction __igc_visual_apply() { if(event_press(SOUTH))combo_run(__igc_macro_0); }\ncombo __igc_macro_0 {set_val(NORTH,100);wait(20);}\n";
        string hash; using (var sha = SHA256.Create()) hash = BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(legacyContent))).Replace("-", "");
        string legacy = "main { /* @igc-visual-call */ __igc_visual_apply(); }\n// <igc-visual-v1>\n// @hash " + hash + "\n" + legacyContent + "// </igc-visual-v1>\n";
        parsed = VisualDocument.Parse(legacy); check(!parsed.ManagedSectionModified && parsed.Macros.Count == 1 && parsed.Macros[0].Condition == null, "v1 visual metadata imports without changing legacy trigger semantics");
        source = parsed.BuildSource(); parsed = VisualDocument.Parse(source); check(parsed.Macros[0].Name == "Legacy" && parsed.Macros[0].Steps[0].Value == 100, "v1 metadata upgrades to v2 losslessly");
        string edited = source.Replace("set_val(NORTH, 100)", "set_val(NORTH, 80)"); parsed = VisualDocument.Parse(edited);
        check(parsed.ManagedSectionModified && parsed.HasAdvancedCode && parsed.Macros.Count == 0, "hand edits become advanced source instead of being overwritten by stale metadata");
        string retained = parsed.BuildSource(); check(retained.Contains("set_val(NORTH, 80)"), "rebuilding preserves hand-edited generated code");
        var oversized = new StringBuilder();
        for (int i = 0; i < 257; i++) oversized.Append("// @rule Invert LX EAST 0 1\n");
        string oversizedContent = oversized.ToString(); using (var sha = SHA256.Create()) hash = BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(oversizedContent))).Replace("-", "");
        string oversizedSource = "main{}\n// <igc-visual-v1>\n// @hash " + hash + "\n" + oversizedContent + "// </igc-visual-v1>\n";
        Reject(() => VisualDocument.Parse(oversizedSource), check, "legacy oversized rules rejected before UI row construction");
        macro = VisualDocument.CreateFigureEightPreset(); macro.Pattern = null;
        Reject(() => VisualDocument.Parse(MetadataSource(macro)), check, "missing axis pattern metadata rejected before UI rendering");
        macro = Sequence(VisualMacroActivation.OnEnter); macro.Pattern = null;
        parsed = VisualDocument.Parse(MetadataSource(macro)); check(parsed.Macros[0].Pattern != null, "dormant sequence pattern defaults safely when old metadata contains null");
        foreach (Action<VisualMacro> corrupt in new Action<VisualMacro>[]
        {
            value => value.Kind = (VisualMacroKind)999,
            value => value.Activation = (VisualMacroActivation)999,
            value => value.Pattern.Shape = (VisualPatternShape)999,
            value => value.Pattern.Blend = (VisualBlend)999,
            value => value.Pattern.Center = (VisualPatternCenter)999,
            value => value.Pattern.TargetX = "SOUTH",
            value => value.Pattern.PeriodMilliseconds = 0,
            value => value.Pattern.TickMilliseconds = 0,
            value => value.Condition.Children[2].Operator = "call()",
            value => value.Condition.Children = null,
            value => value.Trigger = "unknown",
            value => value.Name = "bad name",
            value => value.Steps = null,
            value => value.Steps.Add(null),
            value => value.Steps.Add(new VisualMacroStep { DurationMilliseconds = 0 }),
            value => value.Steps.Add(new VisualMacroStep { Blend = (VisualBlend)999 })
        })
        {
            macro = VisualDocument.CreateFigureEightPreset(); corrupt(macro);
            Reject(() => VisualDocument.Parse(MetadataSource(macro)), check, "malformed v2 macro fields fail safely before UI rendering");
        }
        Reject(() => VisualDocument.Parse(MetadataSource(rule: new VisualRule { Kind = (VisualRuleKind)999 })), check, "invalid v2 rule enum rejected before UI rendering");
        Reject(() => VisualDocument.Parse(MetadataSource(rule: new VisualRule { Kind = VisualRuleKind.Deadzone, Value = 100 })), check, "invalid v2 rule range rejected before UI rendering");
        Reject(() => VisualDocument.Parse(MetadataSource(constant: new VisualConstant { Name = "invalid name" })), check, "invalid v2 constant rejected before UI rendering");
        document = VisualDocument.Parse("int __igc_visual_state_0_last; int __igc_macro_0; main{} function __igc_visual_apply(){}"); document.Macros.Add(VisualDocument.CreateFigureEightPreset());
        parsed = VisualDocument.Parse(document.BuildSource()); check(parsed.Macros.Count == 1, "generated state/helper identifiers avoid collisions with advanced code");

        document = VisualDocument.Parse("main{}"); macro = VisualDocument.CreateFigureEightPreset(); document.Macros.Add(macro); macro.Condition.Children.Add(macro.Condition);
        Reject(() => document.BuildSource(), check, "cyclic condition graph rejected before serialization");
        document = VisualDocument.Parse("main{}"); macro = VisualDocument.CreateFigureEightPreset(); document.Macros.Add(macro);
        for (int i = 0; i < 14; i++) { var parent = new VisualCondition { Kind = VisualConditionKind.Not }; parent.Children.Add(macro.Condition); macro.Condition = parent; }
        Reject(() => document.BuildSource(), check, "deep condition graph rejected before recursion overflow");
        foreach (string expression in new[] { "1); set_val(LX,100); if(1", "1){}main{", "/*unbalanced", "(1", "[1", "1//comment" })
        {
            document = VisualDocument.Parse("main{}"); macro = Sequence(VisualMacroActivation.OnEnter); macro.Condition = new VisualCondition { Kind = VisualConditionKind.Expression, Expression = expression }; document.Macros.Add(macro);
            Reject(() => document.BuildSource(), check, "condition statements/injection rejected: " + expression);
        }
        foreach (int period in new[] { 0, -1, 3600001, int.MaxValue })
        {
            document = VisualDocument.Parse("main{}"); macro = VisualDocument.CreateFigureEightPreset(); macro.Pattern.PeriodMilliseconds = period; document.Macros.Add(macro);
            Reject(() => document.BuildSource(), check, "unsafe motion period rejected");
        }
        document = VisualDocument.Parse("main{}"); macro = VisualDocument.CreateFigureEightPreset(); macro.Pattern.TargetY = "LX"; document.Macros.Add(macro);
        Reject(() => document.BuildSource(), check, "duplicate pattern axes rejected");
        macro.Pattern.TargetY = "SOUTH"; Reject(() => document.BuildSource(), check, "button cannot be used as a signed motion axis");
        macro.Pattern.TargetY = "LY"; macro.Pattern.Center = VisualPatternCenter.Activation;
        Reject(() => document.BuildSource(), check, "ambiguous live-add and fixed-center combination rejected");

        // Stress repeated evaluations of a live pattern: every frame starts from physical state.
        engine = Engine(VisualDocument.CreateFigureEightPreset()); frame = Frame(); frame.Physical[10] = frame.Physical[11] = frame.Physical[2] = 100;
        for (int i = 0; i < 2000; i++)
        {
            frame.Physical[0] = i % 101 - 50; frame.Physical[1] = 50 - i % 101; engine.Execute(frame, i * 4000L, 4);
            int angle = (i * 4 % 1000) * 360 / 1000;
            double dx = Math.Sin(angle * (Math.PI / 180)) * 25, dy = Math.Sin((angle * 2 % 360) * (Math.PI / 180)) * 25;
            check(Math.Abs(frame.Values[0] - frame.Physical[0] - dx) <= 0.500001 && Math.Abs(frame.Values[1] - frame.Physical[1] - dy) <= 0.500001 && engine.NextWakeMilliseconds == 4, "continuous complex pattern rebases deterministically: " + i);
        }
    }
}
