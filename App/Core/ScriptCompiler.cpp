#include "ScriptInternal.h"
#include <algorithm>
#include <climits>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace gpr::detail {
const std::vector<std::string> &BuiltinNames() {
    static const std::vector<std::string> names{
        "get_val",       "get_ival",       "get_lval",   "set_val",
        "event_press",   "event_release",  "get_ptime",  "get_rtime",
        "block",         "swap",           "abs",        "min",
        "max",           "clamp",          "deadzone",   "sensitivity",
        "rand",          "combo_run",      "combo_stop", "combo_restart",
        "combo_running", "combo_stop_all", "remap",      "invert",
        "is_available",  "get_controller", "get_device", "get_runtime",
        "vm_tctrl",      "random",         "get_brtime", "get_pvar",
        "set_pvar",      "get_bit",        "set_bit",    "pow",
        "isqrt",         "hypot",          "add_val",    "sin",
        "cos",           "atan2",          "lerp",       "wave_sin",
        "wave_cos",      "tick",           "get_phase",  "double_tap"};
    return names;
}
int FindBuiltin(const std::string &name) {
    const auto &n = BuiltinNames();
    const auto i = std::find(n.begin(), n.end(), name);
    return i == n.end() ? -1 : static_cast<int>(i - n.begin());
}
bool AcceptsBuiltin(int id, int count) noexcept {
    switch (id) {
    case 7:
    case 21:
    case 25:
    case 26:
    case 27:
        return count == 0;
    case 3:
    case 8:
    case 9:
    case 11:
    case 12:
    case 16:
    case 22:
    case 29:
    case 32:
    case 33:
    case 35:
    case 37:
    case 38:
    case 41:
    case 47:
        return count == 2;
    case 13:
    case 34:
    case 42:
        return count == 3;
    case 31:
        return count == 4;
    case 14:
    case 15:
    case 43:
    case 44:
        return count == 2 || count == 3;
    case 39:
    case 40:
        return count == 1 || count == 2;
    default:
        return count == 1;
    }
}
bool UsesTime(int id) noexcept {
    return id == 6 || id == 7 || id == 27 || id == 30 || id == 43 || id == 44 || id == 46;
}
[[noreturn]] static void Fail(const Token &at, const std::string &message) {
    throw ScriptException(message, at.Line, at.Column);
}
int Arithmetic(Op op, int a, int b, const Token &at) {
    switch (op) {
    case Op::Add:
        return Add(a, b);
    case Op::Sub:
        return Sub(a, b);
    case Op::Mul:
        return Mul(a, b);
    case Op::Div:
        if (!b)
            Fail(at, "Division by zero.");
        return a == INT_MIN && b == -1 ? INT_MIN : a / b;
    case Op::Mod:
        if (!b)
            Fail(at, "Remainder by zero.");
        return a == INT_MIN && b == -1 ? 0 : a % b;
    case Op::BitAnd:
        return a & b;
    case Op::BitOr:
        return a | b;
    case Op::BitXor:
        return a ^ b;
    case Op::Shl:
        return Wrapped(static_cast<std::uint32_t>(a) << (b & 31));
    case Op::Shr: {
        const unsigned shift = static_cast<unsigned>(b) & 31;
        const auto bits = static_cast<std::uint32_t>(a);
        return Wrapped(!shift ? bits : (bits >> shift) | (a < 0 ? ~std::uint32_t{0} << (32 - shift) : 0));
    }
    case Op::Eq:
        return a == b;
    case Op::Ne:
        return a != b;
    case Op::Lt:
        return a < b;
    case Op::Le:
        return a <= b;
    case Op::Gt:
        return a > b;
    case Op::Ge:
        return a >= b;
    default:
        Fail(at, "Unsupported arithmetic operator.");
    }
}
namespace {
template <class T> int N(const T &value) { return static_cast<int>(value.size()); }
std::uint32_t Decode(const std::string &text, std::size_t position, std::size_t &length) {
    if (position >= text.size()) {
        length = 0;
        return 0;
    }
    const auto first = static_cast<unsigned char>(text[position]);
    if (first < 128) {
        length = 1;
        return first;
    }
    length = first >= 0xc2 && first <= 0xdf   ? 2
             : first >= 0xe0 && first <= 0xef ? 3
             : first >= 0xf0 && first <= 0xf4 ? 4
                                              : 0;
    if (!length || position + length > text.size())
        throw ScriptException("Source must be valid UTF-8.");
    std::uint32_t cp = first & ((1u << (7 - length)) - 1u);
    for (std::size_t i = 1; i < length; ++i) {
        const auto c = static_cast<unsigned char>(text[position + i]);
        if ((c & 0xc0) != 0x80)
            throw ScriptException("Source must be valid UTF-8.");
        cp = (cp << 6) | (c & 0x3f);
    }
    if ((length == 2 && cp < 0x80) || (length == 3 && cp < 0x800) || (length == 4 && cp < 0x10000) ||
        cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        throw ScriptException("Source must be valid UTF-8.");
    return cp;
}
bool Space(std::uint32_t c) {
    return (c >= 9 && c <= 13) || c == 32 || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
           c == 0x3000;
}
bool Letter(std::uint32_t cp) {
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))
        return true;
#ifdef _WIN32
    if (cp > 127 && cp < 0xd800 || (cp > 0xdfff && cp <= 0xffff)) {
        WORD type = 0;
        const wchar_t c = static_cast<wchar_t>(cp);
        return GetStringTypeW(CT_CTYPE1, &c, 1, &type) && (type & C1_ALPHA) != 0;
    }
#else
    if (cp > 127 && cp <= 0xffff)
        return true;
#endif
    return false;
}
bool Digit(std::uint32_t c) { return c >= '0' && c <= '9'; }
class Lexer {
    const std::string &source;
    std::size_t position = 0;
    int line = 1, column = 1;
    char Current() const { return position < source.size() ? source[position] : '\0'; }
    std::uint32_t Point() const {
        std::size_t length = 0;
        return Decode(source, position, length);
    }
    char Peek() const { return position + 1 < source.size() ? source[position + 1] : '\0'; }
    void Advance() {
        std::size_t length = 0;
        const auto cp = Decode(source, position, length);
        if (cp == '\n') {
            ++line;
            column = 1;
        } else
            column += cp > 0xffff ? 2 : 1;
        position += length;
    }

