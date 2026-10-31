using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;

namespace GRID2VR;

// An implicit OpenXR API layer (ReShade, OBS Mirror, ...) that GRID2VR may disable for its
// own VR processes. Identity is read by tools/OpenXR-Layers.ps1, which also revalidates it
// against the installed registration before every use.
internal sealed record VrLayerBypass(string LayerName, string ManifestPath, string ManifestHash, string LibraryPath, string DisableVariable)
{
    [JsonIgnore] public string DisplayName => LayerName switch
    {
        "XR_APILAYER_reshade" => "ReShade",
        "XR_APILAYER_NOVENDOR_OBSMirror" => "OpenXR OBS Mirror",
        _ => LayerName
    };
    [JsonIgnore] public string Consequence => LayerName switch
    {
        "XR_APILAYER_reshade" => "ReShade OpenXR effects will be unavailable.",
        "XR_APILAYER_NOVENDOR_OBSMirror" => "OBS Mirror's OpenXR capture will be unavailable.",
        _ => "Features provided by this OpenXR layer will be unavailable."
    };
    public override string ToString() => DisplayName;
    internal bool SameIdentity(VrLayerBypass? other) => other is not null && LayerName == other.LayerName && ManifestHash == other.ManifestHash &&
        DisableVariable == other.DisableVariable && string.Equals(ManifestPath, other.ManifestPath, StringComparison.OrdinalIgnoreCase) &&
        string.Equals(LibraryPath, other.LibraryPath, StringComparison.OrdinalIgnoreCase);
    // Never let a manifest turn a bypass into PATH, runtime, or other process configuration.
    internal static bool SafeDisableVariable(string? value) => value is not null && Regex.IsMatch(value, @"\ADISABLE_(?:XR_|OPENXR_)[A-Za-z0-9_]{1,180}\z");
    internal bool Valid => LayerName is not null && Regex.IsMatch(LayerName, @"\AXR_APILAYER_[A-Za-z0-9_]{1,200}\z") && SafeDisableVariable(DisableVariable) &&
        ManifestHash is not null && Regex.IsMatch(ManifestHash, @"\A[0-9A-F]{64}\z") &&
        ManifestPath is not null && Path.IsPathFullyQualified(ManifestPath) && LibraryPath is not null && Path.IsPathFullyQualified(LibraryPath);
}

internal sealed record VrLayerRecovery(string Message, IReadOnlyList<VrLayerBypass> Layers, string Report);
internal sealed record VrLayerRecoveryChoice(bool Retry, bool Remember);

// Get-VrResolution.ps1 result. Recovery names layers the player may disable; Failure is any
// other problem, left for the session worker to report and log in full.
internal sealed record VrHeadsetPreflight(IReadOnlyList<VrLayerBypass> Applied, VrLayerRecovery? Recovery = null, string? Failure = null)
{
    internal static VrHeadsetPreflight Parse(int exitCode, string output, string error, string runtime, IReadOnlyList<VrLayerBypass> requested)
    {
        if (exitCode is not (0 or 3)) return new(requested, Failure: string.IsNullOrWhiteSpace(error) ? output.Trim() : error.Trim());
        if (output.Length > 512 * 1024) throw new IOException("Headset check returned too much data.");
        try
        {
            using var document = JsonDocument.Parse(output);
            var root = document.RootElement;
            var applied = VrApiLayers.Read(root, "layerBypasses");
            if (exitCode == 0) { HeadsetResolution.Parse(output, runtime); return new(applied); }
            var layers = VrApiLayers.Read(root, "layers");
            if (layers.Count == 0) throw new IOException("Invalid OpenXR layer report.");
            return new(applied, new(root.GetProperty("message").GetString() ?? "", layers, (root.GetProperty("report").GetString() ?? "").ReplaceLineEndings("\r\n")));
        }
        catch (Exception failure) when (failure is JsonException or InvalidOperationException or KeyNotFoundException)
        { throw new IOException("Invalid headset check result.", failure); }
    }
}

