#pragma once
#include <Windows.h>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gpr::ui {
struct ScriptLibraryEntry {
    std::filesystem::path Path;
    std::wstring Label;
    std::wstring Group;
};

// An anchored, keyboard-accessible library. The result is an index in entries;
// searching and selecting never changes a file or the order of the caller's list.
std::optional<std::size_t> ShowScriptLibraryPopup(
    HWND owner, HWND anchor, const std::vector<ScriptLibraryEntry> &entries,
    const std::filesystem::path &current, const std::wstring &title = L"Script library",
    bool animate = true);

namespace detail {
// Also exercised without any input devices or driver by the offline UI tests.
std::vector<std::size_t> FilterScriptLibraryEntries(
    const std::vector<ScriptLibraryEntry> &entries, const std::wstring &query);
} // namespace detail
} // namespace gpr::ui
