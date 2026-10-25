using System.Diagnostics;
using System.Net.Http;
using System.Text.Json;

namespace GRID2VR;

internal sealed record UpdatePreferences(bool CheckAtStartup = true, bool Experimental = false)
{
    public static UpdatePreferences Load(string root)
    {
        var path = Path.Combine(AppPaths.Data(root), "update-preferences.json");
        if (!File.Exists(path)) return new();
        if (new FileInfo(path).Length > 4096) throw new IOException("Update settings exceed their size limit.");
        return JsonSerializer.Deserialize<UpdatePreferences>(File.ReadAllText(path)) ?? new();
    }
    public void Save(string root)
    {
        var path = Path.Combine(AppPaths.Data(root), "update-preferences.json");
        Updates.PlainPath(path); Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try { File.WriteAllText(temp, JsonSerializer.Serialize(this)); File.Move(temp, path, true); }
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }
}

internal sealed class UpdatesPanel : UserControl
{
    private readonly string root;
    private readonly CheckBox startup = new() { Text = "Check for updates at startup", AutoSize = true };
    private readonly CheckBox experimental = new() { Text = "Include experimental releases", AutoSize = true };
    private readonly Label status = new() { Text = "Check GitHub for a newer release.", AutoSize = true };
    private readonly Button check = new() { Text = "Check for updates", AutoSize = true };
    private readonly Button install = new() { Text = "Download and install…", AutoSize = true, Enabled = false };
    private readonly Button cancel = new() { Text = "Cancel download", AutoSize = true, Visible = false };
    private readonly Button notes = new() { Text = "Release notes", AutoSize = true, Enabled = false };
    private readonly Button stable = new() { Text = "Return to stable…", AutoSize = true, Visible = BuildInfo.Channel == "Experimental" };
    private readonly ProgressBar progress = new() { Dock = DockStyle.Top, Visible = false };
    private readonly HttpClient http = Updates.CreateClient();
    private CancellationTokenSource? operation;
    private AppRelease? release;
    private bool sessionIdle;
    public event Action<string>? UpdateAvailable;
    private bool returningStable;
    public UpdatesPanel(string path, bool about = false)
    {
        root = path; AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink; Dock = DockStyle.Top; Name = "Updates";
        var layout = new VerticalStack(); Controls.Add(layout);
        if (!about)
        {
            layout.Controls.Add(new Label { Text = "GRID2VR " + Updates.CurrentVersion, AutoSize = true });
            layout.Controls.Add(startup);
        }
        layout.Controls.Add(experimental);
        var actions=new FlowLayoutPanel { AutoSize=true, Dock=DockStyle.Top, WrapContents=true };
        actions.Controls.AddRange([check,install,notes,stable,cancel]); layout.Controls.Add(actions);layout.Controls.Add(status);layout.Controls.Add(progress);
        var releases = new LinkLabel { Text = "GitHub releases", AutoSize = true };
        releases.LinkClicked += (_, _) => Process.Start(new ProcessStartInfo(Updates.Repository + "/releases") { UseShellExecute = true });
        if (!about) layout.Controls.Add(releases);
        status.Text = "Check GitHub Releases for a newer version. No GitHub account is needed.\nClose the game before updating. Your settings are kept.";
        layout.SizeChanged += (_, _) => status.MaximumSize = new(Math.Max(1, layout.Width - 10), 0);
        check.Click += async (_, _) => await Check(); install.Click += async (_, _) => await Download();
        cancel.Click += (_, _) => operation?.Cancel();
        notes.Click+=(_,_)=>{if(release is not null)Process.Start(new ProcessStartInfo(release.Page.AbsoluteUri){UseShellExecute=true});};
        stable.Click+=async (_,_)=>await ReturnStable();
        startup.CheckedChanged += (_, _) => Save();
        experimental.CheckedChanged += (_, _) => { release = null; Enable(); Save(); };
    }
    public async Task Start(bool checkAtStartup = true)
    {
        try
        {
            var prefs = UpdatePreferences.Load(root);
            initializing = true; startup.Checked = prefs.CheckAtStartup; experimental.Checked = prefs.Experimental; initializing = false;
            if (checkAtStartup && startup.Checked) await Check();
        }
        catch (Exception ex) { status.Text = ex.Message; }
    }
    private bool initializing;
    private void Save()
    {
        if (initializing) return;
        try { new UpdatePreferences(startup.Checked, experimental.Checked).Save(root); }
        catch (Exception ex) { status.Text = ex.Message; }
    }
    public void SetIdle(bool idle) { sessionIdle = idle; Enable(); }
    private void Enable()
    {
        check.Enabled = experimental.Enabled = operation is null;
        notes.Enabled=operation is null && release is not null;
        stable.Enabled=sessionIdle && operation is null;
        install.Enabled = sessionIdle && operation is null && release?.Installer is not null;
    }
    private async Task Check()
    {
        if (operation is not null) return;
        operation = new(); Enable(); status.Text = "Checking GitHub…";
        returningStable=false;
        try
        {
            release = await new Updates(http).Check(experimental.Checked, operation.Token);
            if (!IsDisposed) status.Text = release is null ? "You have the latest available release." : $"Update available: {release.Tag}" + (release.Installer is null ? ". No verified installer is attached; open GitHub releases." : ". Ready to download.");
            if (!IsDisposed && release is not null) UpdateAvailable?.Invoke(release.Tag);
        }
        catch (Exception ex) { release = null; if (!IsDisposed) status.Text = "Update check failed: " + ex.Message; }
        finally { operation.Dispose(); operation = null; if (!IsDisposed) Enable(); }
    }
    private async Task ReturnStable()
    {
        if(operation is not null || !sessionIdle)return;
        operation=new();Enable();status.Text="Finding the latest Stable release…";
        try {
            var candidate=await new Updates(http).Check(false,operation.Token,allowOlder:true);
            if(IsDisposed)return;
            if(candidate?.Installer is null)throw new IOException("No verified Stable installer is available yet.");
            if(MessageBox.Show(this,$"Install Stable {candidate.Tag}? This may be an older version. Settings and careers are kept.","Return to stable",MessageBoxButtons.YesNo,MessageBoxIcon.Question)!=DialogResult.Yes)return;
            release=candidate;returningStable=true;status.Text="Stable release selected. Choose Download and install to continue.";
        } catch(Exception ex){if(!IsDisposed)status.Text=ex.Message;}
        finally{operation.Dispose();operation=null;if(!IsDisposed)Enable();}
    }
    private async Task Download()
    {
        if (operation is not null || release?.Installer is not { } asset || !sessionIdle) return;
        operation = new(); Enable(); progress.Value = 0; progress.Visible = cancel.Visible = true; status.Text = "Downloading " + release.Tag + "…";
        try
        {
            var token = operation.Token;
            var reporting = new Progress<int>(p => { if (!IsDisposed) progress.Value = p; });
            var file = await Task.Run(() => new Updates(http).Download(asset, root, reporting, token), token);
            if (IsDisposed || token.IsCancellationRequested) return;
            if (!sessionIdle) throw new IOException("Download verified. Finish the session before installing.");
            Updates.BackupPreferences(root);
            var previous=UpdatePreferences.Load(root);
            if(returningStable)new UpdatePreferences(previous.CheckAtStartup,false).Save(root);
            try { Updates.Install(root, file, asset); }
            catch { if(returningStable)previous.Save(root);throw; }
            // Installer owns replacement/restart; closing the launcher releases its files.
            Application.Exit();
        }
        catch (OperationCanceledException) { if (!IsDisposed) status.Text = "Download cancelled."; }
        catch (Exception ex) { if (!IsDisposed) status.Text = ex.Message; }
        finally { operation.Dispose(); operation = null; if (!IsDisposed) { progress.Visible = cancel.Visible = false; Enable(); } }
    }
    protected override void Dispose(bool disposing)
    {
        if (disposing) { operation?.Cancel(); http.Dispose(); }
        base.Dispose(disposing);
    }
}
