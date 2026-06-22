# OrcaSlicer ↔ 3D-FarmLab Print Farm Integration Plan

> Status: **design / phase-0**. This document is the required first deliverable. It captures
> what was *discovered* by inspecting both source trees (no invented APIs) and the proposed
> integration architecture. Implementation follows in small, reviewable commits.

---

## 1. Scope & Goals

Integrate OrcaSlicer with the **existing** 3D-FarmLab print-farm backend (`~/3D-FarmLab`). The
backend is **not** modified or redesigned. OrcaSlicer becomes a farm client that can:

1. Log a user in against the farm (session-based, in-memory only).
2. Pull the farm's printer list (read-only; user cannot add printers locally).
3. Slice a model and **upload it to a chosen farm printer**, which the backend prints.
4. Monitor the resulting jobs.

Hard constraints (from the brief, and consistent with the backend design):

- OrcaSlicer **never** talks to a printer IP / Snapmaker / Bambu device directly. All printer
  communication is backend-driven.
- Session token lives **in memory only** — never written to disk, config, or keychain. Login is
  required on every launch; logout / app-exit destroys all auth state.
- The **Print API Key** is separate from login and is used only for print actions.
- Keep modifications **isolated** and rebase-friendly; do not touch slicing algorithms.

---

## 2. OrcaSlicer Architecture Findings

OrcaSlicer is an open-source C++17 slicer (PrusaSlicer fork) with a wxWidgets GUI and CMake build.

Relevant entry points and infrastructure discovered:

| Area | File(s) | Notes |
|------|---------|-------|
| HTTP client | `src/slic3r/Utils/Http.{hpp,cpp}` | libcurl wrapper. Supports `get/post/put/del`, custom headers, multipart `form_add`/`form_add_file`, `on_complete`/`on_error`/`on_progress`, `perform()`/`perform_sync()`, `tls_verify()`, and `on_header_callback()` (lets us read `Set-Cookie`). **This is the only HTTP path the abstraction layer needs.** |
| Print-host abstraction | `src/slic3r/Utils/PrintHost.{hpp,cpp}`, `OctoPrint.cpp` | Existing `PrintHost` interface + an **OctoPrint** implementation. The farm's `slicer-proxy` *emulates OctoPrint*, so this is a natural reuse point (see §4). |
| Network agents | `src/slic3r/Utils/NetworkAgent*`, `*PrinterAgent.cpp` | Vendor agents (Bambu/Creality/Moonraker/Snapmaker). We deliberately **do not** use these for device comms — the farm backend owns that. |
| App startup | `src/slic3r/GUI/GUI_App.cpp` (`GUI_App::OnInit`), `src/OrcaSlicer.cpp` | Where the login gate is injected before the main frame is shown. |
| Existing login dialog | `src/slic3r/GUI/WebUserLoginDialog.{cpp,hpp}` | Bambu-account web login. Pattern reference only; our login is a native form. |
| App config | `AppConfig` (`src/libslic3r/AppConfig.*`) | Where **non-secret** settings (farm URL, auth mode, refresh interval) persist. **No tokens / API key persisted by default.** |
| Tabs / pages | `src/slic3r/GUI/MainFrame.*`, `Tab.*`, `Plater.*` | Where the Jobs page and Settings section attach. |

---

## 3. 3D-FarmLab (Print Farm) Architecture Findings

A Vite/React dashboard + Node.js services + Postgres + Redis, orchestrated via `docker-compose`.
Authoritative docs read in full: `~/3D-FarmLab/API.md`, plus source in `server/`, `slicer-proxy/`,
`poller/`.

Services relevant to us:

