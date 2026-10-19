using System;
using System.Collections.Generic;

namespace GamePadRemapper.Core
{
    public sealed class ControlFrame
    {
        public readonly int[] Physical = new int[256];
        public readonly int[] Values = new int[256];
        public readonly bool[] Available = new bool[256];
        public uint DeviceKey, Family;
        public void CopyPhysicalToOutput() { Array.Copy(Physical, Values, 256); }
    }

    public static class Controls
    {
        public static readonly string[] Names = CreateNames();
        private static readonly Dictionary<string, int> Lookup = CreateLookup();
        private static string[] CreateNames()
        {
            var names = new string[256];
            var canonical = new[] { "LX", "LY", "RX", "RY", "LT", "RT", "UP", "DOWN", "LEFT", "RIGHT", "SOUTH", "EAST", "WEST", "NORTH", "LB", "RB", "VIEW", "MENU", "LS", "RS", "GUIDE", "TOUCH" };
            Array.Copy(canonical, names, canonical.Length);
            for (int i = 22; i < 32; i++) names[i] = "RESERVED_" + i;
            for (int i = 0; i < 64; i++) { names[32 + i] = "BUTTON_" + (i + 1); names[96 + i] = "AXIS_" + (i + 1); }
            for (int i = 0; i < 32; i++) names[160 + i] = "TRIGGER_" + (i + 1);
            for (int i = 0; i < 16; i++) names[192 + i] = "HAT_" + (i + 1);
            for (int i = 0; i < 48; i++) names[208 + i] = "VALUE_" + (i + 1);
            return names;
        }
        private static Dictionary<string, int> CreateLookup()
        {
            var map = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            for (int i = 0; i < 256; i++) map[Names[i]] = i;
            string[] prefixes = { "", "XB1_", "XB360_", "PS4_", "PS5_" };
            foreach (string prefix in prefixes)
            {
                for (int i = 0; i < 22; i++) map[prefix + Names[i]] = i;
                map[prefix + "A"] = 10; map[prefix + "B"] = 11; map[prefix + "X"] = 12; map[prefix + "Y"] = 13;
                map[prefix + "CROSS"] = 10; map[prefix + "CIRCLE"] = 11; map[prefix + "SQUARE"] = 12; map[prefix + "TRIANGLE"] = 13;
                map[prefix + "L1"] = 14; map[prefix + "R1"] = 15; map[prefix + "L2"] = 4; map[prefix + "R2"] = 5;
                map[prefix + "L3"] = 18; map[prefix + "R3"] = 19; map[prefix + "SHARE"] = 16;
                map[prefix + "CREATE"] = 16; map[prefix + "OPTIONS"] = 17; map[prefix + "PS"] = 20;
                map[prefix + "START"] = 17; map[prefix + "BACK"] = 16; map[prefix + "TOUCHPAD"] = 21;
            }
            return map;
        }
        public static int Parse(string name) { int value; return name != null && Lookup.TryGetValue(name, out value) ? value : -1; }
    }

    public class ScriptException : Exception
    {
        public int Line { get; private set; }
        public int Column { get; private set; }
        public ScriptException(string message, int line = 1, int column = 1) : base(message) { Line = line; Column = column; }
        public override string ToString() { return "Line " + Line + ", column " + Column + ": " + Message; }
    }
}
