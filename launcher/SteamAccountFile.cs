using System.Globalization;
using System.Text;

namespace GRID2VR;

// Steam's last-selected account is a fallback hint when ActiveProcess is empty.
// The native provider independently checks the actual SteamUser before save access.
internal static class SteamAccountFile
{
    internal static string? ReadRecent(string text)
    {
        if (text.Length > 65536) return null;
        try
        {
            var reader = new Reader(text);
            if (reader.Token() != "users" || reader.Token() != "{") return null;
            string? recent = null;
            var hasMostRecent = false;
            var automatic = new List<string>();
            var accounts = new HashSet<string>(StringComparer.Ordinal);
            while (reader.Token() is { } account && account != "}")
            {
                if (!accounts.Add(account) || reader.Token() != "{") return null;
                var fields = new Dictionary<string, string>(StringComparer.Ordinal);
                while (reader.Token() is { } key && key != "}")
                {
                    var value = reader.Token();
                    if (key == "{" || value is null or "{" or "}" || !fields.TryAdd(key, value)) return null;
                }
                hasMostRecent |= fields.ContainsKey("MostRecent");
                if (fields.GetValueOrDefault("AutoLogin") == "1") automatic.Add(account);
                if (fields.GetValueOrDefault("MostRecent") != "1") continue;
                if (recent != null || Account(account) is not { } id) return null;
                recent = id;
            }
            if (!reader.ClosedRoot || reader.Token() != null) return null;
            // Current Steam uses AutoLogin and omits MostRecent. Never choose
            // an arbitrary account when either format is ambiguous.
            return hasMostRecent ? recent : automatic.Count == 1 ? Account(automatic[0]) : null;
        }
        catch (FormatException) { return null; }
    }

    static string? Account(string text) => ulong.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var id) &&
        (id >> 32) == 0x01100001 && (uint)id != 0 ? ((uint)id).ToString(CultureInfo.InvariantCulture) : null;

    sealed class Reader(string text)
    {
        int position, depth;
        internal bool ClosedRoot { get; private set; }
        internal string? Token()
        {
            while (position < text.Length && char.IsWhiteSpace(text[position])) position++;
            if (position == text.Length) return null;
            var c = text[position++];
            if (c == '{') { depth++; return "{"; }
            if (c == '}') { if (--depth < 0) throw new FormatException(); ClosedRoot = depth == 0; return "}"; }
            if (c != '"') throw new FormatException();
            var value = new StringBuilder();
            while (position < text.Length)
            {
                c = text[position++];
                if (c == '"') return value.ToString();
                if (c == '\\')
                {
                    if (position == text.Length) throw new FormatException();
                    c = text[position++];
                    if (c is not ('"' or '\\')) throw new FormatException();
                }
                value.Append(c);
            }
            throw new FormatException();
        }
    }
}
