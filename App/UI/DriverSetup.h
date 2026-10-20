#pragma once
#include <Windows.h>
#include <filesystem>

namespace gpr::ui {
inline constexpr wchar_t DriverInstallerName[] = L"InterceptorGamepadControllerSetup.exe";
inline constexpr wchar_t DriverReleasesUrl[] =
    L"https://github.com/TitanCSA/Interceptor-Gamepad-Controller/releases/latest";

struct DriverInstallerSelection {
    std::filesystem::path Path;
    bool RejectedOtherInstaller = false;
};

// Product identity, rather than the shared legacy filename, selects the driver edition.
bool IsGamepadDriverInstaller(const std::filesystem::path &path);
DriverInstallerSelection FindDriverInstaller(const std::filesystem::path &directory);
void OpenDriverSetup(HWND owner, const std::filesystem::path &directory);
} // namespace gpr::ui
