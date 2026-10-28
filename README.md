# GRID2VR

Play GRID 2 in native stereo VR with head tracking and cockpit views.

## Get started

You need **Windows 10/11 (64-bit)**, a supported **GRID 2 build 1.0.85.8679**, and a headset connected through **Virtual Desktop / VDXR** or **SteamVR**. Steam builds remain supported. The Experimental standalone path accepts only the exact audited executable pair; other DVD or modified builds are not automatically compatible.

Standalone installations launch directly and use **Current game career**. Managed careers and Steam career imports require a supported Steam installation. Standalone save/load and headset acceptance are still pending testing.

Choose a release channel before installing. The features below describe current Experimental functionality; Stable 0.1.3 does not include later additions such as DLC cockpit preparation (0.1.9), standalone game launch support (0.1.10), or headset-relative VR render scale (0.1.11).

| Channel | Published release | Downloads |
| --- | --- | --- |
| Stable | [Latest Stable](https://github.com/preseznik/GRID2VR/releases/latest) (currently 0.1.3) | [Installer](https://github.com/preseznik/GRID2VR/releases/download/v0.1.3/GRID2VR-0.1.3-Setup.exe) · [Portable ZIP](https://github.com/preseznik/GRID2VR/releases/download/v0.1.3/GRID2VR-0.1.3.zip) · [SHA-256 checksums](https://github.com/preseznik/GRID2VR/releases/download/v0.1.3/SHA256SUMS.txt) |
| Experimental | [0.1.11 Experimental](https://github.com/preseznik/GRID2VR/releases/tag/v0.1.11) | [Installer](https://github.com/preseznik/GRID2VR/releases/download/v0.1.11/GRID2VR-0.1.11-Setup.exe) · [Portable ZIP](https://github.com/preseznik/GRID2VR/releases/download/v0.1.11/GRID2VR-0.1.11.zip) · [SHA-256 checksums](https://github.com/preseznik/GRID2VR/releases/download/v0.1.11/SHA256SUMS.txt) |

GitHub's **Latest** link selects Stable, excluding Experimental pre-releases even when they are newer. See [all releases](https://github.com/preseznik/GRID2VR/releases) for future Experimental builds and read the selected release's notes. Experimental 0.1.11 has limited headset, graphics and performance coverage; a reported fault when quitting some VR Practice sessions still needs verification. Stable requires a supported Steam installation; the standalone game path requires Experimental 0.1.10 or later.

1. Download the installer for your chosen channel above.
2. Close GRID 2 and the GRID2VR launcher, then run the installer. Select your existing GRID 2 game folder.
3. Open **GRID2VR** using its shortcut. Let the first-time preparation finish.
4. Connect your headset and select the matching VR runtime in the launcher. For direct VDXR, keep SteamVR closed.
5. Choose **Normal** to use the game's menus, or **Practice / Race** to pick a car and track in the launcher. Click **Launch VR**.

Supported installed DLC appears in the Practice / Race car and track lists in Experimental 0.1.4 and later. Experimental 0.1.9 added cockpit views and per-car seat adjustment for the **25 supported DLC cars**. Enable **VR cockpit** to start VR Practice / Race in cockpit view, and adjust each car under **Controls → Seat**. Cockpit preparation requires supported, unchanged DLC sources; if preparation fails, original cameras are retained. The game's DLC ownership requirements still apply. These implemented views do not imply completed headset validation: DLC startup, cockpit placement and per-car headset checks remain pending.

## While playing

- **F10** recenters your view. Change it or bind a controller/wheel button under **Controls → VR**.
- With **VR cockpit** enabled, supported calibrated cars (including the 25 supported DLC cars in Experimental 0.1.9 and later) start VR Practice / Race in cockpit view. Adjust their seating position under **Controls → Seat**.
- Adjust resolution, MSAA and the HUD under **Graphics**. The FPS/GPU display is under **Advanced**.
- In **Experimental 0.1.11 and later**, **VR render scale** uses the connected headset's OpenXR recommended per-eye resolution as 100%. Lower values trade detail for performance; higher values render more pixels. Headset output stays at the recommended size automatically. The launcher shows both dimensions and refreshes the recommendation at each VR launch. Old 720p-based VR scales start at 100% under this new control; desktop resolution is preserved.
- Finish with **Quit** in the game so the launcher can restore its temporary changes. After an interrupted session, use **Restore original files** before launching again.

## Updates and help

In **Experimental 0.1.7 and later**, for the optional cockpit artwork, open **Graphics → Cockpit textures → New cockpit textures**. The launcher downloads, verifies and installs its compatible pack automatically, with download progress and cancellation. Save settings or launch to use it. **Old cockpit textures** switches back to the original artwork. The pack works in desktop and VR; no separate download or commands are needed. Its per-car headset visual checks are still pending.

Experimental 0.1.11 also publishes its compatible [cockpit texture pack](https://github.com/preseznik/GRID2VR/releases/download/v0.1.11/GRID2VR-CockpitTextures-0.2.0-preview.2.zip) alongside the installer, portable ZIP and checksums. Prefer the launcher's automatic pack installation; use the pack matched to your release. Cockpit artwork is optional and separate from DLC cockpit camera and seat support.

Use **Help / About** to check for updates and read the instructions. Enable **Include experimental releases** to receive Experimental updates; Stable remains available through **Return to stable…** in Experimental builds or the Stable download above. Updates preserve your settings and careers.

Having trouble? [Report an issue](https://github.com/preseznik/GRID2VR/issues) with your headset/runtime, what happened, and the latest session logs from **Open logs**.

GRID2VR requires your own copy of GRID 2. No game files or saves are included.

## Building and publishing

Build and validate the native renderer first. Run `tools/Package.ps1` with the new release's `-Version` and `-Channel Stable` or `-Channel Experimental`. Packaging requires the exact cockpit ZIP named by `tools/data/cockpit-texture-pack.json`, normally in `artifacts/cockpit-packs/all-cars`; use `-TexturePack` to supply another local path. Missing or changed artwork, or a native renderer built for another pack, stops packaging before the launcher build.

The package folder contains the installer, portable ZIP, matching cockpit ZIP, checksums and `release-assets.json`. The launcher embeds that pack's catalog and downloads from **its own build's GitHub release tag**. Every release carries its compatible pack, including when the artwork is unchanged. Older releases and download URLs stay intact.

Append `-Publish` to the package command to publish, or run `tools/Publish-Release.ps1 -PackageDirectory <package-folder>` afterward. The source commit must already be on GitHub. Publication uploads all four assets to a draft and verifies their sizes and SHA-256 hashes before making it public. Failed uploads leave the draft for a retry; published releases and conflicting assets are never overwritten. Experimental releases do not replace Stable Latest. Use `-Check` with `Publish-Release.ps1` for local validation without contacting GitHub.
