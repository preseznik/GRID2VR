using System.Diagnostics;
using System.Text.Json;

namespace GRID2VR;

// Keep the native tab/field/footer vocabulary aligned with DiRT2VR's MainForm.
internal sealed partial class MainForm : LauncherForm
{
    private readonly SessionClient client;
    private readonly ProfilePanel profiles;
    private readonly UpdatesPanel updates;
    private readonly Button updateNotice = new() { Text = "New version available", Name = "UpdateAvailable", AutoSize = true, Visible = false, Anchor = AnchorStyles.Right };
    private AppRelease? availableUpdate;
    private readonly ComboBox car = Choice("PracticeCar"), track = Choice("PracticeTrack"), eventChoice = Choice("PracticeEvent"), runtime = Choice("Runtime");
    private readonly ComboBox launchMode = Choice("LaunchMode"), opponentCars = Choice("OpponentCars");
    private readonly Label opponentHint = TextLabel("");
    private readonly TextBox carSearch = new() { Name = "CarSearch", AccessibleName = "Find car", PlaceholderText = "Name, manufacturer or code", Dock = DockStyle.Fill };
    private readonly Label carCount = TextLabel("");
    private ContentItem[] cars = [];
    private ContentItem[] routes = [];
    private readonly ValueSlider laps = new("Laps", 1, 20, 1), opponents = new("Opponents", 1, 7, 7);
    private readonly Label lapsHint = TextLabel("");
    private readonly RenderScaleSlider renderScale = new() { Name = "VrRenderScale", AccessibleName = "VR render scale" };
    private readonly RenderScaleSlider desktopRenderScale = new() { Name = "DesktopRenderScale", AccessibleName = "Desktop render scale" };
    private readonly Button refreshHeadset = new() { Name = "RefreshHeadset", Text = "Refresh headset", AutoSize = true };
    private HeadsetResolution? headsetResolution;
    private string headsetResolutionStatus = "Connect your headset and select Refresh headset to read its resolution.";
    private bool queryingHeadset;
    private readonly Label vrResolutionSummary = TextLabel(""), desktopResolution = TextLabel("");
    private bool updatingResolution;
    private readonly ComboBox desktopMsaa = Choice("DesktopMSAA"), vrMsaa = Choice("VrMSAA"), treeDetail = Choice("TreeDetail"),
        objectDetail = Choice("ObjectDetail"), crowdDetail = Choice("CrowdDetail"), textureFiltering = Choice("TextureFiltering");
    private readonly CheckBox desktopVSync = new() { Name = "DesktopVSync", Text = "On", AutoSize = true };
    private readonly CheckBox desktopBorderless = new() { Name = "DesktopBorderless", Text = "Borderless fullscreen", AutoSize = true };
    private readonly CheckBox vrMirrorBorderless = new() { Name = "VrMirrorBorderless", Text = "Maximize windowed screen", AutoSize = true };
    private readonly Label measuredScene = TextLabel("No measured scene from the last session.");
    private readonly AppearanceSwitch modernInterface = new();
    private readonly CheckBox previewVrFixes = new() { Name = "PreviewVrFixes", Text = "Preview VR fixes", AutoSize = true };
    private readonly CheckBox skipIntro = new() { Name = "SkipIntro", AccessibleName = "Skip intro / logos", Text = "On", AutoSize = true };
    private readonly CheckBox diagnosticLogging = new() { Name = "DiagnosticLogging", AccessibleName = "Diagnostic logging", Text = "On", AutoSize = true };
    private readonly ChaseCameraPanel chaseCamera = new();
    private readonly CheckBox performanceFollowView = new() { Name = "PerformanceFollowView", AccessibleName = "Performance display follows view", Text = "Follow view", AutoSize = true };
    private readonly CheckBox performanceDisplay = new() { Name = "PerformanceDisplay", AccessibleName = "Performance display", Text = "FPS + GPU usage (VR)", AutoSize = true };
    private readonly CheckBox replay3d = new() { Name = "VrReplayCameras", AccessibleName = "3D replays", Text = "On", Checked = true, AutoSize = true };
    private readonly CheckBox garage3d = new() { Name = "VrGarageMenus", AccessibleName = "3D menus", Text = "On", Checked = true, AutoSize = true };
    private readonly CheckBox vrCockpit = new() { Name = "VrCockpit", AccessibleName = "Cockpit view in VR", Text = "On (default)", Checked = true, AutoSize = true };
    private readonly ComboBox cockpitDriver = Choice("CockpitDriver");
    private readonly ComboBox cockpitTextures = Choice("CockpitTextures");
    private readonly CheckBox cockpitExposure = new() { Name = "CockpitExposureLimit", Text = "Limit cockpit exposure", Checked = true, AutoSize = true };
    private readonly CheckBox impactGlitch = new() { Name = "HideImpactGlitch", Text = "Hide impact glitch effect", Checked = true, AutoSize = true };
    private readonly SeatAlignmentPanel seatAlignment = new();
    private readonly Button drivingControls = Button("Driving bindings…", "DrivingControls");
    private readonly HudDistanceSlider hudDistance = new();
    private readonly CheckBox hudFollow = new() { Name = "HudFollowView", Text = "On", AutoSize = true };
    private GraphicsOptions retainedGraphics = new();
    private readonly Button graphicsDefaults = Button("Restore graphics defaults", "GraphicsDefaults");
    private readonly SelectableStatusText state = new() { Name = "SessionStatus", AccessibleName = "Session status", Text = "Reading session state…" },
        notice = new() { Name = "SessionNotice", AccessibleName = "Session message" };
    private readonly Button desktop = Button("Launch", "LaunchDesktop"), vr = Button("Launch VR", "LaunchVR"),
        save = Button("Save settings", "SaveSettings"), recover = Button("Restore original files", "RestoreOriginalFiles"),
        recenter = Button("F10", "RecenterKey"), stop = Button("Stop VR", "StopVR"), end = Button("End session", "EndSession");
    private readonly Label recenterHint = TextLabel("");
    private readonly CheckBox keepHorizonLevel = new() { Name = "KeepHorizonLevel", Text = "Keep horizon level when recentering", Checked = true, AutoSize = true };
    private readonly Label requestedSession = TextLabel("");
    private readonly Button defaults = Button("Restore defaults", "LauncherDefaults");
    private readonly Label layerBypassSummary = TextLabel("");
    private readonly Button manageLayers = Button("Manage layer bypasses…", "ManageVrCompatibility");
    private readonly ToolTip tips = new() { InitialDelay = 450, ReshowDelay = 150, AutoPopDelay = 15000, ShowAlways = true };
    private int recenterKey = 121, recenterModifiers;
    private bool capturingRecenter;
    private DrivingBinding? recenterDevice;
    private readonly Button bindRecenterDevice = Button("Bind controller / wheel…", "RecenterDevice"), clearRecenterDevice = Button("Clear device binding", "ClearRecenterDevice");
    private readonly Label deviceRecenterHint = TextLabel("No controller / wheel binding.");
    private readonly TextBox details = new() { Dock = DockStyle.Fill, ReadOnly = true, Multiline = true, ScrollBars = ScrollBars.Vertical };
    private readonly System.Windows.Forms.Timer timer = new() { Interval = 2000 };
    private bool loaded, busy, polling, pending, active, headset, rendering, stopped, known;
    private DateTime launchGrace;
    private Process? launchWorker;
    private LaunchProgressForm? launchProgress;
    private bool launchingVr;
    private LauncherWindow window = new();
    private readonly List<SectionHeader> sectionHeaders = [];
    private bool windowLoaded;