  public:
    explicit Lexer(const std::string &text) : source(text) {}
    std::vector<Token> Scan() {
        std::vector<Token> result;
        while (position < source.size()) {
            if (Space(Point())) {
                Advance();
                continue;
            }
            if (Current() == '/' && Peek() == '/') {
                while (Current() != '\n' && position < source.size())
                    Advance();
                continue;
            }
            if (Current() == '/' && Peek() == '*') {
                const auto beginLine = line, beginColumn = column;
                Advance();
                Advance();
                while (position < source.size() && !(Current() == '*' && Peek() == '/'))
                    Advance();
                if (position >= source.size())
                    throw ScriptException("Unterminated block comment.", beginLine, beginColumn);
                Advance();
                Advance();
                continue;
            }
            const auto start = position;
            const int sl = line, sc = column;
            if (Letter(Point()) || Current() == '_') {
                do {
                    Advance();
                } while (Letter(Point()) || Digit(Point()) || Current() == '_');
                if (column - sc > 128)
                    throw ScriptException("An identifier may contain at most 128 characters.", sl, sc);
                result.emplace_back(source.substr(start, position - start), sl, sc, 0, false, true);
            } else if (Digit(Point())) {
                const bool hex = Current() == '0' && (Peek() == 'x' || Peek() == 'X');
                if (hex) {
                    Advance();
                    Advance();
                }
                const auto digits = position;
                std::uint64_t number = 0;
                bool overflow = false;
                while (true) {
                    const char c = Current();
                    const int d = c >= '0' && c <= '9'   ? c - '0'
                                  : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                  : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                         : -1;
                    if (d < 0 || d >= (hex ? 16 : 10))
                        break;
                    number = number * (hex ? 16u : 10u) + static_cast<unsigned>(d);
                    overflow |= number > UINT32_MAX;
                    if (overflow)
                        number = 0;
                    Advance();
                }
                if (position == digits || overflow)
                    throw ScriptException("Invalid 32-bit integer literal.", sl, sc);
                result.emplace_back(source.substr(start, position - start), sl, sc,
                                    Wrapped(static_cast<std::uint32_t>(number)), true);
            } else {
                const auto two = source.substr(position, 2), three = source.substr(position, 3);
                if (three == "<<=" || three == ">>=") {
                    Advance();
                    Advance();
                    Advance();
                    result.emplace_back(three, sl, sc);
                } else if (two == "==" || two == "!=" || two == "<=" || two == ">=" || two == "&&" ||
                           two == "||" || two == "++" || two == "--" || two == "+=" || two == "-=" ||
                           two == "*=" || two == "/=" || two == "%=" || two == "&=" || two == "|=" ||
                           two == "^=" || two == "<<" || two == ">>") {
                    Advance();
                    Advance();
                    result.emplace_back(two, sl, sc);
                } else if (std::string("{}()[];,:?=+-*/%!~&|^<>").find(Current()) != std::string::npos) {
                    const std::string one(1, Current());
                    Advance();
                    result.emplace_back(one, sl, sc);
                } else
                    throw ScriptException("Unexpected character. Strings, imports and operating-system calls "
                                          "are not supported.",
                                          sl, sc);
            }
            if (result.size() > 100000)
                throw ScriptException("A script may contain at most 100,000 tokens.", sl, sc);
        }
        result.emplace_back("<end>", line, column);
        return result;
    }
};
struct Expr;
struct Statement;
using E = Expr *;
using S = std::shared_ptr<Statement>;
struct Expr {
    Token At;
    std::string Kind;
    E Left = nullptr, Right = nullptr, Third = nullptr;
    std::vector<E> Args;
    Expr(Token at, std::string kind, E left = {}, E right = {})
        : At(std::move(at)), Kind(std::move(kind)), Left(left), Right(right) {}
};
struct Statement {
    Token At;
    std::string Kind, Name;
    std::vector<S> Items;
    E Expression = nullptr, Condition = nullptr, Increment = nullptr;
    S Body, Else, Initializer;
    int Size = 0;
    bool HasInitialValues = false;
    std::vector<E> InitialValues;
    Statement(Token at, std::string kind) : At(std::move(at)), Kind(std::move(kind)) {}
};
struct Function {
    Token At;
    std::string Name;
    std::vector<std::string> Parameters;
    S Body;
    bool Combo = false;
};
struct Symbol {
    int Offset = 0, Size = 0;
    bool Local = false, Array = false, ReadOnly = false;
};
struct Loop {
    std::vector<int> Breaks, Continues;
};
class Compiler {
    const std::string &source;
    std::vector<Token> tokens;
    int position = 0, depth = 0, globalCount = 0;
    std::unordered_map<std::string, int> constants, functionsByName, combosByName;
    std::unordered_map<std::string, Symbol> globals, locals;
    std::vector<Function> functions;
    std::vector<S> initializers;
    std::vector<Instruction> code;
    std::vector<Loop> loops;
    // Flat expressions can contain tens of thousands of nodes before the
    // nesting check rejects them. Arena ownership prevents recursive teardown
    // of their left-associated expression chains on the error path.
    std::vector<std::unique_ptr<Expr>> expressions;
    Function *currentFunction = nullptr;
    int localCount = 0, instructionCount = 0;
    E MakeExpr(Token at, std::string kind, E left = nullptr, E right = nullptr) {
        auto e = std::make_unique<Expr>(std::move(at), std::move(kind), left, right);
        const auto p = e.get();
        expressions.push_back(std::move(e));
        return p;
    }
    const Token &Current() const { return tokens.at(position); }
    Token Next() { return tokens.at(position++); }
    bool Take(const std::string &value) {
        if (Current().Text != value)
            return false;
        ++position;
        return true;
    }
    Token Expect(const std::string &value) {
        if (Current().Text != value)
            Fail(Current(), "Expected '" + value + "', found '" + Current().Text + "'.");
        return Next();
    }
    Token Identifier() {
        if (!Current().IsIdentifier)
            Fail(Current(), "Expected an identifier.");
        return Next();
    }
    void Enter(const Token &at) {
        if (++depth > 96)
            Fail(at, "Syntax nesting exceeds the safe limit (96).");
    }
    void Leave() { --depth; }
    static int ControlConstant(const std::string &name) {
        for (const char c : name)
            if (c >= 'a' && c <= 'z')
                return -1;
        return Controls::Parse(name);
    }
    bool Reserved(const std::string &name) const {
        static const std::unordered_set<std::string> keywords{
            "define", "const", "int", "main",  "init",     "combo",  "function", "if",
            "else",   "while", "for", "break", "continue", "return", "wait"};
        return keywords.count(name) || constants.count(name) || ControlConstant(name) >= 0 ||
               globals.count(name) || functionsByName.count(name) || FindBuiltin(name) >= 0;
    }
    S ParseBlock() {
        const auto at = Expect("{");
        Enter(at);
        auto node = std::make_shared<Statement>(at, "block");
        while (!Take("}")) {
            if (Current().Text == "<end>")
                Fail(Current(), "Unterminated block.");
            node->Items.push_back(ParseStatement());
        }
        Leave();
        return node;
    }
    void AddGlobal(const S &node, bool readOnly) {
        if (Reserved(node->Name))
            Fail(node->At, "Duplicate or reserved name '" + node->Name + "'.");
        globals.emplace(node->Name, Symbol{globalCount, node->Size == 0 ? 1 : node->Size, false,
                                           node->Size > 0, readOnly});
        globalCount += node->Size == 0 ? 1 : node->Size;
        if (globalCount > 4096)
            Fail(node->At, "Global storage exceeds 4,096 integers.");
        initializers.push_back(node);
    }
    S ParseDeclaration(const Token &at, bool semicolon) {
        auto group = std::make_shared<Statement>(at, "declarations");
        do {
            const auto name = Identifier();
            auto item = std::make_shared<Statement>(name, "declaration");
            item->Name = name.Text;
            if (Take("[")) {
                if (Current().Text == "]")
                    item->Size = -1;
                else {
                    item->Size = Constant(ParseExpression());
                    if (item->Size < 1 || item->Size > 1024)
                        Fail(name, "Array length must be between 1 and 1,024.");
                }
                Expect("]");
            }
            if (Take("=")) {
                if (item->Size != 0) {
                    Expect("{");
                    item->HasInitialValues = true;
                    if (!Take("}")) {
                        do {
                            item->InitialValues.push_back(ParseExpression());
                        } while (Take(",") && Current().Text != "}");
                        Expect("}");
                    }
                    if (item->Size == -1)
                        item->Size = N(item->InitialValues);
                    if (item->Size == 0 || item->Size > 1024 || N(item->InitialValues) > item->Size)
                        Fail(name, "Invalid array initializer length (maximum 1,024 elements).");
                } else
                    item->Expression = ParseExpression();
            }
            if (item->Size == -1)
                Fail(name, "An unsized array needs an initializer.");
            group->Items.push_back(item);
        } while (Take(","));
        if (semicolon)
            Expect(";");
        return group;
    }
    S ParseStatement() {
        Enter(Current());
        S result;
        if (Current().Text == "{")
            result = ParseBlock();
        else {
            const auto at = Current();
            if (Take(";"))
                result = std::make_shared<Statement>(at, "empty");
            else if (Take("int"))
                result = ParseDeclaration(at, true);
            else if (Take("if")) {
                result = std::make_shared<Statement>(at, "if");
                Expect("(");
                result->Condition = ParseExpression();
                Expect(")");
                result->Body = ParseStatement();
                if (Take("else"))
                    result->Else = ParseStatement();
            } else if (Take("while")) {
                result = std::make_shared<Statement>(at, "while");
                Expect("(");
                result->Condition = ParseExpression();
                Expect(")");
                result->Body = ParseStatement();
            } else if (Take("for")) {
                result = std::make_shared<Statement>(at, "for");
                Expect("(");
                if (Take("int"))
                    result->Initializer = ParseDeclaration(at, false);
                else if (Current().Text != ";") {
                    result->Initializer = std::make_shared<Statement>(Current(), "expression");
                    result->Initializer->Expression = ParseExpression();
                }
                Expect(";");
                if (Current().Text != ";")
                    result->Condition = ParseExpression();
                Expect(";");
                if (Current().Text != ")")
                    result->Increment = ParseExpression();
                Expect(")");
                result->Body = ParseStatement();
            } else if (Take("break") || Take("continue")) {
                result = std::make_shared<Statement>(at, at.Text);
                Expect(";");
            } else if (Take("return")) {
                result = std::make_shared<Statement>(at, "return");
                if (Current().Text != ";")
                    result->Expression = ParseExpression();
                Expect(";");
            } else {
                result = std::make_shared<Statement>(at, "expression");
                result->Expression = ParseExpression();
                Expect(";");
            }
        }
        Leave();
        return result;
    }
    static int Precedence(const std::string &op) {
        if (op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%=" || op == "&=" ||
            op == "|=" || op == "^=" || op == "<<=" || op == ">>=")
            return 1;
        if (op == "?")
            return 2;
        if (op == "||")
            return 3;
        if (op == "&&")
            return 4;
        if (op == "|")
            return 5;
        if (op == "^")
            return 6;
        if (op == "&")
            return 7;
        if (op == "==" || op == "!=")
            return 8;
        if (op == "<" || op == "<=" || op == ">" || op == ">=")
            return 9;
        if (op == "<<" || op == ">>")
            return 10;
        if (op == "+" || op == "-")
            return 11;
        if (op == "*" || op == "/" || op == "%")
            return 12;
        return 0;
    }
    E ParseExpression(int minimum = 1) {
        Enter(Current());
        const auto at = Next();
        E left = nullptr;
        if (at.IsNumber)
            left = MakeExpr(at, "number");
        else if (at.Text == "(") {
            left = ParseExpression();
            Expect(")");
        } else if (at.Text == "+" || at.Text == "-" || at.Text == "!" || at.Text == "~" || at.Text == "++" ||
                   at.Text == "--")
            left = MakeExpr(at, "unary", ParseExpression(13));
        else if (at.IsIdentifier)
            left = MakeExpr(at, "name");
        else
            Fail(at, "Expected an expression.");
        while (true) {
            if (Current().Text == "(" && left->Kind == "name") {
                Next();
                auto call = MakeExpr(left->At, "call");
                if (!Take(")")) {
                    do {
                        call->Args.push_back(ParseExpression());
                        if (call->Args.size() > 16)
                            Fail(call->At, "At most 16 arguments are supported.");
                    } while (Take(","));
                    Expect(")");
                }
                left = call;
                continue;
            }
            if (Take("[")) {
                if (left->Kind != "name")
                    Fail(left->At, "Only named arrays may be indexed.");
                left = MakeExpr(left->At, "index", ParseExpression());
                Expect("]");
                continue;
            }
            if (Current().Text == "++" || Current().Text == "--") {
                left = MakeExpr(Next(), "postfix", left);
                continue;
            }
            const int p = Precedence(Current().Text);
            if (p < minimum || !p)
                break;
            const auto op = Next();
            if (op.Text == "?") {
                auto ternary = MakeExpr(op, "conditional", left, ParseExpression());
                Expect(":");
                ternary->Third = ParseExpression(2);
                left = ternary;
            } else
                left = MakeExpr(op, p == 1 ? "assign" : "binary", left, ParseExpression(p == 1 ? p : p + 1));
        }
        Leave();
        return left;
    }
    static Op BinaryOp(const Token &at) {
        const auto &op = at.Text;
        if (op == "+" || op == "+=")
            return Op::Add;
        if (op == "-" || op == "-=")
            return Op::Sub;
        if (op == "*" || op == "*=")
            return Op::Mul;
        if (op == "/" || op == "/=")
            return Op::Div;
        if (op == "%" || op == "%=")
            return Op::Mod;
        if (op == "&" || op == "&=")
            return Op::BitAnd;
        if (op == "|" || op == "|=")
            return Op::BitOr;
        if (op == "^" || op == "^=")
            return Op::BitXor;
        if (op == "<<" || op == "<<=")
            return Op::Shl;
        if (op == ">>" || op == ">>=")
            return Op::Shr;
        if (op == "==")
            return Op::Eq;
        if (op == "!=")
            return Op::Ne;
        if (op == "<")
            return Op::Lt;
        if (op == "<=")
            return Op::Le;
        if (op == ">")
            return Op::Gt;
        if (op == ">=")
            return Op::Ge;
        Fail(at, "Unsupported binary operator.");
    }
    int Constant(const E &e) {
        Enter(e->At);
        const int result = ConstantCore(e);
        Leave();
        return result;
    }
    int ConstantCore(const E &e) {
        const auto &kind = e->Kind;
        const auto &op = e->At.Text;
        if (kind == "number")
            return e->At.Number;
        if (kind == "name") {
            const auto i = constants.find(op);
            if (i != constants.end())
                return i->second;
            const auto c = ControlConstant(op);
            if (c >= 0)
                return c;
        }
        if (kind == "unary") {
            const int a = Constant(e->Left);
            if (op == "+")
                return a;
            if (op == "-")
                return Sub(0, a);
            if (op == "!")
                return !a;
            if (op == "~")
                return ~a;
        }
        if (kind == "binary") {
            const int a = Constant(e->Left);
            if (op == "&&" && !a)
                return 0;
            if (op == "||" && a)
                return 1;
            const int b = Constant(e->Right);
            if (op == "&&" || op == "||")
                return b != 0;
            return Arithmetic(BinaryOp(e->At), a, b, e->At);
        }
        if (kind == "conditional")
            return Constant(e->Left) ? Constant(e->Right) : Constant(e->Third);
        Fail(e->At, "A constant integer expression is required here.");
    }
    int Emit(Op op, int a, int b, const Token &at) {
        if (++instructionCount > 32768)
            Fail(at, "Compiled program exceeds 32,768 instructions.");
        code.push_back({op, a, b, at.Line, at.Column});
        return N(code) - 1;
    }
    void Patch(int index, int target) { code[index].A = target; }
    Symbol Resolve(const E &e) {
        auto i = locals.find(e->At.Text);
        Symbol s;
        if (i != locals.end())
            s = i->second;
        else {
            const auto g = globals.find(e->At.Text);
            if (g == globals.end())
                Fail(e->At, "Unknown variable '" + e->At.Text + "'.");
            s = g->second;
        }
        if ((e->Kind == "index") != s.Array)
            Fail(e->At, s.Array ? "An array needs an index." : "This variable is not an array.");
        return s;
    }
    void CompileDeclaration(const S &node, bool local) {
        Symbol symbol;
        if (local) {
            if (Reserved(node->Name) || locals.count(node->Name))
                Fail(node->At,
                     "Duplicate or reserved variable '" + node->Name + "'. Locals are function-scoped.");
            symbol = {localCount, node->Size == 0 ? 1 : node->Size, true, node->Size > 0, false};
            locals.emplace(node->Name, symbol);
            localCount += symbol.Size;
            if (localCount > 256)
                Fail(node->At,
                     "A function may use at most 256 local integers, including arguments and arrays.");
        } else
            symbol = globals.at(node->Name);
        if (!symbol.Array) {
            if (node->Expression)
                CompileExpression(node->Expression);
            else
                Emit(Op::Push, 0, 0, node->At);
            Emit(symbol.Local ? Op::StoreLocal : Op::Store, symbol.Offset, 0, node->At);
            Emit(Op::Pop, 0, 0, node->At);
        } else if (node->HasInitialValues)
            for (int i = 0; i < N(node->InitialValues); ++i) {
                Emit(Op::Push, i, 0, node->At);
                CompileExpression(node->InitialValues[i]);
                Emit(Op::StoreArray, symbol.Offset, symbol.Size | (symbol.Local ? 65536 : 0), node->At);
                Emit(Op::Pop, 0, 0, node->At);
            }
    }
    void CompileStatement(const S &n) {
        const auto &kind = n->Kind;
        if (kind == "empty")
            return;
        if (kind == "block" || kind == "declarations") {
            for (const auto &item : n->Items)
                CompileStatement(item);
            return;
        }
        if (kind == "declaration") {
            CompileDeclaration(n, true);
            return;
        }
        if (kind == "expression") {
            if (n->Expression->Kind == "call" && n->Expression->At.Text == "wait") {
                if (!currentFunction->Combo || n->Expression->Args.size() != 1)
                    Fail(n->Expression->At, "wait(milliseconds) is only valid as a standalone statement "
                                            "directly inside a combo.");
                CompileExpression(n->Expression->Args[0]);
                Emit(Op::Wait, 0, 0, n->Expression->At);
                return;
            }
            CompileExpression(n->Expression);
            Emit(Op::Pop, 0, 0, n->At);
            return;
        }
        if (kind == "if") {
            CompileExpression(n->Condition);
            const int no = Emit(Op::JumpZero, 0, 0, n->At);
            CompileStatement(n->Body);
            if (!n->Else)
                Patch(no, N(code));
            else {
                const int end = Emit(Op::Jump, 0, 0, n->At);
                Patch(no, N(code));
                CompileStatement(n->Else);
                Patch(end, N(code));
            }
            return;
        }
        if (kind == "while" || kind == "for") {
            if (n->Initializer && n->Initializer->Kind == "expression" &&
                n->Initializer->Expression->Kind == "call" && n->Initializer->Expression->At.Text == "wait")
                Fail(n->Initializer->Expression->At,
                     "wait(milliseconds) must be a standalone statement directly inside a combo.");
            if (n->Initializer)
                CompileStatement(n->Initializer);
            const int start = N(code);
            int exit = -1;
            if (n->Condition) {
                CompileExpression(n->Condition);
                exit = Emit(Op::JumpZero, 0, 0, n->At);
            }
            loops.emplace_back();
            CompileStatement(n->Body);
            const int increment = N(code);
            if (n->Increment) {
                CompileExpression(n->Increment);
                Emit(Op::Pop, 0, 0, n->At);
            }
            Emit(Op::Jump, start, 0, n->At);
            if (exit >= 0)
                Patch(exit, N(code));
            for (const auto jump : loops.back().Breaks)
                Patch(jump, N(code));
            for (const auto jump : loops.back().Continues)
                Patch(jump, increment);
            loops.pop_back();
            return;
        }
        if (kind == "break" || kind == "continue") {
            if (loops.empty())
                Fail(n->At, kind + " requires a surrounding loop.");
            const auto branch = Emit(Op::Jump, 0, 0, n->At);
            (kind == "break" ? loops.back().Breaks : loops.back().Continues).push_back(branch);
            return;
        }
        if (kind == "return") {
            if (n->Expression)
                CompileExpression(n->Expression);
            else
                Emit(Op::Push, 0, 0, n->At);
            Emit(Op::Return, 0, 0, n->At);
            return;
        }
        Fail(n->At, "Unsupported statement.");
    }
    void Load(const E &e, const Symbol &s) {
        if (s.Array) {
            CompileExpression(e->Left);
            Emit(Op::LoadArray, s.Offset, s.Size | (s.Local ? 65536 : 0), e->At);
        } else
            Emit(s.Local ? Op::LoadLocal : Op::Load, s.Offset, 0, e->At);
    }
    void Store(const E &e, const Symbol &s) {
        if (s.ReadOnly)
            Fail(e->At, "A constant array cannot be modified.");
        Emit(s.Array   ? Op::StoreArray
             : s.Local ? Op::StoreLocal
                       : Op::Store,
             s.Offset, s.Array ? s.Size | (s.Local ? 65536 : 0) : 0, e->At);
    }
    void CompileExpression(const E &e) {
        Enter(e->At);
        CompileExpressionCore(e);
        Leave();
    }
    void CompileExpressionCore(const E &e) {
        const auto at = e->At;
        const auto &kind = e->Kind;
        if (kind == "number") {
            Emit(Op::Push, at.Number, 0, at);
            return;
        }
        if (kind == "name") {
            const auto c = constants.find(at.Text);
            const auto combo = combosByName.find(at.Text);
            const int control = ControlConstant(at.Text);
            if (c != constants.end() || control >= 0 || combo != combosByName.end()) {
                Emit(Op::Push,
                     c != constants.end() ? c->second
                     : control >= 0       ? control
                                          : combo->second,
                     0, at);
                return;
            }
            Load(e, Resolve(e));
            return;
        }
        if (kind == "index") {
            Load(e, Resolve(e));
            return;
        }
        if (kind == "conditional") {
            CompileExpression(e->Left);
            const auto no = Emit(Op::JumpZero, 0, 0, at);
            CompileExpression(e->Right);
            const auto end = Emit(Op::Jump, 0, 0, at);
            Patch(no, N(code));
            CompileExpression(e->Third);
            Patch(end, N(code));
            return;
        }
        if (kind == "binary") {
            if (at.Text == "&&" || at.Text == "||") {
                CompileExpression(e->Left);
                const auto skip = Emit(Op::JumpZero, 0, 0, at);
                if (at.Text == "&&") {
                    CompileExpression(e->Right);
                    Emit(Op::Bool, 0, 0, at);
                    const auto done = Emit(Op::Jump, 0, 0, at);
                    Patch(skip, N(code));
                    Emit(Op::Push, 0, 0, at);
                    Patch(done, N(code));
                } else {
                    Emit(Op::Push, 1, 0, at);
                    const auto done = Emit(Op::Jump, 0, 0, at);
                    Patch(skip, N(code));
                    CompileExpression(e->Right);
                    Emit(Op::Bool, 0, 0, at);
                    Patch(done, N(code));
                }
                return;
            }
            CompileExpression(e->Left);
            CompileExpression(e->Right);
            Emit(BinaryOp(at), 0, 0, at);
            return;
        }
        if (kind == "assign") {
            const auto target = e->Left;
            if (target->Kind != "name" && target->Kind != "index")
                Fail(at, "Assignment needs a variable or an indexed array.");
            const auto s = Resolve(target);
            if (s.Array) {
                CompileExpression(target->Left);
                if (at.Text != "=") {
                    Emit(Op::Dup, 0, 0, at);
                    Emit(Op::LoadArray, s.Offset, s.Size | (s.Local ? 65536 : 0), at);
                }
            } else if (at.Text != "=")
                Load(target, s);
            CompileExpression(e->Right);
            if (at.Text != "=")
                Emit(BinaryOp(at), 0, 0, at);
            Store(target, s);
            return;
        }
        if (kind == "postfix" || kind == "unary") {
            if (at.Text == "++" || at.Text == "--") {
                const auto ref = e->Left;
                if (ref->Kind != "name" && ref->Kind != "index")
                    Fail(at, "Increment needs a variable or an indexed array.");
                const auto s = Resolve(ref);
                if (s.Array) {
                    CompileExpression(ref->Left);
                    Emit(Op::Dup, 0, 0, at);
                    Emit(Op::LoadArray, s.Offset, s.Size | (s.Local ? 65536 : 0), at);
                } else
                    Load(ref, s);
                Emit(Op::Push, 1, 0, at);
                Emit(at.Text == "++" ? Op::Add : Op::Sub, 0, 0, at);
                Store(ref, s);
                if (kind == "postfix") {
                    Emit(Op::Push, 1, 0, at);
                    Emit(at.Text == "++" ? Op::Sub : Op::Add, 0, 0, at);
                }
                return;
            }
            CompileExpression(e->Left);
            if (at.Text != "+")
                Emit(at.Text == "-" ? Op::Neg : at.Text == "!" ? Op::Not : Op::BitNot, 0, 0, at);
            return;
        }
        if (kind == "call") {
            if (at.Text == "wait")
                Fail(at, "wait(milliseconds) must be a standalone statement directly inside a combo.");
            const auto builtin = FindBuiltin(at.Text);
            if (builtin >= 0) {
                if (!AcceptsBuiltin(builtin, N(e->Args)))
                    Fail(at, "Invalid argument count for " + at.Text + ".");
                for (const auto &a : e->Args)
                    CompileExpression(a);
                Emit(Op::Builtin, builtin, N(e->Args), at);
                return;
            }
            const auto fn = functionsByName.find(at.Text);
            if (fn == functionsByName.end() || functions[fn->second].Combo || at.Text == "main" ||
                at.Text == "init")
                Fail(at, "Unknown callable function '" + at.Text + "'.");
            if (e->Args.size() != functions[fn->second].Parameters.size())
                Fail(at, "Invalid argument count for " + at.Text + ".");
            for (const auto &a : e->Args)
                CompileExpression(a);
            Emit(Op::Call, fn->second, N(e->Args), at);
            return;
        }
        Fail(at, "Unsupported expression.");
    }

