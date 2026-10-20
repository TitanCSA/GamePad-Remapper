#pragma once
#include "Theme.h"
#include <CommCtrl.h>

namespace gpr::ui {
enum class ButtonKind { Secondary, Primary, Danger, Tab };

// These helpers keep the native control classes, keyboard interaction,
// accessibility and ordinary WM_COMMAND notifications intact.
void StyleButton(HWND window, ButtonKind kind = ButtonKind::Secondary);
void SetButtonSelected(HWND window, bool selected);
void StyleCheckBox(HWND window);
void StyleToggle(HWND window);
void StyleComboBox(HWND window);
void StyleEdit(HWND window);
void StyleScrollbars(HWND window);
// A DPI override is also used by offline render tests; zero follows Windows.
void SetDpi(HWND window, UINT dpi);
void SetAccessibleName(HWND window, const wchar_t *name);

// Forward WM_DRAWITEM from both the main window and the visual form here.
// Returns false for controls that are not owned by this component library.
bool DrawItem(const DRAWITEMSTRUCT &item);

// Offline design preview, using the same native controls as the application.
// The caller owns the returned window and destroys it after taking its image.
HWND CreateWidgetPreview(HINSTANCE instance);
} // namespace gpr::ui
