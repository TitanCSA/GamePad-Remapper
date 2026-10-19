using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Principal;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Threading;
using GamePadRemapper.Core;
using Microsoft.Win32;

namespace GamePadRemapper
{
    internal sealed partial class MainWindow : Window
    {
        private static readonly string[] ControlNames = Controls.Names.Where(name => !name.StartsWith("RESERVED_", StringComparison.Ordinal)).ToArray();
        private const string EmptyScript = "// GamePad Remapper · IGC\n// Use the visual editor or write your own controller logic.\n\nmain {\n}\n";
        private readonly RemapperService service = new RemapperService();
        private readonly DispatcherTimer refreshTimer;
        private readonly string libraryDirectory;
        private readonly bool preview;
        private readonly ListBox library = new ListBox();
        private readonly TextBox code = new TextBox();
        private readonly TextBox lines = new TextBox();
        private readonly TextBox search = new TextBox { Width = 160, ToolTip = "Find in code (Ctrl+F)" };
        private readonly TextBlock title = Theme.Label("Untitled.igc", 17);
        private readonly TextBlock subtitle = Theme.Label("EDITING · no controller capture", 11, Theme.Muted);
        private readonly TextBlock compileStatus = Theme.Label("Ready to edit · scripts are checked automatically", 12, Theme.Muted);
        private readonly TextBlock footer = Theme.Label("Ready · capture starts only when you choose Inject configuration", 11, Theme.Muted);
        private readonly TextBlock liveStatus = Theme.Label("Not capturing", 14);
        private readonly TextBlock metrics = Theme.Label("0 received  /  0 forwarded", 11, Theme.Muted);
        private readonly TextBlock timing = Theme.Label("Transform + forward: —", 11, Theme.Muted);
        private readonly TextBlock codePosition = Theme.Label("Ln 1, Col 1", 11, Theme.Muted);
        private readonly StackPanel devices = new StackPanel();
        private readonly StackPanel visualContent = new StackPanel();
        private readonly TextBox diagnostics = new TextBox { IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, FontFamily = new FontFamily("Consolas"), FontSize = 11, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, MinHeight = 150 };
        private readonly ContentControl editorHost = new ContentControl();
        private readonly Button inject;
        private readonly Button stop;
        private readonly Button codeTab;
        private readonly Button visualTab;
        private readonly ProgressBar activity = new ProgressBar { Height = 3, Foreground = Theme.Mint, Background = Theme.Brush("#23314A"), Maximum = 100, Value = 0 };
        private UIElement codePanel;
        private VisualDocument visualDocument;
        private string currentPath;
        private string savedSource = "";
        private string operationError = "";
        private bool synchronizing;
        private bool visualMode;
        private bool busy;
        private bool closeAllowed;
        private bool closing;
        private ulong lastReceived;
        private string lastDeviceFingerprint;
        private string lastDiagnostics;
        private int refreshTicks;

        internal MainWindow(bool simulatedPreview = false, string directory = null)
        {
            preview = simulatedPreview;
            libraryDirectory = directory ?? ScriptStorage.DefaultDirectory;
            Title = "GamePad Remapper";
            Width = 1440;
            Height = 930;
            MinWidth = 1120;
            MinHeight = 720;
            Background = Theme.Background;
            Foreground = Theme.Text;
            FontFamily = new FontFamily("Segoe UI");
            FontSize = 13;
            WindowStartupLocation = WindowStartupLocation.CenterScreen;
            UseLayoutRounding = true;
            TextOptions.SetTextFormattingMode(this, TextFormattingMode.Display);

            inject = Button("↗  Inject configuration", InjectAsync, true);
            inject.ToolTip = "Compile and apply in one step (Ctrl+Enter). Invalid edits keep the last working configuration.";
            stop = Button("■  Stop", StopAsync);
            stop.IsEnabled = false;
            codeTab = Button("{ }  Code", () => ShowCode());
            visualTab = Button("◈  Visual editor", () => ShowVisual());
            Content = BuildLayout();
            InitializeEditingAutomation();
            code.TextChanged += (sender, args) => OnSourceChanged();
            code.SelectionChanged += (sender, args) => UpdateCaret();
            code.PreviewKeyDown += EditorKeyDown;
            library.SelectionChanged += LibrarySelectionChanged;
            Closing += OnClosing;
            Closed += (sender, args) => { refreshTimer.Stop(); DisposeEditingAutomation(); service.Dispose(); };
            PreviewKeyDown += WindowKeyDown;
            refreshTimer = new DispatcherTimer(DispatcherPriority.Background) { Interval = TimeSpan.FromMilliseconds(150) };
            refreshTimer.Tick += (sender, args) => RefreshService();
            Loaded += (sender, args) => { refreshTimer.Start(); Theme.Reveal(editorHost); };
            LoadLibrary();
            SetSource(EmptyScript, null, false);
            ShowCode();
            if (preview)
            {
                subtitle.Text = "SIMULATED PREVIEW · no driver or controller connection";
                inject.IsEnabled = false;
                ShowPreviewData();
            }
        }

        private UIElement BuildLayout()
        {
            var layout = new Grid { Margin = new Thickness(24, 20, 24, 12) };
            layout.RowDefinitions.Add(new RowDefinition { Height = new GridLength(88) });
            layout.RowDefinitions.Add(new RowDefinition());
            layout.RowDefinitions.Add(new RowDefinition { Height = new GridLength(32) });
            var header = new Grid();
            header.ColumnDefinitions.Add(new ColumnDefinition());
            header.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var brand = new StackPanel();
            var brandLine = new StackPanel { Orientation = Orientation.Horizontal };
            brandLine.Children.Add(new Border { Width = 37, Height = 37, CornerRadius = new CornerRadius(11), Background = Theme.Brush("#8B76F9"), Margin = new Thickness(0, 0, 12, 0), Child = new TextBlock { Text = "◈", FontSize = 25, Foreground = Brushes.White, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center } });
            brandLine.Children.Add(new TextBlock { Text = "GamePad Remapper", FontSize = 25, FontWeight = FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center });
            brand.Children.Add(brandLine);
            brand.Children.Add(new TextBlock { Text = "YOUR CONTROLLER. YOUR LOGIC.", FontSize = 10, Foreground = Theme.Muted, Margin = new Thickness(49, 6, 0, 0) });
            header.Children.Add(brand);
            var headerActions = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Top };
            var reduceMotion = new CheckBox { Content = "Reduce motion", IsChecked = Theme.ReducedMotion, Margin = new Thickness(0, 12, 16, 0) };
            reduceMotion.Checked += (sender, args) => Theme.ReducedMotion = true;
            reduceMotion.Unchecked += (sender, args) => Theme.ReducedMotion = false;
            headerActions.Children.Add(reduceMotion);
            headerActions.Children.Add(Button("Driver setup ↗", InstallDriver));
            Grid.SetColumn(headerActions, 1);
            header.Children.Add(headerActions);
            layout.Children.Add(header);