| Service | File | Role |
|---------|------|------|
| `web` | `server/app.js` (178 KB) | Serves the dashboard, the **session** frontend API (`/api/*`), and the **API-key** API (`/api/v1/*`). |
| `slicer-proxy` | `slicer-proxy/index.js` | **OctoPrint-emulating** upload endpoint. Per-printer base URL `http://<host>:<SLICER_PROXY_PORT>/printers/<id>`. Dispatches uploads by printer profile to the real hardware. Listens on `SLICER_PROXY_PORT` (default `8091`); published behind nginx alongside the dashboard (`HTTP_PORT`, default `8080`). |
| `poller` | `poller/printer_status_poller.py` | Writes live `status` (`idle`/`printing`/`offline`) and `error_message` onto printer rows. |

### 3.1 Two authentication systems (both discovered, neither invented)

**A. Frontend session — `/api/auth/*` (cookie-based).** Backs the dashboard.

- `POST /api/auth/login` body `{ username, passwordHash, remember }` where `passwordHash` is the
  **sha256 hex of the password, hashed client-side** (the plaintext is never sent — confirmed at
  `server/app.js:540`). On success it sets an **HttpOnly `pf_session` cookie** and returns `{ user }`.
  Only the cookie's sha256 hash is persisted server-side (`sessions` table). Rate-limited
  (8 failures / 15 min → `429`).
- `POST /api/auth/logout` — destroys the session, clears the cookie. Idempotent.
- `GET /api/auth/session` — `{ user }` or `{ user: null }`.

  > For a **desktop** client the `pf_session` cookie is just an opaque string we read from the
  > `Set-Cookie` response header (HttpOnly only restricts browser JS, not native clients). We hold
  > it **in memory** and send it back as `Cookie: pf_session=<value>` on `/api/*` calls. This *is*
  > the "session token" the brief asks for — captured, kept in RAM, destroyed on logout/exit.

**B. API-key — `/api/v1/*` and `slicer-proxy` (the "Print API Key").**

- Header `X-Api-Key: <key>` **or** `Authorization: Bearer <key>`.
- Keys carry a **permissions scope** array:
  - `slicer_upload` — required by `slicer-proxy` to push a print (`slicer-proxy/index.js:395`).
  - `printfarm_manage` — full read/write admin over `/api/v1/*`.
  - Legacy keys backfill to both.
- Minted in the dashboard **Settings → Slicer Upload / API Keys**; only the sha256 hash is stored,
  plaintext shown once. This is exactly the brief's **Print API Key**: not used for login, only for
  print actions.

### 3.2 Printer list (read-only source of truth)

- Session path: `GET /api/printers` (`server/app.js:2988`). Public read; connection secrets
  (`ipAddress`, `apiKeyHeader`, `serial`, `url`) are **redacted** unless the session role is
  operator/admin. Live telemetry (`status`, `errorMessage`) is overlaid.
- API-key path: `GET /api/v1/printers` (needs `printfarm_manage`) — full detail.
- `GET /api/printers/:id` / `GET /api/v1/printers/:id` — single printer.

  Discovered printer fields: `id`, `name`, `model`, `profile` (e.g. `snapmaker_u1`,
  `bambulab_a1_mini`, `bambulab_h2s/h2d/h2c`), `status` (`idle`/`printing`/`offline`),
  `errorMessage` (nullable), and redactable connection fields. **Printers only ever come from the
  backend — the brief's "user cannot add printers" is naturally satisfied.**

### 3.3 Print-job submission — the canonical path is the OctoPrint emulation

The farm already defines exactly how a slicer pushes a print. From `slicer-proxy/index.js`:

- Base URL per printer: `http://<host>:<port>/printers/<id>`.
- `GET /printers/<id>/api/version` — auth probe (the slicer "Test" button); `403` on bad key.
- `POST /printers/<id>/api/files/local` — **multipart upload**, first part **named `file`**,
  authenticated by `X-Api-Key` carrying `slicer_upload`. The proxy then:
  - `snapmaker_u1` → Moonraker `POST /server/files/upload` with `print=true` (uploads **and
    auto-starts**).
  - `bambulab_*` → FTPS upload of the `.3mf` + MQTT `project_file` to start it.
  - other profiles → `415` (no upload API).
  Success returns OctoPrint's `201 { done: true, files: { local: { name } } }`.

