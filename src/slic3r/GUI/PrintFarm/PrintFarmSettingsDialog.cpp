// >>> PRINTFARM
#include "PrintFarmSettingsDialog.hpp"

#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PrintFarm/PrintFarmManager.hpp"
#include "libslic3r/AppConfig.hpp"

namespace Slic3r {
namespace GUI {

PrintFarmSettingsDialog::PrintFarmSettingsDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Print Farm Settings"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
{
    const int border = FromDIP(10);
    auto* root = new wxBoxSizer(wxVERTICAL);

    auto* grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(8));
    grid->AddGrowableCol(1, 1);
    const int lbl_flag = wxALIGN_CENTER_VERTICAL;

    auto add_row = [&](const wxString& label, wxWindow* ctrl) {
        grid->Add(new wxStaticText(this, wxID_ANY, label), 0, lbl_flag);
        grid->Add(ctrl, 1, wxEXPAND);
    };

    m_url = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(320), -1));
    add_row(_L("Print Farm URL"), m_url);

    m_proxy_url = new wxTextCtrl(this, wxID_ANY);
    add_row(_L("Upload host (optional)"), m_proxy_url);

    m_auth_mode = new wxChoice(this, wxID_ANY);
    m_auth_mode->Append(_L("Session + API Key"));   // both
    m_auth_mode->Append(_L("Session only"));        // session
    m_auth_mode->Append(_L("API Key only"));        // apikey
    add_row(_L("Authentication mode"), m_auth_mode);

    m_api_key = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
    add_row(_L("Print API Key"), m_api_key);

    m_remember_key = new wxCheckBox(this, wxID_ANY, _L("Remember API key in the system keychain"));
    add_row(wxEmptyString, m_remember_key);

    m_refresh = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                               wxSP_ARROW_KEYS, 5, 3600, 30);
    add_row(_L("Auto-refresh interval (s)"), m_refresh);

    m_insecure_tls = new wxCheckBox(this, wxID_ANY, _L("Allow self-signed TLS certificates"));
    add_row(wxEmptyString, m_insecure_tls);

    root->Add(grid, 0, wxEXPAND | wxALL, border);

    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    root->Add(m_status, 0, wxLEFT | wxRIGHT, border);

    root->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxALL, border);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* test = new wxButton(this, wxID_ANY, _L("Test connection"));
    auto* cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    auto* save = new wxButton(this, wxID_OK, _L("Save"));
    save->SetDefault();
    buttons->Add(test, 0, wxRIGHT, FromDIP(8));
    buttons->AddStretchSpacer(1);
    buttons->Add(cancel, 0, wxRIGHT, FromDIP(8));
    buttons->Add(save, 0);
    root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, border);

    SetSizerAndFit(root);
    CentreOnParent();

    test->Bind(wxEVT_BUTTON, &PrintFarmSettingsDialog::on_test, this);
    save->Bind(wxEVT_BUTTON, &PrintFarmSettingsDialog::on_save, this);

    load_from_manager();
}

void PrintFarmSettingsDialog::load_from_manager()
{
    auto& mgr = PrintFarmManager::instance();
    const PrintFarmConfig& c = mgr.config();
    m_url->SetValue(wxString::FromUTF8(c.url));
    m_proxy_url->SetValue(wxString::FromUTF8(c.slicer_proxy_url));
    switch (c.auth_mode) {
    case PrintFarmConfig::AuthMode::Session: m_auth_mode->SetSelection(1); break;
    case PrintFarmConfig::AuthMode::ApiKey:  m_auth_mode->SetSelection(2); break;
    default:                                 m_auth_mode->SetSelection(0); break;
    }
    m_api_key->SetValue(wxString::FromUTF8(c.api_key));
    m_remember_key->SetValue(mgr.remember_api_key());
    m_refresh->SetValue(c.refresh_interval_s);
    m_insecure_tls->SetValue(c.allow_insecure_tls);
}

static PrintFarmConfig::AuthMode auth_mode_from_choice(int sel)
{
    if (sel == 1) return PrintFarmConfig::AuthMode::Session;
    if (sel == 2) return PrintFarmConfig::AuthMode::ApiKey;
    return PrintFarmConfig::AuthMode::Both;
}

void PrintFarmSettingsDialog::on_test(wxCommandEvent& /*evt*/)
{
    PrintFarmConfig cfg;
    cfg.url               = into_u8(m_url->GetValue());
    cfg.slicer_proxy_url  = into_u8(m_proxy_url->GetValue());
    cfg.allow_insecure_tls = m_insecure_tls->GetValue();
    if (cfg.url.empty()) {
        m_status->SetForegroundColour(wxColour(200, 60, 60));
        m_status->SetLabel(_L("Enter a Print Farm URL first."));
        return;
    }
    auto client = make_print_farm_client(cfg);
    wxSetCursor(*wxHOURGLASS_CURSOR);
    PfResult res = client->ping();
    wxSetCursor(wxNullCursor);
    if (res.ok) {
        m_status->SetForegroundColour(wxColour(40, 150, 60));
        m_status->SetLabel(_L("Backend reachable."));
    } else {
        m_status->SetForegroundColour(wxColour(200, 60, 60));
        m_status->SetLabel(wxString::FromUTF8(res.error));
    }
    Layout();
}

void PrintFarmSettingsDialog::on_save(wxCommandEvent& /*evt*/)
{
    auto& mgr = PrintFarmManager::instance();
    PrintFarmConfig cfg = mgr.config();
    cfg.url               = into_u8(m_url->GetValue());
    cfg.slicer_proxy_url  = into_u8(m_proxy_url->GetValue());
    cfg.auth_mode         = auth_mode_from_choice(m_auth_mode->GetSelection());
    cfg.api_key           = into_u8(m_api_key->GetValue());
    cfg.refresh_interval_s = m_refresh->GetValue();
    cfg.allow_insecure_tls = m_insecure_tls->GetValue();

    mgr.set_remember_api_key(m_remember_key->GetValue());
    mgr.set_config(cfg);
    mgr.save_config(wxGetApp().app_config);
    wxGetApp().app_config->save();

    EndModal(wxID_OK);
}

void PrintFarmSettingsDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    Fit();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
// <<< PRINTFARM
