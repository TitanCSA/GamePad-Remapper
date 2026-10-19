using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.IO;
using System.Web.Script.Serialization;
using GamePadRemapper.Core;

namespace GamePadRemapper
{
    public sealed class ServiceDevice
    {
        public uint Key, Family;
        public string Name = "", Token = "", Diagnostic = "";
        public int[] Values = new int[256];
        public bool[] Available = new bool[256];
        public string[] Controls = ControlsNames;
        private static readonly string[] ControlsNames = GamePadRemapper.Core.Controls.Names;
    }

    public sealed class ServiceSnapshot
    {
        public string Status = "Ready", Error = "";
        public bool Running;
        public ulong Received, Forwarded, Changed, TimerOutputs, Faults;
        public double ProcessingMicroseconds;
        public ServiceDevice[] Devices = new ServiceDevice[0];
    }

    // Only the native worker calls the script VM. UI snapshots are detached
    // copies and cannot change an intercepted packet or contend with the VM.
    public sealed unsafe class RemapperService : IDisposable
    {
        private const string NativeDll = "GamePadRemapper.Native.dll";
        private static readonly Lazy<bool> RuntimeWarmup = new Lazy<bool>(PrepareRuntime, LazyThreadSafetyMode.ExecutionAndPublication);
        private readonly object gate = new object();
        private readonly NativeCallback callback;
        private readonly Dictionary<uint, RuntimeDevice> instances = new Dictionary<uint, RuntimeDevice>();
        private ProgramSlot pending, executing;
        private bool started, disposed;
        private string startupMessage = "Ready · no configuration injected", callbackError = "";
        private ServiceSnapshot lastSnapshot = new ServiceSnapshot();