So **"Slice → Upload To Farm"** = a multipart POST of the sliced `.gcode.3mf` to
`/printers/<id>/api/files/local` with the Print API Key. The backend creates/starts the print. This
is the existing, supported, brand-agnostic contract — no new endpoint required.

### 3.4 Job monitoring

- `GET /api/queue` (session) / `GET /api/v1/queue` (`printfarm_manage`) — stored queue jobs
  (`submitterName`, `filename`, `submittedAt`, `printedStatus`, `priority`, `estimatedTime`, …).
- Live per-printer job state is reflected on the printer record (`status`, current job via the
  poller) and the OctoPrint `GET /printers/<id>/api/job` shape.

  > Note: there is no per-upload "job id" returned by the slicer-proxy upload (it mirrors OctoPrint's
  > fire-and-forget response). The Jobs page therefore composes its view from **(a)** locally-tracked
  > uploads (Pending/Uploading/Failed, owned by OrcaSlicer) and **(b)** backend state — printer
  > `status` + the queue list (Queued/Printing/Completed). See §6 risks.

### 3.5 Other relevant endpoints

- `GET /healthz` (liveness, DB-independent) — used for "backend reachable?" checks.
- `POST /api/v1/slicer-keys` — mint a `slicer_upload` key (admin). Informational only; users paste
  an existing key.

---

## 4. Proposed Integration Architecture

A single isolated module under `src/slic3r/GUI/PrintFarm/` plus a thin abstraction in
`src/slic3r/Utils/`. All HTTP goes through the existing `Http` class. **No UI component performs
HTTP directly** — everything routes through `IPrintFarmClient`.

```
        ┌─────────────────────── OrcaSlicer GUI ───────────────────────┐
        │                                                               │
        │  PrintFarmLoginDialog   PrintFarmJobsPanel   PrintFarmSettings│
        │          │                     │                    │         │
        │          └──────────┬──────────┴──────────┬─────────┘         │
        │                     ▼                      ▼                   │
        │              PrintFarmSession  ◄──►  PrintFarmManager          │
        │              (in-memory token)      (singleton facade,         │
        │                                      printer cache, refresh)   │
        │                          │                                     │
        │                          ▼                                     │
        │                  IPrintFarmClient  (pure interface)            │
        │                          │                                     │
        │                  RestPrintFarmClient                           │
        └──────────────────────────┼────────────────────────────────────┘
                                    ▼
                            Slic3r::Http (libcurl)
                                    ▼
        ┌──────────────── 3D-FarmLab backend (unmodified) ──────────────┐
        │  /api/auth/login·logout·session      (session cookie)          │
        │  /api/printers, /api/queue           (session, read)           │
        │  /printers/<id>/api/files/local      (slicer-proxy, API key)   │
        │  /healthz                            (reachability)            │
        └────────────────────────────────────────────────────────────────┘
```

### 4.1 Abstraction layer (`IPrintFarmClient`)