    public MainForm(SessionClient value, ProfileStore? profileStore = null, Func<bool, CancellationToken, Task<AppRelease?>>? releaseCheck = null,
        Func<bool>? approveAdvancedView = null)
    {
        client = value;
        confirmAdvancedView = approveAdvancedView ?? ConfirmAdvancedView;
        updates = new UpdatesPanel(client.Root, releaseCheck: releaseCheck);
        updates.UpdateAvailable += ShowUpdateNotice;
        profiles = new ProfilePanel(profileStore ?? ProfileService.Open(client.Root));
        Text = "GRID2VR — " + BuildInfo.Channel; Font = new Font("Segoe UI", 10);
        previewVrFixes.Visible = !AppPaths.Packaged(client.Root);
        KeyPreview = true;
        AutoScaleMode = AutoScaleMode.Dpi; MinimumSize = new Size(560, 540);
        ClientSize = new Size(900, 720); StartPosition = FormStartPosition.CenterScreen;
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(20), ColumnCount = 1, RowCount = 5 };
        layout.ColumnStyles.Add(new(SizeType.Percent, 100));
        foreach (var size in new[] { SizeType.AutoSize, SizeType.AutoSize, SizeType.Percent, SizeType.AutoSize, SizeType.AutoSize })
            layout.RowStyles.Add(new(size, size == SizeType.Percent ? 100 : 0));
        var title = TextLabel("GRID 2 VR"); title.Font = new Font(Font.FontFamily, 20, FontStyle.Bold);
        var help = new Button { Text = "?", Name = "HelpAbout", AccessibleName = "Help / About", Size = new Size(36, 36), Anchor = AnchorStyles.Right };
        help.Click += (_, _) => ShowAbout();
        updateNotice.Click += (_, _) => ShowAbout();
        var header = new LauncherHeader(Branding.Logo(64), title, updateNotice, viewPicker, help);
        HelpRequested += (_, e) => { e.Handled = true; ShowAbout(); };
        layout.Controls.Add(header); state.Margin = new Padding(0, 8, 0, 16); layout.Controls.Add(state);
        var tabs = new ModernTabs { Name = "LauncherTabs", Dock = DockStyle.Fill };
        layout.Controls.Add(tabs);
        var selectionColumns = Columns(Page(tabs, "Launcher")); var selection = selectionColumns.First;
        Section(selection, "Event selection");
        launchMode.Items.AddRange(new ContentItem[] { new("Normal", "Normal"), new("Practice", "Practice"), new("Race", "Race") });
        launchMode.SelectedIndex = 1;
        Field(selection, "Launch mode", launchMode);
        Field(selection, "Event", eventChoice); Field(selection, "Track", track);
        Field(selection, "Find car", carSearch); Field(selection, "Car", car); selection.Controls.Add(carCount);
        selection = selectionColumns.Second;
        Section(selection, "Race options");
        Field(selection, "Opponents", opponents);
        opponentCars.Items.AddRange(new ContentItem[] { new("same", "Same car"), new("mixed", "Mixed"), new("class", "Same class") });
        opponentCars.SelectedIndex = 0;
        Field(selection, "Opponent cars", opponentCars);
        opponentHint.Name = "OpponentHint"; selection.Controls.Add(opponentHint);
        Field(selection, "Laps (circuits)", laps); selection.Controls.Add(lapsHint);
        Field(selection, "Conditions", TextLabel("Day / dry"));
        Section(selection, "Requested session");
        requestedSession.Name = "RequestedSession"; selection.Controls.Add(requestedSession);
        selection.Controls.Add(TextLabel("Normal opens the game's menus. Practice starts solo; Race adds computer-controlled opponents. Car, track and laps apply to Practice and Race."));
        selection.Controls.Add(TextLabel("The session runs until you quit. Closing this launcher leaves it running; reopen the launcher to reconnect."));
        selection.Controls.Add(HelpLink("Getting started"));

