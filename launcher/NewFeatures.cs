using System.Runtime.CompilerServices;

namespace GRID2VR;

// "*new*" marks on launcher features added since the last release (iztok's d7611e2c, as in DIRT3VR),
// drawn in the launcher's red accent. A mark shows in development builds before the release that
// first ships it and in that release's Experimental builds; Stable releases never show marks, and
// the next version drops them.
internal static class NewFeatures
{
    public const string Mark = "*new*";
    // The next release. After each release, set the next version and remove the released entries
    // (NewFeaturesTests fails once this build is past NextVersion). Marks should stay until the next
    // Stable (ProJict 93f6bbb7), so until that rule lands, 0.2.16's entries carry forward.
    public const string NextVersion = "0.2.23";

    // Control or section names, and the version that first marks them. 0.2.15 shipped before the
    // marks existed, so its launcher additions are marked in 0.2.16.
    private static readonly Dictionary<string, string> Since = new(StringComparer.Ordinal)
    {
        ["NativeLeftEye"] = NextVersion, ["LastVrPerformance"] = NextVersion, ["RecommendGraphics"] = NextVersion,
        ["SectionVRcompatibility"] = NextVersion, ["DrivingControls"] = NextVersion, ["VrFieldOfView"] = NextVersion, ["VrFieldOfViewCrop"] = NextVersion,
        ["LaunchMode"] = NextVersion,
        ["Runtime"] = NextVersion,
        ["SponsorDepthBias"] = NextVersion,
        ["CockpitDepthPrecision"] = NextVersion,
        ["DesktopSeatPosition"] = NextVersion, ["Desktop cockpit FOV"] = NextVersion, ["InGameSeatAdjustment"] = NextVersion,
        ["ForceFeedbackOverride"] = NextVersion,
        ["ChaseHorizontalOnly"] = NextVersion,
        ["Conditions"] = NextVersion, ["HideLensDust"] = NextVersion, ["CustomTracks"] = NextVersion,
    };
    private static readonly ConditionalWeakTable<Control, object> marked = new(), painting = new();

    public static IReadOnlyDictionary<string, string> Listed => Since;

    // Whether a feature first marked in firstVersion is marked in a build of buildVersion.
    public static bool Shown(string? buildVersion, bool experimental, string firstVersion)
    {
        if (buildVersion is null || !Version.TryParse(buildVersion.Split('-', '+')[0], out var build) || !Version.TryParse(firstVersion, out var first)) return false;
        var order = build.CompareTo(first);
        return order < 0 || (order == 0 && experimental);
    }

    public static IReadOnlyList<string> NamesFor(string? buildVersion, bool experimental) =>
        Since.Where(entry => Shown(buildVersion, experimental, entry.Value)).Select(entry => entry.Key).ToArray();

    // Marks the listed controls under root for this build; returns the names marked.
    public static IReadOnlyList<string> Apply(Control root, string? buildVersion = null, bool? experimental = null)
    {
        var names = NamesFor(buildVersion ?? Updates.CurrentVersion, experimental ?? BuildInfo.Channel == "Experimental").ToHashSet(StringComparer.Ordinal);
        var done = new List<string>();
        foreach (var item in Descendants(root).ToArray())
            if (names.Contains(item.Name)) { MarkControl(item); done.Add(item.Name); }
        return done;
    }

    private static IEnumerable<Control> Descendants(Control parent)
    {
        foreach (Control child in parent.Controls)
        {
            yield return child;
            foreach (var nested in Descendants(child)) yield return nested;
        }
    }

    // Headings draw their own mark and a settings row marks its label. Anything else keeps room
    // to its right, where its parent draws the mark, also after Simple view moves it.
    private static void MarkControl(Control item)
    {
        if (item is SectionHeader header) { header.IsNew = true; return; }
        if (item.Parent is SettingRow row) { row.IsNew = true; return; }
        if (marked.TryGetValue(item, out _)) return;
        marked.Add(item, new object());
        item.Margin = item.Margin with { Right = item.Margin.Right + Gap(item) + Width(item.Font) };
        PaintBeside(item.Parent);
        item.ParentChanged += (_, _) => PaintBeside(item.Parent);
        item.LocationChanged += (_, _) => item.Parent?.Invalidate();
        item.SizeChanged += (_, _) => item.Parent?.Invalidate();
        item.VisibleChanged += (_, _) => item.Parent?.Invalidate();
    }

    private static void PaintBeside(Control? parent)
    {
        if (parent is null || painting.TryGetValue(parent, out _)) return;
        painting.Add(parent, new object());
        parent.Paint += (_, e) =>
        {
            foreach (Control child in parent.Controls)
                if (child.Visible && !child.Bounds.IsEmpty && IsMarked(child)) Draw(e.Graphics, child.Font, child.Right + Gap(parent), child.Top, child.Height);
        };
        parent.Invalidate();
    }

    public static bool IsMarked(Control? item) => item is not null && marked.TryGetValue(item, out _);

    public static Color Color => SystemInformation.HighContrast ? SystemColors.HotTrack : LauncherAppearance.Accent;
    public static int Gap(Control owner) => LauncherAppearance.Px(owner, 6);
    public static int Width(Font font) => TextRenderer.MeasureText(Mark, font, Size.Empty, TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine).Width;

    public static void Draw(Graphics graphics, Font font, int left, int top, int height) =>
        TextRenderer.DrawText(graphics, Mark, font, new Rectangle(left, top, Width(font) + 2, height), Color,
            TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine | TextFormatFlags.NoPadding);
}
