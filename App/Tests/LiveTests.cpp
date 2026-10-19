#include "../Capture/Service.h"
#include <cstdio>
#include <stdexcept>

int wmain(int argc, wchar_t **argv) {
    if (argc == 2 && !wcscmp(argv[1], L"--help")) {
        std::puts("VM only: RemapperLiveTests.exe --live-test report.json [seconds]. Requires "
                  "GAMEPAD_REMAPPER_LAB=1. Normal builds never run this test.");
        return 0;
    }
    try {
        if (argc < 3 || argc > 4 || wcscmp(argv[1], L"--live-test"))
            throw std::invalid_argument("Expected --live-test report.json [seconds].");
        int seconds = 30;
        if (argc == 4) {
            std::size_t used = 0;
            seconds = std::stoi(argv[3], &used);
            if (used != wcslen(argv[3]))
                throw std::invalid_argument("Duration must be an integer.");
        }
        return gpr::RunLiveTest(argv[2], seconds);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
