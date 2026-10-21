#include "ControllerMonitorView.h"
#include "Theme.h"
#include <CommCtrl.h>
#include <OleAcc.h>
#include <Uxtheme.h>
#include <algorithm>
#include <array>
#include <climits>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

namespace gpr::ui {
namespace {
constexpr wchar_t ViewClass[] = L"GprControllerMonitorView";
constexpr UINT_PTR TabSubclass = 0x47504D54;
constexpr int TabsId = 1;
struct Entry {
    std::string Token;
    std::wstring Name;
    std::uint32_t Key = 0, Family = 0;
    std::array<int, 256> Input{}, Output{};
    std::array<bool, 256> Seen{}, InputAvailable{}, OutputAvailable{};
    bool InputValid = false, OutputValid = false;
    std::uint64_t InputSequence = 0, OutputSequence = 0;
};
struct Row {
    int Slot = -1, Top = 0, Height = 0;
    std::wstring Label;
};
struct State {
    HWND Window = nullptr, Tabs = nullptr;
    UINT Dpi = 96;
    std::vector<Entry> Devices;
    std::vector<Row> Rows;
    int Selected = -1, Scroll = 0, Extent = 0, WheelRemainder = 0;
    int Width = 0, Height = 0, ContentTop = 0, ContentBottom = 0;
    bool Monitoring = false;
    std::wstring Status;
    std::array<HFONT, 4> Fonts{};
    std::array<HBRUSH, 4> Brushes{};
    HDC Buffer = nullptr;
    HBITMAP Bitmap = nullptr, PreviousBitmap = nullptr;
    int BufferWidth = 0, BufferHeight = 0;
    std::uint64_t LayoutRevision = 0, PaintCount = 0;
    ~State() {
        if (Buffer) {
            SelectObject(Buffer, PreviousBitmap);
            if (Bitmap) DeleteObject(Bitmap);
            DeleteDC(Buffer);
        }
        for (auto font : Fonts) if (font) DeleteObject(font);
        for (auto brush : Brushes) if (brush) DeleteObject(brush);
    }
    int S(int value) const { return MulDiv(value, static_cast<int>(Dpi), 96); }
    const Entry *Device() const {
        return Selected >= 0 && Selected < static_cast<int>(Devices.size())
                   ? &Devices[static_cast<std::size_t>(Selected)] : nullptr;
    }
};
struct Creation { State *Value = nullptr; bool Adopted = false; };
State *Get(HWND window) { return reinterpret_cast<State *>(GetWindowLongPtrW(window, GWLP_USERDATA)); }
std::wstring Wide(const std::string &text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return L"Controller";
    std::wstring result(static_cast<std::size_t>(length), L' ');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}
const wchar_t *Family(std::uint32_t family) {
    switch (family) {
    case 3: return L"HID";
    case 4: return L"XInput / XUSB";
    case 5: return L"XInput HID";
    case 6: return L"GIP";
    default: return L"Controller";
    }
}
bool Matches(const Entry &entry, const std::string &token, std::uint32_t key, std::uint32_t family) {
    return entry.Family == family && (!entry.Token.empty() && !token.empty()
                                       ? entry.Token == token : entry.Key == key);
}
int Group(int slot) {
    if (slot < 4) return 0;
    if (slot < 6 || (slot >= 160 && slot < 192)) return 1;
    if ((slot >= 6 && slot < 22) || (slot >= 32 && slot < 96)) return 2;
    if (slot >= 96 && slot < 160) return 3;
    if (slot >= 192 && slot < 208) return 4;
    return 5;
}
bool Button(int slot) { return Group(slot) == 2; }
bool Trigger(int slot) { return Group(slot) == 1; }
bool Axis(int slot) { return Group(slot) == 0 || Group(slot) == 3; }
const wchar_t *GroupName(int group) {
    constexpr const wchar_t *names[]{L"STICKS", L"TRIGGERS", L"BUTTONS", L"ADDITIONAL AXES",
                                     L"DIRECTION PADS", L"OTHER CONTROLS"};
    return names[group];
}
std::wstring Label(int slot) {
    constexpr const wchar_t *canonical[]{L"Left stick X", L"Left stick Y", L"Right stick X", L"Right stick Y",
        L"Left trigger", L"Right trigger", L"D-pad up", L"D-pad down", L"D-pad left", L"D-pad right",
        L"South / A / Cross", L"East / B / Circle", L"West / X / Square", L"North / Y / Triangle",
        L"Left shoulder", L"Right shoulder", L"View / Share", L"Menu / Options", L"Left stick press",
        L"Right stick press", L"Guide / Home", L"Touchpad"};
    if (slot < 22) return canonical[slot];
    const auto indexed = [slot](const wchar_t *prefix, int start) {
        return std::wstring(prefix) + L" " + std::to_wstring(slot - start + 1);
    };
    if (slot >= 32 && slot < 96) return indexed(L"Button", 32);
    if (slot >= 96 && slot < 160) return indexed(L"Axis", 96);
    if (slot >= 160 && slot < 192) return indexed(L"Trigger", 160);
    if (slot >= 192 && slot < 208) return indexed(L"Direction pad", 192);
    if (slot >= 208) return indexed(L"Value", 208);
    return L"Reserved " + std::to_wstring(slot);
}
std::wstring Value(int slot, int value, bool valid) {
    if (!valid) return L"\u2014";
    if (Button(slot)) return value ? L"ON" : L"OFF";
    if (slot >= 192 && slot < 208) {
        constexpr const wchar_t *directions[]{L"N", L"NE", L"E", L"SE", L"S", L"SW", L"W", L"NW"};
        if (value < 0) return L"CENTER";
        if (value < 8) return directions[value];
    }
    return std::to_wstring(value) + (Axis(slot) || Trigger(slot) ? L"%" : L"");
}
bool InputKnown(const Entry &entry, int slot) {
    return entry.InputValid && entry.InputAvailable[static_cast<std::size_t>(slot)];
}
bool OutputKnown(const Entry &entry, int slot) {
    return entry.OutputValid && entry.OutputAvailable[static_cast<std::size_t>(slot)];
}
bool Different(const Entry &entry, int slot) {
    return InputKnown(entry, slot) && OutputKnown(entry, slot) &&
           entry.Input[static_cast<std::size_t>(slot)] != entry.Output[static_cast<std::size_t>(slot)];
}
void Fonts(State &state) {
    for (auto &font : state.Fonts) { if (font) DeleteObject(font); font = nullptr; }
    state.Fonts[0] = CreateFontW(-state.S(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    state.Fonts[1] = CreateFontW(-state.S(14), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    state.Fonts[2] = CreateFontW(-state.S(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    state.Fonts[3] = CreateFontW(-state.S(11), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (state.Tabs) SendMessageW(state.Tabs, WM_SETFONT, reinterpret_cast<WPARAM>(state.Fonts[0]), FALSE);
}
HBRUSH Brush(State &state, COLORREF color) {
    return state.Brushes[color == Blue ? 0 : color == Black ? 1 : color == White ? 2 : 3];
}
void Fill(HDC dc, State &state, RECT rect, COLORREF color) { FillRect(dc, &rect, Brush(state, color)); }
void Text(HDC dc, State &state, RECT rect, const std::wstring &text, COLORREF color = White,
          int font = 0, UINT flags = DT_LEFT) {
    const auto old = SelectObject(dc, state.Fonts[static_cast<std::size_t>(font)]);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.data(), static_cast<int>(text.size()), &rect,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | flags);
    SelectObject(dc, old);
}
void InvalidateBody(State &state) {
    RECT rect{0, state.S(44), state.Width, state.Height};
    InvalidateRect(state.Window, &rect, FALSE);
}
void Scroll(State &state, int position) {
    const int page = std::max(0, state.ContentBottom - state.ContentTop);
    const int next = std::clamp(position, 0, std::max(0, state.Extent - page));
    if (next == state.Scroll) return;
    state.Scroll = next;
    SCROLLINFO info{sizeof(info), SIF_POS};
    info.nPos = next;
    SetScrollInfo(state.Window, SB_VERT, &info, TRUE);
    InvalidateBody(state);
    NotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE, state.Window, OBJID_CLIENT, CHILDID_SELF);
}
void Layout(State &state, bool rebuild) {
    RECT area{};
    GetClientRect(state.Window, &area);
    state.Width = area.right; state.Height = area.bottom;
    state.ContentTop = state.S(130);
    state.ContentBottom = std::max(state.ContentTop, state.Height - state.S(48));
    if (state.Tabs) {
        SetWindowPos(state.Tabs, nullptr, state.S(10), state.S(7), std::max(1, state.Width - state.S(20)),
                     state.S(38), SWP_NOACTIVATE | SWP_NOZORDER);
        const int count = static_cast<int>(state.Devices.size());
        const int width = std::clamp((std::max(state.S(120), state.Width - state.S(22))) /
                                       std::max(1, std::min(3, count)), state.S(100), state.S(170));
        SendMessageW(state.Tabs, TCM_SETITEMSIZE, 0, MAKELPARAM(width, state.S(33)));
    }
    if (rebuild) {
        state.Rows.clear();
        if (const auto *entry = state.Device()) {
            int y = 0;
            for (int group = 0; group < 6; ++group) {
                bool header = false;
                for (int slot = 0; slot < 256; ++slot) {
                    if (!entry->Seen[static_cast<std::size_t>(slot)] || Group(slot) != group) continue;
                    if (!header) {
                        state.Rows.push_back({-1, y, state.S(25), GroupName(group)});
                        y += state.S(25); header = true;
                    }
                    const int height = state.S(Axis(slot) || Trigger(slot) ? 36 : 30);
                    state.Rows.push_back({slot, y, height, Label(slot)});
                    y += height;
                }
            }
            state.Extent = y;
        } else state.Extent = 0;
        ++state.LayoutRevision;
    }
    const int page = std::max(0, state.ContentBottom - state.ContentTop);
    state.Scroll = std::clamp(state.Scroll, 0, std::max(0, state.Extent - page));
    SCROLLINFO info{sizeof(info), SIF_PAGE | SIF_POS | SIF_RANGE};
    info.nMin = 0; info.nMax = std::max(0, state.Extent - 1);
    info.nPage = static_cast<UINT>(page); info.nPos = state.Scroll;
    SetScrollInfo(state.Window, SB_VERT, &info, TRUE);
}
void Select(State &state, int index) {
    if (index < 0 || index >= static_cast<int>(state.Devices.size()) || index == state.Selected) return;
    state.Selected = index; state.Scroll = 0;
    TabCtrl_SetCurSel(state.Tabs, index);
    TabCtrl_SetCurFocus(state.Tabs, index);
    Layout(state, true);
    InvalidateRect(state.Tabs, nullptr, FALSE);
    InvalidateBody(state);
    NotifyWinEvent(EVENT_OBJECT_SELECTION, state.Tabs, OBJID_CLIENT, index + 1);
    NotifyWinEvent(EVENT_OBJECT_REORDER, state.Window, OBJID_CLIENT, CHILDID_SELF);
}
void DrawTabs(HDC dc, State &state, const RECT &client) {
    Fill(dc, state, client, Black);
    for (int index = 0; index < static_cast<int>(state.Devices.size()); ++index) {
        RECT rect{};
        if (!TabCtrl_GetItemRect(state.Tabs, index, &rect)) continue;
        const bool selected = index == state.Selected;
        if (!selected) { RECT line{rect.right - 1, rect.top + state.S(8), rect.right, rect.bottom - state.S(8)}; Fill(dc, state, line, Gray); }
        const auto &device = state.Devices[static_cast<std::size_t>(index)];
        std::wstring name = device.Name;
        const auto sameName = [&device](const Entry &candidate) { return candidate.Name == device.Name; };
        if (std::count_if(state.Devices.begin(), state.Devices.end(), sameName) > 1) {
            const auto sameKey = [&device](const Entry &candidate) { return candidate.Key == device.Key; };
            const auto identifier = std::count_if(state.Devices.begin(), state.Devices.end(), sameKey) > 1
                                        ? static_cast<std::uint32_t>(index) + 1 : device.Key;
            name += L" \u00b7 " + std::to_wstring(identifier);
        }
        RECT label{rect.left + state.S(9), rect.top, rect.right - state.S(9), rect.bottom - state.S(3)};
        Text(dc, state, label, name, White, selected ? 1 : 0);
        if (selected) {
            RECT line{rect.left + state.S(5), rect.bottom - state.S(3), rect.right - state.S(5), rect.bottom};
            Fill(dc, state, line, Blue);
        }
        if (GetFocus() == state.Tabs && TabCtrl_GetCurFocus(state.Tabs) == index) {
            InflateRect(&label, -state.S(2), -state.S(5)); DrawFocusRect(dc, &label);
        }
    }
}
LRESULT CALLBACK TabProcedure(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    auto *state = reinterpret_cast<State *>(ref);
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(w);
        RECT area{}; GetClientRect(window, &area);
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, area.right), std::max(1L, area.bottom));
        if (memory && bitmap) {
            const auto old = SelectObject(memory, bitmap);
            DrawTabs(memory, *state, area);
            BitBlt(dc, 0, 0, area.right, area.bottom, memory, 0, 0, SRCCOPY);
            SelectObject(memory, old);
        } else DrawTabs(dc, *state, area);
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (message == WM_PAINT) EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_MOUSEWHEEL) return SendMessageW(state->Window, message, w, l);
    if (message == WM_KEYDOWN && (w == VK_DOWN || w == VK_UP || w == VK_NEXT || w == VK_PRIOR ||
                                  w == VK_HOME || w == VK_END)) {
        SetFocus(state->Window);
        return SendMessageW(state->Window, message, w, l);
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) InvalidateRect(window, nullptr, FALSE);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, TabProcedure, TabSubclass);
    return DefSubclassProc(window, message, w, l);
}
void Cell(HDC dc, State &state, RECT rect, int slot, int value, bool known, bool changed) {
    const COLORREF color = changed ? Blue : known ? White : Gray;
    const auto text = Value(slot, value, known);
    if (Button(slot)) {
        RECT dot{rect.left + state.S(8), (rect.top + rect.bottom) / 2 - state.S(4),
                 rect.left + state.S(16), (rect.top + rect.bottom) / 2 + state.S(4)};
        if (known && value) Fill(dc, state, dot, changed ? Blue : White);
        else FrameRect(dc, &dot, Brush(state, Gray));
        rect.left += state.S(24);
        Text(dc, state, rect, text, color, 2, DT_LEFT);
        return;
    }
    RECT number{rect.left, rect.top, rect.right - state.S(4), rect.bottom - (Axis(slot) || Trigger(slot) ? state.S(7) : 0)};
    Text(dc, state, number, text, color, 2, DT_RIGHT);
    if (Axis(slot) || Trigger(slot)) {
        RECT track{rect.left + state.S(8), rect.bottom - state.S(8), rect.right - state.S(6), rect.bottom - state.S(6)};
        Fill(dc, state, track, Gray);
        if (Axis(slot)) {
            const int center = (track.left + track.right) / 2;
            if (known) {
                const int position = track.left + MulDiv(std::clamp(value, -100, 100) + 100, track.right - track.left, 200);
                RECT bar{std::min(center, position), track.top - state.S(1), std::max(center, position), track.bottom};
                Fill(dc, state, bar, changed ? Blue : White);
            }
            RECT tick{center, track.top - state.S(2), center + 1, track.bottom + state.S(2)};
            Fill(dc, state, tick, White);
        } else if (known) {
            RECT bar = track;
            bar.right = bar.left + MulDiv(std::clamp(value, 0, 100), track.right - track.left, 100);
            Fill(dc, state, bar, changed ? Blue : White);
        }
    }
}
std::array<int, 3> Columns(const State &state) {
    const int left = state.S(15), right = state.Width - state.S(15);
    const int label = std::clamp(MulDiv(right - left, 43, 100), state.S(86), state.S(162));
    return {left, left + label, left + label + (right - left - label) / 2};
}
void Draw(HDC dc, State &state, const RECT &area) {
    Fill(dc, state, area, Black);
    const auto *entry = state.Device();
    RECT subtitle{state.S(16), state.S(48), state.Width - state.S(16), state.S(71)};
    if (!entry) {
        Text(dc, state, subtitle, L"Live controller inputs", White, 1);
        RECT message{state.S(20), state.S(93), state.Width - state.S(20), state.S(140)};
        Text(dc, state, message, state.Monitoring ? L"Waiting for a controller report" : L"Live observation is unavailable", White, 1);
        message.top += state.S(41); message.bottom += state.S(41);
        Text(dc, state, message, L"No script is required to view IN and OUT.", Gray);
        message.top += state.S(30); message.bottom += state.S(30);
        Text(dc, state, message, state.Status, Gray);
        return;
    }
    Text(dc, state, subtitle, entry->Name, White, 1);
    RECT explanation{state.S(16), state.S(73), state.Width - state.S(16), state.S(96)};
    Text(dc, state, explanation, L"IN: physical report   OUT: driver output", Gray, 3);
    const auto columns = Columns(state);
    RECT label{columns[0], state.S(101), columns[1] - state.S(6), state.ContentTop - state.S(4)};
    Text(dc, state, label, L"CONTROL", Gray, 3);
    label.left = columns[1]; label.right = columns[2] - state.S(5); Text(dc, state, label, L"IN", White, 1, DT_RIGHT);
    label.left = columns[2]; label.right = state.Width - state.S(19); Text(dc, state, label, L"OUT", Blue, 1, DT_RIGHT);
    const int save = SaveDC(dc);
    IntersectClipRect(dc, 0, state.ContentTop, state.Width, state.ContentBottom);
    for (const auto &row : state.Rows) {
        RECT rect{columns[0], state.ContentTop + row.Top - state.Scroll, state.Width - state.S(15),
                  state.ContentTop + row.Top + row.Height - state.Scroll};
        if (rect.bottom <= state.ContentTop || rect.top >= state.ContentBottom) continue;
        if (row.Slot < 0) {
            RECT line{rect.left, rect.bottom - 1, rect.right, rect.bottom};
            Fill(dc, state, line, Gray); Text(dc, state, rect, row.Label, Gray, 3); continue;
        }
        const auto slot = static_cast<std::size_t>(row.Slot);
        RECT name{rect.left, rect.top, columns[1] - state.S(8), rect.bottom};
        Text(dc, state, name, row.Label);
        const bool changed = Different(*entry, row.Slot);
        RECT input{columns[1], rect.top, columns[2] - state.S(3), rect.bottom};
        RECT output{columns[2] + state.S(3), rect.top, rect.right, rect.bottom};
        Cell(dc, state, input, row.Slot, entry->Input[slot], InputKnown(*entry, row.Slot), false);
        Cell(dc, state, output, row.Slot, entry->Output[slot], OutputKnown(*entry, row.Slot), changed);
        if (changed) {
            RECT indicator{columns[2], rect.top + state.S(7), columns[2] + state.S(2), rect.bottom - state.S(7)};
            Fill(dc, state, indicator, Blue);
        }
    }
    if (state.Rows.empty()) {
        RECT pending{state.S(16), state.ContentTop, state.Width - state.S(16), state.ContentTop + state.S(50)};
        Text(dc, state, pending, L"Waiting for decoded controls\u2026", Gray);
    }
    RestoreDC(dc, save);
    RECT footer{state.S(15), state.ContentBottom + state.S(5), state.Width - state.S(15), state.Height - state.S(24)};
    Text(dc, state, footer, L"\u2014 Not measured   \u00b7   Blue OUT = modified", Gray, 3);
    footer.top = state.Height - state.S(24); footer.bottom = state.Height - state.S(3);
    const std::wstring sequences = L"IN #" + (entry->InputValid ? std::to_wstring(entry->InputSequence) : L"\u2014") +
                                 L"   OUT #" + (entry->OutputValid ? std::to_wstring(entry->OutputSequence) : L"\u2014");
    Text(dc, state, footer, sequences, Gray, 2);
    if (GetFocus() == state.Window) {
        RECT focus{state.S(3), state.ContentTop, state.Width - state.S(3), state.ContentBottom};
        DrawFocusRect(dc, &focus);
    }
}
void Paint(State &state, HDC destination, const RECT &area) {
    const int width = std::max(1, state.Width), height = std::max(1, state.Height);
    if (!state.Buffer) state.Buffer = CreateCompatibleDC(destination);
    if (state.Buffer && (width != state.BufferWidth || height != state.BufferHeight)) {
        const auto replacement = CreateCompatibleBitmap(destination, width, height);
        if (replacement) {
            const auto old = static_cast<HBITMAP>(SelectObject(state.Buffer, replacement));
            if (!state.PreviousBitmap) state.PreviousBitmap = old;
            if (state.Bitmap) DeleteObject(state.Bitmap);
            state.Bitmap = replacement; state.BufferWidth = width; state.BufferHeight = height;
        }
    }
    if (state.Buffer && state.Bitmap) {
        // Transparent, anti-aliased glyphs must never be redrawn onto their
        // previous pixels outside the erased dirty region. Clipping the
        // persistent buffer prevents gradual text darkening on quiet rows.
        const int saved = SaveDC(state.Buffer);
        IntersectClipRect(state.Buffer, area.left, area.top, area.right, area.bottom);
        Draw(state.Buffer, state, area);
        RestoreDC(state.Buffer, saved);
        BitBlt(destination, area.left, area.top, area.right - area.left, area.bottom - area.top,
               state.Buffer, area.left, area.top, SRCCOPY);
    } else Draw(destination, state, area);
    ++state.PaintCount;
}
ControllerMonitorState Inspect(State &state) {
    ControllerMonitorState result;
    result.SelectedIndex = state.Selected; result.DeviceCount = static_cast<int>(state.Devices.size());
    result.ScrollPosition = state.Scroll; result.ScrollExtent = state.Extent;
    result.LayoutRevision = state.LayoutRevision; result.PaintCount = state.PaintCount; result.Tabs = state.Tabs;
    if (const auto *entry = state.Device()) {
        result.SelectedToken = entry->Token; result.SelectedKey = entry->Key; result.SelectedFamily = entry->Family;
        for (const auto &row : state.Rows) {
            if (row.Slot < 0) continue;
            ControllerMonitorRow view;
            view.Slot = row.Slot; view.Label = row.Label;
            view.InputKnown = InputKnown(*entry, row.Slot); view.OutputKnown = OutputKnown(*entry, row.Slot);
            view.Different = Different(*entry, row.Slot);
            view.InputText = Value(row.Slot, entry->Input[static_cast<std::size_t>(row.Slot)], view.InputKnown);
            view.OutputText = Value(row.Slot, entry->Output[static_cast<std::size_t>(row.Slot)], view.OutputKnown);
            view.Bounds = {state.S(15), state.ContentTop + row.Top - state.Scroll, state.Width - state.S(15),
                           state.ContentTop + row.Top + row.Height - state.Scroll};
            result.Rows.push_back(std::move(view));
        }
    }
    return result;
}
class Accessible final : public IAccessible {
    LONG references_ = 1;
    HWND window_ = nullptr;
    bool Child(VARIANT child, ControllerMonitorState &state, int &index) const {
        if (child.vt != VT_I4 || !IsWindow(window_) || !Get(window_)) return false;
        state = Inspect(*Get(window_)); index = child.lVal - 2;
        return child.lVal == CHILDID_SELF || child.lVal == 1 ||
               (index >= 0 && index < static_cast<int>(state.Rows.size()));
    }
    static HRESULT String(const std::wstring &value, BSTR *out) {
        if (!out) return E_POINTER;
        *out = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
        return *out || value.empty() ? S_OK : E_OUTOFMEMORY;
    }
  public:
    explicit Accessible(HWND window) : window_(window) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDispatch || iid == IID_IAccessible) {
            *out = static_cast<IAccessible *>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto count = InterlockedDecrement(&references_); if (!count) delete this;
        return static_cast<ULONG>(count);
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT *out) override { if (!out) return E_POINTER; *out = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo **) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR *, UINT, LCID, DISPID *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS *, VARIANT *, EXCEPINFO *, UINT *) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE get_accParent(IDispatch **out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (!IsWindow(window_)) return CO_E_OBJNOTCONNECTED;
        return AccessibleObjectFromWindow(GetParent(window_), static_cast<DWORD>(OBJID_CLIENT), IID_IDispatch, reinterpret_cast<void **>(out));
    }
    HRESULT STDMETHODCALLTYPE get_accChildCount(long *out) override {
        if (!out) return E_POINTER; *out = 0;
        if (IsWindow(window_) && Get(window_)) *out = static_cast<long>(Inspect(*Get(window_)).Rows.size()) + 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accChild(VARIANT child, IDispatch **out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (child.vt == VT_I4 && child.lVal == 1 && IsWindow(window_) && Get(window_))
            return AccessibleObjectFromWindow(Get(window_)->Tabs, static_cast<DWORD>(OBJID_CLIENT),
                                              IID_IDispatch, reinterpret_cast<void **>(out));
        return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accName(VARIANT child, BSTR *out) override {
        ControllerMonitorState state; int index = -1; if (!Child(child, state, index)) return E_INVALIDARG;
        return String(index == -2 ? L"Controller IN and OUT. Arrow keys select controllers; Page Up and Page Down scroll controls."
                      : index == -1 ? L"Controllers" : state.Rows[static_cast<std::size_t>(index)].Label, out);
    }
    HRESULT STDMETHODCALLTYPE get_accValue(VARIANT child, BSTR *out) override {
        ControllerMonitorState state; int index = -1; if (!Child(child, state, index)) return E_INVALIDARG;
        if (index < 0) return String(L"Controller " + std::to_wstring(state.SelectedKey) + L", " + Family(state.SelectedFamily), out);
        const auto &row = state.Rows[static_cast<std::size_t>(index)];
        return String(L"IN " + (row.InputKnown ? row.InputText : L"not measured") + L"; OUT " +
                      (row.OutputKnown ? row.OutputText : L"not measured") + (row.Different ? L"; modified" : L""), out);
    }
    HRESULT STDMETHODCALLTYPE get_accDescription(VARIANT, BSTR *out) override {
        return String(L"IN is the last physical driver report. OUT is the independently observed report released by the driver. Blue marks an OUT value that differs from IN.", out);
    }
    HRESULT STDMETHODCALLTYPE get_accRole(VARIANT child, VARIANT *out) override {
        if (!out) return E_POINTER; VariantInit(out); out->vt = VT_I4;
        out->lVal = child.vt == VT_I4 && child.lVal == CHILDID_SELF ? ROLE_SYSTEM_PANE
                    : child.vt == VT_I4 && child.lVal == 1 ? ROLE_SYSTEM_PAGETABLIST : ROLE_SYSTEM_STATICTEXT; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accState(VARIANT child, VARIANT *out) override {
        if (!out) return E_POINTER;
        ControllerMonitorState view; int index = -1; if (!Child(child, view, index)) return E_INVALIDARG;
        VariantInit(out); out->vt = VT_I4; out->lVal = STATE_SYSTEM_READONLY;
        if (!IsWindowVisible(window_)) out->lVal |= STATE_SYSTEM_INVISIBLE;
        if (index < 0) {
            out->lVal |= STATE_SYSTEM_FOCUSABLE;
            if (GetFocus() == (index == -1 ? view.Tabs : window_)) out->lVal |= STATE_SYSTEM_FOCUSED;
        } else {
            const auto *state = Get(window_); const auto &bounds = view.Rows[static_cast<std::size_t>(index)].Bounds;
            if (bounds.bottom <= state->ContentTop || bounds.top >= state->ContentBottom) out->lVal |= STATE_SYSTEM_OFFSCREEN;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accHelp(VARIANT, BSTR *out) override { if (!out) return E_POINTER; *out = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE get_accHelpTopic(BSTR *out, VARIANT, long *topic) override {
        if (out) *out = nullptr; if (topic) *topic = 0; return S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE get_accKeyboardShortcut(VARIANT, BSTR *out) override { return String(L"Left and Right arrows", out); }
    HRESULT STDMETHODCALLTYPE get_accFocus(VARIANT *out) override {
        if (!out) return E_POINTER; VariantInit(out);
        if (GetFocus() == window_) { out->vt = VT_I4; out->lVal = CHILDID_SELF; }
        else if (IsWindow(window_) && Get(window_) && GetFocus() == Get(window_)->Tabs) { out->vt = VT_I4; out->lVal = 1; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_accSelection(VARIANT *out) override { if (!out) return E_POINTER; VariantInit(out); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_accDefaultAction(VARIANT, BSTR *out) override { if (!out) return E_POINTER; *out = nullptr; return S_FALSE; }
    HRESULT STDMETHODCALLTYPE accSelect(long flags, VARIANT child) override {
        if (child.vt != VT_I4 || (child.lVal != CHILDID_SELF && child.lVal != 1) || !(flags & SELFLAG_TAKEFOCUS)) return E_INVALIDARG;
        if (!IsWindow(window_) || !Get(window_)) return CO_E_OBJNOTCONNECTED;
        SetFocus(child.lVal == 1 ? Get(window_)->Tabs : window_); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accLocation(long *x, long *y, long *width, long *height, VARIANT child) override {
        if (!x || !y || !width || !height) return E_POINTER;
        ControllerMonitorState view; int index = -1; if (!Child(child, view, index)) return E_INVALIDARG;
        RECT bounds{}; GetClientRect(window_, &bounds);
        if (index == -1) {
            GetWindowRect(view.Tabs, &bounds); *x = bounds.left; *y = bounds.top;
            *width = bounds.right - bounds.left; *height = bounds.bottom - bounds.top; return S_OK;
        }
        if (index >= 0) bounds = view.Rows[static_cast<std::size_t>(index)].Bounds;
        POINT origin{bounds.left, bounds.top}; ClientToScreen(window_, &origin);
        *x = origin.x; *y = origin.y; *width = bounds.right - bounds.left; *height = bounds.bottom - bounds.top; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accNavigate(long direction, VARIANT start, VARIANT *out) override {
        if (!out) return E_POINTER; VariantInit(out);
        ControllerMonitorState view; int index = -1; if (!Child(start, view, index)) return E_INVALIDARG;
        if (direction == NAVDIR_FIRSTCHILD) index = -1;
        else if (direction == NAVDIR_LASTCHILD) index = static_cast<int>(view.Rows.size()) - 1;
        else if (direction == NAVDIR_NEXT || direction == NAVDIR_DOWN) ++index;
        else if (direction == NAVDIR_PREVIOUS || direction == NAVDIR_UP) --index;
        else return S_FALSE;
        if (index < -1 || index >= static_cast<int>(view.Rows.size())) return S_FALSE;
        out->vt = VT_I4; out->lVal = index + 2; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accHitTest(long x, long y, VARIANT *out) override {
        if (!out) return E_POINTER; VariantInit(out); if (!IsWindow(window_) || !Get(window_)) return S_FALSE;
        POINT point{x, y}; ScreenToClient(window_, &point); RECT area{}; GetClientRect(window_, &area);
        if (!PtInRect(&area, point)) return S_FALSE;
        out->vt = VT_I4; out->lVal = CHILDID_SELF;
        const auto view = Inspect(*Get(window_));
        RECT tabs{}; GetWindowRect(view.Tabs, &tabs);
        POINT screen{x, y};
        if (PtInRect(&tabs, screen)) { out->lVal = 1; return S_OK; }
        const auto *state = Get(window_);
        if (point.y < state->ContentTop || point.y >= state->ContentBottom) return S_OK;
        for (std::size_t index = 0; index < view.Rows.size(); ++index)
            if (PtInRect(&view.Rows[index].Bounds, point)) { out->lVal = static_cast<long>(index) + 2; break; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE accDoDefaultAction(VARIANT) override { return DISP_E_MEMBERNOTFOUND; }
    HRESULT STDMETHODCALLTYPE put_accName(VARIANT, BSTR) override { return E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE put_accValue(VARIANT, BSTR) override { return E_ACCESSDENIED; }
};
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto *state = Get(window);
    if (message == WM_NCCREATE) {
        const auto *create = reinterpret_cast<CREATESTRUCTW *>(l);
        auto *creation = static_cast<Creation *>(create->lpCreateParams);
        state = creation->Value; creation->Adopted = true; state->Window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, w, l);
    switch (message) {
    case WM_CREATE: {
        state->Dpi = GetDpiForWindow(window); if (!state->Dpi) state->Dpi = 96;
        const std::array<COLORREF, 4> colors{Blue, Black, White, Gray};
        for (std::size_t i = 0; i < colors.size(); ++i) state->Brushes[i] = CreateSolidBrush(colors[i]);
        state->Tabs = CreateWindowExW(0, WC_TABCONTROLW, L"Controllers", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            TCS_SINGLELINE | TCS_FIXEDWIDTH | TCS_FOCUSONBUTTONDOWN | TCS_TOOLTIPS, 0, 0, 1, 1, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(TabsId)), reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE)), nullptr);
        if (!state->Tabs) return -1;
        SetWindowTheme(state->Tabs, L"", L"");
        SetWindowSubclass(state->Tabs, TabProcedure, TabSubclass, reinterpret_cast<DWORD_PTR>(state));
        Fonts(*state); Layout(*state, true); return 0;
    }
    case WM_SIZE: Layout(*state, false); InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; const auto dc = BeginPaint(window, &paint); Paint(*state, dc, paint.rcPaint); EndPaint(window, &paint); return 0;
    }
    case WM_PRINTCLIENT: {
        RECT area{}; GetClientRect(window, &area); Paint(*state, reinterpret_cast<HDC>(w), area); return 0;
    }
    case WM_GETOBJECT:
        if (static_cast<LONG>(l) == OBJID_CLIENT) {
            auto *accessible = new (std::nothrow) Accessible(window);
            if (!accessible) return 0;
            const auto result = LresultFromObject(IID_IAccessible, w, accessible); accessible->Release(); return result;
        }
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR *>(l)->hwndFrom == state->Tabs && reinterpret_cast<NMHDR *>(l)->code == TCN_SELCHANGE) {
            Select(*state, TabCtrl_GetCurSel(state->Tabs)); return 0;
        }
        break;
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_KEYDOWN:
        if (w == VK_LEFT || w == VK_RIGHT) {
            const int count = static_cast<int>(state->Devices.size());
            if (count) Select(*state, (state->Selected + (w == VK_RIGHT ? 1 : count - 1)) % count);
        } else if (w == VK_UP || w == VK_DOWN) Scroll(*state, state->Scroll + state->S(w == VK_DOWN ? 36 : -36));
        else if (w == VK_NEXT || w == VK_PRIOR) Scroll(*state, state->Scroll + (w == VK_NEXT ? 1 : -1) * (state->ContentBottom - state->ContentTop));
        else if (w == VK_HOME) Scroll(*state, 0);
        else if (w == VK_END) Scroll(*state, INT_MAX);
        else break;
        return 0;
    case WM_MOUSEWHEEL: {
        UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        state->WheelRemainder += GET_WHEEL_DELTA_WPARAM(w);
        const int steps = state->WheelRemainder / WHEEL_DELTA;
        state->WheelRemainder %= WHEEL_DELTA;
        const int amount = lines == WHEEL_PAGESCROLL ? state->ContentBottom - state->ContentTop : state->S(30) * static_cast<int>(std::min(lines, 100u));
        Scroll(*state, state->Scroll - steps * amount); return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_TRACKPOS | SIF_POS}; GetScrollInfo(window, SB_VERT, &info);
        switch (LOWORD(w)) {
        case SB_TOP: Scroll(*state, 0); break;
        case SB_BOTTOM: Scroll(*state, INT_MAX); break;
        case SB_LINEUP: Scroll(*state, state->Scroll - state->S(30)); break;
        case SB_LINEDOWN: Scroll(*state, state->Scroll + state->S(30)); break;
        case SB_PAGEUP: Scroll(*state, state->Scroll - (state->ContentBottom - state->ContentTop)); break;
        case SB_PAGEDOWN: Scroll(*state, state->Scroll + (state->ContentBottom - state->ContentTop)); break;
        case SB_THUMBTRACK: Scroll(*state, info.nTrackPos); break;
        case SB_THUMBPOSITION: Scroll(*state, info.nPos); break;
        default: break;
        }
        return 0;
    }
    case WM_LBUTTONDOWN: SetFocus(window); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_NCDESTROY:
        if (state->Tabs && IsWindow(state->Tabs)) RemoveWindowSubclass(state->Tabs, TabProcedure, TabSubclass);
        SetWindowLongPtrW(window, GWLP_USERDATA, 0); delete state; return DefWindowProcW(window, message, w, l);
    default: break;
    }
    return DefWindowProcW(window, message, w, l);
}
} // namespace
HWND CreateControllerMonitorView(HWND parent, int id, HINSTANCE instance) {
    static std::once_flag registration;
    static bool registered = false;
    std::call_once(registration, [instance] {
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_TAB_CLASSES}; InitCommonControlsEx(&common);
        WNDCLASSW type{}; type.lpfnWndProc = Procedure; type.hInstance = instance; type.lpszClassName = ViewClass;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        registered = RegisterClassW(&type) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    });
    if (!registered) return nullptr;
    auto *state = new (std::nothrow) State;
    if (!state) return nullptr;
    Creation creation{state};
    // WM_NCDESTROY owns state as soon as WM_NCCREATE succeeds. A failure
    // before that message still has to release the unadopted allocation.
    const auto window = CreateWindowExW(WS_EX_CONTROLPARENT, ViewClass, L"Controller IN and OUT",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_TABSTOP | WS_VSCROLL, 0, 0, 1, 1, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, &creation);
    if (!creation.Adopted) delete state;
    return window;
}
void UpdateControllerMonitorView(HWND window, const ServiceSnapshot &snapshot) {
    auto *state = Get(window); if (!state) return;
    std::string selectedToken; std::uint32_t selectedKey = 0, selectedFamily = 0;
    const int oldSelection = state->Selected;
    if (const auto *selected = state->Device()) { selectedToken = selected->Token; selectedKey = selected->Key; selectedFamily = selected->Family; }
    std::vector<Entry> next;
    next.reserve(snapshot.Devices.size());
    bool tabsChanged = snapshot.Devices.size() != state->Devices.size();
    bool layoutChanged = false;
    int selection = -1;
    for (const auto &source : snapshot.Devices) {
        auto found = std::find_if(state->Devices.begin(), state->Devices.end(), [&](const Entry &entry) {
            return Matches(entry, source.Token, source.Key, source.Family);
        });
        Entry entry;
        if (found != state->Devices.end()) entry.Seen = found->Seen;
        entry.Token = source.Token; entry.Key = source.Key; entry.Family = source.Family;
        entry.Name = source.Name.empty() ? std::wstring(Family(source.Family)) + L" " + std::to_wstring(source.Key) : Wide(source.Name);
        entry.Input = source.Values; entry.Output = source.OutputValues;
        entry.InputAvailable = source.InputAvailable; entry.OutputAvailable = source.OutputAvailable;
        // Never substitute accepted/queued script output for measured egress.
        entry.InputValid = source.Telemetry && source.PhysicalValid;
        entry.OutputValid = source.Telemetry && source.OutputValid;
        entry.InputSequence = source.InputSequence; entry.OutputSequence = source.OutputSequence;
        for (std::size_t slot = 0; slot < entry.Seen.size(); ++slot)
            entry.Seen[slot] = entry.Seen[slot] || source.Available[slot] || source.InputAvailable[slot] || source.OutputAvailable[slot];
        const auto index = next.size();
        if (index >= state->Devices.size() || !Matches(state->Devices[index], source.Token, source.Key, source.Family) ||
            state->Devices[index].Name != entry.Name) tabsChanged = true;
        if (oldSelection >= 0 && Matches(entry, selectedToken, selectedKey, selectedFamily)) {
            selection = static_cast<int>(index);
            layoutChanged = found == state->Devices.end() || entry.Seen != found->Seen;
        }
        next.push_back(std::move(entry));
    }
    if (selection < 0 && !next.empty()) selection = std::min(std::max(0, oldSelection), static_cast<int>(next.size()) - 1);
    const bool selectedChanged = selection < 0 ? oldSelection >= 0 :
        oldSelection < 0 || !Matches(next[static_cast<std::size_t>(selection)], selectedToken, selectedKey, selectedFamily);
    std::array<bool, 256> changedSlots{};
    bool footerChanged = false, nameChanged = false;
    if (!selectedChanged && selection >= 0) {
        const auto &before = *state->Device();
        const auto &after = next[static_cast<std::size_t>(selection)];
        nameChanged = before.Name != after.Name;
        footerChanged = before.InputValid != after.InputValid || before.OutputValid != after.OutputValid ||
                        before.InputSequence != after.InputSequence || before.OutputSequence != after.OutputSequence;
        for (int slot = 0; slot < 256; ++slot) {
            const auto index = static_cast<std::size_t>(slot);
            changedSlots[index] = InputKnown(before, slot) != InputKnown(after, slot) ||
                OutputKnown(before, slot) != OutputKnown(after, slot) ||
                (InputKnown(after, slot) && before.Input[index] != after.Input[index]) ||
                (OutputKnown(after, slot) && before.Output[index] != after.Output[index]);
        }
    }
    const bool emptyChanged = selection < 0 && (state->Monitoring != snapshot.Monitoring ||
        state->Status != Wide(snapshot.MonitorError.empty() ? snapshot.MonitorStatus : snapshot.MonitorError));
    state->Devices = std::move(next); state->Selected = selection;
    state->Monitoring = snapshot.Monitoring;
    state->Status = Wide(snapshot.MonitorError.empty() ? snapshot.MonitorStatus : snapshot.MonitorError);
    if (tabsChanged) {
        SendMessageW(state->Tabs, WM_SETREDRAW, FALSE, 0);
        TabCtrl_DeleteAllItems(state->Tabs);
        for (std::size_t i = 0; i < state->Devices.size(); ++i) {
            auto &entry = state->Devices[i];
            std::wstring accessibleName = entry.Name + L" (" + Family(entry.Family) + L", controller " + std::to_wstring(entry.Key) + L")";
            TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = accessibleName.data();
            TabCtrl_InsertItem(state->Tabs, static_cast<int>(i), &item);
        }
        TabCtrl_SetCurSel(state->Tabs, selection);
        if (selection >= 0) TabCtrl_SetCurFocus(state->Tabs, selection);
        SendMessageW(state->Tabs, WM_SETREDRAW, TRUE, 0); InvalidateRect(state->Tabs, nullptr, FALSE);
    }
    if (selectedChanged) state->Scroll = 0;
    if (tabsChanged || selectedChanged || layoutChanged) {
        Layout(*state, selectedChanged || layoutChanged || state->Rows.empty());
        NotifyWinEvent(EVENT_OBJECT_REORDER, window, OBJID_CLIENT, CHILDID_SELF);
    }
    if (selectedChanged || layoutChanged || nameChanged || tabsChanged || emptyChanged) InvalidateBody(*state);
    else {
        for (const auto &row : state->Rows) {
            if (row.Slot < 0 || !changedSlots[static_cast<std::size_t>(row.Slot)]) continue;
            RECT rect{0, state->ContentTop + row.Top - state->Scroll, state->Width,
                       state->ContentTop + row.Top + row.Height - state->Scroll};
            rect.top = std::max(static_cast<LONG>(state->ContentTop), rect.top);
            rect.bottom = std::min(static_cast<LONG>(state->ContentBottom), rect.bottom);
            if (rect.top < rect.bottom) InvalidateRect(window, &rect, FALSE);
        }
        if (footerChanged) {
            RECT rect{0, state->ContentBottom, state->Width, state->Height}; InvalidateRect(window, &rect, FALSE);
        }
    }
}
void SetControllerMonitorDpi(HWND window, UINT dpi) {
    auto *state = Get(window); if (!state) return;
    if (!dpi) dpi = GetDpiForWindow(window);
    if (!dpi) dpi = 96;
    if (dpi == state->Dpi) return;
    const UINT previous = state->Dpi; state->Dpi = dpi;
    state->Scroll = MulDiv(state->Scroll, static_cast<int>(dpi), static_cast<int>(previous));
    Fonts(*state); Layout(*state, true); InvalidateRect(window, nullptr, FALSE); InvalidateRect(state->Tabs, nullptr, FALSE);
}
bool SelectControllerMonitorDevice(HWND window, const std::string &token, std::uint32_t key, std::uint32_t family) {
    auto *state = Get(window); if (!state) return false;
    for (std::size_t index = 0; index < state->Devices.size(); ++index)
        if (Matches(state->Devices[index], token, key, family)) { Select(*state, static_cast<int>(index)); return true; }
    return false;
}
ControllerMonitorState InspectControllerMonitorView(HWND window) {
    if (auto *state = Get(window)) return Inspect(*state);
    return {};
}
} // namespace gpr::ui
