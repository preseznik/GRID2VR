namespace GRID2VR;

// Launcher-tab CUSTOM tracks panel, modelled on DiRT2VR's launcher/CustomTrackPanel.vb:
// a toggle, the supported tracks with their status, and the selected track's detail
// with Build and install / Manage / Stop build. While the toggle is on, the selected
// custom track replaces the Event/Track choice for Practice and Race; turning it off
// restores the original selection.
internal sealed class CustomTrackPanel : VerticalStack
{
    internal event EventHandler? Changed;
    private readonly string root;
    private readonly CheckBox toggle = new() { Text = "CUSTOM tracks (Experimental)", Name = "CustomTracks", AutoSize = true, Margin = new Padding(0, 4, 0, 8) };
    private readonly TrackBrowser browser;
    private readonly ListBox list = new() { Name = "CustomTrackList", AccessibleName = "Custom tracks", DrawMode = DrawMode.OwnerDrawVariable, IntegralHeight = false, BorderStyle = BorderStyle.FixedSingle };
    private readonly VerticalStack detail = new() { Name = "CustomTrackDetail" };
    private readonly Label title = new() { Name = "CustomTrackTitle", AutoSize = true, UseMnemonic = false, Margin = new Padding(0, 0, 0, 4) };
    private readonly Label description = new() { Name = "CustomTrackDescription", AutoSize = true, UseMnemonic = false, Margin = new Padding(0, 0, 0, 8) };
    private readonly TrackStatusLabel status = new() { Name = "CustomTrackStatus", AutoSize = true, UseMnemonic = false, Padding = new Padding(8), Margin = new Padding(0, 0, 0, 8) };
    private readonly Label hint = new() { Name = "CustomTrackHint", AutoSize = true, UseMnemonic = false, Margin = new Padding(0, 0, 0, 8) };
    // Lighting presets of the selected track, shown when its installed pack has more than Daylight.
    private readonly FlowLayoutPanel conditionsRow = new() { Name = "CustomTrackConditionsRow", AutoSize = true, WrapContents = false, Margin = new Padding(0, 0, 0, 8) };
    private readonly ComboBox conditions = new() { Name = "CustomTrackConditions", AccessibleName = "Conditions", DropDownStyle = ComboBoxStyle.DropDownList, Width = 180 };
    private string preferredCondition = "day";
    // Next build's quality (DiRT2VR's "Build with high quality assets"); follows the installed quality on selection.
    private readonly CheckBox highQuality = new() { Text = "Build with high quality assets", Name = "CustomTrackHighQuality", AutoSize = true, Margin = new Padding(0, 0, 0, 8) };
    private string? qualityShownFor;
    private readonly FlowLayoutPanel actions = new() { Name = "CustomTrackActions", AutoSize = true, WrapContents = true, Margin = new Padding(0, 0, 0, 8) };
    private readonly Button install = new() { Text = "Build and install…", Name = "BuildCustomTrack", AutoSize = true, Padding = new Padding(5, 2, 5, 2) };
    private readonly Button chooseSource = new() { Text = "Choose Assetto Corsa folder…", Name = "ChooseCustomTrackSource", AutoSize = true, Padding = new Padding(5, 2, 5, 2) };
    private readonly Button manage = new() { Text = "Manage…", Name = "ManageCustomTrack", AutoSize = true, Padding = new Padding(5, 2, 5, 2) };
    private readonly Button stop = new() { Text = "Stop build", Name = "StopCustomTrack", AutoSize = true, Padding = new Padding(5, 2, 5, 2) };
    private readonly ProgressBar progress = new() { Name = "CustomTrackProgress", Minimum = 0, Maximum = 100, Height = 18, Margin = new Padding(0, 0, 0, 4) };
    private readonly Label progressText = new() { Name = "CustomTrackProgressText", AutoSize = true, UseMnemonic = false, Margin = new Padding(0, 0, 0, 8) };
    private readonly Label footer = new() { AutoSize = true, UseMnemonic = false, Text = "Turn CUSTOM tracks off to restore your original track selection." };
    private readonly ContextMenuStrip menu = new();
    private readonly Dictionary<string, CustomTrackStatus> statuses = new(StringComparer.Ordinal);
    private CancellationTokenSource? cancellation;
    private bool idle = true, loading;

