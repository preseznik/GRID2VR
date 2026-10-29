namespace GRID2VR;

internal static class Branding
{
    public const string Patreon = "https://www.patreon.com/cw/bohloney";
    public static Icon LoadIcon()
    {
        using var stream = typeof(Branding).Assembly.GetManifestResourceStream("GRID2VR.ico") ?? throw new IOException("Launcher icon is missing.");
        using var icon = new Icon(stream);
        return (Icon)icon.Clone();
    }
    public static Bitmap LoadLogo()
    {
        using var stream = typeof(Branding).Assembly.GetManifestResourceStream("GRID2VR.png") ?? throw new IOException("Launcher logo is missing.");
        using var image = Image.FromStream(stream);
        return new Bitmap(image);
    }
    public static PictureBox Logo(int size)
    {
        var box = new PictureBox { Name = "LauncherLogo", AccessibleName = "GRID2VR logo", Image = LoadLogo(), Size = new Size(size, size), SizeMode = PictureBoxSizeMode.Zoom, Margin = new Padding(0, 0, 12, 4), TabStop = false };
        box.Disposed += (_, _) => box.Image?.Dispose();
        return box;
    }
}
