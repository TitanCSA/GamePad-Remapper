#include "ScriptPanel.h"
#include "ScriptStorage.h"
#include "NativeWidgets.h"
#include "Theme.h"
#include <CommCtrl.h>
#include <windowsx.h>
#include <algorithm>
#include <charconv>
#include <string>

namespace gpr {
namespace {
LRESULT CALLBACK RevealOnFocus(HWND window, UINT message, WPARAM a, LPARAM b, UINT_PTR, DWORD_PTR) {
    if (message == WM_SETFOCUS)
        SendMessageW(GetParent(window), WM_APP + 1, 0, reinterpret_cast<LPARAM>(window));
    return DefSubclassProc(window, message, a, b);
}
int Steps(const ScriptParameter &p) {
    return static_cast<int>(
        std::min<std::int64_t>(10000, (static_cast<std::int64_t>(p.Maximum) - p.Minimum) / p.Step));
}
int Position(const ScriptParameter &p, int value) {
    const auto count = (static_cast<std::int64_t>(p.Maximum) - p.Minimum) / p.Step;
    return count ? static_cast<int>((static_cast<std::int64_t>(value) - p.Minimum) / p.Step * Steps(p) /
                                    count)
                 : 0;
}
int Value(const ScriptParameter &p, int position) {
    const auto count = (static_cast<std::int64_t>(p.Maximum) - p.Minimum) / p.Step;
    return Steps(p) ? static_cast<int>(p.Minimum +
                                       (static_cast<std::int64_t>(position) * count / Steps(p)) * p.Step)
                    : p.Minimum;
}
} // namespace
ScriptPanel::ScriptPanel(HINSTANCE instance, HWND parent, std::function<void(std::size_t, int)> changed,
                         std::function<void(const std::string &)> error)
    : changed_(std::move(changed)), error_(std::move(error)) {
    WNDCLASSW type{};
    type.lpfnWndProc = Procedure;
    type.hInstance = instance;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = L"GprScriptParameters";
    RegisterClassW(&type);
    background_ = CreateSolidBrush(ui::Black);
    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName, L"Script parameters",
                              WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 1, 1, parent, nullptr,
                              instance, this);
    if (!window_) {
        DeleteObject(background_);
        background_ = nullptr;
        throw std::runtime_error("Could not create the script parameter panel.");
    }
    ui::StyleScrollbars(window_);
    SetDpi(GetDpiForWindow(parent));
}
ScriptPanel::~ScriptPanel() {
    if (IsWindow(window_))
        DestroyWindow(window_);
    if (font_)
        DeleteObject(font_);
    if (heading_)
        DeleteObject(heading_);
    if (background_)
        DeleteObject(background_);
}
void ScriptPanel::SetDpi(UINT dpi) {
    dpi_ = dpi ? dpi : 96;
    if (font_)
        DeleteObject(font_);
    if (heading_)
        DeleteObject(heading_);
    const auto make = [&](int size, int weight) {
        return CreateFontW(-MulDiv(size, static_cast<int>(dpi_), 96), 0, 0, 0, weight, FALSE, FALSE,
                           FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    font_ = make(15, FW_NORMAL);
    heading_ = make(20, FW_SEMIBOLD);
    Rebuild();
}
void ScriptPanel::Set(const ScriptInterface &schema, const ParameterValues &values) {
    schema.Validate(values);
    schema_ = schema;
    values_ = values;
    scroll_ = 0;
    Rebuild();
}
void ScriptPanel::Rebuild() {
    building_ = true;
    for (auto child : children_)
        DestroyWindow(child);
    children_.clear();
    inputs_.clear();
    readouts_.clear();
    tops_.clear();
    heights_.clear();
    int y = 12;
    const auto create = [&](const wchar_t *kind, const std::wstring &text, DWORD style, int id,
                            int height, bool heading = false) {
        HWND child = CreateWindowExW(0, kind, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
                                     window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                     GetModuleHandleW(nullptr), nullptr);
        if (!child)
            throw std::runtime_error("Could not create a script interface control.");
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(heading ? heading_ : font_), FALSE);
        children_.push_back(child);
        tops_.push_back(y);
        heights_.push_back(height);
        y += height + 8;
        ui::SetDpi(child, dpi_);
        return child;
    };
    create(L"STATIC", Utf16(schema_.Title.empty() ? "Script settings" : schema_.Title), SS_LEFT, 0, 32,
           true);
    if (!schema_.Description.empty())
        create(L"STATIC", Utf16(schema_.Description), SS_LEFT, 0, 44);
    if (schema_.Parameters.empty())
        create(L"STATIC",
               L"This script has no custom settings.\nUse Edit script to add a ui block, or apply it "
               L"with its existing behavior.",
               SS_LEFT, 0, 80);
    std::string group;
    for (std::size_t i = 0; i < schema_.Parameters.size(); ++i) {
        const auto &p = schema_.Parameters[i];
        if (p.Group != group) {
            group = p.Group;
            if (!group.empty()) {
                y += 12;
                create(L"STATIC", Utf16(group), SS_LEFT, 0, 24, true);
            }
        }
        const int labelTop = y;
        if (p.Kind != ParameterKind::Toggle)
            create(L"STATIC", Utf16(p.Label), SS_LEFT, 0, 26);
        HWND input = nullptr, readout = nullptr;
        const int id = FirstInput + static_cast<int>(i);
        if (p.Kind == ParameterKind::Toggle) {
            input = create(L"BUTTON", Utf16(p.Label), BS_AUTOCHECKBOX | WS_TABSTOP, id, 36);
            ui::StyleToggle(input);
            SendMessageW(input, BM_SETCHECK, values_[i] ? BST_CHECKED : BST_UNCHECKED, 0);
        } else if (p.Kind == ParameterKind::Select) {
            input = create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id, 38);
            ui::StyleComboBox(input);
            for (std::size_t c = 0; c < p.Choices.size(); ++c) {
                SendMessageW(input, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(Utf16(p.Choices[c].Label).c_str()));
                if (p.Choices[c].Value == values_[i])
                    SendMessageW(input, CB_SETCURSEL, c, 0);
            }
        } else if (p.Kind == ParameterKind::Slider) {
            readout = create(L"STATIC", Utf16(std::to_string(values_[i]) + p.Unit), SS_RIGHT, 0, 24);
            tops_.back() = labelTop;
            y -= 32;
            input = create(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, id, 38);
            SendMessageW(input, TBM_SETRANGEMIN, FALSE, 0);
            SendMessageW(input, TBM_SETRANGEMAX, FALSE, Steps(p));
            SendMessageW(input, TBM_SETPOS, TRUE, Position(p, values_[i]));
        } else {
            input = create(L"EDIT", std::to_wstring(values_[i]), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                           id, 36);
            ui::StyleEdit(input);
            SendMessageW(input, EM_SETLIMITTEXT, 12, 0);
            readout = create(L"STATIC",
                             Utf16(std::to_string(p.Minimum) + " .. " + std::to_string(p.Maximum) +
                                   p.Unit + " | step " + std::to_string(p.Step)),
                             SS_LEFT, 0, 28);
        }
        ui::SetAccessibleName(input, Utf16(p.Label).c_str());
        SetWindowSubclass(input, RevealOnFocus, 1, 0);
        inputs_.push_back(input);
        readouts_.push_back(readout);
        if (!p.Help.empty())
            create(L"STATIC", Utf16(p.Help), SS_LEFT, 0, 36);
        y += 12;
    }
    contentHeight_ = y;
    building_ = false;
    Layout();
}
void ScriptPanel::Layout() {
    RECT r{};
    GetClientRect(window_, &r);
    const int height = MulDiv(r.bottom, 96, static_cast<int>(dpi_));
    scroll_ = std::clamp(scroll_, 0, std::max(0, contentHeight_ - height));
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMax = std::max(0, contentHeight_ - 1);
    info.nPage = static_cast<UINT>(std::max(1, height));
    info.nPos = scroll_;
    SetScrollInfo(window_, SB_VERT, &info, TRUE);
    for (std::size_t i = 0; i < children_.size(); ++i) {
        wchar_t type[32]{};
        GetClassNameW(children_[i], type, 32);
        const int h = _wcsicmp(L"COMBOBOX", type) == 0 ? 280 : heights_[i];
        bool readout = false, sliderLabel = false;
        for (std::size_t p = 0; p < readouts_.size(); ++p)
            if (schema_.Parameters[p].Kind == ParameterKind::Slider) {
                readout |= children_[i] == readouts_[p];
                sliderLabel |= i + 1 < children_.size() && children_[i + 1] == readouts_[p];
            }
        const int pad = MulDiv(12, static_cast<int>(dpi_), 96),
                  extra = MulDiv(120, static_cast<int>(dpi_), 96);
        const int full = static_cast<int>(r.right) - MulDiv(40, static_cast<int>(dpi_), 96);
        MoveWindow(children_[i], readout ? pad + full - extra : pad,
                   MulDiv(tops_[i] - scroll_, static_cast<int>(dpi_), 96),
                   std::max(1, readout       ? extra
                               : sliderLabel ? full - extra
                                             : full),
                   MulDiv(h, static_cast<int>(dpi_), 96), TRUE);
    }
    InvalidateRect(window_, nullptr, FALSE);
}
void ScriptPanel::Scroll(int position) {
    scroll_ = position;
    Layout();
}
void ScriptPanel::Changed(std::size_t i, int value) {
    if (building_ || !schema_.Parameters[i].Accepts(value) || values_[i] == value)
        return;
    values_[i] = value;
    if (readouts_[i] && schema_.Parameters[i].Kind == ParameterKind::Slider)
        SetWindowTextW(readouts_[i], Utf16(std::to_string(value) + schema_.Parameters[i].Unit).c_str());
    changed_(i, value);
}
LRESULT CALLBACK ScriptPanel::Procedure(HWND w, UINT message, WPARAM a, LPARAM b) {
    auto self = reinterpret_cast<ScriptPanel *>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<ScriptPanel *>(reinterpret_cast<CREATESTRUCTW *>(b)->lpCreateParams);
        self->window_ = w;
        SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self)
        try {
            return self->Message(message, a, b);
        } catch (const std::exception &e) {
            try {
                self->error_(e.what());
            } catch (...) {
            }
            return 0;
        }
    return DefWindowProcW(w, message, a, b);
}
LRESULT ScriptPanel::Message(UINT message, WPARAM a, LPARAM b) {
    switch (message) {
    case WM_APP + 1: {
        if (building_)
            return 0;
        RECT child{}, client{};
        GetWindowRect(reinterpret_cast<HWND>(b), &child);
        MapWindowPoints(nullptr, window_, reinterpret_cast<POINT *>(&child), 2);
        GetClientRect(window_, &client);
        if (child.top < 0)
            Scroll(scroll_ + MulDiv(child.top, 96, static_cast<int>(dpi_)) - 8);
        else if (child.bottom > client.bottom)
            Scroll(scroll_ + MulDiv(child.bottom - client.bottom, 96, static_cast<int>(dpi_)) + 8);
        return 0;
    }
    case WM_SIZE:
        Layout();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window_, &paint);
        FillRect(dc, &paint.rcPaint, background_);
        EndPaint(window_, &paint);
        return 0;
    }
    case WM_PRINTCLIENT: {
        RECT r{};
        GetClientRect(window_, &r);
        FillRect(reinterpret_cast<HDC>(a), &r, background_);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT r{};
        GetClientRect(window_, &r);
        FillRect(reinterpret_cast<HDC>(a), &r, background_);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(a), ui::White);
        SetBkColor(reinterpret_cast<HDC>(a), ui::Black);
        return reinterpret_cast<LRESULT>(background_);
    case WM_DRAWITEM:
        if (ui::DrawItem(*reinterpret_cast<DRAWITEMSTRUCT *>(b)))
            return TRUE;
        break;
    case WM_NOTIFY: {
        const auto header = reinterpret_cast<NMHDR *>(b);
        const auto id = static_cast<int>(header->idFrom) - FirstInput;
        if (header->code == NM_CUSTOMDRAW && id >= 0 &&
            static_cast<std::size_t>(id) < schema_.Parameters.size() &&
            schema_.Parameters[id].Kind == ParameterKind::Slider) {
            const auto draw = reinterpret_cast<NMCUSTOMDRAW *>(b);
            if (draw->dwDrawStage == CDDS_PREPAINT) {
                RECT area{}, channel{}, thumb{};
                GetClientRect(header->hwndFrom, &area);
                FillRect(draw->hdc, &area, background_);
                SendMessageW(header->hwndFrom, TBM_GETCHANNELRECT, 0,
                             reinterpret_cast<LPARAM>(&channel));
                SendMessageW(header->hwndFrom, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
                const int center = (thumb.left + thumb.right) / 2, cy = (thumb.top + thumb.bottom) / 2;
                channel.top = cy - 2;
                channel.bottom = cy + 2;
                auto brush = CreateSolidBrush(ui::Gray);
                FillRect(draw->hdc, &channel, brush);
                DeleteObject(brush);
                channel.right = center;
                brush = CreateSolidBrush(ui::Blue);
                FillRect(draw->hdc, &channel, brush);
                DeleteObject(brush);
                brush = CreateSolidBrush(IsWindowEnabled(header->hwndFrom) ? ui::White : ui::Gray);
                const auto oldBrush = SelectObject(draw->hdc, brush),
                           oldPen = SelectObject(draw->hdc, GetStockObject(NULL_PEN));
                const int radius = MulDiv(9, static_cast<int>(dpi_), 96);
                Ellipse(draw->hdc, center - radius, cy - radius, center + radius, cy + radius);
                SelectObject(draw->hdc, oldPen);
                SelectObject(draw->hdc, oldBrush);
                DeleteObject(brush);
                if (GetFocus() == header->hwndFrom) {
                    brush = CreateSolidBrush(ui::Blue);
                    FrameRect(draw->hdc, &area, brush);
                    DeleteObject(brush);
                }
                return CDRF_SKIPDEFAULT;
            }
        }
        break;
    }
    case WM_MEASUREITEM:
        reinterpret_cast<MEASUREITEMSTRUCT *>(b)->itemHeight =
            static_cast<UINT>(MulDiv(30, static_cast<int>(dpi_), 96));
        return TRUE;
    case WM_HSCROLL: {
        if (building_)
            return 0;
        const auto id = GetDlgCtrlID(reinterpret_cast<HWND>(b)) - FirstInput;
        if (id >= 0 && static_cast<std::size_t>(id) < inputs_.size())
            Changed(static_cast<std::size_t>(id),
                    Value(schema_.Parameters[id],
                          static_cast<int>(SendMessageW(inputs_[id], TBM_GETPOS, 0, 0))));
        return 0;
    }
    case WM_COMMAND: {
        const auto id = LOWORD(a) - FirstInput;
        if (building_ || id < 0 || static_cast<std::size_t>(id) >= inputs_.size())
            return 0;
        const auto &p = schema_.Parameters[id];
        if (p.Kind == ParameterKind::Toggle && HIWORD(a) == BN_CLICKED)
            Changed(id, SendMessageW(inputs_[id], BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0);
        else if (p.Kind == ParameterKind::Select &&
                 (HIWORD(a) == CBN_SELENDOK || (HIWORD(a) == CBN_SELCHANGE &&
                                                !SendMessageW(inputs_[id], CB_GETDROPPEDSTATE, 0, 0)))) {
            const auto index = static_cast<std::size_t>(SendMessageW(inputs_[id], CB_GETCURSEL, 0, 0));
            if (index < p.Choices.size())
                Changed(id, p.Choices[index].Value);
        } else if (p.Kind == ParameterKind::Number && HIWORD(a) == EN_CHANGE) {
            wchar_t text[32]{};
            GetWindowTextW(inputs_[id], text, 32);
            const auto narrow = Utf8(text);
            int value = 0;
            const auto parsed = std::from_chars(narrow.data(), narrow.data() + narrow.size(), value);
            const bool valid = parsed.ec == std::errc{} && parsed.ptr == narrow.data() + narrow.size() &&
                               p.Accepts(value);
            SetWindowTextW(
                readouts_[id],
                Utf16(valid ? std::to_string(p.Minimum) + " .. " + std::to_string(p.Maximum) + p.Unit
                            : "Enter a value in range with step " + std::to_string(p.Step))
                    .c_str());
            if (valid)
                Changed(id, value);
        }
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_TRACKPOS};
        GetScrollInfo(window_, SB_VERT, &info);
        int pos = scroll_;
        switch (LOWORD(a)) {
        case SB_LINEUP:
            pos -= 36;
            break;
        case SB_LINEDOWN:
            pos += 36;
            break;
        case SB_PAGEUP:
            pos -= 240;
            break;
        case SB_PAGEDOWN:
            pos += 240;
            break;
        case SB_THUMBTRACK:
            pos = info.nTrackPos;
            break;
        case SB_TOP:
            pos = 0;
            break;
        case SB_BOTTOM:
            pos = contentHeight_;
            break;
        }
        Scroll(pos);
        return 0;
    }
    case WM_MOUSEWHEEL:
        Scroll(scroll_ + (GET_WHEEL_DELTA_WPARAM(a) > 0 ? -96 : 96));
        return 0;
    }
    return DefWindowProcW(window_, message, a, b);
}
} // namespace gpr
