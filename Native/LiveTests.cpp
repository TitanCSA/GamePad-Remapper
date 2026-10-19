#include "Bridge.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>

// Explicit lab opt-in only. The managed --live-test path is the release gate;
// this smaller runner can isolate the native callback boundary in a test VM.
namespace {
int __cdecl Transform(std::uint32_t,std::uint32_t,const int* physical,const std::uint8_t* available,int* output,std::uint32_t,void*) {
    for(unsigned i=0;i<256;++i)output[i]=physical[i];
    if(available[0])output[0]=-physical[0];else if(available[96])output[96]=-physical[96];
    return 0;
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc==2&&!wcscmp(argv[1],L"--self-test")){std::puts("Native live runner guarded; no driver access");return 0;}
    wchar_t guard[8]{};
    if(argc<2||wcscmp(argv[1],L"--live-test")||GetEnvironmentVariableW(L"GAMEPAD_REMAPPER_LAB",guard,8)!=1||guard[0]!=L'1') {
        std::fputs("Live test requires --live-test and GAMEPAD_REMAPPER_LAB=1 in a dedicated VM.\n",stderr);return 2;
    }
    const auto error=GR_Start(Transform,nullptr);if(error){std::fprintf(stderr,"Start: %u\n",error);return 1;}
    std::this_thread::sleep_for(std::chrono::seconds(5));GR_Stop();
    GR_Snapshot snapshot{};snapshot.size=sizeof(snapshot);GR_GetSnapshot(&snapshot);
    std::printf("Received=%llu Forwarded=%llu Changed=%llu Faults=%llu Worker_us=%.3f\n",snapshot.received,snapshot.forwarded,snapshot.changed,snapshot.faults,snapshot.processing_microseconds);
    return snapshot.received&&snapshot.changed&&!snapshot.faults?0:1;
}
