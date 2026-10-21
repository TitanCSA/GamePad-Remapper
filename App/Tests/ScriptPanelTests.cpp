#include "../UI/ScriptPanel.h"
#include "../UI/ScriptStorage.h"
#include <CommCtrl.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
int checks = 0;
void Check(bool value, const char *message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
std::wstring Text(HWND window) {
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(window)) + 1, L'\0');
    value.resize(static_cast<std::size_t>(GetWindowTextW(window, value.data(), static_cast<int>(value.size()))));
    return value;
}
HWND FindText(HWND parent, const std::wstring &text) {
    for (auto child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        if (Text(child) == text) return child;
    return nullptr;
}
RECT Bounds(HWND window, HWND parent) {
    RECT area{};
    GetWindowRect(window, &area);
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT *>(&area), 2);
    return area;
}
void CheckWrapping(HWND panel) {
    const auto dc = GetDC(panel);
    Check(dc != nullptr, "Wrapped script text must be measurable with its real native font.");
    for (auto child = GetWindow(panel, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t type[32]{};
        GetClassNameW(child, type, 32);
        if (_wcsicmp(type, L"STATIC") != 0) continue;
        RECT area{};
        GetClientRect(child, &area);
        const auto font = reinterpret_cast<HFONT>(SendMessageW(child, WM_GETFONT, 0, 0));
        const auto previous = font ? SelectObject(dc, font) : nullptr;
        RECT measured{0, 0, area.right, 0};
        auto caption = Text(child);
        DrawTextW(dc, caption.data(), static_cast<int>(caption.size()), &measured,
                  DT_CALCRECT | DT_NOPREFIX | DT_WORDBREAK | DT_EDITCONTROL);
        if (font) SelectObject(dc, previous);
        if (area.bottom < measured.bottom) {
            ReleaseDC(panel, dc);
            Check(false, "Script descriptions, labels, ranges and help must fit their measured wrapped height.");
        }
        Check(true, "Wrapped script text fits its actual native control bounds.");
    }
    ReleaseDC(panel, dc);
}
struct Window {
    HWND Value;
    ~Window() { if (IsWindow(Value)) DestroyWindow(Value); }
    operator HWND() const { return Value; }
};
struct Com {
    HRESULT Status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Com() { if (SUCCEEDED(Status)) CoUninitialize(); }
};
} // namespace

