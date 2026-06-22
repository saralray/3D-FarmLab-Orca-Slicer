<div align="center">

# 3D-FarmLab-Orca-Slicer

**A fork of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) integrated with the [3D-FarmLab](https://github.com/saralray/3D-FarmLab) print-farm platform.**

Slice as you always have, then sign in to your Print Farm and send the job to any
managed printer — Bambu Lab or Snapmaker — straight from the slicer, with no
device IPs, API keys, or manual file shuffling to manage.

</div>

---

## What this fork adds

This build keeps everything OrcaSlicer does and layers a Print Farm workflow on top:

- **In-app login** — a Print Farm sign-in screen is embedded in the OrcaSlicer
  window on every launch (session-based, no popup). The session lives in memory
  only and is destroyed on logout/exit.
- **Automatic printer sync** — after login, the farm's printers are loaded and
  appear directly in the **printer dropdown** in *Prepare*, listed as
  `Name (Farm)`. Selecting one switches to the matching machine profile and marks
  it as the upload target. Printers are read-only — you cannot add them by hand.
- **Zero-touch credentials** — on login the slicer mints a short-lived,
  session-bound upload token from the backend. You never create or paste an API
  key, and the token is revoked automatically on logout / app exit.
- **Upload to Farm** — a new action in the slice/print menu pushes the sliced
  result to the selected farm printer. The backend forwards it to the device
  (Bambu over MQTT, Snapmaker over Moonraker); OrcaSlicer never talks to a
  printer directly.
- **Jobs dashboard** — *Print Farm → Open Print Farm* shows synced printers and
  the job queue, with manual + automatic refresh and a cancel action.
- **Settings** — *Print Farm → Print Farm Settings* configures the server URL,
  auth mode, refresh interval and an optional manual API key (stored in the OS
  keychain, never in plaintext).

All Print Farm code is isolated under `src/slic3r/{Utils,GUI}/PrintFarm/` and
behind `// >>> PRINTFARM` markers on the few upstream files it touches, so the
fork stays easy to rebase onto upstream OrcaSlicer. See
[`docs/printfarm-integration-plan.md`](docs/printfarm-integration-plan.md) and
[`docs/printfarm-REBASE.md`](docs/printfarm-REBASE.md).

## How it connects

```
OrcaSlicer ──login (session)──▶ 3D-FarmLab API ──▶ printers / jobs
           ──mint token───────▶ ephemeral slicer_upload key (in memory only)
           ──Upload to Farm───▶ slicer-proxy ──▶ printer (Bambu MQTT / Snapmaker Moonraker)
```

- **Session token**: captured from the `pf_session` cookie, kept in memory only —
  never written to disk, config, or keychain. Login is required every launch.
- **Upload token**: a `slicer_upload`-scoped key minted per session and revoked on
  exit. A manual key can be set in Settings as an alternative (kept in the OS
  keychain).
- The proxy unwraps the plate G-code from the uploaded `.gcode.3mf` for
  Klipper/Moonraker printers, so Snapmaker U1 and Bambu printers both "just work".

## First run

1. Start the app. The Print Farm login screen covers the window.
2. Enter your **Server URL** (e.g. `http://127.0.0.1:8080`) — it stays editable on
   the login screen so you can change it later — plus your email and password.
3. After signing in, your farm printers appear in the printer dropdown. Pick one,
   slice, then **Upload to Farm**.

## Building

Cross-platform build, same as upstream OrcaSlicer.

```bash
# Linux: install system deps once, then build
./build_linux.sh -u          # system dependencies (needs sudo)
./build_linux.sh -d -j 6 -r  # third-party dependencies (Release)
./build_linux.sh -s -j 6 -r  # build the slicer
./build_linux.sh -i          # (optional) package a Linux AppImage installer

# Incremental rebuilds after the first build
cmake --build build --config Release --target OrcaSlicer -j 6
```

macOS / Windows build instructions are unchanged from upstream OrcaSlicer — see
the upstream [wiki](https://github.com/OrcaSlicer/OrcaSlicer/wiki/How-to-build).

### Installing

An AppImage is self-contained — you can just run it. For a desktop menu entry
and icon, use the installer:

```bash
./install.sh                 # install for the current user (~/.local)
./install.sh --system        # install for all users (uses sudo)
./install.sh /path/to.AppImage   # install a specific AppImage
./install.sh --uninstall     # remove it
```

With no argument it picks up the newest `build/*.AppImage`. After installing,
launch **3D-FarmLab-Orca-Slicer** from your application menu.

The displayed app name is `3D-FarmLab-Orca-Slicer`, but the user-data directory
key is intentionally left as `OrcaSlicer`, so settings, presets and profiles are
shared with a stock OrcaSlicer install and are not orphaned by the rebrand.

## Tests

```bash
cd build && ctest --output-on-failure
```

## Credits & License

This project is a fork of **OrcaSlicer** (which itself builds on Bambu Studio,
PrusaSlicer and Slic3r). All credit for the slicing engine and UI goes to the
[OrcaSlicer project](https://github.com/OrcaSlicer/OrcaSlicer) and its
contributors.

Licensed under the **GNU Affero General Public License, version 3 (AGPL-3.0)**,
the same as upstream OrcaSlicer. Per the AGPL, the complete corresponding source
of this fork — including the Print Farm integration — is published in this
repository.
