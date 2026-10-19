#pragma once
#include <Windows.h>
#include <string>
#include <vector>

namespace gpr {
int RunApplication(HINSTANCE instance, int show, const std::vector<std::wstring> &arguments);
int RunUiTests(const std::wstring &report, const std::wstring &renderDirectory);
} // namespace gpr