int main() {
    Com com;
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
        Check(InitCommonControlsEx(&controls) != FALSE, "Native script controls must initialize offline.");
        WNDCLASSW type{};
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"GprScriptPanelTests";
        Check(RegisterClassW(&type) != 0, "The offscreen script-panel test parent must register.");
        Window parent{CreateWindowExW(0, type.lpszClassName, L"Offline script panel tests",
                                      WS_OVERLAPPEDWINDOW, -32000, -32000, 900, 700, nullptr, nullptr,
                                      type.hInstance, nullptr)};
        Check(parent.Value != nullptr, "The script-panel test parent must be created.");
        const auto program = gpr::ScriptCompiler::Compile(R"igc(
ui {
    title("Long controller settings title that must wrap within the available script workspace");
    description("This description explains the complete controller behavior in several lines. It remains readable in a narrow workspace, after resizing and after changing display scale, without clipping its final words.");
    group("Controller behavior and compound conditions");
    slider zone("A long deadzone setting label that shares its row with a numeric percentage readout", 0, 99, 8, "%");
    help("Long help explains exactly which physical input is affected and how this parameter changes script behavior. Every sentence must remain visible, including these final words after the workspace becomes narrow.");
    toggle enabled("Enable this controller behavior only while the selected compound condition is active", true);
    select mode("Controller output mode with a long descriptive caption", 7) { 7: "Normal"; 42: "Invert"; };
    number delay_ms("Delay before repeating a controller sequence", 1, 2000, 80, 1, " ms");
}
main {}
)igc");
        int notifications = 0;
        std::size_t changedIndex = 0;
        int changedValue = 0;
        std::string error;
        gpr::ScriptPanel panel(type.hInstance, parent,
            [&](std::size_t index, int value) { ++notifications; changedIndex = index; changedValue = value; },
            [&](const std::string &message) { error = message; });
        panel.Set(program->Interface(), program->Interface().Defaults());
        ShowWindow(panel.Handle(), SW_SHOW);
        MoveWindow(panel.Handle(), 0, 0, 620, 280, FALSE);
        CheckWrapping(panel.Handle());
        const auto description = FindText(panel.Handle(), gpr::Utf16(program->Interface().Description));
        Check(description != nullptr, "The script description remains exposed as native text.");
        const auto wide = Bounds(description, panel.Handle());
        const auto slider = GetDlgItem(panel.Handle(), gpr::ScriptPanel::FirstInput);
        const auto toggle = GetDlgItem(panel.Handle(), gpr::ScriptPanel::FirstInput + 1);
        const auto combo = GetDlgItem(panel.Handle(), gpr::ScriptPanel::FirstInput + 2);
        const auto number = GetDlgItem(panel.Handle(), gpr::ScriptPanel::FirstInput + 3);
        Check(slider && toggle && combo && number, "All parameter kinds retain their native input IDs.");
        const auto toggleBounds = Bounds(toggle, panel.Handle());
        Check(toggleBounds.right - toggleBounds.left >= 48,
              "Compact toggles must fit the complete track and its keyboard focus ring.");
        SetWindowTextW(number, L"-");
        Check(notifications == 0, "Invalid numeric edits must not publish a script parameter.");
        MoveWindow(panel.Handle(), 0, 0, 250, 280, FALSE);
        CheckWrapping(panel.Handle());
        const auto narrow = Bounds(description, panel.Handle());
        Check(narrow.bottom - narrow.top > wide.bottom - wide.top,
              "Narrow workspaces must increase wrapped text height instead of cutting descriptions.");
        const auto label = FindText(panel.Handle(), gpr::Utf16(program->Interface().Parameters[0].Label));
        const auto readout = FindText(panel.Handle(), L"8%");
        const auto labelBounds = Bounds(label, panel.Handle()), readoutBounds = Bounds(readout, panel.Handle());
        Check(label && readout && labelBounds.right <= readoutBounds.left,
              "A wrapped slider label must reserve separate space for its percentage readout.");
        for (const UINT dpi : {144u, 192u, 96u}) {
            panel.SetDpi(dpi);
            MoveWindow(panel.Handle(), 0, 0, MulDiv(270, static_cast<int>(dpi), 96),
                       MulDiv(280, static_cast<int>(dpi), 96), FALSE);
            CheckWrapping(panel.Handle());
            Check(GetDlgItem(panel.Handle(), gpr::ScriptPanel::FirstInput) == slider &&
                      GetDlgItem(panel.Handle(), gpr::ScriptPanel::FirstInput + 3) == number && Text(number) == L"-",
                  "Resize and DPI changes must preserve input HWNDs and an unfinished numeric edit.");
        }
        SendMessageW(panel.Handle(), WM_VSCROLL, SB_TOP, 0);
        const auto topBounds = Bounds(description, panel.Handle());
        SCROLLINFO thumb{sizeof(thumb), SIF_POS};
        thumb.nPos = 120;
        SetScrollInfo(panel.Handle(), SB_VERT, &thumb, TRUE);
        SendMessageW(panel.Handle(), WM_VSCROLL, SB_THUMBPOSITION, 0);
        Check(Bounds(description, panel.Handle()).top < topBounds.top,
              "Custom scrollbar thumb completion must use the actual native scroll position.");
        SendMessageW(panel.Handle(), WM_VSCROLL, SB_TOP, 0);
        SendMessageW(number, WM_SETFOCUS, 0, 0);
        SCROLLINFO scroll{sizeof(scroll), SIF_POS};
        GetScrollInfo(panel.Handle(), SB_VERT, &scroll);
        Check(scroll.nPos > 0, "Native focus reveal must scroll the existing input into the viewport.");
        SendMessageW(slider, TBM_SETPOS, TRUE, 42);
        SendMessageW(panel.Handle(), WM_HSCROLL, TB_THUMBPOSITION, reinterpret_cast<LPARAM>(slider));
        Check(notifications == 1 && changedIndex == 0 && changedValue == 42,
              "Reflowed sliders must still publish the selected script value exactly once.");
        const auto toggleLabel = FindText(panel.Handle(), gpr::Utf16(program->Interface().Parameters[1].Label));
        Check(toggleLabel != nullptr, "Long toggle captions must remain available as wrapped native text.");
        SendMessageW(panel.Handle(), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(toggleLabel), STN_CLICKED),
                     reinterpret_cast<LPARAM>(toggleLabel));
        Check(notifications == 2 && changedIndex == 1 && changedValue == 0 &&
                  SendMessageW(toggle, BM_GETCHECK, 0, 0) == BST_UNCHECKED,
              "Clicking a wrapped toggle label must activate its native checkbox exactly once.");
        SendMessageW(combo, CB_SETCURSEL, 1, 0);
        SendMessageW(panel.Handle(), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo), CBN_SELCHANGE),
                     reinterpret_cast<LPARAM>(combo));
        Check(notifications == 3 && changedIndex == 2 && changedValue == 42,
              "Reflowed selections must retain declared script values rather than list indices.");
        const auto gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        const auto userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
        for (int repeat = 0; repeat < 120; ++repeat) {
            MoveWindow(panel.Handle(), 0, 0, 240 + repeat % 3 * 100, 280, FALSE);
            panel.SetDpi((repeat & 1) ? 96 : 144);
            Check(GetDlgItem(panel.Handle(), gpr::ScriptPanel::FirstInput) == slider && Text(number) == L"-",
                  "Repeated script-panel reflow must preserve the original input and edit state.");
        }
        Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdiBefore + 2,
              "Repeated script-panel reflow and DPI changes must not leak GDI resources.");
        Check(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= userBefore + 2,
              "Repeated script-panel reflow must not create additional native windows.");
        Check(notifications == 3 && error.empty(), "Layout changes must not publish parameters or report errors.");
        std::printf("PASS %d script-panel checks; no driver, capture, host input or files opened.\n", checks);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL script panel: %s\n", error.what());
        return 1;
    }
}
