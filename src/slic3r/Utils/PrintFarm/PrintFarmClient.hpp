#ifndef slic3r_PrintFarmClient_hpp_
#define slic3r_PrintFarmClient_hpp_

// >>> PRINTFARM
//
// Abstraction layer for the 3D-FarmLab print-farm backend.
//
// UI code must NOT perform HTTP directly; everything goes through IPrintFarmClient.
// The concrete implementation (RestPrintFarmClient) is built on Slic3r::Http.
//
// Two credentials are supported simultaneously (see docs/printfarm-integration-plan.md):
//   * Session cookie (pf_session) -- general access (login, printers, jobs). In-memory only.
//   * Print API Key (X-Api-Key, slicer_upload scope) -- print actions (upload to a printer).

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {

// Persisted (non-secret) configuration. Tokens/cookies are NEVER stored here.
struct PrintFarmConfig
{
    std::string url;                 // dashboard base URL, e.g. https://farm.example.com
    std::string slicer_proxy_url;    // optional override for the OctoPrint-emulating upload host;
                                     // empty => derive from `url`.
    std::string api_key;             // optional Print API Key (slicer_upload scope)
    enum class AuthMode { Session, ApiKey, Both } auth_mode = AuthMode::Both;
    int  refresh_interval_s = 30;    // jobs/printers auto-refresh cadence
    bool allow_insecure_tls = false; // allow self-signed farm certs (off by default)

    bool is_configured() const { return !url.empty(); }
};

// Mirrors a GET /api/printers record (live telemetry overlaid by the poller).
struct PfPrinter
{
    std::string id;
    std::string name;
    std::string model;
    std::string profile;        // snapmaker_u1, bambulab_a1_mini, bambulab_h2s/h2d/h2c, ...
    std::string status;         // idle | printing | offline
    std::string error_message;  // human-readable fault, empty when healthy
    bool        can_upload = false; // derived: profile supports slicer-proxy upload
};

// Job status as surfaced to the user. Backend "queue" rows + locally-tracked uploads.
enum class PfJobStatus { Pending, Uploading, Queued, Printing, Completed, Failed, Cancelled };
const char* to_string(PfJobStatus);

struct PfJob
{
    std::string id;
    std::string name;           // filename / subtask name
    std::string printer_id;
    std::string printer_name;
    PfJobStatus status = PfJobStatus::Pending;
    std::string submitted_at;
    std::string submitter;
    std::string detail;         // optional extra info (error text, progress, ...)
};

struct PfUser
{
    std::string id;
    std::string name;
    std::string username;
    std::string role;
};

// Uniform result. No exceptions cross the abstraction boundary.
struct PfResult
{
    bool        ok = false;
    int         http_status = 0;
    std::string error;          // user-facing message; empty on success

    static PfResult success(int status = 200) { return PfResult{true, status, {}}; }
    static PfResult failure(std::string msg, int status = 0) { return PfResult{false, status, std::move(msg)}; }
};

class IPrintFarmClient
{
public:
    using ProgressFn = std::function<void(int /*percent 0..100*/)>;

    virtual ~IPrintFarmClient() = default;

    // ---- Session auth (general access). Captures pf_session in memory on success. ----
    virtual PfResult login(const std::string& email, const std::string& password, PfUser& out) = 0;
    virtual PfResult logout() = 0;
    virtual bool     is_authenticated() const = 0;
    virtual PfUser   current_user() const = 0;

    // ---- Reachability ----
    virtual PfResult ping() = 0; // GET /healthz

    // ---- Printers (session credential) ----
    virtual PfResult get_printers(std::vector<PfPrinter>& out) = 0;
    virtual PfResult get_printer(const std::string& id, PfPrinter& out) = 0;

    // ---- Jobs (session credential) ----
    virtual PfResult get_jobs(std::vector<PfJob>& out) = 0;

    // ---- Print actions (Print API Key credential) ----
    // Multipart upload of a sliced file to the slicer-proxy; backend creates/starts the print.
    virtual PfResult upload_job(const std::string& printer_id,
                                const std::string& file_path,
                                const ProgressFn&  on_progress,
                                PfJob&             out) = 0;

    // Updates the credentials/endpoints used by subsequent calls (e.g. after a settings change).
    virtual void set_config(const PrintFarmConfig& cfg) = 0;
};

// Factory. Builds a RestPrintFarmClient from the given config.
std::unique_ptr<IPrintFarmClient> make_print_farm_client(const PrintFarmConfig& cfg);

} // namespace Slic3r
// <<< PRINTFARM

#endif // slic3r_PrintFarmClient_hpp_
