using System.Text.Json;

namespace GRID2VR;

internal sealed record HeadsetResolution(string Runtime, string Headset, int Width, int Height)
{
    public string RuntimeName { get; init; } = "";
    private static bool SceneSupported(int width, int height) => width > 0 && height > 0 && width <= 8192 && height <= 8192 && (long)width * height <= 64 * 1024 * 1024;
    private static bool OutputSupported(int width, int height) => SceneSupported(width, height) && (long)width * height <= 16 * 1024 * 1024;
    internal int MaximumRenderScale => MaximumRenderScaleAt(100, 100);
    // The scene shrinks with the field of view, matching the renderer's integer arithmetic.
    private static int Scene(int size, int scale, int fov) => (int)((long)size * scale * fov / 10000);
    internal int MaximumRenderScaleAt(int fovX, int fovY) => Enumerable.Range(50, 251).LastOrDefault(scale => SceneSupported(Scene(Width, scale, fovX), Scene(Height, scale, fovY)));
    internal static HeadsetResolution Parse(string json, string runtime)
    {
        var value = JsonSerializer.Deserialize<HeadsetResolution>(json, new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
        if (value is null || value.Runtime != runtime || string.IsNullOrWhiteSpace(value.Headset) || value.Headset.Length > 256 || value.RuntimeName is null || value.RuntimeName.Length > 128 || !OutputSupported(value.Width, value.Height))
            throw new IOException("Headset query returned an invalid resolution.");
        return value;
    }
    internal string Summary(int scale, int fovX = 100, int fovY = 100)
    {
        var width = Scene(Width, scale, fovX); var height = Scene(Height, scale, fovY);
        var provider = Runtime == "Default" ? "Default OpenXR" + (RuntimeName.Length == 0 ? "" : $" ({RuntimeName})") : Runtime;
        var output = $"{Headset} · {provider}\nOutput: {Width} × {Height} per eye (100% baseline).";
        var crop = fovX == fovY ? $"{fovY}% field of view" : fovX == 100 ? $"{fovY}% field of view, top and bottom" : $"{fovX}% × {fovY}% field of view";
        var render = $"Render: {width} × {height} per eye at {scale}%" + (fovY < 100 || fovX < 100 ? $", {crop}." : ".");
        return output + "\n" + render + $"\nMaximum supported at this headset resolution: {MaximumRenderScaleAt(fovX, fovY)}%." +
            (SceneSupported(width, height) && width >= 320 && height >= 180 ? "" : width < 320 || height < 180
                ? "\nBelow the renderer minimum. Raise VR render scale or field of view before launching."
                : "\nExceeds renderer limits. Lower VR render scale before launching.");
    }
}
