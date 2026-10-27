namespace GRID2VR;

internal sealed record GraphicsOptions(int RenderScale = 100, bool DesktopVSync = true, string DesktopMsaa = "off",
    string TreeDetail = "ultra", string ObjectDetail = "ultra", string CrowdDetail = "ultra", string TextureFiltering = "ultra", int HeadsetScale = 100, string VrMsaa = "off")
{
    // Retain legacy 720p-based fields for rollback and desktop migration. They
    // must never be interpreted as percentages of a headset recommendation.
    public bool UseVrResolutionPreset { get; init; }
    public int VrRenderScale { get; init; } = 100;
    public int? DesktopRenderScale { get; init; }
    public static GraphicsOptions Defaults => new();
    [System.Text.Json.Serialization.JsonIgnore]
    public int EffectiveDesktopRenderScale => DesktopRenderScale ?? RenderScale;
    public bool VrMirrorBorderless { get; init; }
    public decimal HudDistance { get; init; } = 1m;
    public bool HudFollowView { get; init; }
    public bool HudGauges { get; init; }
    public bool HudLapTime { get; init; } = true;
    public bool HudPosition { get; init; } = true;
    public bool HudMap { get; init; } = true;
    public bool HudProgress { get; init; } = true;
    [System.Text.Json.Serialization.JsonIgnore]
    public int HiddenHudElements => (HudGauges ? 0 : 1) | (HudLapTime ? 0 : 2) | (HudPosition ? 0 : 4) | (HudMap ? 0 : 8) | (HudProgress ? 0 : 16);

    public void Validate()
    {
        if (VrRenderScale is < 50 or > 300) throw new ArgumentException("VR render scale must be 50–300%.");
        if (RenderScale is < 50 or > 300 || DesktopRenderScale is < 50 or > 300 || HeadsetScale is < 25 or > 200 || HeadsetScale % 25 != 0 || DesktopMsaa is not ("off" or "2xmsaa" or "4xmsaa" or "8xmsaa") || VrMsaa is not ("off" or "2xmsaa" or "4xmsaa" or "8xmsaa") ||
            TreeDetail is not ("medium" or "high" or "ultra") || ObjectDetail is not ("medium" or "high" or "ultra") ||
            CrowdDetail is not ("low" or "medium" or "high" or "ultra") || TextureFiltering is not ("off" or "low" or "high" or "ultra"))
            throw new ArgumentException("Invalid graphics settings. Restore graphics defaults or choose a listed option.");
        if (HudDistance is < 1m or > 20m || HudDistance * 2m != decimal.Truncate(HudDistance * 2m))
            throw new ArgumentException("HUD distance must be 1–20 metres in 0.5 m steps.");
    }

    public string SceneLabel => $"{1280 * RenderScale / 100} × {720 * RenderScale / 100}";
    public string HeadsetLabel => $"{1280 * HeadsetScale / 100} × {720 * HeadsetScale / 100} per eye";
}
