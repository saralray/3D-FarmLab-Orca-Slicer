# Print Farm integration — rebase & maintenance guide

This fork adds an optional **Print Farm** integration (login gate, printer sync,
jobs dashboard, and "Upload to Farm"). It is built to survive upstream
OrcaSlicer rebases with minimal conflict surface. This document explains how the
changes are organized and what to check after a rebase.

See `docs/printfarm-integration-plan.md` for the full architecture/design.

## Design principle: isolate, don't entangle

Almost all logic lives in **new files** under dedicated directories, so upstream
edits rarely touch them:

```
src/slic3r/Utils/PrintFarm/      # transport-agnostic API client (no wx, no GUI)
  PrintFarmClient.hpp            #   IPrintFarmClient interface + DTOs + factory
  RestPrintFarmClient.{hpp,cpp}  #   concrete impl over Slic3r::Http
src/slic3r/GUI/PrintFarm/        # all wx UI + the app-facing manager
  PrintFarmManager.{hpp,cpp}     #   singleton facade: config + in-memory session
  PrintFarmLoginDialog.{hpp,cpp} #   startup login gate
  PrintFarmSettingsDialog.{hpp,cpp}
  PrintFarmJobsDialog.{hpp,cpp}  #   printers + jobs dashboard, upload, cancel
docs/printfarm-integration-plan.md
docs/printfarm-REBASE.md         # this file
```

The UI never performs HTTP directly — it always goes through
`IPrintFarmClient`. OrcaSlicer never talks to a printer or printer IP directly;
all device communication is the backend's responsibility (the OctoPrint-emulating
slicer-proxy upload path).

## Edits to upstream files are fenced with markers

Every change inside a pre-existing upstream file is wrapped in:

```cpp
// >>> PRINTFARM
... added lines ...
// <<< PRINTFARM
```

(CMake uses `# >>> PRINTFARM` / `# <<< PRINTFARM`.)

`git grep -n "PRINTFARM"` lists every touch point. The complete set of modified
upstream files:

| File | What was added |
|------|----------------|
| `src/slic3r/CMakeLists.txt` | New PrintFarm sources in `SLIC3R_GUI_SOURCES` |
| `src/slic3r/GUI/GUI_App.cpp` | Login gate before `MainFrame`; `clear_session()` on exit |
| `src/slic3r/GUI/MainFrame.cpp` | "Print Farm" menu; `eUploadToFarm` in the print-button dropdown + dispatch |
| `src/slic3r/GUI/MainFrame.hpp` | `eUploadToFarm` enum value in `PrintSelectType` |
| `src/slic3r/GUI/Plater.cpp` | `Plater::export_to_farm()` (silent sliced-bundle export → upload) |
| `src/slic3r/GUI/Plater.hpp` | `export_to_farm()` declaration |

Existing "Send to Printer" / "Print" actions are **left intact** — "Upload to
Farm" is added alongside them and only appears while logged in, so disabling the
integration cannot regress stock behavior.

## After an upstream rebase — checklist

1. **Resolve the fenced hunks.** Conflicts will almost always sit inside
   `>>> PRINTFARM` / `<<< PRINTFARM` blocks. Keep the fenced additions; re-apply
   them against the new surrounding upstream code.
2. **`PrintSelectType` enum** (`MainFrame.hpp`): if upstream added enum values,
   keep `eUploadToFarm` as the highest value to avoid renumbering.
3. **Print-button dropdown** (`MainFrame.cpp`): the "Upload to Farm" button is
   appended just before `p->Popup(m_print_btn);`. If upstream restructured the
   popup, re-anchor it there.
4. **`export_3mf` strategy flags** (`Plater.cpp`): `export_to_farm()` mirrors
   `export_gcode_3mf()` and uses
   `SaveStrategy::Silence | SplitModel | WithGcode | SkipModel`. If upstream
   changes the sliced-export strategy, mirror the change here.
5. **CMake source list**: confirm all eight PrintFarm files are still listed in
   `SLIC3R_GUI_SOURCES`.
6. **Build** `RelWithDebInfo`/`Release` and smoke-test: launch → login gate,
   Print Farm menu present, settings test-connection, dropdown shows
   "Upload to Farm" when logged in.

## Security invariants (do not break during rebase)

- Session token is captured from the `pf_session` cookie and held **in memory
  only** — never written to `AppConfig`, disk, or keychain. Login is required on
  every launch; logout / app-exit clears it.
- The Print API Key is separate from login. It is never written to the plaintext
  config file: when the user opts in it is stored in the OS keychain via
  `wxSecretStore` (service `OrcaSlicer/PrintFarm`); otherwise it stays in memory
  for the session only. `PrintFarmManager::load_config` migrates any legacy
  plaintext key from older builds into the keychain and strips it from the config.
- Passwords and tokens are never logged. Structured logs use the `[printfarm]`
  prefix at appropriate levels.
