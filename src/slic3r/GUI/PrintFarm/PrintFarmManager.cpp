// >>> PRINTFARM
#include "PrintFarmManager.hpp"

#include <algorithm>

#include <boost/log/trivial.hpp>

#include <wx/secretstore.h>

#include "libslic3r/AppConfig.hpp"

namespace Slic3r {
namespace GUI {

static const char* PF_SECTION = "print_farm";

// The Print API Key is a secret and must never sit in the plaintext config file.
// It is stored in the OS keychain (libsecret / macOS Keychain / Windows Credential
// Manager) via wxSecretStore -- the same facility OrcaCloudServiceAgent uses for
// auth tokens. A dedicated service name keeps it isolated from the cloud entry.
static const char* PF_SECRET_SERVICE = "OrcaSlicer/PrintFarm";
static const char* PF_SECRET_USER    = "print_api_key";

// Returns false when no keychain is available; callers then keep the key in
// memory for the session only rather than ever writing it to disk.
static bool secret_store_save_api_key(const std::string& key)
{
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk()) {
        BOOST_LOG_TRIVIAL(warning) << "[printfarm] system keychain unavailable; API key not persisted";
        return false;
    }
    wxSecretValue value(wxString::FromUTF8(key.c_str()));
    if (!store.Save(PF_SECRET_SERVICE, PF_SECRET_USER, value)) {
        BOOST_LOG_TRIVIAL(warning) << "[printfarm] failed to save API key to system keychain";
        return false;
    }
    return true;
}

static bool secret_store_load_api_key(std::string& out)
{
    out.clear();
    wxSecretStore store = wxSecretStore::GetDefault();
    if (!store.IsOk())
        return false;
    wxString      user;
    wxSecretValue value;
    if (store.Load(PF_SECRET_SERVICE, user, value) && value.IsOk()) {
        out.assign(static_cast<const char*>(value.GetData()), value.GetSize());
        return !out.empty();
    }
    return false;
}

static void secret_store_clear_api_key()
{
    wxSecretStore store = wxSecretStore::GetDefault();
    if (store.IsOk())
        store.Delete(PF_SECRET_SERVICE);
}

PrintFarmManager& PrintFarmManager::instance()
{
    static PrintFarmManager s_instance;
    return s_instance;
}

static PrintFarmConfig::AuthMode parse_auth_mode(const std::string& s)
{
    if (s == "session") return PrintFarmConfig::AuthMode::Session;
    if (s == "apikey")  return PrintFarmConfig::AuthMode::ApiKey;
    return PrintFarmConfig::AuthMode::Both;
}

static const char* auth_mode_to_string(PrintFarmConfig::AuthMode m)
{
    switch (m) {
    case PrintFarmConfig::AuthMode::Session: return "session";
    case PrintFarmConfig::AuthMode::ApiKey:  return "apikey";
    default:                                 return "both";
    }
}

void PrintFarmManager::load_config(AppConfig* cfg)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (cfg == nullptr)
        return;

    m_config.url              = cfg->get(PF_SECTION, "url");
    m_config.slicer_proxy_url = cfg->get(PF_SECTION, "slicer_proxy_url");
    m_config.auth_mode        = parse_auth_mode(cfg->get(PF_SECTION, "auth_mode"));
    m_config.allow_insecure_tls = cfg->get_bool(PF_SECTION, "allow_insecure_tls");
    m_remember_api_key        = cfg->get_bool(PF_SECTION, "remember_api_key");

    const std::string interval = cfg->get(PF_SECTION, "refresh_interval");
    if (!interval.empty()) {
        try { m_config.refresh_interval_s = std::max(5, std::stoi(interval)); } catch (...) {}
    }

    // The API key lives in the OS keychain, never in the config file, and only when
    // the user opted in. Never persist session tokens/cookies anywhere.
    m_config.api_key.clear();
    if (m_remember_api_key) {
        // Migrate any plaintext key written by older builds into the keychain, then
        // drop it from the config so it no longer sits on disk in the clear.
        const std::string legacy = cfg->get(PF_SECTION, "api_key");
        if (!legacy.empty()) {
            m_config.api_key = legacy;
            secret_store_save_api_key(legacy);
            cfg->erase(PF_SECTION, "api_key");
        } else {
            secret_store_load_api_key(m_config.api_key);
        }
    } else {
        cfg->erase(PF_SECTION, "api_key"); // clean up any stale plaintext entry
    }

    rebuild_client();
}

void PrintFarmManager::save_config(AppConfig* cfg) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (cfg == nullptr)
        return;

    cfg->set(PF_SECTION, "url", m_config.url);
    cfg->set(PF_SECTION, "slicer_proxy_url", m_config.slicer_proxy_url);
    cfg->set(PF_SECTION, "auth_mode", auth_mode_to_string(m_config.auth_mode));
    cfg->set(PF_SECTION, "refresh_interval", std::to_string(m_config.refresh_interval_s));
    cfg->set(PF_SECTION, "allow_insecure_tls", m_config.allow_insecure_tls);
    cfg->set(PF_SECTION, "remember_api_key", m_remember_api_key);

    // Secret: store in the OS keychain only. Always strip any legacy plaintext entry
    // from the config file (covers migration from older builds).
    cfg->erase(PF_SECTION, "api_key");
    if (m_remember_api_key && !m_config.api_key.empty())
        secret_store_save_api_key(m_config.api_key);
    else
        secret_store_clear_api_key();
}

void PrintFarmManager::set_config(const PrintFarmConfig& cfg)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = cfg;
    rebuild_client();
}

void PrintFarmManager::rebuild_client()
{
    // Caller holds m_mutex. Rebuilding drops any prior in-memory session.
    m_client = make_print_farm_client(m_config);
    m_printers.clear();
}

PfResult PrintFarmManager::login(const std::string& email, const std::string& password)
{
    IPrintFarmClient* client = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_client)
            rebuild_client();
        client = m_client.get();
    }
    if (!client)
        return PfResult::failure("Print Farm is not configured.");

    PfUser user;
    PfResult res = client->login(email, password, user);
    if (res.ok)
        BOOST_LOG_TRIVIAL(info) << "[printfarm] manager: user logged in";
    return res;
}

void PrintFarmManager::logout()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_client)
        m_client->logout();
    m_printers.clear();
}

void PrintFarmManager::clear_session()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_client)
        m_client->logout();
    m_printers.clear();
}

bool PrintFarmManager::is_logged_in() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_client && m_client->is_authenticated();
}

PfUser PrintFarmManager::user() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_client ? m_client->current_user() : PfUser{};
}

PfResult PrintFarmManager::refresh_printers()
{
    IPrintFarmClient* client = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        client = m_client.get();
    }
    if (!client)
        return PfResult::failure("Print Farm is not configured.");

    std::vector<PfPrinter> fetched;
    PfResult res = client->get_printers(fetched);
    if (res.ok) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_printers = std::move(fetched);
    }
    return res;
}

std::vector<PfPrinter> PrintFarmManager::printers() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_printers;
}

} // namespace GUI
} // namespace Slic3r
// <<< PRINTFARM
