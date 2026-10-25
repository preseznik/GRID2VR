using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;
using System.Xml.Linq;

namespace GRID2VR;

internal sealed record DrivingBinding
{
    public string Action { get; init; } = "";
    public string DeviceId { get; init; } = "";
    public string Device { get; init; } = "";
    public string DeviceType { get; init; } = "";
    public string Input { get; init; } = "";
    public string Calibration { get; init; } = "uniDirPos";
    public decimal DeadZone { get; init; }
    public decimal Saturation { get; init; } = 1;
    [JsonIgnore] public bool Keyboard => DeviceType == "keyboard";
}

// GRID 2's names and calibration vocabulary come from its shipped input/*.xml.
// These deliberately differ from the older DiRT 2 schema.
internal sealed class DrivingControls
{
    internal const string RuntimeNotice = "Enable launcher driving bindings to use your saved controls for Desktop and VR launches. Changes apply on the next launch. The game's saved bindings remain unchanged.";
    public int Version { get; set; } = 1;
    public bool Enabled { get; set; }
    public List<DrivingBinding> Bindings { get; set; } = [];
    public static readonly string[] Actions = ["Steer Left", "Steer Right", "Accelerate", "Brake", "Clutch", "Handbrake",
        "Gear Up", "Gear Down", "Gear 1", "Gear 2", "Gear 3", "Gear 4", "Gear 5", "Gear 6", "Gear 7", "Gear Reverse",
        "Change View", "Look Back", "Look Left", "Look Right", "Look Up", "Look Down", "Horn", "Boost", "Pause", "Instant Replay",
        "Menu Start Button", "Menu Select", "Menu Back", "Menu Up", "Menu Down", "Menu Left", "Menu Right",
        "Replay Rewind", "Replay Fast Forward", "Replay Pause", "Replay Next Camera", "Replay Prev Camera", "Replay Jump In", "Replay Exit", "Replay UI On Off"];
    public static readonly string[] Calibrations = ["uniDirPos", "uniDirNeg", "biDirLower", "biDirUpper"];
    internal static readonly string[] XboxAxes = ["xinput_left_stick_x", "xinput_left_stick_y", "xinput_right_stick_x", "xinput_right_stick_y", "xinput_left_trigger", "xinput_right_trigger"];
    internal static readonly string[] DirectAxes = ["di_x_axis", "di_y_axis", "di_z_axis", "di_x_axis_rotation", "di_y_axis_rotation", "di_z_axis_rotation", "di_slider_0", "di_slider_1"];
    internal static readonly string[] XboxButtons = ["xinput_dpad_up", "xinput_dpad_down", "xinput_dpad_left", "xinput_dpad_right", "xinput_button_start", "xinput_button_back", "xinput_button_left_stick", "xinput_button_right_stick", "xinput_button_left_shoulder", "xinput_button_right_shoulder", "", "", "xinput_button_a", "xinput_button_b", "xinput_button_x", "xinput_button_y"];
    private static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true, WriteIndented = true };

    public static string SettingsPath(string root) => Path.Combine(AppPaths.Data(Path.GetFullPath(root)), "driving-controls.json");
    public static DrivingControls Load(string root)
    {
        var path = SettingsPath(root);
        if (!File.Exists(path)) return new();
        if (new FileInfo(path).Length > 131072) throw new IOException("Driving controls file is too large.");
        var value = JsonSerializer.Deserialize<DrivingControls>(File.ReadAllText(path), Json) ?? throw new IOException("Invalid driving controls file.");
        value.Validate();
        return value;
    }
    public void Save(string root)
    {
        Validate();
        if (Enabled && Problems() is { Count: > 0 } problems) throw new IOException(string.Join(Environment.NewLine, problems));
        var path = SettingsPath(root);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try { File.WriteAllText(temporary, JsonSerializer.Serialize(this, Json)); File.Move(temporary, path, true); }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
    public void Validate()
    {
        if (Version != 1 || Bindings is null || Bindings.Count > Actions.Length * 2) throw new IOException("Invalid driving controls configuration.");
        var slots = new HashSet<(string, bool)>();
        foreach (var b in Bindings)
        {
            if (b is null || !Actions.Contains(b.Action) || !Calibrations.Contains(b.Calibration) ||
                string.IsNullOrWhiteSpace(b.Device) || b.Device.Length > 260 || b.Device.Any(char.IsControl) ||
                string.IsNullOrWhiteSpace(b.DeviceId) || b.DeviceId.Length > 128 || b.DeviceId.Any(char.IsControl) ||
                b.DeadZone is < 0 or > .9m || b.Saturation is < .1m or > 1 || b.DeadZone >= b.Saturation || !ValidInput(b))
                throw new IOException("Invalid driving binding. Check device, input and calibration.");
            if (!slots.Add((b.Action, b.Keyboard))) throw new IOException("Duplicate binding for " + b.Action + ".");
        }
    }
    private static bool ValidInput(DrivingBinding b)
    {
        if (b.Input is null || b.DeviceType is null) return false;
        if (b.Keyboard) return b.DeviceId == "keyboard" && Regex.IsMatch(b.Input, @"\Avk_code_0x[0-9A-F]{2}\z") &&
            int.TryParse(b.Input.AsSpan(10), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var key) && key is >= 8 and <= 254;
        if (b.DeviceType == "xinput_pad") return Regex.IsMatch(b.DeviceId, @"\Axinput:[0-3]\z") &&
            (XboxAxes.Contains(b.Input) || XboxButtons.Contains(b.Input) && b.Input.Length > 0);
        return Guid.TryParseExact(b.DeviceType, "B", out _) && Guid.TryParseExact(b.DeviceId, "B", out _) &&
            (DirectAxes.Contains(b.Input) || Regex.IsMatch(b.Input, @"\Adi_dpad_[0-3]_(up|down|left|right)\z") ||
             Regex.IsMatch(b.Input, @"\Adi_button_([0-9]|[1-9][0-9]|1[01][0-9]|12[0-7])\z"));
    }
    public List<string> Problems()
    {
        var errors = new List<string>();
        foreach (var keyboard in new[] { false, true })
        {
            var left = Bindings.FirstOrDefault(b => b.Action == "Steer Left" && b.Keyboard == keyboard);
            var right = Bindings.FirstOrDefault(b => b.Action == "Steer Right" && b.Keyboard == keyboard);
            if (left is not null && right is not null && left.DeviceType == right.DeviceType && left.Input == right.Input && left.Calibration == right.Calibration)
                errors.Add("Left and right steering use the same direction. Rebind steering.");
        }
        foreach (var b in Bindings)
            if (b.DeviceType == "xinput_pad" && b.Input.EndsWith("_trigger", StringComparison.Ordinal) && b.Calibration != "uniDirPos")
                errors.Add(b.Action + ": Xbox triggers must increase when pressed.");
        return errors;
    }
    public void Assign(DrivingBinding binding)
    {
        Bindings.RemoveAll(b => b.Action == binding.Action && b.Keyboard == binding.Keyboard);
        Bindings.Add(binding);
    }
    // Native device_type selects product families, not controller slot/instance.
    // DeviceId is retained for capture and diagnostics, not presented as routing isolation.
    public XDocument ToXml()
    {
        Validate();
        var root = new XElement("DrivingControls", new XAttribute("version", 1));
        foreach (var group in Bindings.GroupBy(b => b.DeviceType))
        {
            var map = new XElement("ActionMap", new XAttribute("device_type", group.Key));
            foreach (var b in group)
                map.Add(new XElement("Action", new XAttribute("id", b.Action), new XElement("Axis", new XAttribute("id", b.Input),
                    new XAttribute("type", b.Calibration), new XAttribute("deadzone", b.DeadZone.ToString(CultureInfo.InvariantCulture)),
                    new XAttribute("saturation", b.Saturation.ToString(CultureInfo.InvariantCulture)))));
            root.Add(map);
        }
        return new(root);
    }
    public static List<DrivingBinding> XboxPreset()
    {
        var result = new List<DrivingBinding>();
        void Add(string action, string input, string calibration = "uniDirPos", decimal dead = 0) => result.Add(new()
            { Action = action, Device = "Xbox controller", DeviceId = "xinput:0", DeviceType = "xinput_pad", Input = "xinput_" + input, Calibration = calibration, DeadZone = dead });
        Add("Steer Left", "left_stick_x", "biDirLower", .2m); Add("Steer Right", "left_stick_x", "biDirUpper", .2m);
        Add("Accelerate", "right_trigger"); Add("Brake", "left_trigger"); Add("Handbrake", "button_b");
        Add("Gear Up", "button_x"); Add("Gear Down", "button_a"); Add("Change View", "button_right_shoulder");
        Add("Look Back", "button_right_stick"); Add("Boost", "button_left_shoulder"); Add("Pause", "button_start"); Add("Instant Replay", "button_y");
        Add("Menu Start Button", "button_start"); Add("Menu Select", "button_a"); Add("Menu Back", "button_b");
        foreach (var direction in new[] { "Up", "Down", "Left", "Right" }) Add("Menu " + direction, "dpad_" + direction.ToLowerInvariant());
        Add("Replay Rewind", "left_trigger"); Add("Replay Fast Forward", "right_trigger"); Add("Replay Pause", "button_a");
        Add("Replay Next Camera", "button_right_shoulder"); Add("Replay Prev Camera", "button_left_shoulder");
        Add("Replay Jump In", "button_x"); Add("Replay Exit", "button_b"); Add("Replay UI On Off", "button_back");
        return result;
    }
    public static string? KeyInput(Keys key)
    {
        var value = (int)(key & Keys.KeyCode);
        if (value is < 8 or > 254 || key is Keys.LWin or Keys.RWin or Keys.Apps || (key & Keys.Modifiers) != 0) return null;
        return "vk_code_0x" + value.ToString("X2", CultureInfo.InvariantCulture);
    }
}
