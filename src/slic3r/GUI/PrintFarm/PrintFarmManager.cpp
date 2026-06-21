// >>> PRINTFARM
#include "PrintFarmManager.hpp"

#include <algorithm>

#include <boost/log/trivial.hpp>

#include "libslic3r/AppConfig.hpp"

namespace Slic3r {
namespace GUI {

static const char* PF_SECTION = "print_farm";

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

    // The API key is persisted only when the user opted in. Never persist tokens/cookies.
    m_config.api_key = m_remember_api_key ? cfg->get(PF_SECTION, "api_key") : std::string{};

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

    if (m_remember_api_key)
        cfg->set(PF_SECTION, "api_key", m_config.api_key);
    else
        cfg->erase(PF_SECTION, "api_key");
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
