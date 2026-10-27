# GRID2VR 0.1.10 — Experimental

- Look around from the close and far chase cameras with the mouse or right stick, on desktop and in VR, while driving or watching an offline replay. Enable **Free look** under **Controls → General**; it is off by default.
- Scroll to zoom. Choose hold-left, hold-right or always-on mouse look, invert vertical movement, keep your camera angle and adjust mouse and stick sensitivity separately.
- Move the replay camera while paused, then return to VR driving. Camera rotation stays centered on the car when nearby scenery pushes the camera closer.
- Keep the selected VR scene resolution when resizing the desktop mirror. Enable **Graphics → VR mirror → Maximize windowed screen** to fill the monitor with a borderless mirror.
- Correct the initial cockpit view for supported DLC cars when the race loads.
- Extend the optional **New cockpit textures** pack to all 76 cars, including the Caterham SP/300.R. The matching pack downloads through the launcher (about 125 MB); **Old cockpit textures** switches back to the originals.
- Choose your GRID 2 folder during both fresh installation and upgrades, using **Browse GRID 2 Installation**.
- Support the standalone GRID 2 1.0.85.8679 build with direct launch and optional intro skipping. Use **Current game career** for this build; managed careers and Steam career imports remain available for supported Steam copies.

This is an Experimental pre-release. Broader car/controller coverage, standalone save/load and clean VR shutdown still need testing. No game files are included.

**Known issue:** closing a VR session with **Alt+F4** can report a shutdown fault after successful driving and replay. This remains unresolved.

# GRID2VR 0.1.9 — Experimental

- Add cockpit views and per-car seat adjustment for the 25 supported DLC cars. You still need to own the corresponding DLC.
- Start VR Practice and Race in cockpit view when **VR cockpit** is enabled.
- Fix SteamVR launch preparation failing in installed copies of GRID2VR.
- Restore temporary changes and unlock the launcher after a failed launch or normal game exit, including when GRID 2 refreshes its detected memory or display modes.
- Recover untouched interrupted launches automatically when reopening the launcher or upgrading. Changed files still require recovery checks.
- Support game hardware settings stored in OneDrive Documents folders.
- Make **Open Logs** find early startup failures and show the underlying error. Each launch keeps its logs, diagnostics and recovery backups in a separate dated folder.

For sharper cockpit artwork, select **Graphics → Cockpit textures → New cockpit textures**. The optional 123 MB pack downloads and installs through the launcher; **Old cockpit textures** switches back to the originals. This option was introduced in 0.1.7 and remains available in 0.1.9.

This is an Experimental pre-release. DLC startup, cockpit placement and headset checks with this build are still pending.

# GRID2VR 0.1.7 — Experimental

- Select **Graphics → Cockpit textures → New cockpit textures** to download, verify and install the optional artwork inside the launcher.
- Show download progress and allow cancellation. Reuse an already installed pack after verification, and check it before launching with new textures.
- Keep the core installer compact and self-contained. The optional 123 MB pack downloads once, when selected; no separate download or commands are needed.

Save settings or launch after selecting the textures. **Old cockpit textures** switches back to the original artwork. This release retains the Steam compatibility fixes from 0.1.6. Per-car headset visual acceptance for the texture preview remains pending.

# GRID2VR 0.1.6 — Experimental

- Fix installation and launch checks rejecting valid Steam copies of the supported GRID 2 version because their executable hashes differ.
- Reduce installer size by about one third while keeping all required runtimes bundled.
- Add Old / New cockpit texture choices for desktop and VR. The optional texture pack covers 75 cars; the Caterham SP/300.R retains its original texture.

New cockpit textures require the separate optional pack. Headset visual acceptance for this preview remains pending.

# GRID2VR 0.1.5 — Experimental

- Automatically check for updates at startup and show New version available beside ? when an update is found.
- Check for updates automatically when Help / About opens or the experimental-release preference changes.
- Mark DLC cars and tracks with (DLC) in the selection lists and session summary.

Startup checks respect your saved setting. Experimental updates remain optional; installation starts only when you choose Download and install.

# GRID2VR 0.1.4 — Experimental

- Select supported installed DLC cars and tracks in Practice / Race, including Spa-Francorchamps and Bathurst.
- Include DLC cars in Same car, Mixed and Same class opponent selections.
- Show the current preparation step and elapsed time while launching desktop and VR sessions.

DLC cars use their original game cameras; custom VR cockpit views and seat adjustments are not yet available for them. DLC ownership is required. Live DLC gameplay and headset checks are still pending.

# GRID2VR 0.1.3 — Stable

- Choose Same car, Mixed or Same class for Race opponents.
- Same class groups cars by vehicle type; Mixed draws from all installed selectable models.
- Remember your opponent car choice between launches and show the selected class in Race options.
- Start desktop Normal, Practice and Race sessions in borderless windowed mode.

# GRID2VR 0.1.2 — Stable

- Fix VR failing to enter gameplay when the instance-buffer diagnostic log fills.
- Track live instance buffers independently of the bounded log, while retaining resource validation.
- Choose whether the FPS/GPU display follows your view or stays fixed below your recentered forward view.

Verified with the Ford Focus on Okutama through VDXR, including driving, pause/resume and normal Quit. Graphics defaults and saved settings are unchanged.

# GRID2VR 0.1.1 — Stable

- Show preparation progress and the current task, including individual cockpit cameras.
- Include the missing shutdown report helper so quitting releases the launcher normally.
- Preserve recovery receipts if optional report collection fails, and recover interrupted sessions without claiming a clean game exit.

VR rendering and graphics defaults are unchanged.

# GRID2VR 0.1.0 — Stable

First standalone release of GRID2VR.

- Native stereo VR with head tracking through Virtual Desktop VDXR or SteamVR.
- Normal, Practice and Race launches, with car and track selection.
- Custom cockpit cameras, head hiding and individual seat adjustments.
- Render resolution, headset texture scale, MSAA and VR HUD controls.
- Custom driving controls and keyboard, controller or wheel recenter bindings.
- Separate career profiles, startup skipping and a performance display.
- A red modern interface, responsive layout, Help / About and verified GitHub updates.

Requires the supported Steam GRID 2 executable (1.0.85.8679). No game files or careers are included. Gameplay and device coverage varies; report issues with the session logs.
