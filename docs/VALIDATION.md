# Validation - 1.1.1

Optimized Windows x64 MSVC/C# builds use warnings as errors. The compact
[machine-readable results](validation.json) include exact tested binary hashes.
[Build and repeat the software checks](BUILD.md).

| Area | Result |
| --- | --- |
| Native conversion | **197,833 checks**: signed axes, unchanged bytes, unrelated fields, triggers, GIP tails, HID identities, repeated usages, relative values and bounds. |
| Native memory checks | The same pure conversion tests passed under MSVC AddressSanitizer. Decoded scalar relative movements are neutralized in synthetic templates, so an unrelated timed action does not replay a prior movement. Wide opaque binary fields remain outside this scalar handling. |
| Startup failures | A private fault-injected build of the actual native worker passed 100 startup failures in the VM: error 21, ABI 2, final cause published, no input callback/capture. This harness differs from the release binary only by replacing initialization with the injected failure. |
| Compiler/runtime | **38 groups / 82775 assertions**: arithmetic, isolation, timing, clipping, combo cancellation/restart, rejected timestamps, missing control endpoints, strict syntax, typed visual metadata and long-running clocks. |
| Stress | Two measured 101,000-frame loops after warmup, 32,000 parallel-instance frames, 3,000 malformed scripts, and 3,000 randomized ordered-clipping comparisons. Zero GC in both measured loops. |
| UI/storage | **48 checks**: background validation, stale-result rejection, invalid-field gating and discard, noncapturing live apply, stable controller cards, view transitions, recovery retirement, atomic saves and strict text handling. Four actual WPF renders inspected. |
| Signed integration | Exact unchanged Microsoft-signed driver in a dedicated Windows 11 VM, with real managed/native/driver capture and forwarding against HID/GIP fixtures. |
| Complex motion | A+B+RX>50 drives additive GIP figure-eight motion around physical LX=10 / LY=-20. Generic HID uses BUTTON_1+BUTTON_2 and AXIS_1/AXIS_2. Both produced multiple distinct points without center drift. |
| Quiet input | Fixture phases include 1,400 ms without physical submissions. Active patterns continue; button release or a false axis threshold restores physical output and stops timed changes. |
| Recovery | Invalid source keeps the active config. Live replacement works. Runtime division by zero reports its cause and releases capture. Live apply cannot restart a faulted/stopped capture. Explicit reinjection and Stop pass. |

The normal integration phase processed **242 physical reports**, forwarded
**3393 reports** including **3149 scheduled outputs**, and recorded **3253 changed
outputs**, with **zero capture faults**. A later deliberate runtime fault was
handled as expected. The runner took 31.235 s, used 0.625 process CPU seconds,
approximately 33.832 MiB private process memory and 1 generation-0 collections.
This describes the test runner at its fixture cadence, not the full WPF UI or
a guaranteed maximum resource cost.

New regressions include thousands of self-restarts without stack growth,
cancellation cleanup, preservation of input history after rejected timestamps,
malformed visual metadata and invalid unsaved edits. Passing these bounded tests
does not prove the absence of every possible bug.

## Timing scope

The pure VM median / p95 batch averages were **1.156 / 1.371 us per frame**;
the additive figure-eight benchmark measured **1.542 / 2.239 us**. Each distribution
contains 101 averages of 1,000 frames, after 10,000 warmup frames. These are
software throughput measurements, not per-packet tail or hardware latency.

The live worker mean was **205.919 us per physical event**, including initial source
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
