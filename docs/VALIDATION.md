# Validation

## 2.3.1 automatic observation and controls

Normal interactive startup requests administrator rights before opening the
window and starts passive IN/OUT observation automatically. There is no enable
or retry button. The existing health timer retries failed connections at most
once every three seconds; a healthy or starting monitor is never restarted.
Missing-driver errors remain visible and copyable. Observation does not apply a
script or start the capture trial. Offline preview, compilation and tests remain
outside this startup path.

The toolbar places Driver setup on the left and Scripts on the right. The
Scripts popup opens toward the left when needed. Enabled interactive controls
use the hand cursor; text editing keeps its text cursor and the divider keeps
its resize cursor.

All 13 CTest suites and 99 native UI checks pass in Release and AddressSanitizer.
The widget suite includes 11 new cursor checks. UI checks cover startup privilege
decisions, lossless Windows argument quoting, bounded reconnect scheduling,
monitor failure diagnostics and toolbar order. Popup tests cover right-edge
alignment. No UAC prompt was accepted, host driver opened or live capture started
by these offline tests. Current inputs, results and scope are recorded in
[ui-validation.json](ui-validation.json). The signed driver and capture worker
are unchanged; the earlier live test results below retain their original scope.

## 2.3.0 interface

The native interface uses a searchable, grouped Scripts popup, separate controller
tabs and fixed IN/OUT rows. The workspace divider is adjustable. Parameter labels
and descriptions wrap when resized, and short control animations respect both
the Windows preference and the application's Reduce interface motion setting.

All **13 CTest suites** and **95 native UI checks** pass in optimized Release
and AddressSanitizer builds. The changed interface has dedicated checks:

| Suite | Checks | Scope |
| --- | ---: | --- |
| ControllerView | 40 | Selection through reorder/removal, independent field validity, scrolling, stable layout, unchanged/unselected snapshot repaint suppression, pixel stability and bounded GDI resources |
| ScriptLibrary | 72 | Grouping, Unicode search, original-index selection, keyboard/focus handling, dismissal, reduced motion and large-list scrolling |
| ScriptPanel | 196 | Text wrapping, resize and DPI reflow, retained focus/values/invalid edits, callbacks and resource lifetime |
| Widgets | 6,128 | Native controls, animation completion/reversal/cancellation, reduced motion, DPI and resource lifetime |

The controller-view stress fixture publishes 10,000 simulated updates with 100
paints. It also checks that repeated identical painting yields identical pixels;
old antialiased text cannot accumulate underneath new values. Values update
directly, with no animation or interpolation of input data. These are software
fixtures, not a measurement of physical input latency or display refresh rate.

