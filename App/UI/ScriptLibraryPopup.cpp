#include "ScriptLibraryPopup.h"
#include "NativeWidgets.h"
#include <dwmapi.h>
#include <algorithm>
#include <cwctype>
#include <limits>
#include <numeric>
#include <utility>

namespace gpr::ui {
namespace {
constexpr wchar_t PopupClass[] = L"GprScriptLibraryPopup";
constexpr wchar_t ListClass[] = L"GprScriptLibraryList";
constexpr UINT_PTR SearchSubclass = 0x4750524c;
constexpr int SearchId = 101;

int Compare(const std::wstring &a, const std::wstring &b) {
    if (a.size() > INT_MAX || b.size() > INT_MAX)
        return a.compare(b);
    const int result = CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                            static_cast<int>(b.size()), TRUE);
    return result ? result - CSTR_EQUAL : a.compare(b);
}
std::wstring Lower(std::wstring value) {
    if (value.size() <= MAXDWORD && !value.empty())
        CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}
bool SamePath(const std::filesystem::path &a, const std::filesystem::path &b) {
    return !a.empty() && !b.empty() && Compare(a.lexically_normal().native(), b.lexically_normal().native()) == 0;
}
std::wstring Label(const ScriptLibraryEntry &entry) {
    return entry.Label.empty() ? entry.Path.stem().wstring() : entry.Label;
}
std::wstring Group(const ScriptLibraryEntry &entry) {
    return entry.Group.empty() ? L"Scripts" : entry.Group;
}
struct Row {
    int Top = 0, Height = 0;
    std::optional<std::size_t> Entry;
    std::wstring Heading;
};
struct Library {
    HWND Window = nullptr, Owner = nullptr, Search = nullptr, List = nullptr;
    const std::vector<ScriptLibraryEntry> &Entries;
    const std::filesystem::path &Current;
    const std::wstring &TitleText;
    std::vector<std::size_t> Matches;
    std::vector<Row> Rows;
    std::optional<std::size_t> Selected, Result;
    HFONT Body = nullptr, Heading = nullptr, Title = nullptr;
    HBRUSH Background = CreateSolidBrush(Black);
    HBRUSH Accent = CreateSolidBrush(Blue);
    HPEN BorderPen = CreatePen(PS_SOLID, 1, Gray);
    UINT Dpi = 96;
    int Scroll = 0, ContentHeight = 0;
    int RegionWidth = 0, RegionHeight = 0;
    UINT RegionDpi = 0;
    bool Done = false, Animation = false, RoundedFallback = false;

