# Validation

## 2.2.0 script-defined interfaces

Separate Use/Edit workspaces and the `ui` compiler/runtime additions are tested
with seven offline CTest suites and the native UI self-test in optimized Release
and AddressSanitizer configurations. Both keep the unchanged 1.0.4.0 user-mode
SDK and Microsoft-signed controller kernel 1.0.2.0; no new driver signature is
required. Results are recorded in [validation.json](validation.json).

- Parameter fixtures cover declaration metadata, UTF-8 and escaped labels,
  groups, steps, enum values, 32-bit limits, invalid declarations and read-only
  runtime symbols. Ten thousand updates preserve globals, `init` state, waiting
  combo segments/deadlines and physical press history.
- Service fixtures cover ten thousand hot publications, an additional concurrent
  update/evaluation workload, multiple independent devices, new-device settings,
  invalid/stale-program rejection and preserved explicit reinjection behavior.
- Native UI checks exercise both workspaces, slider dragging, switches,
  dropdown values, numeric overflow, exact source preservation, automatic
  per-file settings, default reset, corrupted JSON, read-only storage fallback
  and migration, keyboard focus scrolling, compact layouts and 100 panel
  rebuild/DPI cycles with bounded GDI/USER resources.
- Visual/code roundtrips preserve interface declarations. Quoted `main`,
  `define` and visual-marker text cannot be mistaken for source structure.
- All ten shipped scripts compile and execute. Existing capture conversion,
  reference parity, script stress, visual-document, widget and snapshot
  notification tests remain enabled. Native render tests include the settings
  workspace, code/visual authoring, compact sizes and 150% DPI.

These are software and native-control tests. They do not install/open a driver,
create capture, measure physical controller latency or establish universal
hardware compatibility. The older sections below describe their named releases
and historical live checks, not fresh physical-device tests for 2.2.0.

## 2.1.1 controller and display update

The embedded SDK is byte-identical to Gamepad & Controller SDK 1.0.4.0.
It retains successfully opened pending HID/XInputHID/XUSB collection files
and preserves native startup failures. The bundled installer now contains
Microsoft-signed controller driver 1.0.2.0. Its two-hour limit is unchanged.

The capture worker publishes changed snapshots after forwarding, without the
former 100 ms limit or a display-only wake timer. The Win32 monitor uses one
pending message for the newest state, acknowledges before reading, and ignores
stale window generations. The former 150 ms active-monitor interval is removed.
Idle health refresh and decorative editor animation are separate from input
processing and do not throttle snapshot notifications or script deadlines.

All six CTest suites and offline UI self-tests pass in Release and under
AddressSanitizer. These include 30,010 production notification-helper checks,
100,000 concurrent notifications, 10,000 bursts, 197,833 capture/codec checks,
24 service-boundary checks, 6,046 widget checks with 100 lifetime cycles and
202,000 script VM stress frames. Final quiet updates, stale notifications,
shutdown, forwarding failures and exact startup causes are covered.
No host driver was opened or installed. Physical hardware latency/FPS and
the updated application with a physical controller were not measured here.
The following older validation remains historical evidence, not a claim that
its live VM checks were repeated for 2.1.1.

## 2.1.0 interface update

This release redesigns the native C++ interface. The application remains one
optimized **AMD64 EXE with no CLR header**, a static C++ runtime, ASLR, DEP and
Control Flow Guard. [Build the checks](BUILD.md);
[machine-readable results](validation.json).

The application palette is blue `#2563EB`, black `#111111`, white `#FFFFFF`
and gray `#9CA3AF`. Windows title bars, system dialogs and font antialiasing
remain controlled by the operating system.

| Area | Verification |
| --- | --- |
| Native components | 6,046 checks, including keyboard notifications, native values, accessibility names, popup sizing, selection, focus, offscreen scrollbar rendering, thumb dragging beyond 65,535 pixels and preservation of the other scroll axis. |
| Resource/lifetime stress | 100 control creation/destruction cycles at 96, 144 and 192 DPI; reentrant destruction during notifications; GDI and USER resource counts checked. |
| Interface/storage | Offline native checks cover Code/Visual roundtrips, transactional editing, validation ordering, recovery, UTF-8, atomic saves, focus scrolling, monitor selection and window layout. Rendered code, visual rules, macros, motion settings, popup selections, component states, compact windows and 150% DPI. |
| Capture conversion | 197,833 checks for unchanged reports, signed axes, triggers, GIP tails, HID identities, relative scalars and bounds. |
| Compiler/runtime | 25 groups / 79,278 assertions, 202,000 stress frames and zero VM allocations after warmup. |
| Reference parity | 107 frozen programs, each with 16 deterministic input frames, compared against 1.1.2 for compile/fault results, all 256 controls and timer deadlines. No managed runtime is used by these tests. |
| Visual documents | 3,667 assertions for imports, escaping, source hashes, generated source, nested triggers, activation/pattern modes, schema limits and motion stress. |
| Service boundary | 24 contracts using a fake backend that cannot access a driver. Covers ownership, replacement, faults and restoration of physical values. |
| AddressSanitizer | Complete instrumented C++ application, all five offline CTest targets and the native UI/storage test, using matched MSVC 14.44 compiler/runtime. Instrumented binaries are not shipped. |
| Windows 11 | Exact release application and widget test binaries pass offline in the dedicated Hyper-V guest. This run installs no driver and starts no capture. |

## Capture integration baseline

`App/Core`, `App/Capture` and the original two-file `Interceptor` SDK are
unchanged from **2.0.0**, commit `9cbc97f`. That version's exact release EXE
passed real capture/forwarding tests with virtual HID and GIP fixtures and the
unchanged Microsoft-signed driver in the Windows 11 laboratory VM. This is a
historical integration baseline, not a new capture test of the 2.1.0 EXE.

The baseline exercised remapping, timed button sequences and additive
figure-eight motion around physical LX=10 / LY=-20. It recorded 19 distinct GIP
points and 22 HID points, continued scheduled motion during 1,400 ms gaps in
physical reports, and restored physical output when conditions ceased. Invalid
source preserved capture; a deliberate runtime fault released capture. Live apply
could not restart a stopped session; explicit reinjection and Stop passed.

The original signed package and its two-hour demo are unchanged. A fresh
two-hour expiry wait and kernel negotiation tests were not repeated for this
interface update.

## Scope

Offline interface and sanitizer tests do not access a driver, global input,
clipboard or file dialogs. The Windows 11 guest's boot settings and certificates
matched its baseline after testing, no product driver was installed, its lab
folder was removed and Windows shut down normally. Nothing was installed or
captured on the host.

Physical XUSB/XInputHID hardware, every controller/transport, proprietary
checksums, feedback, sensors and adaptive triggers were not individually
retested. Descriptor support does not establish universal compatibility.
Software throughput is not USB, game or display latency. Bounded stress and
AddressSanitizer cannot prove absence of every defect. These checks are not
Secure Boot/HVCI certification.
