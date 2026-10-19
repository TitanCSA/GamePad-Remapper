# IGC language and runtime

IGC is an original, sandboxed C-like scripting language inspired by the familiar `main`, `combo`, `get_val` and `set_val` workflow of [Cronus GPC](https://guide.cronus.support/gpc/gpc-scripting-core-controller-functions). It is **not a GPC compiler, a Cronus firmware emulator, or a promise that existing GPC files run unchanged**. No Cronus firmware, libraries or source code are included.

The application compiles each `.igc` file once to immutable bytecode. Each intercepted device gets its own independent VM, variables, event history and macro state. Program installation is atomic at the native worker boundary. Script execution cannot open files, use the network, start a process, import libraries or call the operating system.

## Statements and values

- Signed 32-bit `int`, `define NAME = constant;`, `const int NAME = constant;`, fixed or inferred-size integer arrays, and read-only `const int table[] = { ... };`.
- Global and function-local variables. Globals persist within a running instance. Locals are cleared at function entry; names are function-scoped and cannot shadow another declaration.
- `init { ... }` runs once before the first `main`. `main { ... }` handles each physical input and scheduled timer tick.
- `if` / `else`, `while`, `for`, `break`, `continue`, `return`, user `function name(args) { ... }`, and `combo name { ... }`.
- Arithmetic `+ - * / %`, comparisons, short-circuit `&& ||`, bitwise `& | ^ ~ << >>`, unary `!`, ternary `?:`, assignment and compound assignment, prefix/postfix `++ --`.
- Integer overflow wraps in two's complement; division truncates toward zero. Shift counts use their low five bits. A zero divisor, invalid array index or invalid function argument faults the script.
- Decimal and hexadecimal literals; `// line` and `/* block */` comments. Strings, dynamic allocation, classes, pointers, imports, GPC `data(...)` and hardware-specific declarations are unavailable.

Lowercase variable names such as `a`, `x` and `y` are valid. Control constants are uppercase. `TRUE`/`FALSE` and `true`/`false` are available. Language keywords and builtin names cannot be redeclared. `main` and `init` must be actual entry blocks, not function or combo names. Array lengths must be positive; only omitted `[]` lengths are inferred from an initializer.

## Controls

Native XUSB/GIP semantic controls are `LX LY RX RY LT RT UP DOWN LEFT RIGHT SOUTH EAST WEST NORTH LB RB VIEW MENU LS RS GUIDE TOUCH`. Positional face-button names avoid ambiguity between Xbox and PlayStation labels. `A/B/X/Y`, `CROSS/CIRCLE/SQUARE/TRIANGLE` and familiar `XB1_`, `XB360_`, `PS4_`, `PS5_` names are aliases for these slots.

For generic HID controllers, **descriptor identities are authoritative**: `BUTTON_1..64`, `AXIS_1..64`, `TRIGGER_1..32`, `HAT_1..16`, `VALUE_1..48`. The native adapter sorts complete field identities from Windows preparsed descriptors so report arrival order does not change a saved mapping. If descriptor seeding fails, diagnostics identify the capture-local discovery fallback. These names are not a model database. A generic HID `AXIS_1` does not necessarily mean the left stick. Check `is_available()` and the application's monitor. Unsupported overflow fields remain unmodified by the native adapter.

Unavailable controls read as neutral (`-1` for hats, `0` otherwise) and cannot generate phantom press edges. A newly available, physically pressed control generates one real press edge.

Axis values use `-100..100`, triggers `0..100`, buttons `0/100`, and hats `-1` (neutral) or `0..7`. Generic value fields and relative or non-normalizable scalar axes use raw `VALUE_n` slots within the language's signed 32-bit range; larger unsigned values are clipped in the script view. Writes to normalized controls clamp to their respective range; the native HID encoder additionally honors descriptor bounds. Percent rounding can lose sub-percent precision for a modified axis. Unchanged fields preserve the original report bytes.

Decoded relative scalars represent movement in the current physical report, not a held position. They return to zero for timer work and unrelated reports; synthetic templates neutralize those previous deltas so an unrelated timed button press does not replay movement. Explicit raw `VALUE_n` writes can generate new relative movement in their owning report. Unknown bytes and wide binary fields remain outside this scalar behavior; no vendor-specific interpretation is invented.

## Builtins

| Function | Result / effect |
|---|---|
| `get_val(control)` | Current output, including earlier changes in this frame. |
| `get_ival(control)` | Original physical input for this frame. |
| `get_lval(control)` | Previous frame's original physical input. |
| `set_val(control, value)` | Set output; return its clamped value. |
| `add_val(control, delta)` | Add to current output using a 64-bit intermediate; return the clamped result. Earlier transforms and additions are preserved. |
| `event_press(control)`, `event_release(control)` | Physical neutral/active edge, once per transition. Axis activity means nonzero; hat activity means not `-1`. |
| `double_tap(control, windowMs)` | True on a press with a previous distinct press at most `windowMs` earlier. First press is false; holding is not another press. Window `1..86,400,000 ms`, measured press-to-press without millisecond rounding. Repeated queries in one frame return the same result. |
| `get_ptime(control)` | Milliseconds since the last physical neutral/active transition. |
| `get_brtime(control)` | Current released duration, or the released duration immediately before the current press. |
| `get_rtime()`, `get_runtime()` | Current frame interval, or time since this instance first ran, in milliseconds. Values saturate at `INT_MAX`; use `get_phase` for indefinitely repeating motion. |
| `get_phase(periodMs)` | Monotonic elapsed milliseconds modulo `periodMs`, in `0..periodMs-1`. Period `1..86,400,000 ms`; the 64-bit clock is reduced before conversion to `int`. |
| `is_available(control)` | Whether this report/device exposes that control. |
| `get_controller()`, `get_device()` | Native family ID or current capture-local device key. |
| `remap(source, target)` | Copy physical source to target and neutralize source. Does nothing unless both controls are available, so a missing target cannot consume an existing button. |
| `swap(first, second)` | Exchange current output values when both controls are available; otherwise leave them unchanged. |
| `invert(control)` | Negate the current value; useful for signed axes. |
| `block(control, milliseconds)` | Neutralize until the monotonic deadline. Calling again restarts the deadline; `0` cancels. |
| `deadzone(control, threshold[, secondControl])` | Scalar rescaled inner deadzone, threshold `0..99`; optionally apply the same threshold to another axis. This is not GPC's radial/multi-argument deadzone API. |
| `sensitivity(control, percent)` | Multiply by `0..1000%`, clamped to the control range. Three arguments accept `(control, ignoredMidpoint, percent)` for migration; no midpoint curve is simulated. |
| `abs(v)`, `min(a,b)`, `max(a,b)`, `clamp(v,min,max)` | Integer arithmetic helpers. `abs(INT_MIN)` saturates to `INT_MAX`. |
| `rand(min,max)`, `random(min,max)` | Inclusive deterministic per-instance pseudorandom integer; not cryptographic. |
| `get_bit(v,index)`, `set_bit(v,index,bit)` | Read a bit, or return a changed word; index `0..31`. |
| `pow(base, exponent)`, `isqrt(v)`, `hypot(a,b)` | Integer power (exponent `0..31`), floor square root, or saturated integer hypotenuse. |
| `sin(degrees[, amplitude])`, `cos(degrees[, amplitude])` | Degree-based sine/cosine scaled by `amplitude` (default `100`). Any signed 32-bit angle/amplitude; negative amplitudes reverse direction. Results round to nearest integer, half away from zero, and saturate to the integer range. |
| `atan2(y, x)` | Direction in degrees, rounded to an integer in `-180..180`; `(0, 0)` returns `0`. |
| `lerp(from, to, percent)` | `from + (to-from)*percent/100`, using safe 64-bit intermediates and division toward zero. Percent `0..100`; endpoints are any signed integers. |
| `wave_sin(periodMs, amplitude[, phaseDegrees])`, `wave_cos(periodMs, amplitude[, phaseDegrees])` | A scaled sine/cosine wave driven by the monotonic clock since this instance first ran. Period `1..86,400,000 ms`; signed integer amplitude and degree offset (default `0`). Same rounding as `sin`/`cos`. Clock phase is recomputed, never accumulated. |
| `get_pvar(slot,min,max,default)`, `set_pvar(slot,value)` | 64 session-local saved values. No disk persistence; Stop, reinjection or Reset clears them. |
| `combo_run(name)` | Start an idle combo; leave a running combo alone. |
| `combo_restart(name)`, `combo_stop(name)` | Restart from the beginning, or stop and release held outputs. |
| `combo_running(name)`, `combo_stop_all()` | Query running state, or stop all combos. |
| `wait(milliseconds)` | Yield a combo without blocking the worker. Must be a standalone statement directly inside a combo, including its loops/branches; not an argument, assignment or condition. `0` schedules at least one millisecond. |
| `tick(milliseconds)` | Request another evaluation at `1..20 ms` on this executed path. Call again while the behavior remains active. Multiple calls select the smallest interval; earlier combo/block deadlines still win. Overrides the default clock-function cadence for this pass. Returns `0`. |
| `vm_tctrl(milliseconds)` | Legacy persistent `1..20 ms` cadence; default clock-function cadence is `1 ms`. Once called, periodic work continues even if the original branch later becomes inactive. Prefer scoped `tick` for conditional motion. |

## Timing and precedence

Each execution starts from the newest physical state, runs main, applies active blocks, then applies combos in declaration order. `set_val` replaces the value at that point; `add_val` adds to it. Later combos compose with or replace earlier output according to their actions.

During a combo's `wait`, absolute assignments stay held, while additions are reapplied to the current output from main and earlier combos. A physical `LX` of `10` plus a held offset of `20` produces `30`; moving physical `LX` to `30` produces `50`. It does not accumulate another `20` on every timer tick. Intermediate clipping is preserved: starting at `80`, `add_val(LX,80)` then `add_val(LX,-80)` produces `20`, including during the hold. At the next segment, held actions are released unless assigned again. Completion, Stop, reinjection, capture failure and script fault restore physical forwarding.

Stopping the currently executing combo, including from a called function or `combo_stop_all`, immediately abandons that combo's continuation and cancels its current segment's writes. Restarting itself abandons the old expression/call stack and starts the new combo within the same shared instruction budget; an endless immediate-restart loop faults instead of overflowing a stack. If a later combo stops an earlier combo whose output was already applied in this pass, a one-shot cleanup evaluation is requested after `1 ms`, even without another physical report. Windows scheduling still affects the actual delivery time.

Timers use the host's supplied monotonic microsecond clock. They are deadlines, not busy waits. Scheduling, Windows load, hardware report cadence, and the driver determine when the next update reaches an application. Zero latency is not possible or claimed.

Scripts without active timing logic or macros are event-driven. Calling `get_ptime`, `get_brtime`, `get_rtime`, `get_runtime`, `get_phase`, `wave_sin` or `wave_cos` in the currently executed main path requests the default `1 ms` evaluation cadence. An inactive branch or unused function does not poll. Put `tick(4)` in that active branch to choose `4 ms` for the current pass; leaving the branch removes its periodic request. Earlier combo/block deadlines remain respected. Calls inside a waiting combo need only that combo's deadline; use main logic for continuously recomputed motion. `block` uses one deadline. Explicit `vm_tctrl` retains its continuous cadence until Stop/reinjection, for compatibility.

An edge is computed once from physical input before script changes: repeated `double_tap` or `event_press` calls cannot consume it. Double-tap history clears when the control is unavailable or the instance is reset. The test is rolling: with three sufficiently close distinct presses, the second and third can both qualify. No timer is needed while waiting for a second press.

Ordinary counters increment once per actual execution, not at a fixed emulated GPC firmware frequency. Use `get_phase` and deadlines instead of assuming a precise frame interval. Unlike console hardware, no authentication, USB protocol emulation, rumble/LED/OLED, accelerometer/touch-surface or adaptive-trigger APIs are invented here.

## Chord plus additive figure eight

This behavior holds A + B and requires camera `RX > 50%`. With physical `LX=10`, `LY=-20`, a quarter-cycle offset of `(+30, 0)` produces `(40, -20)`; the physical position remains the center. If that physical position moves, the pattern follows it on the next frame.

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

The X sine runs once per cycle; the Y sine runs twice, making an infinity shape. Each activation starts at phase zero. `get_phase` derives phase from monotonic time, so missed ticks do not accumulate drift and long uptimes do not freeze it. Output resets from physical input each evaluation; the same offsets are never repeatedly added to yesterday's output. With earlier transforms, `add_val` adds to their current output instead. Releasing the condition removes this motion at the next processed physical input; missing target axes prevent activation. Axis limits can clip a lobe near an edge.

For generic HID, bind `BUTTON_n` and `AXIS_n` from the monitor; do not infer button labels from a brand. See [Additive Figure Eight](../Scripts/07%20-%20Additive%20Figure%20Eight.igc), [Generic HID Orbit](../Scripts/08%20-%20Generic%20HID%20Orbit.igc) and [Chords, Hold and Double Tap](../Scripts/09%20-%20Chords%20Hold%20and%20Double%20Tap.igc).

The primitives also allow custom curves, nested state machines, tables and conditional sequences within the VM's limits. Motion is not limited to the built-in visual shapes; those are editable starting points, not a claim of literally unlimited computation.

## Bounds and failure behavior

Limits: 524,288 text characters (the editor also caps UTF-8 files at 1 MiB), 100,000 tokens, 96 syntax/expression nesting, 32,768 compiled instructions, 4,096 global integers, 1,024 elements per array, 256 locals per function, 16 arguments, 32 nested calls, 32 combos, and 1,024 expression-stack values. One frame shares a 10,000 instruction budget across init, main and all combos. `wait` and `block` accept at most 24 hours. A long loop cannot hang the worker; a fault reports its source line/column and restores the frame to physical input. The service stops the faulty script.

## Code and visual editing

Visual view owns a marked, hashed generated section and one marked call at the end of main. Remaps, axis transforms, disabled rules, editable numeric defines and timed macro steps roundtrip through that section. Existing custom blocks remain in the file and run before the visual overlay. Numeric `define` values in custom code can be edited; their names and references must be renamed in code view.

- Triggers combine nested **ALL**, **ANY** and **NOT**, held/pressed/released controls, signed or absolute-value comparisons, held durations, double taps and one custom IGC expression.
- Sequences support **On activation**, **While true**, **On deactivation** and **Toggle**. While-true sequences repeat while their condition holds and stop when it becomes false. Steps support **Add** and **Replace**.
- Continuous motion offers figure eight, circle and sine, separate signed axes, `0..100%` width/height, `1..3,600,000 ms` cycle, `1..20 ms` tick, and live/activation/zero center selection. **Add** follows existing output; fixed or zero centers use **Replace**. The infinity recipe defaults to a `4 ms` tick, `25%` width/height and a `1,000 ms` cycle.
- Motion **On activation** / **On deactivation** runs one cycle from that edge; a new matching edge restarts its phase and duration. **While true** runs until the condition becomes false, and **Toggle** runs until the next matching activation. A one-shot uses a monotonic combo deadline, then releases its output on the following scheduled evaluation (normally one selected tick later). Windows scheduling can add delay. Missing target axes stop motion safely.
- Visual documents allow 32 macros, 256 rules/constants, 256 steps per macro, and conditions bounded to 128 nodes, 12 nested levels and 16 children per ALL/ANY group. Visual held/double-tap fields accept `1..60,000 ms`; raw language timing primitives allow the larger bounds listed above. New group controls in the UI stop offering deeper groups after level five; existing deeper valid code remains accessible.

Choose the infinity recipe to create the A + B / RX threshold behavior in one click, then adjust the card. Plain-language summaries show the selected condition, timing and blend. Editor changes are validated in the background after a `400 ms` editing pause; this debounce does not delay input processing. **Live apply valid edits** is optional per session: after an explicit **Inject configuration**, valid edits can atomically replace the active program without restarting capture. Invalid edits leave the working program active; Stop/failure cannot be restarted by validation. A successful replacement starts fresh per-device VM state and releases the old macro holds. **Ctrl+Enter** applies manually; **Ctrl+S** saves. Unsaved recovery drafts do not overwrite saved `.igc` files.

If generated code is hand-edited, its checksum changes. The edited section becomes preserved advanced code rather than being rebuilt from stale visual metadata. New visual rules can still be added in a fresh section. Arbitrary user code has no universal equivalent visual graph; it remains clearly identified as advanced code rather than being discarded or presented as fully represented by the cards.