    Library(HWND owner, const std::vector<ScriptLibraryEntry> &entries,
            const std::filesystem::path &current, const std::wstring &title)
        : Owner(owner), Entries(entries), Current(current), TitleText(title) {}
    ~Library() {
        if (Window && IsWindow(Window))
            DestroyWindow(Window);
        for (auto font : {Body, Heading, Title})
            if (font)
                DeleteObject(font);
        for (auto brush : {Background, Accent})
            if (brush)
                DeleteObject(brush);
        if (BorderPen)
            DeleteObject(BorderPen);
    }
    int S(int value) const { return MulDiv(value, static_cast<int>(Dpi), 96); }
    void UpdateRegion() {
        if (!RoundedFallback || !Window)
            return;
        RECT area{};
        if (!GetWindowRect(Window, &area))
            return;
        const int width = area.right - area.left, height = area.bottom - area.top;
        if (RegionWidth == width && RegionHeight == height && RegionDpi == Dpi)
            return;
        const auto region = CreateRoundRectRgn(0, 0, width + 1, height + 1, S(16), S(16));
        if (!region)
            return;
        RegionWidth = width;
        RegionHeight = height;
        RegionDpi = Dpi;
        // Windows owns a region only after SetWindowRgn succeeds.
        if (!SetWindowRgn(Window, region, TRUE)) {
            DeleteObject(region);
            RegionWidth = RegionHeight = 0;
            RegionDpi = 0;
        }
    }
    void Fonts() {
        for (auto font : {Body, Heading, Title})
            if (font)
                DeleteObject(font);
        Body = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, L"Segoe UI");
        Heading = CreateFontW(-S(11), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
        Title = CreateFontW(-S(17), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
        if (Search) {
            SendMessageW(Search, WM_SETFONT, reinterpret_cast<WPARAM>(Body), TRUE);
            SetDpi(Search, Dpi);
        }
        if (List)
            SetDpi(List, Dpi);
    }
    void Finish(std::optional<std::size_t> selected = std::nullopt) {
        if (Done)
            return;
        Result = selected;
        Done = true;
    }
    int ViewHeight() const {
        RECT area{};
        GetClientRect(List, &area);
        return area.bottom;
    }
    void ScrollTo(int value) {
        Scroll = std::clamp(value, 0, std::max(0, ContentHeight - ViewHeight()));
        SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
        info.nMax = std::max(0, ContentHeight - 1);
        info.nPage = static_cast<UINT>(std::max(0, ViewHeight()));
        info.nPos = Scroll;
        SetScrollInfo(List, SB_VERT, &info, TRUE);
        InvalidateRect(List, nullptr, FALSE);
    }
    void Reveal() {
        if (!Selected)
            return;
        for (const auto &row : Rows) {
            if (row.Entry != Selected)
                continue;
            if (row.Top < Scroll)
                ScrollTo(row.Top);
            else if (row.Top + row.Height > Scroll + ViewHeight())
                ScrollTo(row.Top + row.Height - ViewHeight());
            break;
        }
    }
    void Rebuild(bool preserve = true) {
        std::wstring query(static_cast<std::size_t>(GetWindowTextLengthW(Search)) + 1, L'\0');
        query.resize(static_cast<std::size_t>(GetWindowTextW(Search, query.data(), static_cast<int>(query.size()))));
        Matches = detail::FilterScriptLibraryEntries(Entries, query);
        const auto old = Selected;
        Selected.reset();
        if (preserve && old && std::find(Matches.begin(), Matches.end(), *old) != Matches.end())
            Selected = old;
        if (!Selected && !Matches.empty()) {
            Selected = Matches.front();
            for (auto index : Matches)
                if (SamePath(Entries[index].Path, Current)) {
                    Selected = index;
                    break;
                }
        }
        Rows.clear();
        ContentHeight = S(6);
        std::wstring previous;
        bool first = true;
        for (auto index : Matches) {
            const auto group = Group(Entries[index]);
            if (first || Compare(group, previous) != 0) {
                Rows.push_back({ContentHeight, S(29), std::nullopt, group});
                ContentHeight += S(29);
                previous = group;
                first = false;
            }
            Rows.push_back({ContentHeight, S(43), index, {}});
            ContentHeight += S(43);
        }
        ContentHeight += S(6);
        ScrollTo(0);
        Reveal();
        InvalidateRect(Window, nullptr, FALSE);
    }
    void Layout() {
        if (!Search || !List)
            return;
        RECT area{};
        GetClientRect(Window, &area);
        const int inset = S(16), searchY = S(53), searchHeight = S(36), footer = S(35);
        MoveWindow(Search, inset, searchY, std::max<int>(1, area.right - 2 * inset), searchHeight, TRUE);
        MoveWindow(List, S(6), searchY + searchHeight + S(8), std::max<int>(1, area.right - S(12)),
                   std::max<int>(1, area.bottom - searchY - searchHeight - S(8) - footer), TRUE);
        ScrollTo(Scroll);
    }
    bool Key(WPARAM key) {
        if (key == VK_ESCAPE) {
            Finish();
            return true;
        }
        if (key == VK_RETURN) {
            if (Selected)
                Finish(Selected);
            return true;
        }
        if (key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT) {
            if (Matches.empty())
                return true;
            auto found = Selected ? std::find(Matches.begin(), Matches.end(), *Selected) : Matches.end();
            int index = found == Matches.end() ? 0 : static_cast<int>(found - Matches.begin());
            const int page = std::max(1, ViewHeight() / S(43) - 1);
            index += key == VK_UP ? -1 : key == VK_DOWN ? 1 : key == VK_PRIOR ? -page : page;
            index = std::clamp(index, 0, static_cast<int>(Matches.size() - 1));
            Selected = Matches[static_cast<std::size_t>(index)];
            Reveal();
            InvalidateRect(List, nullptr, FALSE);
            return true;
        }
        return false;
    }
    void Caption(HDC dc, const std::wstring &text, RECT area, HFONT font, COLORREF colour,
                 UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX) {
        SelectObject(dc, font);
        SetTextColor(dc, colour);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, text.c_str(), static_cast<int>(std::min<std::size_t>(text.size(), INT_MAX)), &area, flags);
    }
    void Paint(HWND window, bool list, HDC supplied = nullptr) {
        PAINTSTRUCT paint{};
        const HDC target = supplied ? supplied : BeginPaint(window, &paint);
        RECT area{};
        GetClientRect(window, &area);
        const HDC dc = CreateCompatibleDC(target);
        const HBITMAP bitmap = CreateCompatibleBitmap(target, std::max(1L, area.right), std::max(1L, area.bottom));
        const auto previous = SelectObject(dc, bitmap);
        FillRect(dc, &area, Background);
        if (list) {
            if (Matches.empty()) {
                RECT message{S(22), S(20), area.right - S(22), S(70)};
                Caption(dc, Entries.empty() ? L"No scripts in your library yet." : L"No matching scripts.",
                        message, Body, Gray);
            }
            for (const auto &row : Rows) {
                RECT bounds{S(8), row.Top - Scroll, area.right - S(8), row.Top + row.Height - Scroll};
                if (bounds.bottom < 0 || bounds.top >= area.bottom)
                    continue;
                if (!row.Entry) {
                    bounds.left += S(10);
                    Caption(dc, row.Heading, bounds, Heading, Gray);
                    continue;
                }
                const auto index = *row.Entry;
                const bool active = Selected == row.Entry;
                const bool current = SamePath(Entries[index].Path, Current);
                if (active) {
                    const auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
                    const auto oldBrush = SelectObject(dc, Accent);
                    RoundRect(dc, bounds.left, bounds.top + S(2), bounds.right, bounds.bottom - S(2), S(12), S(12));
                    SelectObject(dc, oldBrush);
                    SelectObject(dc, oldPen);
                }
                if (current) {
                    RECT marker{bounds.left + S(10), bounds.top + S(16), bounds.left + S(14), bounds.top + S(27)};
                    FillRect(dc, &marker, active ? reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)) : Accent);
                }
                bounds.left += S(24);
                if (current) {
                    RECT tag = bounds;
                    tag.left = tag.right - S(49);
                    Caption(dc, L"OPEN", tag, Heading, active ? White : Gray, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                    bounds.right -= S(62);
                }
                Caption(dc, Label(Entries[index]), bounds, Body, White);
            }
        } else {
            const auto oldPen = SelectObject(dc, BorderPen);
            const auto oldBrush = SelectObject(dc, Background);
            RoundRect(dc, 0, 0, area.right, area.bottom, S(16), S(16));
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            RECT title{S(17), S(12), area.right - S(76), S(43)};
            Caption(dc, TitleText, title, Title, White);
            RECT count{area.right - S(79), S(16), area.right - S(18), S(42)};
            Caption(dc, std::to_wstring(Matches.size()), count, Heading, Gray, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            RECT footer{S(17), area.bottom - S(34), area.right - S(15), area.bottom - S(5)};
            Caption(dc, L"\x2191\x2193 Browse    Enter Open    Esc Close", footer, Heading, Gray);
        }
        BitBlt(target, 0, 0, area.right, area.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
        if (!supplied)
            EndPaint(window, &paint);
    }
};

LRESULT CALLBACK SearchProcedure(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR reference) {
    auto &state = *reinterpret_cast<Library *>(reference);
    if (message == WM_GETDLGCODE)
        return DefSubclassProc(window, message, w, l) | DLGC_WANTALLKEYS;
    if (message == WM_KEYDOWN && w == VK_TAB) {
        SetFocus(state.List);
        return 0;
    }
    if (message == WM_KEYDOWN && state.Key(w))
        return 0;
    if (message == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE))
        return 0;
    return DefSubclassProc(window, message, w, l);
}
LRESULT CALLBACK ListProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto *state = reinterpret_cast<Library *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<Library *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state)
        return DefWindowProcW(window, message, w, l);
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: state->Paint(window, true); return 0;
    case WM_PRINTCLIENT: state->Paint(window, true, reinterpret_cast<HDC>(w)); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (w == VK_TAB) {
            SetFocus(state->Search);
            return 0;
        }
        if (state->Key(w))
            return 0;
        break;
    case WM_CHAR:
        if (w >= L' ' && w != 127) {
            SetFocus(state->Search);
            SendMessageW(state->Search, message, w, l);
        }
        return 0;
    case WM_MOUSEWHEEL:
        state->ScrollTo(state->Scroll - GET_WHEEL_DELTA_WPARAM(w) * state->S(43) * 3 / WHEEL_DELTA);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_POS | SIF_TRACKPOS};
        GetScrollInfo(window, SB_VERT, &info);
        const int code = LOWORD(w);
        const int amount = code == SB_LINEUP ? -state->S(43) : code == SB_LINEDOWN ? state->S(43)
            : code == SB_PAGEUP ? -state->ViewHeight() : code == SB_PAGEDOWN ? state->ViewHeight() : 0;
        state->ScrollTo(code == SB_THUMBPOSITION ? info.nPos : code == SB_THUMBTRACK ? info.nTrackPos
                        : code == SB_TOP ? 0 : code == SB_BOTTOM ? state->ContentHeight : state->Scroll + amount);
        return 0;
    }
    case WM_LBUTTONUP: {
        const int y = static_cast<short>(HIWORD(l)) + state->Scroll;
        for (const auto &row : state->Rows)
            if (row.Entry && y >= row.Top && y < row.Top + row.Height) {
                state->Selected = row.Entry;
                state->Finish(row.Entry);
                return 0;
            }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int y = static_cast<short>(HIWORD(l)) + state->Scroll;
        for (const auto &row : state->Rows)
            if (row.Entry && y >= row.Top && y < row.Top + row.Height && state->Selected != row.Entry) {
                state->Selected = row.Entry;
                InvalidateRect(window, nullptr, FALSE);
                break;
            }
        return 0;
    }
    }
    return DefWindowProcW(window, message, w, l);
}
LRESULT CALLBACK PopupProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto *state = reinterpret_cast<Library *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<Library *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        state->Window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state)
        return DefWindowProcW(window, message, w, l);
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: state->Paint(window, false); return 0;
    case WM_PRINTCLIENT: state->Paint(window, false, reinterpret_cast<HDC>(w)); return 0;
    case WM_SIZE: state->UpdateRegion(); state->Layout(); return 0;
    case WM_CLOSE: state->Finish(); return 0;
    case WM_DESTROY: state->Done = true; state->Window = nullptr; return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE)
            state->Finish();
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == IDCANCEL) {
            state->Finish();
            return 0;
        }
        if (LOWORD(w) == SearchId && HIWORD(w) == EN_CHANGE)
            state->Rebuild();
        return 0;
    case WM_MOUSEWHEEL:
        SendMessageW(state->List, message, w, l);
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(w), White);
        SetBkColor(reinterpret_cast<HDC>(w), Black);
        return reinterpret_cast<LRESULT>(state->Background);
    case WM_KEYDOWN:
        if (state->Key(w))
            return 0;
        break;
    case WM_DPICHANGED: {
        state->Dpi = HIWORD(w);
        state->Fonts();
        const auto rectangle = reinterpret_cast<RECT *>(l);
        SetWindowPos(window, nullptr, rectangle->left, rectangle->top,
                     rectangle->right - rectangle->left, rectangle->bottom - rectangle->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        state->Rebuild();
        state->UpdateRegion();
        return 0;
    }
    }
    return DefWindowProcW(window, message, w, l);
}
void RegisterClasses() {
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW type{};
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.style = CS_DROPSHADOW;
    type.lpszClassName = PopupClass;
    type.lpfnWndProc = PopupProcedure;
    RegisterClassW(&type);
    type.style = 0;
    type.lpszClassName = ListClass;
    type.lpfnWndProc = ListProcedure;
    RegisterClassW(&type);
}
bool OutsideMouseMessage(UINT message) {
    return message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN ||
           message == WM_XBUTTONDOWN || message == WM_NCLBUTTONDOWN || message == WM_NCRBUTTONDOWN ||
           message == WM_NCMBUTTONDOWN || message == WM_NCXBUTTONDOWN;
}
} // namespace

