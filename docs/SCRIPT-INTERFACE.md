# Script-defined controls

**Use script** is for selecting, configuring and running scripts. **Edit script**
contains the Code and Visual authoring tools. A script's optional `ui` block
defines its usage interface; the application creates accessible native Windows
controls from these declarations. No external UI framework, DLL or separate
interface file is required.

```c
ui {
    title("My controller");
    description("Adjust the behavior without changing its code.");

    group("Sticks");
    toggle enabled("Enable processing", true);
    slider zone("Inner deadzone", 0, 99, 8, "%");
    help("A rescaled deadzone for the left stick.");
    slider speed("Camera sensitivity", 0, 200, 100, 5, "%");
    select camera_mode("Camera mode", 7) {
        7: "Normal";
        42: "Invert vertical";
        -3: "Invert both";
    };

    group("Timing");
    number pulse_ms("Pulse duration", 10, 500, 80, 5, " ms");
}

main {
    if (enabled) {
        deadzone(LX, zone);
        deadzone(LY, zone);
        sensitivity(RX, speed);
        sensitivity(RY, speed);
        if (camera_mode == 42 || camera_mode == -3) invert(RY);
        if (camera_mode == -3) invert(RX);
    }
    if (get_ival(RB) && is_available(SOUTH)) combo_run(Pulse);
    else combo_stop(Pulse);
}

combo Pulse {
    set_val(SOUTH, 100); wait(pulse_ms);
    set_val(SOUTH, 0); wait(pulse_ms);
}
```

Native XUSB/GIP controls are shown above. For generic HID devices, use the
descriptor-based `AXIS_n` / `BUTTON_n` slots displayed in the monitor.

## Declarations

| Declaration | Usage control / meaning |
| --- | --- |
| `slider name("Label", min, max, initial[, step][, "unit"]);` | A slider with a visible value. Step defaults to 1. |
| `toggle name("Label", true);` | An on/off switch, read as integer 1 or 0. |
| `select name("Label", initial) { value: "Choice"; ... };` | A dropdown. The script receives the declared value, independently of its list position. |
| `number name("Label", min, max, initial[, step][, "unit"]);` | An integer edit field, including negative numbers where allowed. |
| `title("Text");` / `description("Text");` | Script heading and instructions. |
| `group("Text");` | Section heading for following parameters; `group("")` ends the named section. |
| `help("Text");` | Help for the preceding parameter. |

One `ui` block per script; controls appear in declaration order. Names follow
ordinary IGC identifier rules and cannot collide with controls, builtins,
variables, constants or functions. The compiler reports declaration errors with
their line and column. Bounds/defaults can use previously declared numeric
`define` constants. Strings support UTF-8 labels and `\"`, `\\`, `\n`, `\r`,
`\t` escapes; strings remain unavailable as executable IGC values.

Each parameter is a read-only signed 32-bit integer in `init`, `main`, functions
and combos. Assignments or increments to it are compiler errors. UI settings
apply to all devices running that script; device globals and combos remain
independent. A value copied into a normal variable during `init` stays a copy:
read the parameter itself when you want its current value.

Numeric values must be in range and on the step lattice `min + n * step`.
The initial value must be valid; a dropdown's initial value must exist among
its unique choices. Maximums need not be reachable when not aligned to the
step. Fields reject overflow, invalid text and off-step values without sending
them to the runtime. Sliders with more than 10,000 steps use a bounded native
track scale; use a numeric field when every individual value must be reachable.
The `deadzone` builtin accepts at most 99; declaring a generic slider to 100
does not change a builtin's accepted argument range.

Limits: 64 parameters, 64 choices per dropdown and 1,024 UTF-8 bytes per UI
string, within the ordinary script size/token limits. These are scalar controls,
not arbitrary executable Windows widgets or string-valued runtime variables.

## Runtime and saved settings

Changing a valid parameter in **Use script** updates the active matching script
on its owning worker, wakes an idle worker and reevaluates current input. It
does not compile code, recreate capture, rerun `init`, reset globals, restart
combos or invent a physical press edge. A waiting combo keeps its current held
segment and deadline; its next segment reads the new values. Windows scheduling
and controller transport still affect delivery time.

Changing a setting while stopped prepares it for **Apply script**; it never
starts capture on its own. Parameters for edited but unapplied code do not
modify the older active program. **Live apply** in the editor controls code
replacement, separately from parameter updates. Applying changed code still
creates fresh per-device VM state, as before.

The active configuration also records its script path and applied values.
Two files containing identical code can have different saved settings: selecting
the other file does not silently change the first one's running configuration.
Reloaded values that differ from the active settings are shown as waiting for
Apply. Parameter edits update only the matching active file/version.

Settings are automatically saved beside the script as
`Example.igc.settings.json`, after a 300 ms UI editing pause and before switching
files or closing. This delay concerns disk persistence only, not the running
parameter update. Source text is unchanged. Keep the sidecar with the script
when moving or sharing your chosen values; omit it to use the author's defaults.
If the script directory or sidecar is read-only, settings are saved automatically
under `%LOCALAPPDATA%\GamePadRemapper\Settings`, identified by the script's
absolute path. The status line identifies this fallback. Once the sidecar is
writable, the next change saves beside the script and removes the fallback.
Unsaved scripts retain
settings in memory until saved, including across the application's elevation
handoff. **Reset settings** saves the declared defaults. Unknown or now-invalid
saved values fall back to defaults after an interface edit; malformed settings
are reported without being executed. A script without `ui` remains usable and
shows that it has no custom settings.

Code and Visual authoring preserve the `ui` declaration. Text inside interface
labels is never treated as a `main` block, editable `define` or visual metadata.
See [the full language](LANGUAGE.md) and
[the complete example](../Scripts/10%20-%20Custom%20Script%20Interface.igc).
