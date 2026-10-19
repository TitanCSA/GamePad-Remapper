#include "MainWindow.h"
#include "ScriptStorage.h"
#include "../Core/VisualDocument.h"
#include "../Capture/Service.h"
#include "../ThirdParty/json.hpp"
#include <CommCtrl.h>
#include <CommDlg.h>
#include <Richedit.h>
#include <Shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>

namespace gpr {
namespace {
constexpr COLORREF Background = RGB(10, 16, 32), Panel = RGB(17, 26, 46), Raised = RGB(25, 37, 59),
                   Edge = RGB(45, 60, 86), Ink = RGB(237, 242, 254), Muted = RGB(145, 161, 189),
                   Accent = RGB(167, 154, 255), Mint = RGB(126, 231, 206), Danger = RGB(255, 158, 170);
constexpr UINT CompiledMessage = WM_APP + 20, OperationMessage = WM_APP + 21;
enum Id {
    Library = 100,
    NewFile,
    OpenFile,
    Folder,
    Recover,
    Save,
    SaveAs,
    CodeTab,
    VisualTab,
    FindText,
    FindNext,
    Check,
    Inject,
    Stop,
    Live,
    Driver,
    Reduce,
    Code,
    Monitor,
    Copy,
    VisualList,
    AddRule,
    AddMacro,
    Infinity,
    AddConstant,
    RemoveItem,
    Form,
    TriggerTree,
    AddCondition,
    RemoveCondition,
    Steps,
    AddStep,
    RemoveStep,
    UpStep,
    DownStep,
    Property = 1000
};
enum Field {
    Name = 1000,
    Enabled,
    Kind,
    Source,
    Target,
    Value,
    Activation,
    ConditionKind,
    ConditionControl,
    ConditionOperator,
    ConditionValue,
    ConditionTime,
    Expression,
    StepControl,
    StepValue,
    StepDuration,
    StepBlend,
    Shape,
    TargetX,
    TargetY,
    Width,
    Height,
    Period,
    Tick,
    Blend,
    Center
};
enum class RowKind { Rule, Macro, Constant };
struct Row {
    RowKind Kind;
    std::size_t Index;
};
std::wstring Text(HWND window) {
    const int count = GetWindowTextLengthW(window);
    std::wstring value(static_cast<std::size_t>(count) + 1, L'\0');
    const int read = GetWindowTextW(window, value.data(), count + 1);
    value.resize(static_cast<std::size_t>(std::max(0, read)));
    return value;
}
void SetText(HWND window, const std::wstring &text) {
    if (Text(window) != text)
        SetWindowTextW(window, text.c_str());
}
std::wstring Quote(const std::wstring &value) { return L"\"" + value + L"\""; }
std::wstring Argument(const std::vector<std::wstring> &args, const std::wstring &option) {
    const auto it = std::find(args.begin(), args.end(), option);
    return it != args.end() && std::next(it) != args.end() ? *std::next(it) : L"";
}
bool Has(const std::vector<std::wstring> &args, const std::wstring &value) {
    return std::find(args.begin(), args.end(), value) != args.end();
}
bool Administrator() {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID group = nullptr;
    BOOL member = FALSE;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0,
                                 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &member);
        FreeSid(group);
    }
    return member != FALSE;
}
std::filesystem::path ExecutableDirectory() {
    std::wstring value(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
    value.resize(count);
    return std::filesystem::path(value).parent_path();
}
void Clipboard(HWND owner, const std::wstring &text) {
    if (!OpenClipboard(owner))
        throw std::runtime_error("The clipboard is busy. Try again.");
    const SIZE_T size = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!data) {
        CloseClipboard();
        throw std::bad_alloc();
    }
    void *memory = GlobalLock(data);
    if (!memory) {
        GlobalFree(data);
        CloseClipboard();
        throw std::bad_alloc();
    }
    memcpy(memory, text.c_str(), size);
    GlobalUnlock(data);
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, data)) {
        GlobalFree(data);
        CloseClipboard();
        throw std::runtime_error("Clipboard copy failed.");
    }
    CloseClipboard();
}
struct CompileResult {
    std::uint64_t Version = 0;
    std::string Source, Error;
    std::shared_ptr<const ScriptProgram> Program;
};
class CompilerWorker final {
    HWND window_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
    std::optional<std::pair<std::uint64_t, std::string>> pending_;
    std::thread thread_;
    void Run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            wake_.wait(lock, [this] { return stop_ || pending_.has_value(); });
            if (stop_)
                break;
            const auto version = pending_->first;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
            if (wake_.wait_until(lock, deadline, [this, version] {
                    return stop_ || !pending_ || pending_->first != version;
                }))
                continue;
            auto request = std::move(*pending_);
            pending_.reset();
            lock.unlock();
            auto result = std::make_unique<CompileResult>();
            result->Version = request.first;
            result->Source = std::move(request.second);
            try {
                result->Program = ScriptCompiler::Compile(result->Source);
            } catch (const ScriptException &error) {
                result->Error = error.Diagnostic();
            } catch (const std::exception &error) {
                result->Error = error.what();
            }
            lock.lock();
            if (!stop_ && (!pending_ || pending_->first == result->Version)) {
                if (PostMessageW(window_, CompiledMessage, 0, reinterpret_cast<LPARAM>(result.get())))
                    result.release();
            }
        }
    }

  public:
    explicit CompilerWorker(HWND window) : window_(window), thread_([this] { Run(); }) {}
    ~CompilerWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_one();
        if (thread_.joinable())
            thread_.join();
    }
    void Submit(std::uint64_t version, std::string source) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = std::make_pair(version, std::move(source));
        }
        wake_.notify_one();
    }
};
struct OperationResult {
    bool Injected = false;
    std::string Source, Error;
};
class DraftWriter final {
    std::filesystem::path path_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<std::string> pending_;
    std::string error_;
    bool stop_ = false, busy_ = false;
    std::thread thread_;
    void Run() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            changed_.wait(lock, [this] { return stop_ || pending_.has_value(); });
            if (stop_)
                return;
            auto source = std::move(*pending_);
            pending_.reset();
            busy_ = true;
            lock.unlock();
            std::string error;
            try {
                ScriptStorage::Write(path_, source, false);
            } catch (const std::exception &value) {
                error = value.what();
            }
            lock.lock();
            error_ = std::move(error);
            busy_ = false;
            changed_.notify_all();
        }
    }

  public:
    explicit DraftWriter(std::filesystem::path path) : path_(std::move(path)), thread_([this] { Run(); }) {}
    ~DraftWriter() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            pending_.reset();
        }
        changed_.notify_all();
        thread_.join();
    }
    void Submit(std::string source) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = std::move(source);
        }
        changed_.notify_one();
    }
    void CancelAndWait() {
        std::unique_lock<std::mutex> lock(mutex_);
        pending_.reset();
        changed_.wait(lock, [this] { return !busy_; });
    }
    std::string TakeError() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string result;
        result.swap(error_);
        return result;
    }
};

