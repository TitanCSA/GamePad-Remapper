#include "../UI/NativeWidgets.h"
#include "../ThirdParty/json.hpp"
#include <Windows.h>
#include <CommCtrl.h>
#include <OleAcc.h>
#include <Richedit.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(                                                                                             \
    linker,                                                                                                  \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
using namespace gpr::ui;
int checks = 0;
int notifications = 0;
HWND destroyOnNotification = nullptr;
void Check(bool condition, const char *message) {
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
LRESULT CALLBACK ParentProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_DRAWITEM && DrawItem(*reinterpret_cast<DRAWITEMSTRUCT *>(l)))
        return TRUE;
    if (message == WM_MEASUREITEM) {
        auto &item = *reinterpret_cast<MEASUREITEMSTRUCT *>(l);
        item.itemHeight = 26;
        return TRUE;
    }
    if (message == WM_COMMAND) {
        if (HIWORD(w) == BN_CLICKED)
            ++notifications;
        if (reinterpret_cast<HWND>(l) == destroyOnNotification && destroyOnNotification) {
            const auto child = destroyOnNotification;
            destroyOnNotification = nullptr;
            DestroyWindow(child);
        }
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
void Pump() {
    MSG message{};
    int count = 0;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (++count > 10000)
            throw std::runtime_error("Native widget message queue did not settle.");
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
void PumpFor(DWORD milliseconds) {
    const auto end = GetTickCount64() + milliseconds;
    do {
        Pump();
        const auto now = GetTickCount64();
        if (now >= end)
            break;
        MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(end - now), QS_ALLINPUT,
                                    MWMO_INPUTAVAILABLE);
    } while (true);
    Pump();
}
struct AnimationTrace {
    int Timers = 0;
    bool DestroyOnTimer = false;
    bool Destroyed = false;
};
constexpr UINT_PTR AnimationTraceId = 0x47505454;
LRESULT CALLBACK TraceAnimation(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR,
                                DWORD_PTR reference) {
    auto &trace = *reinterpret_cast<AnimationTrace *>(reference);
    if (message == WM_TIMER) {
        ++trace.Timers;
        if (trace.DestroyOnTimer) {
            DestroyWindow(window);
            return 0;
        }
    }
    if (message == WM_NCDESTROY) {
        trace.Destroyed = true;
        RemoveWindowSubclass(window, TraceAnimation, AnimationTraceId);
    }
    return DefSubclassProc(window, message, w, l);
}
void ObserveAnimation(HWND window, AnimationTrace &trace) {
    Check(SetWindowSubclass(window, TraceAnimation, AnimationTraceId,
                            reinterpret_cast<DWORD_PTR>(&trace)) != FALSE,
          "Offline widget animations must remain observable through an ordinary native subclass.");
}
void Hover(HWND window) {
    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(20, 12));
    // Synthetic messages must not leave a real cursor tracker on the host.
    TRACKMOUSEEVENT cancel{sizeof(cancel), TME_CANCEL | TME_LEAVE, window, 0};
    TrackMouseEvent(&cancel);
    MSG queued{};
    while (PeekMessageW(&queued, window, WM_MOUSELEAVE, WM_MOUSELEAVE, PM_REMOVE)) {
    }
}
class Window final {
    HWND value_ = nullptr;

