#include "../UI/DriverSetup.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
unsigned checks = 0;
void Check(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
struct TemporaryDirectory {
    std::filesystem::path Path;
    TemporaryDirectory() {
        wchar_t temporary[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, temporary))
            throw std::runtime_error("Cannot locate the temporary directory.");
        Path = std::filesystem::path(temporary) /
               (L"GPR-Setup-Test-" + std::to_wstring(GetCurrentProcessId()) + L"-contrôleur");
        std::filesystem::create_directories(Path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(Path, error);
    }
};
}
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc != 3)
            throw std::runtime_error("Expected gamepad and classic VERSIONINFO fixture paths.");
        const std::filesystem::path gamepad = argv[1], classic = argv[2];
        using namespace gpr::ui;
        Check(IsGamepadDriverInstaller(gamepad), "The GPC product resource is accepted.");
        Check(!IsGamepadDriverInstaller(classic), "The classic product resource is rejected.");
        TemporaryDirectory temporary;
        const auto legacy = temporary.Path / L"InterceptorSetup.exe";
        const auto unique = temporary.Path / DriverInstallerName;
        auto selection = FindDriverInstaller(temporary.Path);
        Check(selection.Path.empty() && !selection.RejectedOtherInstaller,
              "A standalone app opens the correct driver release page.");
        std::filesystem::copy_file(classic, legacy);
        selection = FindDriverInstaller(temporary.Path);
        Check(selection.Path.empty() && selection.RejectedOtherInstaller,
              "A classic setup beside a standalone app is never selected.");
        std::filesystem::copy_file(gamepad, unique);
        selection = FindDriverInstaller(temporary.Path);
        Check(selection.Path == unique && !selection.RejectedOtherInstaller,
              "The unique GPC installer wins over a generic classic installer.");
        std::filesystem::copy_file(gamepad, legacy, std::filesystem::copy_options::overwrite_existing);
        Check(FindDriverInstaller(temporary.Path).Path == unique,
              "The unique filename also wins when both files are correct.");
        std::filesystem::copy_file(classic, unique, std::filesystem::copy_options::overwrite_existing);
        selection = FindDriverInstaller(temporary.Path);
        Check(selection.Path == legacy && selection.RejectedOtherInstaller,
              "An incorrectly renamed classic file cannot override a verified legacy GPC setup.");
        std::filesystem::remove(legacy);
        std::filesystem::remove(unique);
        std::ofstream(unique, std::ios::binary) << "not an executable";
        selection = FindDriverInstaller(temporary.Path);
        Check(selection.Path.empty() && selection.RejectedOtherInstaller,
              "Missing or malformed VERSIONINFO is rejected.");
        std::filesystem::remove(unique);
        std::filesystem::create_directory(unique);
        Check(!IsGamepadDriverInstaller(unique), "A directory cannot become a setup candidate.");
        Check(!IsGamepadDriverInstaller(temporary.Path / L"missing.exe"),
              "Missing files do not produce an installer.");
        std::cout << checks << " driver setup checks passed; no executable was launched.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
