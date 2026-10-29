using System.Diagnostics;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace GRID2VR;

internal sealed record Choices(string Car = "pag", string Track = "p2p/okutama/route_1", string Runtime = "VDXR", int RecenterKey = 121, int RecenterModifiers = 0, int Laps = 1)
{
    public GraphicsOptions Graphics { get; init; } = GraphicsOptions.Defaults;
    public bool ModernInterface { get; init; } = true;
    public bool PreviewVrFixes { get; init; }
    public bool SkipIntro { get; init; }
    public bool DiagnosticLogging { get; init; }
    public bool PerformanceDisplay { get; init; }
    public bool PerformanceFollowView { get; init; } = true;
    public string LaunchMode { get; init; } = "Practice";
    public int Opponents { get; init; } = 7;
    public string OpponentCars { get; init; } = "same";
    public bool VrReplayCameras { get; init; } = true;
    public bool VrGarageMenus { get; init; } = true;
    public bool VrCockpit { get; init; } = true;
    public string CockpitDriver { get; init; } = "HeadOnly";
    public bool CockpitExposureLimit { get; init; } = true;
    public bool HideImpactGlitch { get; init; } = true;
    public string CockpitTextures { get; init; } = "Off";
    public DrivingBinding? RecenterDevice { get; init; }
    public bool KeepHorizonLevel { get; init; } = true;
    public bool ChaseFreeLook { get; init; }
    public bool ChaseKeepAngle { get; init; }
    public string ChaseMouseMode { get; init; } = "right";
    public int ChaseMaxZoomOut { get; init; } = 180;
    public int ChaseMouseSensitivity { get; init; } = 100;
    public int ChaseStickSensitivity { get; init; } = 100;
    public bool ChaseInvertVertical { get; init; }
}
internal sealed record ContentItem(string Id, string Label, string? ClassId = null, string? ClassName = null, bool CockpitAvailable = true, bool Dlc = false)
{
    public override string ToString() => Label;
}

