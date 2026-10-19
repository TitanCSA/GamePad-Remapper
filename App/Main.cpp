#include "UI/MainWindow.h"
#include "UI/ScriptStorage.h"
#include "Capture/Service.h"
#include "Core/VisualDocument.h"
#include "ThirdParty/json.hpp"
#include <Windows.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
std::wstring Argument(const std::vector<std::wstring> &args, const wchar_t *name) {
    for (std::size_t i = 0; i < args.size(); ++i)
        if (args[i] == name) {
            if (i + 1 == args.size())
                throw std::invalid_argument("A command-line argument is missing its value.");
            return args[i + 1];
        }
    return {};
}
std::wstring Wide(const char *text) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (count <= 1)
        return L"Unknown application error";
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), count);
    result.pop_back();
    return result;
}
bool OfflineFailure(const std::vector<std::wstring> &args, const std::string &error) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--ui-self-test" || args[i] == L"--live-test") {
            if (i + 1 < args.size()) {
                try {
                    const std::filesystem::path path(args[i + 1]);
                    if (path.has_parent_path())
                        std::filesystem::create_directories(path.parent_path());
                    std::ofstream file(path, std::ios::binary | std::ios::trunc);
                    file << nlohmann::json({{"Passed", false}, {"Error", error}}).dump(2);
                } catch (...) {
                }
            }
            return true;
        }
        if (args[i] == L"--render-tests" || args[i] == L"--check-script")
            return true;
    }
    return false;
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int count = 0;
    auto raw = CommandLineToArgvW(GetCommandLineW(), &count);
    std::vector<std::wstring> args;
    if (raw) {
        for (int i = 1; i < count; ++i)
            args.emplace_back(raw[i]);
        LocalFree(raw);
    }
    try {
        const auto live = Argument(args, L"--live-test");
        if (!live.empty()) {
            int seconds = 30;
            for (std::size_t i = 0; i + 2 < args.size(); ++i)
                if (args[i] == L"--live-test") {
                    std::size_t used = 0;
                    seconds = std::stoi(args[i + 2], &used);
                    if (used != args[i + 2].size())
                        throw std::invalid_argument("Live-test duration must be an integer.");
                }
            return gpr::RunLiveTest(live, seconds);
        }
        const auto test = Argument(args, L"--ui-self-test"), render = Argument(args, L"--render-tests");
        if (!test.empty() || !render.empty())
            return gpr::RunUiTests(test, render);
        const auto check = Argument(args, L"--check-script");
        if (!check.empty()) {
            const auto source = gpr::ScriptStorage::Read(std::filesystem::path(check));
            gpr::ScriptCompiler::Compile(source);
            return 0;
        }
        return gpr::RunApplication(instance, show, args);
    } catch (const gpr::ScriptException &error) {
        if (OfflineFailure(args, error.Diagnostic()))
            return 1;
        MessageBoxW(nullptr, Wide(error.Diagnostic().c_str()).c_str(), L"GamePad Remapper",
                    MB_OK | MB_ICONERROR);
        return 1;
    } catch (const std::exception &error) {
        if (OfflineFailure(args, error.what()))
            return 1;
        MessageBoxW(nullptr, Wide(error.what()).c_str(), L"GamePad Remapper", MB_OK | MB_ICONERROR);
        return 1;
    }
}
