#include "../Capture/SnapshotNotification.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
std::atomic<unsigned> posts{0};
std::atomic<ULONG_PTR> generation{0};
bool reject = false;
unsigned checks = 0;
BOOL WINAPI Post(HWND, UINT, WPARAM w, LPARAM) {
    if (reject) return FALSE;
    generation = static_cast<ULONG_PTR>(w); ++posts; return TRUE;
}
void Check(bool condition, const char* why) {
    ++checks; if (!condition) throw std::runtime_error(why);
}
}
int main() {
    try {
        Remapper::SnapshotNotification notification(Post);
        const auto first = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(1));
        const auto second = reinterpret_cast<HWND>(static_cast<ULONG_PTR>(2));
        notification.Attach(first, WM_APP + 27);
        std::vector<std::thread> threads;
        for (unsigned n = 0; n < 4; ++n) threads.emplace_back([&] {
            for (unsigned i = 0; i < 25000; ++i) notification.Changed();
        });
        for (auto& thread : threads) thread.join();
        Check(posts == 1, "100000 concurrent input notifications occupy one UI message");
        Check(!notification.Acknowledge(second, generation), "another window cannot acknowledge this snapshot");
        Check(notification.Acknowledge(first, generation), "release the slot before reading the newest snapshot");
        notification.Changed();
        Check(posts == 2, "an update during a snapshot read can post its quiet final state");
        Check(notification.Acknowledge(first, generation), "the final state can be acknowledged without another input");
        for (unsigned n = 0; n < 10000; ++n) {
            const auto before = posts.load();
            for (unsigned i = 0; i < 64; ++i) notification.Changed();
            Check(posts == before + 1, "a burst is coalesced without a fixed display interval");
            Check(notification.Acknowledge(first, generation), "one delivery releases the pending slot");
            Check(!notification.Acknowledge(first, generation), "duplicate delivery cannot consume another update");
        }
        notification.Changed(); const auto old = generation.load();
        notification.Attach(first, WM_APP + 27); notification.Changed();
        Check(generation != old && !notification.Acknowledge(first, old), "stale messages from a previous window generation cannot hide its final state");
        Check(notification.Acknowledge(first, generation), "new generation retains its pending update");
        notification.Attach(nullptr, 0); const auto before = posts.load();
        notification.Changed(); Check(posts == before, "closed windows receive no notification");
        notification.Attach(second, WM_APP + 27); reject = true; notification.Changed();
        Check(posts == before, "a failed PostMessage does not occupy the pending slot");
        reject = false; notification.Changed();
        Check(posts == before + 1 && notification.Acknowledge(second, generation), "a later post succeeds after transient dispatcher rejection");
        std::printf("PASS %u snapshot notification checks; 100000 concurrent notifications and 10000 bursts; no window, driver or capture opened.\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL notification: %s\n", error.what()); return 1;
    }
}
