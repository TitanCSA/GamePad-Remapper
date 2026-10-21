# GamePad Remapper — Native C++ Controller Scripting & Macros

A desktop editor for controller remapping, compound triggers, continuous motion
and timed macros in **`.igc` scripts**,
powered by [Interceptor Gamepad & Controller](https://github.com/TitanCSA/Interceptor-Gamepad-Controller),
a Windows Kernel Filter Driver for HID, XInput/XUSB, XInputHID and GIP input.

**[Download](https://github.com/TitanCSA/GamePad-Remapper/releases/latest)** ·
[IGC language](docs/LANGUAGE.md) · [Build](docs/BUILD.md) · [Test results](docs/VALIDATION.md)

**2.2.3:** updated to controller SDK **1.0.4.1** and the corrected **1.0.6.1** driver installer with Microsoft-signed kernel **1.0.4.0**. Capture checks the loaded USB XUSB backend before starting. Localized Windows errors retain their message instead of appearing as invalid script encoding. [Controller state display](docs/images/controller-states.png).

Two workspaces, **Use script** and **Edit script**. Script authors declare sliders, on/off switches, dropdowns and numeric fields in a `ui` block; players adjust them without touching the code. Settings are remembered per file and update a running script without recompilation or resetting its macros. [Create a script interface](docs/SCRIPT-INTERFACE.md).

Scripts execute in the application. Captured reports are modified and returned through the kernel filter, feeding the associated device's native Windows input paths, including XInput and translated HID views for supported XUSB/GIP controllers. No per-game integration is required. Support depends on the captured transport and collection; vendor paths outside that filter and unsupported formats are not covered. See [low-level routing and validation](docs/VALIDATION.md).

| Use a script | Edit its code |
| --- | --- |
| ![Script-defined sliders, switch and dropdown; simulated session](docs/images/script-settings.png) | ![IGC code editor; simulated device preview](docs/images/code-editor.png) |

Screenshots show the real native Windows interface with explicitly simulated controller
activity. Normal use displays actual driver state and never invents devices.

<details>
<summary>Macro designer</summary>

![Timed macro steps and editable script constants](docs/images/macro-editor.png)

</details>

The interface uses four colors: **blue, black, white and gray**. Consistent
controls, clear keyboard focus and readable spacing keep the script, visual
settings and live session easy to distinguish.

## Two workspaces, one script

- **Use script:** pick a saved script, adjust its custom settings and apply it.
  Sliders, switches, selections and numeric fields come directly from its `ui`
  declaration. Parameter edits apply immediately to the active version, with
  no Live apply switch required. **Reset settings** restores the author's defaults.
- **Edit script:** author the behavior and its interface in **Code**, or use
  the existing **Visual** designer. Switching workspace preserves the same file.

- **Code:** a C-like language with `init`, `main`, variables, arrays, functions,
  conditionals, loops, arithmetic, input events and timed `combo` sequences.
- **Visual:** remap and axis cards; nested **ALL / ANY / NOT** triggers, holds,
  double taps and axis thresholds; timed sequences or figure-eight, circle and
  sine motion. Add the ready-to-edit infinity recipe with one click.
  Advanced code is preserved when switching views.
- **Library:** create, open and save `.igc` files; atomic saves keep the previous
  version as `.bak`. Unsaved edits get recovery drafts. Saved scripts are
  available from the sidebar.
- **Apply script:** compile and apply behavior to captured controllers.
  **Stop** releases capture. Applying a script updates user-mode behavior; it
  does not load script code into the kernel or flash controller firmware.
- **Fewer clicks:** automatic background validation, readable behavior summaries,
  connected controls first in selectors, and optional **Live apply valid edits**.
  Invalid edits keep the last working configuration.
- **Monitor:** every available control in separate **INPUT** (physical) and
  **OUTPUT** (accepted by the driver) columns. `*` identifies a changed value;
  `--` means no accepted output is known yet. HID output reflects descriptor
  limits and quantization, including controllers with multiple input reports.
  Stopped sessions are labeled as historical. Forwarding counts, measured
  processing time and copyable diagnostics remain available. Display updates
  are event-driven and run separately from the input worker. An accepted report
  is not an independent measurement of what a game or browser has consumed.

## Start

Windows 10/11 **x64**. Native C++ application, with no .NET dependency.

1. Download and extract `GamePad-Remapper-2.2.3-win-x64.zip`. The application is a single native C++ EXE.
   The ZIP includes example scripts and the driver installer; no .NET runtime or application DLLs are needed.
2. If the current **Interceptor Gamepad & Controller 1.0.4.0** driver is already installed and its associations are complete, keep it and continue to step 3. Otherwise run **`InterceptorGamepadControllerSetup.exe`** from this ZIP.
   **Driver setup** checks the installer's edition before launching it; a classic
   Interceptor installer with the old shared filename will not be launched.
   If requested, use Windows **Restart**, then reopen the installer to complete
   device associations. See the [driver installation instructions](https://github.com/TitanCSA/Interceptor-Gamepad-Controller#quick-start).
3. Open **`GamePadRemapper.exe`**. Select an example or create a script. Editing
   and compiling work without an installed driver or administrator rights.
4. In **Use script**, adjust the settings and choose **Apply script**
   (**Ctrl+Enter**). Use **Edit script** to change the code or visual rules.
   Approve Windows elevation when requested: the driver restricts capture to
   administrators. Starting a capture does not require moving a controller.
5. Use **Stop** before another capture application. Closing this app also
   releases capture and restores physical input passthrough.

Your library is stored in `Documents/GamePad Remapper/Scripts`. Settings are
automatically saved in a neighboring `.igc.settings.json` file; keep it with the
script to carry your values to another folder. Read-only folders use a local
Windows user-profile fallback. Changing parameters leaves the
`.igc` code untouched. You can open
`.igc` files from any folder. Enable **Live apply valid edits** once per session
to apply subsequent valid changes after a short editing pause. It only updates
an already-running capture; opening a file or enabling the option never starts
capture. **Ctrl+S** saves the script; recovery drafts do not replace that file.

## Declare your own settings

```c
ui {
    title("My controller");
    toggle enabled("Enable processing", true);
    slider zone("Deadzone", 0, 99, 8, "%");
    select mode("Camera mode", 0) {
        0: "Normal";
        1: "Invert vertical";
    };
}

main {
    if (enabled) {
        deadzone(LX, zone);
        deadzone(LY, zone);
        if (mode == 1) invert(RY);
    }
}
```

The **Use script** workspace creates the controls automatically. Their names
(`enabled`, `zone`, `mode`) are read-only integer values in the script. The
deadzone builtin accepts `0..99`; other sliders can use `0..100` or another range.
See the [complete customizable example](Scripts/10%20-%20Custom%20Script%20Interface.igc)
and [interface reference](docs/SCRIPT-INTERFACE.md) for numeric fields, units,
groups, help text, saved values and hot-update behavior.

## Complex behavior without a complex setup

In **Visual editor**, choose **∞ A + B → additive figure eight**, then
**Apply script**. The recipe holds A + B (`SOUTH` + `EAST`) and checks
that camera `RX > 50%`; it adds an infinity motion to `LX` / `LY` while that
condition remains true. Adjust the two axes, width, height and cycle directly
on the card. Generic HID controllers need their reported `BUTTON_n` / `AXIS_n`
bindings instead of assumed button labels.

With physical `LX = 10%` and `LY = -20%`, those values are the moving center.
Each evaluation starts from current physical input and adds the motion once:
there is no accumulated drift. Moving the stick moves that center; releasing
either button or dropping `RX` to `50%` stops the recipe on the next received
input. Output stays within the axis limits. **Replace** can instead follow
physical input, hold the position at activation, or use `(0, 0)` as its center.

Combine nested conditions, custom expressions, stateful functions and combos
for other behaviors. Sequences support activation, repeat-while-true,
deactivation and toggle modes; steps can **Add** or **Replace**. The code view
remains available for logic beyond the visual cards.

<details>
<summary>The same additive figure eight as an IGC script</summary>

```c
define PERIOD = 1200;
int active, origin;

main {
    if (get_ival(SOUTH) && get_ival(EAST) && get_ival(RX) > 50 &&
        is_available(LX) && is_available(LY)) {
        if (!active) { origin = get_phase(PERIOD); active = 1; }
        int phase = (get_phase(PERIOD) - origin + PERIOD) % PERIOD;
        int angle = phase * 360 / PERIOD;
        add_val(LX, sin(angle, 30));
        add_val(LY, sin(angle * 2, 20));
        tick(4);
    } else active = 0;
}
```

`sin` uses degrees and returns an integer scaled by its second argument.
`add_val` composes with existing output; with no earlier transform, that is the
physical stick. `get_phase` uses monotonic time, so phase does not drift when
Windows delivers a late tick. `tick(4)` requests updates only on this active
path. See [the complete example](Scripts/07%20-%20Additive%20Figure%20Eight.igc)
and [generic HID version](Scripts/08%20-%20Generic%20HID%20Orbit.igc).

</details>

## A small IGC script

```c
define TAP_MS = 80;

main {
    // Native XInput / GIP controls. HID uses descriptor-based names below.
    if (is_available(LY)) invert(LY);
    if (event_press(SOUTH)) combo_run(DoubleTap);
}

combo DoubleTap {
    set_val(EAST, 100); wait(TAP_MS);
    set_val(EAST, 0);   wait(TAP_MS);
    set_val(EAST, 100); wait(TAP_MS);
    set_val(EAST, 0);   wait(TAP_MS);
}
```

For a generic HID controller, use the displayed **`BUTTON_1`, `AXIS_1`,
`TRIGGER_1`, `HAT_1`** and other descriptor-based controls. HID button usages do
not universally identify A/Cross or B/Circle, so the app does not guess a brand
layout from a product name. Only controls actually present on that controller
can be changed. Script state is independent for each captured device. Wide
binary fields and proprietary features outside the descriptor/protocol remain
unchanged; arbitrary vendor checksums and hardware features are not emulated.

IGC is an original language **inspired by GPC**, not the Cronus compiler or a
drop-in implementation of every GPC firmware API. The
[language reference](docs/LANGUAGE.md) describes supported syntax, timing,
identifiers, limits and differences. Hardware-specific Cronus display, console
authentication and device APIs are not simulated.

## Driver compatibility and demo

Support comes from the connected controller's **driver stack, descriptor and
protocol**, including supported DS4/DualShock 4, DualSense, wired Xbox 360 and
Xbox One/Series profiles. A matching brand name alone is not a compatibility
guarantee. Unsupported bytes pass through unchanged. See the
[driver compatibility guide](https://github.com/TitanCSA/Interceptor-Gamepad-Controller/blob/main/docs/COMPATIBILITY.md).

The signed driver is a **two-hour demo per Windows boot**, starting at the first
successful capture and shared by applications using this driver edition.
Closing or reinstalling does not reset it. At expiry, captures are revoked and
physical input passes through normally. Use Windows **Restart** for a new
allowance. For the unrestricted driver, DM **`titancsa`** on Discord.

The remapper is open source under [MIT](LICENSE). Driver binary terms are
[separate](DRIVER-BINARY-LICENSE.txt). The release includes the unmodified
Microsoft-signed driver installer; the driver signature does not sign the
remapper EXE. Kernel driver source and signing credentials are not published.

## Performance and reliability

Scripts compile once to bounded bytecode. A dedicated worker processes input
outside the UI thread; timed work uses one-shot timers, and an idle script does
not require a busy polling loop. The runtime limits instructions, stacks and
arrays. Script faults release capture rather than keep a broken transform active.

No software can promise zero latency or universal hardware support. Measured
software costs, stress tests and signed-driver VM tests are recorded with their
scope in [validation](docs/VALIDATION.md).

Built by **TitanCSA**, using the independent
[Interceptor Gamepad & Controller](https://github.com/TitanCSA/Interceptor-Gamepad-Controller)
project and the interception workflow popularized by
[oblitum/Interception](https://github.com/oblitum/Interception). No affiliation with
Cronus, oblitum, Microsoft, Sony or other controller manufacturers.

## Source layout

Only four content folders:

```text
App/          C++ application, UI, script engine, capture, tests and build tools
Scripts/      Ready-to-edit .igc examples
docs/         Documentation and screenshots
Interceptor/  The original two-file C++ user-mode SDK
```

Everything in the application is **C++17**, including the editor, visual model,
compiler and runtime. Capture and the SDK are linked directly into the EXE.
Build with `./Build.ps1`; generated files stay inside ignored `App/.build/`.
See the [build instructions](docs/BUILD.md).
