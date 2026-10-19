# Validation â€” 2.0.0 native C++

The application, editor, visual document model, compiler, VM and capture adapter
are C++17. The release is a single native **AMD64 EXE with no CLR header**, built
with warnings as errors, a static C++ runtime, ASLR, DEP and Control Flow Guard.
[Build the checks](BUILD.md); [machine-readable results](validation.json).

| Area | Result |
| --- | --- |
| Capture conversion | **197,833 checks**: exact unchanged reports, signed axes, triggers, GIP tails, HID identities, relative scalars and bounds. |
| Compiler/runtime | **25 groups / 79278 assertions** plus **202,000 measured frames** with **zero VM allocations** after warmup. Full operators, arrays, functions, all 48 builtins, combo restart/stop, clipping, timing, availability and limits. |
| Reference parity | **107 frozen programs**, each with 16 deterministic input frames, compared against version 1.1.2 for compile/fault results, all 256 output controls and next timer deadlines. Includes original regression programs and randomized integer expressions. No C# compiler/runtime is used by the tests. |
| Visual documents | **3,667 assertions**: exact legacy v1/v2 imports, UTF-8 and JSON escaping, source hashes, generated-source stability, nested triggers, every activation/pattern mode, schema limits and motion stress. |
| Service boundary | **24 contracts** with a fake backend that cannot access a driver: independent sources, same-program reinjection, live replacement, missing/invalid programs, original-input restoration on fault, detached snapshots, startup errors and ownership. |
| UI/storage | **42 offline checks** with no service construction or driver access. Code/Visual roundtrip, transactional edits, NOT conversions preserving full conditions, validation ordering, invalid-field discard, recovery, UTF-8/UTF-16, atomic saves and background draft retirement. Four real native interface renders were inspected. |
| Memory instrumentation | Full application and all four CTest targets pass with matched MSVC 14.44 AddressSanitizer, plus the offline native UI test. Instrumented binaries and their runtime are not shipped. |
| Signed integration | The exact release EXE passed UI and real capture/forwarding tests against virtual HID and GIP fixtures in a dedicated Windows 11 Hyper-V VM. The original Microsoft-signed driver was unchanged. |

The integration test exercised remapping, timed button sequences and continuous
additive figure-eight motion around physical LX=10 / LY=-20. It recorded
**19 distinct GIP points and 22 HID points**, kept motion running during
1,400 ms gaps without physical reports, and restored physical output when the
button/axis conditions ceased. Invalid source preserved capture; a deliberate
division-by-zero fault reported its cause and released capture. Live apply
could not restart a stopped/faulted session; explicit reinjection and Stop passed.

The initial remapping phase received **26 physical reports**, forwarded
**30 reports**, changed **30 outputs**, and emitted **4
scheduled outputs**, with zero unintended capture faults. The later pattern phase
used the same real native worker and signed-driver pipeline.

## Measurements and scope

The pure VM stress loop averaged **0.803 us per frame** over 202,000 frames
after 10,000 warmup frames, with no per-frame allocations. This is software
throughput, not a per-packet tail latency or a hardware measurement.

The headless integration runner took **31.024 s** and used **0.438 process CPU
seconds**. Its private memory at completion was **2.668 MiB**. The initial
physical-event worker average was **184.850 us**, including initial source setup,
transformation, encoding and synchronous send. It excludes receive/decode wait,
USB polling, games and display. These VM measurements describe this workload;
they are not full-UI resource measurements or guaranteed maximum costs.

Timed work uses one-shot Windows timers, with no busy-spin or global timer
resolution change. Conditional ticks disappear when inactive. The UI compiler
and draft writer run outside the input worker. Ordinary editing and offline
tests never start capture; Inject remains explicit and administrator-only.

## Practical limits

Physical XUSB/XInputHID hardware, every controller/transport, proprietary
checksums, feedback, sensors and adaptive triggers were not individually tested.
Descriptor and protocol support does not establish universal compatibility.
Bounded tests and AddressSanitizer cannot prove the absence of every bug.

The existing signed driver can require a reconnect after installation to obtain
GIP negotiation metadata. The laboratory fixture was reconnected before these
tests. This application-only release does not alter kernel negotiation, the
two-hour per-boot demo or the signed package. A fresh two-hour expiry wait was
not repeated; this is not Secure Boot/HVCI certification.

After normal installation and uninstall/restart cycles, the VM's boot settings,
certificates and class filters matched the baseline. Product services, packages
and tasks were removed, the lab folder was deleted, and Windows shut down
normally. Nothing was installed or captured on the host. Native Windows UAC,
clipboard and file dialogs remain subject to the user's desktop policies.