  public:
    explicit Compiler(const std::string &text) : source(text), tokens(Lexer(text).Scan()) {
        constants["TRUE"] = constants["true"] = 1;
        constants["FALSE"] = constants["false"] = 0;
    }
    std::shared_ptr<const ProgramData> Compile() {
        while (Current().Text != "<end>") {
            const auto top = Next();
            if (top.Text == "define" || top.Text == "const") {
                if (top.Text == "const")
                    Take("int");
                const auto name = Identifier();
                if (top.Text == "const" && Current().Text == "[") {
                    --position;
                    const auto declaration = ParseDeclaration(top, true);
                    for (const auto &entry : declaration->Items) {
                        if (!entry->Size || !entry->HasInitialValues)
                            Fail(entry->At, "A constant array needs an initializer.");
                        for (const auto &value : entry->InitialValues)
                            (void)Constant(value);
                        AddGlobal(entry, true);
                    }
                    continue;
                }
                if (Reserved(name.Text))
                    Fail(name, "Duplicate or reserved name '" + name.Text + "'.");
                Expect("=");
                const int value = Constant(ParseExpression());
                Expect(";");
                constants.emplace(name.Text, value);
            } else if (top.Text == "int") {
                const auto declaration = ParseDeclaration(top, true);
                for (const auto &entry : declaration->Items)
                    AddGlobal(entry, false);
            } else if (top.Text == "main" || top.Text == "init" || top.Text == "combo" ||
                       top.Text == "function") {
                Function fn;
                fn.At = top;
                fn.Name = top.Text;
                fn.Combo = top.Text == "combo";
                if (top.Text == "combo" || top.Text == "function") {
                    fn.Name = Identifier().Text;
                    if (Reserved(fn.Name))
                        Fail(top, "Duplicate or reserved function name '" + fn.Name + "'.");
                } else if (functionsByName.count(fn.Name))
                    Fail(top, "Duplicate or reserved function name '" + fn.Name + "'.");
                if (functions.size() >= 96)
                    Fail(top, "At most 96 functions, including main, init and combos, are supported.");
                functionsByName.emplace(fn.Name, N(functions));
                if (fn.Combo) {
                    if (combosByName.size() >= 32)
                        Fail(top, "At most 32 combos are supported.");
                    combosByName.emplace(fn.Name, N(combosByName));
                }
                if (top.Text == "function") {
                    Expect("(");
                    if (!Take(")")) {
                        do {
                            Take("int");
                            const auto parameter = Identifier();
                            if (std::find(fn.Parameters.begin(), fn.Parameters.end(), parameter.Text) !=
                                    fn.Parameters.end() ||
                                Reserved(parameter.Text))
                                Fail(parameter, "Duplicate or reserved parameter name.");
                            fn.Parameters.push_back(parameter.Text);
                            if (fn.Parameters.size() > 16)
                                Fail(parameter, "At most 16 function arguments are supported.");
                        } while (Take(","));
                        Expect(")");
                    }
                }
                fn.Body = ParseBlock();
                functions.push_back(std::move(fn));
            } else
                Fail(top, "Expected define, int, init, main, function or combo.");
        }
        if (!functionsByName.count("main"))
            throw ScriptException("A script needs a main { ... } block.");
        if (!functionsByName.count("init")) {
            if (functions.size() >= 96)
                Fail(Current(), "At most 96 functions, including main, init and combos, are supported.");
            functionsByName.emplace("init", N(functions));
            Function fn;
            fn.Name = "init";
            fn.At = Token("init");
            fn.Body = std::make_shared<Statement>(Token("{"), "block");
            functions.push_back(std::move(fn));
        }
        auto program = std::make_shared<ProgramData>();
        program->Source = source;
        program->GlobalCount = globalCount;
        program->Init = functionsByName.at("init");
        program->Main = functionsByName.at("main");
        for (auto &fn : functions) {
            currentFunction = &fn;
            code.clear();
            locals.clear();
            localCount = 0;
            for (const auto &arg : fn.Parameters)
                locals.emplace(arg, Symbol{localCount++, 1, true, false, false});
            if (fn.Name == "init")
                for (const auto &declaration : initializers)
                    CompileDeclaration(declaration, false);
            CompileStatement(fn.Body);
            Emit(Op::Push, 0, 0, fn.At);
            Emit(Op::Return, 0, 0, fn.At);
            program->Blocks.push_back({fn.Name, std::move(code), N(fn.Parameters), localCount, fn.Combo});
        }
        program->ComboBlocks.resize(combosByName.size());
        for (const auto &item : combosByName)
            program->ComboBlocks[item.second] = functionsByName.at(item.first);
        return program;
    }
};
} // namespace
std::shared_ptr<const ProgramData> CompileProgram(const std::string &text) {
    std::size_t units = 0;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t length = 0;
        const auto cp = Decode(text, i, length);
        i += length;
        units += cp > 0xffff ? 2 : 1;
        if (units > 524288)
            throw ScriptException("A script may contain at most 512 KiB of text.");
    }
    return Compiler(text).Compile();
}
} // namespace gpr::detail
namespace gpr {
ScriptProgram::ScriptProgram(std::shared_ptr<const detail::ProgramData> data) : data_(std::move(data)) {}
const std::string &ScriptProgram::Source() const noexcept { return data_->Source; }
std::shared_ptr<const ScriptProgram> ScriptCompiler::Compile(const std::string &source) {
    return std::shared_ptr<const ScriptProgram>(new ScriptProgram(detail::CompileProgram(source)));
}
} // namespace gpr