  public:
    explicit Window(HWND value) : value_(value) {
        if (!value_)
            throw std::runtime_error("A native widget test window could not be created.");
    }
    ~Window() {
        if (IsWindow(value_))
            DestroyWindow(value_);
    }
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;
    operator HWND() const { return value_; }
};
struct Com final {
    HRESULT Status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Com() {
        if (SUCCEEDED(Status))
            CoUninitialize();
    }
};
struct RichEditLibrary final {
    HMODULE Handle = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ~RichEditLibrary() {
        if (Handle)
            FreeLibrary(Handle);
    }
};
HWND Child(HWND parent, const wchar_t *kind, const wchar_t *text, DWORD style, int id, int y) {
    return CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 8, y, 250, 36, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr),
                           nullptr);
}
void CursorContracts(HWND parent) {
    const auto previous = GetCursor();
    struct Restore {
        HCURSOR Cursor;
        ~Restore() { SetCursor(Cursor); }
    } restore{previous};
    const auto arrow = LoadCursorW(nullptr, IDC_ARROW);
    const auto hand = LoadCursorW(nullptr, IDC_HAND);
    const auto cursor = [arrow](HWND window) {
        SetCursor(arrow);
        SendMessageW(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window),
                     MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        return GetCursor();
    };
    Window button(Child(parent, L"BUTTON", L"Click", BS_PUSHBUTTON, 801, 0));
    StyleButton(button);
    Check(cursor(button) == hand, "Enabled buttons show a hand cursor.");
    EnableWindow(button, FALSE);
    Check(cursor(button) != hand, "Disabled buttons do not advertise an available action.");
    Window toggle(Child(parent, L"BUTTON", L"Toggle", BS_AUTOCHECKBOX, 802, 40));
    StyleToggle(toggle);
    Check(cursor(toggle) == hand, "Toggles show a hand cursor.");
    Window combo(Child(parent, L"COMBOBOX", L"", CBS_DROPDOWNLIST, 803, 80));
    StyleComboBox(combo);
    Check(cursor(combo) == hand, "Dropdown selectors show a hand cursor.");
    COMBOBOXINFO info{sizeof(info)};
    Check(GetComboBoxInfo(combo, &info) && info.hwndList && cursor(info.hwndList) == hand,
          "Native dropdown popup items also show a hand cursor.");
    Window edit(Child(parent, L"EDIT", L"Text", ES_AUTOHSCROLL, 804, 120));
    StyleEdit(edit);
    Check(cursor(edit) != hand, "Editable text preserves its native text cursor.");
    Window slider(Child(parent, TRACKBAR_CLASSW, L"", TBS_HORZ, 805, 160));
    SetDpi(slider, 96);
    Check(cursor(slider) == hand, "Script sliders show a hand cursor.");
    Window label(Child(parent, L"STATIC", L"Toggle label", SS_NOTIFY, 806, 200));
    SetDpi(label, 96);
    Check(cursor(label) == hand, "Clickable toggle labels show a hand cursor.");
    Window plain(Child(parent, L"STATIC", L"Description", 0, 807, 240));
    SetDpi(plain, 96);
    Check(cursor(plain) != hand, "Ordinary descriptions do not appear clickable.");
    Window list(Child(parent, L"LISTBOX", L"", LBS_NOTIFY, 808, 280));
    SetDpi(list, 96);
    Check(cursor(list) == hand, "Selectable list items show a hand cursor.");
    Window tree(Child(parent, WC_TREEVIEWW, L"", TVS_HASBUTTONS, 809, 320));
    SetDpi(tree, 96);
    Check(cursor(tree) == hand, "Visual condition trees show a hand cursor.");
}
std::wstring Caption(HWND window) {
    const int size = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(size) + 1, L'\0');
    const int copied = GetWindowTextW(window, text.data(), size + 1);
    text.resize(static_cast<std::size_t>(std::max(0, copied)));
    return text;
}
bool Palette(COLORREF value) { return value == Blue || value == Black || value == White || value == Gray; }
class Canvas final {
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;

