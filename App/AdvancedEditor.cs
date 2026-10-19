using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using GamePadRemapper.Core;

namespace GamePadRemapper
{
    internal sealed partial class MainWindow
    {
        private readonly CheckBox liveApply = new CheckBox { Content = "Live apply valid edits", Margin = new Thickness(0, 10, 0, 0), ToolTip = "Opt in for this session. After you explicitly Inject, valid edits replace the active configuration automatically. Invalid edits keep the last working configuration. This never starts capture." };
        private readonly Button recoverDraft = new Button { Content = "Recover unsaved draft", Margin = new Thickness(0, 0, 0, 8), Visibility = Visibility.Collapsed };
        private readonly HashSet<TextBox> invalidVisualInputs = new HashSet<TextBox>();
        private readonly Dictionary<VisualMacro, TextBlock> macroSummaries = new Dictionary<VisualMacro, TextBlock>();
        private readonly Dictionary<VisualMacro, Expander> macroConditions = new Dictionary<VisualMacro, Expander>();
        private readonly CompilationCoordinator compilation = new CompilationCoordinator();
        private ServiceDevice[] editorDevices = new ServiceDevice[0];
        private string[] editorControlNames = ControlNames;
        private DispatcherTimer validationTimer;
        private Task draftWrite = Task.CompletedTask;
        private string draftPath, recoveryDirectory, recoveredDraftPath, activeSource;
        private long documentRevision;
        private Func<string, MessageBoxResult> confirmDiscardPrompt;
        private bool automaticValidationRunning, visualCommitFailed, editingDisposed, loadingDocument, visualValidationBlocked;
        private int lastLineCount;
        private int automaticValidationCount;
        private long acceptedValidationVersion;

        private void InitializeEditingAutomation()
        {
            recoveryDirectory = preview ? Path.Combine(libraryDirectory, ".recovery") : ScriptStorage.RecoveryDirectory;
            draftPath = Path.Combine(recoveryDirectory, Guid.NewGuid().ToString("N") + ".igc");
            validationTimer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(400) };
            validationTimer.Tick += (sender, args) => RunAutomaticValidation();
            liveApply.Checked += (sender, args) => { footer.Text = "Live apply enabled for this session. Capture starts only after you choose Inject."; QueueAutomaticValidation(); };
            liveApply.Unchecked += (sender, args) => footer.Text = "Live apply disabled. Choose Inject to apply changes.";
            recoverDraft.Style = (Style)Application.Current.FindResource("QuietButton");
            recoverDraft.Click += (sender, args) => RecoverLatestDraft();
            UpdateRecoveryButton();
        }

        private void QueueAutomaticValidation()
        {
            if (editingDisposed || validationTimer == null) return;
            compilation.Advance();
            validationTimer.Stop();
            validationTimer.Start();
        }

        private async void RunAutomaticValidation()
        {
            validationTimer.Stop();
            if (editingDisposed || closing) return;
            if (busy || automaticValidationRunning) { validationTimer.Start(); return; }
            automaticValidationRunning = true;
            ++automaticValidationCount;
            string source = code.Text;
            long version = compilation.Version;
            long document = documentRevision;
            try
            {
                var result = await compilation.CompileAsync(source, version);
                if (!compilation.IsCurrent(result) || editingDisposed || closing || busy || !String.Equals(code.Text, source, StringComparison.Ordinal)) return;
                acceptedValidationVersion = version;
                visualValidationBlocked = visualMode && (invalidVisualInputs.Count != 0 || visualCommitFailed);
                if (visualValidationBlocked) { /* Keep the visual-field error; do not apply a partial visual edit. */ }
                else if (result.Error != null) SetError(result.Error, false);
                else
                {
                    operationError = "";
                    bool applied = false;
                    if (liveApply.IsChecked == true && !preview && !String.Equals(activeSource, source, StringComparison.Ordinal))
                    {
                        // TryUpdate only replaces an existing capture. It cannot start or restart one.
                        applied = service.TryUpdate(result.Program);
                        if (applied) activeSource = source;
                    }
                    bool active = !preview && String.Equals(activeSource, source, StringComparison.Ordinal) && service.IsRunning;
                    compileStatus.Text = applied ? "✓ Live changes applied" : active ? "✓ This version is active" : "✓ Valid script · ready to inject";
                    compileStatus.Foreground = Theme.Mint;
                    if (applied) footer.Text = "Valid edits applied atomically · Ctrl+Enter also applies manually";
                }
                if (IsDirty)
                {
                    try { draftWrite = Task.Run(() => ScriptStorage.WriteDraft(draftPath, source)); await draftWrite; if (editingDisposed || closing || document != documentRevision || !IsDirty) DeleteOwnDraft(); }
                    catch (IOException exception) { footer.Text = "Draft recovery unavailable: " + exception.Message; }
                    catch (UnauthorizedAccessException exception) { footer.Text = "Draft recovery unavailable: " + exception.Message; }
                }
            }
            catch (Exception exception) { if (!editingDisposed) SetError(exception, false); }
            finally
            {
                automaticValidationRunning = false;
                if (!editingDisposed && !closing && compilation.Version != version) validationTimer.Start();
            }
        }