internal sealed class SessionClient(string root)
{
    public string Root { get; } = Path.GetFullPath(root);
    public string Artifacts => AppPaths.Data(Root);
    private string SettingsPath => Path.Combine(Artifacts, "launcher-settings.json");
    private string WindowPath => Path.Combine(Artifacts, "launcher-window.json");
    private static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true, WriteIndented = true };

    public static string FindRoot(string start)
    {
        if (File.Exists(Path.Combine(start, "GRID2VR", "package.json"))) return Path.Combine(start, "GRID2VR");
        for (var folder = new DirectoryInfo(start); folder != null; folder = folder.Parent)
            if (File.Exists(Path.Combine(folder.FullName, "tools", "Start-UserSession.ps1"))) return folder.FullName;
        throw new IOException("Keep this prototype inside the GRID2VR workspace.");
    }

    public ProcessStartInfo Command(string script, params string[] arguments)
    {
        if (script is not ("Start-UserSession.ps1" or "Session-Control.ps1" or "Cockpit-TexturePack.ps1" or "Get-VrResolution.ps1")) throw new ArgumentException("Unknown command.");
        var shell = AppPaths.Shell(Root);
        if (!File.Exists(shell)) throw new IOException("PowerShell 7 is required for this workspace prototype.");
        var info = new ProcessStartInfo(shell) { WorkingDirectory = Root, UseShellExecute = false, CreateNoWindow = true };
        foreach (var arg in new[] { "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", Path.Combine(Root, "tools", script) }.Concat(arguments))
            info.ArgumentList.Add(arg);
        return info;
    }

    public static void Validate(Choices value)
    {
        if (!Regex.IsMatch(value.Car, @"\A[a-z0-9_]+\z") ||
            !Regex.IsMatch(value.Track, @"\A(circuits|cities|p2p|australia|europe)/[a-z0-9_]+/route_[0-9]+\z") ||
            value.Runtime is not ("VDXR" or "SteamVR")) throw new ArgumentException("Invalid session selection.");
        if (value.LaunchMode is not ("Practice" or "Race" or "Normal")) throw new ArgumentException("Choose Normal, Practice or Race.");
        if (value.Opponents is < 1 or > 7) throw new ArgumentException("Choose between 1 and 7 opponents.");
        if (value.OpponentCars is not ("same" or "mixed" or "class")) throw new ArgumentException("Choose Same car, Mixed or Same class.");
        if (value.CockpitDriver is not ("HeadOnly" or "HideDriver")) throw new ArgumentException("Choose a cockpit driver visibility option.");
        if (value.CockpitTextures is not ("Off" or "Replace")) throw new ArgumentException("Choose old or new cockpit textures.");
        if (!RecenterShortcut.Valid(value.RecenterKey, value.RecenterModifiers)) throw new ArgumentException("Invalid recenter shortcut; system shortcuts are reserved.");
        if (value.Laps is < 1 or > 20) throw new ArgumentException("Choose between 1 and 20 circuit laps.");
        if (value.Graphics == null) throw new ArgumentException("Graphics settings are missing.");
        if (value.ChaseMouseMode is not ("left" or "right" or "always")) throw new ArgumentException("Choose Hold left, Hold right or Always on for mouse look.");
        if (value.ChaseMaxZoomOut is < 180 or > 1000 || value.ChaseMaxZoomOut % 10 != 0) throw new ArgumentException("Choose maximum zoom out between 180% and 1000%, in 10% steps.");
        if (value.ChaseMouseSensitivity is < 25 or > 300 || value.ChaseStickSensitivity is < 25 or > 300) throw new ArgumentException("Choose chase-camera sensitivity between 25% and 300%.");
        value.Graphics.Validate();
        if (value.RecenterDevice is not null) DeviceRecenter.Validate(value.RecenterDevice);
    }

    public Choices Load()
    {
        if (!File.Exists(SettingsPath)) return new();
        if (new FileInfo(SettingsPath).Length > 8192) throw new IOException("Launcher settings exceed their size limit.");
        return ParseChoices(File.ReadAllText(SettingsPath));
    }

    internal static Choices ParseChoices(string text)
    {
        var value = JsonSerializer.Deserialize<Choices>(text, Json) ?? throw new IOException("Invalid launcher settings.");
        using var document = JsonDocument.Parse(text);
        // Match DiRT2VR: existing installations keep Classic; fresh settings use Modern.
        if (!document.RootElement.EnumerateObject().Any(p => p.Name.Equals("ModernInterface", StringComparison.OrdinalIgnoreCase)))
            value = value with { ModernInterface = false };
        // Retain the accepted Focus preview choice when upgrading old settings.
        if (!document.RootElement.EnumerateObject().Any(p => p.Name.Equals("CockpitTextures", StringComparison.OrdinalIgnoreCase)))
        {
            var legacy = document.RootElement.EnumerateObject().FirstOrDefault(p => p.Name.Equals("FocusCockpit", StringComparison.OrdinalIgnoreCase));
            if (legacy.Value.ValueKind == JsonValueKind.String)
                value = value with { CockpitTextures = legacy.Value.GetString() == "Replace" ? "Replace" : "Off" };
        }
        Validate(value);
        return value;
    }

    public void Save(Choices value)
    {
        Validate(value);
        Directory.CreateDirectory(Artifacts);
        var temporary = SettingsPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try { File.WriteAllText(temporary, JsonSerializer.Serialize(value, Json)); File.Move(temporary, SettingsPath, true); }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    public LauncherWindow LoadWindow()
    {
        try
        {
            if (!File.Exists(WindowPath) || new FileInfo(WindowPath).Length > 4096) return new();
            var value = JsonSerializer.Deserialize<LauncherWindow>(File.ReadAllText(WindowPath), Json);
            return value is { Valid: true } ? value : new();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException) { return new(); }
    }

    internal bool UseAdvancedView(LauncherWindow window) => window.AdvancedView ?? File.Exists(SettingsPath);

    public void SaveWindow(LauncherWindow value)
    {
        if (!value.Valid) return;
        Directory.CreateDirectory(Artifacts);
        var temporary = WindowPath + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try { File.WriteAllText(temporary, JsonSerializer.Serialize(value, Json)); File.Move(temporary, WindowPath, true); }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }

    public string LogsFolder()
    {
        Updates.PlainPath(Artifacts);
        Directory.CreateDirectory(Artifacts);
        foreach (var name in new[] { "session-pending.json", "user-session-last.json" })
        {
            var path = Path.Combine(Artifacts, name);
            if (!File.Exists(path)) continue;
            try
            {
                Updates.PlainPath(path);
                if (new FileInfo(path).Length > 256 * 1024) return Artifacts;
                using var document = JsonDocument.Parse(File.ReadAllText(path));
                var receipt = document.RootElement;
                if (receipt.ValueKind != JsonValueKind.Object) return Artifacts;
                if (receipt.TryGetProperty("logDirectory", out var logDirectory) && logDirectory.ValueKind == JsonValueKind.String &&
                    logDirectory.GetString() is { Length: > 0 } current && OwnedLogFolder(current) is { } currentLogs) return currentLogs;
                if (receipt.TryGetProperty("run", out var run) && run.ValueKind == JsonValueKind.String && run.GetString() is { Length: > 0 } folder &&
                    OwnedLogFolder(folder) is { } found) return found;
                // Early launch failures have a worker log here, but no diagnostic run yet.
                if (receipt.TryGetProperty("log", out var log) && log.ValueKind == JsonValueKind.String && log.GetString() is { } file &&
                    Path.IsPathFullyQualified(file) && string.Equals(Path.GetDirectoryName(Path.GetFullPath(file)), Artifacts, StringComparison.OrdinalIgnoreCase) &&
                    Regex.IsMatch(Path.GetFileName(file), @"\Auser-session-[0-9a-f]{32}\.log\z")) return Artifacts;
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or ArgumentException)
            {
                // Broken or incomplete status must not prevent access to the actual logs.
                return Artifacts;
            }
        }
        var logs = Path.Combine(Artifacts, "logs");
        Updates.PlainPath(logs);
        if (Directory.Exists(logs))
            foreach (var folder in Directory.EnumerateDirectories(logs).OrderDescending(StringComparer.OrdinalIgnoreCase))
                if (OwnedLogFolder(folder) is { } found) return found;
        if (Directory.Exists(Artifacts))
            foreach (var folder in Directory.EnumerateDirectories(Artifacts, "diagnostic-*").OrderDescending(StringComparer.OrdinalIgnoreCase))
                if (OwnedLogFolder(folder) is { } found) return found;
        return Artifacts;
    }

    internal string? OwnedLogFolder(string path)
    {
        if (!Path.IsPathFullyQualified(path)) return null;
        var folder = Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar);
        var parent = Path.GetDirectoryName(folder);
        var legacy = string.Equals(parent, Artifacts, StringComparison.OrdinalIgnoreCase) &&
            Regex.IsMatch(Path.GetFileName(folder), @"\Adiagnostic-\d{8}-\d{6}-\d{3}\z");
        var grouped = string.Equals(parent, Path.Combine(Artifacts, "logs"), StringComparison.OrdinalIgnoreCase) &&
            Regex.IsMatch(Path.GetFileName(folder), @"\A\d{8}-\d{6}-\d{3}\z");
        if ((!legacy && !grouped) || !Directory.Exists(folder)) return null;
        for (var current = new DirectoryInfo(folder); current != null; current = current.Parent)
            if (current.Attributes.HasFlag(FileAttributes.ReparsePoint)) throw new IOException("Linked log folder retained: " + folder);
        return folder;
    }

    public ProcessStartInfo LaunchCommand(Choices value, bool vr)
    {
        Validate(value);
        var command = Command("Start-UserSession.ps1", "-Mode", vr ? "VR" : "Desktop", "-XrRuntime", value.Runtime, "-Car", value.Car, "-Track", value.Track,
            "-LaunchMode", value.LaunchMode, "-Opponents", value.Opponents.ToString(System.Globalization.CultureInfo.InvariantCulture),
            "-OpponentCars", value.OpponentCars,
            "-RecenterKey", value.RecenterKey.ToString(System.Globalization.CultureInfo.InvariantCulture),
            "-RecenterModifiers", value.RecenterModifiers.ToString(System.Globalization.CultureInfo.InvariantCulture),
            "-Laps", EffectiveLaps(value.Track, value.Laps).ToString(System.Globalization.CultureInfo.InvariantCulture),
            "-RenderScale", (vr ? value.Graphics.RenderScale : value.Graphics.EffectiveDesktopRenderScale).ToString(System.Globalization.CultureInfo.InvariantCulture),
            "-DesktopVSync", value.Graphics.DesktopVSync ? "1" : "0", "-DesktopMsaa", value.Graphics.DesktopMsaa,
            "-VrMsaa", value.Graphics.VrMsaa,
            "-TreeDetail", value.Graphics.TreeDetail, "-ObjectDetail", value.Graphics.ObjectDetail,
            "-CrowdDetail", value.Graphics.CrowdDetail, "-TextureFiltering", value.Graphics.TextureFiltering,
            "-GraphicsQualityBase64", GraphicsCatalog.EncodeExpanded(value.Graphics));
        foreach (var arg in new[] { "-ChaseFreeLook", value.ChaseFreeLook ? "1" : "0", "-ChaseKeepAngle", value.ChaseKeepAngle ? "1" : "0",
            "-ChaseMouseMode", value.ChaseMouseMode, "-ChaseMaxZoomOut", value.ChaseMaxZoomOut.ToString(System.Globalization.CultureInfo.InvariantCulture),
            "-ChaseMouseSensitivity", value.ChaseMouseSensitivity.ToString(System.Globalization.CultureInfo.InvariantCulture),
            "-ChaseStickSensitivity", value.ChaseStickSensitivity.ToString(System.Globalization.CultureInfo.InvariantCulture),
            "-ChaseInvertVertical", value.ChaseInvertVertical ? "1" : "0" }) command.ArgumentList.Add(arg);
        if (vr)
        {
            foreach (var arg in new[] { "-HudDistance", value.Graphics.HudDistance.ToString(System.Globalization.CultureInfo.InvariantCulture),
                "-CockpitMode", value.VrCockpit ? value.CockpitDriver : "Off",
                "-VrReplayCameras", value.VrReplayCameras ? "1" : "0",
                "-VrGarageMenus", value.VrGarageMenus ? "1" : "0",
                "-HudFollowView", value.Graphics.HudFollowView ? "1" : "0", "-HudHiddenElements", value.Graphics.HiddenHudElements.ToString(System.Globalization.CultureInfo.InvariantCulture) })
                command.ArgumentList.Add(arg);
        }
        if (vr) {
            command.ArgumentList.Add("-VrRenderScale");
            command.ArgumentList.Add(value.Graphics.VrRenderScale.ToString(System.Globalization.CultureInfo.InvariantCulture));
        }
        if (vr && value.Graphics.VrMirrorBorderless) command.ArgumentList.Add("-VrMirrorBorderless");
        if (!vr)
        {
            command.ArgumentList.Add("-DesktopBorderless");
            command.ArgumentList.Add(value.Graphics.DesktopBorderless ? "1" : "0");
        }
        if (vr && value.PerformanceDisplay)
        {
            command.ArgumentList.Add("-PerformanceDisplay");
            command.ArgumentList.Add("-PerformanceFollowView");
            command.ArgumentList.Add(value.PerformanceFollowView ? "1" : "0");
        }
        if (vr && !value.KeepHorizonLevel) command.ArgumentList.Add("-RecenterTilt");
        if (vr && !value.CockpitExposureLimit) command.ArgumentList.Add("-NativeCockpitExposure");
        if (vr && !value.HideImpactGlitch) command.ArgumentList.Add("-NativeImpactEffect");
        if (vr && value.RecenterDevice is not null) { command.ArgumentList.Add("-RecenterDeviceBase64"); command.ArgumentList.Add(DeviceRecenter.Encode(value.RecenterDevice)); }
        if (value.SkipIntro) command.ArgumentList.Add("-SkipIntro");
        if (value.DiagnosticLogging) command.ArgumentList.Add("-DiagnosticLogging");
        if (value.CockpitTextures != "Off") { command.ArgumentList.Add("-CockpitTextures"); command.ArgumentList.Add(value.CockpitTextures); }
        if (vr && (value.PreviewVrFixes || AppPaths.Packaged(Root))) command.ArgumentList.Add("-PreviewVrFixes");
        return command;
    }

    internal ProcessStartInfo PrepareLaunchCommand(Choices value, bool vr)
    {
        var command = LaunchCommand(value, vr);
        var controls = ExportDrivingControls();
        if (controls is not null)
        {
            command.ArgumentList.Add("-DrivingControls");
            command.ArgumentList.Add(controls);
        }
        // Normal launches pass the game's own Press Start screen, which claims the device.
        if (value.LaunchMode != "Normal" && DrivingControls.Load(Root).DeviceClaimSpec() is { } claim)
        {
            command.ArgumentList.Add("-DeviceClaim");
            command.ArgumentList.Add(claim);
        }
        return command;
    }

    public Process Launch(Choices value, bool vr)
    {
        var command = PrepareLaunchCommand(value, vr);
        Save(value);
        // No redirected streams: the independent worker survives closing this UI.
        return Process.Start(command) ?? throw new IOException("Could not start the session worker.");
    }

    internal string? ExportDrivingControls()
    {
        var controls = DrivingControls.Load(Root);
        if (!controls.Enabled || controls.Bindings.Count == 0 && controls.Transmission == "game") return null;
        var problems = controls.Problems();
        if (problems.Count != 0) throw new IOException(string.Join(Environment.NewLine, problems));
        Directory.CreateDirectory(Artifacts);
        var path = Path.Combine(Artifacts, "driving-controls.xml");
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try { controls.ToXml().Save(temporary); File.Move(temporary, path, true); }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
        return path;
    }

    public async Task<JsonElement> Control(string action)
    {
        if (action is not ("Status" or "Recenter" or "StopVR" or "End" or "Recover")) throw new ArgumentException("Unknown session action.");
        return JsonDocument.Parse(await ReadCommand(Command("Session-Control.ps1", "-Action", action))).RootElement.Clone();
    }

    public async Task<(ContentItem[] Cars, ContentItem[] Tracks)> Catalog()
    {
        var info = new ProcessStartInfo(AppPaths.Python(Root)) { WorkingDirectory = Root, UseShellExecute = false, CreateNoWindow = true };
        if(AppPaths.Packaged(Root)) { info.Environment.Remove("PYTHONHOME");info.Environment.Remove("PYTHONPATH");info.Environment["PYTHONNOUSERSITE"]="1";info.Environment["PYTHONDONTWRITEBYTECODE"]="1"; }
        info.ArgumentList.Add(Path.Combine(Root, "tools", "session_config.py")); info.ArgumentList.Add("--game-root"); info.ArgumentList.Add(AppPaths.Game(Root)); info.ArgumentList.Add("catalog");
        using var doc = JsonDocument.Parse(await ReadCommand(info));
        return ParseCatalog(doc.RootElement);
    }

    internal static (ContentItem[] Cars, ContentItem[] Tracks) ParseCatalog(JsonElement catalog)
    {
        static bool Flag(JsonElement item, string name, bool fallback) => item.TryGetProperty(name, out var value) ? value.GetBoolean() : fallback;
        static string Label(JsonElement item, string fallback)
        {
            var label = item.TryGetProperty("label", out var value) && !string.IsNullOrWhiteSpace(value.GetString()) ? value.GetString()! : fallback;
            return Flag(item, "dlc", false) ? label + " (DLC)" : label;
        }
        var cars = catalog.GetProperty("cars").EnumerateArray()
            .Where(c => c.GetProperty("liveries").EnumerateArray().Any(l => l.GetString() == "user"))
            .Select(c =>
            {
                var id = c.GetProperty("id").GetString()!;
                var dlc = Flag(c, "dlc", false);
                return new ContentItem(id, Label(c, CarLabel(id)), c.GetProperty("classId").GetString(), c.GetProperty("className").GetString(),
                    Flag(c, "cockpitAvailable", !dlc), dlc);
            }).OrderBy(c => c.Label).ToArray();
        var tracks = catalog.GetProperty("tracks").EnumerateArray().Select(t =>
        {
            var id = t.GetProperty("id").GetString()!;
            return new ContentItem(id, Label(t, TrackLabel(id)), Dlc: Flag(t, "dlc", false));
        }).OrderBy(t => t.Label).ToArray();
        return (cars, tracks);
    }

    internal static string CarLabel(string id) => CarNames.Label(id);
    internal static bool IsPointToPoint(string track) => track.StartsWith("p2p/", StringComparison.Ordinal);
    internal static int EffectiveLaps(string track, int laps) => IsPointToPoint(track) ? 1 : laps;
    internal static string LocationLabel(string id) => System.Globalization.CultureInfo.InvariantCulture.TextInfo
        .ToTitleCase(id[(id.LastIndexOf('/') + 1)..].Replace('_', ' '));
    internal static string TrackLabel(string id)
    {
        var separator = id.LastIndexOf('/');
        return $"{LocationLabel(id[..separator])} — {LocationLabel(id[(separator + 1)..])}";
    }

    internal async Task<HeadsetResolution> ReadHeadsetResolution(string runtime) => HeadsetResolution.Parse(
        await ReadCommand(Command("Get-VrResolution.ps1", "-Runtime", runtime), TimeSpan.FromSeconds(25)), runtime);

    private static async Task<string> ReadCommand(ProcessStartInfo info, TimeSpan? timeout = null)
    {
        info.RedirectStandardOutput = info.RedirectStandardError = true;
        using var process = Process.Start(info) ?? throw new IOException("Could not start command.");
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        using var cancellation = new CancellationTokenSource(timeout ?? Timeout.InfiniteTimeSpan);
        try { await process.WaitForExitAsync(cancellation.Token); }
        catch (OperationCanceledException) {
            if (!process.HasExited) process.Kill(entireProcessTree: true);
            throw new IOException("Headset query timed out. Connect the headset and refresh.");
        }
        var text = await output; var failure = await error;
        if (process.ExitCode != 0) throw new IOException(string.IsNullOrWhiteSpace(failure) ? text : failure);
        return text;
    }
}