The native UI test covers script use/edit transitions, unsaved changes, menu
discovery, multiple simulated controllers, diagnostic formatting on demand,
compact/minimum layouts and high DPI. Rendered documentation images explicitly
use simulated input. Current build hashes and scope are recorded in
[ui-validation.json for 2.3.0](https://github.com/TitanCSA/GamePad-Remapper/blob/v2.3.0/docs/ui-validation.json).

The capture worker, SDK and driver package are unchanged from 2.2.4. This update
does not open a host driver, install a driver or repeat the live VM/consumer
tests below. Their historical results retain their original scope. The complete
release continues to include the exact 1.0.7.0-demo installer with the
Microsoft-signed 1.0.5.0 kernel.

## 2.2.4 continuous monitor

The right panel observes independent IN and OUT telemetry, without a script or
capture context. This release bundles the **1.0.7.0-demo** installer with
Microsoft-signed controller kernel **1.0.5.0**. SDK **1.0.4.2** reuses its descriptor parser through two pure
internal adapter functions. The classic Interceptor product is unchanged.

The application passes all ten offline CTest suites and 93 native UI checks in
both optimized Release and AddressSanitizer builds. Monitor fixtures execute the actual monitor, descriptor decoder and
Windows transport against isolated event/IO boundaries. They cover first
subscription after boot, closing/reopening a session, multi-report HID validity,
native and partial Guide state, lost records, late completions after removal,
normal HID read cancellation and separation from capture/trial IO. Service and
worker stress retains 10,080 and 25,685 checks. AddressSanitizer validation covers
the changed monitor/service/worker paths. See [monitor behavior](LIVE-MONITOR.md).

Kernel tests exercise the actual production ring and extracted report-completion
and lifetime code. They verify no OUT at SEND acceptance or queue insertion,
one IN per physical receive across backpressure, bounded observer loss, concurrent
subscription/close, and normal versus terminal device lifetimes. Production and
WDK code-analysis builds pass. These isolated checks do not measure physical
hardware latency or establish compatibility with every device. Results
are recorded separately in [monitor-validation.json](monitor-validation.json);
the historical release results below retain their original scope.

In the isolated Windows 11 VM, the real monitor receives valid HID, XUSB and
GIP IN/OUT reports before loading a script. Two passive runs each contain 201
snapshots: 152 coherent IN/OUT pairs per native family and 179 per family in the
HID/GIP run. Sequence numbers and QPC timestamps advance, the trial remains
unstarted, and unstimulated controls remain unknown.

Two further 601-snapshot service runs apply a script, observe modified Windows
completions and stop capture while monitoring continues:

| Fixture | Modified snapshots after the first actual changed OUT | Fresh passthrough pairs after Stop |
| --- | ---: | ---: |
| XUSB | 191 | 192 |
| GIP, native run | 191 | 192 |
| HID | 179 | 228 |
| GIP, HID run | 179 | 228 |

All eligible pairs match the expected state, with zero observed ring loss,
script faults or monitor errors. Assertions require successful statuses, known
fields, raw bytes, a later OUT sequence and an OUT QPC at least as recent as IN. Apply checks exclude
old egress before the first actual modified completion; Stop checks require
fresh IN and OUT after the prior script phase. Repeated snapshots are not report
rates. The HID run follows the native run in the same boot and correctly retains
its existing trial start time. The final application also exercises the native
UI and Copy diagnostics at 1024 pixels. These tests use controlled virtual
devices and Driver Verifier, not physical controllers or a latency benchmark.

The VM used a development-signed copy of the exact production kernel body. An
isolated INF version 1.0.5.1 avoids Windows caching an earlier same-version lab
package; only that lab metadata/catalog differs. The distributed 1.0.5.0 package
has since been Microsoft-signed. The original results above retain their
laboratory-signature scope; returned-package checks are described below. Live
XInputHID, power/hotplug and foreign-capture transitions were not repeated in
these four probe runs; isolated tests cover their implemented boundaries.

### Returned Microsoft-signed package

The final **1.0.7.0** console installer was additionally exercised with the
returned **1.0.5.0** package. Initial installation returned restart-required
code 3010; reinstallation returned 0. The installed SYS and catalogue have valid
Microsoft signatures, and `signtool /kp /c` verifies installed catalogue
membership with exit code 0. Uninstallation returned 3010, followed by 0 after
a normal Windows restart. The UI installer's embedded payload is byte-identical;
its setup buttons were not clicked during this run.

With that installed package, a further 601-snapshot XUSB/GIP service run records
53 telemetry frames, zero loss, faults or errors, and an unstarted trial before
capture. Each family has **191** matching modified snapshots and **191** fresh
passthrough pairs after Stop. Assertions exclude nine Apply-transition snapshots
and fifty old or incoherent post-Stop snapshots per family, using the same
sequence, QPC, known-field and raw-byte guards as above.

The exact final application also passes real GUI **Apply**, **Stop** and three
**Copy diagnostics** operations with HID/GIP fixtures. It shows physical axes
beside changed OUT values of `50% / -25%`, then matching IN/OUT after Stop while
observation continues. The guest was restored to its original class-filter
values and checkpoints and shut down normally; nothing was installed on the
host.

This separate returned-package run preserves the guest's original test-signing
and Driver Verifier configuration. It verifies Microsoft signatures and kernel
policy catalogue membership, but does not test a Secure Boot or test-signing-off
boot. Browser, `joy.cpl`, WGI, standalone XInputHID consumers, power/hotplug and
foreign-capture transitions were not newly repeated. The earlier targeted
Driver Verifier results retain their separate laboratory scope.

## 2.2.3 driver integration (historical)

This version uses the exact two-file Interceptor Gamepad & Controller SDK **1.0.4.1** and bundles the unchanged **1.0.6.1** installer with Microsoft-signed kernel **1.0.4.0**. Older sections below describe historical releases and their driver versions. The classic Interceptor product is unchanged.

The application compiles and runs `.igc` code in user mode. The capture worker receives the real device report, applies the script, and returns it through `Send_Input`. Scheduled output uses the same captured source and saved physical report. XUSB runs below the Windows function driver, so native Windows translation receives the returned state; associated HID/XInputHID/GIP paths deliver the driver's modified reports. No per-process API hook or synthetic keyboard/mouse event is used.

This scope covers supported reports on associated device stacks. It does not certify unfiltered sibling interfaces, direct vendor USB protocols, every firmware, wireless mode or consumer policy. Windows APIs also present different control layouts; for example, [Microsoft documents combined triggers in DirectInput for an XInput controller](https://learn.microsoft.com/en-us/windows/win32/xinput/xinput-and-directinput). A script cannot make an absent control appear in an API that does not expose it.

The optimized native application passes all nine CTest suites and 81 native UI checks without opening the host driver. The production worker itself is exercised against an isolated transport: 25,685 checks cover 12,800 mixed reports, all five supported payload forms, forwarding order, timed output, physical-state restoration, failed sends and exact startup diagnostics. Service and worker tests also pass AddressSanitizer (10,072 and 25,685 checks). The interpreter stress suite evaluates 202,000 frames; its processing measurements describe software execution, not controller-to-application latency.

Localized Windows failures retain their original error code and valid UTF-8 text. Regression tests cover accented messages and restart-required error 3010; the exact release application also displays that failure correctly in the French Windows test VM.

Independent Windows consumers are tested with the exact release EXE, the Microsoft-signed kernel and controlled virtual XUSB/GIP controllers in an isolated Windows 11 VM. The real application opens and applies a real `.igc` script; consumer processes do not receive injected API results. The script changes stick axes and replaces A with B. Tests check baseline, Apply, Stop, reapply and forced application termination:

- Edge's native Gamepad API: 342 matching stable samples per family. Analysis excludes 34 states per family within 400 ms of physical report transitions; no background or foreground-warmup frames are excluded.
- Foreground Windows.Gaming.Input: 263 matching stable samples per family. Analysis explicitly separates 58 foreground warmup, 92 background and 56 report-transition query frames. Windows can neutralize readings after a focus change until another physical report arrives; these sparse-report tests do not establish focus-transition latency.
- Actual `joy.cpl` property windows held in the foreground: all 54 XUSB and 52 GIP stable Apply/reapply images show the changed stick and button, including differing physical states. Baseline, Stop and forced termination restore the expected physical state (46 XUSB and 47 GIP stable images). Analysis waits 1.5 s after guarded focus changes and 400 ms after physical transitions when checking passthrough; it does not claim a latency bound. Separately, 92 stable XUSB samples in each of WinMM and DirectInput match the modified state.
- Generic HID in Edge: 252 matching native states across baseline, Apply, Stop, reapply and forced termination. This fixture's descriptor exposes Y before X in the capture library; the script changes `AXIS_1`/`AXIS_2` and the browser receives the corresponding Y/X values and changed button. Expected axes come from the descriptor, not an assumption that `AXIS_1` always means X. Assertions use foreground steady event windows and are not latency measurements.

The guarded live-service test also passes timed output during quiet input, additive figure-eight motion, invalid-source preservation, fault recovery, Stop and reinjection with virtual HID/GIP fixtures (122 distinct HID and 116 GIP motion points). Its output snapshots establish accepted driver sends; this is separate from independent consumer verification.

Driver acceptance alone is not proof that a browser or game consumed a state. These are controlled virtual-device tests, not certification of every physical controller, firmware, application or vendor interface. Windows 10 physical hardware and end-to-end input latency were not measured.


## 2.2.2 installer selection and physical/output states

Eight offline CTest suites pass in optimized Release and AddressSanitizer.
Installer selection checks real VERSIONINFO identities, including the classic
and controller installers, missing/corrupt files, legacy filenames and unique
filename priority. Tests never launch either installer on the host.

The monitor no longer truncates devices to twelve controls or combines input
and output on a single line. Output is committed after successful `Send_Input`,
with independent known-state bits for each control in a multi-report HID device.
Unknown input/output is not reported as a measured zero. Generic HID output is
projected through descriptor ranges and logical resolution; native XUSB/GIP
output is reread after wire-range saturation. Relative deltas are not held or
replayed during later report or timer evaluations. Stopped sessions are marked
as historical. Driver acceptance does not independently verify consumption by
a game, browser or Windows control panel.

Conversion tests cover low-resolution HID axes, hats, buttons, relative
multi-report ordering, unknown output for an unaccepted new report, and native
saturation. Existing script, parameter, service and lifetime stress remains
enabled. The shipped SDK is still 1.0.4.0 and the bundled Microsoft-signed kernel
is still 1.0.2.0. Separate GPC kernel 1.0.3.0 compatibility corrections require
a new Microsoft signature before distribution; no unsigned candidate is
included in the application download. No host driver was opened or installed.

The exact 2.2.2 release EXE also passes 81 offline native UI checks in Windows 11.
Its live capture test with virtual HID/GIP fixtures passes remapping, timed
output, additive motion through quiet input periods, fault recovery, Stop and
reinjection; it observes 115 distinct HID and 110 distinct GIP motion points.
The separate XUSB viewer test passes six idle-start cycles and 3,721 assertions,
including comparison against an independent XInput consumer. These runs use an
isolated development-signed copy of the new 1.0.3.0 kernel body, which is not
distributed. Physical browser and `joy.cpl` behavior remains unverified until
the new Microsoft-signed driver is tested on the affected hardware.

## 2.2.1 script-defined interfaces

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
- Active configuration checks distinguish identical code in different files
  and saved values that differ from the currently applied settings.
- Visual/code roundtrips preserve interface declarations. Quoted `main`,
  `define` and visual-marker text cannot be mistaken for source structure.
- All ten shipped scripts compile and execute. Existing capture conversion,
  reference parity, script stress, visual-document, widget and snapshot
  notification tests remain enabled. Native render tests include the settings
  workspace, code/visual authoring, compact sizes and 150% DPI.

These are software and native-control tests. They do not install/open a driver,
create capture, measure physical controller latency or establish universal
hardware compatibility. The older sections below describe their named releases
and historical live checks, not fresh physical-device tests for 2.2.1.

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