// DiRT2VR's recovery: when an OpenXR add-on blocks the headset check, name it, disable it for
// GRID2VR only after the player agrees, and remember that only by choice after a working retry.
// Any other headset failure is left to the session worker, which reports and logs it.
internal static class VrLayerRecoveryFlow
{
    internal static async Task<(IReadOnlyList<VrLayerBypass> Layers, string Warning)?> Check(IReadOnlyList<VrLayerBypass> remembered,
        Func<IReadOnlyList<VrLayerBypass>, Task<VrHeadsetPreflight>> preflight, Func<VrLayerRecovery, VrLayerRecoveryChoice?> confirm,
        Action<IReadOnlyList<VrLayerBypass>> remember, Action<string>? progress = null)
    {
        var first = await preflight(remembered);
        if (first.Recovery is not { } recovery) return (first.Applied, "");
        progress?.Invoke("Review the VR compatibility prompt to continue…");
        if (confirm(recovery) is not { Retry: true } choice) return null;
        progress?.Invoke("Checking the VR headset…");
        var approved = first.Applied.Concat(recovery.Layers).ToArray();
        var retry = await preflight(approved);
        if (retry.Recovery is not null || retry.Failure is not null)
            throw new IOException($"VR still could not start after disabling {string.Join(", ", recovery.Layers.Select(layer => layer.DisplayName))}. " +
                $"{retry.Recovery?.Message ?? retry.Failure}\nOriginal failure: {recovery.Message}");
        // Re-read after the dialog: never apply approval to a registration changed while it was open.
        if (retry.Applied.Count != approved.Length || !approved.All(layer => retry.Applied.Any(layer.SameIdentity)))
            throw new IOException("The OpenXR layer registration changed. Launch VR again to review it.");
        if (!choice.Remember) return (retry.Applied, "");
        try { remember(recovery.Layers); return (retry.Applied, ""); }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException)
        { return (retry.Applied, "The OpenXR layer bypass works for this session, but could not be remembered: " + error.Message); }
    }
}

internal static class VrApiLayers
{
    internal const int MaxLayers = 16;
    private static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase, PropertyNameCaseInsensitive = true, WriteIndented = true };

    internal static string Encode(IReadOnlyList<VrLayerBypass> layers)
    {
        if (layers.Count > MaxLayers || layers.Any(layer => !layer.Valid)) throw new IOException("Invalid OpenXR layer bypass.");
        var text = Convert.ToBase64String(JsonSerializer.SerializeToUtf8Bytes(layers, Json));
        return text.Length <= 16384 ? text : throw new IOException("Too many OpenXR layer bypasses. Re-enable some under Settings → VR compatibility.");
    }

    internal static List<VrLayerBypass> Read(JsonElement root, string property)
    {
        if (!root.TryGetProperty(property, out var value) || value.ValueKind == JsonValueKind.Null) return [];
        var layers = value.ValueKind == JsonValueKind.Array ? value.Deserialize<List<VrLayerBypass>>(Json) : [value.Deserialize<VrLayerBypass>(Json)!];
        if (layers is null || layers.Count > MaxLayers || layers.Any(layer => layer is null || !layer.Valid)) throw new IOException("Invalid OpenXR layer report.");
        return layers;
    }

    internal static string PreferencesPath(string data) => Path.Combine(data, "vr-layer-bypasses.json");

    // Invalid or unreadable preferences grant no permission to bypass a layer.
    internal static List<VrLayerBypass> Remembered(string data)
    {
        try
        {
            var path = PreferencesPath(data);
            if (!File.Exists(path) || new FileInfo(path).Length > 64 * 1024) return [];
            Updates.PlainPath(path);
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            if (!document.RootElement.TryGetProperty("version", out var version) || version.ValueKind != JsonValueKind.Number || version.GetInt32() != 1) return [];
            return Read(document.RootElement, "bypasses");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException or FormatException) { return []; }
    }

    internal static void Remember(string data, IEnumerable<VrLayerBypass> layers)
    {
        var saved = Remembered(data);
        foreach (var layer in layers) { saved.RemoveAll(item => item.LayerName == layer.LayerName); saved.Add(layer); }
        Save(data, saved);
    }

    internal static void Reenable(string data, string layerName)
    {
        var saved = Remembered(data);
        saved.RemoveAll(item => item.LayerName == layerName);
        Save(data, saved);
    }

    private static void Save(string data, List<VrLayerBypass> layers)
    {
        if (layers.Count > MaxLayers || layers.Any(layer => !layer.Valid)) throw new IOException("Invalid OpenXR layer bypass.");
        Directory.CreateDirectory(data);
        var path = PreferencesPath(data);
        Updates.PlainPath(path);
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try { File.WriteAllText(temporary, JsonSerializer.Serialize(new { version = 1, bypasses = layers }, Json)); File.Move(temporary, path, true); }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
}
