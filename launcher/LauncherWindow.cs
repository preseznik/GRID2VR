namespace GRID2VR;

// Window preferences have their own file: closing the UI must not save pending
// car/track/graphics edits. Dimensions are client-area pixels at 96 DPI.
// CollapsedSections holds section titles joined by '|'; null keeps every section open.
internal sealed record LauncherWindow(int Width = 900, int Height = 720, bool Maximized = false, bool? AdvancedView = null,
    string? CollapsedSections = null)
{
    internal const char SectionSeparator = '|';
    internal bool Valid => Width is >= 320 and <= 8192 && Height is >= 240 and <= 8192 && (CollapsedSections?.Length ?? 0) <= 2048;
    internal HashSet<string> Collapsed() => [.. (CollapsedSections ?? "").Split(SectionSeparator, StringSplitOptions.RemoveEmptyEntries)];
    internal LauncherWindow WithCollapsed(IEnumerable<string> titles)
    {
        var joined = string.Join(SectionSeparator, titles.Where(t => t.Length > 0 && !t.Contains(SectionSeparator)).Distinct().Order(StringComparer.Ordinal));
        return this with { CollapsedSections = joined.Length == 0 ? null : joined };
    }
    public Size ClientSizeAtDpi(int dpi, Size available) => new(
        Math.Clamp((int)Math.Round(Width * Math.Max(96, dpi) / 96.0), 1, Math.Max(1, available.Width)),
        Math.Clamp((int)Math.Round(Height * Math.Max(96, dpi) / 96.0), 1, Math.Max(1, available.Height)));
    public static LauncherWindow FromClientSize(Size size, int dpi) => new(
        (int)Math.Round(size.Width * 96.0 / Math.Max(96, dpi)),
        (int)Math.Round(size.Height * 96.0 / Math.Max(96, dpi)));
}
