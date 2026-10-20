// Resource-only test fixture. It is read as a file and never executed by a test.
extern "C" __declspec(dllexport) unsigned InstallerFixture() { return 1; }
