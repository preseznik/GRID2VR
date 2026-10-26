using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace GRID2VR
{
    // Used only for the known Documents hardware settings, never game files or saves.
    public static class HardwareSettingsPath
    {
        public static bool IsCloudTag(uint tag)
        {
            // IO_REPARSE_TAG_CLOUD and CLOUD_1 through CLOUD_F (cldflt placeholders).
            return (tag & ~0x0000F000u) == 0x9000001Au;
        }

        public static string Validate(string path)
        {
            string full = Path.GetFullPath(path);
            ValidateAncestors(full);
            return full;
        }

        private static void ValidateAncestors(string path)
        {
            var parent = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(parent)) ValidateAncestors(parent);
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) == 0) return;
            uint tag = ReadTag(path);
            if (!IsCloudTag(tag))
                throw new IOException("Linked hardware settings retained: " + path + " (tag 0x" + tag.ToString("X8") + ").");
        }

        public static uint ReadTag(string path)
        {
            // Inspect the reparse point itself, including directories, without following it.
            using (SafeFileHandle handle = CreateFileW(path, 0, 7, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero))
            {
                if (handle.IsInvalid) throw new IOException("Cannot inspect hardware settings: " + path, new Win32Exception(Marshal.GetLastWin32Error()));
                AttributeTag info;
                if (!GetFileInformationByHandleEx(handle, 9, out info, (uint)Marshal.SizeOf(typeof(AttributeTag))))
                    throw new IOException("Cannot inspect hardware settings: " + path, new Win32Exception(Marshal.GetLastWin32Error()));
                return info.ReparseTag;
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct AttributeTag { public uint FileAttributes; public uint ReparseTag; }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
        private static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetFileInformationByHandleEx(SafeFileHandle handle, int infoClass, out AttributeTag info, uint size);
    }
}