        Page(tabs, "Profile").Controls.Add(profiles);
        profiles.Controls.Add(HelpLink("Profile"));
        tabs.SelectedIndexChanged += (_, _) => { if (tabs.SelectedTab?.Text == "Profile") profiles.Reload(); };
        var graphicsColumns = Columns(Page(tabs, "Graphics")); var graphics = graphicsColumns.First;
        InitializeGraphicsPresets(graphics);
        Section(graphics, "VR rendering");
        Field(graphics, "VR render scale", renderScale);
        graphics.Controls.Add(refreshHeadset);
        vrResolutionSummary.Name = "VrResolutionSummary";
        Field(graphics, "Next VR launch", vrResolutionSummary);
        Field(graphics, "Measured scene", measuredScene);
        vrMsaa.Items.AddRange(new ContentItem[] { new("off", "Off (default)"), new("2xmsaa", "2× MSAA"), new("4xmsaa", "4× MSAA"), new("8xmsaa", "8× MSAA") });
        Field(graphics, "Anti-aliasing (VR)", vrMsaa);
        measuredScene.Tag = vrResolutionSummary.Tag = desktopResolution.Tag = "hint";
        graphics.Controls.Add(TextLabel("100% renders at the connected headset’s recommended resolution. Lower values trade detail for performance; higher values can improve sharpness and use more GPU time and memory. Headset output stays at 100%. Changes apply on the next VR launch."));
        graphics.Controls.Add(TextLabel("VR anti-aliasing is independent of desktop MSAA and defaults to Off. Higher MSAA uses more GPU time and memory. VR keeps motion blur off; quality follows your launcher choices."));
        graphics.Controls.Add(impactGlitch);
        graphics.Controls.Add(TextLabel("Hard impacts flash a screen-wide glitch that looks like a second flat picture over the scene in VR. Hidden by default; untick to keep the game's effect. Applies on the next VR launch."));
        Section(graphics, "VR mirror");
        graphics.Controls.Add(vrMirrorBorderless);
        graphics.Controls.Add(TextLabel("Fill the mirror's monitor without borders on the next VR launch. VR resolution stays independent of window size."));
        graphics = graphicsColumns.Second;
        Section(graphics, "Detail");
        Field(graphics, "Tree detail", treeDetail); Field(graphics, "Object detail", objectDetail);
        Field(graphics, "Crowd detail", crowdDetail); Field(graphics, "Texture filtering", textureFiltering);
        InitializeQualityEditors(graphics);
        graphics = graphicsColumns.First;
        Section(graphics, "VR cockpit");
        Field(graphics, "Cockpit view", vrCockpit);
        cockpitDriver.Items.AddRange(new ContentItem[] { new("HeadOnly", "Hide head (default)"), new("HideDriver", "Hide entire driver") });
        cockpitDriver.SelectedIndex = 0;
        Field(graphics, "Driver visibility", cockpitDriver);
        graphics.Controls.Add(cockpitExposure);
        graphics.Controls.Add(TextLabel("Keep the track's exposure ceiling in cockpit view so the dark interior cannot over-brighten the outside. Exposure still adapts below that limit. Applies on the next VR launch."));
        vrCockpit.CheckedChanged += (_, _) => seatAlignment.Enabled = cockpitExposure.Enabled = cockpitDriver.Enabled = vrCockpit.Enabled && vrCockpit.Checked;
        graphics.Controls.Add(TextLabel("Calibrated cars start VR in the cockpit. Use your camera-cycle control to change views. DLC cars without calibration keep their original cameras. Applies to the next VR launch, including cars chosen through the game's menus."));
        graphics.Controls.Add(HelpLink("VR cockpit"));
        graphics.Controls.Add(TextLabel("Adjust individual cars in Controls → Seat."));
        Section(graphics, "Cockpit textures");
        cockpitTextures.Items.AddRange(new ContentItem[] { new("Off", "Old cockpit textures"), new("Replace", "New cockpit textures") });
        cockpitTextures.SelectedIndex = 0;
        Field(graphics, "Cockpit textures", cockpitTextures);
        graphics.Controls.Add(TextLabel("Choose New cockpit textures to download and install the optional pack automatically (123 MB, once). Applies to the next desktop or VR launch. Old cockpit textures restores the original artwork."));
        cockpitTextures.SelectionChangeCommitted += async (_, _) => await Act(() =>
        {
            if (EnsureCockpitTextures()) notice.Text = ChoiceId(cockpitTextures) == "Replace"
                ? "New cockpit textures are ready. Save settings or launch to use them."
                : "Original cockpit textures selected. Save settings or launch to use them.";
            return Task.CompletedTask;
        });
        graphics = graphicsColumns.Second;
        Section(graphics, "VR HUD");
        Field(graphics, "Distance", hudDistance); Field(graphics, "Follow view", hudFollow);
        graphics.Controls.Add(TextLabel("Distance and Follow view apply to the HUD and results text in stereo VR. Individual HUD elements cannot be hidden yet."));
        graphics.Controls.Add(HelpLink("VR HUD"));
        Section(graphics, "Desktop");
        Field(graphics, "Window", desktopBorderless);
        Field(graphics, "Render resolution", desktopRenderScale); Field(graphics, "Requested desktop", desktopResolution);
        desktopMsaa.Items.AddRange(new ContentItem[] { new("off", "Off"), new("2xmsaa", "2× MSAA"), new("4xmsaa", "4× MSAA"), new("8xmsaa", "8× MSAA") });
        Field(graphics, "VSync", desktopVSync); Field(graphics, "Anti-aliasing (MSAA)", desktopMsaa);
        graphics.Controls.Add(TextLabel("Borderless fullscreen fills the game window's monitor. Turn it off for a bordered window. Desktop settings apply only to Launch; VR has separate settings."));
        graphics.Controls.Add(graphicsDefaults);
        graphics.Controls.Add(TextLabel("VR defaults to 100% of the connected headset’s recommended resolution. Changes apply to the next launch; original game settings are restored afterward."));
        renderScale.ValueChanged += (_, _) => UpdateResolutionLabels();
        refreshHeadset.Click += async (_, _) => await RefreshHeadsetResolution();
        desktopRenderScale.ValueChanged += (_, _) => UpdateResolutionLabels();
        graphicsDefaults.Click += (_, _) => { ClearGraphicsPresetUndo(); ApplyGraphics(GraphicsOptions.Defaults); vrCockpit.Checked = true; cockpitExposure.Checked = true; impactGlitch.Checked = true; SelectChoice(cockpitDriver, "HeadOnly"); SelectChoice(cockpitTextures, "Off"); notice.Text = "Graphics defaults restored. Save settings or launch to keep them."; };
        ApplyGraphics(GraphicsOptions.Defaults);
        InitializeGraphicsEditTracking();
        graphics.Controls.Add(HelpLink("VR rendering"));
        var controlPage = new TabPage("Controls") { Padding = new Padding(8) };
        var controlTabs = new ModernTabs { Name = "ControlTabs", Dock = DockStyle.Fill };
        controlPage.Controls.Add(controlTabs); tabs.TabPages.Add(controlPage);
        var controls = Page(controlTabs, "General");
        controls.Controls.Add(chaseCamera);
        Section(controls, "Session controls");
        controls.Controls.Add(end);
        controls.Controls.Add(TextLabel("Use Quit in the game for a normal exit. End session stops VR, closes this session's game process and restores original files."));
        controls.Controls.Add(HelpLink("Controls"));
        controls = Page(controlTabs, "VR");
        Section(controls, "Keyboard shortcuts");
        Field(controls, "Recenter", recenter);
        controls.Controls.Add(recenterHint); UpdateRecenterLabel();
        controls.Controls.Add(TextLabel("Click the key above to change it before launching. Press Escape to cancel. Save settings or launch to keep your binding; changes apply to the next session."));
        Section(controls, "Controller / wheel recenter");
        controls.Controls.Add(deviceRecenterHint); controls.Controls.Add(bindRecenterDevice); controls.Controls.Add(clearRecenterDevice);
        controls.Controls.Add(TextLabel("Bind a button or hat direction on a controller, wheel or button box. The keyboard shortcut stays available. Select an unused button: its normal game action is not blocked. Applies on the next VR launch and works with the launcher closed."));
        Section(controls, "Recenter alignment");
        controls.Controls.Add(keepHorizonLevel);
        controls.Controls.Add(TextLabel("Reset position and forward direction without changing the horizon. Uncheck to align pitch and roll to your current head pose too. Applies to keyboard, controller and wheel recentering on the next VR launch."));
        Section(controls, "VR session");
        controls.Controls.Add(stop);
        controls.Controls.Add(TextLabel("Stop VR releases the headset and leaves GRID 2 running; launch again after quitting to return to VR."));
        controls = Page(controlTabs, "Driving");
        Section(controls, "Driving controls");
        controls.Controls.Add(drivingControls);
        controls.Controls.Add(TextLabel(DrivingControls.RuntimeNotice));
        controls.Controls.Add(HelpLink("Controls"));
        controls = Page(controlTabs, "Seat");
        controls.Controls.Add(seatAlignment);
        controls.Controls.Add(HelpLink("VR cockpit"));

        var settingColumns = Columns(Page(tabs, "Settings")); var settings = settingColumns.First;
        Section(settings, "Paths and runtime");
        Field(settings, "Game folder", new TextBox { Text = AppPaths.Game(client.Root), ReadOnly = true, Dock = DockStyle.Fill });
        runtime.Items.AddRange(["VDXR", "SteamVR"]); runtime.SelectedIndex = 0;
        Field(settings, "OpenXR runtime", runtime);
        settings.Controls.Add(TextLabel("VDXR: connect through Virtual Desktop and keep SteamVR closed. SteamVR: connect the headset through SteamVR. The choice applies only to this session."));
        settings.Controls.Add(TextLabel("Uses your installed GRID 2. Settings and careers are kept when updating."));
        Section(settings, "VR compatibility");
        layerBypassSummary.Name = "VrCompatibilitySummary";
        settings.Controls.Add(layerBypassSummary); settings.Controls.Add(manageLayers);
        settings.Controls.Add(TextLabel("If an OpenXR add-on such as ReShade or OBS Mirror stops VR from starting, Launch VR names it and offers to disable it for GRID2VR only. Other applications keep their settings."));
        UpdateLayerBypassSummary();
        Section(settings, "Startup & diagnostics");
        Field(settings, "Skip intro / logos", skipIntro);
        Field(settings, "Diagnostic logging", diagnosticLogging);
        settings.Controls.Add(TextLabel("Skip intro / logos skips startup movies in all launch modes. Pre-race camera flythroughs and the first career race remain. Diagnostic logging saves extra troubleshooting details; essential recovery receipts are kept with it off. Changes apply to the next launch."));
        settings = settingColumns.Second;
        if(!AppPaths.Packaged(client.Root))
        {
            Section(settings, "VR preview");
            settings.Controls.Add(previewVrFixes);
            settings.Controls.Add(TextLabel("Trackside visibility, race-finish and shutdown fixes for any selectable car, track, runtime and graphics settings. Uses your choices without changing them. Desktop Launch is unaffected."));
        }
        previewVrFixes.CheckedChanged += (_, _) => UpdateRequestedSession();
        Section(settings, "Launcher preferences");
        Field(settings, "Modern interface", modernInterface);
        settings.Controls.Add(TextLabel("Preview the red-accented Modern interface, or switch off for Classic. Save settings or launch to keep your choice. Both follow Windows light/dark and high-contrast settings."));
        modernInterface.CheckedChanged += (_, _) =>
        {
            LauncherAppearance.Modern = modernInterface.Checked;
            LauncherAppearance.Apply(this);
        };
        settings.Controls.Add(defaults);
        settings.Controls.Add(TextLabel("Reset all launcher choices, including startup and diagnostics options. Save settings or launch to keep the reset. Installed game settings and driving bindings are unchanged."));
        settings.Controls.Add(HelpLink("Settings and recovery"));
        Section(settings, "Updates"); settings.Controls.Add(updates);

