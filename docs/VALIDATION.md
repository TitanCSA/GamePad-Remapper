# Validation - 1.1.0

Optimized Windows x64 MSVC/C# builds use warnings as errors. The compact
[machine-readable results](validation.json) include exact tested binary hashes.
[Build and repeat the software checks](BUILD.md).

| Area | Result |
| --- | --- |
| Native conversion | **196,829 checks**: signed axes, unchanged bytes, unrelated fields, triggers, GIP tails, HID identities, repeated usages, relative values and bounds. |
| Compiler/runtime | **32 groups / 78761 assertions**: arithmetic, isolation, timing, clipping, additive combo holds, nested visual conditions, all activation modes and long-running clocks. |
| Stress | Two measured 101,000-frame loops after warmup, 32,000 parallel-instance frames, 3,000 malformed scripts, and 3,000 randomized ordered-clipping comparisons. Zero GC in both measured loops. |
| UI/storage | **32 checks**: background validation, debounce, stale-result rejection, noncapturing live apply, visual transitions, metadata preservation, numeric error retention, atomic saves and drafts. Four actual WPF renders inspected. |
| Signed integration | Exact unchanged Microsoft-signed driver in a dedicated Windows 11 VM, with real managed/native/driver capture and forwarding against HID/GIP fixtures. |
| Complex motion | A+B+RX>50 drives additive GIP figure-eight motion around physical LX=10 / LY=-20. Generic HID uses BUTTON_1+BUTTON_2 and AXIS_1/AXIS_2. Both produced multiple distinct points without center drift. |
| Quiet input | Fixture phases include 1,400 ms without physical submissions. Active patterns continue; button release or a false axis threshold restores physical output and stops timed changes. |
| Recovery | Invalid source keeps the active config. Live replacement works. Runtime division by zero reports its cause and releases capture. Live apply cannot restart a faulted/stopped capture. Explicit reinjection and Stop pass. |

The normal integration phase processed **238 physical reports**, forwarded
**3361 reports** including **3121 scheduled outputs**, and recorded **3213 changed
outputs**, with **zero capture faults**. A later deliberate runtime fault was
handled as expected. The runner took 31.076 s, used 0.375 process CPU seconds,
approximately 35.168 MiB private process memory and 1 generation-0 collections.
This describes the test runner at its fixture cadence, not the full WPF UI or
a guaranteed maximum resource cost.

## Timing scope

The pure VM median / p95 batch averages were **1.144 / 1.182 us per frame**;
the additive figure-eight benchmark measured **1.512 / 2.187 us**. Each distribution
contains 101 averages of 1,000 frames, after 10,000 warmup frames. These are
software throughput measurements, not per-packet tail or hardware latency.

The live worker mean was **212.472 us per physical event**, including initial source
setup, callback, encoding and synchronous send. It excludes input wait,
receive/batch decoding, USB polling, games and display. VM scheduling and load
affect all timing; no zero-latency claim is made.

One native worker uses high-resolution one-shot Windows timers, with a standard
timer fallback. There is no busy-spin or global timer-resolution change.
Conditional ticks disappear when their path is inactive. The UI's final quiet
state gets one deferred publication, then that wake is removed. Runtime warmup
uses a fixed throwaway program before capture.

## Practical limits

Integration exercised virtual **HID and GIP** fixtures. XUSB conversion was
exhaustively checked in software. Physical XUSB/XInputHID, every model/wireless
transport, proprietary checksums, vibration, sensors and adaptive triggers were
not individually tested. Descriptor support is not a universal hardware guarantee.

The first GIP attempt exposed a setup limitation of the existing signed driver:
the source had not received protocol metadata after installation/restart and
remained unready. Reconnecting the laboratory USB fixture completed negotiation;
the final test used that ready source and a Windows XInput consumer. This release
does not change the kernel driver's protocol negotiation. If a GIP source is
missing, inspect the driver's diagnostics and reconnect it after installation.

The dedicated VM's preexisting boot settings, fixture certificates and class
filters were restored; product services/packages/tasks were removed and Windows
was shut down normally. Nothing was installed or captured on the host. This is
not Secure Boot/HVCI certification. The signed driver and its existing two-hour
per-boot allowance are unchanged; a fresh two-hour expiry wait was not repeated.

UAC, open/save dialogs and clipboard access use Windows facilities. Unattended
checks do not claim manual coverage of every desktop policy or privilege prompt.
