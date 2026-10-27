using System.Text.Json;

namespace GRID2VR;

internal sealed record HelpTopic(string Title, string Text)
{
    public override string ToString() => Title;
}

// Text shared by the UI and its window-free checks.
internal static class LauncherPresentation
{
    public static ContentItem[] FilterCars(IEnumerable<ContentItem> cars, string query) => cars
        .Where(car => car.Label.Contains(query.Trim(), StringComparison.OrdinalIgnoreCase) || car.Id.Contains(query.Trim(), StringComparison.OrdinalIgnoreCase)).ToArray();

    public static string OpponentSelection(ContentItem? car, string selection) => selection switch
    {
        "mixed" => "Mixed cars",
        "class" => string.IsNullOrEmpty(car?.ClassId) ? "Same class (unknown class; same car)" : $"Same class: {car.ClassName}",
        _ => "Same car"
    };

    public static string OpponentHint(ContentItem? car, IEnumerable<ContentItem> cars, string mode, string selection)
    {
        if (mode != "Race") return "Your opponent car choice is kept for Race mode.";
        if (selection == "same") return "All opponents use your selected car.";
        if (selection == "mixed") return "Mixed: all installed vehicle classes. Models repeat when needed.";
        if (car == null) return "Select a car to see its class.";
        if (string.IsNullOrEmpty(car.ClassId)) return "Unknown vehicle class: opponents use your selected car.";
        return $"Class: {car.ClassName}. " + (cars.Any(c => c.Id != car.Id && c.ClassId == car.ClassId)
            ? "Vehicle category; no tier restriction." : "No other installed models in this class; opponents use your car.");
    }

    public static string RequestedSession(ContentItem? car, ContentItem? track, string? runtime, int key, int modifiers, int laps = 1, string mode = "Practice", int opponents = 7, string opponentCars = "same")
    {
        var vr = $"VR: {runtime} · Recenter: {RecenterShortcut.Label(key, modifiers)}";
        if (mode == "Normal") return $"Normal launch · choose your event in the game\n{vr}";
        if (car == null || track == null || runtime == null) return "Select a car, track and runtime.";
        var length = SessionClient.IsPointToPoint(track.Id) ? "point-to-point" : $"{laps} {(laps == 1 ? "lap" : "laps")}";
        var eventMode = mode == "Race" ? $"race · {opponents} AI {(opponents == 1 ? "opponent" : "opponents")} · {OpponentSelection(car, opponentCars)}" : "solo practice";
        var cockpit = car.CockpitAvailable ? "" : $"\n{(car.Dlc ? "DLC car" : "This car")} uses original cameras; custom VR cockpit and seat alignment are unavailable.";
        return $"{car.Label}\n{track.Label}\n{vr}\nDay / dry · {eventMode} · {length}{cockpit}";
    }

    public static string MeasuredScene(JsonElement receipt, bool current)
    {
        var prefix = current ? "Current session: " : "Last session: ";
        var requested = Size(receipt, "requested", out var requestedWidth, out var requestedHeight);
        var observed = receipt.ValueKind == JsonValueKind.Object && receipt.TryGetProperty("sceneObserved", out var flag) && flag.ValueKind == JsonValueKind.True;
        if (!observed || !Size(receipt, "scene", out var width, out var height))
            return requested ? $"{prefix}scene not yet observed (requested {requestedWidth} × {requestedHeight})."
                : current ? "Current session: scene not yet measured." : "No measured scene from the last session.";
        var actual = $"{prefix}{width} × {height}";
        if (!requested) return actual + " · requested size unavailable.";
        return actual + (width == requestedWidth && height == requestedHeight
            ? " · matches this session's request."
            : $" · mismatch: requested {requestedWidth} × {requestedHeight}.");
    }

