#pragma once
#include <filesystem>
#include <string>

namespace gpr {
std::wstring Utf16(const std::string &text);
std::string Utf8(const std::wstring &text);
struct ScriptStorage final {
    static constexpr std::size_t MaximumBytes = 524288;
    static std::filesystem::path DefaultDirectory();
    static std::filesystem::path RecoveryDirectory();
    static std::filesystem::path SessionDirectory();
    static std::filesystem::path UniquePath(const std::filesystem::path &directory);
    static std::string Read(const std::filesystem::path &path);
    static void Write(const std::filesystem::path &path, const std::string &source, bool backup = true);
};
} // namespace gpr
