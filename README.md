# GamePad Remapper - Windows Controller Remapping, Scripts and Macros

A **complete C++ example application** showing how to use the
[Interceptor Gamepad & Controller kernel filter driver](https://github.com/TitanCSA/Interceptor-Gamepad-Controller)
and its SDK to build a real remapper. Use it as a ready-to-run tool or study its
source to build your own. This **native C++ Windows application** remaps gamepad controls, builds
timed macros and runs complex **`.igc` scripts**. Edit behavior in code or
with visual rules, create script-specific settings, and inspect live controller
**IN / OUT** state. Powered by the
[Interceptor Gamepad & Controller kernel filter driver](https://github.com/TitanCSA/Interceptor-Gamepad-Controller).

**[Download](https://github.com/TitanCSA/GamePad-Remapper/releases/latest)** |
[IGC language](docs/LANGUAGE.md) | [Script settings](docs/SCRIPT-INTERFACE.md) |
[Live IN / OUT](docs/LIVE-MONITOR.md) | [Build](docs/BUILD.md)

![Script settings beside the tabbed IN / OUT inspector; simulated session](docs/images/script-settings.png)

Screenshots show the native interface with simulated controller activity.
Normal operation displays measurements from the driver.

## Learn from the application

The driver intercepts physical reports; this example processes scripts in user
mode and returns modified input through the SDK. It does not install a second
virtual-controller driver. The source demonstrates:

- [Capture loop](App/Capture/Remapper.cpp): receive, decode, transform and forward input from the supported controller families.
- [Application service](App/Capture/Service.cpp): manage capture from the interface and expose its state.
- [Passive monitor](App/Capture/DriverMonitor.cpp): show measured IN / OUT states even without a running script.

For a smaller starting point, use the driver's
[five standalone controller examples](https://github.com/TitanCSA/Interceptor-Gamepad-Controller/tree/main/ExamplesConsole).

<a id="start"></a>

## Install and start

Requires **Windows 10/11 x64**. The application is a standalone C++ EXE, with no
.NET runtime or application DLL dependency.

1. Download and extract the **GamePad-Remapper** Windows x64 ZIP.
2. Run its **InterceptorSetup.exe** (the bundled Gamepad & Controller edition) and choose
   **Install / reinstall**. If requested, use Windows **Restart**, then reopen
   setup to finish device associations. **Driver setup** opens this same edition.
3. Launch **GamePadRemapper.exe** and accept the administrator prompt. Controller
   **IN / OUT observation starts automatically**, even without a script.
4. Open **Scripts**, choose an example, adjust its settings in **Use script**,
   then choose **Apply script** (**Ctrl+Enter**).
5. Choose **Stop** to release capture and restore passthrough. Live observation
   continues. Closing the application also releases capture.

For removal, stop the remapper and use the bundled driver installer's **Uninstall**.
The controller driver is Microsoft-signed; this signature does not automatically
sign the application EXE. [Driver setup and recovery](https://github.com/TitanCSA/Interceptor-Gamepad-Controller/blob/main/docs/INSTALLATION.md).

## Two workspaces, one script

| Workspace | Use it for |
| --- | --- |
| **Use script** | Choose a saved script and adjust its sliders, switches, dropdowns and numeric fields |
| **Edit script** | Write IGC code or edit visual remaps, conditions, sequences and motion rules |

Both workspaces use the same `.igc` file. A script's `ui` block defines the
settings shown in **Use script**. Parameter changes update an active script
without recompilation or resetting its macros. **Reset settings** restores defaults.

The **Code** editor provides a C-like language with variables, arrays, functions,
conditions, loops, input events and timed `combo` sequences. The **Visual** editor
provides remap/axis cards, nested **ALL / ANY / NOT** conditions, holds, double
taps, thresholds, timed sequences and figure-eight/circle/sine motion.
Advanced code is preserved when switching views.

Open **Scripts** for searchable examples and saved files. **Quick actions**
provides new/open/save-copy operations, the scripts folder and available draft
recovery. Use **Ctrl+S** to save. Optional **Live apply valid edits** updates an
already-running capture; invalid edits keep the last working configuration.

Scripts normally live in `Documents/GamePad Remapper/Scripts`, but files can be
opened from any folder. Parameter values are stored beside a script in
`.igc.settings.json`, with a user-profile fallback for read-only folders.

<a id="declare-your-own-settings"></a>

## Create a script with its own settings

This example uses standard XInput/GIP control names. It remaps the south button
to the east button, applies a configurable deadzone and optionally inverts camera Y:

```c
ui {
    title("My controller");
    slider zone("Left-stick deadzone", 0, 99, 8, "%");
    toggle invert_y("Invert camera Y", false);
}

main {
    remap(SOUTH, EAST);
    deadzone(LX, zone);
    deadzone(LY, zone);
    if (invert_y) invert(RY);
}
```

The physical east button remains usable: both sources can activate the destination.
For generic HID controllers, use the displayed `BUTTON_1`, `AXIS_1`, `TRIGGER_1`
and `HAT_1` controls; HID descriptors do not universally assign console button names.
See the [script interface guide](docs/SCRIPT-INTERFACE.md) and
[language reference](docs/LANGUAGE.md).

For complex behavior, combine conditions and actions or start from the included
figure-eight example: hold two buttons while the camera axis exceeds a threshold,
then add motion around the physical position of another stick. Code supports
custom state and timed combos; visual rules support activation, deactivation,
repeat and toggle modes. [Script examples](Scripts).

## Live controller IN / OUT

Select a controller tab on the right. Each control has a stable row showing
physical **IN** beside the driver's **OUT**; changed OUT values are highlighted.
Missing measurements appear as a dash. Scroll to inspect additional controls,
or resize the script/inspector divider.

Observation is always active on normal launch and works independently of scripts.
It reconnects when the driver becomes available. **Copy diagnostics** includes
raw reports, timestamps, counts and errors. OUT measures successful completions
towards Windows, not confirmation that every application consumed a report.
[Measurement details](docs/LIVE-MONITOR.md).

## How remapping works

A worker captures supported physical reports, executes the compiled script and
returns modified input through the kernel filter into the associated device's
native Windows paths. Scripts execute in the application; applying one does not
load script code into the kernel or flash controller firmware. No per-game
integration is required for these supported input paths.

Compatibility follows the driver's transport, descriptor and protocol profiles,
including supported **DS4 / DualShock 4, DualSense, wired Xbox 360 and Xbox One /
Series** controllers. Unsupported vendor paths and proprietary formats are
outside that scope. [Driver compatibility](https://github.com/TitanCSA/Interceptor-Gamepad-Controller/blob/main/docs/COMPATIBILITY.md).

IGC is an original language inspired by GPC, with its own compiler and bounded
runtime. It is not a drop-in implementation of every Cronus firmware API.
The [language guide](docs/LANGUAGE.md) defines supported syntax and limits.
Measured software costs and test scope are in [validation](docs/VALIDATION.md).

<a id="source-layout"></a>

## Source and build

```text
App/          C++ application, UI, compiler, runtime, capture and tests
Scripts/      Ready-to-edit .igc examples
docs/         Guides and screenshots
Interceptor/  The two-file controller SDK
```

Build with `powershell -ExecutionPolicy Bypass -File .\Build.ps1`.
Everything in the application is C++17; the SDK is linked into the EXE.
Generated files stay in ignored `App/.build/`. [Build instructions](docs/BUILD.md).

Editing and compiling scripts work without an installed driver. Offline preview
(`--demo`) and checking (`--check-script file.igc`) do not request elevation.
Live capture and telemetry require administrator rights. Use **Quick actions >
Reduce interface motion** to disable optional UI animations.

## Driver demo and license

The bundled controller driver allows **two hours of capture per Windows kernel
boot**, starting with the first successful capture and shared across applications
using that edition. Closing or reinstalling does not reset it. At expiry, capture
stops and physical input passes through normally. Use Windows **Restart** for a
new allowance. Passive IN / OUT observation does not start or reset this timer.
For the unrestricted driver, DM **`titancsa`** on Discord.

The remapper is [MIT-licensed](LICENSE); driver binaries have
[separate terms](DRIVER-BINARY-LICENSE.txt). Kernel source and signing credentials
are not published. Built by **TitanCSA**, using
[Interceptor Gamepad & Controller](https://github.com/TitanCSA/Interceptor-Gamepad-Controller)
and the interception workflow popularized by
[oblitum/Interception](https://github.com/oblitum/Interception). No affiliation with
Cronus, oblitum, Microsoft, Sony or other controller manufacturers.

## Responsible use and disclaimer

This project, including its drivers, SDKs, libraries, applications, installers and examples, was created for lawful, constructive purposes such as accessibility, device customization, research, testing and software development. Illegal, malicious or unauthorized use is neither intended nor endorsed by TitanCSA or the contributors.

You are solely responsible for how you use, modify or distribute the software, for obtaining any necessary authorization, and for complying with applicable laws, third-party rights and service rules. TitanCSA and the contributors do not accept responsibility for users' actions, including illegal activity or misuse of the drivers or SDKs.

To the maximum extent permitted by applicable law, the software is provided "AS IS", without warranties of any kind, and TitanCSA and the contributors are not liable for claims, damages, data loss, system damage or other consequences arising from its use or misuse. Nothing in this notice excludes liability that cannot legally be excluded. Existing project and third-party license terms remain applicable.
