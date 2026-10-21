#pragma once
#include "../Capture/Service.h"
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace gpr::ui {
// A passive presentation component. Updating it never opens the driver, starts
// capture, changes a script or reschedules an input report.
HWND CreateControllerMonitorView(HWND parent, int id, HINSTANCE instance);
void UpdateControllerMonitorView(HWND window, const ServiceSnapshot &snapshot);
void SetControllerMonitorDpi(HWND window, UINT dpi);
bool SelectControllerMonitorDevice(HWND window, const std::string &token,
                                   std::uint32_t key, std::uint32_t family);

// Detached presentation state for accessibility, offline render tests and UI
// integration. Bounds are client coordinates, including the scroll position.
struct ControllerMonitorRow {
    int Slot = -1;
    std::wstring Label, InputText, OutputText;
    RECT Bounds{};
    bool InputKnown = false, OutputKnown = false, Different = false;
};
struct ControllerMonitorState {
    std::string SelectedToken;
    std::uint32_t SelectedKey = 0, SelectedFamily = 0;
    int SelectedIndex = -1, DeviceCount = 0, ScrollPosition = 0, ScrollExtent = 0;
    std::uint64_t LayoutRevision = 0, PaintCount = 0;
    HWND Tabs = nullptr;
    std::vector<ControllerMonitorRow> Rows;
};
ControllerMonitorState InspectControllerMonitorView(HWND window);
} // namespace gpr::ui