```cpp
// src/slic3r/Utils/PrintFarm/PrintFarmClient.hpp
namespace Slic3r {

struct PfPrinter {           // mirrors GET /api/printers record
    std::string id, name, model, profile, status, error_message;
    bool can_upload = false; // derived: profile supports slicer-proxy upload
};

struct PfJob {               // mirrors a queue row + local upload state
    std::string id, name, printer_id, status; // Pending/Uploading/Queued/Printing/Completed/Failed/Cancelled
    std::string submitted_at, submitter;
};

struct PfUser { std::string id, name, username, role; };

struct PfResult {            // uniform result; no exceptions across the boundary
    bool ok = false;
    int  http_status = 0;
    std::string error;       // user-facing message ("" on success)
};

class IPrintFarmClient {
public:
    virtual ~IPrintFarmClient() = default;

    // Session auth (general access). Captures pf_session cookie in memory on success.
    virtual PfResult login(const std::string& email, const std::string& password, PfUser& out) = 0;
    virtual PfResult logout() = 0;
    virtual bool     is_authenticated() const = 0;

    // Reachability
    virtual PfResult ping() = 0; // GET /healthz

    // Printers (session token)
    virtual PfResult get_printers(std::vector<PfPrinter>& out) = 0;
    virtual PfResult get_printer(const std::string& id, PfPrinter& out) = 0;

    // Jobs (session token)
    virtual PfResult get_jobs(std::vector<PfJob>& out) = 0;

    // Print actions (Print API Key) — multipart upload to slicer-proxy.
    virtual PfResult upload_job(const std::string& printer_id,
                                const std::string& file_path,
                                std::function<void(int /*pct*/)> on_progress,
                                PfJob& out) = 0;
};

// Factory + accessor (built from current settings)
std::unique_ptr<IPrintFarmClient> make_print_farm_client(const PrintFarmConfig&);
} // namespace Slic3r
```

`RestPrintFarmClient` implements this on top of `Http`. It holds the in-memory `pf_session` cookie
and the (optionally configured) Print API Key, and selects the credential per call: **session
cookie** for `/api/*`, **`X-Api-Key`** for the slicer-proxy upload.

### 4.2 Session & lifecycle

`PrintFarmSession` owns the cookie string and `PfUser` purely in RAM. Created on login, cleared on
logout and in `GUI_App::OnExit`. Nothing touches `AppConfig`/disk.

### 4.3 Configuration (non-secret, persisted)

Stored via `AppConfig` under a `print_farm` section:

| Key | Meaning | Default |
|-----|---------|---------|
| `print_farm_url` | Base URL (e.g. `https://farm.example.com`) | empty |
| `print_farm_auth_mode` | `session` \| `apikey` \| `both` | `both` |
| `print_farm_refresh_interval` | Jobs/printers auto-refresh seconds | `30` |
| `print_farm_api_key` | **Optional** Print API Key. Persisted only if user opts in; otherwise prompted per session and held in memory. | empty |

> Tokens/cookies are **never** persisted. The API key is borderline: the brief says session tokens
> must not be stored, and treats the Print API Key as a configured value. We default to **not**
> persisting it and offer an explicit "remember API key" opt-in stored in `AppConfig` (documented as
> a security trade-off), matching how OctoPrint host API keys are already stored today.

---

## 5. Source-tree changes (new files, isolation-first)

```
docs/printfarm-integration-plan.md                         (this file)
src/slic3r/Utils/PrintFarm/
    PrintFarmClient.hpp           # IPrintFarmClient, DTOs, PrintFarmConfig
    RestPrintFarmClient.hpp/.cpp  # Http-based implementation
    PrintFarmSession.hpp/.cpp     # in-memory session/cookie + Print API Key
src/slic3r/GUI/PrintFarm/
    PrintFarmManager.hpp/.cpp     # singleton facade: printer cache, refresh timer, signals
    PrintFarmLoginDialog.hpp/.cpp # startup login gate (email + password)
    PrintFarmJobsPanel.hpp/.cpp   # Jobs monitoring page
    PrintFarmSettingsPanel.hpp/.cpp # Settings section
```

Minimal edits to existing files (kept surgical for rebase):

- `src/slic3r/GUI/GUI_App.cpp` — invoke the login gate in `OnInit` before showing the main frame;
  clear session in `OnExit`.
- `src/slic3r/GUI/MainFrame.cpp` — register the Jobs page and the "Upload to Farm" action.
- `src/slic3r/GUI/Plater.cpp` — add an "Upload to Farm" entry point next to the existing
  send/export action (gated; falls back to stock behavior when farm URL is unset).
- `src/slic3r/CMakeLists.txt` — add the new sources.

All new behavior is **gated on `print_farm_url` being configured**; with it empty OrcaSlicer behaves
exactly as upstream (satisfies "features gated by options must not affect existing behavior").

---

