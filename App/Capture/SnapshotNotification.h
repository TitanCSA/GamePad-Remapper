#pragma once
#include <Windows.h>
#include <mutex>

namespace Remapper {
// One pending UI message carries the newest detached snapshot. Acknowledge
// BEFORE reading it so a final concurrent update can request another redraw.
class SnapshotNotification final {
    std::mutex gate_;
    HWND window_ = nullptr;
    UINT message_ = 0;
    ULONG_PTR generation_ = 0;
    bool pending_ = false;
    decltype(&PostMessageW) post_;
  public:
    explicit SnapshotNotification(decltype(&PostMessageW) post = PostMessageW) : post_(post) {}
    void Attach(HWND window, UINT message) {
        std::lock_guard<std::mutex> lock(gate_);
        window_ = window; message_ = message; ++generation_; pending_ = false;
    }
    void Changed() {
        std::lock_guard<std::mutex> lock(gate_);
        if (window_ && !pending_)
            pending_ = post_(window_, message_, static_cast<WPARAM>(generation_), 0) != FALSE;
    }
    bool Acknowledge(HWND window, ULONG_PTR generation) {
        std::lock_guard<std::mutex> lock(gate_);
        if (!pending_ || window != window_ || generation != generation_) return false;
        pending_ = false; return true;
    }
};
} // namespace Remapper