        private void DisposeEditingAutomation()
        {
            editingDisposed = true;
            if (validationTimer != null) validationTimer.Stop();
            compilation.Dispose();
        }

        private void DeleteOwnDraft()
        {
            if (String.IsNullOrEmpty(draftPath)) return;
            try { if (File.Exists(draftPath)) File.Delete(draftPath); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }

        private void DeleteRecoveredDraft()
        {
            if (String.IsNullOrEmpty(recoveredDraftPath)) return;
            try { if (File.Exists(recoveredDraftPath)) File.Delete(recoveredDraftPath); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            recoveredDraftPath = null;
            UpdateRecoveryButton();
        }

        private void UpdateRecoveryButton()
        {
            try { recoverDraft.Visibility = Directory.Exists(recoveryDirectory) && Directory.EnumerateFiles(recoveryDirectory, "*.igc").Any(path => !String.Equals(path, draftPath, StringComparison.OrdinalIgnoreCase) && !String.Equals(path, recoveredDraftPath, StringComparison.OrdinalIgnoreCase)) ? Visibility.Visible : Visibility.Collapsed; }
            catch (IOException) { recoverDraft.Visibility = Visibility.Collapsed; }
            catch (UnauthorizedAccessException) { recoverDraft.Visibility = Visibility.Collapsed; }
        }

        private void RecoverLatestDraft()
        {
            if (busy || !ConfirmDiscard()) return;
            try
            {
                string path = Directory.EnumerateFiles(recoveryDirectory, "*.igc").Where(candidate => !String.Equals(candidate, draftPath, StringComparison.OrdinalIgnoreCase) && !String.Equals(candidate, recoveredDraftPath, StringComparison.OrdinalIgnoreCase)).OrderByDescending(File.GetLastWriteTimeUtc).FirstOrDefault();
                if (path == null) { UpdateRecoveryButton(); return; }
                SetSource(ScriptStorage.Read(path), null, true);
                recoveredDraftPath = path;
                UpdateRecoveryButton();
                ShowCode();
                footer.Text = "Recovered unsaved draft. Save to choose its file name; the original file was not changed.";
            }
            catch (Exception exception) { SetError(exception); }
        }

        private bool CommitFocusedVisualField()
        {
            if (!visualMode) return true;
            var focused = Keyboard.FocusedElement as FrameworkElement;
            if (focused != null && (focused is TextBox || focused is ComboBox)) focused.MoveFocus(new TraversalRequest(FocusNavigationDirection.Next));
            if (invalidVisualInputs.Count == 0 && !visualCommitFailed) return true;
            footer.Text = "Correct the highlighted visual field before switching views or applying. Your edit is still here.";
            return false;
        }

        private void ResetVisualToSource()
        {
            invalidVisualInputs.Clear();
            visualCommitFailed = false;
            if (visualValidationBlocked) { visualValidationBlocked = false; QueueAutomaticValidation(); }
            if (!visualMode) return;
            visualMode = false;
            ShowVisual();
        }

        private void BuildMacroEditors()
        {
            macroSummaries.Clear();
            macroConditions.Clear();
            var recipes = new WrapPanel { Margin = new Thickness(0, 8, 0, 12) };
            var recipe = Button("∞  A + B → additive figure eight", AddInfinityRecipe);
            recipe.IsEnabled = visualDocument.Macros.Count < 32;
            recipes.Children.Add(recipe);
            recipes.Children.Add(Theme.Label("One click adds a complete, editable trigger + motion recipe.", 11, Theme.Muted));
            visualContent.Children.Add(recipes);
            Section("MACROS", "+ Add macro", () =>
            {
                var macro = new VisualMacro { Name = UniqueMacroName("Macro"), Enabled = true, Condition = new VisualCondition { Kind = VisualConditionKind.Pressed, Control = DefaultButton() } };
                macro.Steps.Add(new VisualMacroStep { Control = DefaultButton(), Value = 100, DurationMilliseconds = 100 });
                visualDocument.Macros.Add(macro);
                CommitVisual(true);
            }, visualDocument.Macros.Count < 32);
            foreach (var macro in visualDocument.Macros.ToArray()) BuildMacroEditor(macro);
            if (visualDocument.Macros.Count == 0) EmptyHint("Create a timed sequence or continuous axis motion. Combine any/all/not conditions, holds, double taps and axis thresholds.");
            EmptyHint("SOUTH = Xbox A / PlayStation Cross; EAST = Xbox B / PlayStation Circle. LX / LY and RX / RY are the left and right sticks when those controls exist. Generic HID controllers expose their reported AXIS / BUTTON / VALUE fields instead of guessed face-button mappings.");
            foreach (string warning in visualDocument.GetWarnings()) visualContent.Children.Add(new TextBlock { Text = warning, Foreground = Theme.Accent, TextWrapping = TextWrapping.Wrap, FontSize = 11, Margin = new Thickness(0, 0, 0, 9) });
        }

        private string UniqueMacroName(string prefix)
        {
            if (!visualDocument.Macros.Any(macro => String.Equals(macro.Name, prefix, StringComparison.Ordinal))) return prefix;
            for (int index = 2; ; ++index)
            {
                string name = prefix + "_" + index.ToString(CultureInfo.InvariantCulture);
                if (!visualDocument.Macros.Any(macro => String.Equals(macro.Name, name, StringComparison.Ordinal))) return name;
            }
        }

        private string DefaultButton()
        {
            if (preview) return "SOUTH";
            foreach (var device in editorDevices)
                if (device.Family == 3 && device.Available != null)
                    for (int index = 0; index < device.Available.Length && index < Controls.Names.Length; ++index)
                        if (device.Available[index] && Controls.Names[index].StartsWith("BUTTON_", StringComparison.Ordinal)) return Controls.Names[index];
            return "SOUTH";
        }

        private void RefreshEditorControlNames()
        {
            editorDevices = preview ? new ServiceDevice[0] : service.GetSnapshot().Devices;
            var available = new HashSet<string>(StringComparer.Ordinal);
            foreach (var device in editorDevices)
                for (int index = 0; device.Available != null && index < device.Available.Length && index < Controls.Names.Length; ++index)
                    if (device.Available[index]) available.Add(Controls.Names[index]);
            editorControlNames = available.Count == 0 ? ControlNames : ControlNames.OrderBy(name => available.Contains(name) ? 0 : 1).ToArray();
        }

        internal void AddInfinityRecipe()
        {
            if (!ShowVisual()) return;
            var macro = VisualDocument.CreateFigureEightPreset();
            macro.Name = UniqueMacroName("Infinity");
            visualDocument.Macros.Add(macro);
            CommitVisual(true);
            ScrollToMacros();
            footer.Text = "A + B held, camera X > 50% → figure eight added to the other stick. Adjust the trigger, axes and amplitudes below.";
        }

        private void BuildMacroEditor(VisualMacro macro)
        {
            var fields = new StackPanel();
            var header = new WrapPanel();
            header.Children.Add(TextField("Name", macro.Name, value => { macro.Name = value; CommitVisual(false); }, 145));
            header.Children.Add(ChoiceField("Action", macro.Kind, value => { macro.Kind = value; if (value == VisualMacroKind.Sequence && macro.Steps.Count == 0) macro.Steps.Add(new VisualMacroStep { Control = DefaultButton(), Value = 100, DurationMilliseconds = 100 }); CommitVisual(true); }, 155));
            header.Children.Add(ChoiceField("Activation", macro.Activation, value => { macro.Activation = value; CommitVisual(false); }, 145));
            var enabled = new CheckBox { Content = "Enabled", IsChecked = macro.Enabled, Margin = new Thickness(0, 24, 8, 0) };
            enabled.Click += (sender, args) => { macro.Enabled = enabled.IsChecked == true; CommitVisual(false); };
            header.Children.Add(enabled);
            fields.Children.Add(header);
            var summary = new TextBlock { Foreground = Theme.Mint, FontSize = 12, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 11, 0, 12) };
            macroSummaries[macro] = summary;
            fields.Children.Add(summary);
            if (macro.Condition == null) macro.Condition = new VisualCondition { Kind = VisualConditionKind.Pressed, Control = macro.Trigger };
            var conditionEditor = new Expander { Header = "Trigger", IsExpanded = true, Content = BuildConditionEditor(macro.Condition, 0), Foreground = Theme.Text, Margin = new Thickness(0, 2, 0, 5) };
            macroConditions[macro] = conditionEditor;
            fields.Children.Add(conditionEditor);
            if (macro.Kind == VisualMacroKind.AxisPattern) fields.Children.Add(BuildPatternEditor(macro));
            else
            {
                foreach (var step in macro.Steps.ToArray())
                {
                    var row = new WrapPanel { Margin = new Thickness(0, 10, 0, 0) };
                    row.Children.Add(ControlPicker("Control", step.Control, value => { step.Control = value; CommitVisual(false); }));
                    row.Children.Add(NumberField("Value", step.Value, value => { step.Value = value; CommitVisual(false); }));
                    row.Children.Add(NumberField("Hold (ms)", step.DurationMilliseconds, value => { step.DurationMilliseconds = value; CommitVisual(false); }));
                    row.Children.Add(ChoiceField("Output", step.Blend, value => { step.Blend = value; CommitVisual(false); }, 100));
                    var remove = Button("Remove step", () => { macro.Steps.Remove(step); CommitVisual(true); });
                    remove.IsEnabled = macro.Steps.Count > 1;
                    row.Children.Add(remove);
                    fields.Children.Add(row);
                }
                var steps = new WrapPanel { Margin = new Thickness(0, 12, 0, 0) };
                var addStep = Button("+ Timed step", () => { macro.Steps.Add(new VisualMacroStep { Control = DefaultButton(), Value = 0, DurationMilliseconds = 50 }); CommitVisual(true); });
                addStep.IsEnabled = macro.Steps.Count < 256;
                steps.Children.Add(addStep);
                fields.Children.Add(steps);
            }
            var actions = new WrapPanel { Margin = new Thickness(0, 12, 0, 0) };
            actions.Children.Add(Button("Remove macro", () => { visualDocument.Macros.Remove(macro); CommitVisual(true); }));
            fields.Children.Add(actions);
            visualContent.Children.Add(VisualCard(fields));
            UpdateMacroSummaries();
        }

        private UIElement BuildConditionEditor(VisualCondition condition, int depth)
        {
            var document = visualDocument;
            var body = new StackPanel();
            var row = new WrapPanel();
            row.Children.Add(ChoiceField(depth == 0 ? "When" : "Condition", condition.Kind, kind =>
            {
                condition.Kind = kind;
                if (kind == VisualConditionKind.All || kind == VisualConditionKind.Any || kind == VisualConditionKind.Not)
                {
                    if (condition.Children.Count == 0) condition.Children.Add(new VisualCondition { Kind = VisualConditionKind.Held, Control = DefaultButton() });
                    if (kind == VisualConditionKind.Not && condition.Children.Count > 1) condition.Children.RemoveRange(1, condition.Children.Count - 1);
                }
                else
                {
                    condition.Children.Clear();
                    if (kind == VisualConditionKind.Expression && String.IsNullOrWhiteSpace(condition.Expression)) condition.Expression = "is_available(" + condition.Control + ") && get_ival(" + condition.Control + ") != 0";
                }
                CommitVisual(true);
            }, 160));
            bool group = condition.Kind == VisualConditionKind.All || condition.Kind == VisualConditionKind.Any || condition.Kind == VisualConditionKind.Not;
            if (!group && condition.Kind != VisualConditionKind.Expression)
            {
                row.Children.Add(ControlPicker("Control", condition.Control, value => { condition.Control = value; CommitVisual(false); }));
                if (condition.Kind == VisualConditionKind.Compare || condition.Kind == VisualConditionKind.AbsoluteCompare)
                {
                    var compare = new ComboBox { ItemsSource = new[] { ">", ">=", "<", "<=", "==", "!=" }, SelectedItem = condition.Operator };
                    compare.SelectionChanged += (sender, args) => { if (loadingDocument || !ReferenceEquals(document, visualDocument)) return; condition.Operator = (string)compare.SelectedItem; CommitVisual(false); };
                    row.Children.Add(Field("Compare", compare, 76));
                    row.Children.Add(NumberField("Threshold", condition.Value, value => { condition.Value = value; CommitVisual(false); }));
                }
                if (condition.Kind == VisualConditionKind.HeldFor || condition.Kind == VisualConditionKind.DoubleTap)
                    row.Children.Add(NumberField(condition.Kind == VisualConditionKind.HeldFor ? "Held for (ms)" : "Tap window (ms)", condition.Milliseconds, value => { condition.Milliseconds = value; CommitVisual(false); }));
            }
            body.Children.Add(row);
            if (condition.Kind == VisualConditionKind.Expression)
            {
                var expression = new TextBox { Text = condition.Expression ?? "", AcceptsReturn = true, MinHeight = 58, TextWrapping = TextWrapping.Wrap, FontFamily = new FontFamily("Consolas"), Margin = new Thickness(0, 8, 0, 0), ToolTip = "IGC expression, e.g. get_ival(SOUTH) && abs(get_ival(RX)) > 50. Use is_available for optional controls." };
                expression.LostKeyboardFocus += (sender, args) => { if (loadingDocument || !ReferenceEquals(document, visualDocument)) return; condition.Expression = expression.Text.Trim(); CommitVisual(false); };
                body.Children.Add(expression);
            }
            if (group)
            {
                foreach (var child in condition.Children.ToArray())
                {
                    var childRow = new DockPanel { Margin = new Thickness(9, 9, 0, 0) };
                    if (condition.Kind != VisualConditionKind.Not)
                    {
                        var remove = Button("×", () => { condition.Children.Remove(child); CommitVisual(true); });
                        remove.IsEnabled = condition.Children.Count > 1;
                        DockPanel.SetDock(remove, Dock.Right);
                        childRow.Children.Add(remove);
                    }
                    childRow.Children.Add(BuildConditionEditor(child, depth + 1));
                    body.Children.Add(childRow);
                }
                if (condition.Kind != VisualConditionKind.Not)
                {
                    var add = Button("+ Condition", () => { condition.Children.Add(new VisualCondition { Kind = VisualConditionKind.Held, Control = DefaultButton() }); CommitVisual(true); });
                    add.IsEnabled = condition.Children.Count < 16 && depth < 5;
                    add.Margin = new Thickness(9, 9, 0, 0);
                    add.HorizontalAlignment = HorizontalAlignment.Left;
                    body.Children.Add(add);
                }
            }
            return new Border { Child = body, BorderBrush = Theme.Brush("#364665"), BorderThickness = new Thickness(2, 0, 0, 0), Padding = new Thickness(9, 0, 0, 0), Margin = new Thickness(0, 4, 0, 4) };
        }

        private UIElement BuildPatternEditor(VisualMacro macro)
        {
            var pattern = macro.Pattern;
            var panel = new StackPanel { Margin = new Thickness(0, 13, 0, 0) };
            var axes = new WrapPanel();
            axes.Children.Add(ChoiceField("Motion", pattern.Shape, value => { pattern.Shape = value; CommitVisual(false); }, 135));
            axes.Children.Add(ControlPicker("Horizontal axis", pattern.TargetX, value => { pattern.TargetX = value; CommitVisual(false); }));
            axes.Children.Add(ControlPicker("Vertical axis", pattern.TargetY, value => { pattern.TargetY = value; CommitVisual(false); }));
            axes.Children.Add(NumberField("Width %", pattern.AmplitudeX, value => { pattern.AmplitudeX = value; CommitVisual(false); }));
            axes.Children.Add(NumberField("Height %", pattern.AmplitudeY, value => { pattern.AmplitudeY = value; CommitVisual(false); }));
            panel.Children.Add(axes);
            var timingRow = new WrapPanel { Margin = new Thickness(0, 10, 0, 0) };
            timingRow.Children.Add(NumberField("Cycle (ms)", pattern.PeriodMilliseconds, value => { pattern.PeriodMilliseconds = value; CommitVisual(false); }));
            timingRow.Children.Add(ChoiceField("Output", pattern.Blend, value => { pattern.Blend = value; if (value == VisualBlend.Add) pattern.Center = VisualPatternCenter.DynamicPhysical; CommitVisual(true); }, 100));
            timingRow.Children.Add(ChoiceField("Center", pattern.Center, value => { pattern.Center = value; if (value != VisualPatternCenter.DynamicPhysical) pattern.Blend = VisualBlend.Replace; CommitVisual(true); }, 170));
            panel.Children.Add(timingRow);
            var advanced = new Expander { Header = "Timing precision", Margin = new Thickness(0, 10, 0, 0), Foreground = Theme.Muted };
            var advancedPanel = new StackPanel();
            advancedPanel.Children.Add(NumberField("Tick (1–20 ms)", pattern.TickMilliseconds, value => { pattern.TickMilliseconds = value; CommitVisual(false); }));
            advancedPanel.Children.Add(Theme.Label("Default 4 ms. Ticks run only while the motion is active. Windows scheduling and controller transport still affect timing.", 11, Theme.Muted));
            advanced.Content = advancedPanel;
            panel.Children.Add(advanced);
            return panel;
        }

        private sealed class EditorChoice<T>
        {
            internal T Value;
            public override string ToString() { return ChoiceLabel(Value.ToString()); }
        }

        private UIElement ChoiceField<T>(string label, T selected, Action<T> changed, double width) where T : struct
        {
            var document = visualDocument;
            var items = Enum.GetValues(typeof(T)).Cast<T>().Select(value => new EditorChoice<T> { Value = value }).ToArray();
            var combo = new ComboBox { ItemsSource = items, SelectedItem = items.First(item => EqualityComparer<T>.Default.Equals(item.Value, selected)) };
            combo.SelectionChanged += (sender, args) => { if (loadingDocument || !ReferenceEquals(document, visualDocument)) return; var item = combo.SelectedItem as EditorChoice<T>; if (item != null) changed(item.Value); };
            return Field(label, combo, width);
        }

        private static string ChoiceLabel(string value)
        {
            switch (value)
            {
                case "All": return "ALL of these";
                case "Any": return "ANY of these";
                case "Not": return "NOT this";
                case "Held": return "Button held";
                case "Pressed": return "Button pressed";
                case "Released": return "Button released";
                case "Compare": return "Axis / value";
                case "AbsoluteCompare": return "Absolute value";
                case "HeldFor": return "Held for time";
                case "DoubleTap": return "Double tap";
                case "Expression": return "IGC expression";
                case "OnEnter": return "On activation";
                case "WhileTrue": return "While true";
                case "OnExit": return "On deactivation";
                case "AxisPattern": return "Axis motion";
                case "FigureEight": return "Figure eight (∞)";
                case "DynamicPhysical": return "Follow physical input";
                case "Activation": return "Position at activation";
                case "Zero": return "Stick center (0, 0)";
                case "Add": return "Add";
                case "Replace": return "Replace";
                default: return value;
            }
        }

        private void UpdateMacroSummaries()
        {
            foreach (var pair in macroSummaries)
            {
                var macro = pair.Key;
                string condition = DescribeCondition(macro.Condition, 0);
                string action = macro.Kind == VisualMacroKind.AxisPattern ? ChoiceLabel(macro.Pattern.Shape.ToString()) + " on " + macro.Pattern.TargetX + " / " + macro.Pattern.TargetY + ", " + macro.Pattern.AmplitudeX + "% × " + macro.Pattern.AmplitudeY + "%, " + macro.Pattern.PeriodMilliseconds + " ms per cycle. " +
                    (macro.Pattern.Blend == VisualBlend.Add ? "Adds to existing values" : "Replaces axis values") + "; " + ChoiceLabel(macro.Pattern.Center.ToString()).ToLowerInvariant() + "." : macro.Steps.Count + " timed step(s).";
                if (macro.Kind == VisualMacroKind.AxisPattern && (macro.Activation == VisualMacroActivation.OnEnter || macro.Activation == VisualMacroActivation.OnExit)) action += " Plays one full cycle per activation.";
                if (macro.Activation == VisualMacroActivation.Toggle) action += " Activate again to stop.";
                pair.Value.Text = ChoiceLabel(macro.Activation.ToString()) + ": " + condition + " → " + action;
                Expander conditionEditor;
                if (macroConditions.TryGetValue(macro, out conditionEditor)) conditionEditor.Header = "Trigger · " + condition;
            }
        }

        private static string DescribeCondition(VisualCondition condition, int depth)
        {
            if (condition == null) return "trigger button pressed";
            if (depth > 12) return "nested condition";
            switch (condition.Kind)
            {
                case VisualConditionKind.All: return "(" + String.Join(" AND ", condition.Children.Select(child => DescribeCondition(child, depth + 1))) + ")";
                case VisualConditionKind.Any: return "(" + String.Join(" OR ", condition.Children.Select(child => DescribeCondition(child, depth + 1))) + ")";
                case VisualConditionKind.Not: return "NOT " + (condition.Children.Count == 0 ? "condition" : DescribeCondition(condition.Children[0], depth + 1));
                case VisualConditionKind.Compare: return condition.Control + " " + condition.Operator + " " + condition.Value + ControlUnit(condition.Control);
                case VisualConditionKind.AbsoluteCompare: return "|" + condition.Control + "| " + condition.Operator + " " + condition.Value + ControlUnit(condition.Control);
                case VisualConditionKind.HeldFor: return condition.Control + " held for " + condition.Milliseconds + " ms";
                case VisualConditionKind.DoubleTap: return condition.Control + " tapped twice within " + condition.Milliseconds + " ms";
                case VisualConditionKind.Expression: return condition.Expression ?? "IGC expression";
                default: return condition.Control + " " + ChoiceLabel(condition.Kind.ToString()).Replace("Button ", "").ToLowerInvariant();
            }
        }

        private static string ControlUnit(string control) { int id = Controls.Parse(control); return id >= 0 && id < 192 ? "%" : ""; }

        internal bool LiveApplyEnabled { get { return liveApply.IsChecked == true; } set { liveApply.IsChecked = value; } }
        internal long AcceptedValidationVersion { get { return acceptedValidationVersion; } }
        internal long CurrentValidationVersion { get { return compilation.Version; } }
        internal bool CaptureRunning { get { return service.IsRunning; } }
        internal string CompilationMessage { get { return compileStatus.Text; } }
        internal int AutomaticValidationCount { get { return automaticValidationCount; } }
        internal void PrepareMacroPreview()
        {
            foreach (var condition in macroConditions.Values) condition.IsExpanded = false;
            ScrollToMacros();
        }
        internal bool TestEditorKindTransitions()
        {
            string source = code.Text;
            try
            {
                if (!ShowVisual() || visualDocument.Macros.Count == 0) return false;
                var macro = visualDocument.Macros.Last();
                var action = Descendants<ComboBox>(visualContent).Last(combo => combo.SelectedItem is EditorChoice<VisualMacroKind>);
                action.SelectedItem = action.Items.Cast<EditorChoice<VisualMacroKind>>().First(item => item.Value == VisualMacroKind.Sequence);
                if (visualCommitFailed || macro.Kind != VisualMacroKind.Sequence || macro.Steps.Count == 0) return false;
                UpdateLayout();
                var condition = Descendants<ComboBox>(visualContent).First(combo => combo.SelectedItem is EditorChoice<VisualConditionKind>);
                condition.SelectedItem = condition.Items.Cast<EditorChoice<VisualConditionKind>>().First(item => item.Value == VisualConditionKind.Expression);
                UpdateLayout();
                return !visualCommitFailed && Descendants<TextBox>(visualContent).Any(box => box.Text.StartsWith("is_available(", StringComparison.Ordinal)) && ScriptCompiler.Compile(code.Text) != null;
            }
            finally { visualCommitFailed = false; invalidVisualInputs.Clear(); code.Text = source; ShowVisual(); }
        }
        internal bool TestInvalidVisualFieldSwitch()
        {
            if (!ShowVisual()) return false;
            var field = Descendants<TextBox>(visualContent).FirstOrDefault(box => { int number; return Int32.TryParse(box.Text, out number); });
            if (field == null) return false;
            string previous = field.Text, source = code.Text;
            field.Focus();
            field.Text = "invalid-number";
            codeTab.Focus();
            field.RaiseEvent(new KeyboardFocusChangedEventArgs(Keyboard.PrimaryDevice, 0, field, codeTab) { RoutedEvent = Keyboard.LostKeyboardFocusEvent });
            ShowCode();
            bool preserved = visualMode && String.Equals(source, code.Text, StringComparison.Ordinal) && invalidVisualInputs.Contains(field);
            field.Focus(); field.Text = previous; codeTab.Focus();
            field.RaiseEvent(new KeyboardFocusChangedEventArgs(Keyboard.PrimaryDevice, 0, field, codeTab) { RoutedEvent = Keyboard.LostKeyboardFocusEvent });
            ShowCode();
            return preserved && !visualMode;
        }

        internal void PrepareStoppedSessionValidation()
        {
            activeSource = code.Text;
            QueueAutomaticValidation();
        }

        internal void RunEditingRegressionTests(Action<bool, string> require, Action waitForValidation, string directory)
        {
            string original = code.Text;
            try
            {
                var document = VisualDocument.Parse("main {}\n");
                document.Rules.Add(new VisualRule { Kind = VisualRuleKind.Deadzone, Source = "LX", Value = 8 });
                string valid = document.BuildSource();
                string file = Path.Combine(directory, "visual-discard.igc");
                ScriptStorage.Write(file, valid);
                SetSource(valid, file, false);
                ShowVisual(); UpdateLayout();
                var field = Descendants<TextBox>(visualContent).First(box => box.Text == "8");
                field.Text = "bad-number";
                RaiseVisualFieldCommit(field);
                waitForValidation();
                require(compileStatus.Text.Contains("whole number") && visualValidationBlocked, "Automatic checks preserve invalid visual-field errors instead of applying partial edits");
                bool prompted = false;
                confirmDiscardPrompt = message => { prompted = message.Contains("invalid"); return MessageBoxResult.Cancel; };
                require(!ConfirmDiscard() && prompted && invalidVisualInputs.Contains(field), "Cancelling a discard keeps invalid visual edits available");
                field.Text = "8"; RaiseVisualFieldCommit(field);
                waitForValidation();
                require(compileStatus.Text.Contains("Valid") && !visualValidationBlocked, "Correcting a field to its previous value resumes automatic validation");
                field.Text = "bad-number"; RaiseVisualFieldCommit(field);
                confirmDiscardPrompt = message => { prompted = message.Contains("invalid"); return MessageBoxResult.No; };
                require(ConfirmDiscard() && prompted && invalidVisualInputs.Count == 0 && !visualCommitFailed && code.Text == valid, "Invalid visual edits can be discarded instead of trapping the editor");

                UpdateLayout();
                field = Descendants<TextBox>(visualContent).First(box => box.Text == "8");
                field.Text = "bad-number";
                RaiseVisualFieldCommit(field);
                confirmDiscardPrompt = message => MessageBoxResult.Yes;
                require(ConfirmDiscard() && File.ReadAllText(file) == valid && !IsDirty && invalidVisualInputs.Count == 0, "Saving before discard preserves the valid script and drops invalid fields");

                UpdateLayout();
                var staleField = Descendants<TextBox>(visualContent).First(box => box.Text == "8");
                staleField.Text = "bad-number"; RaiseVisualFieldCommit(staleField);
                var next = VisualDocument.Parse("// Another document\nmain {}\n");
                next.Rules.Add(new VisualRule { Kind = VisualRuleKind.Invert, Source = "RY" });
                string replacement = next.BuildSource();
                SetSource(replacement, Path.Combine(directory, "replacement.igc"), false);
                staleField.Text = "25"; RaiseVisualFieldCommit(staleField);
                require(code.Text == replacement && visualDocument.Rules.Count == 1 && visualDocument.Rules[0].Source == "RY" && invalidVisualInputs.Count == 0, "Replacing a document detaches stale visual fields from the new script");

                ShowCode();
                string before = code.Text;
                SetBusy(true);
                try
                {
                    code.Select(0, 0);
                    EditorKeyDown(code, new KeyEventArgs(Keyboard.PrimaryDevice, PresentationSource.FromVisual(this), 0, Key.Tab));
                    require(code.Text == before, "Tab cannot edit the code while compilation or injection makes it read only");
                }
                finally { SetBusy(false); }

                var device = new ServiceDevice { Key = 123, Family = 3, Name = "Offline test controller", Token = "offline-test" };
                device.Available[32] = device.Available[96] = true;
                RefreshControllers(new[] { device }, true);
                var card = controllerCards[123];
                device.Values[96] = 30; device.OutputValues[96] = 60;
                device.Available[97] = true; device.Diagnostic = "Updated diagnostic";
                RefreshControllers(new[] { device }, true);
                require(ReferenceEquals(card, controllerCards[123]) && card.Values.Text.Contains("AXIS_1 30 → 60") && card.Details.Text.Contains("3 controls") && card.Diagnostic.Text == device.Diagnostic, "Live monitor reuses its controller card while values and diagnostics change");
                for (int index = 32; index < 96; ++index) device.Available[index] = true;
                device.Available[159] = true; device.Values[159] = device.OutputValues[159] = -40;
                require(FormatControllerValues(device).Contains("AXIS_64 -40"), "Active controls stay visible behind many idle HID buttons");
                RefreshControllers(new ServiceDevice[0], false);
                require(controllerCards.Count == 0 && renderedDeviceKeys.Length == 0 && devices.Children.Count == 1, "Disconnected controller cards are released");

                SetSource("main {}\n", Path.Combine(directory, "clean.igc"), false);
                string recoverable = Path.Combine(recoveryDirectory, Guid.NewGuid().ToString("N") + ".igc");
                ScriptStorage.WriteDraft(recoverable, valid);
                UpdateRecoveryButton();
                require(recoverDraft.Visibility == Visibility.Visible, "An unsaved draft is discoverable without changing any script");
                RecoverLatestDraft();
                require(code.Text == valid && IsDirty && recoveredDraftPath == recoverable, "Recovering a draft opens it as unsaved text");
                currentPath = Path.Combine(directory, "recovered.igc");
                require(SaveScript(false) && !File.Exists(recoverable) && File.ReadAllText(currentPath) == valid, "Saving a recovered draft retires that recovery file");

                string bad = Path.Combine(libraryDirectory, "invalid-encoding.igc");
                File.WriteAllBytes(bad, new byte[] { 0x6d, 0x61, 0x69, 0x6e, 0x20, 0xc3, 0x28 });
                string saved = currentPath, source = code.Text;
                LoadLibrary();
                library.SelectedItem = library.Items.Cast<ListBoxItem>().First(item => String.Equals(item.Tag as string, bad, StringComparison.OrdinalIgnoreCase));
                require(currentPath == saved && code.Text == source && String.Equals(((ListBoxItem)library.SelectedItem).Tag as string, saved, StringComparison.OrdinalIgnoreCase), "A file that cannot be decoded leaves the previous script and library selection intact");
                bool rejected = false;
                try { ScriptStorage.Write(saved, "main {}\n// " + new string((char)0xd800, 1)); }
                catch (IOException) { rejected = true; }
                require(rejected && File.ReadAllText(saved) == source, "Invalid Unicode cannot silently replace a saved script");
                File.Delete(bad);
            }
            finally
            {
                confirmDiscardPrompt = null;
                invalidVisualInputs.Clear(); visualCommitFailed = false;
                SetSource(original, null, true);
                ShowCode(); ResetPreview();
            }
        }

        private void RaiseVisualFieldCommit(TextBox field)
        {
            field.RaiseEvent(new KeyboardFocusChangedEventArgs(Keyboard.PrimaryDevice, 0, field, codeTab) { RoutedEvent = Keyboard.LostKeyboardFocusEvent });
        }
    }

