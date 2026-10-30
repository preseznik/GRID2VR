using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace GRID2VR
{
    // Shared by the installer/launcher and PowerShell preparation tools.
    public static class GameExecutable
    {
        public const string AvxBaseline = "519cd994d2ff92a2a7a2a784856854183f53e359b83f8b9563cc09a0ae91e502";
        public const string RegularBaseline = "f92c2e1134b5d6d61404131224b41c182089bf563d28731eef0faa7d8096ab4c";
        public const string AvxStandalone = "825f30a49a668870c7bfab0309c3afb01a396fc1cec0be8df9764e32bf2fb53f";
        public const string RegularStandalone = "d8ec0c631b453d6e5e9adebae4cfc9b612823fec316344c28544bc65245ba35d";

        public static bool IsStandalone(string identity)
        {
            return identity == AvxStandalone || identity == RegularStandalone;
        }

        public static string ValidateInstallation(string game)
        {
            string regular = Validate(Path.Combine(game, "grid2.exe"), false);
            string avx = Validate(Path.Combine(game, "grid2_avx.exe"), true);
            if (IsStandalone(regular) != IsStandalone(avx))
                throw new IOException("Mixed Steam and standalone GRID 2 executables are unsupported.");
            return IsStandalone(avx) ? "Direct" : "Steam";
        }

        public static string Validate(string path, bool avx)
        {
            path = Path.GetFullPath(path);
            for (string part = path; !String.IsNullOrEmpty(part); part = Path.GetDirectoryName(part) ?? String.Empty)
                if ((File.GetAttributes(part) & FileAttributes.ReparsePoint) != 0)
                    throw new IOException("Linked game executable path is unsupported: " + path);
            // Deny writes/replacement until the fingerprint and signature refer to the same file.
            using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                int certificate = avx ? 0xef8800 : 0xee7800;
                int security = avx ? 0x1f0 : 0x1e0;
                if (file.Length < certificate || file.Length > certificate + 65536)
                    throw Unsupported(path);
                var bytes = new byte[(int)file.Length];
                int read = 0;
                while (read < bytes.Length)
                {
                    int count = file.Read(bytes, read, bytes.Length - read);
                    if (count == 0) throw Unsupported(path);
                    read += count;
                }
                string actual = Hash(bytes, bytes.Length);
                // These two unsigned files are admitted only by complete, variant-specific
                // identity. Never apply the signed Steam normalization to standalone files.
                if (file.Length == certificate && actual == (avx ? AvxStandalone : RegularStandalone)) return actual;
                if (file.Length < certificate + 8) throw Unsupported(path);
                if (actual == (avx ? AvxBaseline : RegularBaseline)) return actual;
                uint certificateBytes = BitConverter.ToUInt32(bytes, security + 4);
                if (BitConverter.ToUInt32(bytes, security) != certificate ||
                    certificateBytes != bytes.Length - certificate) throw Unsupported(path);

                // Evidence-derived Steam variation, always authenticated below. The rest of
                // the image remains exact, including code, PE layout, import tables and data.
                // Never use this normalization without checking the original file's signature.
                Array.Clear(bytes, 0x4c, 0x84);
                Array.Clear(bytes, avx ? 0x1b0 : 0x1a0, 4);
                Array.Clear(bytes, security + 4, 4);
                int start = avx ? 0xbb9368 : 0xba716c;
                Array.Clear(bytes, start, (avx ? 0xd1d720 : 0xd0a7c4) - start);
                Array.Clear(bytes, avx ? 0xdf005c : 0xdde05c, 16); // STEAM_MINSTANCE
                string expected = avx ? "12086dce2b2ccf0142fa3ad4662f832147899b1eb03ce2203c78e516a6082202"
                                      : "5f1601f57bc0628cbb6196af07015eaa6e58f3385618d9e8ca44ca50558af5b3";
                if (Hash(bytes, certificate) != expected || !ValveSigned(path, file.SafeFileHandle.DangerousGetHandle()))
                    throw Unsupported(path);
                return actual; // Preservation identity, not the normalized build fingerprint.
            }
        }

        private static IOException Unsupported(string path)
        {
            return new IOException("Unsupported or changed GRID 2 executable (requires the Valve-signed 1.0.85.8679 build or the exact supported standalone files): " + Path.GetFileName(path));
        }
        private static string Hash(byte[] bytes, int count)
        {
            using (var sha = SHA256.Create())
                return BitConverter.ToString(sha.ComputeHash(bytes, 0, count)).Replace("-", "").ToLowerInvariant();
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct TrustFile { public uint Size; [MarshalAs(UnmanagedType.LPWStr)] public string Path; public IntPtr Handle, Subject; }
        [StructLayout(LayoutKind.Sequential)]
        private struct TrustData
        {
            public uint Size; public IntPtr Policy, Sip; public uint Ui, Revocation, Choice;
            public IntPtr File; public uint Action; public IntPtr State, Url; public uint Flags, UiContext;
            public IntPtr SignatureSettings;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct Signer { public uint Size; public System.Runtime.InteropServices.ComTypes.FILETIME Time; public uint Count; public IntPtr Certificates; }
        [StructLayout(LayoutKind.Sequential)]
        private struct ProviderCertificate { public uint Size; public IntPtr Certificate; }
        [DllImport("wintrust.dll", ExactSpelling = true)]
        private static extern int WinVerifyTrust(IntPtr window, ref Guid action, ref TrustData data);
        [DllImport("wintrust.dll", ExactSpelling = true)]
        private static extern IntPtr WTHelperProvDataFromStateData(IntPtr state);
        [DllImport("wintrust.dll", ExactSpelling = true)]
        private static extern IntPtr WTHelperGetProvSignerFromChain(IntPtr data, uint index, [MarshalAs(UnmanagedType.Bool)] bool counterSigner, uint counterIndex);
        [DllImport("crypt32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern uint CertGetNameStringW(IntPtr certificate, uint type, uint flags, IntPtr parameter, StringBuilder name, uint size);

        private static bool ValveSigned(string path, IntPtr handle)
        {
            var info = new TrustFile { Size = (uint)Marshal.SizeOf(typeof(TrustFile)), Path = path, Handle = handle };
            IntPtr memory = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(TrustFile)));
            Marshal.StructureToPtr(info, memory, false);
            var data = new TrustData { Size = (uint)Marshal.SizeOf(typeof(TrustData)), Ui = 2, Choice = 1,
                File = memory, Action = 1, Flags = 0x1000 | 0x10 }; // Offline, no UI, no revocation network requests.
            var action = new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE");
            try
            {
                if (WinVerifyTrust(new IntPtr(-1), ref action, ref data) != 0) return false;
                IntPtr provider = WTHelperProvDataFromStateData(data.State);
                if (provider == IntPtr.Zero) return false;
                IntPtr pointer = WTHelperGetProvSignerFromChain(provider, 0, false, 0);
                if (pointer == IntPtr.Zero) return false;
                var signer = Marshal.PtrToStructure<Signer>(pointer);
                if (signer.Count == 0 || signer.Certificates == IntPtr.Zero) return false;
                var certificate = Marshal.PtrToStructure<ProviderCertificate>(signer.Certificates);
                var name = new StringBuilder(256);
                return certificate.Certificate != IntPtr.Zero &&
                    CertGetNameStringW(certificate.Certificate, 4, 0, IntPtr.Zero, name, 256) == 12 && name.ToString() == "Valve Corp.";
            }
            finally
            {
                data.Action = 2;
                WinVerifyTrust(new IntPtr(-1), ref action, ref data);
                Marshal.DestroyStructure(memory, typeof(TrustFile));
                Marshal.FreeHGlobal(memory);
            }
        }
    }
    // Read-only: the files a running game process loaded under one module name.
    // GRID 2 is 32-bit and the tools may be 64-bit, so all module views are listed.
    // The renderer proxy is effective only if this names the game folder's copy.
    public static class GameProcessModules
    {
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr OpenProcess(uint access, bool inherit, int id);
        [DllImport("kernel32.dll")]
        private static extern bool CloseHandle(IntPtr handle);
        [DllImport("psapi.dll", SetLastError = true)]
        private static extern bool EnumProcessModulesEx(IntPtr process, [Out] IntPtr[] modules, uint size, out uint needed, uint filter);
        [DllImport("psapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetModuleFileNameExW(IntPtr process, IntPtr module, StringBuilder name, uint size);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetProcessMitigationPolicy(IntPtr process, int policy, out uint value, UIntPtr size);

        // "Prefer system32 images" (PROCESS_MITIGATION_IMAGE_LOAD_POLICY bit 2): the
        // loader then takes Windows' d3d11.dll over the game folder's. A parent such
        // as Steam can apply it when it creates the game process.
        public static bool PrefersSystem32Images(int processId)
        {
            const uint QueryInformation = 0x0400;
            const int ImageLoadPolicy = 10;
            IntPtr process = OpenProcess(QueryInformation, false, processId);
            if (process == IntPtr.Zero) throw new IOException("The game process cannot be inspected.");
            try
            {
                uint flags;
                if (!GetProcessMitigationPolicy(process, ImageLoadPolicy, out flags, (UIntPtr)4))
                    throw new IOException("The game process image-load policy cannot be read.");
                return (flags & 4) != 0;
            }
            finally { CloseHandle(process); }
        }

        public sealed class ProcessLink
        {
            public int Id { get; set; }
            public string Name { get; set; } = "";
            public string Path { get; set; } = ""; // empty when Windows does not report it
            public bool? PreferSystem32Images { get; set; }
        }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct ProcessEntry
        {
            public uint Size, Usage, ProcessId;
            public IntPtr DefaultHeap;
            public uint Module, Threads, ParentProcessId;
            public int BasePriority;
            public uint Flags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string ExeFile;
        }
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint processId);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool Process32FirstW(IntPtr snapshot, ref ProcessEntry entry);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool Process32NextW(IntPtr snapshot, ref ProcessEntry entry);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetProcessTimes(IntPtr process, out long creation, out long exit, out long kernel, out long user);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool QueryFullProcessImageNameW(IntPtr process, uint flags, StringBuilder name, ref uint size);

        // Read-only: a process and its live ancestors, nearest first. The image-load
        // rule passes from parent to child, so the farthest ancestor that has it is
        // where it came from. A parent must have started before its child; an exited
        // parent (or a reused id) ends the walk, as does one that cannot be opened.
        public static ProcessLink[] Ancestry(int processId, int limit)
        {
            var known = new Dictionary<int, ProcessEntry>();
            IntPtr snapshot = CreateToolhelp32Snapshot(2, 0);
            if (snapshot == new IntPtr(-1)) throw new IOException("Processes cannot be listed.");
            try
            {
                var entry = new ProcessEntry { Size = (uint)Marshal.SizeOf<ProcessEntry>() };
                for (bool more = Process32FirstW(snapshot, ref entry); more; more = Process32NextW(snapshot, ref entry))
                    known[(int)entry.ProcessId] = entry;
            }
            finally { CloseHandle(snapshot); }
            var result = new List<ProcessLink>();
            long childStarted = long.MaxValue;
            for (int id = processId; id > 4 && result.Count < limit && known.TryGetValue(id, out var entry);)
            {
                const uint QueryLimited = 0x1000;
                IntPtr process = OpenProcess(QueryLimited, false, id);
                if (process == IntPtr.Zero) break;
                long started;
                string path = "";
                try
                {
                    if (!GetProcessTimes(process, out started, out _, out _, out _) || started > childStarted) break;
                    var name = new StringBuilder(1024);
                    uint size = (uint)name.Capacity;
                    if (QueryFullProcessImageNameW(process, 0, name, ref size)) path = name.ToString();
                }
                finally { CloseHandle(process); }
                bool? prefer = null;
                try { prefer = PrefersSystem32Images(id); } catch (IOException) { }
                result.Add(new ProcessLink { Id = id, Name = entry.ExeFile, Path = path, PreferSystem32Images = prefer });
                childStarted = started;
                id = (int)entry.ParentProcessId;
            }
            return result.ToArray();
        }

        public static string[] Paths(int processId, string moduleName)
        {
            const uint QueryInformation = 0x0400, VmRead = 0x0010, AllModules = 0x03;
            IntPtr process = OpenProcess(QueryInformation | VmRead, false, processId);
            if (process == IntPtr.Zero) throw new IOException("The game process cannot be inspected.");
            try
            {
                var modules = new IntPtr[2048];
                uint needed;
                if (!EnumProcessModulesEx(process, modules, (uint)(modules.Length * IntPtr.Size), out needed, AllModules))
                    throw new IOException("The game process modules cannot be listed.");
                var result = new List<string>();
                int count = Math.Min(modules.Length, (int)(needed / (uint)IntPtr.Size));
                for (int i = 0; i < count; ++i)
                {
                    var name = new StringBuilder(1024);
                    if (GetModuleFileNameExW(process, modules[i], name, (uint)name.Capacity) == 0) continue;
                    string path = name.ToString();
                    if (string.Equals(Path.GetFileName(path), moduleName, StringComparison.OrdinalIgnoreCase)) result.Add(path);
                }
                return result.ToArray();
            }
            finally { CloseHandle(process); }
        }
    }
}
