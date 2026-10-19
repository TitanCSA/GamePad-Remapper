using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using GamePadRemapper.Core;

[assembly: System.Reflection.AssemblyTitle("GamePad Remapper")]
[assembly: System.Reflection.AssemblyDescription("IGC controller scripts, visual remapping and macros for Interceptor Gamepad Controller")]
[assembly: System.Reflection.AssemblyCompany("TitanCSA")]
[assembly: System.Reflection.AssemblyProduct("GamePad Remapper")]
[assembly: System.Reflection.AssemblyVersion("1.1.0.0")]
[assembly: System.Reflection.AssemblyFileVersion("1.1.0.0")]

namespace GamePadRemapper
{
    internal static class Program
    {
        [STAThread]
        public static int Main(string[] args)
        {
            try
            {
                string liveReport = Argument(args, "--live-test");
                if (liveReport != null)
                {
                    int index = Array.IndexOf(args, "--live-test");
                    int seconds = 6;
                    if (index + 2 < args.Length && !Int32.TryParse(args[index + 2], NumberStyles.Integer, CultureInfo.InvariantCulture, out seconds)) throw new ArgumentException("The live-test duration must be an integer.");
                    if (seconds < 4 || seconds > 60) throw new ArgumentException("Live tests must last between 4 and 60 seconds.");
                    return DriverLiveTest.Run(liveReport, seconds);
                }
                if (args.Contains("--help"))
                {
                    MessageBox.Show("GamePadRemapper.exe [--open file.igc] [--demo]\n\n--demo: explicitly simulated UI preview, no driver capture\n--render-tests directory: render simulated interface images\n--ui-self-test report.json: run editor and storage tests without a driver", "GamePad Remapper");
                    return 0;
                }
                string renderDirectory = Argument(args, "--render-tests");
                string testReport = Argument(args, "--ui-self-test");
                if (renderDirectory != null || testReport != null) return RunInterfaceTests(renderDirectory, testReport);
                var app = new Application { ShutdownMode = ShutdownMode.OnMainWindowClose };
                Theme.Apply(app);
                var window = new MainWindow(args.Contains("--demo"));
                window.LiveApplyEnabled = args.Contains("--live-apply");
                app.DispatcherUnhandledException += (sender, error) =>
                {
                    error.Handled = true;
                    MessageBox.Show(window, "The operation failed.\n\n" + error.Exception.Message, "GamePad Remapper", MessageBoxButton.OK, MessageBoxImage.Error);
                };
                string open = Argument(args, "--open");
                string session = Argument(args, "--session");
                if (session != null) window.RestoreSession(session, Argument(args, "--origin"));
                else if (open != null) window.OpenPath(open);
                if (args.Contains("--inject")) window.Loaded += (sender, eventArgs) => window.InjectAsync();
                return app.Run(window);
            }
            catch (Exception exception)
            {
                int testIndex = Array.IndexOf(args, "--ui-self-test");
                if (testIndex >= 0 && testIndex + 1 < args.Length)
                {
                    string path = Path.GetFullPath(args[testIndex + 1]);
                    Directory.CreateDirectory(Path.GetDirectoryName(path));
                    var serializer = new System.Web.Script.Serialization.JavaScriptSerializer();
                    File.WriteAllText(path, serializer.Serialize(new { Passed = false, Error = exception.ToString(), CaptureStarted = false }), new UTF8Encoding(false));
                    return 1;
                }
                try { MessageBox.Show(exception.Message, "GamePad Remapper could not start", MessageBoxButton.OK, MessageBoxImage.Error); }
                catch (InvalidOperationException) { }
                return 1;
            }
        }

        private static string Argument(string[] args, string name)
        {
            int index = Array.IndexOf(args, name);
            if (index < 0) return null;
            if (index + 1 >= args.Length) throw new ArgumentException("Missing argument after " + name + ".");
            return args[index + 1];
        }

