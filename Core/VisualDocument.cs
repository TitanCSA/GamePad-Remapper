using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using System.Security.Cryptography;

namespace GamePadRemapper.Core
{
    public enum VisualRuleKind { Remap, Invert, Deadzone, Sensitivity, Block }
    public sealed class VisualRule
    {
        public VisualRuleKind Kind { get; set; }
        public string Source { get; set; } = "SOUTH";
        public string Target { get; set; } = "EAST";
        public int Value { get; set; }
        public bool Enabled { get; set; } = true;
    }
    public sealed class VisualConstant
    {
        public string Name { get; set; }
        public int Value { get; set; }
        internal string OriginalName;
        internal int OriginalStart = -1, OriginalLength;
    }
    public sealed class VisualMacroStep
    {
        public string Control { get; set; } = "SOUTH";
        public int Value { get; set; } = 100;
        public int DurationMilliseconds { get; set; } = 100;
    }
    public sealed class VisualMacro
    {
        public string Name { get; set; } = "Macro";
        public string Trigger { get; set; } = "SOUTH";
        public bool Enabled { get; set; } = true;
        public List<VisualMacroStep> Steps { get; } = new List<VisualMacroStep>();
    }
    public sealed class VisualDocument
    {
        private const string Begin = "// <igc-visual-v1>", End = "// </igc-visual-v1>";
        private const string CallPrefix = "/* @igc-visual-call */ ";
        private readonly string original;
        public List<VisualRule> Rules { get; } = new List<VisualRule>();
        public List<VisualConstant> Constants { get; } = new List<VisualConstant>();
        public List<VisualMacro> Macros { get; } = new List<VisualMacro>();
        public bool HasAdvancedCode { get; private set; }
        public bool ManagedSectionModified { get; private set; }
        private static readonly Regex IntegerDefines = new Regex(@"\bdefine\s+([A-Za-z_]\w*)\s*=\s*(-?(?:0[xX][0-9a-fA-F]+|[0-9]+))\s*;", RegexOptions.CultureInvariant);
        private VisualDocument(string source) { original = source; }
        public static VisualDocument Parse(string source)
        {
            if (source == null) throw new ArgumentNullException("source");
            if (source.Length > 524288) throw new ScriptException("A script may contain at most 512 KiB of text.");
            int begin = source.IndexOf(Begin, StringComparison.Ordinal), end = source.IndexOf(End, StringComparison.Ordinal);
            if ((begin < 0) != (end < 0) || begin >= 0 && end < begin) throw new ScriptException("Visual section markers are incomplete.");
            bool modified = false;
            if (begin >= 0)
            {
                int hashStart = begin + Begin.Length; while (hashStart < end && (source[hashStart] == '\r' || source[hashStart] == '\n')) hashStart++;
                int hashEnd = source.IndexOf('\n', hashStart); string hashLine = hashEnd < 0 ? "" : source.Substring(hashStart, hashEnd - hashStart).Trim();
                string content = hashEnd >= 0 && hashEnd < end ? source.Substring(hashEnd + 1, end - hashEnd - 1) : "";
                modified = !hashLine.StartsWith("// @hash ", StringComparison.Ordinal) || hashLine.Substring(9) != Hash(content);
            }
            // Hand edits in a generated block become ordinary advanced code. Never rebuild from stale visual metadata.
            string preserved = modified ? source.Replace(Begin, "// Advanced code (edited visual section)").Replace(End, "// End advanced code").Replace(CallPrefix, "/* Custom visual call */ ") : RemoveManagedSection(source);
            var doc = new VisualDocument(preserved) { ManagedSectionModified = modified };
            if (begin >= 0 && !modified)
            {
                if (end < begin || source.IndexOf(Begin, begin + Begin.Length, StringComparison.Ordinal) >= 0) throw new ScriptException("Invalid or duplicate visual section markers.");
                VisualMacro current = null;
                string[] lines = source.Substring(begin, end - begin).Split('\n');
                foreach (string raw in lines)
                {
                    string line = raw.Trim();
                    if (!line.StartsWith("// @", StringComparison.Ordinal)) continue;
                    string[] fields = line.Substring(4).Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                    if (fields.Length == 0) continue;
                    if (fields[0] == "hash") continue;
                    int number, enabled;
                    if (fields[0] == "rule" && fields.Length == 6)
                    {
                        VisualRuleKind kind;
                        if (!Enum.TryParse(fields[1], out kind) || !Enum.IsDefined(typeof(VisualRuleKind), kind) || !int.TryParse(fields[4], NumberStyles.Integer, CultureInfo.InvariantCulture, out number) || !int.TryParse(fields[5], out enabled) || enabled < 0 || enabled > 1) throw new ScriptException("Malformed visual rule metadata.");
                        doc.Rules.Add(new VisualRule { Kind = kind, Source = fields[2], Target = fields[3], Value = number, Enabled = enabled == 1 });
                    }
                    else if (fields[0] == "macro" && fields.Length == 4)
                    {
                        if (!int.TryParse(fields[3], out enabled) || enabled < 0 || enabled > 1) throw new ScriptException("Malformed visual macro metadata.");
                        current = new VisualMacro { Name = fields[1], Trigger = fields[2], Enabled = enabled == 1 }; doc.Macros.Add(current);
                    }
                    else if (fields[0] == "step" && fields.Length == 4 && current != null)
                    {
                        int duration; if (!int.TryParse(fields[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out number) || !int.TryParse(fields[3], out duration)) throw new ScriptException("Malformed visual macro step metadata.");
                        current.Steps.Add(new VisualMacroStep { Control = fields[1], Value = number, DurationMilliseconds = duration });
                    }
                    else if (fields[0] == "constant" && fields.Length == 3)
                    {
                        if (!int.TryParse(fields[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out number)) throw new ScriptException("Malformed visual constant metadata.");
                        doc.Constants.Add(new VisualConstant { Name = fields[1], Value = number });
                    }
                    else throw new ScriptException("Unknown or malformed visual metadata. Edit this file in code view to repair its managed section.");
                }
            }
            string withoutComments = MaskComments(doc.original);
            foreach (Match match in IntegerDefines.Matches(withoutComments))
            {
                string literal = match.Groups[2].Value; int value;
                if (!TryInteger(literal, out value)) continue;
                doc.Constants.Add(new VisualConstant { Name = match.Groups[1].Value, OriginalName = match.Groups[1].Value, Value = value, OriginalStart = match.Groups[2].Index, OriginalLength = match.Groups[2].Length });
            }
            // A plain main block and editable integer defines are represented fully. Other source is preserved as advanced code.
            string remaining = IntegerDefines.Replace(withoutComments, "");
            remaining = Regex.Replace(remaining, @"\bmain\s*\{\s*\}", "", RegexOptions.CultureInvariant);
            doc.HasAdvancedCode = !string.IsNullOrWhiteSpace(remaining);
            return doc;
        }
        private static bool TryInteger(string text, out int value)
        {
            bool negative = text.StartsWith("-", StringComparison.Ordinal); string digits = negative ? text.Substring(1) : text; uint number;
            bool hex = digits.StartsWith("0x", StringComparison.OrdinalIgnoreCase);
            if (!uint.TryParse(hex ? digits.Substring(2) : digits, hex ? NumberStyles.AllowHexSpecifier : NumberStyles.None, CultureInfo.InvariantCulture, out number)) { value = 0; return false; }
            value = unchecked((int)(negative ? 0u - number : number)); return true;
        }
        private static string RemoveManagedSection(string source)
        {
            int start = source.IndexOf(Begin, StringComparison.Ordinal), finish = source.IndexOf(End, StringComparison.Ordinal);
            if ((start < 0) != (finish < 0) || start >= 0 && finish < start) throw new ScriptException("Visual section markers are incomplete.");
            if (start >= 0) source = source.Remove(start, finish + End.Length - start);
            return Regex.Replace(source, @"/\* @igc-visual-call \*/ __igc_visual_apply(?:_[0-9]+)?\(\);", "", RegexOptions.CultureInvariant);
        }
        private static string Hash(string text)
        {
            using (var sha = SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(Encoding.UTF8.GetBytes(text.Replace("\r\n", "\n")))).Replace("-", "");
        }
        private static string MaskComments(string source)
        {
            char[] chars = source.ToCharArray(); bool line = false, block = false;
            for (int i = 0; i < chars.Length; i++)
            {
                char current = chars[i], next = i + 1 < chars.Length ? chars[i + 1] : '\0';
                if (!line && !block && current == '/' && next == '/') { line = true; chars[i] = ' '; }
                else if (!line && !block && current == '/' && next == '*') { block = true; chars[i] = ' '; }
                else if (line) { if (current == '\r' || current == '\n') line = false; else chars[i] = ' '; }
                else if (block)
                {
                    if (current == '*' && next == '/') { chars[i] = chars[i + 1] = ' '; i++; block = false; }
                    else if (current != '\r' && current != '\n') chars[i] = ' ';
                }
            }
            return new string(chars);
        }
        private static string Name(string name)
        {
            if (name == null || !Regex.IsMatch(name, @"\A[A-Za-z_][A-Za-z_0-9]{0,79}\z", RegexOptions.CultureInvariant) || name.StartsWith("__igc_", StringComparison.Ordinal)) throw new ScriptException("Use a valid name (letters, digits and underscores; no __igc_ prefix).");
            return name;
        }
        private static string Control(string control)
        {
            int id = Controls.Parse(control); if (id < 0 || id >= 22 && id < 32) throw new ScriptException("Unknown control '" + control + "'."); return Controls.Names[id];
        }
        public string BuildSource()
        {
            if (Rules.Count > 256 || Macros.Count > 32 || Constants.Count > 256) throw new ScriptException("Visual document exceeds 256 rules/constants or 32 macros.");
            string advanced = original;
            var replacements = new List<VisualConstant>(); var names = new HashSet<string>(StringComparer.Ordinal);
            foreach (VisualConstant item in Constants)
            {
                if (item == null) throw new ScriptException("A visual constant is missing."); Name(item.Name); if (!names.Add(item.Name)) throw new ScriptException("Duplicate constant name '" + item.Name + "'.");
                if (item.OriginalStart >= 0)
                {
                    if (item.Name != item.OriginalName) throw new ScriptException("A constant in custom code cannot be renamed in visual view; rename it and its references in code view.");
                    replacements.Add(item);
                }
            }
            replacements.Sort((a, b) => b.OriginalStart.CompareTo(a.OriginalStart));
            foreach (VisualConstant item in replacements) advanced = advanced.Remove(item.OriginalStart, item.OriginalLength).Insert(item.OriginalStart, item.Value.ToString(CultureInfo.InvariantCulture));
            string maskedAdvanced = MaskComments(advanced), macroPrefix = "__igc_macro_"; int macroGeneration = 0;
            while (Regex.IsMatch(maskedAdvanced, @"\b" + macroPrefix + @"[0-9]+\b", RegexOptions.CultureInvariant)) macroPrefix = "__igc_macro_" + (++macroGeneration).ToString(CultureInfo.InvariantCulture) + "_";
            var metadata = new StringBuilder(); var body = new StringBuilder(); var comboSource = new StringBuilder();
            foreach (VisualConstant item in Constants) if (item.OriginalStart < 0)
            {
                metadata.Append("// @constant ").Append(item.Name).Append(' ').Append(item.Value.ToString(CultureInfo.InvariantCulture)).AppendLine();
                metadata.Append("define ").Append(item.Name).Append(" = ").Append(item.Value.ToString(CultureInfo.InvariantCulture)).AppendLine(";");
            }
            foreach (VisualRule rule in Rules)
            {
                if (rule == null || !Enum.IsDefined(typeof(VisualRuleKind), rule.Kind)) throw new ScriptException("Invalid visual rule.");
                string control = Control(rule.Source), target = Control(rule.Target ?? "SOUTH");
                metadata.Append("// @rule ").Append(rule.Kind).Append(' ').Append(control).Append(' ').Append(target).Append(' ').Append(rule.Value.ToString(CultureInfo.InvariantCulture)).Append(' ').AppendLine(rule.Enabled ? "1" : "0");
                if (!rule.Enabled) continue;
                switch (rule.Kind)
                {
                    case VisualRuleKind.Remap: body.Append("    remap(").Append(control).Append(", ").Append(target).AppendLine(");"); break;
                    case VisualRuleKind.Invert: body.Append("    invert(").Append(control).AppendLine(");"); break;
                    case VisualRuleKind.Deadzone: if (rule.Value < 0 || rule.Value > 99) throw new ScriptException("Deadzone must be between 0 and 99 percent."); body.Append("    deadzone(").Append(control).Append(", ").Append(rule.Value).AppendLine(");"); break;
                    case VisualRuleKind.Sensitivity: if (rule.Value < 0 || rule.Value > 1000) throw new ScriptException("Sensitivity must be between 0 and 1000 percent."); body.Append("    sensitivity(").Append(control).Append(", ").Append(rule.Value).AppendLine(");"); break;
                    case VisualRuleKind.Block: body.Append("    set_val(").Append(control).Append(", ").Append(Controls.Parse(control) >= 192 && Controls.Parse(control) < 208 ? "-1" : "0").AppendLine(");"); break;
                }
            }
            var macroNames = new HashSet<string>(StringComparer.Ordinal);
            for (int m = 0; m < Macros.Count; m++)
            {
                VisualMacro macro = Macros[m]; if (macro == null) throw new ScriptException("A macro is missing.");
                string label = Name(macro.Name), trigger = Control(macro.Trigger); if (!macroNames.Add(label)) throw new ScriptException("Duplicate macro name '" + label + "'.");
                if (macro.Steps.Count > 256) throw new ScriptException("A visual macro may contain at most 256 steps.");
                metadata.Append("// @macro ").Append(label).Append(' ').Append(trigger).Append(' ').AppendLine(macro.Enabled ? "1" : "0");
                string comboName = macroPrefix + m.ToString(CultureInfo.InvariantCulture);
                if (macro.Enabled) { body.Append("    if (event_press(").Append(trigger).Append(")) combo_run(").Append(comboName).AppendLine(");"); comboSource.Append("combo ").Append(comboName).AppendLine(" {"); }
                foreach (VisualMacroStep step in macro.Steps)
                {
                    if (step == null) throw new ScriptException("A macro step is missing."); string control = Control(step.Control);
                    if (step.DurationMilliseconds < 1 || step.DurationMilliseconds > 60000) throw new ScriptException("A macro step duration must be between 1 and 60,000 ms.");
                    metadata.Append("// @step ").Append(control).Append(' ').Append(step.Value.ToString(CultureInfo.InvariantCulture)).Append(' ').Append(step.DurationMilliseconds).AppendLine();
                    if (macro.Enabled) comboSource.Append("    set_val(").Append(control).Append(", ").Append(step.Value.ToString(CultureInfo.InvariantCulture)).Append("); wait(").Append(step.DurationMilliseconds).AppendLine(");");
                }
                if (macro.Enabled) comboSource.AppendLine("}");
            }
            string helper = "__igc_visual_apply"; int suffix = 0;
            while (Regex.IsMatch(MaskComments(advanced), @"\b" + helper + @"\b", RegexOptions.CultureInvariant)) helper = "__igc_visual_apply_" + (++suffix).ToString(CultureInfo.InvariantCulture);
            string callMarker = CallPrefix + helper + "();";
            // Add one marked call to the existing main. Only this insertion and the managed section are owned by visual view.
            var tokens = new Lexer(advanced).Scan(); int mainStart = -1, mainEnd = -1;
            for (int i = 0; i < tokens.Count - 1; i++) if (tokens[i].Text == "main" && tokens[i + 1].Text == "{")
            {
                int nesting = 1;
                for (int j = i + 2; j < tokens.Count; j++)
                {
                    if (tokens[j].Text == "{") nesting++;
                    else if (tokens[j].Text == "}" && --nesting == 0) { mainEnd = Offset(advanced, tokens[j].Line, tokens[j].Column); break; }
                }
                mainStart = i; break;
            }
            if (mainStart < 0) advanced += Environment.NewLine + "main { " + callMarker + " }" + Environment.NewLine;
            else { if (mainEnd < 0) throw new ScriptException("The main block is incomplete."); advanced = advanced.Insert(mainEnd, Environment.NewLine + "    " + callMarker + Environment.NewLine); }
            string content = metadata + "function " + helper + "() {" + Environment.NewLine + body + "}" + Environment.NewLine + comboSource;
            string result = advanced.TrimEnd() + Environment.NewLine + Environment.NewLine + Begin + Environment.NewLine + "// @hash " + Hash(content) + Environment.NewLine + content + End + Environment.NewLine;
            ScriptCompiler.Compile(result); return result;
        }
        private static int Offset(string source, int line, int column)
        {
            int offset = 0; for (int n = 1; n < line; n++) { offset = source.IndexOf('\n', offset); if (offset < 0) throw new ScriptException("Invalid source line."); offset++; } return offset + column - 1;
        }
    }
}
