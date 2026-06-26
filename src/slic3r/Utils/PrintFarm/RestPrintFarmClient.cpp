// >>> PRINTFARM
#include "RestPrintFarmClient.hpp"

#include <cctype>
#include <regex>
#include <sstream>

#include <openssl/sha.h>
#include <boost/log/trivial.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include "nlohmann/json.hpp"
#include "slic3r/Utils/Http.hpp"

namespace Slic3r {

using json = nlohmann::json;

// Structured log tag so farm activity is greppable. Never logs passwords or tokens.
#define PF_LOG(lvl) BOOST_LOG_TRIVIAL(lvl) << "[printfarm] "

const char* to_string(PfJobStatus s)
{
    switch (s) {
    case PfJobStatus::Pending:   return "Pending";
    case PfJobStatus::Uploading: return "Uploading";
    case PfJobStatus::Queued:    return "Queued";
    case PfJobStatus::Printing:  return "Printing";
    case PfJobStatus::Completed: return "Completed";
    case PfJobStatus::Failed:    return "Failed";
    case PfJobStatus::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

namespace {

// sha256 hex, matching the backend's client-side password hashing (server/app.js:540).
// Uses OpenSSL exactly like slic3r/Utils/SimplyPrint.cpp.
std::string sha256_hex(const std::string& input)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash);
    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.reserve(SHA256_DIGEST_LENGTH * 2);
    for (unsigned char c : hash) {
        out.push_back(hexd[c >> 4]);
        out.push_back(hexd[c & 0x0f]);
    }
    return out;
}

std::string trim_trailing_slashes(std::string s)
{
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

// Make a basename safe for the printer's FTP server. Bambu FTP returns
// "553 Could not create file" for names with spaces or non-ASCII characters,
// so we (1) drop the internal "orca-farm-<hex>-" temp prefix added in
// Plater::export_to_farm(), and (2) replace anything outside [A-Za-z0-9._-]
// with '_'. The ".gcode.3mf" bundle extension is preserved — that is exactly
// what Bambu printers expect to print.
std::string ftp_safe_filename(const std::string& path)
{
    std::string name = boost::filesystem::path(path).filename().string();
    name = std::regex_replace(name, std::regex(R"(^orca-farm-[0-9a-fA-F]+-)"), "");
    for (char& c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-'))
            c = '_';
    }
    if (name.empty())
        name = "print.gcode.3mf";
    // Bambu FTP also rejects very long names; cap the stem while keeping the
    // ".gcode.3mf" (or other) extension intact.
    constexpr size_t kMaxLen = 100;
    if (name.size() > kMaxLen) {
        const std::string ext = boost::algorithm::ends_with(name, ".gcode.3mf")
                                    ? ".gcode.3mf"
                                    : boost::filesystem::path(name).extension().string();
        const size_t stem_len = ext.size() < kMaxLen ? kMaxLen - ext.size() : 0;
        name = name.substr(0, stem_len) + ext;
    }
    return name;
}

// Pull the pf_session value out of an accumulated Set-Cookie header blob.
std::string parse_session_cookie(const std::string& headers)
{
    // Set-Cookie: pf_session=<value>; Path=/; HttpOnly; ...
    std::smatch m;
    std::regex re(R"(pf_session=([^;\r\n]+))", std::regex::icase);
    if (std::regex_search(headers, m, re))
        return m[1].str();
    return {};
}

// Profiles the slicer-proxy can actually push to (slicer-proxy/index.js dispatch table).
bool profile_supports_upload(const std::string& profile)
{
    return profile == "snapmaker_u1" || profile == "bambulab_a1_mini" ||
           profile == "bambulab_h2s" || profile == "bambulab_h2d" ||
           profile == "bambulab_h2c";
}

PfPrinter parse_printer(const json& j)
{
    PfPrinter p;
    p.id            = j.value("id", std::string{});
    p.name          = j.value("name", std::string{});
    p.model         = j.value("model", std::string{});
    p.profile       = j.value("profile", std::string{});
    p.status        = j.value("status", std::string{});
    if (j.contains("errorMessage") && !j["errorMessage"].is_null())
        p.error_message = j["errorMessage"].get<std::string>();
    p.can_upload = profile_supports_upload(p.profile);
    if (j.contains("spools") && j["spools"].is_array()) {
        for (const auto& s : j["spools"]) {
            PfSpool spool;
            spool.id        = s.value("id", std::string{});
            spool.color     = s.value("color", std::string{});
            spool.material  = s.value("material", std::string{});
            spool.remaining = s.value("remaining", 0.0);
            spool.weight    = s.value("weight", 0.0);
            p.spools.push_back(std::move(spool));
        }
    }
    return p;
}

// Map a backend queue row's printed_status to our PfJobStatus.
PfJobStatus map_queue_status(const json& j)
{
    const std::string s = j.value("printedStatus", j.value("printed_status", std::string{}));
    if (s == "printing")  return PfJobStatus::Printing;
    if (s == "printed" || s == "done" || s == "completed") return PfJobStatus::Completed;
    if (s == "failed" || s == "error")     return PfJobStatus::Failed;
    if (s == "cancelled" || s == "canceled") return PfJobStatus::Cancelled;
    return PfJobStatus::Queued;
}

// Best-effort human message for a non-2xx / transport error.
std::string http_error_message(const std::string& body, const std::string& error, unsigned status)
{
    if (!error.empty())
        return error;
    // The backend returns { "error": "..." } on most failures.
    try {
        auto j = json::parse(body);
        if (j.contains("error") && j["error"].is_string())
            return j["error"].get<std::string>();
    } catch (...) {}
    if (status != 0)
        return "Request failed (HTTP " + std::to_string(status) + ")";
    return "Request failed";
}

} // namespace

RestPrintFarmClient::RestPrintFarmClient(PrintFarmConfig cfg) : m_cfg(std::move(cfg)) {}

void RestPrintFarmClient::set_config(const PrintFarmConfig& cfg)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cfg = cfg;
}

std::string RestPrintFarmClient::api_url(const std::string& path) const
{
    return trim_trailing_slashes(m_cfg.url) + path;
}

std::string RestPrintFarmClient::proxy_url(const std::string& path) const
{
    const std::string base = m_cfg.slicer_proxy_url.empty() ? m_cfg.url : m_cfg.slicer_proxy_url;
    return trim_trailing_slashes(base) + path;
}

void RestPrintFarmClient::apply_session(Http& http) const
{
    if (!m_session_cookie.empty())
        http.header("Cookie", "pf_session=" + m_session_cookie);
}

void RestPrintFarmClient::apply_api_key(Http& http) const
{
    if (!m_cfg.api_key.empty())
        http.header("X-Api-Key", m_cfg.api_key);
}

void RestPrintFarmClient::apply_tls(Http& http) const
{
    // Verify by default for credentialed calls; only relax when the user opted in.
    http.tls_verify(!m_cfg.allow_insecure_tls);
}

bool RestPrintFarmClient::is_authenticated() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_session_cookie.empty();
}

