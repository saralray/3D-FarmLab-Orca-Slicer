// >>> PRINTFARM
#include "PrintFarmLoginDialog.hpp"

#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/busyinfo.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PrintFarm/PrintFarmManager.hpp"

namespace Slic3r {
namespace GUI {

PrintFarmLoginDialog::PrintFarmLoginDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Sign in to Print Farm"), wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
{
    auto& mgr = PrintFarmManager::instance();

    auto* root = new wxBoxSizer(wxVERTICAL);
    const int border = FromDIP(12);

    auto* heading = new wxStaticText(this, wxID_ANY, _L("Sign in to continue"));
    wxFont hf = heading->GetFont();
    hf.SetPointSize(hf.GetPointSize() + 2);
    hf.MakeBold();
    heading->SetFont(hf);
    root->Add(heading, 0, wxLEFT | wxRIGHT | wxTOP, border);

    auto* server = new wxStaticText(this, wxID_ANY,
        wxString::Format(_L("Server: %s"), wxString::FromUTF8(mgr.config().url)));
    server->SetForegroundColour(wxColour(120, 120, 120));
    root->Add(server, 0, wxLEFT | wxRIGHT | wxTOP, border);

    auto* grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(8));
    grid->AddGrowableCol(1, 1);

    grid->Add(new wxStaticText(this, wxID_ANY, _L("Email")), 0, wxALIGN_CENTER_VERTICAL);
    m_email = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(260), -1));
    grid->Add(m_email, 1, wxEXPAND);

    grid->Add(new wxStaticText(this, wxID_ANY, _L("Password")), 0, wxALIGN_CENTER_VERTICAL);
    m_password = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD | wxTE_PROCESS_ENTER);
    grid->Add(m_password, 1, wxEXPAND);

    root->Add(grid, 0, wxEXPAND | wxALL, border);

    m_error = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_error->SetForegroundColour(wxColour(200, 60, 60));
    root->Add(m_error, 0, wxLEFT | wxRIGHT, border);

    auto* line = new wxStaticLine(this, wxID_ANY);
    root->Add(line, 0, wxEXPAND | wxALL, border);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    m_quit = new wxButton(this, wxID_CANCEL, _L("Quit"));
    m_sign_in = new wxButton(this, wxID_OK, _L("Sign in"));
    m_sign_in->SetDefault();
    buttons->AddStretchSpacer(1);
    buttons->Add(m_quit, 0, wxRIGHT, FromDIP(8));
    buttons->Add(m_sign_in, 0);
    root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, border);

    SetSizerAndFit(root);
    CentreOnScreen();

    m_sign_in->Bind(wxEVT_BUTTON, &PrintFarmLoginDialog::on_sign_in, this);
    m_password->Bind(wxEVT_TEXT_ENTER, &PrintFarmLoginDialog::on_sign_in, this);
    m_email->SetFocus();
}

void PrintFarmLoginDialog::set_busy(bool busy)
{
    m_sign_in->Enable(!busy);
    m_email->Enable(!busy);
    m_password->Enable(!busy);
    if (busy) {
        wxSetCursor(*wxHOURGLASS_CURSOR);
        m_error->SetLabel(_L("Signing in…"));
    } else {
        wxSetCursor(wxNullCursor);
    }
}

void PrintFarmLoginDialog::on_sign_in(wxCommandEvent& /*evt*/)
{
    const std::string email = into_u8(m_email->GetValue());
    const std::string pwd   = into_u8(m_password->GetValue());
    if (email.empty() || pwd.empty()) {
        m_error->SetLabel(_L("Please enter your email and password."));
        return;
    }

    set_busy(true);
    PfResult res = PrintFarmManager::instance().login(email, pwd);
    set_busy(false);

    if (res.ok) {
        EndModal(wxID_OK);
        return;
    }
    m_error->SetLabel(wxString::FromUTF8(res.error.empty() ? "Login failed." : res.error));
    m_password->SetValue(wxEmptyString);
    m_password->SetFocus();
    Layout();
    Fit();
}

void PrintFarmLoginDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    Fit();
    Refresh();
}

bool PrintFarmLoginDialog::run_login_gate(wxWindow* parent)
{
    if (!PrintFarmManager::instance().is_enabled())
        return true; // farm not configured -> behave like stock OrcaSlicer

    PrintFarmLoginDialog dlg(parent);
    return dlg.ShowModal() == wxID_OK;
}

} // namespace GUI
} // namespace Slic3r
// <<< PRINTFARM
