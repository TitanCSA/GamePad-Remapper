# Build and test

Windows x64, Visual Studio 2022 / Build Tools with **Desktop development with
C++**, a Windows SDK, **.NET Framework 4.8 targeting pack**, and CMake 3.20+.
There are no NuGet or JavaScript dependencies. The driver and WDK are not
needed to build, edit, compile scripts, render the UI or run pure tests.

```powershell
.\Build.ps1                 # native adapter, language engine, app and pure tests
.\Build.ps1 -Target Test    # repeat tests against existing binaries
.\Build.ps1 -Target Managed # rebuild C# only
.\Build.ps1 -Render         # also render code/visual screenshots without capture
.\Build.ps1 -Target Package # test, download pinned installer and make portable ZIP
```

The application is `bin/GamePadRemapper.exe`; keep its two DLLs alongside it.
`dist/` contains the portable ZIP, the original Interceptor UI installer and
SHA-256 checksums. Packaging verifies the exact installer SHA-256 against the
referenced driver release. It never rebuilds or resigns a driver.

`Core/` implements the independent IGC compiler, runtime and visual document
model. `App/` is WPF, with a native callback adapter in `DriverBridge.cs`.
`Native/` handles capture ownership, descriptor-based controller conversion,
timers and forwarding through the unmodified two-file `Interceptor/` SDK.
`Scripts/` contains editable examples. `Tests/` contains software tests.

The build compiles optimized x64 binaries with warnings as errors. C++ uses
the static CRT; the app needs the .NET Framework 4.8 runtime supplied with
current Windows 10/11 installations. Application and installer EXEs are not
covered by the embedded driver's Microsoft signature.

Pure tests and UI render tests never create a capture or install a driver.
Live tests are separate, explicitly guarded laboratory entry points. Do not
run capture tests on a production desktop. See [validation](VALIDATION.md) for
what was measured and the limits of those results.