        private static int RunInterfaceTests(string renderDirectory, string report)
        {
            string temporary = Path.Combine(Path.GetTempPath(), "GamePadRemapper-UI-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(temporary);
            var checks = new List<string>();
            var app = new Application { ShutdownMode = ShutdownMode.OnExplicitShutdown };
            Theme.Apply(app);
            Theme.ReducedMotion = true;
            var window = new MainWindow(true, Path.Combine(temporary, "Scripts"));
            window.Show();
            Pump();
            bool passed = false;
            string error = null;
            var watch = Stopwatch.StartNew();
            try
            {
                RemapperService.WarmRuntime();
                RemapperService.WarmRuntime();
                Require(true, "Runtime warmup is repeatable without capture or user-script state", checks);
                window.Source = "// This custom comment must survive.\nmain {\n}\n";
                Require(window.CheckScript(), "Empty main compiles", checks);
                Require(CheckAsynchronously(window), "Background compiler returns to the UI thread", checks);
                Require(!window.LiveApplyEnabled && !window.CaptureRunning, "Live apply is opt in and opening the editor does not capture", checks);
                using (var coordinator = new CompilationCoordinator())
                {
                    coordinator.Advance();
                    var stale = coordinator.CompileAsync("main { set_val(SOUTH, 100); }", coordinator.Version);
                    coordinator.Advance();
                    var current = coordinator.CompileAsync("main { set_val(EAST, 50); }", coordinator.Version);
                    Require(!coordinator.IsCurrent(stale.GetAwaiter().GetResult()), "A stale background compilation cannot be accepted", checks);
                    Require(coordinator.IsCurrent(current.GetAwaiter().GetResult()), "The newest background compilation can be accepted", checks);
                }
                using (var inactive = new RemapperService())
                    Require(!inactive.TryUpdate(ScriptCompiler.Compile("main {}")), "Updating a stopped session cannot start capture", checks);
                Require(MainWindow.HasControl("BUTTON_64") && MainWindow.HasControl("AXIS_64") && MainWindow.HasControl("TRIGGER_32") && MainWindow.HasControl("HAT_16") && MainWindow.HasControl("VALUE_48"), "Visual selectors include all dynamic HID controls", checks);
                string untouched = window.Source;
                Require(window.ShowVisual(), "Visual mode opens", checks);
                window.ShowCode();
                Require(window.Source == untouched, "Switching views preserves exact source", checks);
                window.ShowVisual();
                window.AddTestRule();
                Require(window.Source.Contains("// This custom comment must survive."), "Visual edit preserves custom source", checks);
                Require(window.CheckScript(), "Visual rule compiles", checks);
                var document = VisualDocument.Parse(window.Source);
                Require(document.Rules.Count == 1 && document.Rules[0].Source == "LY", "Visual rule survives round trip", checks);
                document.Macros.Add(new VisualMacro { Name = "EditorTest", Trigger = "SOUTH", Enabled = true });
                document.Macros[0].Steps.Add(new VisualMacroStep { Control = "EAST", Value = 100, DurationMilliseconds = 75 });
                document.Macros[0].Steps.Add(new VisualMacroStep { Control = "EAST", Value = 0, DurationMilliseconds = 25 });
                window.Source = document.BuildSource();
                Require(window.CheckScript(), "Visual macro compiles", checks);
                Require(VisualDocument.Parse(window.Source).Macros[0].Steps.Count == 2, "Macro steps survive round trip", checks);
                window.AddInfinityRecipe();
                var infinity = VisualDocument.Parse(window.Source).Macros.Last();
                Require(infinity.Kind == VisualMacroKind.AxisPattern && infinity.Activation == VisualMacroActivation.WhileTrue && infinity.Pattern.Shape == VisualPatternShape.FigureEight && infinity.Pattern.Blend == VisualBlend.Add, "One-click infinity recipe preserves additive continuous motion", checks);
                Require(infinity.Condition.Kind == VisualConditionKind.All && infinity.Condition.Children.Count == 3 && infinity.Condition.Children[2].Control == "RX" && infinity.Condition.Children[2].Value == 50, "Infinity recipe combines A, B and camera X threshold", checks);
                Require(window.CheckScript(), "Compound trigger and figure-eight editor source compiles", checks);
                Require(window.TestEditorKindTransitions(), "Changing motion to a sequence or expression creates editable valid defaults", checks);
                Require(window.TestInvalidVisualFieldSwitch(), "Invalid numeric visual edits stay visible until corrected", checks);
                WaitForValidation(window);
                int validationCount = window.AutomaticValidationCount;
                for (int edit = 0; edit < 30; ++edit) window.Source = "main {} // edit " + edit.ToString(CultureInfo.InvariantCulture);
                WaitForValidation(window);
                Require(window.AutomaticValidationCount == validationCount + 1, "Rapid edits are debounced into one background compilation", checks);
                window.LiveApplyEnabled = true;
                window.Source = "main { add_val(LX, 12); }";
                WaitForValidation(window);
                Require(!window.CaptureRunning && window.CompilationMessage.Contains("Valid"), "Live apply can validate in the editor without starting capture", checks);
                window.Source = "main { set_val(SOUTH, ); }";
                WaitForValidation(window);
                Require(window.CompilationMessage.Contains("Line") && !window.CaptureRunning, "Automatic validation reports errors without starting capture", checks);
                window.LiveApplyEnabled = false;
                window.Source = document.BuildSource();
                string valid = window.Source;
                window.Source = "main { set_val(SOUTH, ); }";
                Require(!window.CheckScript(), "Invalid script produces compiler diagnostic", checks);
                Require(window.DiagnosticText.IndexOf("Line", StringComparison.OrdinalIgnoreCase) >= 0, "Compiler diagnostic includes source location", checks);
                window.Source = valid;
                window.TestSearch("LY");
                Require(window.SelectionLength == 2, "Code search selects matching text", checks);
                string file = Path.Combine(temporary, "roundtrip.igc");
                ScriptStorage.Write(file, valid);
                ScriptStorage.Write(file, valid + "\n// saved atomically\n");
                Require(File.ReadAllText(file).EndsWith("// saved atomically\n", StringComparison.Ordinal), "Atomic save writes new version", checks);
                Require(File.ReadAllText(file + ".bak") == valid, "Atomic save preserves previous version", checks);
                string draft = Path.Combine(temporary, "draft.igc");
                ScriptStorage.WriteDraft(draft, "main {} // draft one");
                ScriptStorage.WriteDraft(draft, "main {} // draft two");
                Require(File.ReadAllText(draft).Contains("draft two") && !File.Exists(draft + ".bak"), "Draft recovery is atomic and does not create backup accumulation", checks);
                window.OpenPath(file);
                Require(!window.IsDirty, "Opening a saved script clears dirty state", checks);
                window.Source += "\n// edited\n";
                Require(window.IsDirty, "Editing marks the script unsaved", checks);
                Require(window.DiagnosticText.Contains("SIMULATED") || window.DiagnosticText.Contains("Last operation"), "Test interface never captures hardware", checks);
                if (renderDirectory != null)
                {
                    Directory.CreateDirectory(renderDirectory);
                    window.PreparePreview(ScreenshotSource());
                    window.ShowCode();
                    Render(window, Path.Combine(renderDirectory, "code-editor.png"));
                    window.ShowVisual();
                    Render(window, Path.Combine(renderDirectory, "visual-editor.png"));
                    window.ScrollToMacros();
                    window.PrepareMacroPreview();
                    Render(window, Path.Combine(renderDirectory, "macro-editor.png"));
                    window.Width = 1140;
                    window.Height = 760;
                    Render(window, Path.Combine(renderDirectory, "compact-layout.png"));
                    string staleError = Path.Combine(renderDirectory, "render-error.txt");
                    if (File.Exists(staleError)) File.Delete(staleError);
                    checks.Add("Rendered actual WPF code, visual and compact windows");
                }
                passed = true;
            }
            catch (Exception exception) { error = exception.ToString(); }
            finally
            {
                watch.Stop();
                window.CloseForTest();
                app.Shutdown(passed ? 0 : 1);
                if (report != null)
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(report)));
                    var json = new System.Web.Script.Serialization.JavaScriptSerializer();
                    File.WriteAllText(report, json.Serialize(new { Passed = passed, Checks = checks, Error = error, ElapsedMilliseconds = watch.Elapsed.TotalMilliseconds, CaptureStarted = false, TemporaryDirectory = temporary }), new UTF8Encoding(false));
                }
                if (renderDirectory != null && !passed) File.WriteAllText(Path.Combine(renderDirectory, "render-error.txt"), error ?? "Unknown error", new UTF8Encoding(false));
            }
            return passed ? 0 : 1;
        }

        private static string ScreenshotSource()
        {
            string source = "// My everyday controller\n// A small deadzone, inverted camera and a timed button sequence.\n\ndefine CAMERA_SPEED = 115;\n\nmain {\n    // Add advanced logic here. Visual cards remain editable.\n}\n";
            var visual = VisualDocument.Parse(source);
            visual.Rules.Add(new VisualRule { Kind = VisualRuleKind.Deadzone, Source = "LX", Value = 8, Enabled = true });
            visual.Rules.Add(new VisualRule { Kind = VisualRuleKind.Invert, Source = "RY", Enabled = true });
            visual.Rules.Add(new VisualRule { Kind = VisualRuleKind.Remap, Source = "SOUTH", Target = "EAST", Enabled = false });
            var macro = new VisualMacro { Name = "Quick_sequence", Trigger = "LB", Enabled = true };
            macro.Steps.Add(new VisualMacroStep { Control = "SOUTH", Value = 100, DurationMilliseconds = 90 });
            macro.Steps.Add(new VisualMacroStep { Control = "SOUTH", Value = 0, DurationMilliseconds = 60 });
            visual.Macros.Add(VisualDocument.CreateFigureEightPreset());
            visual.Macros.Add(macro);
            return visual.BuildSource();
        }

        private static void Require(bool condition, string name, List<string> checks)
        {
            if (!condition) throw new InvalidOperationException(name);
            checks.Add(name);
        }

        private static bool CheckAsynchronously(MainWindow window)
        {
            bool complete = false, result = false;
            Exception failure = null;
            var frame = new DispatcherFrame();
            var deadline = Stopwatch.StartNew();
            var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(5) };
            timer.Tick += (sender, args) => { if (complete || deadline.ElapsedMilliseconds > 5000) frame.Continue = false; };
            Dispatcher.CurrentDispatcher.BeginInvoke(new Action(async () =>
            {
                try { result = await window.CheckScriptTask(); }
                catch (Exception exception) { failure = exception; }
                finally { complete = true; }
            }));
            timer.Start();
            Dispatcher.PushFrame(frame);
            timer.Stop();
            if (failure != null) throw new InvalidOperationException("Background compilation failed", failure);
            if (!complete) throw new TimeoutException("The background compiler did not complete.");
            return result;
        }

        private static void Pump()
        {
            var frame = new DispatcherFrame();
            Dispatcher.CurrentDispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, new Action(() => frame.Continue = false));
            Dispatcher.PushFrame(frame);
        }

        private static void WaitForValidation(MainWindow window)
        {
            var frame = new DispatcherFrame();
            var deadline = Stopwatch.StartNew();
            var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(10) };
            timer.Tick += (sender, args) => { if (window.AcceptedValidationVersion == window.CurrentValidationVersion || deadline.ElapsedMilliseconds > 8000) frame.Continue = false; };
            timer.Start();
            Dispatcher.PushFrame(frame);
            timer.Stop();
            if (window.AcceptedValidationVersion != window.CurrentValidationVersion) throw new TimeoutException("Debounced validation did not complete.");
        }

        private static void Render(MainWindow window, string path)
        {
            window.UpdateLayout();
            Pump();
            var visual = new DrawingVisual();
            using (DrawingContext context = visual.RenderOpen()) context.DrawRectangle(new VisualBrush(window), null, new Rect(0, 0, window.ActualWidth, window.ActualHeight));
            var bitmap = new RenderTargetBitmap((int)Math.Ceiling(window.ActualWidth), (int)Math.Ceiling(window.ActualHeight), 96, 96, PixelFormats.Pbgra32);
            bitmap.Render(visual);
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(bitmap));
            using (var stream = File.Create(path)) encoder.Save(stream);
        }
    }
}
