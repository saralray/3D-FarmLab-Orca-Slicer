// >>> PRINTFARM
#include "PrintFarmLoginPanel.hpp"

#include <wx/sizer.h>
#include <wx/statline.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PrintFarm/PrintFarmManager.hpp"
#include "libslic3r/AppConfig.hpp"

namespace Slic3r {
namespace GUI {

// Default backend address offered on first run, before any URL is configured.
static const char* PF_DEFAULT_URL = "http://127.0.0.1:8080";

PrintFarmLoginPanel::PrintFarmLoginPanel(wxWindow*             parent,
                                         std::function<void()> on_success,
                                         std::function<void()> on_quit)
    : wxPanel(parent, wxID_ANY)
    , m_on_success(std::move(on_success))
    , m_on_quit(std::move(on_quit))
{
    auto& mgr = PrintFarmManager::instance();
    const bool configured = mgr.is_enabled();

    SetBackgroundColour(wxColour(38, 38, 41)); // matches Orca's dark canvas

    // A centered "card" holds the form so the screen reads as an app login page,
    // not an OS dialog.
    auto* card = new wxPanel(this, wxID_ANY);
    card->SetBackgroundColour(wxColour(54, 54, 58));
    auto* card_sizer = new wxBoxSizer(wxVERTICAL);
    const int border = FromDIP(16);

    auto make_label = [&](const wxString& text) {
        auto* t = new wxStaticText(card, wxID_ANY, text);
        t->SetForegroundColour(wxColour(200, 200, 200));
        return t;
    };

    auto* heading = new wxStaticText(card, wxID_ANY, _L("Sign in to Print Farm"));
    wxFont hf = heading->GetFont();
    hf.SetPointSize(hf.GetPointSize() + 4);
    hf.MakeBold();
    heading->SetFont(hf);
    heading->SetForegroundColour(wxColour(240, 240, 240));
    card_sizer->Add(heading, 0, wxLEFT | wxRIGHT | wxTOP, border);

    auto* sub = make_label(_L("Sign in to load your farm printers and submit prints."));
    card_sizer->Add(sub, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(6));

    auto* grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(8));
    grid->AddGrowableCol(1, 1);

    // The server URL is always editable here so it can be corrected/changed even
    // after it has been configured (e.g. the backend moved) without needing to be
    // signed in to reach Settings.
    grid->Add(make_label(_L("Server URL")), 0, wxALIGN_CENTER_VERTICAL);
    m_server_url = new wxTextCtrl(card, wxID_ANY,
                                  wxString::FromUTF8(configured ? mgr.config().url : PF_DEFAULT_URL),
                                  wxDefaultPosition, wxSize(FromDIP(300), -1));
    grid->Add(m_server_url, 1, wxEXPAND);

    grid->Add(make_label(_L("Email")), 0, wxALIGN_CENTER_VERTICAL);
    m_email = new wxTextCtrl(card, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(300), -1));
    grid->Add(m_email, 1, wxEXPAND);

    grid->Add(make_label(_L("Password")), 0, wxALIGN_CENTER_VERTICAL);
    m_password = new wxTextCtrl(card, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                wxTE_PASSWORD | wxTE_PROCESS_ENTER);
    grid->Add(m_password, 1, wxEXPAND);

    card_sizer->Add(grid, 0, wxEXPAND | wxALL, border);

    m_error = new wxStaticText(card, wxID_ANY, wxEmptyString);
    m_error->SetForegroundColour(wxColour(220, 90, 90));
    card_sizer->Add(m_error, 0, wxLEFT | wxRIGHT, border);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    m_quit = new wxButton(card, wxID_ANY, _L("Quit"));
    m_sign_in = new wxButton(card, wxID_ANY, _L("Sign in"));
    m_sign_in->SetDefault();
    buttons->AddStretchSpacer(1);
    buttons->Add(m_quit, 0, wxRIGHT, FromDIP(8));
    buttons->Add(m_sign_in, 0);
    card_sizer->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, border);

    card->SetSizer(card_sizer);

    // Center the card in the panel.
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->AddStretchSpacer(1);
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    row->AddStretchSpacer(1);
    row->Add(card, 0, wxALIGN_CENTER_VERTICAL);
    row->AddStretchSpacer(1);
    root->Add(row, 0, wxEXPAND);
    root->AddStretchSpacer(1);
    SetSizer(root);

    m_sign_in->Bind(wxEVT_BUTTON, &PrintFarmLoginPanel::on_sign_in, this);
    m_password->Bind(wxEVT_TEXT_ENTER, &PrintFarmLoginPanel::on_sign_in, this);
    m_quit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (m_on_quit) m_on_quit(); });

    m_email->SetFocus();
}

void PrintFarmLoginPanel::set_busy(bool busy)
{
    m_sign_in->Enable(!busy);
    m_email->Enable(!busy);
    m_password->Enable(!busy);
    if (m_server_url)
        m_server_url->Enable(!busy);
    if (busy)
        m_error->SetLabel(_L("Signing in…"));
}

void PrintFarmLoginPanel::on_sign_in(wxCommandEvent& /*evt*/)
{
    const std::string email = into_u8(m_email->GetValue());
    const std::string pwd   = into_u8(m_password->GetValue());
    if (email.empty() || pwd.empty()) {
        m_error->SetLabel(_L("Please enter your email and password."));
        return;
    }

    auto& mgr = PrintFarmManager::instance();

    // First run: persist the server URL before authenticating.
    if (m_server_url != nullptr) {
        std::string url = into_u8(m_server_url->GetValue());
        while (!url.empty() && (url.back() == ' ' || url.back() == '/'))
            url.pop_back();
        if (url.empty()) {
            m_error->SetLabel(_L("Please enter the Print Farm server URL."));
            return;
        }
        PrintFarmConfig cfg = mgr.config();
        cfg.url = url;
        mgr.set_config(cfg);
        mgr.save_config(wxGetApp().app_config);
        wxGetApp().app_config->save();
    }

    set_busy(true);
    PfResult res = mgr.login(email, pwd);
    set_busy(false);

    if (res.ok) {
        if (m_on_success)
            m_on_success();
        return;
    }
    m_error->SetLabel(wxString::FromUTF8(res.error.empty() ? "Login failed." : res.error));
    m_password->SetValue(wxEmptyString);
    m_password->SetFocus();
    Layout();
}

} // namespace GUI
} // namespace Slic3r
// <<< PRINTFARM
