# Third-party notices

`Interceptor/Interceptor.h` and `Interceptor.cpp` are the public user-mode C++17
SDK from [TitanCSA/Interceptor-Gamepad-Controller](https://github.com/TitanCSA/Interceptor-Gamepad-Controller),
version 1.0.0.0, copyright 2026 TitanCSA, MIT licensed. Kernel driver source,
private signing tools and credentials are not part of this repository.

The `InterceptorSetup.exe` release asset is the unmodified installer from that
project. Its embedded driver package is signed by Microsoft Windows Hardware
Compatibility Publisher. This signature does not sign the remapper application
or the installer EXE. The kernel binary's separate demo license is reproduced
in [DRIVER-BINARY-LICENSE.txt](DRIVER-BINARY-LICENSE.txt). Installing the driver
is an explicit user action, separate from opening or editing scripts.

IGC is an independent language inspired by the public GPC programming model.
It includes original implementation code and uses no Cronus firmware, compiler,
SDK, script library or proprietary runtime. Cronus, GPC, Xbox, PlayStation,
DualShock, DualSense and other third-party names are their owners' names or
trademarks. This project is not affiliated with or endorsed by those owners.

The interception workflow follows [oblitum/Interception](https://github.com/oblitum/Interception).
This application uses the distinct Interceptor Gamepad Controller SDK and is
not ABI compatible with the original Interception driver.