PfUser RestPrintFarmClient::current_user() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_user;
}

PfResult RestPrintFarmClient::ping()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_cfg.is_configured())
        return PfResult::failure("Print Farm URL is not configured.");

    PfResult result = PfResult::failure("Backend unreachable.");
    auto http = Http::get(api_url("/healthz"));
    apply_tls(http);
    http.timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string, unsigned status) { result = PfResult::success(status); })
        .on_error([&](std::string body, std::string error, unsigned status) {
            result = PfResult::failure(http_error_message(body, error, status), status);
        })
        .perform_sync();
    return result;
}

PfResult RestPrintFarmClient::login(const std::string& email, const std::string& password, PfUser& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_cfg.is_configured())
        return PfResult::failure("Print Farm URL is not configured.");

    // The backend expects { username, passwordHash } with a client-side sha256 of the password.
    json body = {
        {"username", email},
        {"passwordHash", sha256_hex(password)},
        {"remember", false},
    };

    PfResult    result = PfResult::failure("Login failed.");
    std::string headers_blob;
    std::string resp_body;

    auto http = Http::post(api_url("/api/auth/login"));
    apply_tls(http);
    http.header("Content-Type", "application/json")
        // Bounded timeout so an unreachable backend fails the sign-in promptly
        // instead of blocking the UI thread (login runs synchronously). Matches
        // the timeouts used by the other endpoints in this client.
        .timeout_connect(5)
        .timeout_max(20)
        .set_post_body(body.dump())
        .on_header_callback([&](std::string h) { headers_blob = std::move(h); })
        .on_complete([&](std::string b, unsigned status) {
            resp_body = std::move(b);
            result    = PfResult::success(status);
        })
        .on_error([&](std::string b, std::string error, unsigned status) {
            if (status == 401)
                result = PfResult::failure("Invalid email or password.", status);
            else if (status == 429)
                result = PfResult::failure("Too many attempts. Please wait and try again.", status);
            else
                result = PfResult::failure(http_error_message(b, error, status), status);
        })
        .perform_sync();

    if (!result.ok) {
        PF_LOG(warning) << "login failed for user '" << email << "' (status " << result.http_status << ")";
        return result;
    }

    const std::string cookie = parse_session_cookie(headers_blob);
    if (cookie.empty()) {
        PF_LOG(error) << "login response carried no pf_session cookie";
        return PfResult::failure("Login succeeded but no session was issued by the server.");
    }

    m_session_cookie = cookie; // in-memory only
    try {
        auto j = json::parse(resp_body);
        const auto& u = j.contains("user") ? j["user"] : j;
        m_user.id       = u.value("id", std::string{});
        m_user.name     = u.value("name", std::string{});
        m_user.username = u.value("username", email);
        m_user.role     = u.value("role", std::string{});
    } catch (...) {
        m_user = PfUser{};
        m_user.username = email;
    }
    out = m_user;
    PF_LOG(info) << "login succeeded for '" << m_user.username << "' role=" << m_user.role;
    return PfResult::success(result.http_status);
}

