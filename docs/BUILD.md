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
.\Build.ps1 -Target Package     # test and create the portable release archive
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

Packaging creates the portable ZIP, standalone application, driver installer
under both its descriptive and legacy filenames, and `SHA256SUMS.txt` in
`App/.build/dist/`. The ZIP contains the application, examples, documentation
and original signed-driver installer. The exact
driver installer hash is verified; packaging never rebuilds or signs a driver.
Microsoft signatures cover the embedded kernel driver package, not this app
or the installer EXE.

The nine CTest suites include the script-interface compiler/VM fixtures,
concurrent parameter-publication checks and the actual capture worker against
an isolated transport. The offline UI test also exercises
custom controls, sidecar/fallback storage, workspace switching and DPI/resource
lifetimes. Only `.igc` examples are packaged; personal `.igc.settings.json`
values and recovery drafts are excluded.
