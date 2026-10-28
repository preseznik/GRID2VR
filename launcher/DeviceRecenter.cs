using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace GRID2VR;

internal sealed class RecenterEdge
{
    private bool armed;
    public bool Update(bool connected, bool active, bool down)
    {
        if (!connected || !active) { armed = false; return false; }
        if (!down) { armed = true; return false; }
        if (!armed) return false;
        armed = false; return true;
    }
}

internal static class DeviceRecenter
{
    public static void Validate(DrivingBinding binding)
    {
        // Share device validation with driving bindings, but do not install a game binding.
        new DrivingControls { Bindings = [binding with { Action = "Change View" }] }.Validate();
        if (binding.Action != "Recenter" || binding.Keyboard || DrivingControls.DirectAxes.Contains(binding.Input) || DrivingControls.XboxAxes.Contains(binding.Input))
            throw new IOException("Recenter requires a controller/wheel button or hat direction.");
    }
    public static string Encode(DrivingBinding binding)
    {
        Validate(binding); return Convert.ToBase64String(Encoding.UTF8.GetBytes(JsonSerializer.Serialize(binding)));
    }
    public static DrivingBinding Decode(string encoded)
    {
        if (encoded.Length > 4096) throw new IOException("Recenter binding exceeds its size limit.");
        var binding = JsonSerializer.Deserialize<DrivingBinding>(Convert.FromBase64String(encoded)) ?? throw new IOException("Missing recenter binding.");
        Validate(binding); return binding;
    }
    public static bool Down(DrivingBinding binding, DrivingInput.Sample sample)
    {
        if (sample.Connected == 0) return false;
        if (binding.DeviceType == "xinput_pad") return sample.Buttons[Array.IndexOf(DrivingControls.XboxButtons, binding.Input)] != 0;
        if (binding.Input.StartsWith("di_button_", StringComparison.Ordinal)) return sample.Buttons[int.Parse(binding.Input[10..])] != 0;
        var parts = binding.Input.Split('_');
        return DrivingInput.PovDirection(sample.Pov[int.Parse(parts[2])]) == Array.IndexOf(new[] { "up", "right", "down", "left" }, parts[3]);
    }
    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(nint window, out uint process);
    internal static string OwnedRun(string root, string run)
    {
        // Session logs now live in logs/<timestamp>; keep the same ownership
        // rules as the launcher's log viewer, including legacy diagnostic runs.
        return new SessionClient(root).OwnedLogFolder(run) ?? throw new IOException("Unowned helper session log folder.");
    }
    // Independent hidden process: closing the launcher never disables this shortcut.
    // It only reads nonexclusive input and writes the existing native recenter request.
    public static int Watch(string root, string run, string encoded, int pid, long ticks)
    {
        string? receipt = null; int requests = 0, connectedPolls = 0; string result = "game-exited";
        try
        {
            run = OwnedRun(root, run); receipt = Path.Combine(run, "recenter-controller.json");
            var binding = Decode(encoded);
            using var mutex = new Mutex(false, @"Local\GRID2VR.Recenter." + pid);
            bool locked; try { locked = mutex.WaitOne(0); } catch (AbandonedMutexException) { locked = true; }
            if (!locked) return 0;
            try
            {
                using var game = Process.GetProcessById(pid);
                if (game.StartTime.ToUniversalTime().Ticks != ticks) throw new IOException("Game process identity changed.");
                var journalPath = Path.Combine(run, "session.json");
                if (new FileInfo(journalPath).Length > 1024 * 1024) throw new IOException("Session receipt exceeds its bound.");
                using var journal = JsonDocument.Parse(File.ReadAllText(journalPath));
                var owned = journal.RootElement.GetProperty("processes").EnumerateArray().Any(p => p.GetProperty("id").GetInt32() == pid &&
                    string.Equals(p.GetProperty("path").GetString(), game.MainModule?.FileName, StringComparison.OrdinalIgnoreCase) &&
                    Math.Abs(DateTimeOffset.Parse(p.GetProperty("createdUtc").GetString()!).UtcTicks - ticks) < TimeSpan.TicksPerSecond);
                if (!owned || game.ProcessName is not ("grid2" or "grid2_avx")) throw new IOException("Game does not belong to this session.");
                using var window = new Form { ShowInTaskbar = false }; // HWND is required by DirectInput; never Show().
                var handle = window.Handle;
                DrivingInput? input = null; DrivingInput.Device? device = null;
                var edge = new RecenterEdge(); long refreshAt = 0;
                try
                {
                    while (!game.HasExited)
                    {
                        if (File.Exists(Path.Combine(run, "runtime-stopped")) || File.Exists(Path.Combine(run, "session-exit.request"))) { result = "session-stopped"; break; }
                        var now = Environment.TickCount64;
                        if (device is null && now >= refreshAt)
                        {
                            input?.Dispose(); input = new DrivingInput(handle);
                            device = input.Devices.FirstOrDefault(d => d.Id.Equals(binding.DeviceId, StringComparison.OrdinalIgnoreCase) && d.Product.Equals(binding.DeviceType, StringComparison.OrdinalIgnoreCase));
                            refreshAt = now + 2000;
                        }
                        var sample = device is null ? DrivingInput.Sample.Empty() : input!.Read(device);
                        if (sample.Connected != 0) connectedPolls++;
                        GetWindowThreadProcessId(GetForegroundWindow(), out var foreground);
                        if (edge.Update(sample.Connected != 0, foreground == pid, Down(binding, sample)))
                        {
                            var request = Path.Combine(run, "runtime-recenter.request");
                            try { using var file = new FileStream(request, FileMode.CreateNew, FileAccess.Write, FileShare.Read); requests++; }
                            catch (IOException) when (File.Exists(request)) { } // Native consumer has not read the preceding request yet.
                        }
                        if (sample.Connected == 0) device = null;
                        Application.DoEvents(); Thread.Sleep(25);
                    }
                }
                finally { input?.Dispose(); }
            }
            finally { mutex.ReleaseMutex(); }
            return 0;
        }
        catch (Exception ex) { result = ex.Message; return 1; }
        finally
        {
            if (receipt is not null)
                try { File.WriteAllText(receipt, JsonSerializer.Serialize(new { requests, connectedPolls, result })); } catch (IOException) { }
        }
    }
}
