#include "../UI/ScriptLibraryPopup.h"
#include <CommCtrl.h>
#include <dwmapi.h>
#include <algorithm>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace {
using namespace gpr::ui;
int checks = 0;
void Check(bool value, const char *message) {
    ++checks;
    if (!value)
        throw std::runtime_error(message);
}
struct Window {
    HWND Value = nullptr;
    ~Window() { if (IsWindow(Value)) DestroyWindow(Value); }
};
enum class Action { Search, Down, Up, Escape, NoMatches, Outside, Anchor, Close, Tabs, OutsideFocus, PaintTraffic, ScrollPosition, TopEscape, CancelCommand, Geometry, RightAlignment };
struct Automation {
    HWND Owner = nullptr, Anchor = nullptr;
    Action Kind = Action::Escape;
    bool Ran = false, TabsWorked = false;
    HWND OutsideFocus = nullptr;
    bool PaintSettled = false;
    bool ScrollHonoured = false;
    bool FallbackClosed = false;
    bool GeometryHonoured = false;
};
Automation *active = nullptr;
int outsideClicks = 0;
LRESULT CALLBACK OwnerProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_APP + 51 && active) {
        const auto popup = reinterpret_cast<HWND>(l);
        const auto list = FindWindowExW(popup, nullptr, L"GprScriptLibraryList", nullptr);
        active->PaintSettled = !GetUpdateRect(popup, nullptr, FALSE) && !GetUpdateRect(list, nullptr, FALSE);
        SendMessageW(popup, WM_CLOSE, 0, 0);
        return 0;
    }
    if (message == WM_APP + 52 && active) {
        active->FallbackClosed = true;
        SendMessageW(reinterpret_cast<HWND>(l), WM_CLOSE, 0, 0);
        return 0;
    }
    if (message == WM_LBUTTONDOWN)
        ++outsideClicks;
    return DefWindowProcW(window, message, w, l);
}
BOOL CALLBACK FindPopup(HWND window, LPARAM reference) {
    wchar_t type[64]{};
    GetClassNameW(window, type, 64);
    if (std::wstring(type) == L"GprScriptLibraryPopup" && active && GetWindow(window, GW_OWNER) == active->Owner) {
        *reinterpret_cast<HWND *>(reference) = window;
        return FALSE;
    }
    return TRUE;
}
void CALLBACK Automate(HWND owner, UINT, UINT_PTR timer, DWORD) {
    HWND popup = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), FindPopup, reinterpret_cast<LPARAM>(&popup));
    if (!popup)
        return;
    KillTimer(owner, timer);
    active->Ran = true;
    HWND search = FindWindowExW(popup, nullptr, L"EDIT", nullptr);
    HWND list = FindWindowExW(popup, nullptr, L"GprScriptLibraryList", nullptr);
    UpdateWindow(popup);
    UpdateWindow(list);
    switch (active->Kind) {
    case Action::RightAlignment: {
        RECT area{}, button{};
        GetWindowRect(popup, &area);
        GetWindowRect(active->Anchor, &button);
        active->GeometryHonoured = area.right == button.right && area.left < button.left;
        SendMessageW(popup, WM_CLOSE, 0, 0);
        break;
    }
    case Action::Search:
        SetWindowTextW(search, L"MY turbo");
        SendMessageW(search, WM_KEYDOWN, VK_RETURN, 0);
        break;
    case Action::Down:
        SendMessageW(search, WM_KEYDOWN, VK_DOWN, 0);
        SendMessageW(search, WM_KEYDOWN, VK_RETURN, 0);
        break;
    case Action::Up:
        SendMessageW(search, WM_KEYDOWN, VK_UP, 0);
        SendMessageW(search, WM_KEYDOWN, VK_RETURN, 0);
        break;
    case Action::NoMatches:
        SetWindowTextW(search, L"this-does-not-exist");
        SendMessageW(search, WM_KEYDOWN, VK_RETURN, 0);
        SendMessageW(search, WM_KEYDOWN, VK_ESCAPE, 0);
        break;
    case Action::Outside:
        PostMessageW(owner, WM_LBUTTONDOWN, 0, 0);
        break;
    case Action::OutsideFocus:
        PostMessageW(active->OutsideFocus, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, 2));
        break;
    case Action::PaintTraffic:
        SetWindowTextW(search, L"MY turbo");
        PostMessageW(owner, WM_APP + 51, 0, reinterpret_cast<LPARAM>(popup));
        break;
    case Action::ScrollPosition: {
        SCROLLINFO range{sizeof(range), SIF_RANGE | SIF_PAGE};
        GetScrollInfo(list, SB_VERT, &range);
        const int position = std::max(0, range.nMax - static_cast<int>(range.nPage));
        SCROLLINFO setting{sizeof(setting), SIF_POS};
        setting.nPos = position;
        SetScrollInfo(list, SB_VERT, &setting, FALSE);
        SendMessageW(list, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, static_cast<WORD>(std::min(position, 65535))), 0);
        GetScrollInfo(list, SB_VERT, &setting);
        active->ScrollHonoured = position > 65535 && setting.nPos == position;
        SendMessageW(popup, WM_CLOSE, 0, 0);
        break;
    }
    case Action::TopEscape:
        PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0);
        PostMessageW(owner, WM_APP + 52, 0, reinterpret_cast<LPARAM>(popup));
        break;
    case Action::CancelCommand:
        PostMessageW(popup, WM_COMMAND, IDCANCEL, 0);
        PostMessageW(owner, WM_APP + 52, 0, reinterpret_cast<LPARAM>(popup));
        break;
    case Action::Geometry: {
        const DWORD rounded = 2;
        const bool fallback = FAILED(DwmSetWindowAttribute(popup, 33, &rounded, sizeof(rounded)));
        RECT area{};
        GetWindowRect(popup, &area);
        SetWindowPos(popup, nullptr, area.left, area.top, area.right - area.left + 24,
                     area.bottom - area.top + 36, SWP_NOACTIVATE | SWP_NOZORDER);
        GetWindowRect(popup, &area);
        const auto region = CreateRectRgn(0, 0, 0, 0);
        const bool resized = !fallback || (GetWindowRgn(popup, region) != ERROR &&
            !PtInRegion(region, 0, 0) && PtInRegion(region, (area.right - area.left) / 2,
                                                 (area.bottom - area.top) / 2));
        SendMessageW(popup, WM_DPICHANGED, MAKEWPARAM(144, 144), reinterpret_cast<LPARAM>(&area));
        RECT bounds{};
        const bool dpi = !fallback || (GetWindowRgn(popup, region) != ERROR &&
            GetRgnBox(region, &bounds) != ERROR && bounds.right >= area.right - area.left &&
            bounds.bottom >= area.bottom - area.top && !PtInRegion(region, 0, 0));
        DeleteObject(region);
        active->GeometryHonoured = resized && dpi;
        SendMessageW(popup, WM_CLOSE, 0, 0);
        break;
    }
    case Action::Anchor:
        PostMessageW(active->Anchor, WM_LBUTTONDOWN, 0, 0);
        break;
    case Action::Close:
        SendMessageW(popup, WM_CLOSE, 0, 0);
        break;
    case Action::Tabs: {
        SendMessageW(search, WM_KEYDOWN, VK_TAB, 0);
        const bool intoList = GetFocus() == list;
        SendMessageW(list, WM_KEYDOWN, VK_TAB, 0);
        active->TabsWorked = intoList && GetFocus() == search;
        SendMessageW(search, WM_KEYDOWN, VK_ESCAPE, 0);
        break;
    }
    case Action::Escape:
        SendMessageW(search, WM_KEYDOWN, VK_ESCAPE, 0);
        break;
    }
}
std::optional<std::size_t> Show(HWND owner, HWND anchor,
                              const std::vector<ScriptLibraryEntry> &entries,
                              const std::filesystem::path &current, Action kind, bool *tabs = nullptr,
                              HWND outsideFocus = nullptr, bool *paintSettled = nullptr,
                              bool *scrollHonoured = nullptr, bool *fallbackClosed = nullptr,
                              bool *geometryHonoured = nullptr) {
    Automation automation{owner, anchor, kind};
    automation.OutsideFocus = outsideFocus;
    active = &automation;
    if (!SetTimer(owner, 201, 10, Automate))
        throw std::runtime_error("Could not schedule local popup messages.");
    const auto result = ShowScriptLibraryPopup(owner, anchor, entries, current, L"Script library", false);
    KillTimer(owner, 201);
    MSG pending{};
    while (PeekMessageW(&pending, owner, WM_APP + 52, WM_APP + 52, PM_REMOVE)) {}
    active = nullptr;
    Check(automation.Ran, "Popup automation did not run.");
    if (tabs)
        *tabs = automation.TabsWorked;
    if (paintSettled)
        *paintSettled = automation.PaintSettled;
    if (scrollHonoured)
        *scrollHonoured = automation.ScrollHonoured;
    if (fallbackClosed)
        *fallbackClosed = automation.FallbackClosed;
    if (geometryHonoured)
        *geometryHonoured = automation.GeometryHonoured;
    return result;
}
} // namespace