class MainWindow final {
    HINSTANCE instance_;
    HWND window_ = nullptr, form_ = nullptr;
    std::map<int, HWND> controls_, fields_;
    std::vector<HWND> labels_;
    HFONT font_ = nullptr, codeFont_ = nullptr, titleFont_ = nullptr;
    HBRUSH panelBrush_ = nullptr, editBrush_ = nullptr;
    UINT dpi_ = 96;
    bool demo_ = false, visual_ = false, building_ = false, settingCode_ = false, busy_ = false,
         closing_ = false, closeRequested_ = false, invalidVisual_ = false, reduced_ = false,
         formDirty_ = false;
    std::uint64_t version_ = 0, accepted_ = 0;
    int scroll_ = 0, formHeight_ = 0;
    std::filesystem::path libraryDirectory_, currentPath_, draftPath_, consumedDraft_;
    std::string source_, saved_, active_, status_, error_, diagnostic_;
    std::vector<std::filesystem::path> libraryFiles_;
    std::vector<Row> rows_;
    int row_ = -1, step_ = 0;
    std::vector<std::size_t> condition_;
    std::vector<std::vector<std::size_t>> treePaths_;
    std::optional<VisualDocument> document_;
    std::unique_ptr<RemapperService> service_;
    std::unique_ptr<CompilerWorker> compiler_;
    std::unique_ptr<DraftWriter> drafts_;
    std::thread operation_;
    ServiceSnapshot snapshot_;
    std::function<int(const std::wstring &)> discardPrompt_;
    RECT editorRect_{};
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        MainWindow *self = reinterpret_cast<MainWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<MainWindow *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self) {
            try {
                return self->Message(message, w, l);
            } catch (const std::exception &error) {
                self->Error(error.what());
                return 0;
            }
        }
        return DefWindowProcW(window, message, w, l);
    }
    static LRESULT CALLBACK FormProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto *self = reinterpret_cast<MainWindow *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self) {
            try {
                if (message == WM_ERASEBKGND) {
                    RECT area{};
                    GetClientRect(window, &area);
                    FillRect(reinterpret_cast<HDC>(w), &area, self->panelBrush_);
                    return 1;
                }
                if (message == WM_PAINT) {
                    PAINTSTRUCT paint{};
                    const auto dc = BeginPaint(window, &paint);
                    FillRect(dc, &paint.rcPaint, self->panelBrush_);
                    EndPaint(window, &paint);
                    return 0;
                }
                if (message == WM_PRINTCLIENT) {
                    RECT area{};
                    GetClientRect(window, &area);
                    FillRect(reinterpret_cast<HDC>(w), &area, self->panelBrush_);
                    return 0;
                }
                if (message == WM_COMMAND || message == WM_NOTIFY || message == WM_DRAWITEM ||
                    message == WM_MEASUREITEM || message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT ||
                    message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLORBTN)
                    return SendMessageW(self->window_, message, w, l);
                if (message == WM_VSCROLL) {
                    self->Scroll(LOWORD(w), HIWORD(w));
                    return 0;
                }
                if (message == WM_MOUSEWHEEL) {
                    self->Scroll(GET_WHEEL_DELTA_WPARAM(w) > 0 ? SB_LINEUP : SB_LINEDOWN, 0);
                    return 0;
                }
            } catch (const std::exception &error) {
                self->Error(error.what());
                return 0;
            }
        }
        return DefWindowProcW(window, message, w, l);
    }
    static LRESULT CALLBACK CodeProcedure(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR,
                                          DWORD_PTR data) {
        auto *self = reinterpret_cast<MainWindow *>(data);
        if (message == WM_KEYDOWN && w == VK_TAB && !(GetKeyState(VK_CONTROL) & 0x8000) &&
            !(GetKeyState(VK_SHIFT) & 0x8000)) {
            if (!self->busy_)
                SendMessageW(window, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"    "));
            return 0;
        }
        return DefSubclassProc(window, message, w, l);
    }
    int S(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    HWND Control(int id, const wchar_t *type, const std::wstring &text, DWORD style, HWND parent = nullptr) {
        HWND value = CreateWindowExW(0, type, text.c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
                                     parent ? parent : window_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (!value)
            throw std::runtime_error("A Windows interface control could not be created.");
        SendMessageW(value, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
        return value;
    }
    HWND Button(int id, const std::wstring &text, HWND parent = nullptr) {
        return Control(id, L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, parent);
    }
    void Place(HWND control, int x, int y, int width, int height) {
        MoveWindow(control, S(x), S(y), std::max(1, S(width)), std::max(1, S(height)), TRUE);
    }
    void Fonts() {
        if (font_)
            DeleteObject(font_);
        if (codeFont_)
            DeleteObject(codeFont_);
        if (titleFont_)
            DeleteObject(titleFont_);
        font_ =
            CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        codeFont_ =
            CreateFontW(-S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        titleFont_ = CreateFontW(-S(25), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                 L"Segoe UI");
    }
    void Rich(HWND control, bool mono) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(mono ? codeFont_ : font_), TRUE);
        SendMessageW(control, EM_SETBKGNDCOLOR, 0, Panel);
        CHARFORMAT2W format{};
        format.cbSize = sizeof(format);
        format.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
        format.crTextColor = Ink;
        format.yHeight = 195;
        wcscpy_s(format.szFaceName, mono ? L"Consolas" : L"Segoe UI");
        SendMessageW(control, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&format));
        SendMessageW(control, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
    }
    void CreateControls() {
        for (const auto &item :
             std::vector<std::pair<int, std::wstring>>{{NewFile, L"+ New"},
                                                       {OpenFile, L"Open…"},
                                                       {Folder, L"Scripts folder ↗"},
                                                       {Recover, L"Recover draft"},
                                                       {Save, L"Save"},
                                                       {SaveAs, L"Save as…"},
                                                       {CodeTab, L"{ }  Code"},
                                                       {VisualTab, L"◇  Visual"},
                                                       {FindNext, L"Find"},
                                                       {Check, L"Check script"},
                                                       {Inject, L"↗  Inject configuration"},
                                                       {Stop, L"■ Stop"},
                                                       {Driver, L"Driver setup ↗"},
                                                       {Copy, L"Copy diagnostics"},
                                                       {AddRule, L"+ Rule"},
                                                       {AddMacro, L"+ Macro"},
                                                       {Infinity, L"∞ Recipe"},
                                                       {AddConstant, L"+ Constant"},
                                                       {RemoveItem, L"Remove"}})
            controls_[item.first] = Button(item.first, item.second);
        controls_[Library] =
            Control(Library, L"LISTBOX", L"",
                    LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP);
        controls_[VisualList] =
            Control(VisualList, L"LISTBOX", L"",
                    LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP);
        controls_[FindText] = Control(FindText, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP);
        controls_[Live] = Control(Live, L"BUTTON", L"Live apply valid edits", BS_AUTOCHECKBOX | WS_TABSTOP);
        controls_[Reduce] = Control(Reduce, L"BUTTON", L"Reduce motion", BS_AUTOCHECKBOX | WS_TABSTOP);
        SetWindowTheme(controls_[Live], L"", L"");
        SetWindowTheme(controls_[Reduce], L"", L"");
        controls_[Code] =
            Control(Code, MSFTEDIT_CLASS, L"",
                    ES_MULTILINE | ES_WANTRETURN | ES_NOHIDESEL | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP);
        Rich(controls_[Code], true);
        SendMessageW(controls_[Code], EM_EXLIMITTEXT, 0, ScriptStorage::MaximumBytes);
        SendMessageW(controls_[Code], EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SCROLL);
        SetWindowSubclass(controls_[Code], CodeProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
        controls_[Monitor] =
            Control(Monitor, MSFTEDIT_CLASS, L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_TABSTOP);
        Rich(controls_[Monitor], false);
        form_ = CreateWindowExW(0, L"GprVisualForm", L"", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 1, 1,
                                window_, reinterpret_cast<HMENU>(Form), instance_, nullptr);
        SetWindowLongPtrW(form_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SetWindowLongPtrW(form_, GWL_EXSTYLE, WS_EX_CONTROLPARENT);
        BOOL animation = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0);
        reduced_ = animation == FALSE;
        SendMessageW(controls_[Reduce], BM_SETCHECK, reduced_ ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(controls_[Stop], FALSE);
        ShowWindow(controls_[Recover], SW_HIDE);
    }
    void Visibility() {
        for (int id : {AddRule, AddMacro, Infinity, AddConstant, RemoveItem, VisualList})
            ShowWindow(controls_[id], visual_ ? SW_SHOW : SW_HIDE);
        ShowWindow(form_, visual_ ? SW_SHOW : SW_HIDE);
        ShowWindow(controls_[Code], visual_ ? SW_HIDE : SW_SHOW);
        InvalidateRect(window_, nullptr, FALSE);
    }
    void Layout() {
        RECT r{};
        GetClientRect(window_, &r);
        const int width = MulDiv(r.right, 96, static_cast<int>(dpi_)),
                  height = MulDiv(r.bottom, 96, static_cast<int>(dpi_));
        const int right = width - 24, body = 112, bottom = height - 42, ex = 254, er = width - 310,
                  ew = er - ex;
        editorRect_ = {S(ex), S(body), S(er), S(bottom)};
        Place(controls_[Driver], right - 136, 26, 136, 34);
        Place(controls_[Reduce], right - 285, 31, 138, 24);
        Place(controls_[NewFile], 40, 178, 90, 34);
        Place(controls_[OpenFile], 138, 178, 90, 34);
        Place(controls_[Library], 40, 226, 188, std::max(100, bottom - 226 - 136));
        Place(controls_[Recover], 40, bottom - 126, 188, 31);
        Place(controls_[Folder], 40, bottom - 88, 188, 32);
        Place(controls_[Save], er - 172, body + 18, 65, 32);
        Place(controls_[SaveAs], er - 100, body + 18, 80, 32);
        Place(controls_[CodeTab], ex + 20, body + 66, 92, 33);
        Place(controls_[VisualTab], ex + 120, body + 66, 98, 33);
        Place(controls_[FindText], er - 225, body + 66, 135, 30);
        Place(controls_[FindNext], er - 84, body + 66, 64, 30);
        Place(controls_[Code], ex + 20, body + 115, ew - 40, std::max(80, bottom - body - 245));
        int bx = ex + 20;
        for (int id : {AddRule, AddMacro, Infinity, AddConstant}) {
            Place(controls_[id], bx, body + 111, id == Infinity ? 94 : 83, 30);
            bx += id == Infinity ? 102 : 91;
        }
        Place(controls_[RemoveItem], er - 92, body + 111, 72, 30);
        Place(controls_[VisualList], ex + 20, body + 154, 155, std::max(80, bottom - body - 284));
        Place(form_, ex + 185, body + 154, ew - 205, std::max(80, bottom - body - 284));
        Place(controls_[Live], ex + 20, bottom - 95, 240, 24);
        Place(controls_[Check], ex + 20, bottom - 59, 106, 34);
        Place(controls_[Inject], ex + 135, bottom - 59, 200, 34);
        Place(controls_[Stop], er - 92, bottom - 59, 72, 34);
        Place(controls_[Monitor], er + 36, body + 54, right - er - 52, std::max(100, bottom - body - 115));
        Place(controls_[Copy], er + 36, bottom - 46, right - er - 52, 30);
        if (visual_ && !formDirty_)
            BuildForm();
        InvalidateRect(window_, nullptr, FALSE);
    }
    void Fill(HDC dc, RECT r, COLORREF color, int radius = 0) {
        HBRUSH brush = CreateSolidBrush(color);
        if (radius) {
            HGDIOBJ old = SelectObject(dc, brush), pen = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, r.left, r.top, r.right, r.bottom, S(radius), S(radius));
            SelectObject(dc, pen);
            SelectObject(dc, old);
        } else
            FillRect(dc, &r, brush);
        DeleteObject(brush);
    }
    void Draw(HDC dc, const std::wstring &text, RECT r, COLORREF color, HFONT font = nullptr,
              UINT flags = DT_LEFT | DT_WORDBREAK) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        HGDIOBJ old = SelectObject(dc, font ? font : font_);
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, flags | DT_NOPREFIX);
        SelectObject(dc, old);
    }
    void Paint(HDC dc) {
        RECT r{};
        GetClientRect(window_, &r);
        Fill(dc, r, Background);
        const int width = MulDiv(r.right, 96, static_cast<int>(dpi_)),
                  height = MulDiv(r.bottom, 96, static_cast<int>(dpi_));
        Fill(dc, {S(24), S(112), S(240), r.bottom - S(42)}, Panel, 16);
        Fill(dc, editorRect_, Panel, 16);
        Fill(dc, {S(width - 294), S(112), r.right - S(24), r.bottom - S(42)}, Panel, 16);
        Draw(dc, L"◇  GamePad Remapper", {S(26), S(24), S(700), S(65)}, Ink, titleFont_);
        Draw(dc, L"NATIVE CONTROLLER SCRIPTS · YOUR CONTROLLER. YOUR LOGIC.", {S(29), S(72), S(850), S(92)},
             Muted);
        Draw(dc, L"SCRIPT LIBRARY", {S(40), S(132), S(228), S(153)}, Muted);
        Draw(dc, L"Made for your playstyle", {S(40), S(155), S(230), S(176)}, Ink);
        Draw(dc, L".igc scripts stay on your PC\nDriver demo: 2 h per boot",
             {S(40), S(height - 89), S(225), S(height - 45)}, Muted);
        std::wstring name = currentPath_.empty() ? L"Untitled.igc" : currentPath_.filename().wstring();
        if (source_ != saved_ || invalidVisual_)
            name += L"  •";
        Draw(dc, name, {editorRect_.left + S(20), S(132), editorRect_.right - S(180), S(157)}, Ink);
        Draw(dc,
             demo_                ? L"SIMULATED PREVIEW · no driver access"
             : active_.empty()    ? L"EDITING · no configuration active"
             : active_ == source_ ? L"LIVE · this version is active"
                                  : L"LIVE · edited version not applied",
             {editorRect_.left + S(20), S(161), editorRect_.right - S(20), S(180)}, Muted);
        Draw(dc, Utf16(status_),
             {editorRect_.left + S(20), S(height - 162), editorRect_.right - S(20), S(height - 136)},
             error_.empty() ? Mint : Danger);
        Draw(dc, L"LIVE SESSION & CONTROLLERS", {S(width - 278), S(132), S(width - 42), S(156)}, Muted);
        Draw(dc,
             demo_ ? L"SIMULATED PREVIEW · no driver or controller connection"
                   : L"Ctrl+Enter apply · Ctrl+S save · Ctrl+F find",
             {S(24), r.bottom - S(29), r.right - S(24), r.bottom - S(5)}, Muted);
        RECT progress{editorRect_.left + S(20), editorRect_.bottom - S(12), editorRect_.right - S(20),
                      editorRect_.bottom - S(9)};
        Fill(dc, progress, Edge);
        if (busy_) {
            const int available = progress.right - progress.left;
            const int tick = static_cast<int>((GetTickCount64() / 15) % 100);
            const int x = reduced_ ? 0 : available * tick / 100;
            RECT segment{progress.left + x, progress.top,
                         std::min(progress.right, progress.left + x + available / 3), progress.bottom};
            Fill(dc, segment, Accent);
        } else if (!active_.empty())
            Fill(dc, progress, Mint);
    }
    void DrawItem(const DRAWITEMSTRUCT &item) {
        if (item.itemID == static_cast<UINT>(-1))
            return;
        if (item.CtlType == ODT_BUTTON) {
            const bool primary = item.CtlID == Inject;
            COLORREF color = primary ? RGB(116, 94, 223) : Raised;
            if (item.itemState & ODS_SELECTED)
                color = Edge;
            if (!(item.itemState & ODS_DISABLED) && item.itemState & ODS_HOTLIGHT)
                color = Edge;
            Fill(item.hDC, item.rcItem, color, 9);
            Draw(item.hDC, Text(item.hwndItem), item.rcItem, item.itemState & ODS_DISABLED ? Muted : Ink,
                 font_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return;
        }
        if (item.CtlType == ODT_LISTBOX) {
            Fill(item.hDC, item.rcItem, item.itemState & ODS_SELECTED ? RGB(43, 40, 76) : Panel, 8);
            RECT name = item.rcItem;
            name.left += S(10);
            name.top += S(8);
            std::wstring heading, detail;
            if (item.CtlID == Library && item.itemID < libraryFiles_.size()) {
                heading = libraryFiles_[item.itemID].stem().wstring();
                detail = L".igc · local script";
            } else if (item.CtlID == VisualList && item.itemID < rows_.size()) {
                const auto row = rows_[item.itemID];
                if (row.Kind == RowKind::Rule) {
                    heading = Utf16(document_->Rules[row.Index].Source);
                    detail = L"INPUT RULE";
                } else if (row.Kind == RowKind::Macro) {
                    heading = Utf16(document_->Macros[row.Index].Name);
                    detail = L"MACRO";
                } else {
                    heading = Utf16(document_->Constants[row.Index].Name);
                    detail = L"CONSTANT";
                }
            }
            name.bottom = name.top + S(21);
            Draw(item.hDC, heading, name, Ink, font_, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            name.top += S(22);
            name.bottom += S(22);
            Draw(item.hDC, detail, name, Muted, font_, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    void Error(const std::string &text) {
        error_ = text;
        status_ = text;
        diagnostic_ = "Last operation failed\n" + text + "\n\nNo partial configuration was applied.";
        UpdateMonitor();
        InvalidateRect(window_, nullptr, FALSE);
    }
    void Dirty() {
        SetWindowTextW(window_,
                       ((currentPath_.empty() ? L"Untitled.igc" : currentPath_.filename().wstring()) +
                        (source_ != saved_ ? L" *" : L"") + L" — GamePad Remapper")
                           .c_str());
        InvalidateRect(window_, &editorRect_, FALSE);
    }
    void Queue() {
        ++version_;
        if (compiler_)
            compiler_->Submit(version_, source_);
    }
    void SourceChanged() {
        if (settingCode_)
            return;
        source_ = Utf8(Text(controls_[Code]));
        if (source_.size() > ScriptStorage::MaximumBytes)
            throw std::runtime_error("Scripts may contain at most 512 KiB of UTF-8 text.");
        status_ = "Edited · checking automatically…";
        error_.clear();
        Dirty();
        Queue();
    }
    void SetSource(std::string source, std::filesystem::path path, bool modified) {
        building_ = true;
        invalidVisual_ = false;
        document_.reset();
        condition_.clear();
        row_ = -1;
        step_ = 0;
        source_ = std::move(source);
        saved_ = modified ? std::string{} : source_;
        currentPath_ = std::move(path);
        settingCode_ = true;
        SetWindowTextW(controls_[Code], Utf16(source_).c_str());
        settingCode_ = false;
        building_ = false;
        if (visual_)
            EnterVisual();
        Dirty();
        Queue();
    }
    void LoadLibrary() {
        std::filesystem::create_directories(libraryDirectory_);
        const auto examples = ExecutableDirectory() / L"Scripts";
        if (std::filesystem::exists(examples))
            for (const auto &file : std::filesystem::directory_iterator(examples))
                if (file.is_regular_file() && _wcsicmp(file.path().extension().c_str(), L".igc") == 0) {
                    const auto target = libraryDirectory_ / file.path().filename();
                    if (!std::filesystem::exists(target) &&
                        !std::filesystem::exists(target.wstring() + L".bak"))
                        std::filesystem::copy_file(file.path(), target);
                }
        libraryFiles_.clear();
        for (const auto &file : std::filesystem::directory_iterator(libraryDirectory_))
            if (file.is_regular_file() && _wcsicmp(file.path().extension().c_str(), L".igc") == 0)
                libraryFiles_.push_back(file.path());
        if (!currentPath_.empty() &&
            std::find(libraryFiles_.begin(), libraryFiles_.end(), currentPath_) == libraryFiles_.end())
            libraryFiles_.push_back(currentPath_);
        std::sort(libraryFiles_.begin(), libraryFiles_.end());
        SendMessageW(controls_[Library], LB_RESETCONTENT, 0, 0);
        for (std::size_t i = 0; i < libraryFiles_.size(); ++i) {
            SendMessageW(controls_[Library], LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(libraryFiles_[i].filename().c_str()));
            if (libraryFiles_[i] == currentPath_)
                SendMessageW(controls_[Library], LB_SETCURSEL, i, 0);
        }
        ShowWindow(controls_[Recover], RecoveryCandidate().empty() ? SW_HIDE : SW_SHOW);
    }
    std::filesystem::path RecoveryCandidate() {
        auto dir = draftPath_.parent_path();
        if (!std::filesystem::exists(dir))
            return {};
        std::filesystem::path result;
        std::filesystem::file_time_type latest{};
        for (const auto &file : std::filesystem::directory_iterator(dir))
            if (file.is_regular_file() && _wcsicmp(file.path().extension().c_str(), L".igc") == 0 &&
                file.path() != draftPath_ && file.path() != consumedDraft_) {
                const auto time = file.last_write_time();
                if (result.empty() || time > latest) {
                    result = file.path();
                    latest = time;
                }
            }
        return result;
    }
    void RetireDrafts() {
        if (drafts_)
            drafts_->CancelAndWait();
        std::error_code ignored;
        std::filesystem::remove(draftPath_, ignored);
        if (!consumedDraft_.empty())
            std::filesystem::remove(consumedDraft_, ignored);
        consumedDraft_.clear();
    }
    std::filesystem::path FileDialog(bool save) {
        wchar_t name[32768]{};
        if (save) {
            const auto initial =
                currentPath_.empty() ? L"My controller.igc" : currentPath_.filename().wstring();
            wcscpy_s(name, initial.c_str());
        }
        const auto directory = libraryDirectory_.wstring();
        OPENFILENAMEW request{};
        request.lStructSize = sizeof(request);
        request.hwndOwner = window_;
        request.lpstrFilter = L"IGC controller scripts (*.igc)\0*.igc\0All files\0*.*\0";
        request.lpstrFile = name;
        request.nMaxFile = 32768;
        request.lpstrInitialDir = directory.c_str();
        request.lpstrDefExt = L"igc";
        request.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                        (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        const BOOL result = save ? GetSaveFileNameW(&request) : GetOpenFileNameW(&request);
        return result ? std::filesystem::path(name) : std::filesystem::path{};
    }
    bool SaveFile(bool as, bool requireValid = true) {
        if (requireValid && !FlushForm())
            return false;
        auto path = as || currentPath_.empty() ? FileDialog(true) : currentPath_;
        if (path.empty())
            return false;
        ScriptStorage::Write(path, source_);
        currentPath_ = std::move(path);
        saved_ = source_;
        RetireDrafts();
        LoadLibrary();
        Dirty();
        status_ = "Saved · the previous version is in .bak";
        return true;
    }
    bool Confirm() {
        const bool valid = FlushForm();
        if (source_ == saved_ && valid)
            return true;
        const std::wstring message =
            valid ? L"Save your changes before continuing?"
                  : L"Some visual fields are invalid and have not been applied.\n\nSave the last valid "
                    L"script before continuing?\nYes saves the valid script and discards those fields. No "
                    L"discards the edits. Cancel returns to the editor.";
        const int choice = discardPrompt_ ? discardPrompt_(message)
                                          : MessageBoxW(window_, message.c_str(), L"Unsaved script",
                                                        MB_YESNOCANCEL | MB_ICONQUESTION);
        if (choice == IDCANCEL)
            return false;
        if (choice == IDYES && !SaveFile(false, valid))
            return false;
        if (!valid) {
            invalidVisual_ = false;
            document_ = VisualDocument::Parse(source_);
            BuildVisualList();
        }
        return true;
    }
    void Open(const std::filesystem::path &path) {
        const auto source = ScriptStorage::Read(path);
        RetireDrafts();
        SetSource(source, std::filesystem::absolute(path), false);
        LoadLibrary();
    }
    void Find() {
        if (!FlushForm())
            return;
        visual_ = false;
        Visibility();
        const auto needle = Text(controls_[FindText]);
        if (needle.empty()) {
            SetFocus(controls_[FindText]);
            return;
        }
        FINDTEXTEXW find{};
        CHARRANGE range{};
        SendMessageW(controls_[Code], EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&range));
        find.chrg.cpMin = range.cpMax;
        find.chrg.cpMax = -1;
        find.lpstrText = const_cast<wchar_t *>(needle.c_str());
        LRESULT result =
            SendMessageW(controls_[Code], EM_FINDTEXTEXW, FR_DOWN, reinterpret_cast<LPARAM>(&find));
        if (result < 0) {
            find.chrg.cpMin = 0;
            result = SendMessageW(controls_[Code], EM_FINDTEXTEXW, FR_DOWN, reinterpret_cast<LPARAM>(&find));
        }
        if (result >= 0) {
            SendMessageW(controls_[Code], EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&find.chrgText));
            SendMessageW(controls_[Code], EM_SCROLLCARET, 0, 0);
            SetFocus(controls_[Code]);
        } else
            status_ = "No matching text";
    }
    std::wstring FieldText(int id) const {
        const auto it = fields_.find(id);
        return it == fields_.end() ? L"" : Text(it->second);
    }
    int Number(int id) const {
        const auto text = FieldText(id);
        std::size_t used = 0;
        try {
            const long long value = std::stoll(text, &used);
            if (used != text.size() || value < INT_MIN || value > INT_MAX)
                throw std::exception();
            return static_cast<int>(value);
        } catch (...) {
            throw std::runtime_error("Enter a whole number in the highlighted property.");
        }
    }
    int Selected(int id) const {
        const auto it = fields_.find(id);
        if (it == fields_.end())
            return 0;
        const auto selected = SendMessageW(it->second, CB_GETCURSEL, 0, 0);
        return selected == CB_ERR ? 0 : static_cast<int>(selected);
    }
    bool Checked(int id) const { return SendMessageW(fields_.at(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    HWND Label(const std::wstring &text, int x, int y, int width) {
        auto value = Control(0, L"STATIC", text, SS_LEFT, form_);
        Place(value, x, y - scroll_, width, 20);
        labels_.push_back(value);
        return value;
    }
    void Edit(int id, const std::wstring &label, const std::string &value, int x, int y, int width,
              bool multiline = false) {
        Label(label, x, y, width);
        auto input = Control(id, L"EDIT", Utf16(value),
                             WS_BORDER | WS_TABSTOP |
                                 (multiline ? ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL : ES_AUTOHSCROLL),
                             form_);
        Place(input, x, y + 23 - scroll_, width, multiline ? 75 : 29);
        SendMessageW(input, EM_LIMITTEXT, id == Expression ? 4096 : 4096, 0);
        fields_[id] = input;
    }
    void EditNumber(int id, const std::wstring &label, int value, int x, int y, int width) {
        Edit(id, label, std::to_string(value), x, y, width);
    }
    void Select(int id, const std::wstring &label, const std::vector<std::wstring> &options, int value, int x,
                int y, int width) {
        Label(label, x, y, width);
        auto input = Control(id, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, form_);
        for (const auto &option : options)
            SendMessageW(input, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option.c_str()));
        SendMessageW(input, CB_SETCURSEL, value, 0);
        Place(input, x, y + 23 - scroll_, width, 300);
        fields_[id] = input;
    }
    void ControlSelect(int id, const std::wstring &label, const std::string &value, int x, int y, int width) {
        Label(label, x, y, width);
        auto input =
            Control(id, L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, form_);
        std::vector<std::string> names;
        for (std::size_t i = 0; i < Controls::Names().size(); ++i)
            if (i < 22 || i >= 32)
                names.push_back(Controls::Names()[i]);
        auto available = [this](const std::string &name) {
            const int index = Controls::Parse(name);
            return std::any_of(snapshot_.Devices.begin(), snapshot_.Devices.end(),
                               [index](const ServiceDevice &d) {
                                   return index >= 0 && d.Available[static_cast<std::size_t>(index)];
                               });
        };
        std::stable_sort(names.begin(), names.end(),
                         [&](const auto &a, const auto &b) { return available(a) > available(b); });
        for (const auto &name : names) {
            const auto option = Utf16(name);
            SendMessageW(input, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option.c_str()));
        }
        SetWindowTextW(input, Utf16(value).c_str());
        Place(input, x, y + 23 - scroll_, width, 320);
        fields_[id] = input;
    }
    void EnableField(bool enabled, int x, int y) {
        auto input = Control(Enabled, L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, form_);
        SetWindowTheme(input, L"", L"");
        SendMessageW(input, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        Place(input, x, y - scroll_, 110, 26);
        fields_[Enabled] = input;
    }
    int FormWidth() const {
        RECT r{};
        GetClientRect(form_, &r);
        return std::max(260, MulDiv(r.right, 96, static_cast<int>(dpi_)) - 24);
    }
    VisualCondition *ConditionAt(VisualMacro &macro, const std::vector<std::size_t> &path) {
        if (!macro.Condition) {
            VisualCondition condition;
            condition.Kind = VisualConditionKind::Pressed;
            condition.Control = macro.Trigger;
            macro.Condition = std::move(condition);
        }
        auto *node = &*macro.Condition;
        for (auto index : path) {
            if (index >= node->Children.size())
                return nullptr;
            node = &node->Children[index];
        }
        return node;
    }
    static std::wstring ConditionLabel(const VisualCondition &node) {
        static const wchar_t *kinds[] = {L"ALL",      L"ANY",        L"NOT",        L"Held",
                                         L"Pressed",  L"Released",   L"Axis/value", L"Absolute",
                                         L"Held for", L"Double tap", L"Expression"};
        std::wstring label = kinds[static_cast<std::size_t>(node.Kind)];
        if (node.Kind >= VisualConditionKind::Held && node.Kind != VisualConditionKind::Expression)
            label += L" · " + Utf16(node.Control);
        if (node.Kind == VisualConditionKind::Compare || node.Kind == VisualConditionKind::AbsoluteCompare)
            label += L" " + Utf16(node.Operator) + L" " + std::to_wstring(node.Value);
        return label;
    }
    void TreeNode(HWND tree, const VisualCondition &node, HTREEITEM parent, std::vector<std::size_t> path) {
        const auto label = ConditionLabel(node);
        treePaths_.push_back(path);
        TVINSERTSTRUCTW insert{};
        insert.hParent = parent;
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT | TVIF_PARAM;
        insert.item.pszText = const_cast<wchar_t *>(label.c_str());
        insert.item.lParam = static_cast<LPARAM>(treePaths_.size() - 1);
        const auto item = reinterpret_cast<HTREEITEM>(
            SendMessageW(tree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&insert)));
        if (path == condition_)
            TreeView_SelectItem(tree, item);
        for (std::size_t i = 0; i < node.Children.size(); ++i) {
            auto child = path;
            child.push_back(i);
            TreeNode(tree, node.Children[i], item, std::move(child));
        }
        TreeView_Expand(tree, item, TVE_EXPAND);
    }
    void BuildVisualList() {
        building_ = true;
        rows_.clear();
        SendMessageW(controls_[VisualList], LB_RESETCONTENT, 0, 0);
        if (document_) {
            for (std::size_t i = 0; i < document_->Rules.size(); ++i)
                rows_.push_back({RowKind::Rule, i});
            for (std::size_t i = 0; i < document_->Macros.size(); ++i)
                rows_.push_back({RowKind::Macro, i});
            for (std::size_t i = 0; i < document_->Constants.size(); ++i)
                rows_.push_back({RowKind::Constant, i});
            for (const auto &row : rows_) {
                std::wstring name = row.Kind == RowKind::Rule ? Utf16(document_->Rules[row.Index].Source)
                                    : row.Kind == RowKind::Macro
                                        ? Utf16(document_->Macros[row.Index].Name)
                                        : Utf16(document_->Constants[row.Index].Name);
                SendMessageW(controls_[VisualList], LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
            }
            if (rows_.empty())
                row_ = -1;
            else
                row_ = std::max(0, std::min(row_, static_cast<int>(rows_.size() - 1)));
            SendMessageW(controls_[VisualList], LB_SETCURSEL, row_, 0);
        }
        building_ = false;
        BuildForm();
    }
    void BuildForm() {
        if (!form_)
            return;
        building_ = true;
        formDirty_ = false;
        for (auto &field : fields_)
            DestroyWindow(field.second);
        for (auto label : labels_)
            DestroyWindow(label);
        fields_.clear();
        labels_.clear();
        treePaths_.clear();
        if (!document_ || row_ < 0 || static_cast<std::size_t>(row_) >= rows_.size()) {
            Label(L"Choose a rule or macro, or add an infinity recipe.", 12, 12, FormWidth());
            formHeight_ = 100;
            building_ = false;
            return;
        }
        const auto row = rows_[static_cast<std::size_t>(row_)];
        const int width = FormWidth(), half = (width - 12) / 2;
        int y = 8;
        if (row.Kind == RowKind::Rule) {
            const auto &rule = document_->Rules[row.Index];
            Select(Kind, L"Input rule", {L"Remap", L"Invert", L"Deadzone", L"Sensitivity", L"Block"},
                   static_cast<int>(rule.Kind), 12, y, half);
            EnableField(rule.Enabled, half + 24, y + 25);
            y += 72;
            ControlSelect(Source, L"From / control", rule.Source, 12, y, half);
            if (rule.Kind == VisualRuleKind::Remap)
                ControlSelect(Target, L"To", rule.Target, half + 24, y, half);
            y += 72;
            if (rule.Kind == VisualRuleKind::Deadzone || rule.Kind == VisualRuleKind::Sensitivity) {
                EditNumber(Value,
                           rule.Kind == VisualRuleKind::Deadzone ? L"Deadzone 0–99%" : L"Sensitivity 0–1000%",
                           rule.Value, 12, y, half);
                y += 72;
            }
        } else if (row.Kind == RowKind::Constant) {
            const auto &constant = document_->Constants[row.Index];
            Edit(Name, L"Constant name", constant.Name, 12, y, width);
            y += 72;
            EditNumber(Value, L"Integer value", constant.Value, 12, y, half);
            y += 72;
        } else {
            auto &macro = document_->Macros[row.Index];
            Edit(Name, L"Macro name", macro.Name, 12, y, width - 126);
            EnableField(macro.Enabled, width - 102, y + 24);
            y += 72;
            Select(Kind, L"Action", {L"Timed sequence", L"Axis motion"}, static_cast<int>(macro.Kind), 12, y,
                   half);
            Select(Activation, L"Activation",
                   {L"On activation", L"While true", L"On deactivation", L"Toggle"},
                   static_cast<int>(macro.Activation), half + 24, y, half);
            y += 72;
            Label(L"TRIGGER · Combine ALL / ANY / NOT, buttons and axis thresholds", 12, y, width);
            y += 26;
            auto tree = Control(TriggerTree, WC_TREEVIEWW, L"",
                                TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
                                    WS_BORDER | WS_TABSTOP,
                                form_);
            TreeView_SetBkColor(tree, Raised);
            TreeView_SetTextColor(tree, Ink);
            Place(tree, 12, y - scroll_, width, 100);
            fields_[TriggerTree] = tree;
            TreeNode(tree, *ConditionAt(macro, {}), TVI_ROOT, {});
            y += 108;
            for (auto item : std::vector<std::pair<int, std::wstring>>{
                     {AddCondition, L"+ Condition"}, {RemoveCondition, L"Remove condition"}}) {
                auto button = Button(item.first, item.second, form_);
                Place(button, item.first == AddCondition ? 12 : 150, y - scroll_,
                      item.first == AddCondition ? 130 : 145, 29);
                fields_[item.first] = button;
            }
            y += 44;
            auto *node = ConditionAt(macro, condition_);
            if (!node) {
                condition_.clear();
                node = ConditionAt(macro, {});
            }
            Select(ConditionKind, L"Selected condition",
                   {L"ALL of these", L"ANY of these", L"NOT this", L"Button held", L"Button pressed",
                    L"Button released", L"Axis / value", L"Absolute value", L"Held for time", L"Double tap",
                    L"IGC expression"},
                   static_cast<int>(node->Kind), 12, y, width);
            y += 72;
            if (node->Kind >= VisualConditionKind::Held && node->Kind != VisualConditionKind::Expression) {
                ControlSelect(ConditionControl, L"Control", node->Control, 12, y, half);
                if (node->Kind == VisualConditionKind::HeldFor ||
                    node->Kind == VisualConditionKind::DoubleTap)
                    EditNumber(ConditionTime, L"Time window 1–60000 ms", node->Milliseconds, half + 24, y,
                               half);
                y += 72;
                if (node->Kind == VisualConditionKind::Compare ||
                    node->Kind == VisualConditionKind::AbsoluteCompare) {
                    const std::vector<std::wstring> ops{L">", L">=", L"<", L"<=", L"==", L"!="};
                    const auto found = std::find(ops.begin(), ops.end(), Utf16(node->Operator));
                    Select(ConditionOperator, L"Comparison", ops,
                           found == ops.end() ? 0 : static_cast<int>(found - ops.begin()), 12, y, half);
                    EditNumber(ConditionValue, L"Threshold", node->Value, half + 24, y, half);
                    y += 72;
                }
            }
            if (node->Kind == VisualConditionKind::Expression) {
                Edit(Expression, L"IGC condition expression", node->Expression, 12, y, width, true);
                y += 125;
            }
            y += 8;
            Label(macro.Kind == VisualMacroKind::AxisPattern
                      ? L"MOTION · Values can be added around your physical stick position"
                      : L"TIMED STEPS · Select a step to edit its values",
                  12, y, width);
            y += 30;
            if (macro.Kind == VisualMacroKind::AxisPattern) {
                const auto &p = macro.Pattern;
                Select(Shape, L"Shape", {L"Figure eight (∞)", L"Circle", L"Sine"}, static_cast<int>(p.Shape),
                       12, y, half);
                Select(Blend, L"Output", {L"Add", L"Replace"}, static_cast<int>(p.Blend), half + 24, y, half);
                y += 72;
                ControlSelect(TargetX, L"Horizontal axis", p.TargetX, 12, y, half);
                ControlSelect(TargetY, L"Vertical axis", p.TargetY, half + 24, y, half);
                y += 72;
                EditNumber(Width, L"Width 0–100%", p.AmplitudeX, 12, y, half);
                EditNumber(Height, L"Height 0–100%", p.AmplitudeY, half + 24, y, half);
                y += 72;
                EditNumber(Period, L"Cycle 1–3600000 ms", p.PeriodMilliseconds, 12, y, half);
                EditNumber(Tick, L"Update 1–20 ms", p.TickMilliseconds, half + 24, y, half);
                y += 72;
                Select(Center, L"Motion center",
                       {L"Follow physical input", L"Position at activation", L"Stick center (0,0)"},
                       static_cast<int>(p.Center), 12, y, width);
                y += 72;
                Label(L"On activation/deactivation plays one cycle. While true stops on release. Toggle "
                      L"activates again to stop. Add follows the live output; fixed centers use Replace.",
                      12, y, width);
                y += 65;
            } else {
                auto list =
                    Control(Steps, L"LISTBOX", L"", LBS_NOTIFY | WS_VSCROLL | WS_BORDER | WS_TABSTOP, form_);
                for (std::size_t i = 0; i < macro.Steps.size(); ++i) {
                    const auto &s = macro.Steps[i];
                    const auto line = std::to_wstring(i + 1) + L". " + Utf16(s.Control) + L"  " +
                                      std::to_wstring(s.Value) + L"  / " +
                                      std::to_wstring(s.DurationMilliseconds) + L" ms";
                    SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
                }
                step_ = macro.Steps.empty()
                            ? 0
                            : std::max(0, std::min(step_, static_cast<int>(macro.Steps.size() - 1)));
                SendMessageW(list, LB_SETCURSEL, step_, 0);
                Place(list, 12, y - scroll_, width, 110);
                fields_[Steps] = list;
                y += 120;
                if (!macro.Steps.empty()) {
                    const auto &s = macro.Steps[static_cast<std::size_t>(step_)];
                    ControlSelect(StepControl, L"Control", s.Control, 12, y, half);
                    Select(StepBlend, L"Output", {L"Add", L"Replace"}, static_cast<int>(s.Blend), half + 24,
                           y, half);
                    y += 72;
                    EditNumber(StepValue, L"Value", s.Value, 12, y, half);
                    EditNumber(StepDuration, L"Hold 1–60000 ms", s.DurationMilliseconds, half + 24, y, half);
                    y += 72;
                }
                int x = 12;
                for (auto item : std::vector<std::pair<int, std::wstring>>{
                         {AddStep, L"+ Step"}, {RemoveStep, L"Remove"}, {UpStep, L"↑"}, {DownStep, L"↓"}}) {
                    auto button = Button(item.first, item.second, form_);
                    Place(button, x, y - scroll_, item.first == AddStep || item.first == RemoveStep ? 88 : 40,
                          29);
                    fields_[item.first] = button;
                    x += item.first == AddStep || item.first == RemoveStep ? 96 : 48;
                }
                y += 44;
            }
        }
        formHeight_ = y + 18;
        RECT client{};
        GetClientRect(form_, &client);
        SCROLLINFO info{sizeof(info),
                        SIF_RANGE | SIF_PAGE | SIF_POS,
                        0,
                        S(formHeight_),
                        static_cast<UINT>(client.bottom),
                        S(scroll_),
                        0};
        SetScrollInfo(form_, SB_VERT, &info, TRUE);
        building_ = false;
        EnableWindow(form_, !busy_);
    }
    void Scroll(int code, int thumb) {
        RECT client{};
        GetClientRect(form_, &client);
        const int max = std::max(0, formHeight_ - MulDiv(client.bottom, 96, static_cast<int>(dpi_)));
        const int old = scroll_;
        if (code == SB_LINEUP)
            scroll_ -= 38;
        else if (code == SB_LINEDOWN)
            scroll_ += 38;
        else if (code == SB_PAGEUP)
            scroll_ -= max > 0 ? MulDiv(client.bottom, 96, static_cast<int>(dpi_)) : 0;
        else if (code == SB_PAGEDOWN)
            scroll_ += MulDiv(client.bottom, 96, static_cast<int>(dpi_));
        else if (code == SB_THUMBTRACK || code == SB_THUMBPOSITION) {
            SCROLLINFO info{sizeof(info), SIF_TRACKPOS, 0, 0, 0, 0, 0};
            GetScrollInfo(form_, SB_VERT, &info);
            scroll_ = MulDiv(info.nTrackPos, 96, static_cast<int>(dpi_));
            (void)thumb;
        }
        scroll_ = std::clamp(scroll_, 0, max);
        if (scroll_ != old) {
            ScrollWindowEx(form_, 0, S(old - scroll_), nullptr, nullptr, nullptr, nullptr,
                           SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
            SetScrollPos(form_, SB_VERT, S(scroll_), TRUE);
        }
    }
    bool FlushForm(bool rebuild = false, int changed = 0) {
        if (!visual_ || building_ || !formDirty_ || !document_ || row_ < 0 ||
            static_cast<std::size_t>(row_) >= rows_.size())
            return true;
        try {
            auto candidate = *document_;
            const auto row = rows_[static_cast<std::size_t>(row_)];
            if (row.Kind == RowKind::Rule) {
                auto &r = candidate.Rules[row.Index];
                r.Enabled = Checked(Enabled);
                r.Kind = static_cast<VisualRuleKind>(Selected(Kind));
                r.Source = Utf8(FieldText(Source));
                if (fields_.count(Target))
                    r.Target = Utf8(FieldText(Target));
                if (fields_.count(Value))
                    r.Value = Number(Value);
                if (changed == Kind && r.Kind == VisualRuleKind::Deadzone && (r.Value < 0 || r.Value > 99))
                    r.Value = 8;
            } else if (row.Kind == RowKind::Constant) {
                auto &c = candidate.Constants[row.Index];
                c.Name = Utf8(FieldText(Name));
                c.Value = Number(Value);
            } else {
                auto &m = candidate.Macros[row.Index];
                m.Name = Utf8(FieldText(Name));
                m.Enabled = Checked(Enabled);
                m.Kind = static_cast<VisualMacroKind>(Selected(Kind));
                m.Activation = static_cast<VisualMacroActivation>(Selected(Activation));
                if (m.Kind == VisualMacroKind::Sequence && m.Steps.empty())
                    m.Steps.push_back({});
                auto *node = ConditionAt(m, condition_);
                if (!node)
                    throw std::runtime_error("Select a valid trigger condition.");
                const auto kind = static_cast<VisualConditionKind>(Selected(ConditionKind));
                if (kind != node->Kind) {
                    if (kind == VisualConditionKind::Not) {
                        VisualCondition wrapper;
                        wrapper.Kind = kind;
                        wrapper.Children.push_back(*node);
                        *node = std::move(wrapper);
                    } else if (kind == VisualConditionKind::All || kind == VisualConditionKind::Any) {
                        if (node->Kind == VisualConditionKind::Not && node->Children.size() == 1) {
                            auto child = std::move(node->Children.front());
                            *node = std::move(child);
                        }
                        if (node->Kind != VisualConditionKind::All &&
                            node->Kind != VisualConditionKind::Any) {
                            VisualCondition wrapper;
                            wrapper.Children.push_back(*node);
                            *node = std::move(wrapper);
                        }
                        node->Kind = kind;
                    } else if (node->Kind == VisualConditionKind::Not && node->Children.size() == 1 &&
                               node->Children.front().Kind == kind) {
                        auto child = std::move(node->Children.front());
                        *node = std::move(child);
                    } else {
                        node->Kind = kind;
                        node->Children.clear();
                    }
                    if (kind == VisualConditionKind::Expression && node->Expression.empty())
                        node->Expression = "is_available(SOUTH) && get_ival(SOUTH) != 0";
                } else {
                    if (fields_.count(ConditionControl))
                        node->Control = Utf8(FieldText(ConditionControl));
                    if (fields_.count(ConditionOperator))
                        node->Operator = Utf8(FieldText(ConditionOperator));
                    if (fields_.count(ConditionValue))
                        node->Value = Number(ConditionValue);
                    if (fields_.count(ConditionTime))
                        node->Milliseconds = Number(ConditionTime);
                    if (fields_.count(Expression))
                        node->Expression = Utf8(FieldText(Expression));
                }
                if (fields_.count(Shape)) {
                    auto &p = m.Pattern;
                    p.Shape = static_cast<VisualPatternShape>(Selected(Shape));
                    p.Blend = static_cast<VisualBlend>(Selected(Blend));
                    p.Center = static_cast<VisualPatternCenter>(Selected(Center));
                    p.TargetX = Utf8(FieldText(TargetX));
                    p.TargetY = Utf8(FieldText(TargetY));
                    p.AmplitudeX = Number(Width);
                    p.AmplitudeY = Number(Height);
                    p.PeriodMilliseconds = Number(Period);
                    p.TickMilliseconds = Number(Tick);
                    if (changed == Blend && p.Blend == VisualBlend::Add)
                        p.Center = VisualPatternCenter::DynamicPhysical;
                    if (changed == Center && p.Center != VisualPatternCenter::DynamicPhysical)
                        p.Blend = VisualBlend::Replace;
                }
                if (fields_.count(StepControl) && static_cast<std::size_t>(step_) < m.Steps.size()) {
                    auto &s = m.Steps[static_cast<std::size_t>(step_)];
                    s.Control = Utf8(FieldText(StepControl));
                    s.Value = Number(StepValue);
                    s.DurationMilliseconds = Number(StepDuration);
                    s.Blend = static_cast<VisualBlend>(Selected(StepBlend));
                }
            }
            const auto source = candidate.BuildSource();
            document_ = std::move(candidate);
            invalidVisual_ = false;
            formDirty_ = false;
            error_.clear();
            if (source != source_) {
                source_ = source;
                settingCode_ = true;
                SetWindowTextW(controls_[Code], Utf16(source_).c_str());
                settingCode_ = false;
                Dirty();
                Queue();
            }
            if (rebuild)
                BuildForm();
            InvalidateRect(controls_[VisualList], nullptr, FALSE);
            return true;
        } catch (const ScriptException &error) {
            invalidVisual_ = true;
            Error(error.Diagnostic());
            return false;
        } catch (const std::exception &error) {
            invalidVisual_ = true;
            Error(error.what());
            return false;
        }
    }
    void EnterVisual() {
        if (!FlushForm())
            return;
        try {
            document_ = VisualDocument::Parse(source_);
            visual_ = true;
            invalidVisual_ = false;
            BuildVisualList();
            Visibility();
            Layout();
        } catch (const std::exception &error) {
            visual_ = false;
            Visibility();
            Error(error.what());
        }
    }
    void CommitDocument() {
        const auto source = document_->BuildSource();
        invalidVisual_ = false;
        error_.clear();
        source_ = source;
        settingCode_ = true;
        SetWindowTextW(controls_[Code], Utf16(source_).c_str());
        settingCode_ = false;
        Dirty();
        Queue();
        BuildVisualList();
    }
    std::string UniqueMacro(const std::string &prefix) {
        std::string name = prefix;
        int count = 1;
        while (std::any_of(document_->Macros.begin(), document_->Macros.end(),
                           [&](const VisualMacro &m) { return m.Name == name; }))
            name = prefix + "_" + std::to_string(++count);
        return name;
    }
    void Add(int kind) {
        if (!FlushForm())
            return;
        if (!visual_)
            EnterVisual();
        if (!document_)
            return;
        const auto backup = *document_;
        const int oldRow = row_;
        try {
            if (kind == AddRule) {
                if (document_->Rules.size() >= 256)
                    throw std::runtime_error("The visual editor supports 256 rules.");
                document_->Rules.push_back({});
                row_ = static_cast<int>(document_->Rules.size() - 1);
            } else if (kind == AddConstant) {
                if (document_->Constants.size() >= 256)
                    throw std::runtime_error("The visual editor supports 256 constants.");
                VisualConstant c;
                c.Name = "SETTING_" + std::to_string(document_->Constants.size() + 1);
                while (std::any_of(document_->Constants.begin(), document_->Constants.end(),
                                   [&](const VisualConstant &value) { return value.Name == c.Name; }))
                    c.Name = "SETTING_" + std::to_string(std::stoul(c.Name.substr(8)) + 1);
                c.Value = 50;
                document_->Constants.push_back(c);
                row_ = static_cast<int>(document_->Rules.size() + document_->Macros.size() +
                                        document_->Constants.size() - 1);
            } else {
                if (document_->Macros.size() >= 32)
                    throw std::runtime_error("The visual editor supports 32 macros.");
                auto m = kind == Infinity ? VisualDocument::CreateFigureEightPreset() : VisualMacro{};
                m.Name = UniqueMacro(kind == Infinity ? "Infinity" : "Macro");
                if (kind != Infinity)
                    m.Steps.push_back({});
                document_->Macros.push_back(std::move(m));
                row_ = static_cast<int>(document_->Rules.size() + document_->Macros.size() - 1);
            }
            condition_.clear();
            step_ = 0;
            scroll_ = 0;
            CommitDocument();
        } catch (...) {
            document_ = backup;
            row_ = oldRow;
            BuildVisualList();
            throw;
        }
    }
    void EditStructure(int id) {
        if (!FlushForm() || !document_ || row_ < 0)
            return;
        const auto backup = *document_;
        const auto previousCondition = condition_;
        const int previousRow = row_, previousStep = step_;
        try {
            const auto row = rows_[static_cast<std::size_t>(row_)];
            if (id == RemoveItem) {
                if (row.Kind == RowKind::Rule)
                    document_->Rules.erase(document_->Rules.begin() + static_cast<std::ptrdiff_t>(row.Index));
                else if (row.Kind == RowKind::Constant)
                    document_->Constants.erase(document_->Constants.begin() +
                                               static_cast<std::ptrdiff_t>(row.Index));
                else
                    document_->Macros.erase(document_->Macros.begin() +
                                            static_cast<std::ptrdiff_t>(row.Index));
                condition_.clear();
                step_ = 0;
            } else if (row.Kind == RowKind::Macro) {
                auto &m = document_->Macros[row.Index];
                if (id == AddCondition) {
                    auto *node = ConditionAt(m, condition_);
                    if (!node)
                        return;
                    if (node->Kind == VisualConditionKind::All || node->Kind == VisualConditionKind::Any) {
                        if (node->Children.size() >= 16)
                            throw std::runtime_error("A condition group supports 16 children.");
                        node->Children.push_back({});
                        condition_.push_back(node->Children.size() - 1);
                    } else {
                        VisualCondition group;
                        group.Kind = VisualConditionKind::All;
                        group.Children.push_back(*node);
                        group.Children.push_back({});
                        *node = std::move(group);
                        condition_.push_back(1);
                    }
                } else if (id == RemoveCondition) {
                    if (condition_.empty()) {
                        VisualCondition value;
                        value.Kind = VisualConditionKind::Pressed;
                        m.Condition = value;
                    } else {
                        auto parentPath = condition_;
                        const auto index = parentPath.back();
                        parentPath.pop_back();
                        auto *parent = ConditionAt(m, parentPath);
                        if (parent && parent->Children.size() > 1)
                            parent->Children.erase(parent->Children.begin() +
                                                   static_cast<std::ptrdiff_t>(index));
                        else if (parent)
                            parent->Children[0] = {};
                        condition_ = std::move(parentPath);
                    }
                } else if (id == AddStep) {
                    if (m.Steps.size() >= 256)
                        throw std::runtime_error("A visual sequence supports 256 steps.");
                    m.Steps.push_back({});
                    step_ = static_cast<int>(m.Steps.size() - 1);
                } else if (id == RemoveStep) {
                    if (m.Steps.size() <= 1)
                        throw std::runtime_error(
                            "A sequence needs one step. Remove the macro to delete the sequence.");
                    m.Steps.erase(m.Steps.begin() + step_);
                    step_ = std::max(0, step_ - 1);
                } else if (id == UpStep && step_ > 0) {
                    std::swap(m.Steps[static_cast<std::size_t>(step_)],
                              m.Steps[static_cast<std::size_t>(step_ - 1)]);
                    --step_;
                } else if (id == DownStep && static_cast<std::size_t>(step_ + 1) < m.Steps.size()) {
                    std::swap(m.Steps[static_cast<std::size_t>(step_)],
                              m.Steps[static_cast<std::size_t>(step_ + 1)]);
                    ++step_;
                }
            }
            CommitDocument();
        } catch (...) {
            document_ = backup;
            condition_ = previousCondition;
            row_ = previousRow;
            step_ = previousStep;
            BuildVisualList();
            throw;
        }
    }
    void Busy(bool value) {
        busy_ = value;
        SendMessageW(controls_[Code], EM_SETREADONLY, value, 0);
        EnableWindow(form_, !value);
        for (int id : {Library, NewFile, OpenFile, Recover, Save, SaveAs, Inject, Check, AddRule, AddMacro,
                       Infinity, AddConstant, RemoveItem, Live})
            EnableWindow(controls_[id], !value && !(demo_ && id == Inject));
        EnableWindow(controls_[Stop], snapshot_.Running && !value);
        if (value)
            SetTimer(window_, 2, reduced_ ? 150 : 30, nullptr);
        else
            KillTimer(window_, 2);
        InvalidateRect(window_, &editorRect_, FALSE);
    }
    std::uint64_t injectVersion_ = 0;
    void RequestInject() {
        if (demo_ || busy_ || !FlushForm())
            return;
        Busy(true);
        status_ = "Checking configuration…";
        Queue();
        injectVersion_ = version_;
    }
    void Elevated() {
        const auto directory = ScriptStorage::SessionDirectory();
        std::filesystem::create_directories(directory);
        const auto pending = directory / draftPath_.filename();
        ScriptStorage::Write(pending, source_, false);
        if (drafts_)
            drafts_->CancelAndWait();
        ScriptStorage::Write(draftPath_, source_, false);
        std::wstring executable(32768, L'\0');
        executable.resize(
            GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
        std::wstring parameters = L"--session " + Quote(pending.wstring()) + L" --origin " +
                                  Quote(currentPath_.wstring()) + L" --inject";
        if (SendMessageW(controls_[Live], BM_GETCHECK, 0, 0) == BST_CHECKED)
            parameters += L" --live-apply";
        SHELLEXECUTEINFOW request{};
        request.cbSize = sizeof(request);
        request.fMask = SEE_MASK_NOCLOSEPROCESS;
        request.hwnd = window_;
        request.lpVerb = L"runas";
        request.lpFile = executable.c_str();
        request.lpParameters = parameters.c_str();
        request.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&request)) {
            const auto failure = GetLastError();
            std::error_code ignored;
            std::filesystem::remove(pending, ignored);
            Busy(false);
            if (failure == ERROR_CANCELLED) {
                status_ = "Administrator prompt cancelled. Your script is still open.";
                return;
            }
            throw std::runtime_error("Windows could not start the elevated app.");
        }
        if (request.hProcess)
            CloseHandle(request.hProcess);
        closing_ = true;
        DestroyWindow(window_);
    }
    void StartOperation(bool inject, std::shared_ptr<const ScriptProgram> program = {}) {
        if (!service_) {
            Busy(false);
            return;
        }
        if (operation_.joinable())
            operation_.join();
        Busy(true);
        const auto text = source_;
        operation_ = std::thread([this, inject, program = std::move(program), text] {
            auto result = std::make_unique<OperationResult>();
            result->Injected = inject;
            result->Source = text;
            try {
                if (inject)
                    service_->Inject(program);
                else
                    service_->Stop();
            } catch (const std::exception &error) {
                result->Error = error.what();
            }
            if (PostMessageW(window_, OperationMessage, 0, reinterpret_cast<LPARAM>(result.get())))
                result.release();
        });
    }
    void Compiled(std::unique_ptr<CompileResult> result) {
        if (result->Version != version_ || result->Source != source_)
            return;
        accepted_ = result->Version;
        if (source_ != saved_ && drafts_)
            drafts_->Submit(source_);
        if (!result->Error.empty()) {
            Error(result->Error);
            if (injectVersion_) {
                injectVersion_ = 0;
                Busy(false);
            }
            if (closeRequested_)
                Close();
            return;
        }
        if (injectVersion_ == result->Version) {
            injectVersion_ = 0;
            if (closeRequested_) {
                Busy(false);
                Close();
                return;
            }
            try {
                if (!Administrator())
                    Elevated();
                else
                    StartOperation(true, result->Program);
            } catch (...) {
                Busy(false);
                throw;
            }
            return;
        }
        if (!invalidVisual_ && !formDirty_) {
            error_.clear();
            bool applied = false;
            if (!demo_ && !busy_ && service_ &&
                SendMessageW(controls_[Live], BM_GETCHECK, 0, 0) == BST_CHECKED && active_ != source_)
                applied = service_->TryUpdate(result->Program);
            if (applied)
                active_ = source_;
            const bool active = service_ && service_->IsRunning() && active_ == source_;
            status_ = applied  ? "✓ Live changes applied"
                      : active ? "✓ This version is active"
                               : "✓ Valid script · ready to inject";
        }
        UpdateMonitor();
        InvalidateRect(window_, &editorRect_, FALSE);
    }
    void Refresh() {
        if (drafts_) {
            const auto failure = drafts_->TakeError();
            if (!failure.empty()) {
                status_ = "Draft recovery unavailable: " + failure;
                UpdateMonitor();
                InvalidateRect(window_, &editorRect_, FALSE);
            }
        }
        if (busy_ || demo_ || !service_)
            return;
        snapshot_ = service_->GetSnapshot();
        if (!snapshot_.Running)
            active_.clear();
        EnableWindow(controls_[Stop], snapshot_.Running);
        SetTimer(window_, 1, snapshot_.Running ? 150 : 1000, nullptr);
        UpdateMonitor();
        InvalidateRect(window_, &editorRect_, FALSE);
    }
    std::string Values(const ServiceDevice &device) const {
        std::string output;
        int shown = 0;
        for (int phase = 0; phase < 3; ++phase)
            for (std::size_t i = 0; i < 256; ++i) {
                if (!device.Available[i])
                    continue;
                const bool changed = device.OutputValues[i] != device.Values[i];
                const bool active = i >= 192 && i < 208 ? device.Values[i] >= 0 : device.Values[i] != 0;
                if (phase == 0 ? !changed : phase == 1 ? changed || !active : changed || active)
                    continue;
                if (shown++ >= 12 || output.size() > 240)
                    return output + " …";
                if (!output.empty())
                    output += "  ";
                output += Controls::Names()[i] + " " + std::to_string(device.Values[i]);
                if (changed)
                    output += " → " + std::to_string(device.OutputValues[i]);
            }
        return output.empty() ? "Waiting for a physical report" : output;
    }
    void UpdateMonitor() {
        std::ostringstream out;
        out << "GamePad Remapper 2.0 · native C++\n"
            << (demo_ ? "SIMULATED PREVIEW\nNo driver or controller connection." : snapshot_.Status)
            << "\n\n";
        if (demo_)
            out << "Demo controller\nLX 0  LY 0  RX 0  RY 0\nPreview only; no latency results.\n\n";
        else {
            out << (snapshot_.Running ? "● Configuration active" : "○ Not capturing") << "\n"
                << snapshot_.Received << " received / " << snapshot_.Forwarded << " forwarded\n"
                << snapshot_.Changed << " changed / " << snapshot_.TimerOutputs << " timed outputs\n"
                << std::fixed << std::setprecision(2) << snapshot_.ProcessingMicroseconds
                << " µs software processing\n\n";
            if (snapshot_.Devices.empty())
                out << "CONTROLLERS\nChoose Inject to connect. Waiting for compatible controllers while "
                       "capturing.\n\n";
            for (const auto &device : snapshot_.Devices) {
                out << device.Name << "\n"
                    << device.Token << "\n"
                    << std::count(device.Available.begin(), device.Available.end(), true)
                    << " available controls\n"
                    << Values(device) << "\n"
                    << device.Diagnostic << "\n\n";
                if (device.Family == 3)
                    out << "Generic HID: choose reported AXIS / BUTTON / VALUE controls; face aliases are "
                           "not guessed.\n\n";
            }
        }
        out << "DIAGNOSTICS\n"
            << (!error_.empty() ? error_ : status_) << "\n"
            << snapshot_.Error
            << "\n\nDriver demo: 2 hours after the first capture per Windows boot.\nStop or close restores "
               "normal input.\nSoftware timing excludes USB/controller/display latency.";
        diagnostic_ = out.str();
        SetText(controls_[Monitor], Utf16(diagnostic_));
        Rich(controls_[Monitor], false);
    }
    void Close() {
        if (closing_)
            return;
        if (busy_) {
            closeRequested_ = true;
            status_ = "Finishing the current operation before closing…";
            return;
        }
        closeRequested_ = false;
        if (!Confirm())
            return;
        closing_ = true;
        RetireDrafts();
        DestroyWindow(window_);
    }
    void Command(int id, int event) {
        if (building_)
            return;
        if (id == Code && event == EN_CHANGE) {
            SourceChanged();
            return;
        }
        if (id >= Property) {
            if (event == EN_CHANGE || event == CBN_EDITCHANGE) {
                formDirty_ = true;
                invalidVisual_ = false;
                return;
            }
            if (event == CBN_SELCHANGE || event == BN_CLICKED) {
                formDirty_ = true;
                FlushForm(true, id);
                return;
            }
            if (event == EN_KILLFOCUS || event == CBN_KILLFOCUS)
                FlushForm(false, id);
            return;
        }
        if (id == TriggerTree || id == Form)
            return;
        if (id == Steps && event == LBN_SELCHANGE) {
            const int selected = static_cast<int>(SendMessageW(fields_.at(Steps), LB_GETCURSEL, 0, 0));
            if (FlushForm()) {
                step_ = selected;
                BuildForm();
            } else
                SendMessageW(fields_.at(Steps), LB_SETCURSEL, step_, 0);
            return;
        }
        if (id == VisualList && event == LBN_SELCHANGE) {
            const int selected = static_cast<int>(SendMessageW(controls_[VisualList], LB_GETCURSEL, 0, 0));
            if (FlushForm()) {
                row_ = selected;
                condition_.clear();
                scroll_ = 0;
                step_ = 0;
                BuildForm();
            } else
                SendMessageW(controls_[VisualList], LB_SETCURSEL, row_, 0);
            return;
        }
        if (id == Library && event == LBN_SELCHANGE) {
            const int selected = static_cast<int>(SendMessageW(controls_[Library], LB_GETCURSEL, 0, 0));
            if (selected >= 0 && static_cast<std::size_t>(selected) < libraryFiles_.size() && Confirm()) {
                const auto path = libraryFiles_[static_cast<std::size_t>(selected)];
                try {
                    Open(path);
                } catch (...) {
                    LoadLibrary();
                    throw;
                }
            } else
                LoadLibrary();
            return;
        }
        if (event != BN_CLICKED)
            return;
        switch (id) {
        case NewFile:
            if (!busy_ && Confirm()) {
                RetireDrafts();
                SetSource("// GamePad Remapper · IGC\nmain {\n}\n", {}, true);
                LoadLibrary();
            }
            break;
        case OpenFile:
            if (!busy_ && Confirm()) {
                auto path = FileDialog(false);
                if (!path.empty())
                    Open(path);
            }
            break;
        case Save:
            SaveFile(false);
            break;
        case SaveAs:
            SaveFile(true);
            break;
        case CodeTab:
            if (FlushForm()) {
                visual_ = false;
                Visibility();
            }
            break;
        case VisualTab:
            EnterVisual();
            break;
        case FindNext:
            Find();
            break;
        case Check:
            if (FlushForm()) {
                status_ = "Checking script…";
                Queue();
            }
            break;
        case Inject:
            RequestInject();
            break;
        case Stop:
            if (!busy_)
                StartOperation(false);
            break;
        case Live:
            Queue();
            break;
        case Reduce:
            reduced_ = SendMessageW(controls_[Reduce], BM_GETCHECK, 0, 0) == BST_CHECKED;
            break;
        case Copy:
            Clipboard(window_, Utf16(diagnostic_));
            break;
        case Folder:
            std::filesystem::create_directories(libraryDirectory_);
            ShellExecuteW(window_, L"open", libraryDirectory_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case Driver: {
            const auto installer = ExecutableDirectory() / L"InterceptorSetup.exe";
            ShellExecuteW(window_, L"open",
                          std::filesystem::exists(installer)
                              ? installer.c_str()
                              : L"https://github.com/TitanCSA/Interceptor-Gamepad-Controller/releases/latest",
                          nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case Recover:
            if (Confirm()) {
                const auto path = RecoveryCandidate();
                if (!path.empty()) {
                    const auto source = ScriptStorage::Read(path);
                    RetireDrafts();
                    SetSource(source, {}, true);
                    consumedDraft_ = path;
                    LoadLibrary();
                }
            }
            break;
        case AddRule:
        case AddMacro:
        case AddConstant:
        case Infinity:
            Add(id);
            break;
        case RemoveItem:
        case AddCondition:
        case RemoveCondition:
        case AddStep:
        case RemoveStep:
        case UpStep:
        case DownStep:
            EditStructure(id);
            break;
        default:
            break;
        }
        InvalidateRect(window_, &editorRect_, FALSE);
    }
    LRESULT Message(UINT message, WPARAM w, LPARAM l) {
        switch (message) {
        case WM_CREATE:
            dpi_ = GetDpiForWindow(window_);
            Fonts();
            panelBrush_ = CreateSolidBrush(Panel);
            editBrush_ = CreateSolidBrush(Raised);
            CreateControls();
            return 0;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto *info = reinterpret_cast<MINMAXINFO *>(l);
            info->ptMinTrackSize = {S(1120), S(720)};
            return 0;
        }
        case WM_DPICHANGED:
            dpi_ = HIWORD(w);
            Fonts();
            for (const auto &item : controls_)
                SendMessageW(item.second, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            Rich(controls_[Code], true);
            Rich(controls_[Monitor], false);
            for (const auto &item : fields_)
                SendMessageW(item.second, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            for (const auto label : labels_)
                SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            {
                const auto *r = reinterpret_cast<RECT *>(l);
                SetWindowPos(window_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            Layout();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(window_, &ps);
            Paint(dc);
            EndPaint(window_, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            Paint(reinterpret_cast<HDC>(w));
            return 0;
        case WM_DRAWITEM:
            DrawItem(*reinterpret_cast<DRAWITEMSTRUCT *>(l));
            return TRUE;
        case WM_MEASUREITEM:
            reinterpret_cast<MEASUREITEMSTRUCT *>(l)->itemHeight = static_cast<UINT>(S(52));
            return TRUE;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            auto dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, Ink);
            SetBkColor(dc, message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN ? Panel : Raised);
            return reinterpret_cast<LRESULT>(
                message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN ? panelBrush_ : editBrush_);
        }
        case WM_COMMAND:
            Command(LOWORD(w), HIWORD(w));
            return 0;
        case WM_NOTIFY: {
            auto *header = reinterpret_cast<NMHDR *>(l);
            if (header->idFrom == TriggerTree && header->code == TVN_SELCHANGINGW && !building_) {
                auto *value = reinterpret_cast<NMTREEVIEWW *>(l);
                if (!FlushForm())
                    return TRUE;
                const auto index = static_cast<std::size_t>(value->itemNew.lParam);
                if (index < treePaths_.size()) {
                    condition_ = treePaths_[index];
                    PostMessageW(window_, WM_APP + 25, 0, 0);
                }
            }
            return 0;
        }
        case WM_APP + 25:
            BuildForm();
            return 0;
        case WM_TIMER:
            if (w == 1)
                Refresh();
            else if (w == 2)
                InvalidateRect(window_, &editorRect_, FALSE);
            return 0;
        case CompiledMessage:
            Compiled(std::unique_ptr<CompileResult>(reinterpret_cast<CompileResult *>(l)));
            return 0;
        case OperationMessage: {
            std::unique_ptr<OperationResult> result(reinterpret_cast<OperationResult *>(l));
            if (operation_.joinable())
                operation_.join();
            Busy(false);
            if (result->Error.empty()) {
                active_ = result->Injected ? result->Source : std::string{};
                error_.clear();
                status_ = result->Injected ? "✓ Configuration applied atomically"
                                           : "Session stopped · normal input restored";
            } else
                Error(result->Error);
            Refresh();
            if (closeRequested_)
                Close();
            return 0;
        }
        case WM_CLOSE:
            Close();
            return 0;
        case WM_DESTROY:
            KillTimer(window_, 1);
            KillTimer(window_, 2);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window_, message, w, l);
        }
    }

  public:
    MainWindow(HINSTANCE instance, bool demo, const std::filesystem::path &scripts = {})
        : instance_(instance), demo_(demo),
          libraryDirectory_(scripts.empty() ? ScriptStorage::DefaultDirectory() : scripts) {
        const auto recovery = demo_ ? libraryDirectory_ / L".recovery" : ScriptStorage::RecoveryDirectory();
        draftPath_ = ScriptStorage::UniquePath(recovery);
        if (!demo_)
            service_ = std::make_unique<RemapperService>();
        WNDCLASSEXW type{};
        type.cbSize = sizeof(type);
        type.lpfnWndProc = Procedure;
        type.hInstance = instance_;
        type.lpszClassName = L"GprMainWindow";
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        RegisterClassExW(&type);
        type.lpfnWndProc = FormProcedure;
        type.lpszClassName = L"GprVisualForm";
        RegisterClassExW(&type);
        window_ =
            CreateWindowExW(0, L"GprMainWindow", L"GamePad Remapper", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1440, 930, nullptr, nullptr, instance_, this);
        if (!window_)
            throw std::runtime_error("The application window could not be created.");
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));
        drafts_ = std::make_unique<DraftWriter>(draftPath_);
        compiler_ = std::make_unique<CompilerWorker>(window_);
        LoadLibrary();
        SetSource("// GamePad Remapper · IGC\n// Code and visual edits share one file.\nmain {\n}\n", {},
                  false);
        status_ = "Ready · scripts are checked automatically";
        Busy(false);
        UpdateMonitor();
        SetTimer(window_, 1, 1000, nullptr);
    }
    ~MainWindow() {
        compiler_.reset();
        drafts_.reset();
        if (operation_.joinable())
            operation_.join();
        if (service_)
            service_->Stop();
        MSG message{};
        while (PeekMessageW(&message, window_, CompiledMessage, OperationMessage, PM_REMOVE)) {
            if (message.message == CompiledMessage)
                delete reinterpret_cast<CompileResult *>(message.lParam);
            else if (message.message == OperationMessage)
                delete reinterpret_cast<OperationResult *>(message.lParam);
        }
        if (IsWindow(window_))
            DestroyWindow(window_);
        if (font_)
            DeleteObject(font_);
        if (codeFont_)
            DeleteObject(codeFont_);
        if (titleFont_)
            DeleteObject(titleFont_);
        if (panelBrush_)
            DeleteObject(panelBrush_);
        if (editBrush_)
            DeleteObject(editBrush_);
    }
    HWND Handle() const { return window_; }
    bool Shortcut(MSG &message) {
        if (message.message != WM_KEYDOWN || !(GetKeyState(VK_CONTROL) & 0x8000))
            return false;
        switch (message.wParam) {
        case 'S':
            if (!busy_)
                SaveFile((GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return true;
        case 'O':
            Command(OpenFile, BN_CLICKED);
            return true;
        case 'N':
            Command(NewFile, BN_CLICKED);
            return true;
        case 'F':
            SetFocus(controls_[FindText]);
            SendMessageW(controls_[FindText], EM_SETSEL, 0, -1);
            return true;
        case VK_RETURN:
            RequestInject();
            return true;
        default:
            return false;
        }
    }
    void Arguments(const std::vector<std::wstring> &args) {
        SendMessageW(controls_[Live], BM_SETCHECK, Has(args, L"--live-apply") ? BST_CHECKED : BST_UNCHECKED,
                     0);
        const auto session = Argument(args, L"--session"), open = Argument(args, L"--open");
        if (!session.empty()) {
            const auto full = std::filesystem::absolute(session);
            const auto allowed = std::filesystem::absolute(ScriptStorage::SessionDirectory());
            if (full.parent_path() != allowed || full.extension() != L".igc")
                throw std::runtime_error("Invalid elevation session path.");
            const auto source = ScriptStorage::Read(full);
            const auto origin = Argument(args, L"--origin");
            SetSource(source, origin, true);
            if (!origin.empty() && std::filesystem::exists(origin))
                saved_ = ScriptStorage::Read(origin);
            const auto previous = draftPath_.parent_path() / full.filename();
            if (std::filesystem::exists(previous))
                consumedDraft_ = previous;
            std::filesystem::remove(full);
            LoadLibrary();
        } else if (!open.empty())
            Open(open);
        if (Has(args, L"--inject"))
            RequestInject();
    }
    void Test(std::function<void(bool, const std::string &)> require, const std::filesystem::path &directory);
    void Preview(bool macro) {
        error_.clear();
        status_ = "Simulated preview - scripts compile natively";
        SetWindowTextW(controls_[FindText], L"");
        SendMessageW(controls_[Live], BM_SETCHECK, BST_UNCHECKED, 0);
        auto doc = VisualDocument::Parse("// Everyday controller\ndefine CAMERA_SPEED = 115;\nmain {\n    // "
                                         "Add advanced logic here.\n}\n");
        doc.Rules.push_back({VisualRuleKind::Deadzone, "LX", "EAST", 8, true});
        doc.Rules.push_back({VisualRuleKind::Invert, "RY", "EAST", 0, true});
        doc.Macros.push_back(VisualDocument::CreateFigureEightPreset());
        SetSource(doc.BuildSource(), directoryForPreview(), false);
        ScriptStorage::Write(directoryForPreview(), source_, false);
        LoadLibrary();
        EnterVisual();
        if (macro) {
            row_ = static_cast<int>(doc.Rules.size());
            BuildVisualList();
            scroll_ = 0;
            BuildForm();
        }
        UpdateMonitor();
    }
    std::filesystem::path directoryForPreview() const {
        return libraryDirectory_ / L"Everyday controller.igc";
    }
    void CodePreview() {
        const auto path = libraryDirectory_ / L"07 - Additive Figure Eight.igc";
        if (std::filesystem::exists(path))
            SetSource(ScriptStorage::Read(path), path, false);
        visual_ = false;
        Visibility();
        LoadLibrary();
    }
};

void PumpUntil(const std::function<bool()> &complete, DWORD timeout = 10000) {
    const auto start = GetTickCount64();
    while (!complete()) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message != WM_QUIT) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (GetTickCount64() - start > timeout)
            throw std::runtime_error("The native interface operation exceeded its test deadline.");
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}
void MainWindow::Test(std::function<void(bool, const std::string &)> require,
                      const std::filesystem::path &directory) {
    require(demo_ && !service_, "Offline UI never constructs a capture service or accesses the driver");
    require(IsWindow(controls_[Code]) && IsWindow(controls_[Library]),
            "Native Windows code editor and library controls exist");
    const std::string initial = "// Exact source must survive mode changes.\nmain {\n}\n";
    SetSource(initial, {}, false);
    EnterVisual();
    Command(CodeTab, BN_CLICKED);
    require(source_ == initial && !visual_,
            "Switching code and visual modes preserves exact source without editing");
    Add(AddRule);
    require(document_ && document_->Rules.size() == 1,
            "Adding an input rule updates the source-backed visual document");
    SetWindowTextW(fields_.at(Source), L"LY");
    SendMessageW(fields_.at(Kind), CB_SETCURSEL, static_cast<WPARAM>(VisualRuleKind::Invert), 0);
    formDirty_ = true;
    require(FlushForm(true, Kind), "Visual rule selectors compile valid changes");
    const auto valid = source_;
    SetWindowTextW(fields_.at(Source), L"UNKNOWN_CONTROL");
    formDirty_ = true;
    require(!FlushForm() && invalidVisual_ && source_ == valid,
            "Invalid visual values keep the last valid source unchanged");
    Command(CodeTab, BN_CLICKED);
    require(visual_, "Invalid visual edits remain visible when switching modes");
    discardPrompt_ = [](const auto &) { return IDCANCEL; };
    require(!Confirm() && invalidVisual_, "Cancelling discard keeps the invalid edit available");
    discardPrompt_ = [](const auto &) { return IDNO; };
    require(Confirm() && !invalidVisual_ && source_ == valid,
            "Invalid visual edits can be discarded without trapping the app");
    discardPrompt_ = nullptr;
    Add(Infinity);
    require(document_->Macros.back().Condition->Children.size() == 3 &&
                document_->Macros.back().Pattern.Blend == VisualBlend::Add,
            "One-click infinity combines A, B, camera threshold and additive axis motion");
    auto parsed = VisualDocument::Parse(source_);
    require(parsed.Macros.back().Pattern.Shape == VisualPatternShape::FigureEight,
            "Complex visual motion survives source roundtrip");
    const auto compoundSource = source_;
    bool compoundRetained = true, negationStable = true;
    for (int i = 0; i < 8; ++i) {
        SendMessageW(fields_.at(ConditionKind), CB_SETCURSEL, static_cast<WPARAM>(VisualConditionKind::Not),
                     0);
        formDirty_ = true;
        compoundRetained = compoundRetained && FlushForm(true, ConditionKind) &&
                           document_->Macros.back().Condition->Children.size() == 1 &&
                           document_->Macros.back().Condition->Children.front().Children.size() == 3;
        if (!compoundRetained)
            break;
        SendMessageW(fields_.at(ConditionKind), CB_SETCURSEL, static_cast<WPARAM>(VisualConditionKind::All),
                     0);
        formDirty_ = true;
        negationStable = negationStable && FlushForm(true, ConditionKind) && source_ == compoundSource;
        if (!negationStable)
            break;
    }
    require(compoundRetained, "Negating a compound trigger retains all buttons and axis thresholds");
    require(negationStable, "Repeated negation toggles restore the exact trigger without growing its depth");
    condition_ = {1};
    BuildForm();
    SendMessageW(fields_.at(ConditionKind), CB_SETCURSEL, static_cast<WPARAM>(VisualConditionKind::Not), 0);
    formDirty_ = true;
    require(FlushForm(true, ConditionKind) &&
                document_->Macros.back().Condition->Children[1].Children.front().Control == "EAST",
            "Negating a configured leaf retains its original controller button");
    SendMessageW(fields_.at(ConditionKind), CB_SETCURSEL, static_cast<WPARAM>(VisualConditionKind::Held), 0);
    formDirty_ = true;
    require(FlushForm(true, ConditionKind) && source_ == compoundSource,
            "Removing a leaf negation restores its exact prior condition");
    condition_.clear();
    BuildForm();
    SendMessageW(fields_.at(Kind), CB_SETCURSEL, static_cast<WPARAM>(VisualMacroKind::Sequence), 0);
    formDirty_ = true;
    require(FlushForm(true, Kind) && !document_->Macros.back().Steps.empty(),
            "Switching a motion to a sequence provides a valid editable step");
    SendMessageW(fields_.at(ConditionKind), CB_SETCURSEL,
                 static_cast<WPARAM>(VisualConditionKind::Expression), 0);
    formDirty_ = true;
    require(FlushForm(true, ConditionKind) && fields_.count(Expression) != 0,
            "Switching to an expression supplies a valid editable default");
    Add(AddMacro);
    EditStructure(AddStep);
    require(document_->Macros.back().Steps.size() == 2, "The sequence editor adds multiple timed steps");
    SetWindowTextW(fields_.at(StepValue), L"45");
    SetWindowTextW(fields_.at(StepDuration), L"25");
    formDirty_ = true;
    require(FlushForm(), "Timed step values and durations are editable");
    EditStructure(UpStep);
    require(document_->Macros.back().Steps[0].Value == 45,
            "Timed steps can be reordered without losing their values");
    SetWindowTextW(fields_.at(StepDuration), L"invalid");
    formDirty_ = true;
    const auto durationControl = fields_.at(StepDuration);
    Layout();
    require(fields_.at(StepDuration) == durationControl && Text(durationControl) == L"invalid",
            "Window layout preserves pending visual field edits");
    SendMessageW(fields_.at(Steps), LB_SETCURSEL, 1, 0);
    Command(Steps, LBN_SELCHANGE);
    require(step_ == 0 && SendMessageW(fields_.at(Steps), LB_GETCURSEL, 0, 0) == 0 && invalidVisual_,
            "Invalid step edits keep the selection and fields on the same step");
    SetWindowTextW(fields_.at(StepDuration), L"25");
    formDirty_ = true;
    require(FlushForm(), "Correcting a step error restores a valid sequence");
    EditStructure(RemoveStep);
    require(document_->Macros.back().Steps.size() == 1,
            "Timed steps can be removed while retaining a valid sequence");
    Add(AddConstant);
    SetWindowTextW(fields_.at(Name), L"MY_SETTING");
    SetWindowTextW(fields_.at(Value), L"9");
    formDirty_ = true;
    require(FlushForm() && source_.find("define MY_SETTING = 9") != std::string::npos,
            "Constants are edited directly into the shared IGC source");
    Command(CodeTab, BN_CLICKED);
    SetWindowTextW(controls_[FindText], L"LY");
    Find();
    CHARRANGE selection{};
    SendMessageW(controls_[Code], EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    require(selection.cpMax - selection.cpMin == 2, "Native code search selects the matching control name");
    SetSource("define SETTING_2 = 7; main {}", {}, false);
    EnterVisual();
    Add(AddConstant);
    require(document_->Constants.size() == 2 && document_->Constants[0].Name != document_->Constants[1].Name,
            "New constants automatically avoid existing setting-name collisions");
    auto deep = VisualDocument::Parse("main {}");
    auto deepMacro = VisualDocument::CreateFigureEightPreset();
    VisualCondition node;
    node.Kind = VisualConditionKind::Held;
    for (int i = 0; i < 12; ++i) {
        VisualCondition group;
        group.Kind = VisualConditionKind::Not;
        group.Children.push_back(node);
        node = std::move(group);
    }
    deepMacro.Condition = node;
    deep.Macros.push_back(deepMacro);
    SetSource(deep.BuildSource(), {}, false);
    EnterVisual();
    row_ = 0;
    condition_ = std::vector<std::size_t>(12, 0);
    BuildVisualList();
    const auto beforeStructure = source_;
    bool structureRejected = false;
    try {
        EditStructure(AddCondition);
    } catch (const std::exception &) {
        structureRejected = true;
    }
    require(structureRejected && source_ == beforeStructure && document_->BuildSource() == beforeStructure,
            "Rejected condition-tree edits roll back both source and visual state");
    Command(CodeTab, BN_CLICKED);
    SendMessageW(controls_[Live], BM_SETCHECK, BST_CHECKED, 0);
    for (int i = 0; i < 30; ++i) {
        settingCode_ = true;
        SetWindowTextW(controls_[Code], (L"main {} // edit " + std::to_wstring(i)).c_str());
        settingCode_ = false;
        SourceChanged();
    }
    const auto newest = version_;
    PumpUntil([&] { return accepted_ == newest; });
    require(source_.find("edit 29") != std::string::npos && status_.find("Valid") != std::string::npos,
            "Debounced background validation accepts only the newest source");
    require(!service_ && active_.empty(), "Live apply is harmless without an explicitly started capture");
    RequestInject();
    require(!busy_ && !service_, "Demo mode rejects capture injection");
    SetSource("main { set_val(SOUTH, ); }", {}, true);
    const auto invalidVersion = version_;
    PumpUntil([&] { return accepted_ == invalidVersion; });
    require(!error_.empty(), "Compiler diagnostics return to the native interface");
    drafts_->CancelAndWait();
    require(std::filesystem::exists(draftPath_) && ScriptStorage::Read(draftPath_) == source_,
            "Draft recovery also retains unfinished invalid code");
    drafts_->Submit("main {} // pending draft");
    RetireDrafts();
    require(!std::filesystem::exists(draftPath_),
            "Saving or discarding cancels pending drafts without resurrecting retired files");
    SetSource("main {}\n", {}, false);
    const auto file = directory / L"atomic.igc";
    ScriptStorage::Write(file, "main {} // first\n");
    ScriptStorage::Write(file, "main {} // second\n");
    require(ScriptStorage::Read(file) == "main {} // second\n" &&
                ScriptStorage::Read(file.wstring() + L".bak") == "main {} // first\n",
            "Atomic saves retain the exact previous version in a backup");
    ScriptStorage::Write(file, "// Unicode é 🎮\nmain {}\n");
    require(ScriptStorage::Read(file) == "// Unicode é 🎮\nmain {}\n",
            "UTF-8 storage preserves accented text and supplementary characters");
    const auto upper = libraryDirectory_ / L"UPPER.IGC";
    ScriptStorage::Write(upper, "main {}", false);
    LoadLibrary();
    require(std::find(libraryFiles_.begin(), libraryFiles_.end(), upper) != libraryFiles_.end(),
            "The script library accepts uppercase Windows file extensions");
    std::filesystem::remove(upper);
    LoadLibrary();
    const auto bad = directory / L"bad.igc";
    {
        std::ofstream stream(bad, std::ios::binary);
        stream.write("\xc3\x28", 2);
    }
    bool rejected = false;
    try {
        (void)ScriptStorage::Read(bad);
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "Malformed UTF-8 is rejected instead of silently replacing text");
    rejected = false;
    try {
        ScriptStorage::Write(file, std::string("main{}\0bad", 10));
    } catch (const std::exception &) {
        rejected = true;
    }
    require(rejected, "Embedded zero characters cannot silently truncate scripts");
    const auto recovery = ScriptStorage::UniquePath(draftPath_.parent_path());
    ScriptStorage::Write(recovery, "main {} // recovered\n", false);
    require(RecoveryCandidate() == recovery, "Unsaved draft recovery finds a recoverable script");
    consumedDraft_ = recovery;
    currentPath_ = file;
    source_ = ScriptStorage::Read(recovery);
    settingCode_ = true;
    SetWindowTextW(controls_[Code], Utf16(source_).c_str());
    settingCode_ = false;
    require(SaveFile(false) && !std::filesystem::exists(recovery),
            "Saving a recovered script retires its consumed draft");
    ServiceDevice device;
    device.Available[32] = device.Available[159] = true;
    device.Values[159] = -40;
    device.OutputValues[159] = 30;
    require(Values(device).find("AXIS_64 -40 → 30") != std::string::npos,
            "The live monitor prioritizes changed dynamic HID controls");
    discardPrompt_ = nullptr;
    SetSource(initial, {}, false);
    Command(CodeTab, BN_CLICKED);
    UpdateMonitor();
}

struct UiEnvironment final {
    HMODULE rich = nullptr;
    HRESULT com = E_FAIL;
    UiEnvironment() {
        com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        INITCOMMONCONTROLSEX controls{sizeof(controls),
                                      ICC_STANDARD_CLASSES | ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&controls);
        rich = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!rich)
            throw std::runtime_error("The built-in Windows rich text editor is unavailable.");
    }
    ~UiEnvironment() {
        if (rich)
            FreeLibrary(rich);
        if (SUCCEEDED(com))
            CoUninitialize();
    }
};
void Screenshot(HWND window, const std::filesystem::path &path) {
    RECT area{};
    GetWindowRect(window, &area);
    const int width = area.right - area.left, height = area.bottom - area.top;
    HDC screen = GetDC(window), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    const auto old = SelectObject(memory, bitmap);
    RedrawWindow(window, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_INVALIDATE);
    if (!PrintWindow(window, memory, 2)) {
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(window, screen);
        throw std::runtime_error("Windows could not render the native interface.");
    }
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> image;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT result =
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result))
        result = factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &image);
    if (SUCCEEDED(result))
        result = factory->CreateStream(&stream);
    if (SUCCEEDED(result))
        result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(result))
        result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result))
        result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(result))
        result = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(result))
        result = frame->Initialize(nullptr);
    if (SUCCEEDED(result))
        result = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGR;
    if (SUCCEEDED(result))
        result = frame->SetPixelFormat(&format);
    if (SUCCEEDED(result))
        result = frame->WriteSource(image.Get(), nullptr);
    if (SUCCEEDED(result))
        result = frame->Commit();
    if (SUCCEEDED(result))
        result = encoder->Commit();
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, screen);
    if (FAILED(result))
        throw std::runtime_error("The native UI screenshot could not be encoded.");
}
} // namespace

