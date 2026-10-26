using System;
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
                if (file.Length < certificate + 8 || file.Length > certificate + 65536)
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
            return new IOException("Unsupported or changed GRID 2 executable (requires the Valve-signed 1.0.85.8679 build): " + Path.GetFileName(path));
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
}
