#pragma once
#include <Windows.h>
#include <cstdint>

#if defined(GR_STATIC)
#define GR_API extern "C"
#elif defined(GR_BUILD_DLL)
#define GR_API extern "C" __declspec(dllexport)
#else
#define GR_API extern "C" __declspec(dllimport)
#endif

// ABI 2, x64, UTF-16 strings; fixed bounds are part of the ABI.
// Callback: flags 0=physical, 1=macro deadline, 2=config reset, 3=forget source.
// Return 0 when idle, a positive next deadline in milliseconds, -1 on fault.
// Available slots alone can be edited. Unknown bytes always retain their source.
using GR_Callback = int(__cdecl *)(std::uint32_t key, std::uint32_t family, const int *physical,
                                   const std::uint8_t *available, int *output, std::uint32_t flags,
                                   void *user);

struct GR_Snapshot {
    std::uint32_t size, version, running, device_count;
    std::uint64_t received, forwarded, changed, timer_outputs;
    double processing_microseconds;
    std::uint64_t faults;
    wchar_t status[192], error[512];
};
struct GR_Device {
    std::uint32_t size, key, family, flags;
    wchar_t name[128], token[192], diagnostic[384];
    int values[256];
    std::uint8_t available[256];
    int output_values[256];
};
GR_API std::uint32_t __cdecl GR_Probe(wchar_t *message, std::uint32_t characters);
GR_API std::uint32_t __cdecl GR_Start(GR_Callback callback, void *user);
GR_API void __cdecl GR_Stop();
GR_API void __cdecl GR_Wake();
GR_API std::uint32_t __cdecl GR_GetSnapshot(GR_Snapshot *snapshot);
GR_API std::uint32_t __cdecl GR_GetDevice(std::uint32_t index, GR_Device *device);
