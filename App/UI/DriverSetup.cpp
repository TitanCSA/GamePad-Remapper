#include "DriverSetup.h"
#include <Shellapi.h>
#include <array>
#include <cwchar>
#include <string>
#include <vector>

namespace gpr::ui {
bool IsGamepadDriverInstaller(const std::filesystem::path &path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error))
        return false;
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    // A setup identity is small. Do not allocate an unbounded resource supplied beside the app.
    if (!size || size > 1024 * 1024)
        return false;
    std::vector<unsigned char> bytes(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data()))
        return false;
    struct Translation {
        WORD Language, CodePage;
    };
    Translation *translations = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(bytes.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void **>(&translations), &length) ||
        !translations || length % sizeof(Translation) != 0)
        return false;
    for (std::size_t i = 0; i < length / sizeof(Translation); ++i) {
        std::array<wchar_t, 80> key{};
        swprintf_s(key.data(), key.size(), L"\\StringFileInfo\\%04x%04x\\ProductName",
                   translations[i].Language, translations[i].CodePage);
        wchar_t *product = nullptr;
        UINT characters = 0;
        constexpr wchar_t expected[] = L"InterceptorGamepadController";
        if (VerQueryValueW(bytes.data(), key.data(), reinterpret_cast<void **>(&product), &characters) &&
            product && characters == std::size(expected) &&
            std::wmemcmp(product, expected, std::size(expected)) == 0)
            return true;
    }
    return false;
}

DriverInstallerSelection FindDriverInstaller(const std::filesystem::path &directory) {
    DriverInstallerSelection selection;
    for (const auto name : {DriverInstallerName, L"InterceptorSetup.exe"}) {
        const auto candidate = directory / name;
        std::error_code error;
        if (!std::filesystem::exists(candidate, error))
            continue;
        if (IsGamepadDriverInstaller(candidate)) {
            selection.Path = candidate;
            return selection;
        }
        selection.RejectedOtherInstaller = true;
    }
    return selection;
}

void OpenDriverSetup(HWND owner, const std::filesystem::path &directory) {
    const auto selection = FindDriverInstaller(directory);
    const auto target = selection.Path.empty() ? DriverReleasesUrl : selection.Path.c_str();
    if (selection.Path.empty() && selection.RejectedOtherInstaller)
        MessageBoxW(owner,
                    L"The setup file beside this app is not identified as Interceptor Gamepad & Controller. "
                    L"It will not be started. The correct driver's download page will open instead.",
                    L"Interceptor Gamepad & Controller setup", MB_OK | MB_ICONINFORMATION);
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(owner, L"open", target, nullptr, directory.c_str(), SW_SHOWNORMAL));
    if (result <= 32) {
        const std::wstring message =
            L"Windows could not open Interceptor Gamepad & Controller setup or its download page.\n\n"
            L"Download the gamepad driver from:\n" + std::wstring(DriverReleasesUrl) +
            L"\n\nWindows shell error: " + std::to_wstring(result);
        MessageBoxW(owner, message.c_str(), L"Driver setup could not start", MB_OK | MB_ICONERROR);
    }
}
} // namespace gpr::ui