    internal CustomTrackPanel(string launcherRoot)
    {
        root = launcherRoot; Name = "CustomTrackPanel";
        browser = new TrackBrowser(list, detail, RowHeight) { Name = "CustomTrackBrowser" };
        Controls.Add(toggle);
        foreach (var source in CustomTracks.Supported) list.Items.Add(source);
        list.SelectedIndex = 0;
        title.Font = new Font("Segoe UI", 12, FontStyle.Bold); status.Font = new Font("Segoe UI", 11, FontStyle.Bold);
        actions.Controls.AddRange([install, chooseSource, manage, stop]);
        conditionsRow.Controls.AddRange([new Label { Text = "Conditions", AutoSize = true, UseMnemonic = false, Margin = new Padding(0, 6, 8, 0) }, conditions]);
        conditions.SelectedIndexChanged += (_, _) =>
        {
            if (loading || conditions.SelectedItem is not CustomCondition chosen) return;
            preferredCondition = chosen.Id; Changed?.Invoke(this, EventArgs.Empty);
        };
        foreach (var control in new Control[] { title, description, status, hint, conditionsRow, highQuality, actions, progress, progressText, footer }) detail.Controls.Add(control);
        highQuality.CheckedChanged += (_, _) => { if (!loading) ShowSelected(); };
        var rebuild = menu.Items.Add("Rebuild from source…"); rebuild.Name = "RebuildCustomTrack";
        var folder = menu.Items.Add("Choose Assetto Corsa folder…"); folder.Name = "ManageCustomTrackSource";
        var uninstall = menu.Items.Add("Uninstall…"); uninstall.Name = "UninstallCustomTrack";
        rebuild.Click += async (_, _) => await Build();
        folder.Click += async (_, _) => await ChooseSource();
        uninstall.Click += async (_, _) => await Uninstall();
        install.Click += async (_, _) => await Build();
        chooseSource.Click += async (_, _) => await ChooseSource();
        manage.Click += (_, _) => menu.Show(manage, new Point(0, manage.Height));
        stop.Click += (_, _) => { stop.Enabled = false; cancellation?.Cancel(); };
        list.MeasureItem += (_, e) => e.ItemHeight = RowHeight();
        list.DrawItem += DrawTrack;
        list.SelectedIndexChanged += (_, _) => { if (!loading) { ShowSelected(); Changed?.Invoke(this, EventArgs.Empty); } };
        toggle.CheckedChanged += (_, _) =>
        {
            // As in DiRT2VR: the browser is part of the panel only while the toggle is on.
            if (toggle.Checked) { if (!Controls.Contains(browser)) Controls.Add(browser); }
            else Controls.Remove(browser);
            if (!loading) Changed?.Invoke(this, EventArgs.Empty);
        };
        Disposed += (_, _) => { cancellation?.Cancel(); menu.Dispose(); if (browser.Parent is null) browser.Dispose(); };
        ShowSelected();
    }

    // Every row: name, summary and status lines plus padding.
    private int RowHeight() => (int)Math.Ceiling(Font.GetHeight() * 3 + LauncherAppearance.Px(this, 18));
    internal bool CustomEnabled => toggle.Checked;
    internal bool IsWorking => cancellation is not null;
    internal CustomTrackSource Selected => list.SelectedItem as CustomTrackSource ?? CustomTracks.Supported[0];
    internal bool SelectedInstalled => statuses.TryGetValue(Selected.Id, out var value) && value.Installed is not null;
    internal string SelectedLabel => Selected.Label + " (custom)";
    // The chosen preset when the selected track's installed pack offers it; Daylight otherwise.
    internal string SelectedCondition => conditions.SelectedItem is CustomCondition chosen && VerticalStack.IsShown(conditionsRow) ? chosen.Id : CustomTracks.Daylight.Id;