## 6. Risks & mitigations

| Risk | Mitigation |
|------|------------|
| **Login is cookie-based, not a JSON bearer token.** | Capture `pf_session` from `Set-Cookie` via `Http::on_header_callback`; resend as `Cookie` header. Verified the backend reads it from the cookie (`SESSION_COOKIE='pf_session'`, `server/app.js:722`). |
| Password must be sha256-hashed client-side. | Hash with OpenSSL/`boost` before POST (backend never sees plaintext; we never log it). |
| **Upload returns no durable job id.** | Track uploads locally (Pending→Uploading→Queued/Failed) and correlate Queued/Printing/Completed from `GET /api/queue` + printer `status`. Document the limitation; if the farm later adds a job id we adopt it. |
| slicer-proxy port differs from dashboard port. | Make the proxy base configurable; default to deriving from `print_farm_url` (nginx publishes both on `HTTP_PORT`). Behavior confirmed in `slicer-proxy/index.js` (`defaultAppBase`). |
| Self-signed farm TLS. | Use `Http::tls_verify(true)` by default for credentialed calls; expose an explicit "allow self-signed" toggle (off by default), mirroring existing print-host handling. |
| Rebase drift against upstream OrcaSlicer. | Keep ~99% of code in new files; touch existing files in ≤5 small, clearly-commented hunks. See §8. |
| Profiles that don't support upload (generic). | `PfPrinter.can_upload` derived from profile; UI disables Upload for unsupported printers (matches proxy's `415`). |

---

## 7. Implementation phases (small commits)

1. **`docs`** — this plan. *(commit: `docs(printfarm): integration analysis & plan`)*
2. **`refactor(api)`** — `IPrintFarmClient` + DTOs + `RestPrintFarmClient` + `PrintFarmSession`;
   compiles, no UI wiring. Unit-testable.
3. **`feat(auth)`** — `PrintFarmLoginDialog` + startup gate in `GUI_App` + in-memory session clear
   on exit.
4. **`feat(ui)`** — `PrintFarmSettingsPanel` (URL, auth mode, API key, refresh interval).
5. **`feat(printers)`** — `PrintFarmManager` printer sync (auto on login + manual refresh) and a
   printer picker.
6. **`feat(jobs)`** — "Upload to Farm" workflow onto `/printers/<id>/api/files/local`.
7. **`feat(jobs)`** — Jobs monitoring page (manual + auto refresh, details).
8. **`test/docs`** — targeted tests for the client (mocked Http), build & test instructions, this
   doc updated with final wiring.

Each phase builds and is independently reviewable.

---

## 8. Rebase strategy for future OrcaSlicer updates

- **New files over edits.** All logic lives under `src/slic3r/{Utils,GUI}/PrintFarm/`; upstream
  rarely touches new directories, so they merge cleanly.
- **Tagged edit hunks.** Every edit to an existing upstream file is wrapped with
  `// >>> PRINTFARM` / `// <<< PRINTFARM` markers and kept to a few lines, so conflicts are trivial
  to resolve and easy to grep (`git grep PRINTFARM`).
- **No edits to slicing/engine code** — zero overlap with the hottest-changing areas.
- **CMake additions appended** in a dedicated block, not interleaved.
- Maintain this branch as `feature/printfarm-integration`; rebase onto upstream `main`, resolve the
  handful of marked hunks, rebuild. A short `REBASE.md` checklist will accompany the final phase.

---

## 9. Deliverable mapping (brief → artifact)

| Brief deliverable | Where |
|---|---|
| Architecture analysis | §2, §3 |
| Backend API analysis | §3, `~/3D-FarmLab/API.md` |
| Integration design | §4 |
| Class diagram | §4 (ASCII) + `IPrintFarmClient` |
| Source tree changes | §5 |
| Implementation plan | §7 |
| Build instructions | `AGENTS.md` build cmds + final-phase notes |
| Testing instructions | Phase 8 |
| Rebase strategy | §8 |
