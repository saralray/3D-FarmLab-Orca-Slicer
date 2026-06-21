#ifndef slic3r_GUI_PrintFarmManager_hpp_
#define slic3r_GUI_PrintFarmManager_hpp_

// >>> PRINTFARM
// GUI-side facade over IPrintFarmClient. Owns:
//   * the persisted (non-secret) PrintFarmConfig, loaded/saved via AppConfig,
//   * the REST client instance and therefore the in-memory session (cookie/user),
//   * a cached printer list refreshed on login / on demand.
//
// A single instance lives for the app's lifetime; clear_session() (logout) and
// app exit destroy all auth state. Nothing here is persisted to disk except the
// non-secret config (and, only if the user opts in, the Print API Key).

#include <memory>
#include <mutex>
#include <vector>

#include "slic3r/Utils/PrintFarm/PrintFarmClient.hpp"

namespace Slic3r {
class AppConfig;

namespace GUI {

class PrintFarmManager
{
public:
    static PrintFarmManager& instance();

    // Config (section "print_farm" in AppConfig). load_config also (re)builds the client.
    void                   load_config(AppConfig* cfg);
    void                   save_config(AppConfig* cfg) const;
    const PrintFarmConfig& config() const { return m_config; }
    void                   set_config(const PrintFarmConfig& cfg); // updates client too
    bool                   remember_api_key() const { return m_remember_api_key; }
    void                   set_remember_api_key(bool v) { m_remember_api_key = v; }

    // Whether the farm integration is active (URL configured).
    bool is_enabled() const { return m_config.is_configured(); }

    // Session
    PfResult login(const std::string& email, const std::string& password);
    void     logout();                 // best-effort server logout + clear local state
    void     clear_session();          // local-only clear (called on app exit)
    bool     is_logged_in() const;
    PfUser   user() const;

    // Direct client access for panels (still goes through the abstraction layer).
    IPrintFarmClient* client() const { return m_client.get(); }

    // Cached printers (refreshed from the backend).
    PfResult                      refresh_printers();
    std::vector<PfPrinter>        printers() const;

private:
    PrintFarmManager() = default;
    void rebuild_client();

    mutable std::mutex                m_mutex;
    PrintFarmConfig                   m_config;
    bool                              m_remember_api_key = false;
    std::unique_ptr<IPrintFarmClient> m_client;
    std::vector<PfPrinter>            m_printers;
};

} // namespace GUI
} // namespace Slic3r
// <<< PRINTFARM

#endif // slic3r_GUI_PrintFarmManager_hpp_