    private static bool Size(JsonElement receipt, string key, out int width, out int height)
    {
        width = height = 0;
        return receipt.ValueKind == JsonValueKind.Object && receipt.TryGetProperty(key, out var size) && size.ValueKind == JsonValueKind.Object &&
            size.TryGetProperty("width", out var w) && w.ValueKind == JsonValueKind.Number && w.TryGetInt32(out width) && width > 0 &&
            size.TryGetProperty("height", out var h) && h.ValueKind == JsonValueKind.Number && h.TryGetInt32(out height) && height > 0;
    }

    public static HelpTopic[] Instructions(int key, int modifiers) =>
    [
        new("Getting started", "1. Choose a launch mode. Normal opens GRID 2's own menus so you can choose an event in the game. Practice starts a solo session using the selected car and track. Race adds 1–7 AI opponents. Opponent cars offers Same car, Mixed (any installed selectable model), and Same class (your car's vehicle category, without tier restrictions). Mixed and Same class prefer other models and repeat small pools; if none are available, opponents use your car. Unknown classes also use your car. The Laps slider offers 1–20 laps for circuits; point-to-point routes use one run and retain your circuit lap preference.\n\n" +
            "2. For VR, choose the OpenXR runtime on Settings. For VDXR, connect the headset through Virtual Desktop and keep SteamVR closed. For SteamVR, connect through SteamVR. This choice applies only to GRID2VR's session.\n\n" +
            "3. Choose Launch for desktop play or Launch VR for the headset. Both save your choices. Select GRID 2 while it loads; VR begins when the game and tracking are ready.\n\n" +
            "4. Car and track selection has been confirmed working. Not every installed combination has been driven; circuit lap counts still need an in-game completion check.\n\n" +
            "The session runs until you quit. Closing the launcher leaves the game running; reopen it to see session status."),
        new("Profile", "Profile selects the career used by every Normal, Practice and Race launch, on desktop or in VR. Close GRID 2 before creating, switching or deleting profiles. Selection is saved immediately.\n\n" +
            "Current game career uses the game's existing save. Supported standalone installations use this selection only; the game manages its own saves. Steam installations also offer Create new profile for a fresh career or a separate copy of the current career. Fresh careers need initial setup in Normal mode. Profile names contain 1–24 letters or numbers.\n\n" +
            "Managed careers are stored in the launcher user-data folder and belong to the Steam account that created them. They do not sync to Steam Cloud. Graphics and launcher controls are shared. A verified 100% GRID 2 career template is not bundled.\n\n" +
            "To delete a managed career, first select another profile, then Delete profile and confirm. Current game career cannot be deleted here."),
        new("Controls", $"Recenter: {RecenterShortcut.Label(key, modifiers)}\n\n" +
            "Controller / wheel recenter accepts a button or hat direction on an Xbox controller, DirectInput wheel or button box. The keyboard shortcut remains available. A hidden helper reads only the selected device while GRID 2 is active, requires release before another press, and survives closing the launcher. It does not consume the button's usual game action; choose an unused button. Save settings or launch to apply.\n\n" +
            "Sit comfortably and face forward, then press this shortcut while GRID 2 is active. It works even with the launcher closed. To change the binding, use Controls → VR → Recenter before launching, then press the new key or combination. Escape cancels capture.\n\n" +
            "Keep horizon level when recentering is on by default: position and forward direction reset without applying your head's pitch or roll to the world. Uncheck it to include pitch and roll. This applies from the next VR launch to all recenter bindings. Normal head tracking and the car's banking remain active.\n\n" +
            "Controls has General session actions, VR shortcuts and recenter alignment, Driving bindings, and per-car Seat alignment.\n\n" +
            DrivingControls.RuntimeNotice + "\n\n" +
            "Driving bindings provides a binding wizard, Xbox preset and axis calibration. Enable launcher driving bindings and save to apply your mappings to future launches. Disable them to use the game's bindings alone.\n\n" +
            "Stop VR releases the headset and leaves GRID 2 running. To start VR again, quit the game and launch a new session."),
        new("VR rendering", "Graphics → VR render scale uses the connected headset’s OpenXR recommended resolution as 100%. The runtime derives it from the headset and its optics; runtime quality settings can also affect it. It is not necessarily the panel’s physical pixel count. Refresh headset after connecting or changing runtime settings. Every VR launch reads it again.\n\n" +
            "The slider changes scene rendering only. Lower percentages render fewer pixels for more performance and less detail. Higher percentages can improve sharpness at greater GPU cost. Both width and height are scaled, so 200% renders four times the pixels. Headset output remains at the recommended size automatically. Next VR launch shows render and output dimensions per eye; Measured scene reports the actual game scene from the current or last session.\n\n" +
            "Older 1280 × 720-based scales are retained in saved settings for compatibility. The new headset-relative control starts at 100%; desktop resolution keeps its previous value. Missing headsets and unsupported sizes stop the VR launch with an explanation. These changes apply on the next launch and do not change world scale, FOV, tracking or refresh rate.\n\n" +
            "Tree detail, Object detail, Crowd detail and Texture filtering use GRID 2's native settings. Defaults keep the current Ultra detail and 16× filtering. Non-default graphics still need game/headset checks.\n\n" +
            "Anti-aliasing (VR) offers Off, 2×, 4× and 8× MSAA for the game scene. It is independent of desktop anti-aliasing and defaults to Off to preserve existing settings. Higher values smooth geometry edges but use more GPU time and memory.\n\n" +
            "Desktop Launch starts borderless and fills the monitor where the game window opens. For Launch VR, enable Maximize windowed screen under VR mirror to do the same on the next launch. This saved option starts off and works with SteamVR and VDXR in Normal, Practice and Race. Scene resolution stays at the requested size when the mirror resizes. Alt+Tab and minimization remain available; Windows display resolution and refresh rate are unchanged.\n\n" +
            "Desktop VSync and Anti-aliasing (MSAA) affect Launch only. Launch VR retains its existing VSync setting, motion blur off and current shadow, ambient-occlusion and post-processing profile.\n\n" +
            "Restore graphics defaults resets only graphics choices. Save settings or launch to keep them. Changes apply to the next launch; original game settings are restored after the session.\n\n" +
            "Full-headset stereo, head tracking, keyboard driving, recentering and removing/replacing the headset have been tested through Virtual Desktop's VDXR runtime. SteamVR support is retained for later testing.\n\n" +
            "VR opens on the first game presentation. Loading and recognized menu screens use one level screen fixed in the VR room; it does not follow or tilt with your head. Your recenter shortcut places it in front of you again, including during startup. Driving uses tracked geometry stereo. A recognized pause transition suspends stereo history without closing VR, and resuming starts fresh history. The new fixed screen placement awaits a headset check.\n\n" +
            "Tracking loss can suspend the headset view until tracking returns. If the launcher reports a rendering guard failure, quit GRID 2 or use End session; a new launch is required.\n\n" +
            "Advanced → 3D replays keeps recorded replay cameras in geometry stereo. Advanced → 3D menus does the same for the garage. Both start on to retain the working behavior. Turn either off to use the stationary VR screen for that scene; racing remains in 3D. Loading and ordinary flat menus always use the screen. Field-of-view controls are not yet available."),
        new("VR cockpit", "Cockpit view starts enabled for VR launches. For calibrated cars, it uses the seated driver position and replaces the bonnet camera; your normal camera-cycle control still selects other views. Installed DLC cars without calibration use their original cameras in every launch mode. Custom cockpit and seat alignment are unavailable for those cars. Desktop launches retain their original cameras.\n\n" +
            "Per-car alignment\nChoose the car under Controls → Seat → Align car. Height, forward/back and right/left are centimetres relative to its calibrated cockpit position. Negative height lowers the view. Zero preserves the current placement; Reset this car changes only that car. Save settings or launch to apply on the next session. Normal mode also applies saved adjustments to cars selected in-game.\n\n" +
            "Hide head keeps the driver's body and arms. Hide entire driver is available if a particular interior has obstructions. Cars chosen from the game's menus also receive their own camera position.\n\n" +
            "The S2000 camera has passed desktop and headset checks. Other installed cars have offline asset validation and still need visual checks for seating, interior detail and visibility. Camera changes do not alter VR resolution, MSAA or head tracking. Save settings or launch to keep this choice."),
        new("VR HUD", "Distance\nChoose 1–20 metres in 0.5 m steps. This changes stereo depth while keeping text at the same apparent size. Default: 1 m.\n\n" +
            "Follow view\nOff keeps HUD and results text relative to the seated reference. On keeps it in front of your head. Recenter resets the seated reference.\n\n" +
            "These options apply to HUD and results text during stereo VR. Individual HUD elements cannot be hidden yet. Loading and pause screens use their existing room-fixed presentation; the desktop HUD is unchanged.\n\n" +
            "Save settings or relaunch VR to apply. HUD layout and non-default distances need headset verification."),
        new("Settings and recovery", "Save settings keeps the launch mode, opponent count and car choice, selected car and track, circuit laps, graphics, runtime, recenter shortcut, appearance, startup, diagnostics and VR presentation choices for the next launch. Launch and Launch VR also save them. Driving controls has its own Save button. The car search is temporary.\n\n" +
            "Updates\nGRID2VR checks GitHub releases at startup when enabled and whenever Help / About opens. A New version available button appears beside ? when an update is found; click it to review and install. Changing Include experimental releases checks the selected channel immediately. Experimental releases are optional. Download and install verifies the installer size and SHA-256 before starting it. Finish the game and recovery before installation. Updates never silently install. Update preferences save immediately and do not change graphics or career settings. A private or unavailable public release feed cannot provide updates; use the Releases link for details.\n\n" +
            "Skip intro / logos skips startup logo and attract movies in all launch modes using the native movie player. Pre-race camera flythroughs, the countdown and the first playable career race remain. Startup definitions and career saving stay unchanged. This option starts off, matching DiRT2VR.\n\n" +
            "Diagnostic logging saves extra troubleshooting details for the next launch. Leave it off for normal play to reduce disk usage. Essential session status, errors and recovery receipts are still kept. Open logs shows the saved files. This option starts off, matching DiRT2VR.\n\n" +
            "Advanced → Performance display adds a small FPS and GPU usage panel in the headset. It starts off and applies to the next VR launch. Follow view keeps the panel attached to your gaze; turn it off to hold the panel level below your recentered forward view. Recenter to reposition the fixed panel. FPS counts submitted app frames, not headset refresh or compositor reprojection. GPU usage shows the busiest engine on the game's adapter, including other applications; N/A means Windows could not provide a reading. Desktop Launch ignores this option.\n\n" +
            "Installed releases include the trackside visibility, race-finish and shutdown corrections. They work with your selected car, track, runtime, circuit lap count and graphics settings. Point-to-point routes use one run and keep your circuit lap preference.\n\n" +
            "Modern interface previews GRID2VR's red-accented tabs, hints and buttons immediately. Off restores Classic. Existing saved settings start in Classic; fresh settings use Modern, matching DiRT2VR. Both follow Windows light/dark and high-contrast colours. Appearance changes do not change any game or VR option.\n\n" +
            "The launcher remembers its window size and maximized state automatically when closed. It fits the remembered size to the current display. This is separate from Save settings, so closing does not save unsaved game choices.\n\n" +
            "Restore defaults changes only these launcher choices. Save settings or launch to keep the reset. It does not edit your installed game's settings or driving bindings.\n\n" +
            "Use Quit in the game to finish, then wait for recovery. End session stops VR, closes this session's game process and restores original files.\n\n" +
            "If recovery is pending, close GRID 2 and choose Restore original files. Save progress is retained. Open logs opens the active session's diagnostic directory, or the latest saved session when idle. Advanced shows the latest session status.\n\n" +
            "Installed settings, managed careers, logs and recovery backups are kept under LocalAppData → GRID2VR, separately for each game installation. Upgrading or uninstalling preserves them. Help / About shows the build, release notes and update controls; Experimental builds also offer Return to stable.")
    ];
}
