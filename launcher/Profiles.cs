using System.Text.Json;
using System.Text.Json.Serialization;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace GRID2VR;

internal sealed record ProfileInfo(
    [property: JsonPropertyName("schema")] int Schema,
    [property: JsonPropertyName("id")] string Id,
    [property: JsonPropertyName("name")] string Name,
    [property: JsonPropertyName("kind")] string Kind,
    [property: JsonPropertyName("createdUtc")] DateTime CreatedUtc)
{
    [JsonPropertyName("accountId")] public string? AccountId { get; init; }
    [JsonIgnore] public int FileCount { get; init; }
    [JsonIgnore] public long Bytes { get; init; }
    [JsonIgnore] public DateTime? LastSavedUtc { get; init; }
    [JsonIgnore] public string Status { get; init; } = "";
}

internal sealed record ProfileEntry(string Id, ProfileInfo? Info, string? Error);

// Separate careers live in the workspace. The game worker routes Steam save calls
// here; this store never replaces or rewrites the user's normal Steam career.
internal sealed class ProfileStore(string workspaceRoot, Action requireClosed, Func<string?> currentSaveRoot, Func<string?>? currentAccountId = null,
    bool managedProfilesSupported = true)
{
    internal const string CurrentCareer = "current";
    internal const string StandaloneCareerMessage = "Standalone installations use Current game career. The game manages its existing saves; separate launcher profiles are unavailable.";
    internal const int MaxFiles = 64;
    internal const long MaxBytes = 64L * 1024 * 1024;
    internal const int MaxLocalDirectories = 16;
    internal const int MaxLocalDepth = 4;
    internal const string SessionMutex = @"Global\GRID2VR.Session";
    public string Root { get; } = Path.Combine(AppPaths.Data(Path.GetFullPath(workspaceRoot)), "profiles");
    public bool ManagedProfilesSupported { get; } = managedProfilesSupported;
    readonly string selection = Path.Combine(AppPaths.Data(Path.GetFullPath(workspaceRoot)), "profile-selection.json");
    sealed record Selection([property: JsonPropertyName("schema")] int Schema, [property: JsonPropertyName("id")] string Id);
    sealed record SaveFile(string Path, string Name, long Bytes, DateTime LastWriteUtc);
    sealed record SaveTree(IReadOnlyList<SaveFile> Files, IReadOnlyList<string> Directories);

    internal static bool IsValidNewName(string? name) => name is { Length: >= 1 and <= 24 } &&
        name.All(c => c is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or >= '0' and <= '9');

    static void ValidId(string id)
    {
        if (!Guid.TryParseExact(id, "N", out var guid) || guid.ToString("N") != id)
            throw new InvalidDataException("Invalid profile identifier.");
    }

    static bool ValidAccountId(string? id) => id is { Length: >= 1 and <= 10 } && id.All(c => c is >= '0' and <= '9') &&
        uint.TryParse(id, out var account) && account > 0 && account.ToString(System.Globalization.CultureInfo.InvariantCulture) == id;

    string? AccountId()
    {
        var id = currentAccountId?.Invoke();
        if (id != null && !ValidAccountId(id)) throw new InvalidDataException("The active Steam account identifier is invalid.");
        return id;
    }

    void RequireManagedProfiles()
    {
        if (!ManagedProfilesSupported) throw new IOException(StandaloneCareerMessage);
    }

    internal static void NoLinks(string path)
    {
        for (string? item = Path.GetFullPath(path); item != null; item = Path.GetDirectoryName(item))
        {
            FileAttributes attributes;
            try { attributes = File.GetAttributes(item); }
            catch (FileNotFoundException) { continue; }
            catch (DirectoryNotFoundException) { continue; }
            if ((attributes & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Profile storage and source saves cannot use linked files or folders.");
            if ((attributes & FileAttributes.Directory) == 0)
            {
                using var handle = File.OpenHandle(item, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                if (!GetFileInformationByHandle(handle, out var information))
                    throw new IOException("Could not inspect profile file ownership.", Marshal.GetLastWin32Error());
                if (information.NumberOfLinks != 1)
                    throw new IOException("Profile storage and source saves cannot use hard-linked files.");
            }
        }
    }

    string ProfileRoot(string id)
    {
        ValidId(id);
        var path = Path.Combine(Root, id);
        NoLinks(path);
        return path;
    }

    static T ReadJson<T>(string path)
    {
        NoLinks(path);
        using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        if (stream.Length > 8192) throw new InvalidDataException("Profile metadata is too large.");
        return JsonSerializer.Deserialize<T>(stream) ?? throw new InvalidDataException("Profile metadata is empty.");
    }

    static void NewJson<T>(string path, T value)
    {
        NoLinks(path);
        using var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        JsonSerializer.Serialize(stream, value);
        stream.Flush(true);
    }

    static void AtomicJson<T>(string path, T value)
    {
        NoLinks(path);
        var parent = Path.GetDirectoryName(path)!;
        Directory.CreateDirectory(parent);
        NoLinks(parent);
        var temporary = Path.Combine(parent, ".profile-" + Guid.NewGuid().ToString("N") + ".tmp");
        try { NewJson(temporary, value); NoLinks(path); File.Move(temporary, path, true); }
        finally { NoLinks(temporary); if (File.Exists(temporary)) File.Delete(temporary); }
    }

    static IReadOnlyList<SaveFile> ScanRemote(string path) => ScanTree(path, false).Files;

    static SaveTree ScanLocal(string path)
    {
        NoLinks(path);
        if (!Path.Exists(path)) return new SaveTree([], []);
        return ScanTree(path, true);
    }

    static SaveTree ScanTree(string path, bool recursive)
    {
        NoLinks(path);
        if (!Directory.Exists(path)) throw new IOException("This profile's save folder is missing.");
        var files = new List<SaveFile>();
        var directories = new List<string>();
        var pending = new Stack<(string Path, int Depth)>();
        pending.Push((path, 0));
        long bytes = 0;
        while (pending.TryPop(out var current))
        {
            NoLinks(current.Path);
            directories.Add(current.Path);
            foreach (var child in Directory.EnumerateFileSystemEntries(current.Path))
            {
                NoLinks(child);
                var attributes = File.GetAttributes(child);
                var name = Path.GetFileName(child);
                if (!ValidSaveName(name)) throw new InvalidDataException("The save folder contains an unsupported name.");
                if ((attributes & FileAttributes.Directory) != 0)
                {
                    if (!recursive || current.Depth >= MaxLocalDepth || directories.Count + pending.Count >= MaxLocalDirectories)
                        throw new InvalidDataException("The save folder contains an unsupported or overly deep subfolder.");
                    pending.Push((child, current.Depth + 1));
                    continue;
                }
                var file = new FileInfo(child);
                bytes = checked(bytes + file.Length);
                if (files.Count >= MaxFiles || bytes > MaxBytes)
                    throw new InvalidDataException("Profile saves exceed the 64-file / 64 MiB limit.");
                files.Add(new SaveFile(child, name, file.Length, file.LastWriteTimeUtc));
            }
        }
        return new SaveTree(files, directories);
    }

    static IReadOnlyList<SaveFile> CombinedSaves(string root)
    {
        var files = ScanRemote(Path.Combine(root, "remote")).Concat(ScanLocal(Path.Combine(root, "local")).Files).ToArray();
        if (files.Length > MaxFiles || files.Sum(file => file.Bytes) > MaxBytes)
            throw new InvalidDataException("Profile saves exceed the combined 64-file / 64 MiB limit.");
        return files;
    }

    internal static bool ValidSaveName(string name) => name.Length is >= 1 and <= 180 &&
        name != "." && name != ".." && !name.EndsWith('.') && !name.EndsWith(' ') &&
        !name.Any(c => c < 32 || c > 126 || "<>:\"/\\|?*".Contains(c)) &&
        !IsDeviceName(name.Split('.')[0]);

    static bool IsDeviceName(string name) => name.Equals("CON", StringComparison.OrdinalIgnoreCase) ||
        name.Equals("PRN", StringComparison.OrdinalIgnoreCase) || name.Equals("AUX", StringComparison.OrdinalIgnoreCase) ||
        name.Equals("NUL", StringComparison.OrdinalIgnoreCase) ||
        (name.Length == 4 && name[3] is >= '1' and <= '9' &&
            (name.StartsWith("COM", StringComparison.OrdinalIgnoreCase) || name.StartsWith("LPT", StringComparison.OrdinalIgnoreCase)));

    ProfileInfo ReadInfo(string id)
    {
        var root = ProfileRoot(id);
        var info = ReadJson<ProfileInfo>(Path.Combine(root, "profile.json"));
        if (info.Schema != 1 || info.Id != id || !IsValidNewName(info.Name) ||
            info.Kind is not ("fresh" or "copied") || info.CreatedUtc.Kind != DateTimeKind.Utc || info.CreatedUtc == default ||
            (info.AccountId != null && !ValidAccountId(info.AccountId)))
            throw new InvalidDataException("This profile has incompatible or damaged metadata.");
        foreach (var child in Directory.EnumerateFileSystemEntries(root))
        {
            NoLinks(child);
            if (Path.GetFileName(child) is not ("profile.json" or "remote" or "local"))
                throw new InvalidDataException("This profile contains an unexpected file or folder.");
        }
        return info;
    }

    static ProfileInfo WithDetails(ProfileInfo info, IReadOnlyList<SaveFile> files)
    {
        var saves = files.Where(file => !(file.Name.StartsWith(".grid2vr-write-", StringComparison.Ordinal) && file.Name.EndsWith(".tmp", StringComparison.Ordinal))).ToArray();
        return info with
        {
            FileCount = saves.Length,
            Bytes = saves.Sum(file => file.Bytes),
            LastSavedUtc = saves.Length == 0 ? null : saves.Max(file => file.LastWriteUtc),
            Status = saves.Length == 0 ? "Fresh career — created by GRID 2 on first use" : "Saved career"
        };
    }

    public ProfileInfo Read(string id)
    {
        if (id != CurrentCareer)
        {
            RequireManagedProfiles();
            var info = ReadInfo(id);
            if (info.AccountId != null && info.AccountId != AccountId())
                throw new IOException("This profile belongs to a different Steam account. Sign in to its original account or choose another profile.");
            return WithDetails(info, CombinedSaves(ProfileRoot(id)));
        }
        var current = new ProfileInfo(1, CurrentCareer, "Current game career", "current", DateTime.UnixEpoch);
        if (!ManagedProfilesSupported) return current with { Status = "Existing career — saved and loaded by the game" };
        var source = currentSaveRoot();
        if (source == null) return current with { Status = "Normal game career — no local Steam save found" };
        return WithDetails(current, ScanRemote(source));
    }

    public string SelectedId()
    {
        NoLinks(selection);
        if (!File.Exists(selection))
        {
            if (Directory.Exists(selection)) throw new InvalidDataException("Profile selection is not a file.");
            return CurrentCareer;
        }
        var selected = ReadJson<Selection>(selection);
        if (selected.Schema != 1) throw new InvalidDataException("Unsupported profile selection version.");
        if (selected.Id != CurrentCareer) Read(selected.Id);
        return selected.Id;
    }

    public IReadOnlyList<ProfileEntry> List()
    {
        NoLinks(Root);
        var entries = new List<ProfileEntry> { Entry(CurrentCareer) };
        if (!ManagedProfilesSupported) return entries;
        if (Directory.Exists(Root))
            foreach (var directory in Directory.EnumerateDirectories(Root))
            {
                var id = Path.GetFileName(directory);
                if (Guid.TryParseExact(id, "N", out _)) entries.Add(Entry(id));
            }
        return entries.Take(1).Concat(entries.Skip(1).OrderBy(entry => entry.Info?.Name ?? entry.Id, StringComparer.CurrentCultureIgnoreCase)).ToArray();
    }

    ProfileEntry Entry(string id)
    {
        try { return new ProfileEntry(id, Read(id), null); }
        catch (Exception error) when (error is IOException or InvalidDataException or JsonException or UnauthorizedAccessException or ArgumentException)
        { return new ProfileEntry(id, null, error.Message); }
    }

    public void Select(string id)
    {
        using var guard = new EditGuard(requireClosed);
        if (id != CurrentCareer) Read(id);
        AtomicJson(selection, new Selection(1, id));
    }

    public ProfileInfo Create(string name, bool copyCurrent = false)
    {
        RequireManagedProfiles();
        using var guard = new EditGuard(requireClosed);
        if (!IsValidNewName(name)) throw new ArgumentException("Use 1–24 letters (A–Z) or numbers (0–9), without spaces or symbols.");
        SelectedId(); // Never replace a broken selection without an explicit switch.
        var accountId = AccountId();
        if (currentAccountId != null && accountId == null)
            throw new IOException("The active Steam account could not be identified. Start Steam and sign in before creating a profile.");
        NoLinks(Root);
        Directory.CreateDirectory(Root);
        foreach (var directory in Directory.EnumerateDirectories(Root))
        {
            var existingId = Path.GetFileName(directory);
            if (!Guid.TryParseExact(existingId, "N", out _)) continue;
            var existing = ReadInfo(existingId);
            if (existing.Name.Equals(name, StringComparison.OrdinalIgnoreCase)) throw new ArgumentException("A profile with that name already exists.");
        }
        var source = copyCurrent ? currentSaveRoot() ?? throw new IOException("No local GRID 2 Steam career was found to copy.") : null;
        var files = source == null ? [] : ScanRemote(source);
        if (copyCurrent && files.Count == 0) throw new IOException("The current game career has no save files to copy.");
        var id = Guid.NewGuid().ToString("N");
        var info = new ProfileInfo(1, id, name, copyCurrent ? "copied" : "fresh", DateTime.UtcNow) { AccountId = accountId };
        var staging = Path.Combine(Root, ".creating-" + id);
        var destination = ProfileRoot(id);
        var owned = new List<string>();
        var registered = false;
        try
        {
            Directory.CreateDirectory(Path.Combine(staging, "remote"));
            Directory.CreateDirectory(Path.Combine(staging, "local", "savegame"));
            NoLinks(staging);
            foreach (var file in files)
            {
                NoLinks(file.Path);
                var target = Path.Combine(staging, "remote", file.Name);
                owned.Add(target);
                using (var input = new FileStream(file.Path, FileMode.Open, FileAccess.Read, FileShare.Read))
                using (var output = new FileStream(target, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    if (input.Length != file.Bytes || File.GetLastWriteTimeUtc(file.Path) != file.LastWriteUtc)
                        throw new IOException("The source save changed while the profile was being created. Try again after GRID 2 and Steam Cloud finish saving.");
                    input.CopyTo(output);
                    output.Flush(true);
                }
                File.SetLastWriteTimeUtc(target, file.LastWriteUtc);
            }
            var metadata = Path.Combine(staging, "profile.json");
            owned.Add(metadata); NewJson(metadata, info);
            if (AccountId() != accountId || (source != null &&
                !string.Equals(currentSaveRoot(), source, StringComparison.OrdinalIgnoreCase)))
                throw new IOException("The active Steam account changed while the profile was being created. Try again after signing in.");
            Directory.Move(staging, destination);
            owned = owned.Select(path => Path.Combine(destination, Path.GetRelativePath(staging, path))).ToList();
            staging = destination;
            AtomicJson(selection, new Selection(1, id));
            registered = true;
            return Read(id);
        }
        finally
        {
            if (!registered)
            {
                // Only files created by this attempt; never recursively remove a save directory.
                foreach (var path in owned) { NoLinks(path); if (File.Exists(path)) File.Delete(path); }
                var remote = Path.Combine(staging, "remote");
                NoLinks(remote); if (Directory.Exists(remote)) Directory.Delete(remote);
                var localSave = Path.Combine(staging, "local", "savegame");
                NoLinks(localSave); if (Directory.Exists(localSave)) Directory.Delete(localSave);
                var local = Path.Combine(staging, "local");
                NoLinks(local); if (Directory.Exists(local)) Directory.Delete(local);
                NoLinks(staging); if (Directory.Exists(staging)) Directory.Delete(staging);
            }
        }
    }

    public void Delete(string id)
    {
        RequireManagedProfiles();
        using var guard = new EditGuard(requireClosed);
        if (id == CurrentCareer) throw new IOException("The current game career cannot be deleted here.");
        if (id == SelectedId()) throw new IOException("Use another profile before deleting this one.");
        ReadInfo(id);
        var root = ProfileRoot(id);
        var remote = Path.Combine(root, "remote");
        var local = ScanLocal(Path.Combine(root, "local"));
        var files = CombinedSaves(root).Select(file => file.Path).Append(Path.Combine(root, "profile.json")).ToArray();
        foreach (var path in files.Concat(local.Directories).Append(remote).Append(root))
        {
            NoLinks(path);
            if ((File.GetAttributes(path) & FileAttributes.ReadOnly) != 0)
                throw new IOException("A profile file is read-only. Nothing was deleted.");
        }
        foreach (var path in files.Take(files.Length - 1)) { NoLinks(path); File.Delete(path); }
        foreach (var path in local.Directories.Reverse()) { NoLinks(path); Directory.Delete(path); }
        NoLinks(remote); Directory.Delete(remote);
        var metadata = Path.Combine(root, "profile.json");
        NoLinks(metadata); File.Delete(metadata);
        NoLinks(root); Directory.Delete(root);
    }

    sealed class EditGuard : IDisposable
    {
        readonly Mutex mutex = new(false, SessionMutex);
        bool held;
        internal EditGuard(Action requireClosed)
        {
            try
            {
                try { held = mutex.WaitOne(0); } catch (AbandonedMutexException) { held = true; }
                if (!held) throw new IOException("Close the active GRID2VR session before changing profiles.");
                requireClosed();
            }
            catch { Dispose(); throw; }
        }
        public void Dispose()
        {
            if (held) { mutex.ReleaseMutex(); held = false; }
            mutex.Dispose();
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    struct FileInformation
    {
        public uint Attributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime, AccessTime, WriteTime;
        public uint VolumeSerial, SizeHigh, SizeLow, NumberOfLinks, IndexHigh, IndexLow;
    }
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool GetFileInformationByHandle(SafeFileHandle file, out FileInformation information);
}
