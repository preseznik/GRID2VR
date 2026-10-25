namespace GRID2VR;

internal static class RecenterShortcut
{
    public static bool ValidKey(int key) => key is >= 48 and <= 90 or >= 96 and <= 135 or 8 or 9 or 13 or 19 or 32 or >= 33 and <= 40 or 45 or 46;
    public static bool Valid(int key, int modifiers) => ValidKey(key) && modifiers is >= 0 and <= 7 &&
        !((modifiers & 2) != 0 && key is 9 or 115) && !(key == 46 && (modifiers & 3) == 3);
    public static string Label(int key, int modifiers) =>
        ((modifiers & 1) != 0 ? "Ctrl+" : "") + ((modifiers & 2) != 0 ? "Alt+" : "") +
        ((modifiers & 4) != 0 ? "Shift+" : "") + ((Keys)key).ToString();
}
