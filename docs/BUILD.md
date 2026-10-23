# Build and test

Windows 10/11 x64, Visual Studio 2022 or Build Tools with **Desktop development
with C++**, a Windows SDK, and CMake 3.20+. No C# compiler, .NET targeting pack, NuGet,
JavaScript runtime, WDK or installed driver is needed to build the application.
The JSON dependency is pinned and included in the source tree. The optional
AddressSanitizer configuration needs the C++ AddressSanitizer component and
MSVC 14.38 or newer; the build selects an installed compiler/runtime pair and
records it for repeat tests.

```powershell
.\Build.ps1                     # compile C++ and run offline tests
.\Build.ps1 -Target Build       # compile only
.\Build.ps1 -Target Test        # test existing binaries
.\Build.ps1 -Render             # also render the native interface without capture
.\Build.ps1 -AddressSanitizer   # separate instrumented build and tests
.\Build.ps1 -Target Package -DriverInstaller 'C:\Downloads\InterceptorSetup.exe'
                                # test and package with the pinned driver installer
```

The executable is **`App/.build/Release/bin/GamePadRemapper.exe`**. All application
components and the Interceptor user-mode SDK are linked into this one EXE.
The C++ runtime is statically linked. No application DLLs or CLR are required.
Windows system libraries remain ordinary operating-system dependencies.

The repository has four content folders:

| Folder | Content |
| --- | --- |
| `App` | C++ application, language engine, capture adapter, native UI, tests and packaging tools |
| `Scripts` | Editable `.igc` examples |
| `docs` | Build, language and validation documentation, with interface images |
| `Interceptor` | Original two-file C++ user-mode SDK |

Build products, private validation evidence and release archives are ignored
under `App/.build/`. CMake and the build entry point remain at the root beside
the README and license notices. There is no separate Core, Native, Tests or
Tools folder at the root.

Builds use optimized x64 C++17, `/W4 /WX`, a static CRT, ASLR, DEP and Control Flow
Guard. AddressSanitizer is a separate validation configuration, not a release
dependency. Offline tests do not create capture or install a driver. Explicit
live tests additionally require a dedicated Hyper-V guest and laboratory opt-in.

Packaging creates the portable ZIP, standalone application, a single driver
installer named `InterceptorSetup.exe`, and `SHA256SUMS.txt` in
`App/.build/dist/`. The ZIP contains the application, examples, documentation
and original signed-driver installer. The exact
driver installer hash is verified; packaging never rebuilds or signs a driver.
Microsoft signatures cover the embedded kernel driver package, not this app
or the installer EXE.

Version **2.3.4** packages the verified controller installer supplied in the
**1.0.12.0-demo** bundle: installer **1.0.12.0**, Microsoft-signed kernel **1.0.12.0**
and SDK **1.0.4.2**. Supply that release's original installer to
`-DriverInstaller`; older installers and the classic
Interceptor edition are rejected. Release tags, installer VERSIONINFO and kernel
versions are recorded separately in the generated `package-manifest.json`.

The thirteen CTest suites include passive driver-monitor fixtures, the script-interface compiler/VM fixtures,
concurrent parameter-publication checks and the actual capture worker against
an isolated transport. **ControllerView** covers controller selection, stable
IN/OUT rows, scrolling, field validity and bounded painting resources.
**ScriptLibrary** covers grouped filtering, popup selection and keyboard behavior.
**ScriptPanel** verifies measured wrapping of long descriptions, labels and help,
slider readout spacing, native callbacks and scrolling, and preservation of
controls and unfinished edits during resize and DPI changes.
Native widget tests cover animation completion, reversal, cancellation,
reduced motion, resizing and destruction. The offline UI test also exercises
the Scripts menu, adjustable workspace, custom controls, sidecar/fallback storage,
workspace switching and DPI/resource lifetimes. Only `.igc` examples are packaged; personal `.igc.settings.json`
values and recovery drafts are excluded.

Packaging also rejects managed or AddressSanitizer-instrumented application
binaries. It copies the supplied driver installer unchanged; no signing tool,
private key or kernel source is needed to build the application.

Version 2.3.2 changes the script interpreter's `remap` behavior to preserve the
target's physical input. The driver protocol and capture worker are unchanged.
The inspector paints the latest measured values directly;
its short interface animations are separate from report delivery. Render tests
produce explicitly simulated previews, including multiple controller tabs and
the searchable library popup. They do not establish new physical-device support
or measure hardware latency. See [validation](VALIDATION.md) for test scope.

Normal interactive startup requests elevation before creating the application
window, then starts passive observation automatically. Offline UI/render tests,
`--demo` and `--check-script` remain separate and never request elevation.
