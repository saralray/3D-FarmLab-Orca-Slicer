#ifndef slic3r_RestPrintFarmClient_hpp_
#define slic3r_RestPrintFarmClient_hpp_

// >>> PRINTFARM
// REST implementation of IPrintFarmClient over Slic3r::Http (libcurl).
//
// All session state (the pf_session cookie + the resolved user) lives in this object's
// memory only. Destroying the client (logout / app exit) destroys the session. Nothing
// is written to disk.

#include <mutex>

#include "PrintFarmClient.hpp"

namespace Slic3r {

class RestPrintFarmClient : public IPrintFarmClient
{
public:
    explicit RestPrintFarmClient(PrintFarmConfig cfg);
    ~RestPrintFarmClient() override = default;

    PfResult login(const std::string& email, const std::string& password, PfUser& out) override;
    PfResult logout() override;
    bool     is_authenticated() const override;
    PfUser   current_user() const override;

    PfResult ping() override;

    PfResult get_printers(std::vector<PfPrinter>& out) override;
    PfResult get_printer(const std::string& id, PfPrinter& out) override;

    PfResult get_jobs(std::vector<PfJob>& out) override;
    PfResult get_job(const std::string& id, PfJob& out) override;
    PfResult cancel_job(const std::string& id) override;

    PfResult mint_upload_token() override;
    PfResult revoke_upload_token() override;

    PfResult upload_job(const std::string& printer_id,
                        const std::string& file_path,
                        const ProgressFn&  on_progress,
                        PfJob&             out) override;

    void set_config(const PrintFarmConfig& cfg) override;

private:
    // URL helpers
    std::string api_url(const std::string& path) const;      // <url>/api/...
    std::string proxy_url(const std::string& path) const;    // slicer-proxy base + path

    // Credential application
    void apply_session(class Http& http) const;  // Cookie: pf_session=...
    void apply_api_key(class Http& http) const;   // X-Api-Key: ...
    void apply_tls(class Http& http) const;

    mutable std::mutex m_mutex;
    PrintFarmConfig    m_cfg;
    std::string        m_session_cookie;     // pf_session value, in-memory only
    PfUser             m_user;
    bool               m_minted_token = false; // true if api_key was auto-minted this session
};

} // namespace Slic3r
// <<< PRINTFARM

#endif // slic3r_RestPrintFarmClient_hpp_
