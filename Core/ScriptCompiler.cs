using System;
using System.Collections.Generic;
using System.Globalization;

namespace GamePadRemapper.Core
{
    internal enum Op { Push, Load, Store, LoadLocal, StoreLocal, LoadArray, StoreArray, Dup, Dup2, Pop, Add, Sub, Mul, Div, Mod, Neg, Not, BitNot, BitAnd, BitOr, BitXor, Shl, Shr, Eq, Ne, Lt, Le, Gt, Ge, Bool, Jump, JumpZero, Builtin, Call, Return, Wait }
    internal struct Instruction
    {
        internal Op Op; internal int A, B, Line, Column;
        internal Instruction(Op op, int a, int b, int line, int column) { Op = op; A = a; B = b; Line = line; Column = column; }
    }
    internal sealed class CodeBlock { internal string Name; internal Instruction[] Code; internal int Arguments, Locals; internal bool Combo; }
    public sealed class ScriptProgram
    {
        public string Source { get; private set; }
        internal CodeBlock[] Blocks;
        internal int GlobalCount, Init, Main;
        internal int[] ComboBlocks;
        internal ScriptProgram(string source, CodeBlock[] blocks, int globals, int init, int main, int[] combos)
        { Source = source; Blocks = blocks; GlobalCount = globals; Init = init; Main = main; ComboBlocks = combos; }
        public ScriptInstance CreateInstance() { return new ScriptInstance(this); }
    }
    public static class ScriptCompiler
    {
        public static ScriptProgram Compile(string source)
        {
            if (source == null) throw new ArgumentNullException("source");
            if (source.Length > 524288) throw new ScriptException("A script may contain at most 512 KiB of text.");
            return new Compiler(source).Compile();
        }
    }

    internal struct Token
    {
        internal string Text; internal int Number, Line, Column; internal bool IsNumber, IsIdentifier;
        internal Token(string text, int line, int column, int number = 0, bool numeric = false, bool identifier = false)
        { Text = text; Line = line; Column = column; Number = number; IsNumber = numeric; IsIdentifier = identifier; }
    }
    internal sealed class Lexer
    {
        private readonly string source; private int position, line = 1, column = 1;
        internal Lexer(string source) { this.source = source; }
        private char Current { get { return position < source.Length ? source[position] : '\0'; } }
        private char Peek { get { return position + 1 < source.Length ? source[position + 1] : '\0'; } }
        private void Advance() { if (Current == '\n') { line++; column = 1; } else column++; position++; }
        internal List<Token> Scan()
        {
            var result = new List<Token>();
            while (position < source.Length)
            {
                if (char.IsWhiteSpace(Current)) { Advance(); continue; }
                if (Current == '/' && Peek == '/') { while (Current != '\n' && position < source.Length) Advance(); continue; }
                if (Current == '/' && Peek == '*')
                {
                    int beginLine = line, beginColumn = column; Advance(); Advance();
                    while (position < source.Length && !(Current == '*' && Peek == '/')) Advance();
                    if (position >= source.Length) throw new ScriptException("Unterminated block comment.", beginLine, beginColumn);
                    Advance(); Advance(); continue;
                }
                int start = position, sl = line, sc = column;
                if (char.IsLetter(Current) || Current == '_')
                {
                    do { Advance(); } while (char.IsLetterOrDigit(Current) || Current == '_');
                    string word = source.Substring(start, position - start);
                    if (word.Length > 128) throw new ScriptException("An identifier may contain at most 128 characters.", sl, sc);
                    result.Add(new Token(word, sl, sc, 0, false, true));
                }
                else if (char.IsDigit(Current))
                {
                    bool hex = Current == '0' && (Peek == 'x' || Peek == 'X');
                    if (hex) { Advance(); Advance(); }
                    int digits = position;
                    while (hex ? Uri.IsHexDigit(Current) : char.IsDigit(Current)) Advance();
                    string text = source.Substring(digits, position - digits); uint unsigned;
                    if (text.Length == 0 || !uint.TryParse(text, hex ? NumberStyles.AllowHexSpecifier : NumberStyles.None, CultureInfo.InvariantCulture, out unsigned))
                        throw new ScriptException("Invalid 32-bit integer literal.", sl, sc);
                    result.Add(new Token(source.Substring(start, position - start), sl, sc, unchecked((int)unsigned), true));
                }
                else
                {
                    string two = position + 1 < source.Length ? source.Substring(position, 2) : "";
                    string three = position + 2 < source.Length ? source.Substring(position, 3) : "";
                    if (three == "<<=" || three == ">>=") { Advance(); Advance(); Advance(); result.Add(new Token(three, sl, sc)); }
                    else if (two == "==" || two == "!=" || two == "<=" || two == ">=" || two == "&&" || two == "||" || two == "++" || two == "--" || two == "+=" || two == "-=" || two == "*=" || two == "/=" || two == "%=" || two == "&=" || two == "|=" || two == "^=" || two == "<<" || two == ">>")
                    { Advance(); Advance(); result.Add(new Token(two, sl, sc)); }
                    else if ("{}()[];,:?=+-*/%!~&|^<>".IndexOf(Current) >= 0) { string one = Current.ToString(); Advance(); result.Add(new Token(one, sl, sc)); }
                    else throw new ScriptException("Unexpected character '" + Current + "'. Strings, imports and operating-system calls are not supported.", sl, sc);
                }
                if (result.Count > 100000) throw new ScriptException("A script may contain at most 100,000 tokens.", sl, sc);
            }
            result.Add(new Token("<end>", line, column)); return result;
        }
    }
    internal sealed class Expr
    {
        internal Token Token; internal string Kind; internal Expr Left, Right, Third; internal List<Expr> Args;
        internal Expr(Token token, string kind, Expr left = null, Expr right = null) { Token = token; Kind = kind; Left = left; Right = right; }
    }
    internal sealed class Statement
    {
        internal Token Token; internal string Kind; internal List<Statement> Items; internal Expr Expr, Condition, Increment;
        internal Statement Body, Else, Initializer; internal string Name; internal int Size; internal List<Expr> InitialValues;
        internal Statement(Token token, string kind) { Token = token; Kind = kind; }
    }
    internal sealed class FunctionNode { internal Token Token; internal string Name; internal List<string> Parameters = new List<string>(); internal Statement Body; internal bool Combo; }
    internal sealed class Symbol { internal int Offset, Size; internal bool Local, Array, ReadOnly; }
    internal sealed class LoopContext { internal List<int> Breaks = new List<int>(), Continues = new List<int>(); }