int wmain() {
    try {
        const std::vector<ScriptLibraryEntry> entries = {
            {L"D:\\Scripts\\turbo.igc", L"Turbo", L"My scripts"},
            {L"D:\\Scripts\\aim.igc", L"Aim assist", L"Examples"},
            {L"D:\\Scripts\\DEADZONE.igc", L"Deadzone", L"Examples"},
            {L"D:\\Scripts\\another.igc", L"Turbo", L"My scripts"},
            {L"D:\\Scripts\\caf\u00e9.igc", L"Caf\u00e9", L"Examples"}};
        const auto all = detail::FilterScriptLibraryEntries(entries, L"");
        Check(all == std::vector<std::size_t>({3, 0, 1, 4, 2}), "Custom groups must precede examples with stable name/path ordering.");
        Check(detail::FilterScriptLibraryEntries(entries, L"MY turbo") == std::vector<std::size_t>({3, 0}),
              "Multi-term group/name search failed.");
        Check(detail::FilterScriptLibraryEntries(entries, L"\t exampleS   dead\n") == std::vector<std::size_t>({2}),
              "Whitespace and case handling failed.");
        Check(detail::FilterScriptLibraryEntries(entries, L"deadzone.IGC") == std::vector<std::size_t>({2}),
              "File-name matching failed.");
        Check(detail::FilterScriptLibraryEntries(entries, L"CAF\u00c9") == std::vector<std::size_t>({4}),
              "Unicode matching failed.");
        Check(detail::FilterScriptLibraryEntries(entries, L"infinite").empty(), "Unmatched filter was not empty.");
        Check(detail::FilterScriptLibraryEntries({}, L"").empty(), "Empty library did not stay empty.");
        Check(entries[0].Path == L"D:\\Scripts\\turbo.igc", "Filtering changed the caller's entries.");
        Check(!ShowScriptLibraryPopup(nullptr, nullptr, entries, {}), "Invalid owners did not fail closed.");

        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"GprScriptLibraryOfflineTests";
        type.lpfnWndProc = OwnerProcedure;
        RegisterClassW(&type);
        Window owner{CreateWindowW(type.lpszClassName, L"Local popup test", WS_OVERLAPPED,
                                   40, 40, 640, 480, nullptr, nullptr, type.hInstance, nullptr)};
        Check(owner.Value != nullptr, "Could not create a local test owner.");
        Window anchor{CreateWindowW(L"BUTTON", L"Scripts", WS_CHILD | WS_VISIBLE,
                                    16, 16, 160, 40, owner.Value, nullptr, type.hInstance, nullptr)};
        Check(anchor.Value != nullptr, "Could not create a local test anchor.");
        Window outsideEdit{CreateWindowW(L"EDIT", L"Outside", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                         16, 80, 160, 32, owner.Value, nullptr, type.hInstance, nullptr)};
        Check(outsideEdit.Value != nullptr, "Could not create the outside focus target.");
        Check(Show(owner.Value, anchor.Value, entries, {}, Action::Search) == 3, "Search did not return the original entry index.");
        Check(Show(owner.Value, anchor.Value, entries, entries[1].Path, Action::Down) == 4, "Down navigation crossed groups incorrectly.");
        Check(Show(owner.Value, anchor.Value, entries, entries[4].Path, Action::Up) == 1, "Up navigation crossed groups incorrectly.");
        Check(!Show(owner.Value, anchor.Value, entries, {}, Action::Escape), "Escape selected a script.");
        for (auto action : {Action::TopEscape, Action::CancelCommand}) {
            bool fallbackClosed = false;
            Check(!Show(owner.Value, anchor.Value, entries, {}, action, nullptr, nullptr, nullptr, nullptr, &fallbackClosed),
                  "Cancel command selected a script.");
            Check(!fallbackClosed, "Top-level Escape or IDCANCEL did not close the popup.");
        }
        Check(!Show(owner.Value, anchor.Value, entries, {}, Action::NoMatches), "No-match Enter selected a script.");
        const int before = outsideClicks;
        Check(!Show(owner.Value, anchor.Value, entries, {}, Action::Outside), "Outside click selected a script.");
        Check(outsideClicks == before + 1, "Outside click was swallowed instead of reaching its target.");
        SetFocus(anchor.Value);
        Check(!Show(owner.Value, anchor.Value, entries, {}, Action::OutsideFocus, nullptr, outsideEdit.Value),
              "Outside focus click selected a script.");
        const bool keptOutsideFocus = GetFocus() == outsideEdit.Value;
        SendMessageW(outsideEdit.Value, WM_LBUTTONUP, 0, MAKELPARAM(2, 2));
        Check(keptOutsideFocus, "Dismissal undid the outside control's newly chosen keyboard focus.");
        Check(!Show(owner.Value, anchor.Value, entries, {}, Action::Anchor), "Anchor click did not close the library.");
        Check(!Show(owner.Value, anchor.Value, entries, {}, Action::Close), "WM_CLOSE selected a script.");
        bool tabs = false;
        Check(!Show(owner.Value, anchor.Value, entries, {}, Action::Tabs, &tabs), "Tab navigation selected a script.");
        Check(tabs, "Search/list focus navigation failed.");
        bool paintSettled = false;
        Check(!Show(owner.Value, anchor.Value, entries, {}, Action::PaintTraffic, nullptr, nullptr, &paintSettled),
              "Queued owner traffic selected a script.");
        Check(paintSettled, "Search repaint was starved by posted owner messages.");
        Check(!Show(owner.Value, anchor.Value, {}, {}, Action::NoMatches), "Empty library selected a script.");
        const std::vector<ScriptLibraryEntry> actions = {
            {{}, L"Save", L"Files"}, {{}, L"New", L"Files"}, {{}, L"Reduce motion", L"View"}};
        Check(detail::FilterScriptLibraryEntries(actions, L"") == std::vector<std::size_t>({1, 0, 2}),
              "Examples priority changed ordinary quick-action group ordering.");
        Check(Show(owner.Value, anchor.Value, actions, {}, Action::Down) == 0,
              "Empty-path quick actions did not preserve original indices.");
        std::vector<ScriptLibraryEntry> large;
        for (int index = 0; index < 2500; ++index)
            large.push_back({std::filesystem::path(L"D:\\Examples") / (std::to_wstring(index) + L".igc"),
                             L"Script " + std::to_wstring(index), L"Examples"});
        bool scrollHonoured = false;
        Check(!Show(owner.Value, anchor.Value, large, {}, Action::ScrollPosition, nullptr, nullptr, nullptr, &scrollHonoured),
              "Scrolling selected a script.");
        Check(scrollHonoured, "Native scrollbar thumb position was truncated or reset in a large library.");
        bool geometryHonoured = false;
        Check(!Show(owner.Value, anchor.Value, entries, {}, Action::Geometry, nullptr, nullptr, nullptr,
                    nullptr, nullptr, &geometryHonoured), "Resizing or DPI change selected a script.");
        Check(geometryHonoured, "Rounded popup fallback was lost after resizing or changing DPI.");
        MoveWindow(anchor.Value, 440, 16, 160, 40, FALSE);
        bool rightAligned = false;
        Check(!Show(owner.Value, anchor.Value, entries, {}, Action::RightAlignment, nullptr, nullptr, nullptr,
                    nullptr, nullptr, &rightAligned), "Right-edge menu placement selected a script.");
        Check(rightAligned, "A right-edge Scripts button must open its menu toward the left, aligned to the button.");
        MoveWindow(anchor.Value, 16, 16, 160, 40, FALSE);

        // Repeated creation/destruction checks native handles, not driver state.
        const auto baseline = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int pass = 0; pass < 8; ++pass)
            Check(!Show(owner.Value, anchor.Value, entries, {}, Action::Escape), "Repeated dismissal changed the result.");
        const auto remaining = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        Check(remaining <= baseline + 2, "Repeated popups leaked GDI resources.");
        std::wcout << L"Script library: " << checks << L" checks passed. No files or driver were opened.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