PfResult RestPrintFarmClient::logout()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // Best-effort server-side logout; clear local state regardless of the outcome.
    if (!m_session_cookie.empty() && m_cfg.is_configured()) {
        auto http = Http::post(api_url("/api/auth/logout"));
        apply_tls(http);
        apply_session(http);
        http.timeout_max(10)
            .on_complete([](std::string, unsigned) {})
            .on_error([](std::string, std::string, unsigned) {})
            .perform_sync();
    }
    m_session_cookie.clear();
    m_user = PfUser{};
    PF_LOG(info) << "logout: session state cleared";
    return PfResult::success();
}

PfResult RestPrintFarmClient::get_printers(std::vector<PfPrinter>& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_session_cookie.empty())
        return PfResult::failure("Not signed in.", 401);

    PfResult    result = PfResult::failure("Failed to load printers.");
    std::string resp;
    auto http = Http::get(api_url("/api/printers"));
    apply_tls(http);
    apply_session(http);
    http.timeout_max(20)
        .on_complete([&](std::string b, unsigned status) { resp = std::move(b); result = PfResult::success(status); })
        .on_error([&](std::string b, std::string error, unsigned status) {
            result = PfResult::failure(http_error_message(b, error, status), status);
        })
        .perform_sync();

    if (!result.ok)
        return result;

    out.clear();
    try {
        auto j = json::parse(resp);
        const auto& arr = j.is_array() ? j : (j.contains("printers") ? j["printers"] : json::array());
        for (const auto& item : arr)
            out.push_back(parse_printer(item));
    } catch (const std::exception& e) {
        return PfResult::failure(std::string("Could not parse printer list: ") + e.what());
    }
    PF_LOG(info) << "synced " << out.size() << " printers";
    return result;
}

PfResult RestPrintFarmClient::get_printer(const std::string& id, PfPrinter& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_session_cookie.empty())
        return PfResult::failure("Not signed in.", 401);

    PfResult    result = PfResult::failure("Failed to load printer.");
    std::string resp;
    auto http = Http::get(api_url("/api/printers/" + Http::url_encode(id)));
    apply_tls(http);
    apply_session(http);
    http.timeout_max(15)
        .on_complete([&](std::string b, unsigned status) { resp = std::move(b); result = PfResult::success(status); })
        .on_error([&](std::string b, std::string error, unsigned status) {
            result = PfResult::failure(http_error_message(b, error, status), status);
        })
        .perform_sync();
    if (!result.ok)
        return result;
    try {
        out = parse_printer(json::parse(resp));
    } catch (const std::exception& e) {
        return PfResult::failure(std::string("Could not parse printer: ") + e.what());
    }
    return result;
}