    internal sealed class Compiler
    {
        private static readonly HashSet<string> Keywords = new HashSet<string>(StringComparer.Ordinal)
        { "define", "const", "int", "main", "init", "combo", "function", "if", "else", "while", "for", "break", "continue", "return", "wait" };
        private readonly string source; private readonly List<Token> tokens; private int position, depth, globalCount;
        private readonly Dictionary<string, int> constants = new Dictionary<string, int>(StringComparer.Ordinal);
        private readonly Dictionary<string, Symbol> globals = new Dictionary<string, Symbol>(StringComparer.Ordinal);
        private Dictionary<string, Symbol> locals;
        private readonly List<FunctionNode> functions = new List<FunctionNode>();
        private readonly Dictionary<string, int> functionIds = new Dictionary<string, int>(StringComparer.Ordinal);
        private readonly Dictionary<string, int> comboIds = new Dictionary<string, int>(StringComparer.Ordinal);
        private readonly List<Statement> globalInitializers = new List<Statement>();
        private List<Instruction> code; private FunctionNode currentFunction; private int localCount, instructionCount;
        private readonly Stack<LoopContext> loops = new Stack<LoopContext>();
        internal Compiler(string source)
        {
            this.source = source; tokens = new Lexer(source).Scan(); constants["TRUE"] = constants["true"] = 1; constants["FALSE"] = constants["false"] = 0;
        }
        private Token Current { get { return tokens[position]; } }
        private Token Next() { return tokens[position++]; }
        private bool Take(string value) { if (Current.Text != value) return false; position++; return true; }
        private Token Expect(string value) { if (Current.Text != value) Fail(Current, "Expected '" + value + "', found '" + Current.Text + "'."); return Next(); }
        private Token Identifier() { if (!Current.IsIdentifier) Fail(Current, "Expected an identifier."); return Next(); }
        private static void Fail(Token at, string message) { throw new ScriptException(message, at.Line, at.Column); }
        private void Enter(Token at) { if (++depth > 96) Fail(at, "Syntax nesting exceeds the safe limit (96)."); }
        private void Leave() { depth--; }
        private static int ControlConstant(string name) { return name == name.ToUpperInvariant() ? Controls.Parse(name) : -1; }
        private bool Reserved(string name) { return Keywords.Contains(name) || constants.ContainsKey(name) || ControlConstant(name) >= 0 || globals.ContainsKey(name) || functionIds.ContainsKey(name) || Builtins.Find(name) >= 0; }
        internal ScriptProgram Compile()
        {
            while (Current.Text != "<end>")
            {
                Token top = Next();
                if (top.Text == "define" || top.Text == "const")
                {
                    if (top.Text == "const") Take("int"); Token name = Identifier();
                    if (top.Text == "const" && Current.Text == "[")
                    {
                        position--; var declaration = ParseDeclaration(top, true);
                        foreach (var entry in declaration.Items)
                        {
                            if (entry.Size == 0 || entry.InitialValues == null) Fail(entry.Token, "A constant array needs an initializer.");
                            foreach (var initializer in entry.InitialValues) Constant(initializer);
                            AddGlobal(entry, true);
                        }
                        continue;
                    }
                    if (Reserved(name.Text)) Fail(name, "Duplicate or reserved name '" + name.Text + "'.");
                    Expect("="); int value = Constant(ParseExpression()); Expect(";"); constants.Add(name.Text, value);
                }
                else if (top.Text == "int")
                {
                    var declaration = ParseDeclaration(top, true); foreach (var entry in declaration.Items)
                    {
                        AddGlobal(entry, false);
                    }
                }
                else if (top.Text == "main" || top.Text == "init" || top.Text == "combo" || top.Text == "function")
                {
                    var fn = new FunctionNode { Token = top, Name = top.Text, Combo = top.Text == "combo" };
                    if (top.Text == "combo" || top.Text == "function")
                    {
                        fn.Name = Identifier().Text;
                        if (Reserved(fn.Name)) Fail(top, "Duplicate or reserved function name '" + fn.Name + "'.");
                    }
                    else if (functionIds.ContainsKey(fn.Name)) Fail(top, "Duplicate or reserved function name '" + fn.Name + "'.");
                    if (functions.Count >= 96) Fail(top, "At most 96 functions, including main, init and combos, are supported.");
                    functionIds.Add(fn.Name, functions.Count);
                    if (fn.Combo) { if (comboIds.Count >= 32) Fail(top, "At most 32 combos are supported."); comboIds.Add(fn.Name, comboIds.Count); }
                    if (top.Text == "function")
                    {
                        Expect("("); if (!Take(")")) do
                        {
                            Take("int"); var parameter = Identifier();
                            if (fn.Parameters.Contains(parameter.Text) || Reserved(parameter.Text)) Fail(parameter, "Duplicate or reserved parameter name.");
                            fn.Parameters.Add(parameter.Text); if (fn.Parameters.Count > 16) Fail(parameter, "At most 16 function arguments are supported.");
                        } while (Take(","));
                        if (tokens[position - 1].Text != ")") Expect(")");
                    }
                    fn.Body = ParseBlock(); functions.Add(fn);
                }
                else Fail(top, "Expected define, int, init, main, function or combo.");
            }
            if (!functionIds.ContainsKey("main")) throw new ScriptException("A script needs a main { ... } block.");
            if (!functionIds.ContainsKey("init"))
            {
                if (functions.Count >= 96) Fail(Current, "At most 96 functions, including main, init and combos, are supported.");
                functionIds.Add("init", functions.Count);
                functions.Add(new FunctionNode { Name = "init", Token = new Token("init", 1, 1), Body = new Statement(new Token("{", 1, 1), "block") { Items = new List<Statement>() } });
            }
            var blocks = new CodeBlock[functions.Count];
            for (int i = 0; i < functions.Count; i++)
            {
                currentFunction = functions[i]; code = new List<Instruction>(); locals = new Dictionary<string, Symbol>(StringComparer.Ordinal); localCount = 0;
                foreach (string arg in currentFunction.Parameters) locals.Add(arg, new Symbol { Local = true, Offset = localCount++, Size = 1 });
                if (currentFunction.Name == "init") foreach (var initializer in globalInitializers) CompileDeclaration(initializer, false);
                CompileStatement(currentFunction.Body); Emit(Op.Push, 0, 0, currentFunction.Token); Emit(Op.Return, 0, 0, currentFunction.Token);
                blocks[i] = new CodeBlock { Name = currentFunction.Name, Arguments = currentFunction.Parameters.Count, Locals = localCount, Code = code.ToArray(), Combo = currentFunction.Combo };
            }
            var combos = new int[comboIds.Count]; foreach (var item in comboIds) combos[item.Value] = functionIds[item.Key];
            return new ScriptProgram(source, blocks, globalCount, functionIds["init"], functionIds["main"], combos);
        }
        private Statement ParseBlock()
        {
            var at = Expect("{"); Enter(at); var result = new Statement(at, "block") { Items = new List<Statement>() };
            while (!Take("}")) { if (Current.Text == "<end>") Fail(Current, "Unterminated block."); result.Items.Add(ParseStatement()); }
            Leave(); return result;
        }
        private void AddGlobal(Statement entry, bool readOnly)
        {
            if (Reserved(entry.Name)) Fail(entry.Token, "Duplicate or reserved name '" + entry.Name + "'.");
            globals.Add(entry.Name, new Symbol { Offset = globalCount, Size = entry.Size == 0 ? 1 : entry.Size, Array = entry.Size > 0, ReadOnly = readOnly });
            globalCount += entry.Size == 0 ? 1 : entry.Size;
            if (globalCount > 4096) Fail(entry.Token, "Global storage exceeds 4,096 integers."); globalInitializers.Add(entry);
        }
        private Statement ParseDeclaration(Token at, bool semicolon)
        {
            var group = new Statement(at, "declarations") { Items = new List<Statement>() };
            do
            {
                var name = Identifier(); var item = new Statement(name, "declaration") { Name = name.Text };
                if (Take("["))
                {
                    if (Current.Text == "]") item.Size = -1;
                    else { item.Size = Constant(ParseExpression()); if (item.Size < 1 || item.Size > 1024) Fail(name, "Array length must be between 1 and 1,024."); }
                    Expect("]");
                }
                if (Take("="))
                {
                    if (item.Size != 0)
                    {
                        Expect("{"); item.InitialValues = new List<Expr>();
                        if (!Take("}")) { do { item.InitialValues.Add(ParseExpression()); } while (Take(",") && Current.Text != "}"); Expect("}"); }
                        if (item.Size == -1) item.Size = item.InitialValues.Count;
                        if (item.Size == 0 || item.Size > 1024 || item.InitialValues.Count > item.Size) Fail(name, "Invalid array initializer length (maximum 1,024 elements).");
                    }
                    else item.Expr = ParseExpression();
                }
                if (item.Size == -1) Fail(name, "An unsized array needs an initializer.");
                group.Items.Add(item);
            } while (Take(","));
            if (semicolon) Expect(";"); return group;
        }
        private Statement ParseStatement()
        {
            Enter(Current); Statement result;
            if (Current.Text == "{") result = ParseBlock();
            else
            {
                var at = Current;
                if (Take(";")) result = new Statement(at, "empty");
                else if (Take("int")) result = ParseDeclaration(at, true);
                else if (Take("if"))
                {
                    result = new Statement(at, "if"); Expect("("); result.Condition = ParseExpression(); Expect(")"); result.Body = ParseStatement(); if (Take("else")) result.Else = ParseStatement();
                }
                else if (Take("while")) { result = new Statement(at, "while"); Expect("("); result.Condition = ParseExpression(); Expect(")"); result.Body = ParseStatement(); }
                else if (Take("for"))
                {
                    result = new Statement(at, "for"); Expect("(");
                    if (Take("int")) result.Initializer = ParseDeclaration(at, false);
                    else if (Current.Text != ";") result.Initializer = new Statement(Current, "expression") { Expr = ParseExpression() };
                    Expect(";"); if (Current.Text != ";") result.Condition = ParseExpression(); Expect(";"); if (Current.Text != ")") result.Increment = ParseExpression(); Expect(")"); result.Body = ParseStatement();
                }
                else if (Take("break") || Take("continue")) { result = new Statement(at, at.Text); Expect(";"); }
                else if (Take("return")) { result = new Statement(at, "return"); if (Current.Text != ";") result.Expr = ParseExpression(); Expect(";"); }
                else { result = new Statement(at, "expression") { Expr = ParseExpression() }; Expect(";"); }
            }
            Leave(); return result;
        }
        private static int Precedence(string op)
        {
            switch (op)
            {
                case "=": case "+=": case "-=": case "*=": case "/=": case "%=": case "&=": case "|=": case "^=": case "<<=": case ">>=": return 1;
                case "?": return 2; case "||": return 3; case "&&": return 4; case "|": return 5; case "^": return 6; case "&": return 7;
                case "==": case "!=": return 8; case "<": case "<=": case ">": case ">=": return 9; case "<<": case ">>": return 10;
                case "+": case "-": return 11; case "*": case "/": case "%": return 12; default: return 0;
            }
        }
        private Expr ParseExpression(int minimum = 1)
        {
            Enter(Current); Token at = Next(); Expr left;
            if (at.IsNumber) left = new Expr(at, "number");
            else if (at.Text == "(") { left = ParseExpression(); Expect(")"); }
            else if (at.Text == "+" || at.Text == "-" || at.Text == "!" || at.Text == "~" || at.Text == "++" || at.Text == "--") left = new Expr(at, "unary", ParseExpression(13));
            else if (at.IsIdentifier) left = new Expr(at, "name");
            else { Fail(at, "Expected an expression."); return null; }
            while (true)
            {
                if (Current.Text == "(" && left.Kind == "name")
                {
                    Next(); var call = new Expr(left.Token, "call") { Args = new List<Expr>() };
                    if (!Take(")")) { do { call.Args.Add(ParseExpression()); if (call.Args.Count > 16) Fail(call.Token, "At most 16 arguments are supported."); } while (Take(",")); Expect(")"); }
                    left = call; continue;
                }
                if (Take("[")) { if (left.Kind != "name") Fail(left.Token, "Only named arrays may be indexed."); left = new Expr(left.Token, "index", ParseExpression()); Expect("]"); continue; }
                if (Current.Text == "++" || Current.Text == "--") { left = new Expr(Next(), "postfix", left); continue; }
                int precedence = Precedence(Current.Text); if (precedence < minimum || precedence == 0) break;
                Token op = Next();
                if (op.Text == "?") { var ternary = new Expr(op, "conditional", left, ParseExpression()); Expect(":"); ternary.Third = ParseExpression(2); left = ternary; }
                else left = new Expr(op, precedence == 1 ? "assign" : "binary", left, ParseExpression(precedence == 1 ? precedence : precedence + 1));
            }
            Leave(); return left;
        }
        private int Constant(Expr expression)
        {
            Enter(expression.Token); try { return ConstantCore(expression); } finally { Leave(); }
        }
        private int ConstantCore(Expr expression)
        {
            int value;
            if (expression.Kind == "number") return expression.Token.Number;
            if (expression.Kind == "name") { if (constants.TryGetValue(expression.Token.Text, out value)) return value; value = ControlConstant(expression.Token.Text); if (value >= 0) return value; }
            if (expression.Kind == "unary")
            {
                int a = Constant(expression.Left); switch (expression.Token.Text) { case "+": return a; case "-": return unchecked(-a); case "!": return a == 0 ? 1 : 0; case "~": return ~a; }
            }
            if (expression.Kind == "binary")
            {
                int a = Constant(expression.Left); if (expression.Token.Text == "&&" && a == 0) return 0; if (expression.Token.Text == "||" && a != 0) return 1;
                int b = Constant(expression.Right); if (expression.Token.Text == "&&") return b == 0 ? 0 : 1; if (expression.Token.Text == "||") return b == 0 ? 0 : 1;
                return Arithmetic(BinaryOp(expression.Token), a, b, expression.Token);
            }
            if (expression.Kind == "conditional") return Constant(expression.Left) != 0 ? Constant(expression.Right) : Constant(expression.Third);
            Fail(expression.Token, "A constant integer expression is required here."); return 0;
        }
        internal static int Arithmetic(Op op, int a, int b, Token at)
        {
            unchecked
            {
                switch (op)
                {
                    case Op.Add: return a + b; case Op.Sub: return a - b; case Op.Mul: return a * b;
                    case Op.Div: if (b == 0) Fail(at, "Division by zero."); return a == int.MinValue && b == -1 ? int.MinValue : a / b;
                    case Op.Mod: if (b == 0) Fail(at, "Remainder by zero."); return a == int.MinValue && b == -1 ? 0 : a % b;
                    case Op.BitAnd: return a & b; case Op.BitOr: return a | b; case Op.BitXor: return a ^ b; case Op.Shl: return a << (b & 31); case Op.Shr: return a >> (b & 31);
                    case Op.Eq: return a == b ? 1 : 0; case Op.Ne: return a != b ? 1 : 0; case Op.Lt: return a < b ? 1 : 0; case Op.Le: return a <= b ? 1 : 0; case Op.Gt: return a > b ? 1 : 0; case Op.Ge: return a >= b ? 1 : 0;
                    default: Fail(at, "Unsupported arithmetic operator."); return 0;
                }
            }
        }
        private static Op BinaryOp(Token token)
        {
            switch (token.Text)
            {
                case "+": case "+=": return Op.Add; case "-": case "-=": return Op.Sub; case "*": case "*=": return Op.Mul;
                case "/": case "/=": return Op.Div; case "%": case "%=": return Op.Mod; case "&": case "&=": return Op.BitAnd;
                case "|": case "|=": return Op.BitOr; case "^": case "^=": return Op.BitXor; case "<<": case "<<=": return Op.Shl; case ">>": case ">>=": return Op.Shr;
                case "==": return Op.Eq; case "!=": return Op.Ne; case "<": return Op.Lt; case "<=": return Op.Le; case ">": return Op.Gt; case ">=": return Op.Ge;
                default: Fail(token, "Unsupported binary operator."); return Op.Add;
            }
        }
        private int Emit(Op op, int a, int b, Token at)
        {
            if (++instructionCount > 32768) Fail(at, "Compiled program exceeds 32,768 instructions.");
            code.Add(new Instruction(op, a, b, at.Line, at.Column)); return code.Count - 1;
        }
        private void Patch(int index, int target) { var instruction = code[index]; instruction.A = target; code[index] = instruction; }
        private Symbol Resolve(Expr expression)
        {
            Symbol symbol; if (!locals.TryGetValue(expression.Token.Text, out symbol) && !globals.TryGetValue(expression.Token.Text, out symbol)) Fail(expression.Token, "Unknown variable '" + expression.Token.Text + "'.");
            if ((expression.Kind == "index") != symbol.Array) Fail(expression.Token, symbol.Array ? "An array needs an index." : "This variable is not an array.");
            return symbol;
        }
        private void CompileDeclaration(Statement node, bool local)
        {
            Symbol symbol;
            if (local)
            {
                if (Reserved(node.Name) || locals.ContainsKey(node.Name)) Fail(node.Token, "Duplicate or reserved variable '" + node.Name + "'. Locals are function-scoped.");
                symbol = new Symbol { Offset = localCount, Size = node.Size == 0 ? 1 : node.Size, Local = true, Array = node.Size > 0 }; locals.Add(node.Name, symbol); localCount += symbol.Size;
                if (localCount > 256) Fail(node.Token, "A function may use at most 256 local integers, including arguments and arrays.");
            }
            else symbol = globals[node.Name];
            if (!symbol.Array)
            {
                if (node.Expr != null) CompileExpression(node.Expr); else Emit(Op.Push, 0, 0, node.Token);
                Emit(symbol.Local ? Op.StoreLocal : Op.Store, symbol.Offset, 0, node.Token); Emit(Op.Pop, 0, 0, node.Token);
            }
            else if (node.InitialValues != null)
            {
                for (int i = 0; i < node.InitialValues.Count; i++)
                { Emit(Op.Push, i, 0, node.Token); CompileExpression(node.InitialValues[i]); Emit(Op.StoreArray, symbol.Offset, symbol.Size | (symbol.Local ? 65536 : 0), node.Token); Emit(Op.Pop, 0, 0, node.Token); }
            }
        }
        private void CompileStatement(Statement node)
        {
            switch (node.Kind)
            {
                case "empty": return;
                case "block": case "declarations": foreach (var item in node.Items) CompileStatement(item); return;
                case "declaration": CompileDeclaration(node, true); return;
                case "expression":
                    if (node.Expr.Kind == "call" && node.Expr.Token.Text == "wait")
                    {
                        if (!currentFunction.Combo || node.Expr.Args.Count != 1) Fail(node.Expr.Token, "wait(milliseconds) is only valid as a standalone statement directly inside a combo.");
                        CompileExpression(node.Expr.Args[0]); Emit(Op.Wait, 0, 0, node.Expr.Token); return;
                    }
                    CompileExpression(node.Expr); Emit(Op.Pop, 0, 0, node.Token); return;
                case "if":
                    CompileExpression(node.Condition); int no = Emit(Op.JumpZero, 0, 0, node.Token); CompileStatement(node.Body);
                    if (node.Else == null) Patch(no, code.Count); else { int end = Emit(Op.Jump, 0, 0, node.Token); Patch(no, code.Count); CompileStatement(node.Else); Patch(end, code.Count); } return;
                case "while": case "for":
                    if (node.Initializer != null && node.Initializer.Kind == "expression" && node.Initializer.Expr.Kind == "call" && node.Initializer.Expr.Token.Text == "wait") Fail(node.Initializer.Expr.Token, "wait(milliseconds) must be a standalone statement directly inside a combo.");
                    if (node.Initializer != null) CompileStatement(node.Initializer); int start = code.Count, exit = -1;
                    if (node.Condition != null) { CompileExpression(node.Condition); exit = Emit(Op.JumpZero, 0, 0, node.Token); }
                    var loop = new LoopContext(); loops.Push(loop); CompileStatement(node.Body); int increment = code.Count;
                    if (node.Increment != null) { CompileExpression(node.Increment); Emit(Op.Pop, 0, 0, node.Token); }
                    Emit(Op.Jump, start, 0, node.Token); if (exit >= 0) Patch(exit, code.Count);
                    foreach (int jump in loop.Breaks) Patch(jump, code.Count); foreach (int jump in loop.Continues) Patch(jump, increment); loops.Pop(); return;
                case "break": case "continue":
                    if (loops.Count == 0) Fail(node.Token, node.Kind + " requires a surrounding loop.");
                    int branch = Emit(Op.Jump, 0, 0, node.Token); if (node.Kind == "break") loops.Peek().Breaks.Add(branch); else loops.Peek().Continues.Add(branch); return;
                case "return": if (node.Expr == null) Emit(Op.Push, 0, 0, node.Token); else CompileExpression(node.Expr); Emit(Op.Return, 0, 0, node.Token); return;
                default: Fail(node.Token, "Unsupported statement."); return;
            }
        }
        private void Load(Expr reference, Symbol symbol)
        {
            if (symbol.Array) { CompileExpression(reference.Left); Emit(Op.LoadArray, symbol.Offset, symbol.Size | (symbol.Local ? 65536 : 0), reference.Token); }
            else Emit(symbol.Local ? Op.LoadLocal : Op.Load, symbol.Offset, 0, reference.Token);
        }
        private void Store(Expr reference, Symbol symbol) { if (symbol.ReadOnly) Fail(reference.Token, "A constant array cannot be modified."); Emit(symbol.Array ? Op.StoreArray : symbol.Local ? Op.StoreLocal : Op.Store, symbol.Offset, symbol.Array ? symbol.Size | (symbol.Local ? 65536 : 0) : 0, reference.Token); }
        private void CompileExpression(Expr expression)
        {
            Enter(expression.Token); try { CompileExpressionCore(expression); } finally { Leave(); }
        }
        private void CompileExpressionCore(Expr expression)
        {
            Token at = expression.Token; int value;
            switch (expression.Kind)
            {
                case "number": Emit(Op.Push, at.Number, 0, at); return;
                case "name":
                    if (constants.TryGetValue(at.Text, out value) || (value = ControlConstant(at.Text)) >= 0 || comboIds.TryGetValue(at.Text, out value)) { Emit(Op.Push, value, 0, at); return; }
                    Load(expression, Resolve(expression)); return;
                case "index": Load(expression, Resolve(expression)); return;
                case "conditional":
                    CompileExpression(expression.Left); int no = Emit(Op.JumpZero, 0, 0, at); CompileExpression(expression.Right); int end = Emit(Op.Jump, 0, 0, at); Patch(no, code.Count); CompileExpression(expression.Third); Patch(end, code.Count); return;
                case "binary":
                    if (at.Text == "&&" || at.Text == "||")
                    {
                        CompileExpression(expression.Left); int skip = Emit(Op.JumpZero, 0, 0, at);
                        if (at.Text == "&&") { CompileExpression(expression.Right); Emit(Op.Bool, 0, 0, at); int done = Emit(Op.Jump, 0, 0, at); Patch(skip, code.Count); Emit(Op.Push, 0, 0, at); Patch(done, code.Count); }
                        else { Emit(Op.Push, 1, 0, at); int done = Emit(Op.Jump, 0, 0, at); Patch(skip, code.Count); CompileExpression(expression.Right); Emit(Op.Bool, 0, 0, at); Patch(done, code.Count); } return;
                    }
                    CompileExpression(expression.Left); CompileExpression(expression.Right); Emit(BinaryOp(at), 0, 0, at); return;
                case "assign":
                    var target = expression.Left; if (target.Kind != "name" && target.Kind != "index") Fail(at, "Assignment needs a variable or an indexed array.");
                    var symbol = Resolve(target);
                    if (symbol.Array) { CompileExpression(target.Left); if (at.Text != "=") { Emit(Op.Dup, 0, 0, at); Emit(Op.LoadArray, symbol.Offset, symbol.Size | (symbol.Local ? 65536 : 0), at); } }
                    else if (at.Text != "=") Load(target, symbol);
                    CompileExpression(expression.Right); if (at.Text != "=") Emit(BinaryOp(at), 0, 0, at); Store(target, symbol); return;
                case "postfix": case "unary":
                    if (at.Text == "++" || at.Text == "--")
                    {
                        Expr reference = expression.Left; if (reference.Kind != "name" && reference.Kind != "index") Fail(at, "Increment needs a variable or an indexed array.");
                        var incrementSymbol = Resolve(reference); if (incrementSymbol.Array) { CompileExpression(reference.Left); Emit(Op.Dup, 0, 0, at); Emit(Op.LoadArray, incrementSymbol.Offset, incrementSymbol.Size | (incrementSymbol.Local ? 65536 : 0), at); }
                        else Load(reference, incrementSymbol);
                        Emit(Op.Push, 1, 0, at); Emit(at.Text == "++" ? Op.Add : Op.Sub, 0, 0, at); Store(reference, incrementSymbol);
                        if (expression.Kind == "postfix") { Emit(Op.Push, 1, 0, at); Emit(at.Text == "++" ? Op.Sub : Op.Add, 0, 0, at); } return;
                    }
                    CompileExpression(expression.Left); if (at.Text != "+") Emit(at.Text == "-" ? Op.Neg : at.Text == "!" ? Op.Not : Op.BitNot, 0, 0, at); return;
                case "call":
                    if (at.Text == "wait")
                    {
                        Fail(at, "wait(milliseconds) must be a standalone statement directly inside a combo."); return;
                    }
                    int builtin = Builtins.Find(at.Text), function;
                    if (builtin >= 0)
                    {
                        if (!Builtins.Accepts(builtin, expression.Args.Count)) Fail(at, "Invalid argument count for " + at.Text + ".");
                        foreach (Expr arg in expression.Args) CompileExpression(arg); Emit(Op.Builtin, builtin, expression.Args.Count, at); return;
                    }
                    if (!functionIds.TryGetValue(at.Text, out function) || functions[function].Combo || at.Text == "main" || at.Text == "init") Fail(at, "Unknown callable function '" + at.Text + "'.");
                    if (expression.Args.Count != functions[function].Parameters.Count) Fail(at, "Invalid argument count for " + at.Text + ".");
                    foreach (Expr arg in expression.Args) CompileExpression(arg); Emit(Op.Call, function, expression.Args.Count, at); return;
                default: Fail(at, "Unsupported expression."); return;
            }
        }
    }
}
