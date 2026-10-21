#include "../UI/ControllerMonitorView.h"
#include "../UI/Theme.h"
#include <Windows.h>
#include <CommCtrl.h>
#include <OleAcc.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {
unsigned checks = 0;
void Check(bool value, const char *reason) {
    ++checks;
    if (!value) throw std::runtime_error(reason);
}
void Pump() {
    MSG message{};
    unsigned count = 0;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (++count > 20000) throw std::runtime_error("Monitor messages did not settle.");
        TranslateMessage(&message); DispatchMessageW(&message);
    }
}
struct Window {
    HWND Value = nullptr;
    explicit Window(HWND window) : Value(window) { if (!window) throw std::runtime_error("Window creation failed."); }
    ~Window() { if (IsWindow(Value)) DestroyWindow(Value); }
    operator HWND() const { return Value; }
};
gpr::ServiceDevice Device(unsigned key, unsigned family, const char *token) {
    gpr::ServiceDevice device;
    device.Key = key; device.Family = family; device.Token = token; device.Name = "Offline controller fixture";
    device.Telemetry = true; device.PhysicalValid = device.OutputValid = true;
    for (unsigned slot : {0u, 1u, 4u, 10u, 17u}) {
        device.Available[slot] = device.InputAvailable[slot] = device.OutputAvailable[slot] = true;
    }
    device.InputSequence = 11; device.OutputSequence = 12;
    return device;
}
const gpr::ui::ControllerMonitorRow &Row(const gpr::ui::ControllerMonitorState &state, int slot) {
    const auto found = std::find_if(state.Rows.begin(), state.Rows.end(), [slot](const auto &row) { return row.Slot == slot; });
    if (found == state.Rows.end()) throw std::runtime_error("Expected control was not presented.");
    return *found;
}
std::vector<std::uint32_t> Pixels(HWND window, int width, int height) {
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(window), memory = CreateCompatibleDC(dc); void *raw = nullptr;
    const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &raw, nullptr, 0);
    if (!bitmap || !raw) throw std::runtime_error("Offscreen monitor render allocation failed.");
    const auto old = SelectObject(memory, bitmap);
    SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory), PRF_CLIENT);
    std::vector<std::uint32_t> result(static_cast<std::uint32_t *>(raw), static_cast<std::uint32_t *>(raw) + width * height);
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, dc);
    return result;
}
bool RectEqual(const RECT &first, const RECT &second) {
    return first.left == second.left && first.top == second.top && first.right == second.right && first.bottom == second.bottom;
}
} // namespace
int main() {
    try {
        const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const auto instance = GetModuleHandleW(nullptr);
        Window parent(CreateWindowExW(0, L"STATIC", L"Offline controller monitor test", WS_OVERLAPPEDWINDOW,
                                      0, 0, 720, 700, nullptr, nullptr, instance, nullptr));
        Window view(gpr::ui::CreateControllerMonitorView(parent, 50, instance));
        MoveWindow(view, 0, 0, 510, 540, FALSE);
        gpr::ui::SetControllerMonitorDpi(view, 96);
        gpr::ServiceSnapshot snapshot; snapshot.Monitoring = true;
        gpr::ui::UpdateControllerMonitorView(view, snapshot);
        auto state = gpr::ui::InspectControllerMonitorView(view);
        Check(state.DeviceCount == 0 && state.SelectedIndex == -1 && state.Rows.empty(), "Empty monitor has no fabricated controller or controls.");
        Pixels(view, 510, 540);
        snapshot.Devices = {Device(1, 3, "hid:first"), Device(1, 4, "xusb:first"), Device(1, 6, "gip:first")};
        snapshot.Devices[0].Values[0] = -20; snapshot.Devices[0].OutputValues[0] = 35;
        snapshot.Devices[0].InputAvailable[4] = false;
        gpr::ui::UpdateControllerMonitorView(view, snapshot);
        state = gpr::ui::InspectControllerMonitorView(view);
        Check(state.DeviceCount == 3 && TabCtrl_GetItemCount(state.Tabs) == 3, "Each connected family has a separate native controller tab.");
        Check(state.SelectedToken == "hid:first" && state.Rows.size() == 5, "Initial selection exposes only one controller's controls.");
        Check(Row(state, 0).InputText == L"-20%" && Row(state, 0).OutputText == L"35%" && Row(state, 0).Different,
              "Physical and observed output values remain distinct and expose modification.");
        Check(!Row(state, 4).InputKnown && Row(state, 4).InputText == L"\u2014" && Row(state, 4).OutputText == L"0%",
              "An unknown field is visibly distinct from a measured zero.");
        Check(Row(state, 10).InputText == L"OFF", "Digital controls use readable OFF and ON states.");
        ShowWindow(parent, SW_SHOWNOACTIVATE);
        Pump();
        const auto beforePaint = state;
        const auto rendered = Pixels(view, 510, 540);
        Check(!rendered.empty() && (rendered.front() & 0x00ffffffu) == 0x00111111u,
              "The live monitor uses the application's dark surface rather than a white canvas.");
        Check(std::any_of(rendered.begin(), rendered.end(), [](std::uint32_t pixel) { return (pixel & 0x00ffffffu) == 0x00ffffffu; }),
              "Measured values and labels retain a high-contrast white foreground.");
        LOGFONTW font{};
        GetObjectW(reinterpret_cast<HFONT>(SendMessageW(state.Tabs, WM_GETFONT, 0, 0)), sizeof(font), &font);
        Check(font.lfHeight == -14, "Controller labels use a readable 14-pixel font at 96 DPI.");
        state = gpr::ui::InspectControllerMonitorView(view);
        Check(state.PaintCount > beforePaint.PaintCount && state.LayoutRevision == beforePaint.LayoutRevision &&
                  RectEqual(Row(state, 0).Bounds, Row(beforePaint, 0).Bounds), "Painting does not rebuild or move the layout.");
        const auto second = Pixels(view, 510, 540);
        Check(rendered == second, "Unchanged measurements produce a pixel-identical stable frame.");
        ValidateRect(view, nullptr);
        gpr::ui::UpdateControllerMonitorView(view, snapshot);
        Check(!GetUpdateRect(view, nullptr, FALSE), "An identical snapshot does not repaint the monitor.");
        snapshot.Devices[1].Values[0] = 77;
        gpr::ui::UpdateControllerMonitorView(view, snapshot);
        Check(!GetUpdateRect(view, nullptr, FALSE), "A hidden controller update does not repaint the selected controller.");
        snapshot.Devices[0].Values[0] = -19;
        gpr::ui::UpdateControllerMonitorView(view, snapshot);
        RECT dirty{};
        const bool hasDirty = GetUpdateRect(view, &dirty, FALSE) != FALSE;
        Check(hasDirty && dirty.top == Row(state, 0).Bounds.top && dirty.bottom == Row(state, 0).Bounds.bottom,
              "A value-only update invalidates its fixed row rather than the whole panel.");
        ShowWindow(parent, SW_HIDE);
        SendMessageW(view, WM_KEYDOWN, VK_RIGHT, 0);
        Check(gpr::ui::InspectControllerMonitorView(view).SelectedToken == "xusb:first", "Arrow keys switch between controller tabs.");
        Check(gpr::ui::SelectControllerMonitorDevice(view, "gip:first", 1, 6), "Controller selection accepts capture token and family.");
        std::swap(snapshot.Devices[0], snapshot.Devices[2]);
        gpr::ui::UpdateControllerMonitorView(view, snapshot);
        state = gpr::ui::InspectControllerMonitorView(view);
        Check(state.SelectedToken == "gip:first" && state.SelectedIndex == 0, "Controller selection survives device reordering.");
        snapshot.Devices.erase(snapshot.Devices.begin() + 1);
        gpr::ui::UpdateControllerMonitorView(view, snapshot);
        Check(gpr::ui::InspectControllerMonitorView(view).SelectedToken == "gip:first", "Removing another controller preserves selection.");
        snapshot.Devices.erase(snapshot.Devices.begin());
        gpr::ui::UpdateControllerMonitorView(view, snapshot);
        Check(gpr::ui::InspectControllerMonitorView(view).SelectedToken == "hid:first", "Removing the selected controller picks an existing neighboring tab.");
        snapshot.Devices[0].Token.clear();
        gpr::ui::UpdateControllerMonitorView(view, snapshot);
        Check(gpr::ui::SelectControllerMonitorDevice(view, "", 1, 3), "Token-less fixtures fall back to key and family without conflating families.");
        snapshot.Devices[0].PhysicalValid = false;
        gpr::ui::UpdateControllerMonitorView(view, snapshot);
        state = gpr::ui::InspectControllerMonitorView(view);
        Check(!Row(state, 0).InputKnown && Row(state, 0).OutputKnown && !Row(state, 0).Different,
              "IN invalidation does not erase independently measured OUT or claim a difference.");
        snapshot.Devices[0].Telemetry = false;
        gpr::ui::UpdateControllerMonitorView(view, snapshot);
        state = gpr::ui::InspectControllerMonitorView(view);
        Check(!Row(state, 0).InputKnown && !Row(state, 0).OutputKnown, "Accepted script output without telemetry is never presented as observed egress.");
        auto &device = snapshot.Devices[0];
        device.Telemetry = device.PhysicalValid = device.OutputValid = true;
        device.Available.fill(true); device.InputAvailable.fill(true); device.OutputAvailable.fill(true);
        device.Values[192] = -1; device.OutputValues[192] = 2;
        gpr::ui::UpdateControllerMonitorView(view, snapshot);
        state = gpr::ui::InspectControllerMonitorView(view);
        Check(state.Rows.size() == 256 && state.ScrollExtent > 540, "All generic HID controls remain accessible through scrolling.");
        Check(Row(state, 192).InputText == L"CENTER" && Row(state, 192).OutputText == L"E", "Direction pads distinguish center and discrete direction.");
        SendMessageW(view, WM_KEYDOWN, VK_END, 0);
        state = gpr::ui::InspectControllerMonitorView(view);
        Check(state.ScrollPosition > 0 && Row(state, 255).Bounds.bottom <= 492, "End scrolls to the final generic control.");
        const int oldPosition = state.ScrollPosition;
        SendMessageW(view, WM_KEYDOWN, VK_PRIOR, 0);
        Check(gpr::ui::InspectControllerMonitorView(view).ScrollPosition < oldPosition, "Page Up provides native keyboard scrolling.");
        SendMessageW(view, WM_KEYDOWN, VK_HOME, 0);
        Check(gpr::ui::InspectControllerMonitorView(view).ScrollPosition == 0, "Home returns to the first control.");
        SCROLLINFO customThumb{sizeof(customThumb), SIF_POS};
        customThumb.nPos = 173;
        SetScrollInfo(view, SB_VERT, &customThumb, TRUE);
        SendMessageW(view, WM_VSCROLL, SB_THUMBPOSITION, 0);
        Check(gpr::ui::InspectControllerMonitorView(view).ScrollPosition == 173,
              "A themed thumb commit reads the full current scroll position set by the native scrollbar adapter.");
        SendMessageW(view, WM_KEYDOWN, VK_HOME, 0);
        const auto allControlsLayout = gpr::ui::InspectControllerMonitorView(view).LayoutRevision;
        device.Available.fill(false); device.InputAvailable.fill(false); device.OutputAvailable.fill(false);
        gpr::ui::UpdateControllerMonitorView(view, snapshot);
        Check(gpr::ui::InspectControllerMonitorView(view).Rows.size() == 256 &&
                  gpr::ui::InspectControllerMonitorView(view).LayoutRevision == allControlsLayout,
              "Partial reports cannot remove controls and make rows jump.");
        device.Available.fill(true); device.InputAvailable.fill(true); device.OutputAvailable.fill(true);
        const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        const auto start = std::chrono::steady_clock::now();
        for (int iteration = 0; iteration < 10000; ++iteration) {
            device.Values[0] = iteration % 201 - 100; device.OutputValues[0] = -device.Values[0];
            device.InputSequence = static_cast<std::uint64_t>(iteration) + 100;
            gpr::ui::UpdateControllerMonitorView(view, snapshot);
            if (iteration % 100 == 0) Pixels(view, 510, 540);
        }
        const auto duration = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        state = gpr::ui::InspectControllerMonitorView(view);
        Check(state.LayoutRevision == allControlsLayout && Row(state, 0).InputText == L"50%",
              "High-frequency values do not trigger layout work.");
        Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdiBefore + 2, "Repeated telemetry and painting keep GDI allocation bounded.");
        IAccessible *accessible = nullptr;
        Check(SUCCEEDED(AccessibleObjectFromWindow(view, static_cast<DWORD>(OBJID_CLIENT), IID_IAccessible, reinterpret_cast<void **>(&accessible))) && accessible,
              "The custom monitor exposes native MSAA accessibility.");
        long count = 0; accessible->get_accChildCount(&count);
        Check(count == 257, "Every decoded control and the native tab selector have an accessible child.");
        VARIANT child{}; child.vt = VT_I4; child.lVal = 1;
        IDispatch *accessibleTabs = nullptr;
        Check(SUCCEEDED(accessible->get_accChild(child, &accessibleTabs)) && accessibleTabs,
              "The native tab selector is reachable from the monitor accessibility tree.");
        accessibleTabs->Release(); child.lVal = 2;
        BSTR accessibleValue = nullptr;
        Check(SUCCEEDED(accessible->get_accValue(child, &accessibleValue)) && accessibleValue && wcsstr(accessibleValue, L"IN ") && wcsstr(accessibleValue, L"OUT "),
              "Screen readers receive independent IN and OUT values.");
        SysFreeString(accessibleValue);
        customThumb.nPos = 173;
        SetScrollInfo(view, SB_VERT, &customThumb, TRUE);
        SendMessageW(view, WM_VSCROLL, SB_THUMBPOSITION, 0);
        const auto hit = [&](int y) {
            POINT point{40, y}; ClientToScreen(view, &point);
            VARIANT result{};
            if (FAILED(accessible->accHitTest(point.x, point.y, &result)) || result.vt != VT_I4)
                throw std::runtime_error("The accessible monitor did not identify a client point.");
            return result.lVal;
        };
        Check(hit(110) == CHILDID_SELF,
              "Scrolled controls cannot claim the fixed header through accessibility hit testing.");
        Check(hit(510) == CHILDID_SELF,
              "Scrolled controls cannot claim the fixed footer through accessibility hit testing.");
        Check(hit(175) > 1,
              "Accessibility hit testing still finds a measured control inside the scrolled viewport.");
        accessible->Release();
        gpr::ui::SetControllerMonitorDpi(view, 144);
        state = gpr::ui::InspectControllerMonitorView(view);
        Check(state.LayoutRevision > allControlsLayout && Row(state, 0).Bounds.bottom - Row(state, 0).Bounds.top == 54,
              "DPI changes scale the control rows exactly once.");
        for (int iteration = 0; iteration < 80; ++iteration) {
            Window temporary(gpr::ui::CreateControllerMonitorView(parent, 70, instance));
            MoveWindow(temporary, 0, 0, 510, 540, FALSE);
            gpr::ui::UpdateControllerMonitorView(temporary, snapshot);
            Pixels(temporary, 510, 540);
        }
        Pump();
        Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= gdiBefore + 8, "Destroying monitor windows frees fonts, tab resources and the backing bitmap.");
        if (SUCCEEDED(com)) CoUninitialize();
        std::printf("PASS %u controller monitor checks; 10000 updates %.2f ms; no driver, capture or physical controller opened.\n", checks, duration);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL controller monitor: %s\n", error.what()); return 1;
    }
}