PfResult RestPrintFarmClient::get_jobs(std::vector<PfJob>& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_session_cookie.empty())
        return PfResult::failure("Not signed in.", 401);

    PfResult    result = PfResult::failure("Failed to load jobs.");
    std::string resp;
    auto http = Http::get(api_url("/api/queue"));
    apply_tls(http);
    apply_session(http);
    http.timeout_max(20)
        .on_complete([&](std::string b, unsigned status) { resp = std::move(b); result = PfResult::success(status); })
        .on_error([&](std::string b, std::string error, unsigned status) {
            result = PfResult::failure(http_error_message(b, error, status), status);
        })
        .perform_sync();
    if (!result.ok)
        return result;

    out.clear();
    try {
        auto j = json::parse(resp);
        const auto& arr = j.is_array() ? j : (j.contains("jobs") ? j["jobs"] : json::array());
        for (const auto& item : arr) {
            PfJob job;
            job.id           = item.value("id", std::string{});
            job.name         = item.value("filename", item.value("name", std::string{}));
            job.printer_id   = item.value("printerId", item.value("printer_id", std::string{}));
            job.printer_name = item.value("printerName", std::string{});
            job.submitted_at = item.value("submittedAt", item.value("submitted_at", std::string{}));
            job.submitter    = item.value("submitterName", item.value("submitter", std::string{}));
            job.status       = map_queue_status(item);
            out.push_back(std::move(job));
        }
    } catch (const std::exception& e) {
        return PfResult::failure(std::string("Could not parse jobs: ") + e.what());
    }
    return result;
}

PfResult RestPrintFarmClient::get_job(const std::string& id, PfJob& out)
{
    // The backend exposes the queue as a list (GET /api/queue); there is no
    // single-job endpoint, so fetch the list and select the requested row.
    std::vector<PfJob> jobs;
    PfResult res = get_jobs(jobs);
    if (!res.ok)
        return res;
    for (auto& j : jobs) {
        if (j.id == id) {
            out = std::move(j);
            return PfResult::success(res.http_status);
        }
    }
    return PfResult::failure("Job no longer exists.", 404);
}

PfResult RestPrintFarmClient::cancel_job(const std::string& id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_session_cookie.empty())
        return PfResult::failure("Not signed in.", 401);
    if (id.empty())
        return PfResult::failure("No job selected.");

    PfResult result = PfResult::failure("Failed to cancel the job.");
    auto http = Http::del(api_url("/api/queue/" + Http::url_encode(id)));
    apply_tls(http);
    apply_session(http);
    http.timeout_max(20)
        .on_complete([&](std::string, unsigned status) { result = PfResult::success(status); })
        .on_error([&](std::string b, std::string error, unsigned status) {
            result = PfResult::failure(http_error_message(b, error, status), status);
        })
        .perform_sync();
    PF_LOG(info) << "cancel_job id=" << id << (result.ok ? " ok" : " failed");
    return result;
}

PfResult RestPrintFarmClient::mint_upload_token()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_session_cookie.empty())
        return PfResult::failure("Not signed in.", 401);

    PfResult    result = PfResult::failure("Failed to obtain an upload token.");
    std::string resp;
    auto http = Http::post(api_url("/api/auth/slicer-token"));
    apply_tls(http);
    apply_session(http);
    http.header("Content-Type", "application/json")
        .set_post_body(std::string("{}"))
        .timeout_max(20)
        .on_complete([&](std::string b, unsigned status) { resp = std::move(b); result = PfResult::success(status); })
        .on_error([&](std::string b, std::string error, unsigned status) {
            result = PfResult::failure(http_error_message(b, error, status), status);
        })
        .perform_sync();
    if (!result.ok)
        return result;

    try {
        auto j = json::parse(resp);
        const std::string key = j.value("key", std::string{});
        if (key.empty())
            return PfResult::failure("Server did not return an upload token.");
        m_cfg.api_key  = key;   // in-memory only; never persisted
        m_minted_token = true;
    } catch (const std::exception& e) {
        return PfResult::failure(std::string("Could not parse upload token: ") + e.what());
    }
    PF_LOG(info) << "minted ephemeral upload token";
    return result;
}

