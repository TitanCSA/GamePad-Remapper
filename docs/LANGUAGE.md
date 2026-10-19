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

Lowercase variable names such as `a`, `x` and `y` are valid. Control constants are uppercase. `TRUE`/`FALSE` and `true`/`false` are available.

## Controls

Native XUSB/GIP semantic controls are `LX LY RX RY LT RT UP DOWN LEFT RIGHT SOUTH EAST WEST NORTH LB RB VIEW MENU LS RS GUIDE TOUCH`. Positional face-button names avoid ambiguity between Xbox and PlayStation labels. `A/B/X/Y`, `CROSS/CIRCLE/SQUARE/TRIANGLE` and familiar `XB1_`, `XB360_`, `PS4_`, `PS5_` names are aliases for these slots.

For generic HID controllers, **descriptor identities are authoritative**: `BUTTON_1..64`, `AXIS_1..64`, `TRIGGER_1..32`, `HAT_1..16`, `VALUE_1..48`. The native adapter sorts complete field identities from Windows preparsed descriptors so report arrival order does not change a saved mapping. If descriptor seeding fails, diagnostics identify the capture-local discovery fallback. These names are not a model database. A generic HID `AXIS_1` does not necessarily mean the left stick. Check `is_available()` and the application's monitor. Unsupported overflow fields remain unmodified by the native adapter.

Unavailable controls read as neutral (`-1` for hats, `0` otherwise) and cannot generate phantom press edges. A newly available, physically pressed control generates one real press edge.

Axis values use `-100..100`, triggers `0..100`, buttons `0/100`, and hats `-1` (neutral) or `0..7`. Generic value fields and relative or non-normalizable scalar axes use raw `VALUE_n` slots within the language's signed 32-bit range; larger unsigned values are clipped in the script view. Writes to normalized controls clamp to their respective range; the native HID encoder additionally honors descriptor bounds. Percent rounding can lose sub-percent precision for a modified axis. Unchanged fields preserve the original report bytes.

## Builtins

| Function | Result / effect |
|---|---|
| `get_val(control)` | Current output, including earlier changes in this frame. |
| `get_ival(control)` | Original physical input for this frame. |
| `get_lval(control)` | Previous frame's original physical input. |
| `set_val(control, value)` | Set output; return its clamped value. |
| `event_press(control)`, `event_release(control)` | Physical neutral/active edge, once per transition. Axis activity means nonzero; hat activity means not `-1`. |
| `get_ptime(control)` | Milliseconds since the last physical neutral/active transition. |
| `get_brtime(control)` | Current released duration, or the released duration immediately before the current press. |
| `get_rtime()`, `get_runtime()` | Current frame interval, or time since this instance first ran, in milliseconds. |
| `is_available(control)` | Whether this report/device exposes that control. |
| `get_controller()`, `get_device()` | Native family ID or current capture-local device key. |
| `remap(source, target)` | Copy physical source to target and neutralize source. |
| `swap(first, second)` | Exchange current output values. |
| `invert(control)` | Negate the current value; useful for signed axes. |
| `block(control, milliseconds)` | Neutralize until the monotonic deadline. Calling again restarts the deadline; `0` cancels. |
| `deadzone(control, threshold[, secondControl])` | Scalar rescaled inner deadzone, threshold `0..99`; optionally apply the same threshold to another axis. This is not GPC's radial/multi-argument deadzone API. |
| `sensitivity(control, percent)` | Multiply by `0..1000%`, clamped to the control range. Three arguments accept `(control, ignoredMidpoint, percent)` for migration; no midpoint curve is simulated. |
| `abs(v)`, `min(a,b)`, `max(a,b)`, `clamp(v,min,max)` | Integer arithmetic helpers. `abs(INT_MIN)` saturates to `INT_MAX`. |
| `rand(min,max)`, `random(min,max)` | Inclusive deterministic per-instance pseudorandom integer; not cryptographic. |
| `get_bit(v,index)`, `set_bit(v,index,bit)` | Read a bit, or return a changed word; index `0..31`. |
| `pow(base, exponent)`, `isqrt(v)`, `hypot(a,b)` | Integer power (exponent `0..31`), floor square root, or saturated integer hypotenuse. |
| `get_pvar(slot,min,max,default)`, `set_pvar(slot,value)` | 64 session-local saved values. No disk persistence; Stop, reinjection or Reset clears them. |
| `combo_run(name)` | Start an idle combo; leave a running combo alone. |
| `combo_restart(name)`, `combo_stop(name)` | Restart from the beginning, or stop and release held outputs. |
| `combo_running(name)`, `combo_stop_all()` | Query running state, or stop all combos. |
| `wait(milliseconds)` | Yield a combo without blocking the worker. Valid only directly inside a combo; `0` schedules at least one millisecond. |
| `vm_tctrl(milliseconds)` | Select `1..20 ms` scheduling for time-aware main logic; default `1`. |

## Timing and precedence

Each execution starts from the newest physical state, runs main, applies active blocks, then applies combos in declaration order. Later combos win when they change the same control. The values assigned during a combo segment remain active until that segment's `wait` expires. At the next segment they are released to physical input unless assigned again. Completion, Stop, reinjection, capture failure and script fault restore physical forwarding.

Timers use the host's supplied monotonic microsecond clock. They are deadlines, not busy waits. Scheduling, Windows load, hardware report cadence, and the driver determine when the next update reaches an application. Zero latency is not possible or claimed.

Scripts without active timing logic or macros are event-driven. Calling `get_ptime`, `get_brtime`, `get_rtime` or `get_runtime` in the currently executed main path schedules the next periodic evaluation; an inactive branch or unused function does not poll. Calls inside a waiting combo need only that combo's deadline. `block` likewise uses a single deadline. Explicit `vm_tctrl` requests continuous periodic evaluation at the selected cadence until Stop/reinjection. Ordinary counters increment once per actual execution, not at a fixed emulated GPC firmware frequency. Unlike console hardware, no authentication, USB protocol emulation, rumble/LED/OLED, accelerometer/touch-surface or adaptive-trigger APIs are invented here.

## Bounds and failure behavior

Limits: 524,288 text characters (the editor also caps UTF-8 files at 1 MiB), 100,000 tokens, 96 syntax/expression nesting, 32,768 compiled instructions, 4,096 global integers, 1,024 elements per array, 256 locals per function, 16 arguments, 32 nested calls, 32 combos, and 1,024 expression-stack values. One frame shares a 10,000 instruction budget across init, main and all combos. `wait` and `block` accept at most 24 hours. A long loop cannot hang the worker; a fault reports its source line/column and restores the frame to physical input. The service stops the faulty script.

## Code and visual editing

Visual view owns a marked, hashed generated section and one marked call at the end of main. Remaps, axis transforms, disabled rules, editable numeric defines and timed macro steps roundtrip through that section. Existing custom blocks remain in the file and run before the visual overlay. Numeric `define` values in custom code can be edited; their names and references must be renamed in code view.

If generated code is hand-edited, its checksum changes. The edited section becomes preserved advanced code rather than being rebuilt from stale visual metadata. New visual rules can still be added in a fresh section. Arbitrary user code has no universal equivalent visual graph; it remains clearly identified as advanced code rather than being discarded or presented as fully represented by the cards.
