#pragma once
#include "../Core/Script.h"
#include <Windows.h>
#include <functional>
#include <vector>

namespace gpr {
// Native controls only. This panel never compiles code, accesses a driver or
// owns a script VM; changes are delivered to the window's UI thread.
class ScriptPanel final {
    HWND window_ = nullptr;
    HFONT font_ = nullptr, heading_ = nullptr;
    HBRUSH background_ = nullptr;
    UINT dpi_ = 96;
    ScriptInterface schema_;
    ParameterValues values_{};
    std::vector<HWND> children_, inputs_, readouts_;
    std::vector<int> tops_, heights_;
    int contentHeight_ = 0, scroll_ = 0;
    bool building_ = false;
    std::function<void(std::size_t, int)> changed_;
    std::function<void(const std::string &)> error_;
    static LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM);
    LRESULT Message(UINT, WPARAM, LPARAM);
    void Rebuild();
    void Layout();
    void Scroll(int position);
    void Changed(std::size_t index, int value);

  public:
    static constexpr int FirstInput = 6000;
    ScriptPanel(HINSTANCE instance, HWND parent, std::function<void(std::size_t, int)> changed,
                std::function<void(const std::string &)> error);
    ~ScriptPanel();
    HWND Handle() const noexcept { return window_; }
    void Set(const ScriptInterface &schema, const ParameterValues &values);
    void SetDpi(UINT dpi);
};
} // namespace gpr