std::vector<std::size_t> detail::FilterScriptLibraryEntries(
    const std::vector<ScriptLibraryEntry> &entries, const std::wstring &query) {
    std::vector<std::wstring> words;
    for (std::size_t offset = 0; offset < query.size();) {
        while (offset < query.size() && iswspace(query[offset]))
            ++offset;
        const auto begin = offset;
        while (offset < query.size() && !iswspace(query[offset]))
            ++offset;
        if (offset != begin)
            words.push_back(Lower(query.substr(begin, offset - begin)));
    }
    std::vector<std::size_t> result;
    result.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto text = Lower(Label(entries[index]) + L" " + Group(entries[index]) + L" " + entries[index].Path.filename().wstring());
        if (std::all_of(words.begin(), words.end(), [&text](const auto &word) { return text.find(word) != std::wstring::npos; }))
            result.push_back(index);
    }
    std::stable_sort(result.begin(), result.end(), [&entries](std::size_t a, std::size_t b) {
        const bool aExample = Compare(Group(entries[a]), L"Examples") == 0;
        const bool bExample = Compare(Group(entries[b]), L"Examples") == 0;
        if (aExample != bExample)
            return !aExample;
        const auto group = Compare(Group(entries[a]), Group(entries[b]));
        if (group)
            return group < 0;
        const auto label = Compare(Label(entries[a]), Label(entries[b]));
        return label ? label < 0 : Compare(entries[a].Path.native(), entries[b].Path.native()) < 0;
    });
    return result;
}

