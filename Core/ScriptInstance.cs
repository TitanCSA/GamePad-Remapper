using System;
using System.Threading;

namespace GamePadRemapper.Core
{
    internal static class Builtins
    {
        internal static readonly string[] Names = { "get_val", "get_ival", "get_lval", "set_val", "event_press", "event_release", "get_ptime", "get_rtime", "block", "swap", "abs", "min", "max", "clamp", "deadzone", "sensitivity", "rand", "combo_run", "combo_stop", "combo_restart", "combo_running", "combo_stop_all", "remap", "invert", "is_available", "get_controller", "get_device", "get_runtime", "vm_tctrl", "random", "get_brtime", "get_pvar", "set_pvar", "get_bit", "set_bit", "pow", "isqrt", "hypot" };
        internal static int Find(string name) { for (int i = 0; i < Names.Length; i++) if (Names[i] == name) return i; return -1; }
        internal static bool Accepts(int id, int count)
        {
            switch (id)
            {
                case 7: case 21: case 25: case 26: case 27: return count == 0;
                case 3: case 8: case 9: case 11: case 12: case 16: case 22: case 29: case 32: case 33: case 35: case 37: return count == 2;
                case 13: case 34: return count == 3;
                case 31: return count == 4;
                case 14: return count == 2 || count == 3;
                case 15: return count == 2 || count == 3;
                default: return count == 1;
            }
        }
        internal static bool UsesTime(int id) { return id == 6 || id == 7 || id == 27 || id == 30; }
    }
    internal struct ReturnPoint { internal int Block, Pc, LocalBase, LocalTop, Stack; }
    internal sealed class VmState
    {
        internal readonly int[] Stack = new int[1024], Locals;
        internal readonly ReturnPoint[] Calls = new ReturnPoint[32];
        internal int Block, Pc, Sp, LocalBase, LocalTop, Depth;
        internal VmState(int localCapacity) { Locals = new int[localCapacity]; }
        internal void Start(int block, CodeBlock definition)
        {
            Block = block; Pc = Sp = LocalBase = Depth = 0; LocalTop = definition.Locals;
            if (LocalTop > 0) Array.Clear(Locals, 0, LocalTop);
        }
    }
    internal sealed class ComboState
    {
        internal readonly VmState Vm;
        internal readonly int[] Held = new int[256];
        internal readonly bool[] HasHeld = new bool[256];
        internal bool Running; internal long Deadline;
        internal ComboState(int localCapacity) { Vm = new VmState(localCapacity); }
        internal void Clear() { Running = false; Deadline = 0; Array.Clear(HasHeld, 0, 256); }
    }
    public sealed class ScriptInstance
    {
        private readonly ScriptProgram program;
        private readonly int[] globals, previous = new int[256];
        private readonly long[] changedAt = new long[256], blockedUntil = new long[256];
        private readonly long[] lastReleasedAt = new long[256];
        private readonly int[] savedReleaseTime = new int[256], pvars = new int[64];
        private readonly bool[] hasPvar = new bool[64];
        private readonly bool[] pressed = new bool[256], released = new bool[256];
        private readonly bool[] previousAvailable = new bool[256];
        private readonly VmState main;
        private readonly ComboState[] combos;
        private ControlFrame frame; private ComboState currentCombo; private Instruction currentInstruction;
        private long now, startTime, previousTime; private int elapsed, remaining, busy, tickMilliseconds = 1;
        private uint randomState = 0xA341316Cu; private bool initialized, timeAware, forcedTicks;
        public int NextWakeMilliseconds { get; private set; }
        private int instructionBudget = 10000;
        public int InstructionBudget
        {
            get { return instructionBudget; }
            set { if (value < 100 || value > 100000) throw new ArgumentOutOfRangeException("value"); instructionBudget = value; }
        }
        internal ScriptInstance(ScriptProgram program)
        {
            this.program = program; globals = new int[program.GlobalCount]; combos = new ComboState[program.ComboBlocks.Length];
            int maxLocals = 0; foreach (var block in program.Blocks) maxLocals = Math.Max(maxLocals, block.Locals);
            int localCapacity = Math.Min(8192, maxLocals * 33); main = new VmState(localCapacity);
            for (int i = 0; i < combos.Length; i++) combos[i] = new ComboState(localCapacity);
        }
        public void Reset()
        {
            if (Interlocked.CompareExchange(ref busy, 1, 0) != 0) throw new InvalidOperationException("A script instance cannot be reset while executing.");
            try
            {
                Array.Clear(globals, 0, globals.Length); Array.Clear(previous, 0, 256); Array.Clear(changedAt, 0, 256); Array.Clear(blockedUntil, 0, 256);
                Array.Clear(previousAvailable, 0, 256);
                Array.Clear(lastReleasedAt, 0, 256); Array.Clear(savedReleaseTime, 0, 256); Array.Clear(pvars, 0, 64); Array.Clear(hasPvar, 0, 64);
                foreach (var combo in combos) combo.Clear(); initialized = forcedTicks = timeAware = false; NextWakeMilliseconds = 0; previousTime = startTime = 0; tickMilliseconds = 1; randomState = 0xA341316Cu;
            }
            finally { Volatile.Write(ref busy, 0); }
        }
        public void Execute(ControlFrame input, long nowMicroseconds, int elapsedMilliseconds)
        {
            if (input == null) throw new ArgumentNullException("input");
            if (nowMicroseconds < 0) throw new ArgumentOutOfRangeException("nowMicroseconds");
            if (Interlocked.CompareExchange(ref busy, 1, 0) != 0) throw new InvalidOperationException("A script instance belongs to a single device worker.");
            frame = input; frame.CopyPhysicalToOutput();
            try
            {
                if (initialized && nowMicroseconds < previousTime) throw new ScriptException("Monotonic time moved backwards.");
                now = nowMicroseconds; elapsed = Math.Max(0, elapsedMilliseconds); remaining = instructionBudget; NextWakeMilliseconds = 0; timeAware = false;
                if (!initialized) { startTime = now; for (int i = 0; i < 256; i++) changedAt[i] = lastReleasedAt[i] = now; }
                for (int i = 0; i < 256; i++)
                {
                    // Hats use -1 for neutral; all other controls use zero.
                    bool was = Active(i, previous[i], initialized && previousAvailable[i]), active = Active(i, frame.Physical[i], frame.Available[i]);
                    pressed[i] = !was && active; released[i] = was && !active;
                    if (pressed[i]) savedReleaseTime[i] = (int)Math.Min(int.MaxValue, (now - lastReleasedAt[i]) / 1000);
                    if (released[i]) lastReleasedAt[i] = now;
                    if (was != active) changedAt[i] = now;
                }
                if (!initialized)
                {
                    main.Start(program.Init, program.Blocks[program.Init]); currentCombo = null; Run(main); initialized = true;
                }
                main.Start(program.Main, program.Blocks[program.Main]); currentCombo = null; Run(main);
                for (int i = 0; i < 256; i++) if (blockedUntil[i] > now) frame.Values[i] = Neutral(i);
                for (int c = 0; c < combos.Length; c++)
                {
                    var combo = combos[c]; if (!combo.Running) continue;
                    if (combo.Deadline <= now)
                    {
                        Array.Clear(combo.HasHeld, 0, 256); currentCombo = combo;
                        if (!Run(combo.Vm)) combo.Clear(); currentCombo = null;
                    }
                    if (combo.Running)
                    {
                        for (int i = 0; i < 256; i++) if (combo.HasHeld[i]) frame.Values[i] = combo.Held[i];
                        Schedule(combo.Deadline);
                    }
                }
                for (int c = 0; c < combos.Length; c++) if (combos[c].Running) Schedule(combos[c].Deadline);
                for (int i = 0; i < 256; i++) if (blockedUntil[i] > now) Schedule(blockedUntil[i]);
                if ((timeAware || forcedTicks) && (NextWakeMilliseconds == 0 || NextWakeMilliseconds > tickMilliseconds)) NextWakeMilliseconds = tickMilliseconds;
            }
            catch
            {
                frame.CopyPhysicalToOutput(); foreach (var combo in combos) combo.Clear(); NextWakeMilliseconds = 0; throw;
            }
            finally
            {
                Array.Copy(frame.Physical, previous, 256); Array.Copy(frame.Available, previousAvailable, 256); previousTime = nowMicroseconds; currentCombo = null; frame = null; Volatile.Write(ref busy, 0);
            }
        }
        private static int Neutral(int index) { return index >= 192 && index < 208 ? -1 : 0; }
        private static bool Active(int index, int value, bool valid) { return valid && (index >= 192 && index < 208 ? value >= 0 : value != 0); }
        private void Schedule(long deadline)
        {
            long delta = deadline > now ? deadline - now : 0; int milliseconds = (int)Math.Min(int.MaxValue, Math.Max(1, (delta + 999) / 1000));
            if (NextWakeMilliseconds == 0 || milliseconds < NextWakeMilliseconds) NextWakeMilliseconds = milliseconds;
        }
        private void Error(string message) { throw new ScriptException(message, currentInstruction.Line, currentInstruction.Column); }
        private void Push(VmState state, int value) { if (state.Sp >= state.Stack.Length) Error("Expression stack exceeds 1,024 values."); state.Stack[state.Sp++] = value; }
        private int Pop(VmState state) { if (state.Sp <= 0) Error("Invalid compiled expression stack."); return state.Stack[--state.Sp]; }
        private bool Run(VmState state)
        {
            while (true)
            {
                if (--remaining < 0) Error("Script instruction budget exceeded. Check loops and recursion; output was restored to physical input.");
                var block = program.Blocks[state.Block];
                if ((uint)state.Pc >= (uint)block.Code.Length) Error("Invalid compiled instruction address.");
                currentInstruction = block.Code[state.Pc++]; var instruction = currentInstruction;
                switch (instruction.Op)
                {
                    case Op.Push: Push(state, instruction.A); break;
                    case Op.Load: Push(state, globals[instruction.A]); break;
                    case Op.Store: globals[instruction.A] = state.Stack[state.Sp - 1]; break;
                    case Op.LoadLocal: Push(state, state.Locals[state.LocalBase + instruction.A]); break;
                    case Op.StoreLocal: state.Locals[state.LocalBase + instruction.A] = state.Stack[state.Sp - 1]; break;
                    case Op.LoadArray:
                    {
                        int index = Pop(state), length = instruction.B & 65535; if ((uint)index >= (uint)length) Error("Array index " + index + " is outside 0.." + (length - 1) + ".");
                        Push(state, (instruction.B & 65536) != 0 ? state.Locals[state.LocalBase + instruction.A + index] : globals[instruction.A + index]); break;
                    }
                    case Op.StoreArray:
                    {
                        int value = Pop(state), index = Pop(state), length = instruction.B & 65535; if ((uint)index >= (uint)length) Error("Array index " + index + " is outside 0.." + (length - 1) + ".");
                        if ((instruction.B & 65536) != 0) state.Locals[state.LocalBase + instruction.A + index] = value; else globals[instruction.A + index] = value; Push(state, value); break;
                    }
                    case Op.Dup: Push(state, state.Stack[state.Sp - 1]); break;
                    case Op.Dup2: int second = state.Stack[state.Sp - 2], first = state.Stack[state.Sp - 1]; Push(state, second); Push(state, first); break;
                    case Op.Pop: Pop(state); break;
                    case Op.Neg: state.Stack[state.Sp - 1] = unchecked(-state.Stack[state.Sp - 1]); break;
                    case Op.Not: state.Stack[state.Sp - 1] = state.Stack[state.Sp - 1] == 0 ? 1 : 0; break;
                    case Op.Bool: state.Stack[state.Sp - 1] = state.Stack[state.Sp - 1] == 0 ? 0 : 1; break;
                    case Op.BitNot: state.Stack[state.Sp - 1] = ~state.Stack[state.Sp - 1]; break;
                    case Op.Jump: state.Pc = instruction.A; break;
                    case Op.JumpZero: if (Pop(state) == 0) state.Pc = instruction.A; break;
                    case Op.Builtin:
                    {
                        int start = state.Sp - instruction.B; if (start < 0) Error("Invalid compiled argument stack.");
                        int result = Invoke(instruction.A, state.Stack, start, instruction.B); state.Sp = start; Push(state, result); break;
                    }
                    case Op.Call:
                    {
                        if (state.Depth >= state.Calls.Length) Error("Function call depth exceeds 32.");
                        CodeBlock called = program.Blocks[instruction.A]; int start = state.Sp - instruction.B;
                        if (state.LocalTop + called.Locals > state.Locals.Length) Error("Function local storage exceeds the safe limit.");
                        state.Calls[state.Depth++] = new ReturnPoint { Block = state.Block, Pc = state.Pc, LocalBase = state.LocalBase, LocalTop = state.LocalTop, Stack = start };
                        state.LocalBase = state.LocalTop; state.LocalTop += called.Locals; if (called.Locals > 0) Array.Clear(state.Locals, state.LocalBase, called.Locals);
                        Array.Copy(state.Stack, start, state.Locals, state.LocalBase, instruction.B); state.Sp = start; state.Block = instruction.A; state.Pc = 0; break;
                    }
                    case Op.Return:
                    {
                        int result = Pop(state); if (state.Depth == 0) return false;
                        ReturnPoint back = state.Calls[--state.Depth]; state.Block = back.Block; state.Pc = back.Pc; state.LocalBase = back.LocalBase; state.LocalTop = back.LocalTop; state.Sp = back.Stack; Push(state, result); break;
                    }
                    case Op.Wait:
                    {
                        int duration = Pop(state); if (duration < 0 || duration > 86400000) Error("wait duration must be between 0 and 86,400,000 ms.");
                        if (currentCombo == null || state.Depth != 0) Error("wait is only valid directly inside a combo.");
                        currentCombo.Deadline = SafeDeadline(Math.Max(1, duration)); return true;
                    }
                    default:
                    {
                        int b = Pop(state), a = Pop(state);
                        Push(state, Compiler.Arithmetic(instruction.Op, a, b, new Token("", instruction.Line, instruction.Column))); break;
                    }
                }
            }
        }
        private long SafeDeadline(int milliseconds)
        { long duration = (long)milliseconds * 1000; return now > long.MaxValue - duration ? long.MaxValue : now + duration; }
        private int Control(int value) { if ((uint)value >= 256) Error("Control identifier must be between 0 and 255."); return value; }
        private int Combo(int value) { if ((uint)value >= (uint)combos.Length) Error("Unknown combo identifier."); return value; }
        private void Set(int id, int value)
        {
            id = Control(id); int minimum = id >= 192 && id < 208 ? -1 : id < 4 || id >= 96 && id < 160 ? -100 : 0;
            int maximum = id >= 192 && id < 208 ? 7 : id >= 208 ? int.MaxValue : 100;
            // Preserve arbitrary signed HID value fields; native conversion validates logical bounds.
            if (id >= 208) minimum = int.MinValue;
            value = Math.Max(minimum, Math.Min(maximum, value)); frame.Values[id] = value;
            if (currentCombo != null) { currentCombo.Held[id] = value; currentCombo.HasHeld[id] = true; }
        }
        private static int SafeAbs(int value) { return value == int.MinValue ? int.MaxValue : Math.Abs(value); }
        private int Invoke(int id, int[] arguments, int offset, int count)
        {
            if (currentCombo == null && Builtins.UsesTime(id)) timeAware = true;
            int a = count > 0 ? arguments[offset] : 0, b = count > 1 ? arguments[offset + 1] : 0, c = count > 2 ? arguments[offset + 2] : 0;
            switch (id)
            {
                case 0: a = Control(a); return frame.Available[a] ? frame.Values[a] : Neutral(a);
                case 1: a = Control(a); return frame.Available[a] ? frame.Physical[a] : Neutral(a);
                case 2: a = Control(a); return initialized && previousAvailable[a] ? previous[a] : Neutral(a);
                case 3: Set(a, b); return frame.Values[a];
                case 4: return pressed[Control(a)] ? 1 : 0;
                case 5: return released[Control(a)] ? 1 : 0;
                case 6: a = Control(a); return frame.Available[a] ? (int)Math.Min(int.MaxValue, (now - changedAt[a]) / 1000) : 0;
                case 7: return elapsed;
                case 8:
                    a = Control(a); if (b < 0 || b > 86400000) Error("block duration must be between 0 and 86,400,000 ms.");
                    blockedUntil[a] = b == 0 ? now : SafeDeadline(b); if (b != 0) Set(a, Neutral(a)); return 0;
                case 9: a = Control(a); b = Control(b); int old = frame.Values[a]; Set(a, frame.Values[b]); Set(b, old); return 0;
                case 10: return SafeAbs(a);
                case 11: return Math.Min(a, b);
                case 12: return Math.Max(a, b);
                case 13: if (b > c) Error("clamp minimum must not exceed maximum."); return Math.Max(b, Math.Min(c, a));
                case 14:
                    a = Control(a); if (b < 0 || b >= 100) Error("deadzone threshold must be in 0..99.");
                    int original = frame.Values[a], magnitude = SafeAbs(original);
                    int adjusted = magnitude <= b ? 0 : (int)Math.Min(100, (long)(magnitude - b) * 100 / (100 - b));
                    Set(a, original < 0 ? -adjusted : adjusted); if (count == 3) { int second = Control(c); int v = frame.Values[second], m = SafeAbs(v); Set(second, m <= b ? 0 : (v < 0 ? -1 : 1) * (int)Math.Min(100, (long)(m - b) * 100 / (100 - b))); } return 0;
                case 15:
                    a = Control(a); int percentage = count == 3 ? c : b;
                    if (percentage < 0 || percentage > 1000) Error("sensitivity percentage must be in 0..1000.");
                    Set(a, (int)Math.Max(-100, Math.Min(100, (long)frame.Values[a] * percentage / 100))); return 0;
                case 16: case 29:
                    if (a > b) Error("rand minimum must not exceed maximum.");
                    randomState ^= randomState << 13; randomState ^= randomState >> 17; randomState ^= randomState << 5;
                    return (int)(a + (long)(randomState % ((ulong)((long)b - a) + 1)));
                case 17: case 19:
                    int comboId = Combo(a); var combo = combos[comboId];
                    if (id == 19 || !combo.Running) { combo.Clear(); combo.Running = true; combo.Vm.Start(program.ComboBlocks[comboId], program.Blocks[program.ComboBlocks[comboId]]); combo.Deadline = now; } return 0;
                case 18: combos[Combo(a)].Clear(); return 0;
                case 20: return combos[Combo(a)].Running ? 1 : 0;
                case 21: foreach (var running in combos) running.Clear(); return 0;
                case 22: a = Control(a); b = Control(b); if (a != b) { Set(b, frame.Physical[a]); Set(a, Neutral(a)); } return 0;
                case 23: a = Control(a); Set(a, unchecked(-frame.Values[a])); return 0;
                case 24: return frame.Available[Control(a)] ? 1 : 0;
                case 25: return unchecked((int)frame.Family);
                case 26: return unchecked((int)frame.DeviceKey);
                case 27: return (int)Math.Min(int.MaxValue, (now - startTime) / 1000);
                case 28: if (a < 1 || a > 20) Error("vm_tctrl interval must be in 1..20 ms."); tickMilliseconds = a; forcedTicks = true; return 0;
                case 30: a = Control(a); return !frame.Available[a] ? 0 : Active(a, frame.Physical[a], true) ? savedReleaseTime[a] : (int)Math.Min(int.MaxValue, (now - lastReleasedAt[a]) / 1000);
                case 31:
                    if ((uint)a >= 64) Error("Persistent variable slot must be in 0..63."); if (b > c) Error("get_pvar minimum must not exceed maximum.");
                    int fallback = arguments[offset + 3]; if (fallback < b || fallback > c) Error("get_pvar default must be within its bounds.");
                    return hasPvar[a] && pvars[a] >= b && pvars[a] <= c ? pvars[a] : fallback;
                case 32: if ((uint)a >= 64) Error("Persistent variable slot must be in 0..63."); pvars[a] = b; hasPvar[a] = true; return b;
                case 33: if ((uint)b >= 32) Error("Bit index must be in 0..31."); return (int)((uint)a >> b) & 1;
                case 34: if ((uint)b >= 32 || c < 0 || c > 1) Error("set_bit needs a bit index in 0..31 and value 0 or 1."); return c == 0 ? a & ~(1 << b) : a | 1 << b;
                case 35:
                    if (b < 0 || b > 31) Error("pow exponent must be in 0..31."); int power = 1; while (b-- > 0) power = unchecked(power * a); return power;
                case 36: if (a < 0) Error("isqrt argument must be nonnegative."); return (int)Math.Sqrt(a);
                case 37: return (int)Math.Min(int.MaxValue, Math.Sqrt((double)a * a + (double)b * b));
                default: Error("Unknown compiled builtin."); return 0;
            }
        }
    }
}