PfResult RestPrintFarmClient::revoke_upload_token()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    // Only revoke a token we minted; never touch a manually-configured key.
    if (!m_minted_token)
        return PfResult::success();

    PfResult result = PfResult::failure("Failed to revoke upload token.");
    if (!m_session_cookie.empty()) {
        auto http = Http::del(api_url("/api/auth/slicer-token"));
        apply_tls(http);
        apply_session(http);
        http.timeout_max(10)
            .on_complete([&](std::string, unsigned status) { result = PfResult::success(status); })
            .on_error([&](std::string b, std::string error, unsigned status) {
                result = PfResult::failure(http_error_message(b, error, status), status);
            })
            .perform_sync();
    }
    m_cfg.api_key.clear();
    m_minted_token = false;
    PF_LOG(info) << "revoked ephemeral upload token";
    return result;
}

PfResult RestPrintFarmClient::upload_job(const std::string& printer_id,
                                         const std::string& file_path,
                                         const ProgressFn&  on_progress,
                                         PfJob&             out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_cfg.is_configured())
        return PfResult::failure("Print Farm URL is not configured.");
    if (m_cfg.api_key.empty())
        return PfResult::failure("A Print API Key is required to upload prints. Set one in Print Farm settings.");
    if (printer_id.empty())
        return PfResult::failure("No target printer selected.");

    // slicer-proxy OctoPrint emulation: POST /printers/<id>/api/files/local, multipart "file".
    const std::string url = proxy_url("/printers/" + Http::url_encode(printer_id) + "/api/files/local");

    PfResult result = PfResult::failure("Upload failed.");
    auto http = Http::post(url);
    apply_tls(http);
    apply_api_key(http);
    // OctoPrint-emulation: "select"+"print" tell the backend to start the print
    // right after the file lands, instead of only storing it on the printer.
    http.form_add("select", "true")
        .form_add("print", "true")
        .form_add_file("file", file_path, ftp_safe_filename(file_path))
        .on_progress([&](Http::Progress progress, bool& /*cancel*/) {
            if (on_progress && progress.ultotal > 0) {
                int pct = static_cast<int>((progress.ulnow * 100) / progress.ultotal);
                on_progress(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
            }
        })
        .on_complete([&](std::string, unsigned status) { result = PfResult::success(status); })
        .on_error([&](std::string b, std::string error, unsigned status) {
            std::string msg = http_error_message(b, error, status);
            if (status == 401 || status == 403)
                msg = "Upload rejected: invalid API key or missing 'slicer_upload' permission.";
            else if (status == 404)
                msg = "Upload rejected: printer not found on the farm.";
            else if (status == 415)
                msg = "This printer's profile does not support slicer uploads.";
            // The backend relays the printer's FTP error verbatim. A 553 means the
            // printer could not create the file — almost always no/locked SD card
            // or a missing target folder, neither of which the slicer controls.
            else if (b.find("553") != std::string::npos ||
                     b.find("Could not create file") != std::string::npos)
                msg = "The printer rejected the file (FTP 553: could not create file). "
                      "Check that an SD card is inserted, has free space, and is not "
                      "write-protected, then try again.";
            result = PfResult::failure(msg, status);
        })
        .perform_sync();

    out = PfJob{};
    out.printer_id = printer_id;
    out.status     = result.ok ? PfJobStatus::Queued : PfJobStatus::Failed;
    if (!result.ok)
        out.detail = result.error;
    PF_LOG(info) << "upload to printer '" << printer_id << "' "
                 << (result.ok ? "accepted" : "failed") << " (status " << result.http_status << ")";
    return result;
}

std::unique_ptr<IPrintFarmClient> make_print_farm_client(const PrintFarmConfig& cfg)
{
    return std::make_unique<RestPrintFarmClient>(cfg);
}

} // namespace Slic3r
// <<< PRINTFARM
