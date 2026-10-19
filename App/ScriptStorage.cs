using System;
using System.IO;
using System.Text;

namespace GamePadRemapper
{
    internal static class ScriptStorage
    {
        internal static string DefaultDirectory
        {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "GamePad Remapper", "Scripts"); }
        }

        internal static string Read(string path)
        {
            using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                if (stream.Length > 1024 * 1024) throw new IOException("Scripts must be no larger than 1 MiB.");
                using (var reader = new StreamReader(stream, Encoding.UTF8, true))
                {
                    string source = reader.ReadToEnd();
                    if (source.Length > 524288) throw new IOException("Scripts must contain no more than 512 KiB of text.");
                    return source;
                }
            }
        }

        internal static void Write(string path, string source)
        {
            if (source == null || Encoding.UTF8.GetByteCount(source) > 1024 * 1024) throw new IOException("Scripts must be no larger than 1 MiB.");
            path = Path.GetFullPath(path);
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough))
                using (var writer = new StreamWriter(stream, new UTF8Encoding(false)))
                {
                    writer.Write(source);
                    writer.Flush();
                    stream.Flush(true);
                }
                if (File.Exists(path)) File.Replace(temporary, path, path + ".bak", true);
                else File.Move(temporary, path);
            }
            finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }
    }
}
