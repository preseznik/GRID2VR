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