    internal void Load(bool enabled, string? trackId, string? condition = null)
    {
        loading = true;
        try
        {
            list.SelectedItem = CustomTracks.Supported.FirstOrDefault(s => s.Id == trackId) ?? CustomTracks.Supported[0];
            toggle.Checked = enabled;
            preferredCondition = condition ?? preferredCondition;
        }
        finally { loading = false; }
        ShowSelected();
    }

    internal string LaunchBlockReason()
    {
        if (!CustomEnabled) return "";
        if (IsWorking) return "Wait for the custom track build to finish, or stop it.";
        if (!SelectedInstalled) return $"Build and install {Selected.Label} first, or turn CUSTOM tracks off.";
        return "";
    }

    internal void SetIdle(bool value)
    {
        idle = value;
        toggle.Enabled = value && !IsWorking;
        list.Enabled = !IsWorking; conditions.Enabled = value;
        install.Enabled = chooseSource.Enabled = manage.Enabled = highQuality.Enabled = value && !IsWorking;
        stop.Enabled = cancellation is { IsCancellationRequested: false };
    }

    internal async Task RefreshStatuses()
    {
        var found = await Task.Run(() => CustomTracks.Supported.Select(s => CustomTracks.Status(root, s)).ToArray());
        if (IsDisposed) return;
        foreach (var value in found) statuses[value.Source.Id] = value;
        list.Invalidate(); ShowSelected();
        Changed?.Invoke(this, EventArgs.Empty);
    }

    // Passive screenshots only: real statuses, optionally a build in progress.
    internal void ApplyPreview(CustomTrackStatus value, bool enabled, int? percent, string? line)
    {
        statuses[value.Source.Id] = value; Load(enabled, value.Source.Id);
        if (percent is { } shown) { cancellation = new CancellationTokenSource(); ShowSelected(); ShowProgress(shown, line ?? ""); }
        SetIdle(idle);
    }

    private void ShowSelected()
    {
        var source = Selected;
        statuses.TryGetValue(source.Id, out var value);
        title.Text = source.Label; description.Text = source.Description;
        var installed = value?.Installed;
        status.Ready = installed is not null;
        status.Text = installed is not null ? "Installed · Ready offline" : value?.SourceFolder is null ? "Needs " + source.SourceName : "Not installed";
        // The checkbox starts at the installed quality whenever another track is shown.
        if (qualityShownFor != source.Id || value is null) { highQuality.Checked = installed?.HighQuality ?? false; if (value is not null) qualityShownFor = source.Id; }
        VerticalStack.SetShown(highQuality, !IsWorking);
        hint.Text = installed is not null
            ? $"Built {installed.Built:g} · {(installed.HighQuality ? "High quality assets" : "Standard quality")} · {installed.Bytes / (1024.0 * 1024):N0} MB" + (value?.SourceFolder is { } from ? " · from " + from : "") +
              ". Use Manage to rebuild or uninstall" + (installed.HighQuality != highQuality.Checked ? "; rebuild to apply the quality selected below." : ".")
            : value?.SourceFolder is { } path
                ? $"Build this track from your own {source.SourceName} files (about 2 minutes, about 500 MB). Found: {path}. Installed tracks work offline."
                : $"Build this track from your own {source.SourceName} files. {source.SourceName} was not found in your Steam libraries: install it, or choose its folder.";
        CustomCondition[] offered = value is null ? [CustomTracks.Daylight] : CustomTracks.Available(value);
        bool wasLoading = loading; loading = true;
        try
        {
            conditions.Items.Clear(); conditions.Items.AddRange(offered);
            conditions.SelectedItem = offered.FirstOrDefault(c => c.Id == preferredCondition) ?? offered[0];
        }
        finally { loading = wasLoading; }
        VerticalStack.SetShown(conditionsRow, offered.Length > 1);
        install.Visible = installed is null && !IsWorking; install.Enabled = idle && value?.SourceFolder is not null;
        chooseSource.Visible = installed is null && !IsWorking;
        manage.Visible = installed is not null && !IsWorking;
        stop.Visible = IsWorking;
        VerticalStack.SetShown(progress, IsWorking); VerticalStack.SetShown(progressText, IsWorking);
        if (IsWorking) progress.Height = LauncherAppearance.Px(this, 18);
        list.Invalidate();
    }