        var advancedColumns = Columns(Page(tabs, "Advanced")); var advancedLayout = advancedColumns.First;
        Section(advancedLayout, "VR presentation");
        Field(advancedLayout, "Performance display", performanceDisplay);
        Field(advancedLayout, "Display placement", performanceFollowView);
        performanceDisplay.CheckedChanged += (_, _) => EnableActions();
        var performanceHint = TextLabel("Show app FPS and GPU usage in the headset on the next VR launch. GPU usage is adapter-wide; unavailable readings show N/A. Turn off Follow view to keep the panel level and fixed below your recentered forward view. Recenter to reposition it.");
        advancedLayout.Controls.Add(performanceHint);
        Field(advancedLayout, "3D replays", replay3d);
        Field(advancedLayout, "3D menus", garage3d);
        advancedLayout = advancedColumns.Second;
        Section(advancedLayout, "Session details");
        details.Height = 190;
        advancedLayout.Controls.Add(details);
        notice.Margin = new Padding(0, 10, 0, 8); layout.Controls.Add(notice);
        // Same footer order and labels as DiRT2VR; wraps on smaller windows.
        var logs = Button("Open logs", "OpenLogs");
        layout.Controls.Add(new LauncherFooter(desktop, vr, save, recover, logs)); Controls.Add(layout);
        InitializeViews(tabs);
        void FitStatusLabels()
        {
            var width = Math.Max(1, layout.ClientSize.Width - layout.Padding.Horizontal);
            state.FitWidth(width); notice.FitWidth(width);
        }
        layout.SizeChanged += (_, _) => FitStatusLabels(); FitStatusLabels();
        eventChoice.SelectedIndexChanged += (_, _) =>
        {
            var previous = (track.SelectedItem as ContentItem)?.Id;
            track.Items.Clear();
            var prefix = (eventChoice.SelectedItem as ContentItem)?.Id + "/";
            var choices = routes.Where(t => t.Id.StartsWith(prefix, StringComparison.Ordinal)).ToArray();
            track.Items.AddRange(choices);
            track.SelectedItem = choices.FirstOrDefault(t => t.Id == previous) ?? choices.FirstOrDefault();
        };
        car.SelectedIndexChanged += (_, _) => UpdateRequestedSession();
        carSearch.TextChanged += (_, _) => FilterCars();
        track.SelectedIndexChanged += (_, _) => { UpdateRequestedSession(); EnableActions(); };
        runtime.SelectedIndexChanged += async (_, _) => {
            UpdateRequestedSession(); UpdateResolutionLabels();
            if (loaded) await RefreshHeadsetResolution();
        };
        laps.ValueChanged += (_, _) => UpdateRequestedSession();
        opponents.ValueChanged += (_, _) => UpdateRequestedSession();
        opponentCars.SelectedIndexChanged += (_, _) => UpdateRequestedSession();
        launchMode.SelectedIndexChanged += (_, _) => { UpdateRequestedSession(); EnableActions(); };

