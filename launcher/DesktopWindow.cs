using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace GRID2VR;

// Window frame/placement only. Never activates a window or changes display mode.
internal sealed class DesktopWindow(int processId)
{
    internal const long FrameBits = 0xC00000L | 0x40000L | 0x80000L | 0x30000L;
    internal const long EdgeBits = 0x1L | 0x100L | 0x200L | 0x20000L;
    private const uint PositionFlags = 0x10 | 0x20 | 0x200 | 0x4000; // NoActivate, FrameChanged, NoOwnerZOrder, AsyncWindowPos.
    private sealed record Attempt(long Style, long ExStyle, Rectangle Original, Rectangle Target, long Deadline)
    {
        public bool Complete;
    }
    private readonly Dictionary<nint, Attempt> attempts = [];
    internal int Applied { get; private set; }
    internal string Warning { get; private set; } = "";

    internal static bool Eligible(nint window, int pid)
    {
        if (window == 0 || GetWindowThreadProcessId(window, out var owner) == 0 || owner != pid ||
            !IsWindowVisible(window) || IsIconic(window) || IsHungAppWindow(window)) return false;
        var name = new StringBuilder(256);
        return GetClassNameW(window, name, name.Capacity) != 0 && name.ToString() != "#32770";
    }

    // The watcher checks visibility; the lower-level operation also supports hidden owned-window tests.
    internal void Update(nint window, Rectangle target, long now)
    {
        if (window == 0 || GetWindowThreadProcessId(window, out var owner) == 0 || owner != processId || IsIconic(window)) return;
        if (attempts.TryGetValue(window, out var attempt) && attempt.Complete) return;
        try
        {
            if (attempt is null)
            {
                if (target.Width <= 0 || target.Height <= 0) throw new IOException("Invalid desktop monitor bounds.");
                if (attempts.Count >= 16) { Warning = "Too many startup windows; retaining windowed mode."; return; }
                var original = Bounds(window);
                if (original.Width <= 0 || original.Height <= 0) return;
                attempt = new(Style(window, -16), Style(window, -20), original, target, now + 5000);
                attempts.Add(window, attempt);
                WriteStyle(window, -16, (attempt.Style & ~FrameBits) | 0x80000000L);
                WriteStyle(window, -20, attempt.ExStyle & ~EdgeBits);
                Position(window, target, (attempt.ExStyle & 8) != 0 ? -2 : null);
            }
            else if (Bounds(window) == attempt.Target && (Style(window, -16) & FrameBits) == 0 && (Style(window, -20) & (EdgeBits | 8)) == 0)
            {
                attempt.Complete = true;
                Applied++;
            }
            else if (now >= attempt.Deadline) throw new IOException("The game did not accept borderless window placement.");
        }
        catch (Exception error) when (error is Win32Exception or IOException)
        {
            Warning = "Borderless unavailable; using windowed mode. " + error.Message;
            if (attempt is null) return;
            attempt.Complete = true; // One attempt per startup window; do not fight game/user changes.
            try
            {
                WriteStyle(window, -16, attempt.Style);
                WriteStyle(window, -20, attempt.ExStyle);
                Position(window, attempt.Original, (attempt.ExStyle & 8) != 0 ? -1 : -2);
            }
            catch (Win32Exception rollback) { Warning += " Window restoration failed: " + rollback.Message; }
        }
    }

    private static bool Flag(JsonElement value, string name) => value.TryGetProperty(name, out var flag) && flag.ValueKind == JsonValueKind.True;

    internal static void ValidateOwner(JsonElement journal, string gameRoot, string executable, int pid, long ticks, bool vrMirror = false)
    {
        var name = Path.GetFileName(executable);
        if (name is not ("grid2.exe" or "grid2_avx.exe") ||
            !string.Equals(Path.GetFullPath(executable), Path.Combine(gameRoot, name), StringComparison.OrdinalIgnoreCase) ||
            !journal.GetProperty("runtimeSession").GetBoolean() || !journal.GetProperty("untilExit").GetBoolean() ||
            !(vrMirror ? !Flag(journal, "playerControlOnly") && Flag(journal, "xrProjection") && Flag(journal, "vrMirrorBorderlessRequested")
                : Flag(journal, "playerControlOnly") && Flag(journal, "desktopBorderlessRequested")) ||
            !journal.GetProperty("processes").EnumerateArray().Any(p => p.GetProperty("id").GetInt32() == pid &&
                string.Equals(p.GetProperty("path").GetString(), executable, StringComparison.OrdinalIgnoreCase) &&
                Math.Abs(DateTimeOffset.Parse(p.GetProperty("createdUtc").GetString()!).UtcTicks - ticks) < TimeSpan.TicksPerSecond))
            throw new IOException("Window does not belong to the requested owned game session.");
    }

    internal static bool ResolutionReady(JsonElement resolution, int pid, nint window)
    {
        if (!Flag(resolution, "applied") || !Flag(resolution, "operationComplete") ||
            !resolution.TryGetProperty("processId", out var process) || !process.TryGetInt32(out var owner) || owner != pid ||
            !resolution.TryGetProperty("window", out var handle) || !handle.TryGetInt64(out var value) || value != window.ToInt64() ||
            !resolution.TryGetProperty("requested", out var requested) || !resolution.TryGetProperty("backbuffer", out var actual)) return false;
        return Size(requested, out var w, out var h) && Size(actual, out var aw, out var ah) && w == aw && h == ah;
        static bool Size(JsonElement value, out int width, out int height)
        {
            width = height = 0;
            return value.ValueKind == JsonValueKind.Object && value.TryGetProperty("width", out var w) && w.TryGetInt32(out width) && width > 0 &&
                value.TryGetProperty("height", out var h) && h.TryGetInt32(out height) && height > 0;
        }
    }

