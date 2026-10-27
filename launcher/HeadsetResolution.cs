using System.Text.Json;

namespace GRID2VR;

internal sealed record HeadsetResolution(string Runtime, string Headset, int Width, int Height)
{
    private static bool SceneSupported(int width, int height) => width > 0 && height > 0 && width <= 8192 && height <= 8192 && (long)width * height <= 64 * 1024 * 1024;
    private static bool OutputSupported(int width, int height) => SceneSupported(width, height) && (long)width * height <= 16 * 1024 * 1024;
    internal int MaximumRenderScale => Enumerable.Range(50, 251).LastOrDefault(scale => SceneSupported(Width * scale / 100, Height * scale / 100));
    internal static HeadsetResolution Parse(string json, string runtime)
    {
        var value = JsonSerializer.Deserialize<HeadsetResolution>(json, new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
        if (value is null || value.Runtime != runtime || string.IsNullOrWhiteSpace(value.Headset) || value.Headset.Length > 256 || !OutputSupported(value.Width, value.Height))
            throw new IOException("Headset query returned an invalid resolution.");
        return value;
    }
    internal string Summary(int scale)
    {
        var width = Width * scale / 100; var height = Height * scale / 100;
        var output = $"{Headset} · {Runtime}\nOutput: {Width} × {Height} per eye (100% baseline).";
        var render = $"Render: {width} × {height} per eye at {scale}%.";
        return output + "\n" + render + $"\nMaximum supported at this headset resolution: {MaximumRenderScale}%." +
            (SceneSupported(width, height) && width >= 320 && height >= 180 ? "" : "\nExceeds renderer limits. Lower VR render scale before launching.");
    }
}