    private void ShowProgress(int percent, string line)
    {
        progress.Value = Math.Clamp(percent, 0, 100);
        progressText.Text = $"{percent}% · " + (line.Length > 120 ? line[..120] + "…" : line);
    }

    private async Task Build()
    {
        if (IsWorking || !idle) return;
        var source = Selected;
        statuses.TryGetValue(source.Id, out var value);
        if (value?.SourceFolder is null) { await ChooseSource(); statuses.TryGetValue(source.Id, out value); if (value?.SourceFolder is null) return; }
        var rebuild = value.Installed is not null;
        var shared = CustomTracks.Sharing(source);
        string also = shared.Length > 0 ? $" {string.Join(" and ", shared.Select(s => s.Label))} is built at the same time." : "";
        bool hq = highQuality.Checked;
        string quality = hq ? "\n\nHigh quality assets: source texture resolutions. Uses more disk space, GPU memory and game memory, especially in VR."
            : "\n\nStandard quality.";
        if (MessageBox.Show(this, $"{(rebuild ? "Rebuild" : "Build and install")} {source.Label} from your {source.SourceName} files?{also}{quality}\n\n{value.SourceFolder}\n\nAbout 2 minutes and {(hq ? "1 GB" : "500 MB")}. Keep GRID 2 closed until it finishes." +
                (rebuild ? " The installed copy is replaced only after the new one is complete." : ""),
                (rebuild ? "Rebuild " : "Build ") + source.Label, MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
        using var stopToken = new CancellationTokenSource(); cancellation = stopToken;
        ShowSelected(); ShowProgress(0, "Checking your source files…"); SetIdle(idle); Changed?.Invoke(this, EventArgs.Empty);
        string? failure = null;
        try
        {
            var report = new Progress<(int Percent, string Line)>(p => { if (!IsDisposed) ShowProgress(p.Percent, p.Line); });
            await Task.Run(() => CustomTracks.Build(root, source, report, stopToken.Token, hq));
        }
        catch (OperationCanceledException) { failure = $"{source.Label} build stopped. Any installed copy was kept."; }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException) { failure = error.Message; }
        finally { cancellation = null; }
        if (IsDisposed) return;
        await RefreshStatuses(); SetIdle(idle);
        if (failure is not null) MessageBox.Show(this, failure, source.Label, MessageBoxButtons.OK, MessageBoxIcon.Warning);
    }

    private async Task ChooseSource()
    {
        if (IsWorking) return;
        var source = Selected;
        using var dialog = new FolderBrowserDialog { Description = $"Choose your {source.SourceName} folder (it contains content\\tracks\\{source.SourceTrack})", UseDescriptionForTitle = true, ShowNewFolderButton = false };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        try { await Task.Run(() => CustomTracks.ChooseFolder(root, source, dialog.SelectedPath)); }
        catch (IOException error) { MessageBox.Show(this, error.Message, source.Label, MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
        await RefreshStatuses();
    }

    private async Task Uninstall()
    {
        if (IsWorking || !idle) return;
        var source = Selected;
        var shared = CustomTracks.Sharing(source);
        string also = shared.Length > 0 ? $" This also uninstalls {string.Join(" and ", shared.Select(s => s.Label))}, built together with it." : "";
        if (MessageBox.Show(this, $"Uninstall {source.Label}?{also} Your {source.SourceName} installation, GRID 2 and your saves are unchanged, and you can build it again later.",
                "Uninstall " + source.Label, MessageBoxButtons.OKCancel, MessageBoxIcon.Question, MessageBoxDefaultButton.Button2) != DialogResult.OK) return;
        try { await Task.Run(() => CustomTracks.Remove(root, source)); }
        catch (IOException error) { MessageBox.Show(this, error.Message, source.Label, MessageBoxButtons.OK, MessageBoxIcon.Warning); }
        await RefreshStatuses();
    }

    private void DrawTrack(object? sender, DrawItemEventArgs e)
    {
        if (e.Index < 0 || list.Items[e.Index] is not CustomTrackSource source) return;
        e.DrawBackground();
        statuses.TryGetValue(source.Id, out var value);
        var state = value is null ? "Checking…" : value.Installed is not null ? "Installed" : value.SourceFolder is null ? "Needs " + source.SourceName : "Not installed";
        var pad = LauncherAppearance.Px(this, 8); var line = (int)Math.Ceiling(Font.GetHeight());
        var bounds = new Rectangle(e.Bounds.Left + pad, e.Bounds.Top + pad, e.Bounds.Width - pad * 2, line);
        using var bold = new Font(Font, FontStyle.Bold);
        const TextFormatFlags flags = TextFormatFlags.EndEllipsis | TextFormatFlags.NoPrefix | TextFormatFlags.SingleLine;
        TextRenderer.DrawText(e.Graphics, source.Label, bold, bounds, e.ForeColor, flags);
        bounds.Offset(0, line + 2); TextRenderer.DrawText(e.Graphics, source.Summary, Font, bounds, e.ForeColor, flags);
        bounds.Offset(0, line + 2); TextRenderer.DrawText(e.Graphics, state, Font, bounds, e.ForeColor, flags);
    }

    // List beside the detail when there is room; above it otherwise.
    private sealed class TrackBrowser : Panel
    {
        private readonly ListBox list; private readonly VerticalStack detail; private readonly Func<int> rowHeight; private bool arranging;
        internal TrackBrowser(ListBox list, VerticalStack detail, Func<int> rowHeight)
        {
            this.list = list; this.detail = detail; this.rowHeight = rowHeight; Margin = new Padding(0, 0, 0, 12); TabStop = false;
            Controls.Add(list); Controls.Add(detail);
            detail.SizeChanged += (_, _) => PerformLayout();
        }
        protected override void OnLayout(LayoutEventArgs e)
        {
            base.OnLayout(e);
            if (arranging || ClientSize.Width <= 0) return;
            arranging = true;
            try
            {
                var gap = LauncherAppearance.Px(this, 20);
                // Not ListBox.GetItemHeight: during handle (re)creation the native list is still being
                // refilled and that call fails (Win32Exception 0 with three tracks in the preview).
                var listHeight = list.Items.Count * rowHeight() + 4;
                var wide = ClientSize.Width >= LauncherAppearance.Px(this, 620);
                var listWidth = wide ? Math.Min(LauncherAppearance.Px(this, 240), ClientSize.Width / 3) : ClientSize.Width;
                list.SetBounds(0, 0, listWidth, listHeight);
                var left = wide ? listWidth + gap : 0; var top = wide ? 0 : listHeight + gap;
                detail.SetBounds(left, top, Math.Max(1, ClientSize.Width - left), detail.Height);
                detail.PerformLayout();
                Height = Math.Max(wide ? listHeight : 0, top + detail.Height);
            }
            finally { arranging = false; }
        }
        public override Size GetPreferredSize(Size proposedSize) => new(Width, Height);
    }
}

// Status headline: green when the track is ready, in either theme.
internal sealed class TrackStatusLabel : Label
{
    private bool ready;
    [System.ComponentModel.DesignerSerializationVisibility(System.ComponentModel.DesignerSerializationVisibility.Hidden)]
    internal bool Ready { get => ready; set { ready = value; Invalidate(); } }
    protected override void OnPaint(PaintEventArgs e)
    {
        var dark = BackColor.GetBrightness() < 0.5f;
        var color = !ready ? ForeColor : dark ? Color.FromArgb(0x7C, 0xD9, 0x92) : Color.FromArgb(0x1E, 0x7B, 0x34);
        TextRenderer.DrawText(e.Graphics, Text, Font, new Rectangle(Padding.Left, Padding.Top, Width - Padding.Horizontal, Height - Padding.Vertical), color,
            TextFormatFlags.WordBreak | TextFormatFlags.NoPrefix | TextFormatFlags.Left);
    }
}