std::optional<std::size_t> ShowScriptLibraryPopup(
    HWND owner, HWND anchor, const std::vector<ScriptLibraryEntry> &entries,
    const std::filesystem::path &current, const std::wstring &title, bool animate) {
    if (!IsWindow(owner) || !IsWindow(anchor))
        return std::nullopt;
    RegisterClasses();
    Library state(owner, entries, current, title);
    state.Dpi = GetDpiForWindow(anchor);
    if (!state.Dpi)
        state.Dpi = 96;
    BOOL animation = FALSE;
    state.Animation = animate && SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0) && animation;
    RECT button{};
    GetWindowRect(anchor, &button);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&button, MONITOR_DEFAULTTONEAREST), &monitor))
        return std::nullopt;
    const RECT work = monitor.rcWork;
    const int margin = state.S(8);
    const int width = std::max(1, std::min<int>(state.S(430), work.right - work.left - 2 * margin));
    const auto ordered = detail::FilterScriptLibraryEntries(entries, L"");
    std::size_t groups = 0;
    std::wstring previousGroup;
    for (auto index : ordered) {
        const auto group = Group(entries[index]);
        if (!groups || Compare(previousGroup, group) != 0) {
            ++groups;
            previousGroup = group;
        }
    }
    const int desiredHeight = 136 + static_cast<int>(std::min<std::size_t>(ordered.size(), 10)) * 43 +
                              static_cast<int>(std::min<std::size_t>(groups, 10)) * 29;
    const int height = std::max(1, std::min<int>(state.S(std::clamp(desiredHeight, 235, 540)),
                                           work.bottom - work.top - 2 * margin));
    const int x = std::clamp<int>(button.left, work.left + margin, std::max(work.left + margin, work.right - width - margin));
    int y = button.bottom + state.S(6);
    if (y + height > work.bottom - margin && button.top - state.S(6) - height >= work.top + margin)
        y = button.top - state.S(6) - height;
    y = std::clamp<int>(y, work.top + margin, std::max(work.top + margin, work.bottom - height - margin));
    const auto instance = GetModuleHandleW(nullptr);
    if (!CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, PopupClass, title.c_str(),
                         WS_POPUP | WS_CLIPCHILDREN, x, y, width, height, owner, nullptr, instance, &state))
        return std::nullopt;
    const DWORD rounded = 2;
    state.RoundedFallback = FAILED(DwmSetWindowAttribute(state.Window, 33, &rounded, sizeof(rounded)));
    state.UpdateRegion();
    state.Search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                   0, 0, 0, 0, state.Window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(SearchId)), instance, nullptr);
    state.List = CreateWindowExW(0, ListClass, L"Scripts", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL,
                                 0, 0, 0, 0, state.Window, nullptr, instance, &state);
    if (!state.Search || !state.List)
        return std::nullopt;
    StyleEdit(state.Search);
    StyleScrollbars(state.List);
    SetAnimationsEnabled(state.Search, state.Animation);
    SetAnimationsEnabled(state.List, state.Animation);
    SetAccessibleName(state.Search, L"Search scripts by name or group");
    SetAccessibleName(state.List, L"Script library. Arrow keys browse. Enter opens. Escape closes.");
    SendMessageW(state.Search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search..."));
    SendMessageW(state.Search, EM_SETLIMITTEXT, 512, 0);
    if (!SetWindowSubclass(state.Search, SearchProcedure, SearchSubclass, reinterpret_cast<DWORD_PTR>(&state)))
        return std::nullopt;
    state.Fonts();
    state.Layout();
    state.Rebuild(false);
    const HWND previousFocus = GetFocus();
    if (!state.Animation || !AnimateWindow(state.Window, 95, AW_BLEND | AW_ACTIVATE))
        ShowWindow(state.Window, SW_SHOW);
    SetFocus(state.Search);
    MSG message{};
    bool quit = false;
    while (!state.Done && IsWindow(state.Window)) {
        const int result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            quit = result == 0;
            state.Finish();
            break;
        }
        if (OutsideMouseMessage(message.message) && message.hwnd != state.Window && !IsChild(state.Window, message.hwnd)) {
            state.Finish();
            // Consume the anchor's mouse-down to make a second click close,
            // rather than closing and immediately opening the library again.
            if (message.hwnd == anchor || IsChild(anchor, message.hwnd))
                break;
        }
        if (!IsDialogMessageW(state.Window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        // A continuously reporting controller can keep posted owner messages
        // ahead of low-priority WM_PAINT. Flush only already-dirty popup areas,
        // so search/selection still appear immediately while inputs stream.
        if (!state.Done && state.Window)
            RedrawWindow(state.Window, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
    const HWND focus = GetFocus();
    const bool focusInPopup = state.Window && (focus == state.Window || IsChild(state.Window, focus));
    // A click outside can already have focused a different parent control.
    // Dismissing the library must not undo that deliberate focus change.
    const bool restoreFocus = focusInPopup &&
        (GetForegroundWindow() == state.Window || GetForegroundWindow() == owner);
    if (state.Window && IsWindow(state.Window)) {
        if (state.Animation && IsWindowVisible(owner))
            AnimateWindow(state.Window, 65, AW_HIDE | AW_BLEND);
        DestroyWindow(state.Window);
    }
    if (restoreFocus && IsWindow(previousFocus) && IsWindowEnabled(previousFocus))
        SetFocus(previousFocus);
    if (quit)
        PostQuitMessage(static_cast<int>(message.wParam));
    return state.Result;
}
} // namespace gpr::ui
