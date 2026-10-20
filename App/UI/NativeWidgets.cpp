#include "NativeWidgets.h"
#include <Richedit.h>
#include <initguid.h>
#include <OleAcc.h>
#include <uxtheme.h>
#include <algorithm>
#include <array>
#include <climits>
#include <memory>
#include <stdexcept>
#include <string>

namespace gpr::ui {
namespace {
constexpr UINT_PTR SubclassId = 0x47505257;
constexpr UINT_PTR ScrollTimerId = 0x47505258;
enum class ControlKind { None, Button, Check, Combo, Edit };
struct State {
    unsigned References = 1;
    bool Destroyed = false;
    ControlKind Kind = ControlKind::None;
    ButtonKind Button = ButtonKind::Secondary;
    bool Hover = false, Selected = false, Scrollbars = false;
    bool Toggle = false;
    UINT Dpi = 0;
    int DragBar = -1, DragOffset = 0;
    int RepeatBar = -1, RepeatAction = 0;
    bool EmbeddedEdit = false, PopupBorder = false;
    IAccPropServices *Accessibility = nullptr;
    ~State() {
        if (Accessibility)
            Accessibility->Release();
    }
};
void Release(State *state) {
    if (--state->References == 0)
        delete state;
}
struct StateReference {
    State *Value;
    explicit StateReference(State *state) : Value(state) { ++Value->References; }
    ~StateReference() { Release(Value); }
};
struct Resources {
    std::array<HBRUSH, 4> Brushes{};
    std::array<HPEN, 4> Pens{};
    Resources() {
        const std::array<COLORREF, 4> colours = {Black, White, Gray, Blue};
        for (std::size_t i = 0; i < colours.size(); ++i) {
            Brushes[i] = CreateSolidBrush(colours[i]);
            Pens[i] = CreatePen(PS_SOLID, 1, colours[i]);
        }
    }
    ~Resources() {
        for (auto brush : Brushes)
            if (brush)
                DeleteObject(brush);
        for (auto pen : Pens)
            if (pen)
                DeleteObject(pen);
    }
};
Resources &Gdi() {
    static Resources resources;
    return resources;
}
std::size_t ColourIndex(COLORREF colour) {
    return colour == White ? 1 : colour == Gray ? 2 : colour == Blue ? 3 : 0;
}
HBRUSH Brush(COLORREF colour) { return Gdi().Brushes[ColourIndex(colour)]; }
HPEN Pen(COLORREF colour) { return Gdi().Pens[ColourIndex(colour)]; }
UINT Dpi(HWND window, const State &state) {
    const UINT value = state.Dpi ? state.Dpi : GetDpiForWindow(window);
    return value ? value : 96;
}
int Scale(HWND window, const State &state, int value) {
    return MulDiv(value, static_cast<int>(Dpi(window, state)), 96);
}
struct SavedDc {
    HDC Dc;
    int Saved;
    explicit SavedDc(HDC dc) : Dc(dc), Saved(SaveDC(dc)) {}
    ~SavedDc() {
        if (Saved)
            RestoreDC(Dc, Saved);
    }
};
struct PaintDc {
    HWND Window;
    PAINTSTRUCT Paint{};
    HDC Dc;
    bool Painting;
    PaintDc(HWND window, HDC dc) : Window(window), Dc(dc), Painting(dc == nullptr) {
        if (Painting)
            Dc = BeginPaint(window, &Paint);
    }
    ~PaintDc() {
        if (Painting)
            EndPaint(Window, &Paint);
    }
};
std::wstring Text(HWND window) {
    const int length = std::clamp(GetWindowTextLengthW(window), 0, 32768);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    text.resize(static_cast<std::size_t>(std::max(0, GetWindowTextW(window, text.data(), length + 1))));
    return text;
}
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR reference);
State *Get(HWND window) {
    DWORD_PTR reference = 0;
    if (GetWindowSubclass(window, Procedure, SubclassId, &reference))
        return reinterpret_cast<State *>(reference);
    auto state = std::make_unique<State>();
    if (!SetWindowSubclass(window, Procedure, SubclassId, reinterpret_cast<DWORD_PTR>(state.get())))
        throw std::runtime_error("Windows could not style a native control.");
    return state.release();
}
State *Existing(HWND window) {
    DWORD_PTR reference = 0;
    return GetWindowSubclass(window, Procedure, SubclassId, &reference)
               ? reinterpret_cast<State *>(reference)
               : nullptr;
}
void Frame(HDC dc, RECT rectangle, COLORREF colour, int width = 1) {
    for (int i = 0; i < width && rectangle.right > rectangle.left && rectangle.bottom > rectangle.top;
         ++i) {
        FrameRect(dc, &rectangle, Brush(colour));
        InflateRect(&rectangle, -1, -1);
    }
}
void Rounded(HDC dc, RECT rectangle, COLORREF fill, COLORREF border, int radius) {
    SelectObject(dc, Brush(fill));
    SelectObject(dc, Pen(border));
    RoundRect(dc, rectangle.left, rectangle.top, rectangle.right, rectangle.bottom, radius, radius);
}
void Caption(HDC dc, HWND window, RECT rectangle, COLORREF colour, UINT flags) {
    const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    if (font)
        SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, colour);
    auto caption = Text(window);
    if ((SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEACCEL) != 0)
        flags |= DT_HIDEPREFIX;
    DrawTextW(dc, caption.c_str(), static_cast<int>(caption.size()), &rectangle, flags);
}
void DrawButton(HWND window, HDC dc, const State &state) {
    SavedDc saved(dc);
    RECT rectangle{};
    GetClientRect(window, &rectangle);
    FillRect(dc, &rectangle, Brush(Black));
    const bool enabled = IsWindowEnabled(window) != FALSE;
    const auto native = SendMessageW(window, BM_GETSTATE, 0, 0);
    const bool pressed = (native & BST_PUSHED) != 0;
    const bool focus = GetFocus() == window;
    const bool primary = state.Button == ButtonKind::Primary || state.Selected;
    COLORREF fill = enabled && !pressed && (primary || state.Hover) ? Blue : Black;
    COLORREF border = !enabled ? Gray : primary || state.Hover || focus ? Blue : Gray;
    InflateRect(&rectangle, -1, -1);
    Rounded(dc, rectangle, fill, border, Scale(window, state, 8));
    if (focus && (SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0) {
        auto ring = rectangle;
        InflateRect(&ring, -Scale(window, state, 3), -Scale(window, state, 3));
        Frame(dc, ring, White);
    }
    if (state.Button == ButtonKind::Tab && state.Selected) {
        auto line = rectangle;
        line.top = line.bottom - std::max(2, Scale(window, state, 2));
        FillRect(dc, &line, Brush(White));
    }
    if (pressed)
        OffsetRect(&rectangle, 0, Scale(window, state, 1));
    Caption(dc, window, rectangle, enabled ? White : Gray,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
void DrawCheck(HWND window, HDC dc, const State &state) {
    SavedDc saved(dc);
    RECT rectangle{};
    GetClientRect(window, &rectangle);
    FillRect(dc, &rectangle, Brush(Black));
    const bool enabled = IsWindowEnabled(window) != FALSE;
    const auto checked = SendMessageW(window, BM_GETCHECK, 0, 0);
    if (state.Toggle) {
        const int height = Scale(window, state, 24), width = Scale(window, state, 44);
        const int top = (rectangle.bottom - height) / 2;
        RECT track{Scale(window, state, 2), top, Scale(window, state, 2) + width, top + height};
        Rounded(dc, track, enabled && checked ? Blue : Black,
                enabled && (state.Hover || checked || GetFocus() == window) ? Blue : Gray, height);
        const int margin = Scale(window, state, 4), diameter = height - 2 * margin;
        const int left = checked ? track.right - margin - diameter : track.left + margin;
        SelectObject(dc, Brush(enabled ? White : Gray));
        SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, left, top + margin, left + diameter, top + margin + diameter);
        rectangle.left = track.right + Scale(window, state, 12);
        Caption(dc, window, rectangle, enabled ? White : Gray,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (GetFocus() == window)
            Frame(dc, rectangle, Blue);
        return;
    }
    const int size = Scale(window, state, 18);
    const int left = Scale(window, state, 2), top = (rectangle.bottom - size) / 2;
    RECT box{left, top, left + size, top + size};
    Rounded(dc, box, enabled && checked ? Blue : Black,
            enabled && (state.Hover || GetFocus() == window || checked) ? Blue : Gray,
            Scale(window, state, 4));
    if (checked) {
        SelectObject(dc, Brush(enabled ? White : Gray));
        SelectObject(dc, GetStockObject(NULL_PEN));
        if (checked == BST_INDETERMINATE) {
            RECT mark{left + size / 4, top + size / 2 - 1, left + size * 3 / 4, top + size / 2 + 2};
            FillRect(dc, &mark, Brush(enabled ? White : Gray));
        } else {
            POINT mark[] = {
                {left + size * 2 / 9, top + size * 4 / 9}, {left + size * 4 / 9, top + size * 6 / 9},
                {left + size * 7 / 9, top + size * 2 / 9}, {left + size * 8 / 9, top + size * 3 / 9},
                {left + size * 4 / 9, top + size * 8 / 9}, {left + size / 9, top + size * 5 / 9}};
            Polygon(dc, mark, static_cast<int>(std::size(mark)));
        }
    }
    rectangle.left = box.right + Scale(window, state, 9);
    Caption(dc, window, rectangle, enabled ? White : Gray,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (GetFocus() == window && (SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0) {
        InflateRect(&rectangle, -1, -Scale(window, state, 3));
        Frame(dc, rectangle, Blue);
    }
}
std::wstring ComboText(HWND window, int index) {
    if (index < 0)
        return {};
    const auto length = SendMessageW(window, CB_GETLBTEXTLEN, static_cast<WPARAM>(index), 0);
    if (length == CB_ERR || length < 0 || length > 32768)
        return {};
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    const auto count = SendMessageW(window, CB_GETLBTEXT, static_cast<WPARAM>(index),
                                    reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<std::size_t>(std::max<LRESULT>(0, count == CB_ERR ? 0 : count)));
    return text;
}
void DrawCombo(HWND window, HDC dc, const State &state) {
    SavedDc saved(dc);
    RECT rectangle{};
    GetClientRect(window, &rectangle);
    FillRect(dc, &rectangle, Brush(Black));
    const bool enabled = IsWindowEnabled(window) != FALSE;
    const bool focus = GetFocus() == window || IsChild(window, GetFocus()) ||
                       SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0) != 0;
    Frame(dc, rectangle, enabled && (state.Hover || focus) ? Blue : Gray,
          focus ? std::max(1, Scale(window, state, 2)) : 1);
    const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
    if (font)
        SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? White : Gray);
    auto caption = ComboText(window, static_cast<int>(SendMessageW(window, CB_GETCURSEL, 0, 0)));
    rectangle.left += Scale(window, state, 10);
    rectangle.right -= Scale(window, state, 30);
    DrawTextW(dc, caption.c_str(), static_cast<int>(caption.size()), &rectangle,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT client{};
    GetClientRect(window, &client);
    const int x = client.right - Scale(window, state, 16), y = client.bottom / 2;
    const int distance = Scale(window, state, 4);
    POINT arrow[] = {
        {x - distance, y - distance / 2}, {x + distance, y - distance / 2}, {x, y + distance / 2 + 1}};
    SelectObject(dc, Brush(enabled ? White : Gray));
    SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, arrow, static_cast<int>(std::size(arrow)));
}
void ComboMetrics(HWND window, const State &state) {
    SendMessageW(window, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), Scale(window, state, 30));
    SendMessageW(window, CB_SETITEMHEIGHT, 0, Scale(window, state, 30));
    HDC dc = GetDC(window);
    if (!dc)
        return;
    {
        SavedDc saved(dc);
        const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
        if (font)
            SelectObject(dc, font);
        int width = Scale(window, state, 160);
        const auto count = std::clamp<LRESULT>(SendMessageW(window, CB_GETCOUNT, 0, 0), 0, 4096);
        for (LRESULT i = 0; i < count; ++i) {
            const auto text = ComboText(window, static_cast<int>(i));
            SIZE size{};
            GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
            width = std::max(width, static_cast<int>(size.cx) + Scale(window, state, 48));
        }
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
            width = std::min(width, static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
        SendMessageW(window, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(width), 0);
        COMBOBOXINFO info{sizeof(info)};
        if (GetComboBoxInfo(window, &info) && info.hwndList) {
            SetWindowTheme(info.hwndList, L"", L"");
            auto *list = Get(info.hwndList);
            list->Scrollbars = true;
            list->PopupBorder = true;
            list->Dpi = state.Dpi;
            SetWindowLongPtrW(info.hwndList, GWL_EXSTYLE,
                              GetWindowLongPtrW(info.hwndList, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
        }
    }
    ReleaseDC(window, dc);
}
void ExpandComboWidth(HWND window, int index, const State &state) {
    if (index < 0)
        return;
    HDC dc = GetDC(window);
    if (!dc)
        return;
    {
        SavedDc saved(dc);
        const auto font = reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
        if (font)
            SelectObject(dc, font);
        const auto text = ComboText(window, index);
        SIZE size{};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
        int width = std::max(static_cast<int>(SendMessageW(window, CB_GETDROPPEDWIDTH, 0, 0)),
                             static_cast<int>(size.cx) + Scale(window, state, 48));
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
            width = std::min(width, static_cast<int>(monitor.rcWork.right - monitor.rcWork.left));
        SendMessageW(window, CB_SETDROPPEDWIDTH, static_cast<WPARAM>(width), 0);
    }
    ReleaseDC(window, dc);
}
void DrawEditFrame(HWND window, HDC dc, const State &state) {
    if (state.EmbeddedEdit)
        return;
    SavedDc saved(dc);
    RECT rectangle{};
    GetClientRect(window, &rectangle);
    Frame(dc, rectangle, IsWindowEnabled(window) && GetFocus() == window ? Blue : Gray,
          GetFocus() == window ? std::max(1, Scale(window, state, 2)) : 1);
}
struct Bar {
    RECT Bounds{}, Thumb{}, Track{};
    SCROLLINFO Scroll{sizeof(SCROLLINFO), SIF_ALL};
    bool Visible = false;
};
Bar ScrollBar(HWND window, int bar) {
    Bar result;
    SCROLLBARINFO info{sizeof(info)};
    if (!GetScrollBarInfo(window, bar == SB_VERT ? OBJID_VSCROLL : OBJID_HSCROLL, &info) ||
        (info.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) != 0 ||
        !GetScrollInfo(window, bar, &result.Scroll))
        return result;
    RECT outer{};
    GetWindowRect(window, &outer);
    result.Bounds = info.rcScrollBar;
    OffsetRect(&result.Bounds, -outer.left, -outer.top);
    result.Track = result.Bounds;
    result.Thumb = result.Bounds;
    if (bar == SB_VERT) {
        result.Track.top += info.dxyLineButton;
        result.Track.bottom -= info.dxyLineButton;
        result.Thumb.top = result.Bounds.top + info.xyThumbTop;
        result.Thumb.bottom = result.Bounds.top + info.xyThumbBottom;
        InflateRect(&result.Thumb, -4, 0);
    } else {
        result.Track.left += info.dxyLineButton;
        result.Track.right -= info.dxyLineButton;
        result.Thumb.left = result.Bounds.left + info.xyThumbTop;
        result.Thumb.right = result.Bounds.left + info.xyThumbBottom;
        InflateRect(&result.Thumb, 0, -4);
    }
    result.Visible =
        result.Bounds.right > result.Bounds.left && result.Bounds.bottom > result.Bounds.top;
    return result;
}
void PaintScrollbars(HWND window, const State &state, HDC target = nullptr) {
    if (!state.Scrollbars)
        return;
    HDC dc = target ? target : GetWindowDC(window);
    if (!dc)
        return;
    {
        SavedDc saved(dc);
        if (state.PopupBorder) {
            RECT outer{};
            GetWindowRect(window, &outer);
            RECT frame{0, 0, outer.right - outer.left, outer.bottom - outer.top};
            Frame(dc, frame, Gray);
        }
        for (int which : {SB_VERT, SB_HORZ}) {
            const auto bar = ScrollBar(window, which);
            if (!bar.Visible)
                continue;
            FillRect(dc, &bar.Bounds, Brush(Black));
            if (bar.Scroll.nMax > bar.Scroll.nMin && bar.Thumb.bottom > bar.Thumb.top &&
                bar.Thumb.right > bar.Thumb.left)
                Rounded(dc, bar.Thumb, state.DragBar == which ? Blue : Gray,
                        state.DragBar == which ? Blue : Gray, Scale(window, state, 5));
            // The native arrow hit areas remain available, with quiet four-colour chevrons.
            const bool vertical = which == SB_VERT;
            for (int direction : {-1, 1}) {
                const int centerX = vertical        ? (bar.Bounds.left + bar.Bounds.right) / 2
                                    : direction < 0 ? (bar.Bounds.left + bar.Track.left) / 2
                                                    : (bar.Bounds.right + bar.Track.right) / 2;
                const int centerY = vertical ? direction < 0 ? (bar.Bounds.top + bar.Track.top) / 2
                                                             : (bar.Bounds.bottom + bar.Track.bottom) / 2
                                             : (bar.Bounds.top + bar.Bounds.bottom) / 2;
                const int d = std::max(2, Scale(window, state, 3));
                POINT points[3] = {};
                if (vertical) {
                    points[0] = {centerX - d, centerY - direction};
                    points[1] = {centerX + d, centerY - direction};
                    points[2] = {centerX, centerY + direction * d};
                } else {
                    points[0] = {centerX - direction, centerY - d};
                    points[1] = {centerX - direction, centerY + d};
                    points[2] = {centerX + direction * d, centerY};
                }
                SelectObject(dc, Brush(Gray));
                SelectObject(dc, GetStockObject(NULL_PEN));
                Polygon(dc, points, 3);
            }
        }
        const auto vertical = ScrollBar(window, SB_VERT), horizontal = ScrollBar(window, SB_HORZ);
        if (vertical.Visible && horizontal.Visible) {
            RECT corner{vertical.Bounds.left, horizontal.Bounds.top, vertical.Bounds.right,
                        horizontal.Bounds.bottom};
            FillRect(dc, &corner, Brush(Black));
        }
    }
    if (!target)
        ReleaseDC(window, dc);
}
POINT WindowPoint(HWND window, LPARAM coordinate, bool screen) {
    POINT point{static_cast<short>(LOWORD(coordinate)), static_cast<short>(HIWORD(coordinate))};
    if (!screen)
        ClientToScreen(window, &point);
    RECT outer{};
    GetWindowRect(window, &outer);
    point.x -= outer.left;
    point.y -= outer.top;
    return point;
}
void ScrollTo(HWND window, int which, int position, const Bar &bar) {
    const int maximum =
        std::max(bar.Scroll.nMin,
                 bar.Scroll.nMax - static_cast<int>(bar.Scroll.nPage ? bar.Scroll.nPage - 1 : 0));
    position = std::clamp(position, bar.Scroll.nMin, maximum);
    wchar_t name[64]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    if (wcsstr(name, L"RICHEDIT") || wcsstr(name, L"RichEdit")) {
        // EM_GETSCROLLPOS truncates large coordinates to 16 bits. Preserve the
        // other axis through the native 32-bit scrollbar position instead.
        SCROLLINFO horizontal{sizeof(horizontal), SIF_POS}, vertical{sizeof(vertical), SIF_POS};
        GetScrollInfo(window, SB_HORZ, &horizontal);
        GetScrollInfo(window, SB_VERT, &vertical);
        POINT point{horizontal.nPos, vertical.nPos};
        (which == SB_VERT ? point.y : point.x) = position;
        SendMessageW(window, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&point));
    } else if (_wcsicmp(name, L"Edit") == 0) {
        const int change = position - bar.Scroll.nPos;
        SendMessageW(window, EM_LINESCROLL, which == SB_HORZ ? change : 0,
                     which == SB_VERT ? change : 0);
    } else {
        SCROLLINFO setting{sizeof(setting), SIF_POS, 0, 0, 0, position, 0};
        SetScrollInfo(window, which, &setting, FALSE);
        SendMessageW(window, which == SB_VERT ? WM_VSCROLL : WM_HSCROLL,
                     MAKEWPARAM(SB_THUMBPOSITION, static_cast<WORD>(std::min(position, 65535))), 0);
    }
}
bool BeginScroll(HWND window, State &state, LPARAM coordinate) {
    const auto point = WindowPoint(window, coordinate, true);
    for (int which : {SB_VERT, SB_HORZ}) {
        const auto bar = ScrollBar(window, which);
        if (!bar.Visible || !PtInRect(&bar.Bounds, point))
            continue;
        const bool vertical = which == SB_VERT;
        const int cursor = vertical ? point.y : point.x;
        const int first = vertical ? bar.Thumb.top : bar.Thumb.left;
        const int last = vertical ? bar.Thumb.bottom : bar.Thumb.right;
        const int trackFirst = vertical ? bar.Track.top : bar.Track.left;
        const int trackLast = vertical ? bar.Track.bottom : bar.Track.right;
        if (cursor >= first && cursor < last) {
            state.DragBar = which;
            state.DragOffset = cursor - first;
            SetCapture(window);
        } else {
            const bool line = cursor < trackFirst || cursor >= trackLast;
            const bool before = cursor < first;
            const int action = line     ? before ? SB_LINEUP : SB_LINEDOWN
                               : before ? SB_PAGEUP
                                        : SB_PAGEDOWN;
            SendMessageW(window, vertical ? WM_VSCROLL : WM_HSCROLL, static_cast<WPARAM>(action), 0);
            if (!state.Destroyed) {
                state.RepeatBar = which;
                state.RepeatAction = action;
                SetCapture(window);
                SetTimer(window, ScrollTimerId, 350, nullptr);
            }
        }
        PaintScrollbars(window, state);
        return true;
    }
    return false;
}
void DragScroll(HWND window, State &state, LPARAM coordinate) {
    const auto bar = ScrollBar(window, state.DragBar);
    if (!bar.Visible)
        return;
    const auto point = WindowPoint(window, coordinate, false);
    const bool vertical = state.DragBar == SB_VERT;
    const int first = vertical ? bar.Track.top : bar.Track.left;
    const int last = vertical ? bar.Track.bottom : bar.Track.right;
    const int thumb = vertical ? bar.Thumb.bottom - bar.Thumb.top : bar.Thumb.right - bar.Thumb.left;
    const int travel = last - first - thumb;
    if (travel <= 0)
        return;
    const int offset =
        std::clamp(static_cast<int>(vertical ? point.y : point.x) - first - state.DragOffset, 0, travel);
    const int maximum =
        std::max(bar.Scroll.nMin,
                 bar.Scroll.nMax - static_cast<int>(bar.Scroll.nPage ? bar.Scroll.nPage - 1 : 0));
    const auto position = static_cast<int>(bar.Scroll.nMin + static_cast<long long>(offset) *
                                                                 (maximum - bar.Scroll.nMin) / travel);
    ScrollTo(window, state.DragBar, position, bar);
    PaintScrollbars(window, state);
}
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR,
                           DWORD_PTR reference) {
    auto *state = reinterpret_cast<State *>(reference);
    StateReference lifetime(state);
    if (message == WM_NCDESTROY) {
        state->Destroyed = true;
        if (state->Accessibility) {
            const MSAAPROPID property = PROPID_ACC_NAME;
            state->Accessibility->ClearHwndProps(window, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF,
                                                 &property, 1);
        }
        RemoveWindowSubclass(window, Procedure, SubclassId);
        Release(state);
        return DefSubclassProc(window, message, w, l);
    }
    bool forwarded = false;
    LRESULT nativeResult = 0;
    const auto native = [&] {
        forwarded = true;
        nativeResult = DefSubclassProc(window, message, w, l);
        return nativeResult;
    };
    try {
        if (message == WM_PRINT) {
            const auto result = native();
            if (!state->Destroyed && (l & PRF_NONCLIENT) != 0)
                PaintScrollbars(window, *state, reinterpret_cast<HDC>(w));
            return result;
        }
        if (message == WM_ERASEBKGND && state->PopupBorder) {
            RECT client{};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(w), &client, Brush(Black));
            return 1;
        }
        if (state->Kind == ControlKind::Combo &&
            (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX ||
             message == WM_CTLCOLORSTATIC)) {
            SetBkColor(reinterpret_cast<HDC>(w), Black);
            SetTextColor(reinterpret_cast<HDC>(w), IsWindowEnabled(window) ? White : Gray);
            return reinterpret_cast<LRESULT>(Brush(Black));
        }
        if (message == WM_ERASEBKGND &&
            (state->Kind == ControlKind::Button || state->Kind == ControlKind::Check ||
             state->Kind == ControlKind::Combo))
            return 1;
        if (message == WM_PAINT || message == WM_PRINTCLIENT) {
            if (state->Kind == ControlKind::Button || state->Kind == ControlKind::Check ||
                state->Kind == ControlKind::Combo) {
                PaintDc paint(window, message == WM_PRINTCLIENT ? reinterpret_cast<HDC>(w) : nullptr);
                if (state->Kind == ControlKind::Button)
                    DrawButton(window, paint.Dc, *state);
                else if (state->Kind == ControlKind::Check)
                    DrawCheck(window, paint.Dc, *state);
                else
                    DrawCombo(window, paint.Dc, *state);
                return 0;
            }
            const auto result = native();
            if (state->Destroyed)
                return result;
            if (state->Kind == ControlKind::Edit) {
                HDC dc = message == WM_PRINTCLIENT ? reinterpret_cast<HDC>(w) : GetDC(window);
                if (dc) {
                    DrawEditFrame(window, dc, *state);
                    if (message != WM_PRINTCLIENT)
                        ReleaseDC(window, dc);
                }
            }
            if (message != WM_PRINTCLIENT)
                PaintScrollbars(window, *state);
            return result;
        }
        if (message == WM_NCLBUTTONDOWN && state->Scrollbars && BeginScroll(window, *state, l))
            return 0;
        if (message == WM_TIMER && w == ScrollTimerId && state->RepeatBar >= 0) {
            const auto bar = ScrollBar(window, state->RepeatBar);
            POINT point{};
            GetCursorPos(&point);
            RECT outer{};
            GetWindowRect(window, &outer);
            point.x -= outer.left;
            point.y -= outer.top;
            const bool page = state->RepeatAction == SB_PAGEUP || state->RepeatAction == SB_PAGEDOWN;
            if (bar.Visible && PtInRect(&bar.Bounds, point) && (!page || !PtInRect(&bar.Thumb, point)))
                SendMessageW(window, state->RepeatBar == SB_VERT ? WM_VSCROLL : WM_HSCROLL,
                             static_cast<WPARAM>(state->RepeatAction), 0);
            if (!state->Destroyed) {
                SetTimer(window, ScrollTimerId, 50, nullptr);
                PaintScrollbars(window, *state);
            }
            return 0;
        }
        if (message == WM_MOUSEMOVE && state->DragBar >= 0) {
            DragScroll(window, *state, l);
            return 0;
        }
        if (message == WM_MOUSEMOVE && state->RepeatBar >= 0)
            return 0;
        if (message == WM_LBUTTONUP && (state->DragBar >= 0 || state->RepeatBar >= 0)) {
            const int bar = state->DragBar >= 0 ? state->DragBar : state->RepeatBar;
            state->DragBar = -1;
            state->RepeatBar = -1;
            KillTimer(window, ScrollTimerId);
            if (GetCapture() == window)
                ReleaseCapture();
            SendMessageW(window, bar == SB_VERT ? WM_VSCROLL : WM_HSCROLL, SB_ENDSCROLL, 0);
            PaintScrollbars(window, *state);
            return 0;
        }
        if (message == WM_CAPTURECHANGED) {
            state->DragBar = -1;
            state->RepeatBar = -1;
            KillTimer(window, ScrollTimerId);
        }
        if (message == WM_MOUSEMOVE && !state->Hover) {
            state->Hover = true;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
            TrackMouseEvent(&track);
            InvalidateRect(window, nullptr, FALSE);
        }
        if (message == WM_MOUSELEAVE) {
            state->Hover = false;
            InvalidateRect(window, nullptr, FALSE);
        }
        if (state->EmbeddedEdit && (message == WM_SETFOCUS || message == WM_KILLFOCUS))
            InvalidateRect(GetParent(window), nullptr, FALSE);
        const auto result = native();
        if (state->Destroyed)
            return result;
        if (state->Kind == ControlKind::Combo &&
            (message == WM_SETFONT || message == WM_DPICHANGED_AFTERPARENT))
            ComboMetrics(window, *state);
        if (state->Kind == ControlKind::Combo &&
            (message == CB_ADDSTRING || message == CB_INSERTSTRING) && result >= 0)
            ExpandComboWidth(window, static_cast<int>(result), *state);
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE ||
            message == WM_SETTEXT || message == WM_UPDATEUISTATE || message == WM_LBUTTONDOWN ||
            message == WM_LBUTTONUP || message == BM_SETSTATE || message == BM_SETCHECK ||
            message == CB_SETCURSEL || message == CB_SHOWDROPDOWN ||
            message == WM_DPICHANGED_AFTERPARENT)
            InvalidateRect(window, nullptr, FALSE);
        if (state->Scrollbars &&
            (message == WM_NCPAINT || message == WM_VSCROLL || message == WM_HSCROLL ||
             message == WM_MOUSEWHEEL || message == WM_SIZE || message == WM_SETTEXT ||
             message == WM_NCMOUSEMOVE || message == WM_CAPTURECHANGED))
            PaintScrollbars(window, *state);
        return result;
    } catch (...) {
        return forwarded ? nativeResult : native();
    }
}
void Repaint(HWND window) { InvalidateRect(window, nullptr, FALSE); }
} // namespace
void StyleButton(HWND window, ButtonKind kind) {
    if (!window)
        return;
    auto *state = Get(window);
    state->Kind = ControlKind::Button;
    state->Button = kind;
    SetWindowTheme(window, L"", L"");
    Repaint(window);
}
void SetButtonSelected(HWND window, bool selected) {
    if (!window)
        return;
    auto *state = Get(window);
    state->Selected = selected;
    Repaint(window);
}
void StyleCheckBox(HWND window) {
    if (!window)
        return;
    Get(window)->Kind = ControlKind::Check;
    SetWindowTheme(window, L"", L"");
    Repaint(window);
}
void StyleToggle(HWND window) {
    StyleCheckBox(window);
    if (window) {
        Get(window)->Toggle = true;
        Repaint(window);
    }
}
void StyleComboBox(HWND window) {
    if (!window)
        return;
    auto *state = Get(window);
    state->Kind = ControlKind::Combo;
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    SetWindowLongPtrW(window, GWL_STYLE, style | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS);
    SetWindowTheme(window, L"", L"");
    ComboMetrics(window, *state);
    COMBOBOXINFO info{sizeof(info)};
    if ((style & CBS_DROPDOWNLIST) != CBS_DROPDOWNLIST && GetComboBoxInfo(window, &info) &&
        info.hwndItem) {
        StyleEdit(info.hwndItem);
        Get(info.hwndItem)->EmbeddedEdit = true;
    }
    Repaint(window);
}
void StyleEdit(HWND window) {
    if (!window)
        return;
    auto *state = Get(window);
    state->Kind = ControlKind::Edit;
    SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
    SetWindowLongPtrW(window, GWL_STYLE, GetWindowLongPtrW(window, GWL_STYLE) & ~WS_BORDER);
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    SendMessageW(window, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(Scale(window, *state, 10), Scale(window, *state, 10)));
    SetWindowTheme(window, L"", L"");
    Repaint(window);
}
void StyleScrollbars(HWND window) {
    if (!window)
        return;
    auto *state = Get(window);
    state->Scrollbars = true;
    SetWindowTheme(window, L"", L"");
    PaintScrollbars(window, *state);
}
void SetDpi(HWND window, UINT dpi) {
    if (!window)
        return;
    auto *state = Get(window);
    state->Dpi = dpi >= 72 && dpi <= 768 ? dpi : 0;
    if (state->Kind == ControlKind::Combo)
        ComboMetrics(window, *state);
    if (state->Kind == ControlKind::Combo) {
        COMBOBOXINFO info{sizeof(info)};
        if (GetComboBoxInfo(window, &info) && info.hwndItem) {
            auto *edit = Existing(info.hwndItem);
            if (edit && edit->EmbeddedEdit)
                SetDpi(info.hwndItem, dpi);
        }
    }
    if (state->Kind == ControlKind::Edit)
        SendMessageW(window, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(Scale(window, *state, 10), Scale(window, *state, 10)));
    Repaint(window);
    PaintScrollbars(window, *state);
}
void SetAccessibleName(HWND window, const wchar_t *name) {
    if (!window || !name)
        return;
    auto *state = Get(window);
    if (!state->Accessibility &&
        FAILED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&state->Accessibility))))
        return;
    state->Accessibility->SetHwndPropStr(window, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF,
                                         PROPID_ACC_NAME, name);
}
bool DrawItem(const DRAWITEMSTRUCT &item) {
    if (item.CtlType != ODT_COMBOBOX || !item.hwndItem)
        return false;
    const auto *state = Existing(item.hwndItem);
    if (!state || state->Kind != ControlKind::Combo)
        return false;
    SavedDc saved(item.hDC);
    const bool enabled = (item.itemState & ODS_DISABLED) == 0 && IsWindowEnabled(item.hwndItem);
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    FillRect(item.hDC, &item.rcItem, Brush(enabled && selected ? Blue : Black));
    const auto font = reinterpret_cast<HFONT>(SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    if (font)
        SelectObject(item.hDC, font);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, enabled ? White : Gray);
    const auto caption =
        ComboText(item.hwndItem, item.itemID == UINT_MAX ? -1 : static_cast<int>(item.itemID));
    auto rectangle = item.rcItem;
    rectangle.left += Scale(item.hwndItem, *state, 10);
    rectangle.right -= Scale(item.hwndItem, *state, 10);
    DrawTextW(item.hDC, caption.c_str(), static_cast<int>(caption.size()), &rectangle,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if ((item.itemState & ODS_FOCUS) != 0 && (item.itemState & ODS_NOFOCUSRECT) == 0) {
        auto ring = item.rcItem;
        InflateRect(&ring, -2, -2);
        Frame(item.hDC, ring, White);
    }
    return true;
}
namespace {
struct PreviewState {
    bool OwnedByWindow = false;
    HFONT Body = nullptr, Heading = nullptr, Small = nullptr;
    ~PreviewState() {
        if (Body)
            DeleteObject(Body);
        if (Heading)
            DeleteObject(Heading);
        if (Small)
            DeleteObject(Small);
    }
};
LRESULT CALLBACK PreviewProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto *state = reinterpret_cast<PreviewState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<PreviewState *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (state && state->OwnedByWindow)
            delete state;
    }
    if (message == WM_DRAWITEM && DrawItem(*reinterpret_cast<DRAWITEMSTRUCT *>(l)))
        return TRUE;
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
        const auto child = reinterpret_cast<HWND>(l);
        SetTextColor(reinterpret_cast<HDC>(w), GetDlgCtrlID(child) < 100 ? Gray : White);
        SetBkColor(reinterpret_cast<HDC>(w), Black);
        return reinterpret_cast<LRESULT>(Brush(Black));
    }
    return DefWindowProcW(window, message, w, l);
}
} // namespace
HWND CreateWidgetPreview(HINSTANCE instance) {
    WNDCLASSEXW type{sizeof(type)};
    type.hInstance = instance;
    type.lpfnWndProc = PreviewProcedure;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = Brush(Black);
    type.lpszClassName = L"GamePadRemapper.WidgetPreview";
    if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("The component preview window could not be registered.");
    auto context = std::make_unique<PreviewState>();
    const UINT dpi = GetDpiForSystem();
    const auto s = [dpi](int value) { return MulDiv(value, static_cast<int>(dpi), 96); };
    const auto font = [dpi](int size, int weight) {
        return CreateFontW(-MulDiv(size, static_cast<int>(dpi), 96), 0, 0, 0, weight, FALSE, FALSE,
                           FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    context->Body = font(15, FW_NORMAL);
    context->Heading = font(22, FW_SEMIBOLD);
    context->Small = font(13, FW_NORMAL);
    RECT bounds{0, 0, s(920), s(478)};
    AdjustWindowRectExForDpi(&bounds, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0, dpi);
    HWND window = CreateWindowExW(0, type.lpszClassName, L"GamePad Remapper - Controls",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                  bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr,
                                  nullptr, instance, context.get());
    if (!window)
        throw std::runtime_error("The component preview window could not be created.");
    auto *owned = context.release();
    owned->OwnedByWindow = true;
    const auto child = [&](const wchar_t *kind, const wchar_t *caption, DWORD style, int id, int x,
                           int y, int width, int height, HFONT chosen = nullptr) {
        HWND result = CreateWindowExW(
            0, kind, caption, WS_CHILD | WS_VISIBLE | style, s(x), s(y), s(width), s(height), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (!result)
            throw std::runtime_error("A component preview control could not be created.");
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(chosen ? chosen : owned->Body), FALSE);
        return result;
    };
    try {
        child(L"STATIC", L"Controls and interaction", 0, 101, 32, 24, 800, 32, owned->Heading);
        child(L"STATIC", L"Blue, black, white and gray. Clear states for every action.", 0, 1, 32, 66,
              820, 24, owned->Small);
        const wchar_t *labels[] = {L"Primary action", L"Hover", L"Keyboard focus", L"Unavailable"};
        const wchar_t *captions[] = {L"Apply script", L"Open script", L"Save changes", L"Disabled"};
        HWND focused = nullptr;
        for (int i = 0; i < 4; ++i) {
            child(L"STATIC", labels[i], 0, 2 + i, 32 + i * 220, 120, 196, 22, owned->Small);
            const auto button = child(L"BUTTON", captions[i], BS_PUSHBUTTON | WS_TABSTOP, 110 + i,
                                      32 + i * 220, 151, 196, 44);
            StyleButton(button, i == 0 ? ButtonKind::Primary : ButtonKind::Secondary);
            if (i == 1)
                Get(button)->Hover = true;
            if (i == 2)
                focused = button;
            if (i == 3)
                EnableWindow(button, FALSE);
        }
        child(L"STATIC", L"Choices", 0, 10, 32, 232, 600, 22, owned->Small);
        const auto checked =
            child(L"BUTTON", L"Live apply", BS_AUTOCHECKBOX | WS_TABSTOP, 120, 32, 266, 196, 32);
        StyleCheckBox(checked);
        SendMessageW(checked, BM_SETCHECK, BST_CHECKED, 0);
        StyleCheckBox(
            child(L"BUTTON", L"Reduce motion", BS_AUTOCHECKBOX | WS_TABSTOP, 121, 252, 266, 196, 32));
        const auto disabled = child(L"BUTTON", L"Unavailable", BS_AUTOCHECKBOX, 122, 472, 266, 196, 32);
        StyleCheckBox(disabled);
        SendMessageW(disabled, BM_SETCHECK, BST_CHECKED, 0);
        EnableWindow(disabled, FALSE);
        child(L"STATIC", L"Activation", 0, 11, 32, 336, 300, 22, owned->Small);
        const auto combo =
            child(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, 130,
                  32, 367, 300, 180);
        StyleComboBox(combo);
        for (const wchar_t *caption :
             {L"While condition is true", L"On activation", L"On deactivation", L"Toggle"})
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(caption));
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        child(L"STATIC", L"Controller name", 0, 12, 360, 336, 300, 22, owned->Small);
        const auto edit =
            child(L"EDIT", L"Everyday controller", ES_AUTOHSCROLL | WS_TABSTOP, 131, 360, 367, 300, 36);
        StyleEdit(edit);
        child(L"STATIC", L"Each control keeps Windows keyboard and accessibility support.", 0, 13, 32,
              435, 840, 24, owned->Small);
        ShowWindow(window, SW_SHOWNOACTIVATE);
        SendMessageW(focused, WM_UPDATEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
        SetFocus(focused);
        UpdateWindow(window);
        return window;
    } catch (...) {
        DestroyWindow(window);
        throw;
    }
}
} // namespace gpr::ui
