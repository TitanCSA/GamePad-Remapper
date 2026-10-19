using System;
using System.IO;
using System.Text;

namespace GamePadRemapper
{
    internal static class ScriptStorage
    {
        private static readonly UTF8Encoding Utf8 = new UTF8Encoding(false, true);
        internal static string DefaultDirectory
        {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "GamePad Remapper", "Scripts"); }
        }

        internal static string RecoveryDirectory
        {
            get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "GamePadRemapper", "Recovery"); }
        }

        internal static string Read(string path)
        {
            using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                if (stream.Length > 1024 * 1024) throw new IOException("Scripts must be no larger than 1 MiB.");
                using (var reader = new StreamReader(stream, Utf8, true))
                {
                    string source;
                    try { source = reader.ReadToEnd(); }
                    catch (DecoderFallbackException exception) { throw new IOException("The script contains invalid text. Save it as UTF-8 and open it again.", exception); }
                    if (source.Length > 524288) throw new IOException("Scripts must contain no more than 512 KiB of text.");
                    return source;
                }
            }
        }

        internal static void Write(string path, string source)
        {
            WriteCore(path, source, true);
        }

        internal static void WriteDraft(string path, string source)
        {
            WriteCore(path, source, false);
        }

        private static void WriteCore(string path, string source, bool backup)
        {
            if (source == null || source.Length > 524288) throw new IOException("Scripts must be no larger than 512 KiB of text or 1 MiB encoded.");
            int bytes;
            try { bytes = Utf8.GetByteCount(source); }
            catch (EncoderFallbackException exception) { throw new IOException("The script contains invalid Unicode text and was not saved.", exception); }
            if (bytes > 1024 * 1024) throw new IOException("Scripts must be no larger than 512 KiB of text or 1 MiB encoded.");
            path = Path.GetFullPath(path);
            Directory.CreateDirectory(Path.GetDirectoryName(path));
            string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                using (var stream = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough))
                using (var writer = new StreamWriter(stream, Utf8))
                {
                    writer.Write(source);
                    writer.Flush();
                    stream.Flush(true);
                }
                if (File.Exists(path)) File.Replace(temporary, path, backup ? path + ".bak" : null, true);
                else File.Move(temporary, path);
            }
            finally { if (File.Exists(temporary)) File.Delete(temporary); }
        }
    }
}
