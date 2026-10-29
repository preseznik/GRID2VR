# Unreleased

- Fix missing rumble/force feedback in Practice and Race. These launches skip the game's Press Start screen, which is where GRID 2 claims your controller or wheel, so the game stayed on its keyboard preset and sent no rumble or force feedback. GRID2VR now makes the same claim once the race loads, using the game's own routine (no game files, saves or code bytes are changed). Driving controls has a new **Practice/Race device** choice: Auto (your steering-binding device, then an Xbox controller), Keyboard only (off), Xbox controller, or a bound wheel. Normal launch is unchanged. Each run writes `device-claim.json`.
- Wheels newer than GRID 2 (for example the Logitech G920) have no built-in preset, so the claim had nothing to bind. For Practice/Race sessions with such a wheel, GRID2VR now adds a temporary preset built only from your launcher bindings for it (without clutch/H-pattern, which GRID2VR already handles), listed in `input/paths.xml`. Both game files are hash-checked and restored when the game exits, or by session recovery. Wheels the game already knows keep their own preset.
- Add an observe-only force-feedback diagnostic (`ffb-observer.json` in each run's logs). It counts the game's own XInput rumble calls and DirectInput cooperative-level, acquire, force-feedback command and effect calls, with their results. Calls are passed through unchanged. This is for diagnosing missing rumble/force feedback in direct Race/Practice; it is not a fix.
- Build the nearby-opponent detail fix into every main renderer. `GRID2VR_OPPONENT_LOD_CONTROLLED_ACTIVATION` now defaults to ON; the diagnostic build must turn it off explicitly.
- The renderer embeds its LOD build mode, and release packaging refuses a renderer built without the fix. The local launcher's development pins now accept the rebuilt main renderer in place of the old frozen 0.2.0/0.2.1 build.
- Release packaging now builds the renderer, XR runtime probe and controller-input DLL from the source being released, instead of copying an existing renderer. The renderer embeds its source commit (also logged at startup), and packaging refuses a mismatch. Public releases require committed source. A hand-picked renderer can still be packaged for local tests, but publishing one needs `-AllowPinnedNative` and is recorded in the manifest.

# GRID2VR 0.2.10 — Experimental VR HUD camera local candidate

- Fix the race HUD appearing out of view and doubled in the headset at some render resolutions. GRID 2 draws the HUD through a perspective camera whose inverse-projection terms round slightly differently per render size. At the tester's 2150×2284 eye size one value differed in its last bit from the recorded camera, so every race-HUD draw was rejected and drawn flat in each eye: 54,243 draws per eye from four HUD shaders, none placed. Those two rounding-sensitive values are now checked within a small bound, and normalised only to roundings already seen in recorded cameras. Every other camera value must still match exactly, and unknown cameras are still rejected.
- Add the tester's live camera as a regression fixture, with bounds cases.

The tester confirmed the race HUD is now in view and no longer doubled in the headset.

# GRID2VR 0.2.9 — Experimental nearby-LOD thread gate local candidate

- Fix nearby opponents still dropping to low detail once alongside or behind in 0.2.8. The 0.2.8 user run refused 1,406 of 1,503 floors at the confirming check because VR permission flags changed between the two reads, with zero motion-only cases. The game selects car detail on its own thread, while GRID2VR clears permission at the start of every frame and marks eye-pair rendering on the render thread. Each such flip refused the floor and let the game's forward-only low detail through.
- The confirming check now refuses only on Stop or an owner/manager change. Transient permission flips are counted (`transientGates`) instead of refusing.
- Eye-pair suppression now applies only to the thread rendering the eyes. Previously it also blocked raises on the game's thread during replay, which applied low detail mid-pair, exactly what the suppression was meant to prevent.

The 30-unit nearby radius is unchanged and matches the DiRT2VR fix the user accepted. The tester confirmed nearby opponents now keep their detail alongside and behind in the headset, with a single brief flicker seen once. Broader performance and track coverage remain to be checked.

# GRID2VR 0.2.8 — Experimental nearby-LOD motion refresh local candidate

- Fix most nearby-opponent floors being skipped in 0.2.7. The floor read the cars twice and refused whenever anything differed, but other game threads keep moving cars and the camera between those reads. In the 0.2.7 user run 2379 of 3302 selection passes refused and only 361 of 3941 planned floors were written; each refused pass let native low LOD through, which matches the reported low detail with flicker.
- Motion-only differences now recompute the floor from the newer positions. Changes to owner, list, mesh, LOD bounds or the native selection value, and any VR/eye-replay gate change, still refuse with no write.
- Runtime receipts now count refusals per reason (`floorRefusals`), plus `motionRefreshes`, `retainedPasses` and why native routes ran (`originalByReason`), so any remaining flicker can be attributed.

Nearby body/wheel detail in both eyes, flicker, performance and normal exit still need a user test. Nothing is installed automatically.

# GRID2VR 0.2.7 — Experimental corrected activation local candidate

- Fix the concrete 0.2.6 track-loading crash: preserve and guard the updater's complete eleven-method table instead of copying only its first three methods. The loading lifecycle and clock callbacks remain unchanged; only the intended update method is replaced.
- Make both launcher session/error messages selectable and read-only. Select text or use Ctrl+A then Ctrl+C; native right-click copy is available. Messages wrap, long messages stay bounded, and unchanged status polling preserves selection.
- Add full-interface/slot-corruption/owned virtual-dispatch regressions and offscreen selectable-text tests. Keep the one-attempt controlled activation, full instruction/ownership guards and separate restoration budget.

This unpublished candidate corrects an identified loading bug; successful loading, actual LOD body/wheel behavior in both eyes, performance and normal exit still require a new user test. Do not retest the faulty 0.2.6 binary. Nothing is installed automatically.

# GRID2VR 0.2.6 — Experimental controlled activation local candidate

- Enable one guarded, real opponent-LOD attachment attempt per game process, using the scoped checks that passed the 0.2.5 user probe. Both expected native heap pointers must commit together before the nearby policy can run. Refusal or ownership loss disables the controlled session instead of retrying attachment.
- Keep all 33 instruction/body guards, complete peer-stack checks, foreign-pointer preservation and separate bounded restoration. Eligible nearby opponents use their best loaded native detail within 30 native units; player/cockpit, distant budgets, unsupported assets and replay/eye exclusions remain intact.
- Add specific snapshot-refusal reasons, separate attachment/restoration counts and bounded policy-result counters. Planned floors are distinct from confirmed selected-record writes.

**This is an activation test, not the earlier write-free diagnostic.** It can now change native LOD pointers and selected records after successful qualification. Actual private-caller behavior, adjacent/behind body and wheel appearance in both eyes, performance and normal exit still need user acceptance. Nearby higher-detail cars can cost more rendering time, including with Potato selected. This candidate remains unpublished and is not installed automatically.

# GRID2VR 0.2.5 — Experimental scoped diagnostic-only local candidate

- Separate the two shared critical-section helpers' code-integrity checks from LOD ownership-busy intervals. Both helpers remain fully fingerprinted; all LOD callers, lifecycle routes and private code remain protected by complete peer-stack checks.
- Bind the scope proof to the exact game image and all 33 body guards. Add real transaction fixtures showing that a shared-helper stack word alone can pass, while a parked LOD caller still refuses. Corrupt shared-helper instructions still refuse preparation.
- Keep one write-free probe per process, no native pointer writes/publication and no LOD floors. Logs now report 33 integrity bodies and 32 ownership-busy ranges.
- Keep the old stricter 34-range list in normal write-capable builds; the narrowed list is diagnostic-only pending live qualification.

**Diagnostic only: opponent detail is still expected to remain unchanged.** This tests whether the remaining target-specific guards pass after correcting the broad shared-helper exclusion. Start a VR Race, wait about ten seconds after the grid appears, then Quit normally and retain the transaction/runtime receipts. No full-race replay or visual acceptance test is needed yet. This is unpublished and does not claim a visual or performance fix.

# GRID2VR 0.2.4 — Experimental diagnostic-only local candidate

- Make one write-free opponent-LOD attachment probe per game process. Record the blocking thread, instruction/stack addresses, first matching value/range and transaction/pause-scope durations, then stop retrying and skip further LOD world checks.
- Keep all executable, lifetime, complete-stack and ownership guards. Even a successful probe does not write native pointers, call the ownership publisher or enable LOD floors. Logs retain only scalar witnesses, not full stacks or bulky render captures.

**Diagnostic only: opponent detail is expected to remain unchanged.** This unpublished candidate investigates 0.2.3's `BusyStack` refusal and possible attachment-scan hitches; it is not a visual LOD fix or a performance fix. Start the same VR Race, wait about ten seconds after the grid appears, then Quit normally and retain the latest run logs, especially `opponent-lod-transaction.json` and `opponent-lod-runtime.json`.

# GRID2VR 0.2.3 — Experimental local test candidate

- Correct the startup check that prevented 0.2.2's nearby-opponent LOD candidate from activating when Windows rebased the executable's PE header. Accept only the audited file's preferred base or the actual loaded base; all executable/instruction and VR safety guards remain required.
- Record the loaded/header bases and specific PE refusal reason in the bounded opponent-LOD run log. Add rebased-header integration, malformed-header rejection and Windows nonexecuting image-mapping regressions.

This is an unpublished corrective candidate. The reported 0.2.2 run made zero LOD changes because preparation was refused. This correction addresses that activation defect; adjacent/behind body and wheel detail, both-eye consistency, performance and normal exit still require a new user test. It does not claim a separate wheel-animation fix.

# GRID2VR 0.2.2 — Experimental local test candidate

- Add a guarded VR-only correction for nearby opponents dropping to low-detail models beside or behind the player. Nearby cars use their best loaded detail; distant-car settings and the player cockpit retain native handling. This new runtime path still needs gameplay/headset acceptance.
- Mirror the game's five graphics-quality presets and individual advanced quality controls in the launcher. **Potato / Low-End** selects Ultra Low quality, VR MSAA Off and 70% VR render scale; Undo restores the previous selection until another graphics edit or reset.
- Add **Simple / Advanced** views without resetting hidden custom choices. Entering Advanced asks for approval; the controls/seat/profile shortcuts are aligned and equally sized.
- Make wheel scrolling over sliders, numbers and closed dropdowns scroll the page rather than silently change settings. Clicking, dragging, typing and keyboard editing remain available; open dropdowns retain wheel selection.

This is an unpublished local candidate, not a new public release. Nearby higher-detail cars can increase rendering cost, including with Potato selected. Test adjacent/behind opponents, both-eye consistency, normal exit and restoration before treating the LOD correction as accepted. A failed qualification retains native behavior and is recorded in the session logs. Intel-only graphics extensions remain unavailable; native Automatic hardware detection is not represented as a fixed preset. The prior VR Practice Quit limitation remains under investigation.

# GRID2VR 0.2.1 — Experimental

- Apply launcher driving bindings in game, including combined keyboard, controller, wheel, pedal and separate shifter setups, on desktop and in VR.
- Make controls easier to set up with improved input capture, a **Live input** view, and saved driving profiles.
- Adjust pedal deadzones, saturation and wheel steering linearity in the launcher. New bindings default to **0% deadzone / 100% saturation**; existing calibration is preserved.
- Choose automatic, sequential, H-pattern or H-pattern with clutch from the launcher, without resetting the game's saved bindings.
- Choose how far to press the clutch before a gear engages. A held gear now engages when the pedal reaches that threshold, without returning the shifter to neutral. The pedal's analogue response is preserved.
- Improve desktop borderless fullscreen with a **Borderless fullscreen** setting under **Graphics → Desktop → Window**, enabled by default. The VR mirror setting remains separate.
- Improve HUD placement and correct a grey strip that could appear during loading and camera sweeps.
- Improve VR startup and menu/video resizing with launcher driving bindings enabled, and avoid false failure reports after successful desktop sessions.

Enable **Launcher driving bindings** in **Driving controls** to use the launcher assignments and transmission options.

This is an Experimental pre-release. Desktop driving inputs and the VR candidate have been tested on one setup; other wheel and shifter combinations still need testing. A reported fault when quitting some VR Practice sessions remains under investigation.

For experimental updates, enable **Include experimental releases** under **Help / About**.

# GRID2VR 0.2.0 — Stable

GRID2VR 0.2.0 brings the 0.1.11 Experimental feature set to the Stable channel. Get it through normal launcher updates or GitHub's **Latest** release, without enabling experimental updates.

Highlights since 0.1.3 Stable:

- Select supported installed DLC cars and tracks in **Practice / Race**, including Spa-Francorchamps and Bathurst. Supported DLC cars have cockpit views and per-car seat adjustment. You still need to own the corresponding DLC.
- Use optional **New cockpit textures** for all 76 supported cars, including the Caterham SP/300.R. The launcher downloads the matching artwork; **Old cockpit textures** switches back to the originals.
- Look around and zoom from chase cameras while driving or watching offline replays, on desktop and in VR. Enable **Free look** under **Controls → General**; it is off by default.
- Set **VR render scale** relative to your connected headset's recommended resolution, shown as 100%. The launcher displays the rendered and headset-output dimensions.
- Improve VR graphics changes, garage-to-Custom Event transitions and menu framing. Keep headlights aligned with **Advanced Lighting** off, and the steering wheel animated with **Vehicle Detail** set to **Low**.
- Choose your GRID 2 folder during installation and upgrades. Use a supported Steam or standalone copy of GRID 2, build 1.0.85.8679; standalone copies use **Current game career**.
- See automatic update notices beside **?**, and follow preparation progress when launching desktop or VR sessions.
- Improve shutdown cleanup and prevent expected startup checks from falsely marking successful sessions as failed.

Existing 720p-based VR scales move to the new 100% baseline; desktop resolution is preserved.

This release promotes the existing 0.1.11 functionality and adds no gameplay changes. **Known issue:** a reported fault when quitting some VR Practice sessions still needs further verification. Broader headset, DLC, standalone save/load and performance coverage remains limited.

# GRID2VR 0.1.11 — Experimental

- **VR render scale** now uses your connected headset's recommended resolution as 100%. The launcher shows the rendered and headset-output dimensions. Lower values favour performance; higher values add detail.
- Keep VR working when changing video mode, anti-aliasing, graphics presets and advanced quality settings.
- Improve VR transitions from the garage into Custom Events, and correct menu framing at higher resolutions.
- Keep headlights aligned with the car when **Advanced Lighting** is off.
- Keep the cockpit steering wheel animated with **Vehicle Detail** set to **Low**.
- Improve VR cleanup when closing the game, and stop expected game startup checks from falsely marking successful sessions as failed.

Existing 720p-based VR scales move to the new 100% baseline; desktop resolution is preserved.

This is an Experimental pre-release. A reported fault when quitting some VR Practice sessions still needs further verification. Broader headset, graphics and performance coverage remains limited.

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
