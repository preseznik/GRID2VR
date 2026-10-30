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
    internal const string RuntimeNotice = "Enabled bindings and transmission apply on the next Desktop or VR launch. Keyboard, controller, wheel and shifter work together. H-pattern uses Gear 1–7/Reverse; + clutch also uses Clutch. Unassigned controls keep game bindings.";
    internal const int MaxDevices = 16;
    internal const int MaxFileBytes = 524288;
    public int Version { get; set; } = 2;
    public bool Enabled { get; set; }
    public bool ListenAllDevices { get; set; } = true;
    public string Transmission { get; set; } = "game";
    public int SteeringLinearity { get; set; }
    public int ClutchShiftThreshold { get; set; } = 50;
    // The device claimed for driving (and with it rumble/force feedback) when the
    // game's Controls preset leaves the player on the keyboard: Practice/Race skip
    // Press Start, and Normal's Press Start finds no preset for many wheels.
    // "auto" claims the steering-binding device, then an Xbox pad; "keyboard"
    // claims nothing; "xinput_pad" or a DirectInput product GUID names one family.
    public string DirectLaunchDevice { get; set; } = "auto";
    public List<DrivingBinding> Bindings { get; set; } = [];
    internal static readonly string[] Transmissions = ["game", "automatic", "sequential", "hpattern", "hpattern_clutch"];
    public static readonly string[] Actions = ["Steer Left", "Steer Right", "Accelerate", "Brake", "Clutch", "Handbrake",
        "Gear Up", "Gear Down", "Gear 1", "Gear 2", "Gear 3", "Gear 4", "Gear 5", "Gear 6", "Gear 7", "Gear Reverse",
        "Change View", "Look Back", "Look Left", "Look Right", "Look Up", "Look Down", "Horn", "Boost", "Pause", "Instant Replay",
        "Menu Start Button", "Menu Select", "Menu Back", "Menu Up", "Menu Down", "Menu Left", "Menu Right",
        "Replay Rewind", "Replay Fast Forward", "Replay Pause", "Replay Next Camera", "Replay Prev Camera", "Replay Jump In", "Replay Exit", "Replay UI On Off"];
    internal static readonly string[] WizardActions = ["Steer Left", "Steer Right", "Accelerate", "Brake", "Clutch", "Handbrake",
        "Gear Up", "Gear Down", "Gear 1", "Gear 2", "Gear 3", "Gear 4", "Gear 5", "Gear 6", "Gear 7", "Gear Reverse", "Change View", "Look Back", "Pause"];
    public static readonly string[] Calibrations = ["uniDirPos", "uniDirNeg", "biDirLower", "biDirUpper"];
    internal static readonly string[] XboxAxes = ["xinput_left_stick_x", "xinput_left_stick_y", "xinput_right_stick_x", "xinput_right_stick_y", "xinput_left_trigger", "xinput_right_trigger"];
    internal static readonly string[] DirectAxes = ["di_x_axis", "di_y_axis", "di_z_axis", "di_x_axis_rotation", "di_y_axis_rotation", "di_z_axis_rotation", "di_slider_0", "di_slider_1"];
    internal static readonly string[] XboxButtons = ["xinput_dpad_up", "xinput_dpad_down", "xinput_dpad_left", "xinput_dpad_right", "xinput_button_start", "xinput_button_back", "xinput_button_left_stick", "xinput_button_right_stick", "xinput_button_left_shoulder", "xinput_button_right_shoulder", "", "", "xinput_button_a", "xinput_button_b", "xinput_button_x", "xinput_button_y"];
    private static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true, WriteIndented = true };

    public static string SettingsPath(string root) => Path.Combine(AppPaths.Data(Path.GetFullPath(root)), "driving-controls.json");
    public static string ProfilesPath(string root) => Path.Combine(AppPaths.Data(Path.GetFullPath(root)), "driving-profiles");
    public static DrivingControls Load(string root)
    {
        var path = SettingsPath(root);
        return File.Exists(path) ? LoadFile(path) : new();
    }
    public static DrivingControls LoadFile(string path)
    {
        using var stream = File.OpenRead(path);
        if (stream.Length is < 1 or > MaxFileBytes) throw new IOException("Driving controls file is empty or too large.");
        var bytes = new byte[checked((int)stream.Length)]; stream.ReadExactly(bytes);
        if (stream.ReadByte() != -1) throw new IOException("Driving controls file changed while reading.");
        try
        {
            ReadOnlySpan<byte> json = bytes;
            if (json.StartsWith(new byte[] { 0xef, 0xbb, 0xbf })) json = json[3..];
            var value = JsonSerializer.Deserialize<DrivingControls>(json, Json) ?? throw new IOException("Invalid driving controls file.");
            value.Validate();
            value.Version = 2;
            return value;
        }
        catch (JsonException ex) { throw new IOException("Invalid driving controls JSON.", ex); }
    }
    public void Save(string root) => SaveFile(SettingsPath(root));
    public void SaveFile(string path)
    {
        Validate();
        if (Enabled && Problems() is { Count: > 0 } problems) throw new IOException(string.Join(Environment.NewLine, problems));
        var value = new DrivingControls { Enabled = Enabled, ListenAllDevices = ListenAllDevices, Transmission = Transmission, SteeringLinearity = SteeringLinearity,
            ClutchShiftThreshold = ClutchShiftThreshold, DirectLaunchDevice = DirectLaunchDevice, Bindings = [.. Bindings] };
        var bytes = JsonSerializer.SerializeToUtf8Bytes(value, Json);
        if (bytes.Length > MaxFileBytes) throw new IOException("Driving controls file is too large.");
        path = Path.GetFullPath(path);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try { File.WriteAllBytes(temporary, bytes); File.Move(temporary, path, true); }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
    public void Validate()
    {
        if (Version is not (1 or 2) || Bindings is null || Bindings.Count > Actions.Length * MaxDevices) throw new IOException("Invalid driving controls configuration.");
        if (!Transmissions.Contains(Transmission)) throw new IOException("Invalid transmission setting.");
        if (SteeringLinearity is < -10 or > 10) throw new IOException("Wheel steering linearity must be between -10 and 10.");
        if (ClutchShiftThreshold is < 10 or > 100) throw new IOException("Clutch shift threshold must be between 10% and 100%.");
        if (DirectLaunchDevice is not ("auto" or "keyboard" or "xinput_pad") && !(DirectLaunchDevice is { Length: 38 } && Guid.TryParseExact(DirectLaunchDevice, "B", out _)))
            throw new IOException("Invalid direct-launch controls device.");
        var slots = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var devices = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var b in Bindings)
        {
            if (b is null || !Actions.Contains(b.Action) || !Calibrations.Contains(b.Calibration) ||
                string.IsNullOrWhiteSpace(b.Device) || b.Device.Length > 260 || b.Device.Any(char.IsControl) ||
                string.IsNullOrWhiteSpace(b.DeviceId) || b.DeviceId.Length > 128 || b.DeviceId.Any(char.IsControl) ||
                b.DeadZone is < 0 or > .9m || b.Saturation is < .1m or > 1 || b.DeadZone >= b.Saturation || (float)b.DeadZone >= (float)b.Saturation || !ValidInput(b))
                throw new IOException("Invalid driving binding. Check device, input and calibration.");
            if (!slots.Add(b.DeviceId + "/" + b.Action)) throw new IOException("Duplicate binding for " + b.Action + " on " + b.Device + ".");
            if (devices.TryGetValue(b.DeviceId, out var family) && !StringComparer.OrdinalIgnoreCase.Equals(family, b.DeviceType))
                throw new IOException("A device has conflicting controller types.");
            devices[b.DeviceId] = b.DeviceType;
        }
        if (devices.Count > MaxDevices) throw new IOException($"Driving controls support up to {MaxDevices} devices per profile.");
    }
    private static bool ValidInput(DrivingBinding b)
    {
        if (b.Input is null || b.DeviceType is null) return false;
        if (b.Keyboard) return b.DeviceId == "keyboard" && Regex.IsMatch(b.Input, @"\Avk_code_0x[0-9A-F]{2}\z") &&
            int.TryParse(b.Input.AsSpan(10), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var key) && key is >= 8 and <= 254;
        if (b.DeviceType == "xinput_pad") return Regex.IsMatch(b.DeviceId, @"\Axinput:[0-3]\z") &&
            (XboxAxes.Contains(b.Input) || XboxButtons.Contains(b.Input) && b.Input.Length > 0);
        return b.DeviceType.Length == 38 && b.DeviceId.Length == 38 && Guid.TryParseExact(b.DeviceType, "B", out _) && Guid.TryParseExact(b.DeviceId, "B", out _) &&
            (DirectAxes.Contains(b.Input) || Regex.IsMatch(b.Input, @"\Adi_dpad_[0-3]_(up|down|left|right)\z") ||
             Regex.IsMatch(b.Input, @"\Adi_button_([0-9]|[1-9][0-9]|1[01][0-9]|12[0-7])\z"));
    }
    public List<string> Problems()
    {
        var errors = new List<string>();
        foreach (var device in Bindings.GroupBy(b => b.DeviceId, StringComparer.OrdinalIgnoreCase))
        {
            var left = device.FirstOrDefault(b => b.Action == "Steer Left");
            var right = device.FirstOrDefault(b => b.Action == "Steer Right");
            if (left is not null && right is not null && left.Input == right.Input && left.Calibration == right.Calibration)
                errors.Add("Left and right steering use the same direction. Rebind steering.");
        }
        foreach (var b in Bindings)
            if (b.DeviceType == "xinput_pad" && b.Input.EndsWith("_trigger", StringComparison.Ordinal) && b.Calibration != "uniDirPos")
                errors.Add(b.Action + ": Xbox triggers must increase when pressed.");
        return errors;
    }
    public void Assign(DrivingBinding binding)
    {
        Bindings.RemoveAll(b => b.Action == binding.Action && StringComparer.OrdinalIgnoreCase.Equals(b.DeviceId, binding.DeviceId));
        Bindings.Add(binding);
    }
    public void ApplyXboxPreset() { foreach (var binding in XboxPreset()) Assign(binding); }
    // Session worker spec for every launch mode (tools/Driving-Controls.ps1). Wheels
    // precede an Xbox pad, as in the game's own input/paths.xml priorities.
    internal string? DeviceClaimSpec()
    {
        Validate();
        if (DirectLaunchDevice == "keyboard") return null;
        if (DirectLaunchDevice != "auto") return "device;" + DirectLaunchDevice;
        var steering = Enabled ? Bindings.Where(b => b.Action is "Steer Left" or "Steer Right" && !b.Keyboard).Select(b => b.DeviceType)
            .Distinct(StringComparer.OrdinalIgnoreCase).OrderBy(type => type == "xinput_pad").Take(8) : [];
        return string.Join(";", steering.Prepend("auto"));
    }
    // Both the product family and physical instance are required by the session reader.
    public XDocument ToXml()
    {
        Validate();
        var root = new XElement("DrivingControls", new XAttribute("version", 4), new XAttribute("transmission", Transmission),
            new XAttribute("steering_linearity", SteeringLinearity.ToString(CultureInfo.InvariantCulture)),
            new XAttribute("clutch_shift_threshold", ClutchShiftThreshold.ToString(CultureInfo.InvariantCulture)));
        foreach (var group in Bindings.GroupBy(b => b.DeviceId, StringComparer.OrdinalIgnoreCase))
        {
            var map = new XElement("ActionMap", new XAttribute("device_type", group.First().DeviceType), new XAttribute("device_id", group.Key));
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
        Add("Steer Left", "left_stick_x", "biDirLower"); Add("Steer Right", "left_stick_x", "biDirUpper");
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
