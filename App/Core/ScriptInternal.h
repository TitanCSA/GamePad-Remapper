#pragma once
#include "Script.h"
#include <vector>

namespace gpr::detail {
enum class Op {
    Push,
    Load,
    LoadParameter,
    Store,
    LoadLocal,
    StoreLocal,
    LoadArray,
    StoreArray,
    Dup,
    Dup2,
    Pop,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Neg,
    Not,
    BitNot,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Bool,
    Jump,
    JumpZero,
    Builtin,
    Call,
    Return,
    Wait
};
struct Token {
    std::string Text;
    int Number = 0, Line = 1, Column = 1;
    bool IsNumber = false, IsIdentifier = false, IsString = false;
    Token(std::string text = "", int line = 1, int column = 1, int number = 0, bool numeric = false,
          bool identifier = false)
        : Text(std::move(text)), Number(number), Line(line), Column(column), IsNumber(numeric),
          IsIdentifier(identifier) {}
};
struct Instruction {
    Op Code;
    int A = 0, B = 0, Line = 1, Column = 1;
};
struct CodeBlock {
    std::string Name;
    std::vector<Instruction> Code;
    int Arguments = 0, Locals = 0;
    bool Combo = false;
};
struct ProgramData {
    std::string Source;
    std::vector<CodeBlock> Blocks;
    int GlobalCount = 0, Init = 0, Main = 0;
    std::vector<int> ComboBlocks;
    ScriptInterface Interface;
};
const std::vector<std::string> &BuiltinNames();
int FindBuiltin(const std::string &name);
bool AcceptsBuiltin(int id, int count) noexcept;
bool UsesTime(int id) noexcept;
int Arithmetic(Op op, int a, int b, const Token &at);
inline int Wrapped(std::uint32_t v) noexcept { return static_cast<std::int32_t>(v); }
inline int Add(int a, int b) noexcept {
    return Wrapped(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
inline int Sub(int a, int b) noexcept {
    return Wrapped(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
inline int Mul(int a, int b) noexcept {
    return Wrapped(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}
} // namespace gpr::detail