    private static bool ResolutionReady(string run, int pid, nint window)
    {
        try
        {
            var path = Path.Combine(run, "scene-resolution.json");
            if (!File.Exists(path) || new FileInfo(path).Length > 64 * 1024) return false;
            using var resolution = JsonDocument.Parse(File.ReadAllText(path));
            return ResolutionReady(resolution.RootElement, pid, window);
        }
        catch (Exception error) when (error is IOException or JsonException or InvalidOperationException) { return false; }
    }

    internal static int Watch(string root, string run, int pid, long ticks, bool vrMirror = false)
    {
        string? receipt = null;
        var window = new DesktopWindow(pid);
        var result = "watching";
        void Save() => File.WriteAllText(receipt!, JsonSerializer.Serialize(new { result, appliedWindows = window.Applied, warning = window.Warning }));
        try
        {
            run = DeviceRecenter.OwnedRun(root, run);
            using var mutex = new Mutex(false, @"Local\GRID2VR.DesktopWindow." + pid);
            bool locked; try { locked = mutex.WaitOne(0); } catch (AbandonedMutexException) { locked = true; }
            if (!locked) return 0;
            receipt = Path.Combine(run, vrMirror ? "vr-mirror-window.json" : "desktop-window.json");
            try
            {
                using var game = Process.GetProcessById(pid);
                if (game.StartTime.ToUniversalTime().Ticks != ticks) throw new IOException("Game process identity changed.");
                var journalPath = Path.Combine(run, "session.json");
                if (new FileInfo(journalPath).Length > 1024 * 1024) throw new IOException("Session receipt exceeds its bound.");
                using var journal = JsonDocument.Parse(File.ReadAllText(journalPath));
                ValidateOwner(journal.RootElement, AppPaths.Game(root), game.MainModule?.FileName ?? "", pid, ticks, vrMirror);
                Installation.ValidateGame(AppPaths.Game(root));
                string previous = "";
                while (!game.HasExited)
                {
                    if (File.Exists(Path.Combine(run, "session-exit.request"))) { result = "session-stopped"; break; }
                    game.Refresh();
                    var handle = game.MainWindowHandle;
                    // Once placement starts, always finish verification/rollback,
                    // including if the resulting resize reports a renderer failure.
                    if (Eligible(handle, pid) && (!vrMirror || window.attempts.ContainsKey(handle) || ResolutionReady(run, pid, handle)))
                        window.Update(handle, Screen.FromHandle(handle).Bounds, Environment.TickCount64);
                    var state = window.Applied + ":" + window.Warning;
                    if (state != previous) { Save(); previous = state; }
                    Thread.Sleep(250);
                }
                if (result == "watching") result = window.Applied > 0 ? "game-exited" : "game-exited-window-unobserved";
            }
            finally { mutex.ReleaseMutex(); }
            return 0;
        }
        catch (Exception error) { result = error.Message; return 1; }
        finally { if (receipt is not null) try { Save(); } catch (IOException) { } }
    }

    internal static long Style(nint window, int index)
    {
        Marshal.SetLastPInvokeError(0);
        var value = GetWindowLongPtrW(window, index);
        if (value == 0 && Marshal.GetLastPInvokeError() != 0) throw new Win32Exception(Marshal.GetLastPInvokeError());
        return value.ToInt64();
    }
    private static void WriteStyle(nint window, int index, long value)
    {
        Marshal.SetLastPInvokeError(0);
        if (SetWindowLongPtrW(window, index, new(value)) == 0 && Marshal.GetLastPInvokeError() != 0)
            throw new Win32Exception(Marshal.GetLastPInvokeError());
    }
    internal static Rectangle Bounds(nint window)
    {
        if (!GetWindowRect(window, out var rect)) throw new Win32Exception(Marshal.GetLastPInvokeError());
        return Rectangle.FromLTRB(rect.Left, rect.Top, rect.Right, rect.Bottom);
    }
    private static void Position(nint window, Rectangle target, int? order)
    {
        if (!SetWindowPos(window, order ?? 0, target.X, target.Y, target.Width, target.Height, PositionFlags | (order is null ? 4u : 0u)))
            throw new Win32Exception(Marshal.GetLastPInvokeError());
    }
    [StructLayout(LayoutKind.Sequential)] private struct WindowRect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll", SetLastError = true)] private static extern nint GetWindowLongPtrW(nint window, int index);
    [DllImport("user32.dll", SetLastError = true)] private static extern nint SetWindowLongPtrW(nint window, int index, nint value);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool SetWindowPos(nint window, nint after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool GetWindowRect(nint window, out WindowRect rect);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(nint window, out int pid);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] private static extern bool IsIconic(nint window);
    [DllImport("user32.dll")] private static extern bool IsHungAppWindow(nint window);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassNameW(nint window, StringBuilder name, int length);
}