            var body = new Grid();
            body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(225) });
            body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(16) });
            body.ColumnDefinitions.Add(new ColumnDefinition());
            body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(16) });
            body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(286) });
            var side = BuildLibrary();
            body.Children.Add(side);
            var editor = BuildEditor();
            Grid.SetColumn(editor, 2);
            body.Children.Add(editor);
            var monitor = BuildMonitor();
            Grid.SetColumn(monitor, 4);
            body.Children.Add(monitor);
            Grid.SetRow(body, 1);
            layout.Children.Add(body);
            Grid.SetRow(footer, 2);
            footer.VerticalAlignment = VerticalAlignment.Center;
            layout.Children.Add(footer);
            return layout;
        }

        private UIElement BuildLibrary()
        {
            var panel = new Grid();
            panel.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            panel.RowDefinitions.Add(new RowDefinition());
            panel.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            var top = new StackPanel();
            top.Children.Add(Theme.Label("SCRIPT LIBRARY", 11, Theme.Muted));
            top.Children.Add(new TextBlock { Text = "Made for your playstyle", FontSize = 16, FontWeight = FontWeights.SemiBold, Margin = new Thickness(0, 8, 0, 15) });
            var buttons = new UniformGrid { Columns = 2, Margin = new Thickness(0, 0, 0, 12) };
            buttons.Children.Add(Button("+ New", NewScript));
            buttons.Children.Add(Button("Open…", OpenScript));
            top.Children.Add(buttons);
            panel.Children.Add(top);
            Grid.SetRow(library, 1);
            panel.Children.Add(library);
            var bottom = new StackPanel { Margin = new Thickness(0, 15, 0, 0) };
            bottom.Children.Add(recoverDraft);
            bottom.Children.Add(Button("Open scripts folder ↗", () => { Directory.CreateDirectory(libraryDirectory); Process.Start(new ProcessStartInfo(libraryDirectory) { UseShellExecute = true }); }));
            bottom.Children.Add(new TextBlock { Text = "IGC scripts stay on your PC.\nCode and visual edits share one file.", Foreground = Theme.Muted, FontSize = 11, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 12, 0, 0) });
            bottom.Children.Add(new Border { Background = Theme.Brush("#1B2940"), CornerRadius = new CornerRadius(9), Padding = new Thickness(11), Margin = new Thickness(0, 18, 0, 0), Child = Theme.Label("INTERCEPTOR CONTROLLER\nSigned kernel filter driver\n2-hour driver demo per boot", 11, Theme.Muted) });
            Grid.SetRow(bottom, 2);
            panel.Children.Add(bottom);
            return Theme.Card(panel, new Thickness(16));
        }

        private UIElement BuildEditor()
        {
            var panel = new Grid();
            panel.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            panel.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            panel.RowDefinitions.Add(new RowDefinition());
            panel.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            var top = new Grid { Margin = new Thickness(0, 0, 0, 16) };
            top.ColumnDefinitions.Add(new ColumnDefinition());
            top.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var details = new StackPanel();
            details.Children.Add(title);
            subtitle.Margin = new Thickness(0, 5, 0, 0);
            details.Children.Add(subtitle);
            top.Children.Add(details);
            var saveButtons = new StackPanel { Orientation = Orientation.Horizontal };
            saveButtons.Children.Add(Button("Save", () => SaveScript(false)));
            saveButtons.Children.Add(Button("Save as…", () => SaveScript(true)));
            Grid.SetColumn(saveButtons, 1);
            top.Children.Add(saveButtons);
            panel.Children.Add(top);
            var modeBar = new Grid { Margin = new Thickness(0, 0, 0, 12) };
            modeBar.ColumnDefinitions.Add(new ColumnDefinition());
            modeBar.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var tabs = new StackPanel { Orientation = Orientation.Horizontal };
            tabs.Children.Add(codeTab);
            tabs.Children.Add(visualTab);
            modeBar.Children.Add(tabs);
            var find = new StackPanel { Orientation = Orientation.Horizontal };
            find.Children.Add(search);
            find.Children.Add(Button("Find", FindNext));
            Grid.SetColumn(find, 1);
            modeBar.Children.Add(find);
            Grid.SetRow(modeBar, 1);
            panel.Children.Add(modeBar);

            var sourceGrid = new Grid();
            sourceGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(47) });
            sourceGrid.ColumnDefinitions.Add(new ColumnDefinition());
            lines.IsReadOnly = true;
            lines.IsTabStop = false;
            lines.BorderThickness = new Thickness(0);
            lines.Foreground = Theme.Brush("#687C9E");
            lines.FontFamily = new FontFamily("Consolas");
            lines.FontSize = 13;
            lines.Padding = new Thickness(0, 14, 8, 14);
            lines.TextAlignment = TextAlignment.Right;
            lines.VerticalScrollBarVisibility = ScrollBarVisibility.Hidden;
            sourceGrid.Children.Add(lines);
            code.AcceptsReturn = true;
            code.AcceptsTab = true;
            code.MaxLength = 524288;
            code.FontFamily = new FontFamily("Consolas");
            code.FontSize = 13;
            code.Padding = new Thickness(10, 14, 14, 14);
            code.BorderThickness = new Thickness(0);
            code.VerticalScrollBarVisibility = ScrollBarVisibility.Auto;
            code.HorizontalScrollBarVisibility = ScrollBarVisibility.Auto;
            SpellCheck.SetIsEnabled(code, false);
            code.AddHandler(ScrollViewer.ScrollChangedEvent, new ScrollChangedEventHandler((sender, args) => lines.ScrollToVerticalOffset(args.VerticalOffset)));
            Grid.SetColumn(code, 1);
            sourceGrid.Children.Add(code);
            codePanel = new Border { Child = sourceGrid, BorderThickness = new Thickness(1), BorderBrush = Theme.Brush("#263652"), CornerRadius = new CornerRadius(10), ClipToBounds = true };
            Grid.SetRow(editorHost, 2);
            panel.Children.Add(editorHost);
            var bottom = new StackPanel { Margin = new Thickness(0, 12, 0, 0) };
            var statusRow = new DockPanel();
            DockPanel.SetDock(codePosition, Dock.Right);
            statusRow.Children.Add(codePosition);
            statusRow.Children.Add(compileStatus);
            bottom.Children.Add(statusRow);
            bottom.Children.Add(liveApply);
            var controls = new Grid { Margin = new Thickness(0, 14, 0, 0) };
            controls.ColumnDefinitions.Add(new ColumnDefinition());
            controls.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var buttons = new StackPanel { Orientation = Orientation.Horizontal };
            buttons.Children.Add(Button("✓  Check script", CheckScriptAsync));
            buttons.Children.Add(inject);
            controls.Children.Add(buttons);
            Grid.SetColumn(stop, 1);
            controls.Children.Add(stop);
            bottom.Children.Add(controls);
            activity.Margin = new Thickness(2, 12, 2, 0);
            bottom.Children.Add(activity);
            Grid.SetRow(bottom, 3);
            panel.Children.Add(bottom);
            return Theme.Card(panel, new Thickness(20));
        }

        private UIElement BuildMonitor()
        {
            var scroll = new ScrollViewer { VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled };
            var stack = new StackPanel();
            stack.Children.Add(Theme.Label("LIVE SESSION", 11, Theme.Muted));
            liveStatus.Margin = new Thickness(0, 11, 0, 10);
            liveStatus.FontWeight = FontWeights.SemiBold;
            stack.Children.Add(liveStatus);
            stack.Children.Add(metrics);
            timing.Margin = new Thickness(0, 5, 0, 0);
            stack.Children.Add(timing);
            stack.Children.Add(new Border { Height = 1, Background = Theme.Brush("#26344D"), Margin = new Thickness(0, 20, 0, 20) });
            stack.Children.Add(Theme.Label("CONTROLLERS", 11, Theme.Muted));
            devices.Margin = new Thickness(0, 12, 0, 0);
            devices.Children.Add(Theme.Label("Choose Inject to connect to the driver. Editing never captures inputs.", 12, Theme.Muted));
            stack.Children.Add(devices);
            stack.Children.Add(new Border { Height = 1, Background = Theme.Brush("#26344D"), Margin = new Thickness(0, 20, 0, 20) });
            stack.Children.Add(Theme.Label("DIAGNOSTICS", 11, Theme.Muted));
            diagnostics.Margin = new Thickness(0, 12, 0, 10);
            diagnostics.Text = "Application ready.\n\nNo active interception.\n\nDriver access may require administrator rights. The UAC prompt appears only when you choose Inject.";
            stack.Children.Add(diagnostics);
            stack.Children.Add(Button("Copy diagnostics", CopyDiagnostics));
            stack.Children.Add(new TextBlock { Text = "Stop releases the capture and restores normal controller input. Closing the app also stops the session.", FontSize = 11, Foreground = Theme.Muted, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 17, 0, 0) });
            scroll.Content = stack;
            return Theme.Card(scroll, new Thickness(18));
        }

        private static Button Button(string content, Action action, bool primary = false)
        {
            var button = new Button { Content = content, Margin = new Thickness(0, 0, 7, 0), Style = (Style)Application.Current.FindResource(primary ? "PrimaryButton" : "QuietButton") };
            button.Click += (sender, args) => { try { action(); } catch (Exception exception) { MessageBox.Show(exception.Message, "GamePad Remapper", MessageBoxButton.OK, MessageBoxImage.Error); } };
            return button;
        }

        private void LoadLibrary()
        {
            try
            {
                Directory.CreateDirectory(libraryDirectory);
                string examples = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Scripts");
                if (Directory.Exists(examples))
                    foreach (string source in Directory.EnumerateFiles(examples, "*.igc"))
                    {
                        string target = Path.Combine(libraryDirectory, Path.GetFileName(source));
                        if (!File.Exists(target) && !File.Exists(target + ".bak")) File.Copy(source, target, false);
                    }
                synchronizing = true;
                library.Items.Clear();
                var scriptPaths = Directory.EnumerateFiles(libraryDirectory, "*.igc").ToList();
                if (currentPath != null && File.Exists(currentPath) && !scriptPaths.Contains(currentPath, StringComparer.OrdinalIgnoreCase)) scriptPaths.Add(currentPath);
                foreach (string path in scriptPaths.OrderBy(Path.GetFileName, StringComparer.OrdinalIgnoreCase))
                {
                    var item = new ListBoxItem { Tag = path, ToolTip = path };
                    var label = new StackPanel();
                    label.Children.Add(Theme.Label(Path.GetFileNameWithoutExtension(path), 13));
                    label.Children.Add(new TextBlock { Text = ".igc  ·  local script", FontSize = 10, Foreground = Theme.Muted, Margin = new Thickness(0, 5, 0, 0) });
                    item.Content = label;
                    library.Items.Add(item);
                    if (String.Equals(path, currentPath, StringComparison.OrdinalIgnoreCase)) library.SelectedItem = item;
                }
                if (library.Items.Count == 0) library.Items.Add(new ListBoxItem { Content = Theme.Label("No saved scripts yet", 12, Theme.Muted), IsEnabled = false });
            }
            catch (Exception exception) { operationError = "Script library: " + exception.Message; }
            finally { synchronizing = false; }
        }

        private void LibrarySelectionChanged(object sender, SelectionChangedEventArgs args)
        {
            if (synchronizing) return;
            var selected = library.SelectedItem as ListBoxItem;
            if (selected == null || !(selected.Tag is string)) return;
            string path = (string)selected.Tag;
            if (String.Equals(path, currentPath, StringComparison.OrdinalIgnoreCase)) return;
            if (!ConfirmDiscard()) { LoadLibrary(); return; }
            OpenPath(path);
        }

        internal void OpenPath(string path)
        {
            SetSource(ScriptStorage.Read(path), Path.GetFullPath(path), false);
            LoadLibrary();
            footer.Text = "Opened " + Path.GetFileName(path);
        }

        private void SetSource(string source, string path, bool modified)
        {
            currentPath = path;
            savedSource = modified ? "" : source;
            code.Text = source;
            if (visualMode) ShowVisual();
            UpdateTitle();
        }

        private void OnSourceChanged()
        {
            string source = code.Text;
            int count = 1;
            for (int i = 0; i < source.Length; ++i) if (source[i] == '\n') ++count;
            if (count != lastLineCount)
            {
                var numbering = new StringBuilder(count * 4);
                for (int i = 1; i <= count; ++i) { if (i > 1) numbering.Append('\n'); numbering.Append(i); }
                lines.Text = numbering.ToString();
                lastLineCount = count;
                lines.ScrollToVerticalOffset(code.VerticalOffset);
            }
            UpdateTitle();
            if (!synchronizing) { compileStatus.Text = "Edited · check or inject to apply"; compileStatus.Foreground = Theme.Muted; }
            QueueAutomaticValidation();
        }

        private void UpdateTitle()
        {
            string name = currentPath == null ? "Untitled.igc" : Path.GetFileName(currentPath);
            bool dirty = !String.Equals(code.Text, savedSource, StringComparison.Ordinal);
            title.Text = name + (dirty ? "  •" : "");
            Title = name + (dirty ? " *" : "") + " — GamePad Remapper";
        }

        private void NewScript()
        {
            if (busy) return;
            if (!ConfirmDiscard()) return;
            SetSource(EmptyScript, null, true);
            synchronizing = true;
            library.SelectedIndex = -1;
            synchronizing = false;
        }

        private void OpenScript()
        {
            if (busy) return;
            if (!ConfirmDiscard()) return;
            var dialog = new OpenFileDialog { Filter = "IGC controller scripts (*.igc)|*.igc", InitialDirectory = libraryDirectory, CheckFileExists = true };
            if (dialog.ShowDialog(this) == true) OpenPath(dialog.FileName);
        }

        private bool SaveScript(bool saveAs)
        {
            if (!CommitFocusedVisualField()) return false;
            string path = currentPath;
            if (path == null || saveAs)
            {
                var dialog = new SaveFileDialog { Filter = "IGC controller scripts (*.igc)|*.igc", DefaultExt = ".igc", AddExtension = true, InitialDirectory = libraryDirectory, FileName = path == null ? "My controller.igc" : Path.GetFileName(path) };
                if (dialog.ShowDialog(this) != true) return false;
                path = dialog.FileName;
            }
            ScriptStorage.Write(path, code.Text);
            currentPath = path;
            savedSource = code.Text;
            LoadLibrary();
            UpdateTitle();
            DeleteOwnDraft();
            footer.Text = "Saved " + Path.GetFileName(path) + " · previous version kept in .bak";
            return true;
        }

        private bool ConfirmDiscard()
        {
            if (!CommitFocusedVisualField()) return false;
            if (String.Equals(code.Text, savedSource, StringComparison.Ordinal)) return true;
            var result = MessageBox.Show(this, "Save your changes before continuing?", "Unsaved script", MessageBoxButton.YesNoCancel, MessageBoxImage.Question);
            return result == MessageBoxResult.No || (result == MessageBoxResult.Yes && SaveScript(false));
        }

        internal void ShowCode()
        {
            if (!CommitFocusedVisualField()) return;
            visualMode = false;
            editorHost.Content = codePanel;
            codeTab.Background = Theme.Brush("#353054");
            visualTab.Background = Brushes.Transparent;
            Theme.Reveal(editorHost);
        }

        internal bool ShowVisual()
        {
            if (!CommitFocusedVisualField()) return false;
            try { visualDocument = VisualDocument.Parse(code.Text); }
            catch (Exception exception) { SetError(exception); ShowCode(); return false; }
            visualMode = true;
            RebuildVisual();
            editorHost.Content = new ScrollViewer { Content = visualContent, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled, Padding = new Thickness(0, 0, 10, 0) };
            visualTab.Background = Theme.Brush("#353054");
            codeTab.Background = Brushes.Transparent;
            Theme.Reveal(editorHost);
            return true;
        }

        private void RebuildVisual()
        {
            RefreshEditorControlNames();
            invalidVisualInputs.Clear();
            visualContent.Children.Clear();
            visualContent.Children.Add(new Border { Background = Theme.Brush("#202D3C"), CornerRadius = new CornerRadius(9), Padding = new Thickness(13), Margin = new Thickness(0, 0, 0, 16), Child = Theme.Label(visualDocument.HasAdvancedCode ? "Custom code is preserved. These cards edit the visual section; advanced code stays available in Code." : "Build your controller behavior with cards. Every change is written to the same IGC script.", 12, Theme.Mint) });
            Section("INPUT RULES", "+ Add rule", () => { visualDocument.Rules.Add(new VisualRule { Kind = VisualRuleKind.Remap, Source = DefaultButton(), Target = "EAST", Value = 100, Enabled = true }); CommitVisual(true); }, visualDocument.Rules.Count < 256);
            foreach (var rule in visualDocument.Rules.ToArray())
            {
                var fields = new StackPanel();
                var row = new DockPanel();
                var enabled = new CheckBox { IsChecked = rule.Enabled, Content = "Enabled" };
                enabled.Click += (sender, args) => { rule.Enabled = enabled.IsChecked == true; CommitVisual(false); };
                DockPanel.SetDock(enabled, Dock.Right);
                row.Children.Add(enabled);
                var kind = new ComboBox { Width = 145, HorizontalAlignment = HorizontalAlignment.Left, ItemsSource = Enum.GetValues(typeof(VisualRuleKind)), SelectedItem = rule.Kind };
                kind.SelectionChanged += (sender, args) => { rule.Kind = (VisualRuleKind)kind.SelectedItem; if (rule.Kind == VisualRuleKind.Deadzone && (rule.Value < 0 || rule.Value > 99)) rule.Value = 8; CommitVisual(true); };
                row.Children.Add(kind);
                fields.Children.Add(row);
                var values = new WrapPanel { Margin = new Thickness(0, 10, 0, 0) };
                values.Children.Add(ControlPicker("From / control", rule.Source, value => { rule.Source = value; CommitVisual(false); }));
                if (rule.Kind == VisualRuleKind.Remap) values.Children.Add(ControlPicker("To", rule.Target, value => { rule.Target = value; CommitVisual(false); }));
                if (rule.Kind == VisualRuleKind.Deadzone || rule.Kind == VisualRuleKind.Sensitivity)
                    values.Children.Add(NumberField(rule.Kind == VisualRuleKind.Deadzone ? "Deadzone 0–99%" : "Sensitivity 0–1000%", rule.Value, value => { rule.Value = value; CommitVisual(false); }));
                values.Children.Add(Button("Remove", () => { visualDocument.Rules.Remove(rule); CommitVisual(true); }));
                fields.Children.Add(values);
                visualContent.Children.Add(VisualCard(fields));
            }
            if (visualDocument.Rules.Count == 0) EmptyHint("No visual rules. Add a rule to remap a button or adjust an axis.");
            BuildMacroEditors();
            Section("CONSTANTS", "+ Add constant", () => { visualDocument.Constants.Add(new VisualConstant { Name = "SETTING_" + (visualDocument.Constants.Count + 1), Value = 50 }); CommitVisual(true); }, visualDocument.Constants.Count < 256);
            foreach (var constant in visualDocument.Constants.ToArray())
            {
                var row = new WrapPanel();
                row.Children.Add(TextField("Name", constant.Name, value => { constant.Name = value; CommitVisual(false); }, 160));
                row.Children.Add(NumberField("Value", constant.Value, value => { constant.Value = value; CommitVisual(false); }));
                row.Children.Add(Button("Remove", () => { visualDocument.Constants.Remove(constant); CommitVisual(true); }));
                visualContent.Children.Add(VisualCard(row));
            }
        }

        private void Section(string text, string action, Action handler, bool canAdd = true)
        {
            var row = new DockPanel { Margin = new Thickness(0, 10, 0, 11) };
            var add = Button(action, handler);
            add.IsEnabled = canAdd;
            DockPanel.SetDock(add, Dock.Right);
            row.Children.Add(add);
            var label = Theme.Label(text, 11, Theme.Muted);
            label.VerticalAlignment = VerticalAlignment.Center;
            row.Children.Add(label);
            visualContent.Children.Add(row);
        }

        private void EmptyHint(string value) { visualContent.Children.Add(new TextBlock { Text = value, TextWrapping = TextWrapping.Wrap, Foreground = Theme.Muted, FontSize = 12, Margin = new Thickness(0, 0, 0, 18) }); }
        private static Border VisualCard(UIElement child) { return new Border { Child = child, CornerRadius = new CornerRadius(10), Padding = new Thickness(13), Margin = new Thickness(0, 0, 0, 10), Background = Theme.Raised, BorderBrush = Theme.Brush("#2B3B58"), BorderThickness = new Thickness(1) }; }
        private static StackPanel Field(string text, UIElement input, double width)
        {
            var field = new StackPanel { Width = width, Margin = new Thickness(0, 0, 10, 0) };
            field.Children.Add(new TextBlock { Text = text, FontSize = 10, Foreground = Theme.Muted, Margin = new Thickness(0, 0, 0, 4) });
            field.Children.Add(input);
            return field;
        }

        private UIElement ControlPicker(string text, string value, Action<string> set)
        {
            var selector = new ComboBox { IsEditable = true, ItemsSource = editorControlNames, Text = value, ToolTip = "Reported controls appear first while connected. SOUTH = Xbox A / PlayStation Cross; EAST = B / Circle. Generic HID: use reported AXIS_1, BUTTON_1, TRIGGER_1…" };
            selector.LostKeyboardFocus += (sender, args) => set(selector.Text.Trim());
            selector.SelectionChanged += (sender, args) => { var selected = selector.SelectedItem as string; if (selected != null) set(selected); };
            return Field(text, selector, 111);
        }

        private static UIElement TextField(string text, string value, Action<string> set, double width)
        {
            var input = new TextBox { Text = value };
            input.LostKeyboardFocus += (sender, args) => set(input.Text.Trim());
            return Field(text, input, width);
        }

        private UIElement NumberField(string text, int value, Action<int> set)
        {
            var input = new TextBox { Text = value.ToString(CultureInfo.InvariantCulture) };
            input.LostKeyboardFocus += (sender, args) => { int parsed; if (Int32.TryParse(input.Text, NumberStyles.Integer, CultureInfo.InvariantCulture, out parsed)) { invalidVisualInputs.Remove(input); set(parsed); input.BorderBrush = Theme.Brush("#334461"); } else { invalidVisualInputs.Add(input); input.BorderBrush = Theme.Danger; compileStatus.Text = "Enter a whole number for " + text + "."; compileStatus.Foreground = Theme.Danger; } };
            return Field(text, input, 91);
        }

        private void CommitVisual(bool rebuild)
        {
            try
            {
                string result = visualDocument.BuildSource();
                visualCommitFailed = false;
                synchronizing = true;
                code.Text = result;
                compileStatus.Text = "Visual changes saved in code · inject to apply";
                compileStatus.Foreground = Theme.Muted;
                if (rebuild) RebuildVisual();
                UpdateMacroSummaries();
            }
            catch (Exception exception) { visualCommitFailed = true; SetError(exception); }
            finally { synchronizing = false; }
        }

        private ScriptProgram Compile(bool announce)
        {
            try
            {
                var result = ScriptCompiler.Compile(code.Text);
                if (announce) { compileStatus.Text = "✓ Script valid · ready to inject"; compileStatus.Foreground = Theme.Mint; footer.Text = "Compilation successful. Controller capture has not changed."; }
                operationError = "";
                return result;
            }
            catch (Exception exception) { SetError(exception); return null; }
        }

        private async Task<ScriptProgram> CompileInBackground()
        {
            string source = code.Text;
            long version = compilation.Version;
            try
            {
                var result = await compilation.CompileAsync(source, version);
                if (!compilation.IsCurrent(result) || !String.Equals(source, code.Text, StringComparison.Ordinal)) return null;
                if (result.Error != null) { SetError(result.Error); return null; }
                operationError = "";
                compileStatus.Text = "✓ Script valid · ready to inject";
                compileStatus.Foreground = Theme.Mint;
                return result.Program;
            }
            catch (Exception exception) { SetError(exception); return null; }
        }

        private async void CheckScriptAsync()
        {
            await CheckScriptTask();
        }

        internal async Task<bool> CheckScriptTask()
        {
            if (busy) return false;
            if (!CommitFocusedVisualField()) return false;
            SetBusy(true);
            footer.Text = "Checking script…";
            try
            {
                bool valid = await CompileInBackground() != null;
                if (valid) footer.Text = "Compilation successful. Controller capture has not changed.";
                return valid;
            }
            finally { SetBusy(false); }
        }

        private void SetError(Exception exception, bool locate = true)
        {
            operationError = exception.Message;
            var scriptError = exception as ScriptException;
            compileStatus.Text = scriptError == null ? exception.Message : "Line " + scriptError.Line + ", column " + scriptError.Column + ": " + exception.Message;
            compileStatus.Foreground = Theme.Danger;
            footer.Text = "Configuration not applied. Any running configuration stays active.";
            if (scriptError != null && locate)
            {
                string source = code.Text;
                int index = 0;
                for (int line = 1; line < scriptError.Line && index < source.Length; ++line)
                {
                    int next = source.IndexOf('\n', index);
                    if (next < 0) { index = source.Length; break; }
                    index = next + 1;
                }
                index = Math.Min(source.Length, index + Math.Max(0, scriptError.Column - 1));
                code.Select(index, 0);
                int visualLine = code.GetLineIndexFromCharacterIndex(index);
                if (visualLine >= 0 && code.IsVisible) code.ScrollToLine(visualLine);
            }
            diagnostics.Text = "Last operation failed\n" + compileStatus.Text + "\n\nNo partial configuration was applied.";
        }

        internal async void InjectAsync()
        {
            if (busy || preview) return;
            if (!CommitFocusedVisualField()) return;
            SetBusy(true);
            footer.Text = "Checking script…";
            try
            {
                ScriptProgram program = await CompileInBackground();
                if (program == null) return;
                if (!IsAdministrator()) { RelaunchElevated(); return; }
                footer.Text = "Connecting to Interceptor Gamepad Controller…";
                await Task.Run(() => service.Inject(program));
                activeSource = code.Text;
                operationError = "";
                footer.Text = liveApply.IsChecked == true ? "Configuration active · valid edits apply automatically" : "Configuration active · choose Inject to apply further edits";
                compileStatus.Text = "✓ Configuration applied atomically";
                compileStatus.Foreground = Theme.Mint;
            }
            catch (Exception exception) { SetError(exception); }
            finally { SetBusy(false); RefreshService(); }
        }

        private void RelaunchElevated()
        {
            string sessionDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "GamePadRemapper", "Sessions");
            Directory.CreateDirectory(sessionDirectory);
            string pending = Path.Combine(sessionDirectory, Guid.NewGuid().ToString("N") + ".igc");
            ScriptStorage.Write(pending, code.Text);
            try
            {
                string executable = Process.GetCurrentProcess().MainModule.FileName;
                var start = new ProcessStartInfo(executable, "--session " + Quote(pending) + " --origin " + Quote(currentPath ?? "") + " --inject" + (liveApply.IsChecked == true ? " --live-apply" : "")) { UseShellExecute = true, Verb = "runas", WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory };
                Process.Start(start);
                closeAllowed = true;
                Close();
            }
            catch (Win32Exception exception)
            {
                if (File.Exists(pending)) File.Delete(pending);
                if (exception.NativeErrorCode == 1223) { footer.Text = "Administrator prompt cancelled. Your script is still open; capture has not started."; return; }
                SetError(exception);
            }
        }

        internal void RestoreSession(string session, string origin)
        {
            string permitted = Path.GetFullPath(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "GamePadRemapper", "Sessions")) + Path.DirectorySeparatorChar;
            string full = Path.GetFullPath(session);
            if (!full.StartsWith(permitted, StringComparison.OrdinalIgnoreCase) || !String.Equals(Path.GetExtension(full), ".igc", StringComparison.OrdinalIgnoreCase)) throw new IOException("Invalid elevation session path.");
            string source = ScriptStorage.Read(full);
            string original = String.IsNullOrWhiteSpace(origin) ? null : Path.GetFullPath(origin);
            SetSource(source, original, true);
            if (original != null && File.Exists(original)) savedSource = ScriptStorage.Read(original);
            File.Delete(full);
            UpdateTitle();
        }

        private static string Quote(string value) { return "\"" + value.Replace("\"", "") + "\""; }
        private static bool IsAdministrator() { using (var identity = WindowsIdentity.GetCurrent()) return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator); }

        internal async void StopAsync()
        {
            if (busy || preview) return;
            SetBusy(true);
            try { await Task.Run(() => service.Stop()); operationError = ""; footer.Text = "Session stopped · normal controller input restored"; }
            catch (Exception exception) { SetError(exception); }
            finally { SetBusy(false); RefreshService(); }
        }

        private void SetBusy(bool value)
        {
            busy = value;
            code.IsReadOnly = value;
            visualContent.IsEnabled = !value;
            library.IsEnabled = !value;
            liveApply.IsEnabled = !value;
            inject.IsEnabled = !value && !preview;
            stop.IsEnabled = !value && service.IsRunning;
            activity.IsIndeterminate = value && !Theme.ReducedMotion;
            if (value && Theme.ReducedMotion) activity.Value = 50;
            if (!value) activity.Value = service.IsRunning ? 100 : 0;
        }

        private void RefreshService()
        {
            if (preview || busy) return;
            try
            {
                var snapshot = service.GetSnapshot();
                bool running = snapshot.Running;
                refreshTimer.Interval = TimeSpan.FromMilliseconds(running ? 150 : 1000);
                liveStatus.Text = running ? "●  Configuration active" : "○  Not capturing";
                liveStatus.Foreground = running ? Theme.Mint : Theme.Muted;
                subtitle.Text = running ? (String.Equals(activeSource, code.Text, StringComparison.Ordinal) ? "LIVE · this version is active" : "LIVE · edited version not applied") : "EDITING · no controller capture";
                metrics.Text = snapshot.Received.ToString("N0", CultureInfo.InvariantCulture) + " received  /  " + snapshot.Forwarded.ToString("N0", CultureInfo.InvariantCulture) + " forwarded\n" + snapshot.Changed.ToString("N0", CultureInfo.InvariantCulture) + " changed  /  " + snapshot.TimerOutputs.ToString("N0", CultureInfo.InvariantCulture) + " timed outputs";
                timing.Text = "Transform + forward: " + (running ? snapshot.ProcessingMicroseconds.ToString("0.0", CultureInfo.InvariantCulture) + " µs" : "—");
                stop.IsEnabled = !busy && running;
                if (!busy) activity.Value = snapshot.Received > lastReceived ? 100 : (running ? 16 : 0);
                lastReceived = snapshot.Received;
                string fingerprint = String.Join("|", snapshot.Devices.Select(device => device.Token + ":" + device.Name + ":" + device.Diagnostic + ":" + String.Join(",", device.Available ?? new bool[0]) + ":" + String.Join(",", device.Values ?? new int[0]) + ":" + String.Join(",", device.OutputValues ?? new int[0])));
                if (!String.Equals(fingerprint, lastDeviceFingerprint, StringComparison.Ordinal))
                {
                    lastDeviceFingerprint = fingerprint;
                    devices.Children.Clear();
                    foreach (var device in snapshot.Devices)
                    {
                        var card = new StackPanel();
                        card.Children.Add(Theme.Label(device.Name, 13));
                        int availableCount = device.Available == null ? 0 : device.Available.Count(value => value);
                        card.Children.Add(new TextBlock { Text = FamilyName(device.Family) + "  ·  " + availableCount + " controls available\n" + device.Token, FontSize = 10, Foreground = Theme.Muted, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 5, 0, 8) });
                        if (device.Family == 3) card.Children.Add(new TextBlock { Text = "Generic HID: use the reported AXIS / BUTTON / VALUE fields. Console face names apply only when a mapping is known.", FontSize = 10, Foreground = Theme.Muted, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 8) });
                        if (device.Values != null && device.Controls != null)
                        {
                            var values = new StringBuilder();
                            for (int i = 0; i < device.Values.Length && i < device.Controls.Length; ++i)
                            {
                                if (device.Available != null && i < device.Available.Length && !device.Available[i]) continue;
                                if (values.Length > 0) values.Append("  ");
                                values.Append(device.Controls[i]).Append(' ').Append(device.Values[i]);
                                if (device.OutputValues != null && i < device.OutputValues.Length && device.OutputValues[i] != device.Values[i]) values.Append(" → ").Append(device.OutputValues[i]);
                                if (values.Length > 200) { values.Append(" …"); break; }
                            }
                            card.Children.Add(Theme.Label(values.Length == 0 ? "Waiting for a physical report" : values.ToString(), 11, Theme.Mint));
                        }
                        if (!String.IsNullOrWhiteSpace(device.Diagnostic)) card.Children.Add(Theme.Label(device.Diagnostic, 10, Theme.Muted));
                        devices.Children.Add(VisualCard(card));
                    }
                    if (snapshot.Devices.Length == 0) devices.Children.Add(Theme.Label(running ? "Waiting for compatible controllers. Move a stick or press a button to see input." : "Choose Inject to connect to the driver. Editing never captures inputs.", 12, Theme.Muted));
                }
                if (++refreshTicks % 4 == 0 || lastDiagnostics == null || !String.IsNullOrWhiteSpace(snapshot.Error))
                {
                    string text = "GamePad Remapper 1.1\nInterceptor Gamepad Controller\n\n" + snapshot.Status + "\n\nReceived: " + snapshot.Received + "\nForwarded: " + snapshot.Forwarded + "\nSoftware processing: " + snapshot.ProcessingMicroseconds.ToString("0.00", CultureInfo.InvariantCulture) + " µs\nControllers: " + snapshot.Devices.Length + "\nAdministrator: " + IsAdministrator() + "\n\n" + snapshot.Error + (String.IsNullOrWhiteSpace(operationError) ? "" : "\nLast operation: " + operationError) + "\n\nDriver demo: 2 hours after the first capture per Windows boot.\nTiming excludes controller/USB/display latency.";
                    if (!String.Equals(lastDiagnostics, text, StringComparison.Ordinal)) { diagnostics.Text = text; lastDiagnostics = text; }
                }
            }
            catch (Exception exception) { operationError = exception.Message; diagnostics.Text = "Status unavailable: " + exception.Message; }
        }

        private static string FamilyName(uint family)
        {
            switch (family) { case 3: return "HID"; case 4: return "XUSB"; case 5: return "XInput HID"; case 6: return "GIP"; default: return "Controller · family " + family; }
        }

        private void ShowPreviewData()
        {
            liveStatus.Text = "●  Simulated preview";
            liveStatus.Foreground = Theme.Mint;
            metrics.Text = "Preview only · no physical inputs";
            timing.Text = "Software timing: not measured";
            devices.Children.Clear();
            var sample = new StackPanel();
            sample.Children.Add(Theme.Label("Demo controller", 13));
            sample.Children.Add(new TextBlock { Text = "SIMULATED · UI illustration only", Foreground = Theme.Muted, FontSize = 10, Margin = new Thickness(0, 6, 0, 8) });
            sample.Children.Add(Theme.Label("LX  0   LY  0   RX  0   RY  0\nLT  0   RT  0", 12, Theme.Mint));
            devices.Children.Add(VisualCard(sample));
            diagnostics.Text = "SIMULATED PREVIEW\n\nNo driver session exists.\nNo physical inputs are captured.\nNo latency results are generated.\n\nThis preview demonstrates the interface and script editor.";
            activity.Value = 35;
            footer.Text = "SIMULATED PREVIEW · driver disconnected · editing and compilation remain available";
        }

        private void InstallDriver()
        {
            string installer = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "InterceptorGamepadControllerSetup.exe");
            if (!File.Exists(installer)) installer = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "InterceptorSetup.exe");
            if (!File.Exists(installer)) installer = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Driver", "InterceptorSetup.exe");
            if (File.Exists(installer)) Process.Start(new ProcessStartInfo(installer) { UseShellExecute = true });
            else Process.Start(new ProcessStartInfo("https://github.com/TitanCSA/Interceptor-Gamepad-Controller/releases/latest") { UseShellExecute = true });
            footer.Text = "Driver setup opened. Follow its installation instructions, then choose Inject.";
        }

        private void CopyDiagnostics()
        {
            Clipboard.SetText(diagnostics.Text);
            footer.Text = "Diagnostics copied to clipboard";
        }

        private void FindNext()
        {
            ShowCode();
            if (visualMode) return;
            string term = search.Text;
            if (String.IsNullOrEmpty(term)) { search.Focus(); return; }
            int start = Math.Min(code.Text.Length, code.SelectionStart + Math.Max(1, code.SelectionLength));
            int index = code.Text.IndexOf(term, start, StringComparison.OrdinalIgnoreCase);
            if (index < 0) index = code.Text.IndexOf(term, StringComparison.OrdinalIgnoreCase);
            if (index < 0) { footer.Text = "No match for “" + term + "”"; return; }
            code.Select(index, term.Length);
            code.ScrollToLine(code.GetLineIndexFromCharacterIndex(index));
            code.Focus();
        }

        private void UpdateCaret()
        {
            int line = code.GetLineIndexFromCharacterIndex(code.CaretIndex);
            int start = line < 0 ? 0 : code.GetCharacterIndexFromLineIndex(line);
            codePosition.Text = "Ln " + (line + 1) + ", Col " + (code.CaretIndex - Math.Max(0, start) + 1);
        }

        private void EditorKeyDown(object sender, KeyEventArgs args)
        {
            if (args.Key == Key.Tab && Keyboard.Modifiers == ModifierKeys.None) { code.SelectedText = "    "; code.CaretIndex += code.SelectionLength; args.Handled = true; }
        }

        private void WindowKeyDown(object sender, KeyEventArgs args)
        {
            if (busy) return;
            if ((Keyboard.Modifiers & ModifierKeys.Control) == 0) return;
            switch (args.Key)
            {
                case Key.Enter: InjectAsync(); args.Handled = true; break;
                case Key.S: SaveScript((Keyboard.Modifiers & ModifierKeys.Shift) != 0); args.Handled = true; break;
                case Key.O: OpenScript(); args.Handled = true; break;
                case Key.N: NewScript(); args.Handled = true; break;
                case Key.F: search.Focus(); search.SelectAll(); args.Handled = true; break;
            }
        }

        private async void OnClosing(object sender, CancelEventArgs args)
        {
            if (closeAllowed) return;
            args.Cancel = true;
            if (closing || busy) { footer.Text = "Please wait for the current operation to finish."; return; }
            try
            {
                if (!ConfirmDiscard()) return;
                closing = true;
                SetBusy(true);
                refreshTimer.Stop();
                await Task.Run(() => service.Stop());
                try { await draftWrite; } catch (IOException) { } catch (UnauthorizedAccessException) { }
                DeleteOwnDraft();
                closeAllowed = true;
                Close();
            }
            catch (Exception exception) { closing = false; SetBusy(false); refreshTimer.Start(); SetError(exception); }
        }

        internal string Source { get { return code.Text; } set { code.Text = value; } }
        internal string DiagnosticText { get { return diagnostics.Text; } }
        internal bool IsDirty { get { return !String.Equals(code.Text, savedSource, StringComparison.Ordinal); } }
        internal bool CheckScript() { return Compile(true) != null; }
        internal void AddTestRule() { if (visualDocument == null) throw new InvalidOperationException("Open visual editor first."); visualDocument.Rules.Add(new VisualRule { Kind = VisualRuleKind.Invert, Source = "LY", Target = "LY", Enabled = true }); CommitVisual(true); }
        internal static bool HasControl(string control) { return ControlNames.Contains(control); }
        internal void TestSearch(string text) { search.Text = text; FindNext(); }
        internal int SelectionLength { get { return code.SelectionLength; } }
        internal void ResetPreview() { ShowPreviewData(); }
        internal void PreparePreview(string source)
        {
            string primary = Path.Combine(libraryDirectory, "Everyday controller.igc");
            ScriptStorage.Write(primary, source);
            ScriptStorage.Write(Path.Combine(libraryDirectory, "Camera comfort.igc"), "// Camera comfort\nmain { invert(RY); }\n");
            ScriptStorage.Write(Path.Combine(libraryDirectory, "Quick sequence.igc"), source);
            OpenPath(primary);
            search.Text = "";
            Compile(true);
            ResetPreview();
        }
        internal void ScrollToMacros()
        {
            var viewer = editorHost.Content as ScrollViewer;
            if (viewer == null) return;
            foreach (var block in Descendants<TextBlock>(visualContent))
                if (block.Text == "MACROS")
                {
                    viewer.ScrollToVerticalOffset(Math.Max(0, block.TransformToAncestor(visualContent).Transform(new Point(0, 0)).Y - 10));
                    break;
                }
        }
        private static IEnumerable<T> Descendants<T>(DependencyObject element) where T : DependencyObject
        {
            int count = VisualTreeHelper.GetChildrenCount(element);
            for (int index = 0; index < count; ++index)
            {
                var child = VisualTreeHelper.GetChild(element, index);
                var typed = child as T;
                if (typed != null) yield return typed;
                foreach (var descendant in Descendants<T>(child)) yield return descendant;
            }
        }
        internal void CloseForTest() { closeAllowed = true; Close(); }
    }
}