  public:
    explicit Canvas(HWND window, bool nonclient = false) {
        RECT client{};
        if (nonclient) {
            GetWindowRect(window, &client);
            client.right -= client.left;
            client.bottom -= client.top;
            client.left = client.top = 0;
        } else
            GetClientRect(window, &client);
        const auto screen = GetDC(window);
        dc_ = CreateCompatibleDC(screen);
        bitmap_ = CreateCompatibleBitmap(screen, std::max(1L, client.right), std::max(1L, client.bottom));
        ReleaseDC(window, screen);
        if (!dc_ || !bitmap_) {
            if (bitmap_)
                DeleteObject(bitmap_);
            if (dc_)
                DeleteDC(dc_);
            throw std::runtime_error("A native widget test canvas could not be allocated.");
        }
        previous_ = SelectObject(dc_, bitmap_);
        const auto guard = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(dc_, &client, guard);
        DeleteObject(guard);
        SendMessageW(window, nonclient ? WM_PRINT : WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc_),
                     nonclient ? PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND : PRF_CLIENT);
    }
    ~Canvas() {
        SelectObject(dc_, previous_);
        DeleteObject(bitmap_);
        DeleteDC(dc_);
    }
    Canvas(const Canvas &) = delete;
    Canvas &operator=(const Canvas &) = delete;
    COLORREF Pixel(int x, int y) const { return GetPixel(dc_, x, y); }
};
double Luminance(COLORREF value) {
    const auto channel = [](BYTE v) {
        const double c = static_cast<double>(v) / 255.0;
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(GetRValue(value)) + 0.7152 * channel(GetGValue(value)) +
           0.0722 * channel(GetBValue(value));
}
double Contrast(COLORREF a, COLORREF b) {
    return (std::max(Luminance(a), Luminance(b)) + 0.05) / (std::min(Luminance(a), Luminance(b)) + 0.05);
}
void AccessibleName(HWND window, const wchar_t *name) {
    SetAccessibleName(window, name);
    IAccessible *accessible = nullptr;
    const auto status = AccessibleObjectFromWindow(window, static_cast<DWORD>(OBJID_CLIENT), IID_IAccessible,
                                                   reinterpret_cast<void **>(&accessible));
    Check(SUCCEEDED(status) && accessible, "Styled native controls must expose their accessibility object.");
    VARIANT child{};
    child.vt = VT_I4;
    child.lVal = CHILDID_SELF;
    BSTR actual = nullptr;
    const auto named = accessible->get_accName(child, &actual);
    const bool correct = SUCCEEDED(named) && actual && std::wstring(actual) == name;
    SysFreeString(actual);
    accessible->Release();
    Check(correct, "Native accessibility must expose the explicit field label without changing its value.");
}
void ReentrantDestruction(HWND parent) {
    for (const auto style : {BS_PUSHBUTTON, BS_AUTOCHECKBOX}) {
        Window button(Child(parent, L"BUTTON", L"Rebuild visual form", style, 250, 8));
        if (style == BS_AUTOCHECKBOX)
            StyleCheckBox(button);
        else
            StyleButton(button);
        destroyOnNotification = button;
        SendMessageW(button, BM_CLICK, 0, 0);
        Check(!IsWindow(button) && !destroyOnNotification,
              "A styled button can be destroyed synchronously by its native owner notification.");
    }
    Window edit(Child(parent, L"EDIT", L"Original field", ES_AUTOHSCROLL, 251, 8));
    StyleEdit(edit);
    destroyOnNotification = edit;
    SetWindowTextW(edit, L"Rebuild current form");
    Check(!IsWindow(edit) && !destroyOnNotification,
          "A styled edit can be destroyed while its native text-change notification is running.");
}
void AnimationContracts(HWND parent) {
    // The window stays outside every monitor and never takes focus or injects
    // input. It is visible only so Windows' real timer/paint lifecycle is tested.
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    AnimationTrace trace;
    Window button(Child(parent, L"BUTTON", L"", BS_PUSHBUTTON, 280, 8));
    StyleButton(button);
    ObserveAnimation(button, trace);
    BOOL systemAnimation = FALSE;
    HIGHCONTRASTW contrast{sizeof(contrast)};
    const bool motion = SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &systemAnimation, 0) &&
                        systemAnimation &&
                        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                        (contrast.dwFlags & HCF_HIGHCONTRASTON) == 0;
    const auto hoverStarted = GetTickCount64();
    Hover(button);
    PumpFor(65);
    Canvas moving(button);
    if (motion) {
        Check(trace.Timers > 0, "Visible hover transitions must progress using real window timers.");
        Check(GetTickCount64() - hoverStarted >= 90 ||
                  (moving.Pixel(12, 18) == Blue && moving.Pixel(244, 18) == Black),
              "A hover transition must reveal its blue surface gradually without extra palette colours.");
    } else
        Check(trace.Timers == 0 && moving.Pixel(125, 18) == Blue,
              "Windows reduced-motion or high-contrast preferences must settle without animation timers.");
    PumpFor(150);
    Canvas settled(button);
    Check(settled.Pixel(125, 18) == Blue,
          "A completed hover animation must reach the same readable native button state.");
    const int settledTimers = trace.Timers;
    PumpFor(70);
    Check(trace.Timers == settledTimers, "Settled widgets must leave no idle animation timer running.");
    SendMessageW(button, WM_MOUSELEAVE, 0, 0);
    SetAnimationsEnabled(button, false);
    PumpFor(40);
    const int reducedTimers = trace.Timers;
    Hover(button);
    Canvas reduced(button);
    PumpFor(70);
    Check(reduced.Pixel(125, 18) == Blue && trace.Timers == reducedTimers,
          "The application reduce-motion switch must settle transitions and prevent new timers.");
    SetAnimationsEnabled(button, true);
    SendMessageW(button, WM_MOUSELEAVE, 0, 0);
    SendMessageW(button, WM_CANCELMODE, 0, 0);
    PumpFor(35);
    const int cancelledTimers = trace.Timers;
    PumpFor(70);
    Check(trace.Timers == cancelledTimers, "Cancelling a native interaction must stop its transition timer.");
    Hover(button);
    ShowWindow(button, SW_HIDE);
    PumpFor(35);
    const int hiddenTimers = trace.Timers;
    PumpFor(70);
    Check(trace.Timers == hiddenTimers, "Hidden controls must stop animation timers immediately.");
    ShowWindow(button, SW_SHOWNOACTIVATE);
    SendMessageW(button, WM_MOUSELEAVE, 0, 0);
    EnableWindow(button, FALSE);
    PumpFor(35);
    const int disabledTimers = trace.Timers;
    PumpFor(70);
    Check(trace.Timers == disabledTimers, "Disabling a native control must cancel pending animation work.");
    AnimationTrace toggleTrace;
    Window toggle(Child(parent, L"BUTTON", L"", BS_AUTOCHECKBOX, 281, 52));
    StyleToggle(toggle);
    ObserveAnimation(toggle, toggleTrace);
    SendMessageW(toggle, BM_SETCHECK, BST_CHECKED, 0);
    PumpFor(45);
    SendMessageW(toggle, BM_SETCHECK, BST_UNCHECKED, 0);
    PumpFor(170);
    Canvas toggled(toggle);
    Check(toggled.Pixel(14, 18) == White && toggled.Pixel(34, 18) != White &&
              SendMessageW(toggle, BM_GETCHECK, 0, 0) == BST_UNCHECKED,
          "A reversed toggle animation must settle at its current native check state without drift.");
    const int toggleTimers = toggleTrace.Timers;
    PumpFor(70);
    Check(toggleTimers == toggleTrace.Timers, "A reversed toggle must also leave no idle timer.");
    Window compact(Child(parent, L"BUTTON", L"", BS_AUTOCHECKBOX, 284, 184));
    StyleToggle(compact);
    SetAnimationsEnabled(compact, false);
    SetWindowPos(compact, nullptr, 8, 184, 48, 32, SWP_NOACTIVATE | SWP_NOZORDER);
    SendMessageW(compact, BM_SETCHECK, BST_CHECKED, 0);
    const auto previousFocus = SetFocus(compact);
    SendMessageW(compact, WM_UPDATEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
    Canvas focusedToggle(compact);
    Check(GetFocus() == compact && focusedToggle.Pixel(0, 10) == White,
          "A captionless compact toggle must show a distinct keyboard focus ring around its full track.");
    SendMessageW(compact, WM_UPDATEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    Canvas hiddenFocusToggle(compact);
    Check(hiddenFocusToggle.Pixel(0, 10) == Black,
          "The compact toggle focus ring must obey Windows keyboard-focus visibility hints.");
    SetFocus(previousFocus);
    AnimationTrace destroyed;
    Window doomed(Child(parent, L"BUTTON", L"", BS_PUSHBUTTON, 282, 96));
    StyleButton(doomed);
    ObserveAnimation(doomed, destroyed);
    destroyed.DestroyOnTimer = motion;
    Hover(doomed);
    if (!motion)
        DestroyWindow(doomed);
    PumpFor(170);
    Check(destroyed.Destroyed && !IsWindow(doomed),
          "Destroying an animating native control from its timer callback must release its window safely.");
    const auto gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const auto userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (int i = 0; i < 32; ++i) {
        AnimationTrace transient;
        Window control(Child(parent, L"BUTTON", L"", BS_AUTOCHECKBOX, 283, 140));
        StyleToggle(control);
        ObserveAnimation(control, transient);
        SendMessageW(control, BM_SETCHECK, BST_CHECKED, 0);
        Canvas first(control);
        SetWindowPos(control, nullptr, 8, 140, 310, 44, SWP_NOACTIVATE | SWP_NOZORDER);
        Canvas resized(control);
        SendMessageW(control, BM_SETCHECK, BST_UNCHECKED, 0);
        DestroyWindow(control);
        Check(transient.Destroyed,
              "An interrupted resized toggle must destroy its animation and cached painting surface.");
    }
    PumpFor(160);
    Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdiBefore + 2,
          "Interrupted transitions, resizing and destruction must keep GDI resources bounded.");
    Check(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS) <= userBefore + 2,
          "Interrupted transitions must release timers and native USER resources after destruction.");
    ShowWindow(parent, SW_HIDE);
    Pump();
}
void DragThumb(HWND window, int which = SB_VERT) {
    SCROLLBARINFO bar{sizeof(bar)};
    Check(GetScrollBarInfo(window, which == SB_VERT ? OBJID_VSCROLL : OBJID_HSCROLL, &bar) != FALSE,
          "A scrollable native control must expose its real scrollbar geometry.");
    Check((bar.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) == 0 &&
              bar.xyThumbBottom > bar.xyThumbTop,
          "The local scrollbar must be available before a bounded synthetic drag.");
    const bool vertical = which == SB_VERT;
    const POINT initial{vertical ? (bar.rcScrollBar.left + bar.rcScrollBar.right) / 2
                                 : bar.rcScrollBar.left + (bar.xyThumbTop + bar.xyThumbBottom) / 2,
                        vertical ? bar.rcScrollBar.top + (bar.xyThumbTop + bar.xyThumbBottom) / 2
                                 : (bar.rcScrollBar.top + bar.rcScrollBar.bottom) / 2};
    POINT target{vertical ? initial.x : bar.rcScrollBar.right - bar.dxyLineButton - 2,
                 vertical ? bar.rcScrollBar.bottom - bar.dxyLineButton - 2 : initial.y};
    ScreenToClient(window, &target);
    SendMessageW(window, WM_NCLBUTTONDOWN, vertical ? HTVSCROLL : HTHSCROLL,
                 MAKELPARAM(initial.x, initial.y));
    SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(target.x, target.y));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(target.x, target.y));
    Check(GetCapture() != window, "A completed local scrollbar drag must release native mouse capture.");
}
void PrintedScrollbar(HWND window) {
    SCROLLBARINFO bar{sizeof(bar)};
    RECT outer{};
    Check(GetScrollBarInfo(window, OBJID_VSCROLL, &bar) != FALSE && GetWindowRect(window, &outer),
          "Native scrollbar geometry must be available for a real nonclient render.");
    Canvas canvas(window, true);
    const int x = (bar.rcScrollBar.left + bar.rcScrollBar.right) / 2 - outer.left;
    const int y = bar.rcScrollBar.top + (bar.xyThumbTop + bar.xyThumbBottom) / 2 - outer.top;
    Check(canvas.Pixel(x, y) == Gray || canvas.Pixel(x, y) == Blue,
          "WM_PRINT must paint the actual scrollbar thumb into the supplied image buffer.");
    Check(canvas.Pixel(bar.rcScrollBar.left - outer.left + 1, y) == Black,
          "WM_PRINT scrollbar tracks must retain the dark application palette.");
}
void ScrollContracts(HWND parent) {
    Window list(Child(parent, L"LISTBOX", L"", LBS_HASSTRINGS | LBS_NOTIFY | WS_VSCROLL, 260, 8));
    SetWindowPos(list, nullptr, 8, 8, 300, 180, SWP_NOACTIVATE | SWP_NOZORDER);
    for (int i = 0; i < 200; ++i) {
        const auto text = L"Controller script " + std::to_wstring(i);
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    StyleScrollbars(list);
    PrintedScrollbar(list);
    Check(SendMessageW(list, LB_GETTOPINDEX, 0, 0) == 0, "A populated native list starts at its first item.");
    DragThumb(list);
    Check(SendMessageW(list, LB_GETTOPINDEX, 0, 0) > 100,
          "Dragging the custom scrollbar must actually scroll native list contents.");
    SendMessageW(list, WM_VSCROLL, SB_TOP, 0);
    Check(SendMessageW(list, LB_GETTOPINDEX, 0, 0) == 0,
          "Native scrollbar top navigation must remain functional after a custom drag.");
    SendMessageW(list, WM_VSCROLL, SB_BOTTOM, 0);
    Check(SendMessageW(list, LB_GETTOPINDEX, 0, 0) > 100,
          "Native scrollbar bottom navigation must remain functional after a custom drag.");
    RichEditLibrary rich;
    Check(rich.Handle != nullptr, "The system RichEdit library must load for offline scroll tests.");
    Window code(Child(parent, MSFTEDIT_CLASS, L"",
                      ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL, 261, 204));
    SetWindowPos(code, nullptr, 8, 204, 300, 180, SWP_NOACTIVATE | SWP_NOZORDER);
    std::wstring lines(24000, L'X');
    lines += L"\r\n";
    for (int i = 0; i < 12000; ++i)
        lines += L"main { add_val(LX, 25); }\r\n";
    SendMessageW(code, EM_EXLIMITTEXT, 0, 524288);
    SetWindowTextW(code, lines.c_str());
    StyleScrollbars(code);
    // RichEdit formats large hidden documents lazily; visiting its end establishes
    // the complete scroll range before exercising the actual thumb mapping.
    SendMessageW(code, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(code, EM_SCROLLCARET, 0, 0);
    SendMessageW(code, WM_VSCROLL, SB_TOP, 0);
    SendMessageW(code, EM_SETSEL, 0, 0);
    PrintedScrollbar(code);
    POINT position{};
    SendMessageW(code, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&position));
    Check(position.y == 0, "A populated native code editor starts at the top.");
    SendMessageW(code, WM_HSCROLL, SB_RIGHT, 0);
    SCROLLINFO horizontal{sizeof(horizontal), SIF_POS};
    Check(GetScrollInfo(code, SB_HORZ, &horizontal) != FALSE && horizontal.nPos > 65535,
          "The large native code fixture must exercise horizontal positions beyond 16 bits.");
    const int horizontalBefore = horizontal.nPos;
    DragThumb(code);
    SendMessageW(code, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&position));
    SCROLLINFO actual{sizeof(actual), SIF_ALL};
    Check(GetScrollInfo(code, SB_VERT, &actual) != FALSE,
          "RichEdit must expose its full native scroll position.");
    const auto firstVisible = SendMessageW(code, EM_GETFIRSTVISIBLELINE, 0, 0);
    Check(actual.nPos > 65535 && firstVisible > 5000,
          ("Custom RichEdit thumb dragging must retain full positions beyond the 16-bit scroll limit: " +
           std::to_string(actual.nPos) + " pixels, first line " + std::to_string(firstVisible) + ", range " +
           std::to_string(actual.nMax) + ", characters " + std::to_string(GetWindowTextLengthW(code)) +
           ", lines " + std::to_string(SendMessageW(code, EM_GETLINECOUNT, 0, 0)))
              .c_str());
    Check(GetScrollInfo(code, SB_HORZ, &horizontal) != FALSE && horizontal.nPos == horizontalBefore,
          "Vertical thumb dragging must preserve a horizontal position beyond the 16-bit limit.");
    const int verticalBefore = actual.nPos;
    DragThumb(code, SB_HORZ);
    Check(GetScrollInfo(code, SB_VERT, &actual) != FALSE && actual.nPos == verticalBefore,
          "Horizontal thumb dragging must preserve a vertical position beyond the 16-bit limit.");
    SendMessageW(code, WM_VSCROLL, SB_TOP, 0);
    SendMessageW(code, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&position));
    Check(position.y == 0, "Native code-editor top navigation must survive custom scrollbar styling.");
}
void PaintBlankButton(HWND button) {
    SetWindowTextW(button, L"");
    for (const auto kind :
         {ButtonKind::Secondary, ButtonKind::Primary, ButtonKind::Danger, ButtonKind::Tab}) {
        StyleButton(button, kind);
        for (const bool selected : {false, true}) {
            SetButtonSelected(button, selected);
            for (const bool enabled : {true, false}) {
                EnableWindow(button, enabled);
                Canvas canvas(button);
                Check(Palette(canvas.Pixel(125, 18)),
                      "A solid button surface must use the four-colour palette.");
            }
        }
    }
    EnableWindow(button, TRUE);
}
void Cycle(HWND parent, bool contracts) {
    Window button(Child(parent, L"BUTTON", L"Apply configuration \u03a9", BS_PUSHBUTTON, 201, 8));
    Window check(Child(parent, L"BUTTON", L"Live apply valid edits", BS_AUTOCHECKBOX, 202, 52));
    Window combo(Child(parent, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 203, 96));
    Window edit(Child(parent, L"EDIT", L"LX + physical centre", WS_BORDER | ES_AUTOHSCROLL, 204, 140));
    Window list(Child(parent, L"LISTBOX", L"", LBS_HASSTRINGS | LBS_NOTIFY | WS_VSCROLL, 205, 184));
    constexpr wchar_t choice[] = L"Replace around the current physical input without progressive drift";
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Add to physical input"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice));
    SendMessageW(combo, CB_SETCURSEL, 1, 0);
    SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"07 - Additive Figure Eight.igc"));
    SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"09 - Chords Hold and Double Tap.igc"));
    for (int repeat = 0; repeat < 3; ++repeat) {
        StyleButton(button, ButtonKind::Primary);
        StyleCheckBox(check);
        StyleComboBox(combo);
        StyleEdit(edit);
        StyleScrollbars(list);
    }
    SetAccessibleName(button, L"Apply controller configuration");
    SetAccessibleName(combo, L"Output blend mode");
    SetAccessibleName(edit, L"Controller expression");
    if (contracts) {
        AccessibleName(button, L"Apply the current controller configuration");
        AccessibleName(combo, L"Output blend mode");
        AccessibleName(edit, L"Controller expression");
        Check(Caption(button) == L"Apply configuration \u03a9",
              "Styling must retain the native button's accessible Unicode caption.");
        Check((GetWindowLongPtrW(button, GWL_STYLE) & BS_TYPEMASK) == BS_PUSHBUTTON,
              "Styled push buttons must retain their native control type.");
        Check((GetWindowLongPtrW(check, GWL_STYLE) & BS_TYPEMASK) == BS_AUTOCHECKBOX,
              "Styled check boxes must retain automatic native check behaviour.");
        Check((GetWindowLongPtrW(combo, GWL_STYLE) & CBS_DROPDOWNLIST) == CBS_DROPDOWNLIST,
              "Styling must preserve the native drop-down-list interaction.");
        Check((GetWindowLongPtrW(combo, GWL_STYLE) & CBS_HASSTRINGS) != 0,
              "Owner-drawn combos must retain real accessible strings.");
        const int before = notifications;
        SendMessageW(button, BM_CLICK, 0, 0);
        Check(notifications == before + 1, "A native push button must notify its owner exactly once.");
        Check(SendMessageW(check, BM_GETCHECK, 0, 0) == BST_UNCHECKED,
              "A styled native check box must initially be unchecked.");
        SendMessageW(check, BM_CLICK, 0, 0);
        Check(SendMessageW(check, BM_GETCHECK, 0, 0) == BST_CHECKED && notifications == before + 2,
              "A styled check box must toggle and notify through ordinary BM_CLICK.");
        SendMessageW(check, BM_CLICK, 0, 0);
        Check(SendMessageW(check, BM_GETCHECK, 0, 0) == BST_UNCHECKED,
              "A second native activation must clear the check box.");
        Check(Caption(edit) == L"LX + physical centre", "Styling must not modify editable text.");
        SendMessageW(edit, EM_SETSEL, 0, -1);
        SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"RX + 25"));
        Check(Caption(edit) == L"RX + 25", "Styled edits must preserve native selection and replacement.");
        std::array<wchar_t, 256> text{};
        Check(SendMessageW(combo, CB_GETLBTEXT, 1, reinterpret_cast<LPARAM>(text.data())) > 0 &&
                  std::wstring(text.data()) == choice,
              "Styled combo captions must remain available to native accessibility and type search.");
        Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1,
              "Repeated styling must preserve the selected combo option.");
        text.fill(0);
        Check(SendMessageW(list, LB_GETTEXT, 0, reinterpret_cast<LPARAM>(text.data())) > 0 &&
                  std::wstring(text.data()) == L"07 - Additive Figure Eight.igc",
              "Custom scrollbars must preserve native list strings.");
        Check(GetNextDlgTabItem(parent, button, FALSE) == check &&
                  GetNextDlgTabItem(parent, check, FALSE) == combo &&
                  GetNextDlgTabItem(parent, combo, FALSE) == edit,
              "Styled controls must remain reachable in native tab order.");
        EnableWindow(combo, FALSE);
        Check(GetNextDlgTabItem(parent, check, FALSE) == edit,
              "Native tab navigation must skip disabled styled controls.");
        EnableWindow(combo, TRUE);
    }
    int previousItemHeight = 0;
    for (const UINT dpi : {96u, 144u, 192u}) {
        for (HWND control : {static_cast<HWND>(button), static_cast<HWND>(check), static_cast<HWND>(combo),
                             static_cast<HWND>(edit), static_cast<HWND>(list)})
            SetDpi(control, dpi);
        const auto height = static_cast<int>(SendMessageW(combo, CB_GETITEMHEIGHT, 0, 0));
        Check(height >= MulDiv(16, static_cast<int>(dpi), 96) && height >= previousItemHeight,
              "Combo items must stay legible and increase with DPI.");
        previousItemHeight = height;
        Check(SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1,
              "Changing DPI must preserve the selected combo option.");
        const auto dc = GetDC(combo);
        const auto font = reinterpret_cast<HFONT>(SendMessageW(combo, WM_GETFONT, 0, 0));
        const auto previous = font ? SelectObject(dc, font) : nullptr;
        SIZE extent{};
        GetTextExtentPoint32W(dc, choice, static_cast<int>(std::size(choice) - 1), &extent);
        if (font)
            SelectObject(dc, previous);
        ReleaseDC(combo, dc);
        Check(SendMessageW(combo, CB_GETDROPPEDWIDTH, 0, 0) >= extent.cx,
              "The option popup must fit a long label without clipping it.");
        SetWindowTextW(button, L"");
        PaintBlankButton(button);
        Canvas checkPaint(check), editPaint(edit);
    }
    Pump();
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    nlohmann::ordered_json report;
    std::filesystem::path destination;
    int result = 1;
    DWORD gdiBefore = 0, gdiAfter = 0, userBefore = 0, userAfter = 0;
    const auto started = std::chrono::steady_clock::now();
    report["passed"] = false;
    report["driver_access"] = false;
    report["capture_started"] = false;
    try {
        for (int i = 1; i < argc; ++i) {
            if (std::wstring(argv[i]) != L"--json" || i + 1 >= argc)
                throw std::invalid_argument("Use RemapperWidgetTests [--json report.json].");
            destination = argv[++i];
        }
        Com com;
        Check(SUCCEEDED(com.Status), "Offline accessibility tests must initialize COM on their UI thread.");
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES};
        Check(InitCommonControlsEx(&common) != FALSE, "Native common controls must initialize.");
        const std::set<COLORREF> palette{Blue, Black, White, Gray};
        Check(palette.size() == 4, "Application components must share exactly four solid colours.");
        Check(Contrast(White, Blue) >= 4.5, "Primary action captions must have readable text contrast.");
        Check(Contrast(Black, White) >= 7.0, "Body text must retain strong contrast on white surfaces.");
        Check(Contrast(Gray, Black) >= 4.5,
              "Secondary text must stay readable on the actual dark application surfaces.");
        WNDCLASSEXW type{};
        type.cbSize = sizeof(type);
        type.lpfnWndProc = ParentProcedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"GprOfflineWidgetTests";
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        Check(RegisterClassExW(&type) != 0, "Offline widget parent class must register.");
        Window parent(CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName, L"Offline widget tests",
                                      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, -32000, -32000, 800, 600,
                                      nullptr, nullptr, type.hInstance, nullptr));
        CursorContracts(parent);
        Cycle(parent, true);
        ReentrantDestruction(parent);
        AnimationContracts(parent);
        ScrollContracts(parent);
        for (int i = 0; i < 4; ++i)
            Cycle(parent, false);
        Pump();
        gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
        for (int i = 0; i < 100; ++i)
            Cycle(parent, false);
        Pump();
        gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        userAfter = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
        Check(gdiAfter <= gdiBefore + 2, "Repeated widget lifetime and painting must not leak GDI handles.");
        Check(userAfter <= userBefore + 2, "Repeated widget lifetime must not leak USER handles.");
        report["passed"] = true;
        result = 0;
    } catch (const std::exception &error) {
        report["error"] = error.what();
    }
    report["checks"] = checks;
    report["lifetime_cycles"] = 100;
    report["dpi_cases"] = {96, 144, 192};
    report["gdi_before"] = gdiBefore;
    report["gdi_after"] = gdiAfter;
    report["user_before"] = userBefore;
    report["user_after"] = userAfter;
    report["elapsed_ms"] =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    report["scope"] = "Offscreen native controls, local control messages, animation timers and painting only; no "
                      "host input, capture service, driver, clipboard or file dialogs.";
    if (!destination.empty()) {
        if (destination.has_parent_path())
            std::filesystem::create_directories(destination.parent_path());
        std::ofstream file(destination, std::ios::binary | std::ios::trunc);
        file << report.dump(2) << '\n';
        if (!file)
            return 1;
    }
    std::cout << report.dump() << '\n';
    return result;
}
