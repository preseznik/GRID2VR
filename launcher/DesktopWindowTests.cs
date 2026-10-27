using System.Runtime.InteropServices;
using System.Text.Json;

namespace GRID2VR;

internal static class DesktopWindowTests
{
    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
    internal static void Run(string fixture)
    {
        static void Check(bool value) { if (!value) throw new IOException("Desktop borderless regression."); }
        static void Reject(Action action) { try { action(); } catch (IOException) { return; } throw new IOException("Foreign desktop session accepted."); }
        var gameRoot = Path.Combine(fixture, "game");
        var executable = Path.Combine(gameRoot, "grid2_avx.exe");
        var ticks = DateTime.UtcNow.Ticks;
        var journal = new Dictionary<string, object> { ["runtimeSession"] = true, ["untilExit"] = true, ["playerControlOnly"] = true,
            ["processes"] = new[] { new { id = 123, path = executable, createdUtc = new DateTime(ticks, DateTimeKind.Utc).ToString("o") } } };
        void Validate(string path, long time)
        {
            using var doc = JsonDocument.Parse(JsonSerializer.Serialize(journal));
            DesktopWindow.ValidateOwner(doc.RootElement, gameRoot, path, 123, time);
        }
        Validate(executable, ticks);
        Reject(() => Validate(executable, ticks + TimeSpan.TicksPerSecond * 2));
        Reject(() => Validate(Path.Combine(fixture, "foreign", "grid2_avx.exe"), ticks));
        Reject(() => Validate(Path.Combine(gameRoot, "other.exe"), ticks));
        foreach (var field in new[] { "runtimeSession", "untilExit", "playerControlOnly" })
        {
            journal[field] = false; Reject(() => Validate(executable, ticks)); journal[field] = true;
        }
        journal["playerControlOnly"] = false;
        journal["xrProjection"] = true;
        journal["vrMirrorBorderlessRequested"] = true;
        void ValidateVr()
        {
            using var doc = JsonDocument.Parse(JsonSerializer.Serialize(journal));
            DesktopWindow.ValidateOwner(doc.RootElement, gameRoot, executable, 123, ticks, true);
        }
        ValidateVr();
        Reject(() => Validate(executable, ticks));
        foreach (var field in new[] { "runtimeSession", "untilExit", "xrProjection", "vrMirrorBorderlessRequested" })
        {
            journal[field] = false; Reject(ValidateVr); journal[field] = true;
        }
        journal["playerControlOnly"] = true; Reject(ValidateVr);
        var resolution = new Dictionary<string, object> { ["applied"] = true, ["operationComplete"] = true, ["processId"] = 123, ["window"] = 456,
            ["requested"] = new { width = 3840, height = 2160 }, ["backbuffer"] = new { width = 3840, height = 2160 } };
        bool Ready()
        {
            using var doc = JsonDocument.Parse(JsonSerializer.Serialize(resolution));
            return DesktopWindow.ResolutionReady(doc.RootElement, 123, 456);
        }
        Check(Ready());
        foreach (var field in new[] { "applied", "operationComplete", "processId", "window", "backbuffer" })
        {
            var originalValue = resolution[field];
            resolution[field] = field switch { "processId" => 124, "window" => 457, "backbuffer" => new { width = 2560, height = 1417 }, _ => false };
            Check(!Ready()); resolution.Remove(field); Check(!Ready()); resolution[field] = originalValue;
        }

        // Real Win32 frame/placement tests use only our never-shown fixture windows.
        // No existing app/game window, focus, mouse or keyboard is controlled.
        var foreground = GetForegroundWindow();
        using var form = new Form { ShowInTaskbar = false, StartPosition = FormStartPosition.Manual, Bounds = new(-20000, -20000, 640, 480) };
        var handle = form.Handle;
        var original = DesktopWindow.Bounds(handle); var style = DesktopWindow.Style(handle, -16);
        Check(!form.Visible && !DesktopWindow.Eligible(handle, Environment.ProcessId));
        new DesktopWindow(Environment.ProcessId + 1).Update(handle, new(0, 0, 800, 600), 0);
        Check(DesktopWindow.Bounds(handle) == original && DesktopWindow.Style(handle, -16) == style);
        var target = new Rectangle(original.X, original.Y, 800, 600);
        var manager = new DesktopWindow(Environment.ProcessId);
        manager.Update(handle, target, 0); manager.Update(handle, target, 250);
        Check(manager.Applied == 1 && manager.Warning == "" && DesktopWindow.Bounds(handle) == target);
        Check((DesktopWindow.Style(handle, -16) & DesktopWindow.FrameBits) == 0);
        Check((DesktopWindow.Style(handle, -20) & (DesktopWindow.EdgeBits | 8)) == 0);
        Check(!form.Visible && GetForegroundWindow() == foreground);
        form.Bounds = original; manager.Update(handle, target, 500);
        Check(DesktopWindow.Bounds(handle) == original); // Later user/game changes are respected.

        using var replacement = new Form { ShowInTaskbar = false, StartPosition = FormStartPosition.Manual, Bounds = original };
        var second = replacement.Handle;
        manager.Update(second, target, 750); manager.Update(second, target, 1000);
        Check(manager.Applied == 2 && DesktopWindow.Bounds(second) == target && !replacement.Visible);

        using var rejected = new Form { ShowInTaskbar = false, StartPosition = FormStartPosition.Manual, Bounds = original };
        var third = rejected.Handle; var beforeStyle = DesktopWindow.Style(third, -16); var beforeExStyle = DesktopWindow.Style(third, -20);
        var fallback = new DesktopWindow(Environment.ProcessId);
        fallback.Update(third, target, 0);
        rejected.Bounds = original; // Simulate a game refusing the requested dimensions.
        fallback.Update(third, target, 6000);
        Check(fallback.Applied == 0 && fallback.Warning.Contains("using windowed mode"));
        Check(DesktopWindow.Bounds(third) == original && DesktopWindow.Style(third, -16) == beforeStyle && DesktopWindow.Style(third, -20) == beforeExStyle);
        Check(!rejected.Visible && GetForegroundWindow() == foreground);
    }
}