        desktop.Click += async (_, _) => await Act(() => Launch(false));
        vr.Click += async (_, _) => await Act(() => Launch(true));
        save.Click += async (_, _) => await Act(() => { if (EnsureCockpitTextures()) { seatAlignment.Save(client.Root); client.Save(Current()); notice.Text = "Settings saved. Changes apply to the next session."; } return Task.CompletedTask; });
        drivingControls.Click += async (_, _) => await Act(() => { using var dialog = new DrivingControlsForm(client.Root); dialog.ShowDialog(this); return Task.CompletedTask; });
        manageLayers.Click += async (_, _) => await Act(() => { using var dialog = new VrCompatibilityForm(client.Artifacts); dialog.ShowDialog(this); UpdateLayerBypassSummary(); return Task.CompletedTask; });
        Activated += (_, _) => UpdateLayerBypassSummary();
        recenter.Click += (_, _) => { capturingRecenter = true; recenter.Text = "Press a key…"; EnableActions(); };
        bindRecenterDevice.Click += (_, _) =>
        {
            using var dialog = new DrivingBindingForm("Recenter", false, deviceButtonOnly: true);
            if (dialog.ShowDialog(this) == DialogResult.OK && dialog.Bindings.FirstOrDefault() is { } binding)
            { DeviceRecenter.Validate(binding); recenterDevice = binding; UpdateDeviceRecenter(); }
        };
        clearRecenterDevice.Click += (_, _) => { recenterDevice = null; UpdateDeviceRecenter(); };
        defaults.Click += (_, _) =>
        {
            ApplyChoices(new Choices());
            notice.Text = "Launcher defaults restored. Save settings or launch to keep them.";
        };
        stop.Click += async (_, _) => await Send("StopVR", "VR stop requested. GRID 2 will remain open.");
        end.Click += async (_, _) => await Send("End", "Ending this session and restoring original files…");
        recover.Click += async (_, _) => await Send("Recover", "Original files restored. User save progress retained.");
        logs.Click += async (_, _) => await Act(() => { Process.Start(new ProcessStartInfo(client.LogsFolder()) { UseShellExecute = true }); return Task.CompletedTask; });
        timer.Tick += async (_, _) => await RefreshStatus();
        FormClosed += (_, _) => { timer.Stop(); timer.Dispose(); tips.Dispose(); launchProgress?.Dispose(); launchWorker?.Dispose(); launchWorker = null; };
        SizeChanged += (_, _) =>
        {
            if (!windowLoaded || WindowState == FormWindowState.Minimized) return;
            window = WindowState == FormWindowState.Maximized ? window with { Maximized = true }
                : LauncherWindow.FromClientSize(ClientSize, DeviceDpi) with { AdvancedView = window.AdvancedView, CollapsedSections = window.CollapsedSections };
        };
        FormClosing += (_, _) =>
        {
            if (!windowLoaded) return;
            try { client.SaveWindow(window); }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException) { Debug.WriteLine("Window size could not be saved: " + error.Message); }
        };
        Shown += async (_, _) =>
        {
            var updateCheck = updates.Start();
            RestoreWindow();
            profiles.Reload();
            await Act(async () =>
            {
                var catalog = await client.Catalog();
                cars = catalog.Cars; FilterCars(); routes = catalog.Tracks;
                var events = routes.Select(t => t.Id[..t.Id.LastIndexOf('/')]).Distinct()
                    .Select(id => new ContentItem(id, SessionClient.LocationLabel(id))).OrderBy(e => e.Label).ToArray();
                eventChoice.Items.AddRange(events);
                ApplyChoices(client.Load());
                seatAlignment.LoadCars(cars, (car.SelectedItem as ContentItem)?.Id ?? "s20", SeatAlignment.Load(client.Root));
                loaded = car.SelectedItem != null && track.SelectedItem != null;
                if (!loaded) throw new IOException("No compatible installed practice content found.");
            });
            await RefreshStatus(); timer.Start();
            await RefreshHeadsetResolution();
            await updateCheck;
        };
        ConfigureTooltips(help, logs);
        UpdateRequestedSession();
        EnableActions();
    }

    private void RestoreWindow()
    {
        window = client.LoadWindow();
        var work = Screen.FromControl(this).WorkingArea;
        MinimumSize = new Size(Math.Min(LauncherAppearance.Px(this, 560), work.Width), Math.Min(LauncherAppearance.Px(this, 540), work.Height));
        var available = new Size(work.Width - (Width - ClientSize.Width), work.Height - (Height - ClientSize.Height));
        ClientSize = window.ClientSizeAtDpi(DeviceDpi, available);
        Location = new Point(work.Left + (work.Width - Width) / 2, work.Top + (work.Height - Height) / 2);
        var advanced = client.UseAdvancedView(window);
        var collapsed = window.CollapsedSections;
        window = LauncherWindow.FromClientSize(ClientSize, DeviceDpi) with { Maximized = window.Maximized, AdvancedView = advanced, CollapsedSections = collapsed };
        ApplyCollapsedSections(collapsed);
        viewPicker.Advanced = advanced;
        if (window.Maximized) WindowState = FormWindowState.Maximized;
        windowLoaded = true;
    }

    private void ApplyChoices(Choices choices)
    {
        ClearGraphicsPresetUndo();
        carSearch.Clear();
        car.SelectedItem = car.Items.Cast<ContentItem>().FirstOrDefault(c => c.Id == choices.Car) ?? car.Items.Cast<ContentItem>().FirstOrDefault();
        eventChoice.SelectedItem = eventChoice.Items.Cast<ContentItem>().FirstOrDefault(e => choices.Track.StartsWith(e.Id + "/", StringComparison.Ordinal)) ?? eventChoice.Items.Cast<ContentItem>().FirstOrDefault();
        track.SelectedItem = track.Items.Cast<ContentItem>().FirstOrDefault(t => t.Id == choices.Track) ?? track.Items.Cast<ContentItem>().FirstOrDefault();
        runtime.SelectedItem = choices.Runtime;
        laps.Value = choices.Laps;
        opponents.Value = choices.Opponents; SelectChoice(launchMode, choices.LaunchMode);
        SelectChoice(opponentCars, choices.OpponentCars);
        ApplyGraphics(choices.Graphics);
        modernInterface.Checked = choices.ModernInterface;
        previewVrFixes.Checked = choices.PreviewVrFixes;
        skipIntro.Checked = choices.SkipIntro;
        diagnosticLogging.Checked = choices.DiagnosticLogging;
        performanceDisplay.Checked = choices.PerformanceDisplay;
        performanceFollowView.Checked = choices.PerformanceFollowView;
        replay3d.Checked = choices.VrReplayCameras;
        garage3d.Checked = choices.VrGarageMenus;
        vrCockpit.Checked = choices.VrCockpit;
        SelectChoice(cockpitDriver, choices.CockpitDriver);
        cockpitDriver.Enabled = choices.VrCockpit;
        cockpitExposure.Checked = choices.CockpitExposureLimit;
        impactGlitch.Checked = choices.HideImpactGlitch;
        SelectChoice(cockpitTextures, choices.CockpitTextures);
        cockpitExposure.Enabled = choices.VrCockpit;
        recenterKey = choices.RecenterKey; recenterModifiers = choices.RecenterModifiers; UpdateRecenterLabel();
        recenterDevice = choices.RecenterDevice; UpdateDeviceRecenter();
        keepHorizonLevel.Checked = choices.KeepHorizonLevel;
        chaseCamera.LoadChoices(choices);
    }
    private void FilterCars()
    {
        var previous = (car.SelectedItem as ContentItem)?.Id;
        var matches = LauncherPresentation.FilterCars(cars, carSearch.Text);
        car.BeginUpdate();
        try
        {
            car.Items.Clear(); car.Items.AddRange(matches);
            car.SelectedItem = matches.FirstOrDefault(c => c.Id == previous) ?? matches.FirstOrDefault();
        }
        finally { car.EndUpdate(); }
        carCount.Text = matches.Length == 0 ? "No matching cars. Clear or change the search." : $"{matches.Length} of {cars.Length} installed cars";
        UpdateRequestedSession(); EnableActions();
    }
    private void UpdateRequestedSession()
    {
        var vehicle = car.SelectedItem as ContentItem;
        var mode = ChoiceId(launchMode);
        var rivals = ChoiceId(opponentCars);
        opponentHint.Text = LauncherPresentation.OpponentHint(vehicle, cars, mode, rivals);
        requestedSession.Text = LauncherPresentation.RequestedSession(
            vehicle, track.SelectedItem as ContentItem, runtime.SelectedItem as string, recenterKey, recenterModifiers, laps.Value,
            mode, opponents.Value, rivals) + (!AppPaths.Packaged(client.Root) && previewVrFixes.Checked ? "\nPreview VR fixes enabled" : "");
    }
    private Choices Current() => chaseCamera.ApplyTo(new(((ContentItem)car.SelectedItem!).Id, ((ContentItem)track.SelectedItem!).Id, (string)runtime.SelectedItem!, recenterKey, recenterModifiers, (int)laps.Value)
    {
        Graphics = CurrentGraphics(), ModernInterface = modernInterface.Checked, PreviewVrFixes = previewVrFixes.Checked,
        SkipIntro = skipIntro.Checked, DiagnosticLogging = diagnosticLogging.Checked, PerformanceDisplay = performanceDisplay.Checked, PerformanceFollowView = performanceFollowView.Checked,
        LaunchMode = ChoiceId(launchMode), Opponents = opponents.Value, OpponentCars = ChoiceId(opponentCars), VrReplayCameras = replay3d.Checked, VrGarageMenus = garage3d.Checked,
        VrCockpit = vrCockpit.Checked, CockpitDriver = ChoiceId(cockpitDriver), CockpitExposureLimit = cockpitExposure.Checked, HideImpactGlitch = impactGlitch.Checked, CockpitTextures = ChoiceId(cockpitTextures), RecenterDevice = recenterDevice, KeepHorizonLevel = keepHorizonLevel.Checked
    });
    private static string ChoiceId(ComboBox box) => ((ContentItem)box.SelectedItem!).Id;
    private static void SelectChoice(ComboBox box, string id) => box.SelectedItem = box.Items.Cast<ContentItem>().Single(x => x.Id == id);
    private GraphicsOptions CurrentGraphics() => CurrentQuality(retainedGraphics with
    {
        VrRenderScale = renderScale.Value, DesktopVSync = desktopVSync.Checked, DesktopMsaa = ChoiceId(desktopMsaa), VrMsaa = ChoiceId(vrMsaa),
        DesktopRenderScale = desktopRenderScale.Value == retainedGraphics.RenderScale && retainedGraphics.DesktopRenderScale is null ? null : desktopRenderScale.Value,
        VrMirrorBorderless = vrMirrorBorderless.Checked,
        DesktopBorderless = desktopBorderless.Checked,
        TreeDetail = ChoiceId(treeDetail), ObjectDetail = ChoiceId(objectDetail), CrowdDetail = ChoiceId(crowdDetail), TextureFiltering = ChoiceId(textureFiltering),
        HudDistance = hudDistance.Value, HudFollowView = hudFollow.Checked
    });
    private void ApplyGraphics(GraphicsOptions value)
    {
        updatingResolution = true;
        try
        {
            retainedGraphics = value;
            renderScale.Value = value.VrRenderScale;
            desktopRenderScale.Value = value.EffectiveDesktopRenderScale;
            vrMirrorBorderless.Checked = value.VrMirrorBorderless;
            desktopBorderless.Checked = value.DesktopBorderless;
            desktopVSync.Checked = value.DesktopVSync; SelectChoice(desktopMsaa, value.DesktopMsaa);
            SelectChoice(vrMsaa, value.VrMsaa);
            SelectChoice(treeDetail, value.TreeDetail); SelectChoice(objectDetail, value.ObjectDetail);
            SelectChoice(crowdDetail, value.CrowdDetail); SelectChoice(textureFiltering, value.TextureFiltering);
            foreach (var category in GraphicsCatalog.Categories)
                SelectChoice(qualityEditors[category.Field], GraphicsCatalog.Get(value, category.Field));
            hudDistance.Value = value.HudDistance; hudFollow.Checked = value.HudFollowView;
        }
        finally { updatingResolution = false; }
        UpdateResolutionLabels();
    }
    private void UpdateResolutionLabels()
    {
        if (updatingResolution) return;
        UpdateGraphicsPreset();
        desktopResolution.Text = new GraphicsOptions(RenderScale: desktopRenderScale.Value).SceneLabel;
        vrResolutionSummary.Text = headsetResolution is not null && headsetResolution.Runtime == runtime.SelectedItem as string
            ? headsetResolution.Summary(renderScale.Value) : headsetResolutionStatus;
    }
    private async Task RefreshHeadsetResolution()
    {
        if (!loaded || active || pending || busy || queryingHeadset || IsDisposed || runtime.SelectedItem is not string selectedRuntime) return;
        queryingHeadset = true;
        headsetResolution = null;
        headsetResolutionStatus = "Reading connected headset resolution...";
        UpdateResolutionLabels(); EnableActions();
        try {
            var value = await client.ReadHeadsetResolution(selectedRuntime);
            if (!IsDisposed && runtime.SelectedItem as string == selectedRuntime) headsetResolution = value;
        }
        catch (Exception error) {
            if (!IsDisposed) headsetResolutionStatus = error.Message;
        }
        finally {
            queryingHeadset = false;
            if (!IsDisposed) { UpdateResolutionLabels(); EnableActions(); }
        }
        if (!IsDisposed && runtime.SelectedItem as string != selectedRuntime) await RefreshHeadsetResolution();
    }
    private void UpdateRecenterLabel()
    {
        recenter.Text = RecenterShortcut.Label(recenterKey, recenterModifiers);
        recenterHint.Text = $"Press {recenter.Text} in GRID 2 to recenter. Sit comfortably and face forward. The shortcut also works with the launcher closed.";
        UpdateRequestedSession();
    }
    private void UpdateDeviceRecenter() => deviceRecenterHint.Text = recenterDevice is null ? "No controller / wheel binding." : DrivingInput.Description(recenterDevice);
    protected override bool ProcessCmdKey(ref Message message, Keys keyData)
    {
        if (!capturingRecenter) return base.ProcessCmdKey(ref message, keyData);
        var key = (int)(keyData & Keys.KeyCode);
        if (key == (int)Keys.Escape) { capturingRecenter = false; UpdateRecenterLabel(); EnableActions(); return true; }
        if (key is (int)Keys.ControlKey or (int)Keys.ShiftKey or (int)Keys.Menu or (int)Keys.LWin or (int)Keys.RWin) return true;
        var modifiers = ((keyData & Keys.Control) != 0 ? 1 : 0) | ((keyData & Keys.Alt) != 0 ? 2 : 0) | ((keyData & Keys.Shift) != 0 ? 4 : 0);
        if (!RecenterShortcut.Valid(key, modifiers)) { notice.Text = "Choose another key; system shortcuts are reserved."; return true; }
        recenterKey = key; recenterModifiers = modifiers; capturingRecenter = false;
        UpdateRecenterLabel(); EnableActions(); notice.Text = "Recenter shortcut changed. Save settings or launch to keep it.";
        return true;
    }
    protected override void OnDeactivate(EventArgs e)
    {
        if (capturingRecenter) { capturingRecenter = false; UpdateRecenterLabel(); EnableActions(); }
        base.OnDeactivate(e);
    }
    private bool EnsureCockpitTextures()
    {
        if (ChoiceId(cockpitTextures) != "Replace") return true;
        using var dialog = new CockpitTextureForm(client);
        LauncherAppearance.Apply(dialog);
        if (dialog.ShowDialog(this) == DialogResult.OK) return true;
        SelectChoice(cockpitTextures, "Off");
        notice.Text = "Cockpit texture setup did not finish. Original textures selected; choose New cockpit textures to retry.";
        if (dialog.Failure is not null) MessageBox.Show(this, dialog.Failure.Message, "Cockpit textures", MessageBoxButtons.OK, MessageBoxIcon.Error);
        return false;
    }

    private async Task Launch(bool inVr)
    {
        if (!EnsureCockpitTextures()) return;
        launchProgress?.Dispose();
        launchProgress = new LaunchProgressForm(inVr);
        launchProgress.Show(this);
        await Task.Yield();
        if (IsDisposed) return;
        try
        {
            seatAlignment.Save(client.Root);
            var choices = Current();
            IReadOnlyList<VrLayerBypass> layers = [];
            var layerWarning = "";
            if (inVr)
            {
                launchProgress.DisplayProgress(new("Checking the VR headset…"));
                if (await CheckVrLayers(choices.Runtime) is not { } checkedLayers)
                {
                    if (!IsDisposed) { launchProgress?.Close(); notice.Text = "VR launch cancelled. No OpenXR layer was disabled."; }
                    return;
                }
                (layers, layerWarning) = checkedLayers;
                if (IsDisposed) return;
            }
            launchWorker = client.Launch(choices, inVr, layers); launchingVr = inVr;
            launchGrace = DateTime.UtcNow.AddSeconds(10);
            launchProgress.DisplayProgress(new("Starting the launch worker…"));
            state.Text = "Starting session…";
            notice.Text = (inVr ? $"Select GRID 2 while it loads. VR starts once the game and tracking are ready. Press {RecenterShortcut.Label(recenterKey, recenterModifiers)} in the game to recenter."
                : "GRID 2 is starting. The progress window will close when the game window opens.") + (layerWarning.Length > 0 ? " " + layerWarning : "");
        }
        catch (Exception error) { launchProgress.DisplayProgress(new(error.Message, Failed: true)); throw; }
    }

    private Task<(IReadOnlyList<VrLayerBypass> Layers, string Warning)?> CheckVrLayers(string selectedRuntime) => VrLayerRecoveryFlow.Check(
        VrApiLayers.Remembered(client.Artifacts), layers => client.HeadsetPreflight(selectedRuntime, layers),
        request => IsDisposed ? null : VrLayerRecoveryForm.Confirm(this, request),
        layers => { VrApiLayers.Remember(client.Artifacts, layers); UpdateLayerBypassSummary(); },
        message => launchProgress?.DisplayProgress(new(message)));

    private void UpdateLayerBypassSummary()
    {
        var count = VrApiLayers.Remembered(client.Artifacts).Count;
        layerBypassSummary.Text = count switch { 0 => "No remembered OpenXR layer bypasses.", 1 => "1 remembered OpenXR layer bypass.", _ => $"{count} remembered OpenXR layer bypasses." };
    }

    private void UpdateLaunchProgress(JsonElement data)
    {
        if (launchWorker is null) return;
        var update = LaunchProgress.Read(data, launchWorker.Id, launchWorker.StartTime.ToUniversalTime().Ticks, launchingVr, launchWorker.HasExited);
        launchProgress?.DisplayProgress(update);
        if (!update.Finished && !update.Failed) return;
        if (update.Failed) notice.Text = update.Message;
        launchWorker.Dispose(); launchWorker = null; launchGrace = default;
    }
    private Task Send(string action, string message) => Act(async () => { await client.Control(action); if (!IsDisposed) notice.Text = message; });
    private async Task Act(Func<Task> action)
    {
        if (busy) return;
        busy = true; EnableActions();
        try { await action(); }
        catch (Exception error) { if (!IsDisposed) notice.Text = error.Message; }
        finally { busy = false; if (!IsDisposed) EnableActions(); }
    }
    private async Task RefreshStatus()
    {
        if (polling || busy || IsDisposed) return;
        polling = true;
        var polledWorker = launchWorker;
        try
        {
            var data = await client.Control("Status");
            if (IsDisposed) return;
            if (ReferenceEquals(polledWorker, launchWorker)) UpdateLaunchProgress(data);
            var session = Child(data, "session"); var last = Child(data, "last"); var status = Child(data, "status");
            pending = Flag(data, "pending"); active = Flag(session, "workerAlive") || (Flag(last, "workerAlive") && TextValue(last, "state") == "starting");
            if (active && TextValue(last, "mode") == "VR" && Child(last, "headset").ValueKind == JsonValueKind.Object && TextValue(last, "runtime") == runtime.SelectedItem as string) {
                headsetResolution = HeadsetResolution.Parse(Child(last, "headset").GetRawText(), TextValue(last, "runtime"));
                UpdateResolutionLabels();
            }
            measuredScene.Text = LauncherPresentation.MeasuredScene(Child(data, "resolution"), pending || active);
            UpdateMeasuredPerformance(Child(data, "performance"), Child(last, "graphics"), pending || active);
            headset = TextValue(session, "mode") == "Headset"; stopped = Flag(data, "stopped");
            rendering = !Flag(status, "failed") && status.TryGetProperty("state", out var phase) && phase.GetInt32() == 0;
            known = true;
            if (pending && !active) state.Text = "Recovery pending. Close GRID 2 and choose Restore original files.";
            else if (pending && Flag(status, "failed")) state.Text = "VR stopped on a rendering guard. Quit GRID 2 or choose End session.";
            else if (pending && stopped) state.Text = "VR stopped. Quit GRID 2 or choose End session to finish recovery.";
            else if (pending && rendering) state.Text = "VR active · " + TextValue(session, "runtime") + (!AppPaths.Packaged(client.Root) && Flag(last, "previewVrFixes") ? " · Preview VR fixes" : "");
            else if (pending) state.Text = headset ? "Waiting for game / tracking: " + TextValue(Child(data, "readiness"), "reason") : "Desktop session running.";
            else if (active || DateTime.UtcNow < launchGrace) state.Text = "Preparing session…";
            else if (TextValue(last, "state") == "failed") state.Text = "Launch stopped. You can try again or open Logs for details.";
            else state.Text = TextValue(last, "message", "Ready. Select a car and track, then Launch or Launch VR.");
            details.Text = JsonSerializer.Serialize(data, new JsonSerializerOptions { WriteIndented = true });
        }
        catch (Exception error)
        {
            if (!IsDisposed)
            {
                known = false; state.Text = "Session status unavailable: " + error.Message;
                measuredScene.Text = "Measurement unavailable: session status could not be read.";
                if (launchWorker is not null)
                {
                    if (launchWorker.HasExited) UpdateLaunchProgress(default);
                    else launchProgress?.DisplayProgress(new("Waiting for launch status. Loading continues…"));
                }
            }
        }
        finally { polling = false; if (!IsDisposed) EnableActions(); }
    }
    private void EnableActions()
    {
        var idle = known && !pending && !active && launchWorker is null && DateTime.UtcNow >= launchGrace && !busy;
        profiles.SetIdle(idle);
        updates.SetIdle(idle);
        var selected = loaded && car.SelectedItem != null && track.SelectedItem != null && runtime.SelectedItem != null;
        desktop.Enabled = vr.Enabled = selected && idle && !capturingRecenter;
        vr.Enabled &= !queryingHeadset;
        save.Enabled = selected && !busy && !capturingRecenter;
        launchMode.Enabled = runtime.Enabled = !busy && !active && !pending;
        var normal = ChoiceId(launchMode) == "Normal";
        UpdateSimpleContext(normal, ChoiceId(launchMode) == "Race");
        carSearch.Enabled = car.Enabled = track.Enabled = eventChoice.Enabled = !normal && !busy && !active && !pending;
        var pointToPoint = track.SelectedItem is ContentItem route && SessionClient.IsPointToPoint(route.Id);
        laps.Enabled = loaded && idle && !normal && !pointToPoint && !capturingRecenter;
        opponentCars.Enabled = opponents.Enabled = loaded && idle && ChoiceId(launchMode) == "Race" && !capturingRecenter;
        lapsHint.Text = normal ? "Normal uses the event options you choose in the game." : pointToPoint ? "Point-to-point route: one run. Your circuit lap setting is kept." : "Choose 1–20 laps for circuit and city routes.";
        recover.Enabled = known && pending && !active && !busy;
        recenter.Enabled = drivingControls.Enabled = loaded && idle;
        bindRecenterDevice.Enabled = clearRecenterDevice.Enabled = loaded && idle && !capturingRecenter;
        defaults.Enabled = loaded && idle && !capturingRecenter;
        foreach (var control in new Control[] { renderScale, desktopRenderScale, modernInterface, previewVrFixes, skipIntro, diagnosticLogging, performanceDisplay, replay3d, garage3d, vrCockpit, keepHorizonLevel,
            cockpitTextures, hudDistance, hudFollow, desktopVSync, desktopMsaa, desktopBorderless, vrMsaa, vrMirrorBorderless, treeDetail, objectDetail, crowdDetail, textureFiltering, graphicsDefaults })
            control.Enabled = loaded && idle && !capturingRecenter;
        EnableGraphicsEditors(loaded && idle && !capturingRecenter);
        performanceFollowView.Enabled = performanceDisplay.Enabled && performanceDisplay.Checked;
        cockpitDriver.Enabled = vrCockpit.Enabled && vrCockpit.Checked;
        cockpitExposure.Enabled = cockpitDriver.Enabled;
        seatAlignment.Enabled = cockpitDriver.Enabled;
        stop.Enabled = known && active && headset && !stopped && !busy;
        end.Enabled = known && active && pending && !busy;
        refreshHeadset.Enabled = loaded && !active && !pending && !busy && !capturingRecenter && !queryingHeadset;
    }
    private async void ShowAbout(string? topic = null)
    {
        using var dialog = new AboutForm(client.Root, recenterKey, recenterModifiers, topic, sessionIdle: !active && !pending && !busy, knownUpdate: availableUpdate);
        dialog.UpdateAvailable += ShowUpdateNotice;
        dialog.ShowDialog(this);
        if (!IsDisposed)
        {
            await updates.Start(checkAtStartup:false);
            updates.SetRelease(dialog.AvailableUpdate);
        }
    }
    private void ShowUpdateNotice(AppRelease? result)
    {
        if (IsDisposed) return;
        availableUpdate = result;
        updateNotice.Visible = result is not null;
        updateNotice.AccessibleDescription = result is null ? "" : $"Version {result.Tag} is available. Open Help / About to review and install.";
        tips.SetToolTip(updateNotice, updateNotice.AccessibleDescription);
    }
    private LinkLabel HelpLink(string topic)
    {
        var link = new LinkLabel { Text = "Instructions", AccessibleName = topic + " instructions", AutoSize = true, Margin = new Padding(0, 8, 0, 8) };
        link.LinkClicked += (_, _) => ShowAbout(topic);
        return link;
    }
    private void ConfigureTooltips(Control help, Control logs)
    {
        tips.SetToolTip(help, "Help / About and instructions. You can also press F1.");
        tips.SetToolTip(launchMode, "Normal opens the game menus. Practice starts a selected solo event. Race adds computer-controlled opponents.");
        tips.SetToolTip(opponents, "Race mode: 1–7 computer-controlled opponents. Practice stays solo.");
        tips.SetToolTip(opponentCars, "Same car matches your car. Mixed uses any installed selectable model. Same class uses your car's vehicle category, without tier restrictions. Race mode only.");
        tips.SetToolTip(replay3d, "Keep recorded replays in tracked 3D when supported. Off shows them on the existing VR screen. Driving stays in 3D; apply on the next VR launch.");
        tips.SetToolTip(garage3d, "Show the garage menus in tracked 3D. Off uses the stationary VR screen. Loading and ordinary flat menus stay on that screen. Applies on the next VR launch.");
        tips.SetToolTip(drivingControls, DrivingControls.RuntimeNotice);
        tips.SetToolTip(eventChoice, "Filter the installed tracks by location.");
        tips.SetToolTip(track, "The course for Practice or Race. Normal uses the in-game selection.");
        tips.SetToolTip(car, "Your car for Practice or Race. Normal uses the in-game selection.");
        tips.SetToolTip(carSearch, "Filter by car name, manufacturer or game code. Clear the search to show all installed cars.");
        tips.SetToolTip(laps, "Requested lap count for circuit and city routes. Point-to-point routes always use one run.");
        tips.SetToolTip(renderScale, "100% uses the connected headset’s recommended per-eye resolution. Each percentage scales width and height: 200% is four times the pixels. Output stays at the recommended size. Arrow keys adjust by 1%.");
        tips.SetToolTip(desktopRenderScale, "Desktop scene rendering from 50–300% of 1280 × 720. Applies on the next desktop Launch only; VR resolution is independent.");
        tips.SetToolTip(measuredScene, "Actual native scene dimensions from the current or last session, compared with that session's request. A requested size alone does not prove the game rendered it. No measurement means the scene has not been observed or the build did not report it.");
        tips.SetToolTip(modernInterface, "Preview Modern with red accents, or restore Classic. Save settings keeps the appearance; game and VR settings are unchanged.");
        tips.SetToolTip(previewVrFixes, "Use the latest VR fixes with your selected car, track and settings. Off retains the previous VR build. This option does not change your settings.");
        tips.SetToolTip(skipIntro, "Skip startup logos and the pre-race flythrough in Practice and Race. Normal keeps the original startup sequence for career saving; this preference is retained.");
        tips.SetToolTip(diagnosticLogging, "Save extra diagnostic details for troubleshooting the next launch. Leave off for normal play to reduce disk usage. Essential status and recovery receipts are always kept.");
        tips.SetToolTip(performanceDisplay, "Show app FPS and GPU usage in the headset on the next VR launch. FPS counts submitted app frames, not headset refresh or reprojection. GPU usage is the busiest engine on the game's adapter, including other applications; N/A means unavailable. Desktop Launch ignores this option.");
        tips.SetToolTip(hudDistance, "HUD and results text stereo depth from 1–20 metres in 0.5 m steps. Apparent text size stays the same. Default: 1 m. Applies to the next VR launch.");
        tips.SetToolTip(hudFollow, "Off keeps HUD and results text relative to the seated reference; On follows your head. Recenter resets the seated reference.");
        tips.SetToolTip(treeDetail, "Tree draw distance and detail. Ultra matches the working profile.");
        tips.SetToolTip(objectDetail, "Track-object draw distance and detail. Ultra matches the working profile.");
        tips.SetToolTip(crowdDetail, "Spectator detail. Ultra matches the working profile.");
        tips.SetToolTip(textureFiltering, "Texture sharpness at oblique angles. 16× matches the working profile.");
        tips.SetToolTip(desktopVSync, "Desktop Launch only. Launch VR always turns VSync off so the headset sets the frame rate.");
        tips.SetToolTip(desktopBorderless, "Fill the game window's monitor without borders on the next Desktop launch. Turn off to keep a bordered window at the requested resolution. Windows display settings and VR are unchanged.");
        tips.SetToolTip(desktopMsaa, "Desktop Launch only. Launch VR uses the separate Anti-aliasing (VR) choice.");
        tips.SetToolTip(vrMsaa, "Smooth jagged geometry edges in VR. Off preserves the working default. 2×, 4× and 8× increase GPU time and memory use. Applies to the next VR launch; desktop MSAA is independent.");
        tips.SetToolTip(graphicsDefaults, "Restore only graphics choices to the working profile, then Save or Launch to keep them.");
        tips.SetToolTip(vrMirrorBorderless, "Fill the VR mirror's current monitor without borders on the next VR launch. VR resolution is unchanged.");
        tips.SetToolTip(requestedSession, "Requested choices for the next launch. Check the actual car and track in GRID 2.");
        tips.SetToolTip(runtime, "VDXR connects directly through Virtual Desktop. SteamVR uses SteamVR's OpenXR runtime. Applies to the next session only.");
        tips.SetToolTip(recenter, "Choose the in-game recenter shortcut. Sit comfortably and face forward before pressing it in GRID 2.");
        tips.SetToolTip(desktop, "Save your choices and start the selected launch mode on desktop.");
        tips.SetToolTip(vr, "Save your choices and start the selected launch mode in VR using the selected runtime.");
        tips.SetToolTip(save, "Keep your choices and shortcut for the next launch. A running session is unchanged.");
        tips.SetToolTip(defaults, "Reset this launcher's choices, then save or launch to keep them. Installed game settings are unchanged.");
        tips.SetToolTip(stop, "Release the headset while leaving GRID 2 open. Quit and launch again to return to VR.");
        tips.SetToolTip(end, "Stop VR, close this session's game process and restore original files.");
        tips.SetToolTip(recover, "Restore original game files after an interrupted session. Close GRID 2 first; save progress is retained.");
        tips.SetToolTip(logs, "Open the active session's diagnostic folder, or the most recent saved session's logs when idle.");
    }
    internal static JsonElement Child(JsonElement value, string key) => value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var child) && child.ValueKind == JsonValueKind.Object ? child : JsonSerializer.SerializeToElement(new { });
    internal static bool Flag(JsonElement value, string key) => value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var item) && item.ValueKind == JsonValueKind.True;
    internal static string TextValue(JsonElement value, string key, string fallback = "loading") => value.ValueKind == JsonValueKind.Object && value.TryGetProperty(key, out var item) && item.ValueKind == JsonValueKind.String ? item.GetString()! : fallback;
    private static ComboBox Choice(string name) => new() { Name = name, AccessibleName = name, DropDownStyle = ComboBoxStyle.DropDownList, Dock = DockStyle.Fill, DropDownWidth = 600 };
    private static Button Button(string text, string name) => new() { Text = text, Name = name, AutoSize = true, Padding = new Padding(5, 2, 5, 2) };
    private static Label TextLabel(string text) => new() { Text = text, AutoSize = true, UseMnemonic = false, Margin = new Padding(0, 4, 0, 10) };
    private static VerticalStack Page(TabControl tabs, string title)
    {
        var page = new TabPage(title) { Padding = new Padding(16), AutoScroll = true, BackColor = SystemColors.Control };
        var content = new VerticalStack { Name = title + "Content" };
        page.Controls.Add(content); tabs.TabPages.Add(page);
        return content;
    }
    private static ResponsiveColumns Columns(Control parent)
    {
        var columns = new ResponsiveColumns(); parent.Controls.Add(columns); return columns;
    }
    private void Section(Control content, string title)
    {
        var header = new SectionHeader(title) { Font = new Font(Font, FontStyle.Bold), Margin = new Padding(0, 12, 0, 8) };
        header.CollapsedChanged += (_, _) =>
        {
            // Headers sharing a title (Simple and Advanced views) open and close together.
            foreach (var other in sectionHeaders) if (other.Title == title) other.Collapsed = header.Collapsed;
            window = window.WithCollapsed(sectionHeaders.Where(h => h.Collapsed).Select(h => h.Title).Distinct());
        };
        sectionHeaders.Add(header); content.Controls.Add(header);
    }
    internal IReadOnlyList<SectionHeader> SectionHeaders => sectionHeaders;
    internal string? CollapsedSections => window.CollapsedSections;
    internal void ApplyCollapsedSections(string? saved)
    {
        var collapsed = (window with { CollapsedSections = saved }).Collapsed();
        foreach (var header in sectionHeaders) header.Collapsed = collapsed.Contains(header.Title);
    }
    private static void Field(Control content, string title, Control control)
    {
        content.Controls.Add(new SettingRow(title, control));
    }
}
