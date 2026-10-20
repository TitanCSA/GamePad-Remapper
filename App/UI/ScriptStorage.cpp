#include "ScriptStorage.h"
#include "../ThirdParty/json.hpp"
#include <Windows.h>
#include <ShlObj.h>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <sstream>
#include <iomanip>

namespace gpr {
namespace {
std::filesystem::path SettingsFallback(const std::filesystem::path &script) {
    // Stable per-path identity, not an authentication token. The JSON is still
    // range checked against the compiled script before it reaches a VM.
    auto path = std::filesystem::absolute(script).lexically_normal().wstring();
    if (!path.empty())
        CharLowerBuffW(path.data(), static_cast<DWORD>(path.size()));
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto c : Utf8(path)) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    std::wostringstream name;
    name << std::hex << std::setw(16) << std::setfill(L'0') << hash << L".json";
    return ScriptStorage::RecoveryDirectory().parent_path() / L"Settings" / name.str();
}
} // namespace
std::filesystem::path ScriptStorage::SettingsFile(const std::filesystem::path &script) {
    const auto fallback = SettingsFallback(script);
    return std::filesystem::exists(fallback)
               ? fallback
               : std::filesystem::path(script.wstring() + L".settings.json");
}
std::map<std::string, int> ScriptStorage::ReadSettings(const std::filesystem::path &script) {
    if (script.empty())
        return {};
    const auto path = SettingsFile(script);
    if (!std::filesystem::exists(path))
        return {};
    const auto text = Read(path);
    const auto json = nlohmann::json::parse(text, [](int depth, auto, auto &) {
        if (depth > 4)
            throw std::runtime_error("The script settings file is nested too deeply.");
        return true;
    });
    if (!json.is_object() || !json.contains("schema") || !json["schema"].is_number_integer() ||
        json["schema"] != 1 || !json.contains("values") || !json["values"].is_object() ||
        json["values"].size() > 64)
        throw std::runtime_error("Invalid script settings file; saved values were not applied.");
    std::map<std::string, int> values;
    for (const auto &item : json["values"].items()) {
        if (item.key().size() > 512 || !item.value().is_number_integer() ||
            (item.value().is_number_unsigned() && item.value().get<std::uint64_t>() > INT_MAX) ||
            item.value().get<std::int64_t>() < INT_MIN || item.value().get<std::int64_t>() > INT_MAX)
            throw std::runtime_error("Invalid script parameter value in settings file.");
        values.emplace(item.key(), item.value().get<int>());
    }
    return values;
}
void ScriptStorage::WriteSettings(const std::filesystem::path &script, const ScriptInterface &schema,
                                  const ParameterValues &values) {
    if (script.empty())
        return;
    schema.Validate(values);
    auto fields = nlohmann::json::object();
    for (std::size_t i = 0; i < schema.Parameters.size(); ++i)
        fields[schema.Parameters[i].Name] = values[i];
    const auto text = nlohmann::json({{"schema", 1}, {"values", std::move(fields)}}).dump(2) + "\n";
    const auto fallback = SettingsFallback(script);
    try {
        Write(std::filesystem::path(script.wstring() + L".settings.json"), text, false);
        std::error_code ignored;
        std::filesystem::remove(fallback, ignored);
    } catch (const std::exception &) {
        Write(fallback, text, false);
    }
}
namespace {
std::filesystem::path Folder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &value);
    if (FAILED(result))
        throw std::runtime_error("Windows could not locate the script storage folder.");
    const std::filesystem::path path(value);
    CoTaskMemFree(value);
    return path;
}
[[noreturn]] void Error(const char *operation) {
    const auto code = GetLastError();
    throw std::runtime_error(std::string(operation) + " (Windows error " + std::to_string(code) + ").");
}
struct File final {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~File() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    void Close() {
        if (value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
            value = INVALID_HANDLE_VALUE;
        }
    }
};
} // namespace
std::wstring Utf16(const std::string &text) {
    if (text.empty())
        return {};
    if (text.size() > static_cast<std::size_t>(INT_MAX))
        throw std::runtime_error("Text is too large.");
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (!count)
        throw std::runtime_error(
            "The text is not valid UTF-8. Save the script as UTF-8 and open it again.");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                             result.data(), count))
        Error("Decode text");
    return result;
}
std::string Utf8(const std::wstring &text) {
    if (text.empty())
        return {};
    if (text.size() > static_cast<std::size_t>(INT_MAX))
        throw std::runtime_error("Text is too large.");
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count)
        throw std::runtime_error("The script contains invalid Unicode text and was not changed.");
    std::string result(static_cast<std::size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                             result.data(), count, nullptr, nullptr))
        Error("Encode text");
    return result;
}
std::filesystem::path ScriptStorage::DefaultDirectory() {
    return Folder(FOLDERID_Documents) / L"GamePad Remapper" / L"Scripts";
}
std::filesystem::path ScriptStorage::RecoveryDirectory() {
    return Folder(FOLDERID_LocalAppData) / L"GamePadRemapper" / L"Recovery";
}
std::filesystem::path ScriptStorage::SessionDirectory() {
    return Folder(FOLDERID_LocalAppData) / L"GamePadRemapper" / L"Sessions";
}
std::filesystem::path ScriptStorage::UniquePath(const std::filesystem::path &directory) {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
        throw std::runtime_error("Windows could not create a unique script name.");
    wchar_t name[40]{};
    StringFromGUID2(guid, name, 40);
    std::wstring value(name);
    value.erase(value.begin());
    value.pop_back();
    return directory / (value + L".igc");
}
std::string ScriptStorage::Read(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        throw std::runtime_error("The script could not be opened.");
    const auto length = stream.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > MaximumBytes * 2 + 2)
        throw std::runtime_error("Scripts may contain at most 512 KiB of UTF-8 text.");
    std::string source(static_cast<std::size_t>(length), '\0');
    stream.seekg(0);
    if (!source.empty() && !stream.read(source.data(), static_cast<std::streamsize>(source.size())))
        throw std::runtime_error("The script could not be read completely.");
    if (source.size() >= 2 && ((static_cast<unsigned char>(source[0]) == 0xff &&
                                static_cast<unsigned char>(source[1]) == 0xfe) ||
                               (static_cast<unsigned char>(source[0]) == 0xfe &&
                                static_cast<unsigned char>(source[1]) == 0xff))) {
        const bool big = static_cast<unsigned char>(source[0]) == 0xfe;
        if (source.size() % 2)
            throw std::runtime_error("The script contains incomplete UTF-16 text.");
        std::wstring decoded;
        decoded.reserve(source.size() / 2);
        for (std::size_t i = 2; i < source.size(); i += 2) {
            const auto a = static_cast<unsigned char>(source[i]),
                       b = static_cast<unsigned char>(source[i + 1]);
            decoded.push_back(static_cast<wchar_t>(big ? (a << 8) | b : a | (b << 8)));
        }
        source = Utf8(decoded);
    } else if (source.compare(0, 3, "\xef\xbb\xbf") == 0)
        source.erase(0, 3);
    if (source.size() > MaximumBytes)
        throw std::runtime_error("Scripts may contain at most 512 KiB of UTF-8 text.");
    if (source.find('\0') != std::string::npos)
        throw std::runtime_error("Scripts cannot contain embedded zero characters.");
    (void)Utf16(source);
    return source;
}
void ScriptStorage::Write(const std::filesystem::path &requested, const std::string &source,
                          bool backup) {
    if (source.size() > MaximumBytes || source.find('\0') != std::string::npos)
        throw std::runtime_error(
            "Scripts may contain at most 512 KiB of UTF-8 text, without zero characters.");
    (void)Utf16(source);
    const auto path = std::filesystem::absolute(requested);
    std::filesystem::create_directories(path.parent_path());
    auto temporary = UniquePath(path.parent_path());
    temporary += L".tmp";
    File file;
    file.value = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file.value == INVALID_HANDLE_VALUE)
        Error("Create temporary script");
    try {
        DWORD written = 0;
        if (!source.empty() && (!WriteFile(file.value, source.data(), static_cast<DWORD>(source.size()),
                                           &written, nullptr) ||
                                written != source.size()))
            Error("Write script");
        if (!FlushFileBuffers(file.value))
            Error("Flush script");
        file.Close();
        if (std::filesystem::exists(path)) {
            if (backup) {
                auto previous = path;
                previous += L".bak";
                if (!CopyFileW(path.c_str(), previous.c_str(), FALSE))
                    Error("Preserve previous script");
            }
            if (!ReplaceFileW(path.c_str(), temporary.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS,
                              nullptr, nullptr))
                Error("Replace script atomically");
        } else if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
            Error("Save script atomically");
    } catch (...) {
        file.Close();
        DeleteFileW(temporary.c_str());
        throw;
    }
}
} // namespace gpr
