using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using System.Security.Cryptography;
using System.Web.Script.Serialization;
using System.Reflection;

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
        public VisualBlend Blend { get; set; } = VisualBlend.Replace;
    }
    public enum VisualConditionKind { All, Any, Not, Held, Pressed, Released, Compare, AbsoluteCompare, HeldFor, DoubleTap, Expression }
    public enum VisualMacroActivation { OnEnter, WhileTrue, OnExit, Toggle }
    public enum VisualMacroKind { Sequence, AxisPattern }
    public enum VisualPatternShape { FigureEight, Circle, Sine }
    public enum VisualBlend { Add, Replace }
    public enum VisualPatternCenter { DynamicPhysical, Activation, Zero }
    public sealed class VisualCondition
    {
        public VisualConditionKind Kind { get; set; } = VisualConditionKind.Held;
        public string Control { get; set; } = "SOUTH";
        public string Operator { get; set; } = ">";
        public int Value { get; set; } = 50;
        public int Milliseconds { get; set; } = 250;
        public string Expression { get; set; } = "";
        public List<VisualCondition> Children { get; set; } = new List<VisualCondition>();
    }
    public sealed class VisualAxisPattern
    {
        public string TargetX { get; set; } = "LX";
        public string TargetY { get; set; } = "LY";
        public VisualPatternShape Shape { get; set; } = VisualPatternShape.FigureEight;
        public int AmplitudeX { get; set; } = 25;
        public int AmplitudeY { get; set; } = 25;
        public int PeriodMilliseconds { get; set; } = 1000;
        public int TickMilliseconds { get; set; } = 4;
        public VisualBlend Blend { get; set; } = VisualBlend.Add;
        public VisualPatternCenter Center { get; set; } = VisualPatternCenter.DynamicPhysical;
    }
    public sealed class VisualMacro
    {
        public string Name { get; set; } = "Macro";
        public string Trigger { get; set; } = "SOUTH";
        public bool Enabled { get; set; } = true;
        public VisualCondition Condition { get; set; }
        public VisualMacroActivation Activation { get; set; } = VisualMacroActivation.OnEnter;
        public VisualMacroKind Kind { get; set; } = VisualMacroKind.Sequence;
        public VisualAxisPattern Pattern { get; set; } = new VisualAxisPattern();
        public List<VisualMacroStep> Steps { get; set; } = new List<VisualMacroStep>();
    }
    public sealed class VisualDocument
    {
        private const string Begin = "// <igc-visual-v1>", End = "// </igc-visual-v1>";
        private const string CallPrefix = "/* @igc-visual-call */ ";
        private readonly string original;
        private sealed class Payload
        {
            public Payload() { }
            public int Version { get; set; }
            public List<VisualRule> Rules { get; set; }
            public List<VisualConstant> Constants { get; set; }
            public List<VisualMacro> Macros { get; set; }
        }
        public List<VisualRule> Rules { get; } = new List<VisualRule>();
        public List<VisualConstant> Constants { get; } = new List<VisualConstant>();
        public List<VisualMacro> Macros { get; } = new List<VisualMacro>();
        public bool HasAdvancedCode { get; private set; }
        public bool ManagedSectionModified { get; private set; }
        private static readonly Dictionary<Type, Dictionary<string, Type>> MetadataFields = CreateMetadataFields();
        // Only names representable by the visual editor are owned here. Other valid
        // language identifiers remain ordinary advanced source, unchanged.
        private static readonly Regex IntegerDefines = new Regex(@"\bdefine\s+((?!__igc_)[A-Za-z_][A-Za-z_0-9]{0,79})\s*=\s*(-?(?:0[xX][0-9a-fA-F]+|[0-9]+))\s*;", RegexOptions.CultureInvariant);
        private VisualDocument(string source) { original = source; }
        public static VisualDocument Parse(string source)
        {
            if (source == null) throw new ArgumentNullException("source");
            if (source.Length > 524288) throw new ScriptException("A script may contain at most 512 KiB of text.");
            int begin = source.IndexOf(Begin, StringComparison.Ordinal), end = source.IndexOf(End, StringComparison.Ordinal);
            if ((begin < 0) != (end < 0) || begin >= 0 && end < begin) throw new ScriptException("Visual section markers are incomplete.");
            if (begin >= 0 && (source.IndexOf(Begin, begin + Begin.Length, StringComparison.Ordinal) >= 0 || source.IndexOf(End, end + End.Length, StringComparison.Ordinal) >= 0)) throw new ScriptException("Invalid or duplicate visual section markers.");
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
                bool hasPayload = false;
                string[] lines = source.Substring(begin, end - begin).Split('\n');
                foreach (string raw in lines)
                {
                    string line = raw.Trim();
                    if (!line.StartsWith("// @", StringComparison.Ordinal)) continue;
                    string[] fields = line.Substring(4).Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                    if (fields.Length == 0) continue;
                    if (fields[0] == "hash") continue;
                    int number, enabled;
                    if (fields[0] == "document" && fields.Length == 2)
                    {
                        if (hasPayload || doc.Rules.Count != 0 || doc.Macros.Count != 0 || doc.Constants.Count != 0) throw new ScriptException("Duplicate visual document metadata.");
                        Payload payload = ReadPayload(fields[1]); hasPayload = true;
                        doc.Rules.AddRange(payload.Rules); doc.Macros.AddRange(payload.Macros); doc.Constants.AddRange(payload.Constants);
                    }
                    else if (hasPayload) throw new ScriptException("Mixed visual document metadata is not supported.");
                    else if (fields[0] == "rule" && fields.Length == 6)
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
                    if (doc.Rules.Count > 256 || doc.Macros.Count > 32 || doc.Constants.Count > 256 || current != null && current.Steps.Count > 256) throw new ScriptException("Visual document exceeds 256 rules/constants, 32 macros or 256 steps per macro.");
                }
            }
            string withoutComments = MaskComments(doc.original);
            foreach (Match match in IntegerDefines.Matches(withoutComments))
            {
                string literal = match.Groups[2].Value; int value;
                if (!TryInteger(literal, out value)) continue;
                doc.Constants.Add(new VisualConstant { Name = match.Groups[1].Value, OriginalName = match.Groups[1].Value, Value = value, OriginalStart = match.Groups[2].Index, OriginalLength = match.Groups[2].Length });
                if (doc.Constants.Count > 256) throw new ScriptException("A visual document may contain at most 256 constants.");
            }
            ValidateImportedRows(doc.Rules, doc.Macros, doc.Constants);
            // A plain main block and editable integer defines are represented fully. Other source is preserved as advanced code.
            string remaining = IntegerDefines.Replace(withoutComments, "");
            remaining = Regex.Replace(remaining, @"\bmain\s*\{\s*\}", "", RegexOptions.CultureInvariant);
            doc.HasAdvancedCode = !string.IsNullOrWhiteSpace(remaining);
            return doc;
        }
        private static JavaScriptSerializer Serializer() { return new JavaScriptSerializer { MaxJsonLength = 262144, RecursionLimit = 64 }; }
        private static Payload ReadPayload(string encoded)
        {
            if (encoded.Length > 349528) throw new ScriptException("Visual metadata is too large.");
            Payload payload;
            try
            {
                byte[] bytes = Convert.FromBase64String(encoded);
                string json = new UTF8Encoding(false, true).GetString(bytes);
                // JavaScriptSerializer's typed conversion accepts objects as integer zero
                // and lets numeric conversion exceptions escape. Check exact JSON types first.
                ValidateMetadataValue(Serializer().DeserializeObject(json), typeof(Payload), 0);
                payload = Serializer().Deserialize<Payload>(json);
            }
            catch (Exception error) when (error is ArgumentException || error is InvalidOperationException || error is FormatException || error is DecoderFallbackException || error is OverflowException)
            { throw new ScriptException("Malformed visual document metadata."); }
            if (payload == null || payload.Version != 2 || payload.Rules == null || payload.Constants == null || payload.Macros == null || payload.Rules.Count > 256 || payload.Constants.Count > 256 || payload.Macros.Count > 32) throw new ScriptException("Invalid visual document metadata version or limits.");
            foreach (VisualMacro macro in payload.Macros)
            {
                if (macro == null || macro.Steps == null || macro.Steps.Count > 256) throw new ScriptException("Invalid visual macro metadata.");
                int nodes = 0; ValidateCondition(macro.Condition, 0, ref nodes, new HashSet<VisualCondition>());
            }
            return payload;
        }
        private static Dictionary<Type, Dictionary<string, Type>> CreateMetadataFields()
        {
            var result = new Dictionary<Type, Dictionary<string, Type>>();
            foreach (Type type in new[] { typeof(Payload), typeof(VisualRule), typeof(VisualConstant), typeof(VisualMacro), typeof(VisualMacroStep), typeof(VisualAxisPattern), typeof(VisualCondition) })
            {
                var fields = new Dictionary<string, Type>(StringComparer.Ordinal);
                foreach (PropertyInfo property in type.GetProperties(BindingFlags.Public | BindingFlags.Instance)) fields.Add(property.Name, property.PropertyType);
                result.Add(type, fields);
            }
            return result;
        }
        private static void ValidateMetadataValue(object value, Type expected, int depth)
        {
            if (depth > 48) throw new ScriptException("Visual metadata exceeds its nesting limit.");
            if (value == null)
            {
                if (expected.IsValueType) throw new ScriptException("A visual metadata value has the wrong type.");
                return;
            }
            if (expected.IsEnum)
            {
                if (!(value is int) || !Enum.IsDefined(expected, value)) throw new ScriptException("Invalid visual metadata enumeration.");
                return;
            }
            if (expected == typeof(int) || expected == typeof(bool) || expected == typeof(string))
            {
                if (value.GetType() != expected) throw new ScriptException("A visual metadata value has the wrong type.");
                return;
            }
            if (expected.IsGenericType && expected.GetGenericTypeDefinition() == typeof(List<>))
            {
                var items = value as object[];
                if (items == null) throw new ScriptException("A visual metadata list has the wrong type.");
                Type element = expected.GetGenericArguments()[0];
                int maximum = element == typeof(VisualMacro) ? 32 : element == typeof(VisualCondition) ? 16 : 256;
                if (items.Length > maximum) throw new ScriptException("A visual metadata list exceeds its item limit.");
                foreach (object item in items) ValidateMetadataValue(item, element, depth + 1);
                return;
            }
            var entries = value as Dictionary<string, object>; Dictionary<string, Type> fields;
            if (entries == null || !MetadataFields.TryGetValue(expected, out fields)) throw new ScriptException("A visual metadata object has the wrong type.");
            foreach (KeyValuePair<string, object> entry in entries)
            {
                Type property;
                if (!fields.TryGetValue(entry.Key, out property)) throw new ScriptException("Unknown visual metadata field '" + entry.Key + "'.");
                ValidateMetadataValue(entry.Value, property, depth + 1);
            }
        }
        private static void ValidateImportedRows(List<VisualRule> rules, List<VisualMacro> macros, List<VisualConstant> constants)
        {
            if (rules.Count > 256 || macros.Count > 32 || constants.Count > 256) throw new ScriptException("Visual document exceeds 256 rules/constants or 32 macros.");
            var constantNames = new HashSet<string>(StringComparer.Ordinal);
            foreach (VisualConstant item in constants)
            {
                if (item == null) throw new ScriptException("A visual constant is missing.");
                if (!constantNames.Add(Name(item.Name))) throw new ScriptException("Duplicate constant name '" + item.Name + "'.");
            }
            foreach (VisualRule rule in rules)
            {
                if (rule == null || !Enum.IsDefined(typeof(VisualRuleKind), rule.Kind)) throw new ScriptException("Invalid visual rule.");
                Control(rule.Source); Control(rule.Target ?? "SOUTH");
                if (rule.Kind == VisualRuleKind.Deadzone && (rule.Value < 0 || rule.Value > 99)) throw new ScriptException("Deadzone must be between 0 and 99 percent.");
                if (rule.Kind == VisualRuleKind.Sensitivity && (rule.Value < 0 || rule.Value > 1000)) throw new ScriptException("Sensitivity must be between 0 and 1000 percent.");
            }
            var macroNames = new HashSet<string>(StringComparer.Ordinal);
            foreach (VisualMacro macro in macros)
            {
                if (macro == null || !Enum.IsDefined(typeof(VisualMacroKind), macro.Kind) || !Enum.IsDefined(typeof(VisualMacroActivation), macro.Activation)) throw new ScriptException("Invalid visual macro kind or activation.");
                if (!macroNames.Add(Name(macro.Name))) throw new ScriptException("Duplicate macro name '" + macro.Name + "'.");
                Control(macro.Trigger);
                if (macro.Pattern == null)
                {
                    if (macro.Kind == VisualMacroKind.AxisPattern) throw new ScriptException("An axis pattern is missing its movement settings.");
                    macro.Pattern = new VisualAxisPattern();
                }
                ValidatePattern(macro.Pattern);
                if (macro.Steps == null || macro.Steps.Count > 256) throw new ScriptException("Invalid visual macro steps.");
                foreach (VisualMacroStep step in macro.Steps)
                {
                    if (step == null || !Enum.IsDefined(typeof(VisualBlend), step.Blend)) throw new ScriptException("A macro step is missing or has an invalid blend.");
                    Control(step.Control);
                    if (step.DurationMilliseconds < 1 || step.DurationMilliseconds > 60000) throw new ScriptException("A macro step duration must be between 1 and 60,000 ms.");
                }
                int nodes = 0; ValidateCondition(macro.Condition, 0, ref nodes, new HashSet<VisualCondition>());
            }
        }
        public static VisualMacro CreateFigureEightPreset()
        {
            var condition = new VisualCondition { Kind = VisualConditionKind.All };
            condition.Children.Add(new VisualCondition { Kind = VisualConditionKind.Held, Control = "SOUTH" });
            condition.Children.Add(new VisualCondition { Kind = VisualConditionKind.Held, Control = "EAST" });
            condition.Children.Add(new VisualCondition { Kind = VisualConditionKind.Compare, Control = "RX", Operator = ">", Value = 50 });
            return new VisualMacro { Name = "Infinity", Kind = VisualMacroKind.AxisPattern, Activation = VisualMacroActivation.WhileTrue, Condition = condition };
        }
        public string[] GetWarnings()
        {
            var warnings = new List<string>();
            if (ManagedSectionModified) warnings.Add("Hand-edited generated code is preserved as advanced code; its old visual controls are no longer applied.");
            foreach (VisualMacro macro in Macros)
            {
                if (macro != null && macro.Enabled && macro.Kind == VisualMacroKind.AxisPattern && macro.Pattern != null)
                {
                    if (macro.Pattern.Blend == VisualBlend.Add) warnings.Add(macro.Name + ": additive movement is clamped at the controller's axis limits.");
                    if (macro.Pattern.Center == VisualPatternCenter.Activation) warnings.Add(macro.Name + ": the center is captured on activation; later stick movement does not move it.");
                }
            }
            return warnings.ToArray();
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
            const string call = @"/\* @igc-visual-call \*/ __igc_visual_apply(?:_[0-9]+)?\(\);";
            // Remove the exact insertion owned by BuildSource, including both newlines.
            // Removing just the statement leaves another indented blank line on every edit.
            source = Regex.Replace(source, @"\r?\n    " + call + @"\r?\n", "", RegexOptions.CultureInvariant);
            return Regex.Replace(source, call, "", RegexOptions.CultureInvariant);
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
        private static void ValidateCondition(VisualCondition condition, int depth, ref int nodes, HashSet<VisualCondition> path)
        {
            if (condition == null) { if (depth != 0) throw new ScriptException("A condition is missing."); return; }
            if (depth > 12 || ++nodes > 128 || !path.Add(condition)) throw new ScriptException("Conditions exceed 12 nested levels or 128 nodes, or contain a cycle.");
            if (!Enum.IsDefined(typeof(VisualConditionKind), condition.Kind) || condition.Children == null) throw new ScriptException("Invalid visual condition.");
            if (condition.Kind == VisualConditionKind.All || condition.Kind == VisualConditionKind.Any || condition.Kind == VisualConditionKind.Not)
            {
                int maximum = condition.Kind == VisualConditionKind.Not ? 1 : 16;
                if (condition.Children.Count < 1 || condition.Children.Count > maximum) throw new ScriptException("ALL/ANY need 1–16 children; NOT needs exactly one child.");
                foreach (VisualCondition child in condition.Children) ValidateCondition(child, depth + 1, ref nodes, path);
            }
            else
            {
                if (condition.Children.Count != 0) throw new ScriptException("Only ALL, ANY and NOT conditions may contain children.");
                if (condition.Kind == VisualConditionKind.Expression) ValidateExpression(condition.Expression);
                else
                {
                    Control(condition.Control);
                    if ((condition.Kind == VisualConditionKind.Compare || condition.Kind == VisualConditionKind.AbsoluteCompare) && condition.Operator != ">" && condition.Operator != ">=" && condition.Operator != "<" && condition.Operator != "<=" && condition.Operator != "==" && condition.Operator != "!=") throw new ScriptException("Choose >, >=, <, <=, == or != for an axis comparison.");
                    if ((condition.Kind == VisualConditionKind.HeldFor || condition.Kind == VisualConditionKind.DoubleTap) && (condition.Milliseconds < 1 || condition.Milliseconds > 60000)) throw new ScriptException("Held/double-tap time must be between 1 and 60,000 ms.");
                }
            }
            path.Remove(condition);
        }
        private static void ValidateExpression(string expression)
        {
            if (string.IsNullOrWhiteSpace(expression) || expression.Length > 4096) throw new ScriptException("A custom condition needs an expression of at most 4,096 characters.");
            // One expression only: never allow an imported metadata field to inject statements or comments.
            if (expression.IndexOf(';') >= 0 || expression.IndexOf('{') >= 0 || expression.IndexOf('}') >= 0 || expression.Contains("//") || expression.Contains("/*")) throw new ScriptException("A custom condition must be one expression, without statements or comments.");
            int parentheses = 0, brackets = 0;
            foreach (char character in expression)
            {
                if (character == '(') parentheses++; else if (character == ')' && --parentheses < 0) throw new ScriptException("Unbalanced condition parentheses.");
                if (character == '[') brackets++; else if (character == ']' && --brackets < 0) throw new ScriptException("Unbalanced condition brackets.");
            }
            if (parentheses != 0 || brackets != 0) throw new ScriptException("Unbalanced condition expression.");
        }
        private static string ConditionSource(VisualCondition condition)
        {
            switch (condition.Kind)
            {
                case VisualConditionKind.All: case VisualConditionKind.Any:
                    var children = new List<string>(); foreach (VisualCondition child in condition.Children) children.Add("(" + ConditionSource(child) + ")");
                    return string.Join(condition.Kind == VisualConditionKind.All ? " && " : " || ", children);
                case VisualConditionKind.Not: return "!(" + ConditionSource(condition.Children[0]) + ")";
                case VisualConditionKind.Expression: return "(" + condition.Expression + ")";
            }
            string control = Control(condition.Control), available = "is_available(" + control + ")", physical = "get_ival(" + control + ")";
            switch (condition.Kind)
            {
                case VisualConditionKind.Held: return available + " && " + physical + (IsHat(control) ? " >= 0" : " != 0");
                case VisualConditionKind.Pressed: return available + " && event_press(" + control + ")";
                case VisualConditionKind.Released: return available + " && event_release(" + control + ")";
                case VisualConditionKind.Compare: return available + " && " + physical + " " + condition.Operator + " " + condition.Value.ToString(CultureInfo.InvariantCulture);
                case VisualConditionKind.AbsoluteCompare: return available + " && abs(" + physical + ") " + condition.Operator + " " + condition.Value.ToString(CultureInfo.InvariantCulture);
                case VisualConditionKind.HeldFor: return available + " && " + physical + (IsHat(control) ? " >= 0" : " != 0") + " && get_ptime(" + control + ") >= " + condition.Milliseconds.ToString(CultureInfo.InvariantCulture);
                case VisualConditionKind.DoubleTap: return available + " && double_tap(" + control + ", " + condition.Milliseconds.ToString(CultureInfo.InvariantCulture) + ")";
                default: throw new ScriptException("Unsupported visual condition.");
            }
        }
        private static bool IsHat(string control) { int id = Controls.Parse(control); return id >= 192 && id < 208; }
        private static void ValidatePattern(VisualAxisPattern pattern)
        {
            if (pattern == null || !Enum.IsDefined(typeof(VisualPatternShape), pattern.Shape) || !Enum.IsDefined(typeof(VisualBlend), pattern.Blend) || !Enum.IsDefined(typeof(VisualPatternCenter), pattern.Center)) throw new ScriptException("Invalid axis pattern.");
            string x = Control(pattern.TargetX), y = Control(pattern.TargetY); int ix = Controls.Parse(x), iy = Controls.Parse(y);
            if (!(ix <= 3 || ix >= 96 && ix < 160) || !(iy <= 3 || iy >= 96 && iy < 160) || x == y) throw new ScriptException("A motion pattern needs two different signed axes, such as LX and LY.");
            if (pattern.AmplitudeX < 0 || pattern.AmplitudeX > 100 || pattern.AmplitudeY < 0 || pattern.AmplitudeY > 100) throw new ScriptException("Pattern amplitudes must be between 0 and 100 percent.");
            if (pattern.PeriodMilliseconds < 1 || pattern.PeriodMilliseconds > 3600000) throw new ScriptException("Pattern period must be between 1 and 3,600,000 ms.");
            if (pattern.TickMilliseconds < 1 || pattern.TickMilliseconds > 20) throw new ScriptException("Pattern update interval must be between 1 and 20 ms.");
            if (pattern.Blend == VisualBlend.Add && pattern.Center != VisualPatternCenter.DynamicPhysical) throw new ScriptException("Add blending follows the current output; select the live center, or use Replace for a fixed/zero center.");
        }
        private static void EmitPattern(VisualMacro macro, string state, string condition, string lifetime, StringBuilder definitions, StringBuilder body, StringBuilder comboSource)
        {
            VisualAxisPattern pattern = macro.Pattern; string x = Control(pattern.TargetX), y = Control(pattern.TargetY);
            string period = pattern.PeriodMilliseconds.ToString(CultureInfo.InvariantCulture);
            definitions.Append("int ").Append(state).Append("_was, ").Append(state).Append("_origin, ").Append(state).Append("_phase, ").Append(state).Append("_angle, ").Append(state).Append("_x, ").Append(state).AppendLine("_y;");
            body.Append("    ").Append(state).Append("_was = ").Append(state).AppendLine("_active;");
            body.Append("    ").Append(state).Append("_condition = ").Append(condition).AppendLine(";");
            string enter = state + "_condition && !" + state + "_last", exit = "!" + state + "_condition && " + state + "_last";
            switch (macro.Activation)
            {
                case VisualMacroActivation.OnEnter:
                    body.Append("    if (").Append(enter).Append(") { ").Append(state).Append("_active = 1; ").Append(state).Append("_was = 0; combo_restart(").Append(lifetime).AppendLine("); }"); break;
                case VisualMacroActivation.WhileTrue: body.Append("    ").Append(state).Append("_active = ").Append(state).AppendLine("_condition;"); break;
                case VisualMacroActivation.OnExit:
                    body.Append("    if (").Append(exit).Append(") { ").Append(state).Append("_active = 1; ").Append(state).Append("_was = 0; combo_restart(").Append(lifetime).AppendLine("); }"); break;
                case VisualMacroActivation.Toggle: body.Append("    if (").Append(enter).Append(") ").Append(state).Append("_active = !").Append(state).AppendLine("_active;"); break;
            }
            bool once = macro.Activation == VisualMacroActivation.OnEnter || macro.Activation == VisualMacroActivation.OnExit;
            if (once)
            {
                // The VM owns a 64-bit monotonic wait deadline, including after long sleeps.
                // Its completion releases the pattern on the next selected tick (at most 20 ms).
                comboSource.Append("combo ").Append(lifetime).Append(" { wait(").Append(period).Append("); ").Append(state).AppendLine("_active = 0; }");
            }
            body.Append("    if (!is_available(").Append(x).Append(") || !is_available(").Append(y).Append(")) ").Append(state).AppendLine("_active = 0;");
            if (once) body.Append("    if (!").Append(state).Append("_active) combo_stop(").Append(lifetime).AppendLine(");");
            body.Append("    if (").Append(state).Append("_active && !").Append(state).AppendLine("_was) {");
            body.Append("        ").Append(state).Append("_origin = get_phase(").Append(period).AppendLine(");");
            if (pattern.Center == VisualPatternCenter.Activation)
            {
                body.Append("        ").Append(state).Append("_x = get_ival(").Append(x).AppendLine(");");
                body.Append("        ").Append(state).Append("_y = get_ival(").Append(y).AppendLine(");");
            }
            body.AppendLine("    }");
            body.Append("    if (").Append(state).AppendLine("_active) {");
            body.Append("        tick(").Append(pattern.TickMilliseconds.ToString(CultureInfo.InvariantCulture)).AppendLine(");");
            body.Append("        ").Append(state).Append("_phase = (get_phase(").Append(period).Append(") - ").Append(state).Append("_origin + ").Append(period).Append(") % ").Append(period).AppendLine(";");
            body.Append("        ").Append(state).Append("_angle = ").Append(state).Append("_phase * 360 / ").Append(period).AppendLine(";");
            string deltaX = "sin(" + state + "_angle, " + pattern.AmplitudeX.ToString(CultureInfo.InvariantCulture) + ")";
            string deltaY = pattern.Shape == VisualPatternShape.Sine ? "0" : (pattern.Shape == VisualPatternShape.Circle ? "cos(" + state + "_angle, " : "sin(" + state + "_angle * 2, ") + pattern.AmplitudeY.ToString(CultureInfo.InvariantCulture) + ")";
            if (pattern.Blend == VisualBlend.Add)
            {
                body.Append("        add_val(").Append(x).Append(", ").Append(deltaX).AppendLine(");");
                body.Append("        add_val(").Append(y).Append(", ").Append(deltaY).AppendLine(");");
            }
            else
            {
                string centerX = pattern.Center == VisualPatternCenter.DynamicPhysical ? "get_ival(" + x + ")" : pattern.Center == VisualPatternCenter.Activation ? state + "_x" : "0";
                string centerY = pattern.Center == VisualPatternCenter.DynamicPhysical ? "get_ival(" + y + ")" : pattern.Center == VisualPatternCenter.Activation ? state + "_y" : "0";
                body.Append("        set_val(").Append(x).Append(", ").Append(centerX).Append(" + ").Append(deltaX).AppendLine(");");
                body.Append("        set_val(").Append(y).Append(", ").Append(centerY).Append(" + ").Append(deltaY).AppendLine(");");
            }
            body.AppendLine("    }");
            body.Append("    ").Append(state).Append("_last = ").Append(state).AppendLine("_condition;");
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
            string statePrefix = "__igc_visual_state_"; int stateGeneration = 0;
            while (maskedAdvanced.IndexOf(statePrefix, StringComparison.Ordinal) >= 0) statePrefix = "__igc_visual_state_" + (++stateGeneration).ToString(CultureInfo.InvariantCulture) + "_";
            var definitions = new StringBuilder(); var body = new StringBuilder(); var comboSource = new StringBuilder();
            foreach (VisualConstant item in Constants) if (item.OriginalStart < 0)
            {
                definitions.Append("define ").Append(item.Name).Append(" = ").Append(item.Value.ToString(CultureInfo.InvariantCulture)).AppendLine(";");
            }
            foreach (VisualRule rule in Rules)
            {
                if (rule == null || !Enum.IsDefined(typeof(VisualRuleKind), rule.Kind)) throw new ScriptException("Invalid visual rule.");
                string control = Control(rule.Source), target = Control(rule.Target ?? "SOUTH");
                if (rule.Kind == VisualRuleKind.Deadzone && (rule.Value < 0 || rule.Value > 99)) throw new ScriptException("Deadzone must be between 0 and 99 percent.");
                if (rule.Kind == VisualRuleKind.Sensitivity && (rule.Value < 0 || rule.Value > 1000)) throw new ScriptException("Sensitivity must be between 0 and 1000 percent.");
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
                if (!Enum.IsDefined(typeof(VisualMacroActivation), macro.Activation) || !Enum.IsDefined(typeof(VisualMacroKind), macro.Kind)) throw new ScriptException("Invalid macro activation or kind.");
                if (macro.Steps == null || macro.Steps.Count > 256) throw new ScriptException("A visual macro may contain at most 256 steps.");
                int nodes = 0; ValidateCondition(macro.Condition, 0, ref nodes, new HashSet<VisualCondition>());
                if (macro.Kind == VisualMacroKind.AxisPattern) ValidatePattern(macro.Pattern);
                if (macro.Enabled && macro.Kind == VisualMacroKind.Sequence && macro.Steps.Count == 0) throw new ScriptException("Macro '" + label + "' needs at least one timed step.");
                string comboName = macroPrefix + m.ToString(CultureInfo.InvariantCulture);
                foreach (VisualMacroStep step in macro.Steps)
                {
                    if (step == null || !Enum.IsDefined(typeof(VisualBlend), step.Blend)) throw new ScriptException("A macro step is missing or has an invalid blend."); Control(step.Control);
                    if (step.DurationMilliseconds < 1 || step.DurationMilliseconds > 60000) throw new ScriptException("A macro step duration must be between 1 and 60,000 ms.");
                }
                if (!macro.Enabled) continue;
                string state = statePrefix + m.ToString(CultureInfo.InvariantCulture), condition = macro.Condition == null ? "event_press(" + trigger + ")" : ConditionSource(macro.Condition);
                definitions.Append("int ").Append(state).Append("_last, ").Append(state).Append("_condition, ").Append(state).AppendLine("_active;");
                if (macro.Kind == VisualMacroKind.AxisPattern)
                {
                    string x = Control(macro.Pattern.TargetX), y = Control(macro.Pattern.TargetY);
                    condition = "is_available(" + x + ") && is_available(" + y + ") && (" + condition + ")";
                    EmitPattern(macro, state, condition, comboName, definitions, body, comboSource);
                }
                else
                {
                    body.Append("    ").Append(state).Append("_condition = ").Append(condition).AppendLine(";");
                    string enter = state + "_condition && !" + state + "_last", exit = "!" + state + "_condition && " + state + "_last";
                    switch (macro.Activation)
                    {
                        case VisualMacroActivation.OnEnter: body.Append("    if (").Append(enter).Append(") combo_run(").Append(comboName).AppendLine(");"); break;
                        case VisualMacroActivation.OnExit: body.Append("    if (").Append(exit).Append(") combo_run(").Append(comboName).AppendLine(");"); break;
                        case VisualMacroActivation.WhileTrue: body.Append("    ").Append(state).Append("_active = ").Append(state).AppendLine("_condition;"); break;
                        case VisualMacroActivation.Toggle: body.Append("    if (").Append(enter).Append(") ").Append(state).Append("_active = !").Append(state).AppendLine("_active;"); break;
                    }
                    bool repeat = macro.Activation == VisualMacroActivation.WhileTrue || macro.Activation == VisualMacroActivation.Toggle;
                    if (repeat) body.Append("    if (").Append(state).Append("_active) combo_run(").Append(comboName).Append("); else combo_stop(").Append(comboName).AppendLine(");");
                    body.Append("    ").Append(state).Append("_last = ").Append(state).AppendLine("_condition;");
                    comboSource.Append("combo ").Append(comboName).AppendLine(" {");
                    if (repeat) comboSource.Append("    while (").Append(state).AppendLine("_active) {");
                    foreach (VisualMacroStep step in macro.Steps)
                        comboSource.Append("    ").Append(step.Blend == VisualBlend.Add ? "add_val(" : "set_val(").Append(Control(step.Control)).Append(", ").Append(step.Value.ToString(CultureInfo.InvariantCulture)).Append("); wait(").Append(step.DurationMilliseconds.ToString(CultureInfo.InvariantCulture)).AppendLine(");");
                    if (repeat) comboSource.AppendLine("    }");
                    comboSource.AppendLine("}");
                }
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
            if (mainStart < 0) advanced += Environment.NewLine + "main {" + Environment.NewLine + "    " + callMarker + Environment.NewLine + "}" + Environment.NewLine;
            else { if (mainEnd < 0) throw new ScriptException("The main block is incomplete."); advanced = advanced.Insert(mainEnd, Environment.NewLine + "    " + callMarker + Environment.NewLine); }
            var payload = new Payload { Version = 2, Rules = Rules, Macros = Macros, Constants = new List<VisualConstant>() };
            foreach (VisualConstant item in Constants) if (item.OriginalStart < 0) payload.Constants.Add(item);
            string metadata;
            try { metadata = Convert.ToBase64String(Encoding.UTF8.GetBytes(Serializer().Serialize(payload))); }
            catch (InvalidOperationException) { throw new ScriptException("Visual metadata exceeds its size or nesting limit."); }
            string content = "// @document " + metadata + Environment.NewLine + definitions + "function " + helper + "() {" + Environment.NewLine + body + "}" + Environment.NewLine + comboSource;
            string result = advanced.TrimEnd() + Environment.NewLine + Environment.NewLine + Begin + Environment.NewLine + "// @hash " + Hash(content) + Environment.NewLine + content + End + Environment.NewLine;
            if (result.Length > 524288) throw new ScriptException("A script may contain at most 512 KiB of text.");
            ScriptCompiler.Compile(result); return result;
        }
        private static int Offset(string source, int line, int column)
        {
            int offset = 0; for (int n = 1; n < line; n++) { offset = source.IndexOf('\n', offset); if (offset < 0) throw new ScriptException("Invalid source line."); offset++; } return offset + column - 1;
        }
    }
}