    // A versioned compile result is accepted only for the current source. Cancellation
    // does not interrupt the bounded compiler; it prevents stale work from being applied.
    internal sealed class CompilationCoordinator : IDisposable
    {
        private readonly object gate = new object();
        private CancellationTokenSource cancellation = new CancellationTokenSource();
        private long version;
        private bool disposed;
        internal long Version { get { lock (gate) return version; } }
        internal void Advance()
        {
            lock (gate)
            {
                if (disposed) return;
                ++version;
                cancellation.Cancel();
                cancellation.Dispose();
                cancellation = new CancellationTokenSource();
            }
        }
        internal Task<CompilationResult> CompileAsync(string source, long requestedVersion)
        {
            CancellationToken token;
            lock (gate) token = cancellation.Token;
            return Task.Run(() =>
            {
                var result = new CompilationResult { Version = requestedVersion };
                if (token.IsCancellationRequested) return result;
                try { result.Program = ScriptCompiler.Compile(source); }
                catch (Exception exception) { result.Error = exception; }
                result.Cancelled = token.IsCancellationRequested;
                return result;
            });
        }
        internal bool IsCurrent(CompilationResult result) { lock (gate) return !disposed && !result.Cancelled && result.Version == version && (result.Program != null || result.Error != null); }
        public void Dispose() { lock (gate) { if (disposed) return; disposed = true; cancellation.Cancel(); cancellation.Dispose(); } }
    }

    internal sealed class CompilationResult
    {
        internal long Version;
        internal bool Cancelled;
        internal ScriptProgram Program;
        internal Exception Error;
    }
}
