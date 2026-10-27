# GRID2VR

Play GRID 2 in native stereo VR with head tracking and cockpit views.

## Get started

You need **Windows 10/11 (64-bit)**, the **Steam version of GRID 2** (build 1.0.85.8679), and a headset connected through **Virtual Desktop / VDXR** or **SteamVR**.

1. Download the installer from the [latest release](https://github.com/preseznik/GRID2VR/releases/latest).
2. Close GRID 2 and run the installer. Select your existing GRID 2 game folder.
3. Open **GRID2VR** using its shortcut. Let the first-time preparation finish.
4. Connect your headset and select the matching VR runtime in the launcher. For direct VDXR, keep SteamVR closed.
5. Choose **Normal** to use the game's menus, or **Practice / Race** to pick a car and track in the launcher. Click **Launch VR**.

Supported installed DLC appears in the Practice / Race car and track lists. DLC cars currently use their original game cameras; custom VR cockpit views and seat alignment are available for calibrated cars only. The game's DLC ownership requirements still apply.

## While playing

- **F10** recenters your view. Change it or bind a controller/wheel button under **Controls → VR**.
- Calibrated cars start VR in cockpit view. Adjust their seating position under **Controls → Seat**.
- Adjust resolution, MSAA and the HUD under **Graphics**. The FPS/GPU display is under **Advanced**.
- Finish with **Quit** in the game so the launcher can restore its temporary changes. After an interrupted session, use **Restore original files** before launching again.

## Updates and help

For the optional cockpit artwork, open **Graphics → Cockpit textures → New cockpit textures**. The launcher downloads, verifies and installs its compatible pack automatically, with download progress and cancellation. Save settings or launch to use it. **Old cockpit textures** switches back to the original artwork. The pack works in desktop and VR; no separate download or commands are needed. Its per-car headset visual checks are still pending.

Use **Help / About** to check for updates and read the instructions. Updates preserve your settings and careers.

Having trouble? [Report an issue](https://github.com/preseznik/GRID2VR/issues) with your headset/runtime, what happened, and the latest session logs from **Open logs**.

GRID2VR requires your own copy of GRID 2. No game files or saves are included.

## Building and publishing

Build and validate the native renderer first. Run `tools/Package.ps1` with the new release's `-Version` and `-Channel Stable` or `-Channel Experimental`. Packaging requires the exact cockpit ZIP named by `tools/data/cockpit-texture-pack.json`, normally in `artifacts/cockpit-packs/all-cars`; use `-TexturePack` to supply another local path. Missing or changed artwork, or a native renderer built for another pack, stops packaging before the launcher build.

The package folder contains the installer, portable ZIP, matching cockpit ZIP, checksums and `release-assets.json`. The launcher embeds that pack's catalog and downloads from **its own build's GitHub release tag**. Every release carries its compatible pack, including when the artwork is unchanged. Older releases and download URLs stay intact.

Append `-Publish` to the package command to publish, or run `tools/Publish-Release.ps1 -PackageDirectory <package-folder>` afterward. The source commit must already be on GitHub. Publication uploads all four assets to a draft and verifies their sizes and SHA-256 hashes before making it public. Failed uploads leave the draft for a retry; published releases and conflicting assets are never overwritten. Experimental releases do not replace Stable Latest. Use `-Check` with `Publish-Release.ps1` for local validation without contacting GitHub.