        private sealed class ProgramSlot
        {
            public readonly ScriptProgram Program;
            public ProgramSlot(ScriptProgram program) { Program = program; }
        }
        private sealed class RuntimeDevice
        {
            public ScriptInstance Script;
            public readonly ControlFrame Frame = new ControlFrame();
            public long LastMicroseconds;
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private unsafe delegate int NativeCallback(uint key, uint family, int* physical,
            byte* available, int* output, uint flags, IntPtr user);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct NativeSnapshot
        {
            public uint Size, Version, Running, DeviceCount;
            public ulong Received, Forwarded, Changed, TimerOutputs;
            public double ProcessingMicroseconds;
            public ulong Faults;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 192)] public string Status;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 512)] public string Error;
        }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct NativeDevice
        {
            public uint Size, Key, Family, Flags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Name;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 192)] public string Token;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 384)] public string Diagnostic;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)] public int[] Values;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 256)] public byte[] Available;
        }
        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
        private static extern uint GR_Probe(StringBuilder text, uint characters);
        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)]
        private static extern uint GR_Start(NativeCallback callback, IntPtr user);
        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)] private static extern void GR_Stop();
        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)] private static extern void GR_Wake();
        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)] private static extern uint GR_GetSnapshot(ref NativeSnapshot snapshot);
        [DllImport(NativeDll, CallingConvention = CallingConvention.Cdecl)] private static extern uint GR_GetDevice(uint index, ref NativeDevice device);

        public RemapperService()
        {
            callback = Transform;
            try { var text = new StringBuilder(1024); GR_Probe(text, (uint)text.Capacity); startupMessage = text.ToString(); }
            catch (DllNotFoundException) { startupMessage = "The native bridge is missing. Extract the complete x64 application archive."; }
            catch (BadImageFormatException) { startupMessage = "The native bridge architecture is incorrect. Use the complete Windows x64 release."; }
            catch (EntryPointNotFoundException) { startupMessage = "The native bridge version does not match this application."; }
            lastSnapshot = new ServiceSnapshot { Status = startupMessage };
        }

        public bool IsRunning { get { return GetSnapshot().Running; } }

        // Warm the managed VM's JIT before a real packet can be intercepted.
        // The fixed throwaway program and frames never reach native code,
        // Windows input, the user's script state or the driver trial clock.
        internal static void WarmRuntime()
        {
            if (!RuntimeWarmup.Value) throw new InvalidOperationException("The internal script runtime preparation failed.");
        }

        private static bool PrepareRuntime()
        {
            var program = ScriptCompiler.Compile("int counter; main { counter = counter + 1; set_val(LX,-get_ival(LX)); if(event_press(SOUTH)) combo_run(Tap); } " +
                "combo Tap { set_val(EAST,100); wait(1); set_val(EAST,0); wait(1); }");
            var instance = program.CreateInstance();
            var frame = new ControlFrame();
            frame.Available[0] = frame.Available[10] = frame.Available[11] = true;
            frame.Physical[0] = 25; frame.Physical[10] = 100;
            instance.Execute(frame, 0, 0);
            instance.Execute(frame, 1000, 1);
            frame.Physical[10] = 0;
            instance.Execute(frame, 2000, 1);
            return frame.Values[0] == -25 && frame.Values[11] == 0 && instance.NextWakeMilliseconds == 0;
        }

        public void Inject(ScriptProgram program)
        {
            if (program == null) throw new ArgumentNullException("program");
            lock (gate)
            {
                if (disposed) throw new ObjectDisposedException("RemapperService");
                WarmRuntime();
                Interlocked.Exchange(ref pending, new ProgramSlot(program));
                Interlocked.Exchange(ref callbackError, "");
                if (started && GetNativeSnapshot().Running != 0) { GR_Wake(); return; }
                if (started) { GR_Stop(); started = false; }
                instances.Clear(); executing = null;
                var text = new StringBuilder(1024); uint error = GR_Probe(text, (uint)text.Capacity);
                if (error != 0) throw new Win32Exception((int)error, text.ToString());
                error = GR_Start(callback, IntPtr.Zero);
                if (error != 0)
                {
                    NativeSnapshot failed = GetNativeSnapshot();
                    throw new Win32Exception((int)error, string.IsNullOrEmpty(failed.Error) ? "Capture could not start: " + new Win32Exception((int)error).Message : failed.Error);
                }
                started = true;
            }
        }

        public void Stop()
        {
            lock (gate)
            {
                if (started) { GR_Stop(); started = false; }
                instances.Clear(); executing = null;
            }
        }

        private static NativeSnapshot GetNativeSnapshot()
        {
            var snapshot = new NativeSnapshot { Size = (uint)Marshal.SizeOf(typeof(NativeSnapshot)) };
            uint error = GR_GetSnapshot(ref snapshot);
            if (error != 0) throw new Win32Exception((int)error, "Read remapper status");
            return snapshot;
        }

        public ServiceSnapshot GetSnapshot()
        {
            if (!Monitor.TryEnter(gate)) return Volatile.Read(ref lastSnapshot);
            try
            {
                try
                {
                    var native = GetNativeSnapshot();
                    var result = new ServiceSnapshot {
                        Running = native.Running != 0, Status = started || native.Received != 0 ? native.Status : startupMessage,
                        Error = native.Error ?? "", Received = native.Received, Forwarded = native.Forwarded,
                        Changed = native.Changed, TimerOutputs = native.TimerOutputs, Faults = native.Faults,
                        ProcessingMicroseconds = native.ProcessingMicroseconds
                    };
                    string scriptError = Volatile.Read(ref callbackError);
                    if (scriptError.Length != 0) result.Error = scriptError + (result.Error.Length != 0 ? "\n" + result.Error : "");
                    uint count = Math.Min(native.DeviceCount, 256u);
                    var devices = new List<ServiceDevice>((int)count);
                    for (uint i = 0; i < count; ++i)
                    {
                        var device = new NativeDevice { Size = (uint)Marshal.SizeOf(typeof(NativeDevice)) };
                        if (GR_GetDevice(i, ref device) != 0) break;
                        var visible = new ServiceDevice { Key = device.Key, Family = device.Family,
                            Name = device.Name ?? "Controller", Token = device.Token ?? "", Diagnostic = device.Diagnostic ?? "", Values = device.Values };
                        for (int slot = 0; slot < 256; ++slot) visible.Available[slot] = device.Available[slot] != 0;
                        devices.Add(visible);
                    }
                    result.Devices = devices.ToArray(); Volatile.Write(ref lastSnapshot, result); return result;
                }
                catch (DllNotFoundException) { return new ServiceSnapshot { Status = startupMessage }; }
                catch (BadImageFormatException) { return new ServiceSnapshot { Status = startupMessage }; }
                catch (EntryPointNotFoundException) { return new ServiceSnapshot { Status = startupMessage }; }
            }
            finally { Monitor.Exit(gate); }
        }

        private unsafe int Transform(uint key, uint family, int* physical, byte* available, int* output, uint flags, IntPtr user)
        {
            try
            {
                if (flags == 3) { instances.Remove(key); return 0; }
                ProgramSlot current = Volatile.Read(ref pending);
                if (current == null) return 0;
                if (!ReferenceEquals(current, executing)) { instances.Clear(); executing = current; }
                RuntimeDevice device;
                if (!instances.TryGetValue(key, out device))
                {
                    device = new RuntimeDevice { Script = current.Program.CreateInstance() }; instances.Add(key, device);
                }
                var frame = device.Frame; frame.DeviceKey = key; frame.Family = family;
                for (int i = 0; i < 256; ++i) { frame.Physical[i] = physical[i]; frame.Available[i] = available[i] != 0; }
                long now = (long)(Stopwatch.GetTimestamp() * (1000000.0 / Stopwatch.Frequency));
                int elapsed = device.LastMicroseconds == 0 ? 0 : (int)Math.Min(int.MaxValue, Math.Max(0, (now - device.LastMicroseconds) / 1000));
                device.Script.Execute(frame, now, elapsed); device.LastMicroseconds = now;
                for (int i = 0; i < 256; ++i) output[i] = frame.Values[i];
                return device.Script.NextWakeMilliseconds;
            }
            catch (Exception error)
            {
                Interlocked.Exchange(ref callbackError, error is ScriptException ? error.ToString() : error.Message);
                for (int i = 0; i < 256; ++i) output[i] = physical[i];
                return -1;
            }
        }

        public void Dispose()
        {
            lock (gate) { if (disposed) return; Stop(); disposed = true; }
            GC.KeepAlive(callback);
        }
    }

    internal static class DriverLiveTest
    {
        // This entry point is never run by normal builds or UI startup. It is
        // restricted to an explicitly opted-in Microsoft Hyper-V test guest.
        public static int Run(string outputPath, int seconds)
        {
            bool passed = false;
            string failure = "";
            ServiceSnapshot active = null, swapped = null, faulted = null, stopped = null;
            bool invalidPreserved = false, reinjected = false, initialCaptureActive = false;
            double cpuSeconds = 0; long privateBytes = 0; int collections = 0;
            var watch = Stopwatch.StartNew();
            try
            {
                if (Environment.GetEnvironmentVariable("GAMEPAD_REMAPPER_LAB") != "1")
                    throw new InvalidOperationException("Live capture tests require GAMEPAD_REMAPPER_LAB=1 in the dedicated test VM.");
                string manufacturer = Convert.ToString(Microsoft.Win32.Registry.GetValue(@"HKEY_LOCAL_MACHINE\HARDWARE\DESCRIPTION\System\BIOS", "SystemManufacturer", ""));
                string model = Convert.ToString(Microsoft.Win32.Registry.GetValue(@"HKEY_LOCAL_MACHINE\HARDWARE\DESCRIPTION\System\BIOS", "SystemProductName", ""));
                if (manufacturer.IndexOf("Microsoft", StringComparison.OrdinalIgnoreCase) < 0 || model.Replace(" ", "").IndexOf("VirtualMachine", StringComparison.OrdinalIgnoreCase) < 0)
                    throw new InvalidOperationException("The live test runner is limited to the Microsoft Hyper-V laboratory guest.");
                seconds = Math.Max(4, Math.Min(60, seconds));
                var script = ScriptCompiler.Compile(
                    "main { if(event_press(SOUTH)) combo_run(Tap); if(event_press(BUTTON_1)) combo_run(HidTap); " +
                    "set_val(LX,-get_ival(LX)); set_val(AXIS_1,-get_ival(AXIS_1)); }\n" +
                    "combo Tap { set_val(EAST,100); wait(450); set_val(EAST,0); wait(35); }\n" +
                    "combo HidTap { set_val(BUTTON_2,100); wait(450); set_val(BUTTON_2,0); wait(35); }");
                using (var service = new RemapperService())
                using (var process = Process.GetCurrentProcess())
                {
                    TimeSpan cpuBefore = process.TotalProcessorTime; int gcBefore = GC.CollectionCount(0);
                    service.Inject(script); initialCaptureActive = service.IsRunning;
                    var stage = Stopwatch.StartNew();
                    while (stage.Elapsed.TotalSeconds < seconds) { Thread.Sleep(50); active = service.GetSnapshot(); }
                    if (active == null || !active.Running || active.Received == 0 || active.Changed == 0 || active.TimerOutputs == 0 || active.Faults != 0)
                        throw new InvalidOperationException("The live fixture did not establish physical remapping and scheduled output.");
                    try { ScriptCompiler.Compile("main {"); }
                    catch (ScriptException) { invalidPreserved = service.IsRunning; }
                    service.Inject(ScriptCompiler.Compile("main { set_val(LX,-get_ival(LX)); set_val(AXIS_1,-get_ival(AXIS_1)); }"));
                    Thread.Sleep(1200); swapped = service.GetSnapshot();
                    if (!swapped.Running || swapped.Faults != 0) throw new InvalidOperationException("Replacing the compiled configuration stopped capture.");
                    service.Inject(ScriptCompiler.Compile("int divisor; main { set_val(LX,100/divisor); }"));
                    stage.Restart();
                    do { Thread.Sleep(40); faulted = service.GetSnapshot(); } while (faulted.Running && stage.ElapsedMilliseconds < 3000);
                    if (faulted.Running || faulted.Faults == 0 || faulted.Error.Length == 0)
                        throw new InvalidOperationException("A deliberate runtime error did not stop capture and report its cause.");
                    service.Inject(ScriptCompiler.Compile("main { }"));Thread.Sleep(900);reinjected = service.IsRunning;
                    service.Stop();stopped = service.GetSnapshot();
                    cpuSeconds = (process.TotalProcessorTime - cpuBefore).TotalSeconds;
                    process.Refresh();privateBytes = process.PrivateMemorySize64;collections = GC.CollectionCount(0) - gcBefore;
                    passed = initialCaptureActive && invalidPreserved && reinjected && !stopped.Running && stopped.Faults == 0;
                }
            }
            catch (Exception error) { failure = error.ToString(); }
            string path = Path.GetFullPath(outputPath);Directory.CreateDirectory(Path.GetDirectoryName(path));
            var report = new { Passed = passed, Error = failure, InitialCaptureActive = initialCaptureActive,
                InvalidSourcePreservedCapture = invalidPreserved, ReinjectAfterFault = reinjected,
                Active = active, Swapped = swapped, Faulted = faulted, Stopped = stopped,
                ElapsedSeconds = watch.Elapsed.TotalSeconds, ProcessCpuSeconds = cpuSeconds,
                PrivateBytes = privateBytes, Gen0Collections = collections,
                Scope = "Managed IGC VM, native worker and signed Interceptor Gamepad Controller pipeline against virtual HID/GIP fixtures. Worker processing excludes USB polling and hardware latency." };
            File.WriteAllText(path, new JavaScriptSerializer { MaxJsonLength = 4 * 1024 * 1024 }.Serialize(report), new UTF8Encoding(false));
            return passed ? 0 : 1;
        }
    }
}
