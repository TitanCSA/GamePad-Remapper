# Validation — 1.0.0

Validation was performed before the first release on Windows x64, with strict
optimized MSVC / C# builds. [Build and repeat the software checks](BUILD.md).
The compact [machine-readable summary](validation.json) records the measurements.

| Area | Result |
| --- | --- |
| Native conversion | **196,829 checks passed**: every signed 16-bit XUSB axis value, exact unchanged bytes, unrelated-field preservation, triggers, Guide, GIP tail retention, repeated HID usages, stable descriptor identities, relative values and bounds. |
| IGC compiler and runtime | **22 groups / 70,960 assertions passed**: syntax, arithmetic, arrays/functions/loops, events, timing, combos, deadlines, bounded faults, isolated instances and visual round trips. |
| Stress and invalid source | 101,000 measured VM frames after 10,000 warmup frames; 32,000 frames across parallel independent instances; 3,000 malformed-source cases. No garbage collections in the measured VM loop. |
| UI and storage | **19 checks passed**: noncapturing runtime warmup, background compilation, all dynamic control selectors, view switching, custom-code preservation, macro steps, errors, search, atomic replacement, backup and unsaved state. Actual WPF screenshots rendered and inspected. |
| Signed-driver integration | Actual released Microsoft-signed Interceptor Gamepad Controller installed in a dedicated Windows 11 VM; HID and GIP virtual fixtures remapped and forwarded through the real managed/native/driver pipeline. |
| Fault recovery | Bad source preserves the running configuration. Compiled replacement works during capture. Deliberate division by zero reports line/column and releases capture. Reinjection and Stop succeed. |
| Idle macro output | The fixture producer includes 700 ms every second without submitting a physical report. Scheduled macro output continues during those intervals. |

The final integration run processed **585 physical reports**, forwarded **643
reports** including **58 scheduled outputs**, and counted **624 changed outputs**
in its active phase. No capture faults occurred in that phase. A later deliberate
runtime fault was reported and handled as expected. The test then reinjected an
unchanged script and stopped capture successfully.

The final run took 32.26 seconds overall, consumed approximately 0.219 process
CPU seconds, used approximately 29.6 MiB of private process memory, and recorded
zero generation-0 collections. These figures describe the test runner at its
fixture cadence, not the full WPF UI or a guaranteed maximum resource cost.

## Timing scope

The pure VM benchmark measured about **1.15 µs per frame** for its median batch
and **1.31 µs per frame** for its 95th-percentile batch. These are distributions
of **101 averages of 1,000 frames**, after warmup, not per-packet tail latency.
The VM benchmark excludes interop, driver calls and all hardware.

The final live VM run reported approximately **246 µs mean transform-and-forward
worker time** per physical event, including initial source setup, callback,
encoding and synchronous send. That span excludes input wait, receive/batch
decoding, USB polling, downstream game processing and display latency. VM
scheduling and machine load affect results. It is not a round-trip or
controller-to-screen latency measurement.

The adapter uses a high-resolution one-shot Windows timer when available, with
a standard waitable-timer fallback. It does not busy-spin or change the global
timer resolution. Ordinary remaps are event-driven; scheduled logic wakes only
when required by its deadlines or explicit periodic script behavior. Runtime
JIT warmup happens on a fixed throwaway script before capture begins.

## Practical limits

The integration fixtures exercised **HID and GIP**. XUSB conversions were
exhaustively checked in software; physical XUSB/XInputHID, every controller model,
wireless transport, proprietary checksum, vibration, adaptive trigger and sensor
feature were not individually tested. Generic descriptor support is not a
universal hardware compatibility guarantee.

The VM already had laboratory boot settings and fixture certificates. They were
preserved. This is not a Secure Boot / HVCI certification. Kernel signature and
catalog verification belongs to the referenced signed driver release. A fresh
two-hour expiry wait was not part of the remapper test; this app uses the
unchanged driver and its existing per-boot allowance.

UAC elevation, save/open dialogs and clipboard access use Windows facilities.
Their normal paths are implemented; the unattended tests do not claim a manual
test of every desktop policy, privilege prompt or clipboard owner.