int RunApplication(HINSTANCE instance, int show, const std::vector<std::wstring> &arguments) {
    UiEnvironment environment;
    if (Has(arguments, L"--help")) {
        MessageBoxW(nullptr,
                    L"GamePadRemapper.exe [--open file.igc] [--demo]\n\n--demo: simulated UI with no driver "
                    L"access\n--ui-self-test report.json: offline native interface and storage "
                    L"tests\n--render-tests directory: render the actual native interface\n\nCtrl+Enter "
                    L"applies; Ctrl+S saves; Ctrl+F finds text.",
                    L"GamePad Remapper", MB_OK);
        return 0;
    }
    MainWindow window(instance, Has(arguments, L"--demo"));
    window.Arguments(arguments);
    ShowWindow(window.Handle(), show);
    UpdateWindow(window.Handle());
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        try {
            if (!window.Shortcut(message) && !IsDialogMessageW(window.Handle(), &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        } catch (const std::exception &error) {
            MessageBoxW(window.Handle(), Utf16(error.what()).c_str(), L"GamePad Remapper",
                        MB_OK | MB_ICONERROR);
        }
    }
    return static_cast<int>(message.wParam);
}
int RunUiTests(const std::wstring &report, const std::wstring &renderDirectory) {
    nlohmann::ordered_json output;
    output["Passed"] = false;
    output["CaptureStarted"] = false;
    output["DriverAccess"] = false;
    output["Runtime"] = "C++17 / native Win32";
    std::vector<std::string> checks;
    const auto start = std::chrono::steady_clock::now();
    int exit = 1;
    try {
        UiEnvironment environment;
        wchar_t temp[32768]{};
        GetTempPathW(32768, temp);
        const auto directory = std::filesystem::path(temp) /
                               (L"GamePadRemapper-UI-" + ScriptStorage::UniquePath({}).stem().wstring());
        std::filesystem::create_directories(directory);
        MainWindow window(GetModuleHandleW(nullptr), true, directory / L"Scripts");
        ShowWindow(window.Handle(), SW_SHOWNOACTIVATE);
        UpdateWindow(window.Handle());
        window.Test(
            [&](bool condition, const std::string &name) {
                if (!condition)
                    throw std::runtime_error(name);
                checks.push_back(name);
            },
            directory);
        if (!renderDirectory.empty()) {
            const std::filesystem::path destination(renderDirectory);
            std::filesystem::create_directories(destination);
            window.Preview(false);
            window.CodePreview();
            Screenshot(window.Handle(), destination / L"code-editor.png");
            window.Preview(false);
            Screenshot(window.Handle(), destination / L"visual-editor.png");
            window.Preview(true);
            Screenshot(window.Handle(), destination / L"macro-editor.png");
            SetWindowPos(window.Handle(), nullptr, 0, 0, 1280, 800,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            Screenshot(window.Handle(), destination / L"compact-layout.png");
            checks.push_back("Rendered actual native code, visual, macro and compact interface");
        }
        output["Passed"] = true;
        output["TemporaryDirectory"] = Utf8(directory.wstring());
        exit = 0;
    } catch (const std::exception &error) {
        output["Error"] = error.what();
    }
    output["Checks"] = checks;
    output["ElapsedMilliseconds"] =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (!report.empty()) {
        const auto path = std::filesystem::path(report);
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary);
        file << output.dump(2) << '\n';
    }
    return exit;
}
} // namespace gpr
