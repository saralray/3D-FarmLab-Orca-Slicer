// >>> PRINTFARM
#include "PrintFarmPanel.hpp"

#include <algorithm>

#include <wx/sizer.h>
#include <wx/wrapsizer.h>
#include <wx/scrolwin.h>
#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/statline.h>
#include <wx/button.h>
#include <wx/simplebook.h>
#include <wx/settings.h>
#include <wx/msgdlg.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PrintFarm/PrintFarmManager.hpp"

namespace Slic3r {
namespace GUI {

enum { PF_PANEL_TIMER_ID = wxID_HIGHEST + 4301 };

// Status vocabulary is "idle | printing | offline"; anything else is treated as
// unknown/offline. Colours match the dots shown in the printer selector.
static wxColour status_colour(const std::string& status)
{
    if (status == "printing") return wxColour(0x1A, 0x88, 0xE0); // blue
    if (status == "idle")     return wxColour(0x2E, 0xA0, 0x43); // green
    return wxColour(0x9A, 0x9A, 0x9A);                           // grey (offline/unknown)
}

static wxString status_label(const std::string& status)
{
    if (status == "printing") return _L("Printing");
    if (status == "idle")     return _L("Idle");
    if (status == "offline")  return _L("Offline");
    return status.empty() ? _L("Unknown") : wxString::FromUTF8(status);
}

PrintFarmPanel::PrintFarmPanel(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : wxPanel(parent, id, pos, size, style)
    , m_timer(this, PF_PANEL_TIMER_ID)
{
    SetBackgroundColour(*wxWHITE);
    build_ui();
    Bind(wxEVT_TIMER, &PrintFarmPanel::on_timer, this, PF_PANEL_TIMER_ID);
    update_auth_view();
}

PrintFarmPanel::~PrintFarmPanel()
{
    if (m_timer.IsRunning())
        m_timer.Stop();
}

void PrintFarmPanel::build_ui()
{
    const int border = FromDIP(12);

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_book = new wxSimplebook(this, wxID_ANY);
    root->Add(m_book, 1, wxEXPAND);
    SetSizer(root);

    // ---- Page 0: sign-in prompt (shown when logged out) ----
    auto* login_page = new wxPanel(m_book, wxID_ANY);
    login_page->SetBackgroundColour(*wxWHITE);
    {
        auto* col = new wxBoxSizer(wxVERTICAL);
        col->AddStretchSpacer(1);
        auto* title = new wxStaticText(login_page, wxID_ANY, _L("Sign in to Print Farm"));
        auto title_font = title->GetFont();
        title_font.SetPointSize(title_font.GetPointSize() + 4);
        title_font.MakeBold();
        title->SetFont(title_font);
        col->Add(title, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(8));

        auto* hint = new wxStaticText(login_page, wxID_ANY,
            _L("Connect to your 3D-FarmLab account to view printer status and the print queue."));
        hint->SetForegroundColour(wxColour(120, 120, 120));
        col->Add(hint, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(16));

        auto* sign_in = new wxButton(login_page, wxID_ANY, _L("Sign in"));
        sign_in->Bind(wxEVT_BUTTON, [](wxCommandEvent&) {
            if (wxGetApp().mainframe)
                wxGetApp().mainframe->show_print_farm_login();
        });
        col->Add(sign_in, 0, wxALIGN_CENTER_HORIZONTAL);
        col->AddStretchSpacer(1);

        auto* wrap = new wxBoxSizer(wxHORIZONTAL);
        wrap->AddStretchSpacer(1);
        wrap->Add(col, 0, wxALIGN_CENTER_VERTICAL);
        wrap->AddStretchSpacer(1);
        login_page->SetSizer(wrap);
    }
    m_book->AddPage(login_page, wxEmptyString);

    // ---- Page 1: dashboard (printers grid + job queue) ----
    auto* dash = new wxPanel(m_book, wxID_ANY);
    dash->SetBackgroundColour(*wxWHITE);
    auto* dash_sizer = new wxBoxSizer(wxVERTICAL);

    // Header: title + refresh + sync status.
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    auto* title = new wxStaticText(dash, wxID_ANY, _L("Printers"));
    auto title_font = title->GetFont();
    title_font.MakeBold();
    title->SetFont(title_font);
    header->Add(title, 0, wxALIGN_CENTER_VERTICAL);
    header->AddStretchSpacer(1);
    auto* refresh = new wxButton(dash, wxID_REFRESH, _L("Refresh"));
    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh_all(); });
    header->Add(refresh, 0, wxALIGN_CENTER_VERTICAL);
    dash_sizer->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, border);

    m_status = new wxStaticText(dash, wxID_ANY, wxEmptyString);
    m_status->SetForegroundColour(wxColour(120, 120, 120));
    dash_sizer->Add(m_status, 0, wxLEFT | wxRIGHT | wxTOP, border);

    // Printer status cards in a wrapping, scrollable area.
    m_cards_area = new wxScrolledWindow(dash, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_cards_area->SetBackgroundColour(*wxWHITE);
    m_cards_area->SetScrollRate(0, FromDIP(10));
    m_cards_sizer = new wxWrapSizer(wxHORIZONTAL);
    m_cards_area->SetSizer(m_cards_sizer);
    dash_sizer->Add(m_cards_area, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, border);

    dash_sizer->Add(new wxStaticLine(dash, wxID_ANY), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, border);

    // Job queue.
    auto* jobs_title = new wxStaticText(dash, wxID_ANY, _L("Queue"));
    jobs_title->SetFont(title_font);
    dash_sizer->Add(jobs_title, 0, wxLEFT | wxRIGHT | wxTOP, border);

    m_jobs = new wxListCtrl(dash, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(180)),
                            wxLC_REPORT | wxLC_SINGLE_SEL);
    m_jobs->InsertColumn(0, _L("Job"),       wxLIST_FORMAT_LEFT, FromDIP(220));
    m_jobs->InsertColumn(1, _L("Status"),    wxLIST_FORMAT_LEFT, FromDIP(110));
    m_jobs->InsertColumn(2, _L("Printer"),   wxLIST_FORMAT_LEFT, FromDIP(150));
    m_jobs->InsertColumn(3, _L("Submitter"), wxLIST_FORMAT_LEFT, FromDIP(120));
    m_jobs->InsertColumn(4, _L("Submitted"), wxLIST_FORMAT_LEFT, FromDIP(130));
    dash_sizer->Add(m_jobs, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, border);

    auto* job_buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* cancel = new wxButton(dash, wxID_ANY, _L("Cancel job"));
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const long sel = m_jobs->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (sel < 0 || sel >= static_cast<long>(m_jobs_cache.size())) {
            set_status(_L("Select a job to cancel."), true);
            return;
        }
        const PfJob job = m_jobs_cache[sel];
        wxMessageDialog confirm(this,
            wxString::Format(_L("Cancel the job \"%s\"?"), wxString::FromUTF8(job.name)),
            _L("Cancel job"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        if (confirm.ShowModal() != wxID_YES)
            return;
        auto* client = PrintFarmManager::instance().client();
        if (!client)
            return;
        PfResult res = client->cancel_job(job.id);
        if (!res.ok) {
            set_status(wxString::FromUTF8(res.error), true);
            return;
        }
        set_status(_L("Job cancelled."));
        refresh_jobs();
    });
    job_buttons->Add(cancel, 0);
    dash_sizer->Add(job_buttons, 0, wxALL, border);

    dash->SetSizer(dash_sizer);
    m_book->AddPage(dash, wxEmptyString);
}

wxWindow* PrintFarmPanel::make_printer_card(wxWindow* parent, const PfPrinter& p)
{
    auto* card = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(240), FromDIP(110)));
    card->SetBackgroundColour(wxColour(0xF6, 0xF6, 0xF6));

    auto* col = new wxBoxSizer(wxVERTICAL);
    const int pad = FromDIP(10);

    // Name (bold).
    auto* name = new wxStaticText(card, wxID_ANY, wxString::FromUTF8(p.name),
                                  wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    auto name_font = name->GetFont();
    name_font.MakeBold();
    name->SetFont(name_font);
    col->Add(name, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, pad);

    // Model / profile.
    const std::string model = p.model.empty() ? p.profile : p.model;
    auto* sub = new wxStaticText(card, wxID_ANY, wxString::FromUTF8(model),
                                 wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    sub->SetForegroundColour(wxColour(120, 120, 120));
    col->Add(sub, 0, wxLEFT | wxRIGHT | wxEXPAND, pad);

    col->AddStretchSpacer(1);

    // Status badge: coloured dot + label.
    auto* status_row = new wxBoxSizer(wxHORIZONTAL);
    const int dot = FromDIP(10);
    auto* dot_panel = new wxPanel(card, wxID_ANY, wxDefaultPosition, wxSize(dot, dot));
    dot_panel->SetBackgroundColour(status_colour(p.status));
    status_row->Add(dot_panel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    auto* status_txt = new wxStaticText(card, wxID_ANY, status_label(p.status));
    status_row->Add(status_txt, 0, wxALIGN_CENTER_VERTICAL);
    col->Add(status_row, 0, wxLEFT | wxRIGHT, pad);

    // Error message, if any.
    if (!p.error_message.empty()) {
        auto* err = new wxStaticText(card, wxID_ANY, wxString::FromUTF8(p.error_message),
                                     wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        err->SetForegroundColour(wxColour(200, 60, 60));
        col->Add(err, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, pad);
    } else {
        col->AddSpacer(pad);
    }

    card->SetSizer(col);
    return card;
}

void PrintFarmPanel::rebuild_printer_cards(const std::vector<PfPrinter>& printers)
{
    // Only rebuild when the printer set/status/error actually changed, to avoid
    // flicker and keep scrolling stable across the 30s refresh cadence.
    std::string signature;
    for (const auto& p : printers)
        signature += p.id + '|' + p.status + '|' + p.error_message + '\n';
    if (signature == m_cards_signature && !m_cards_sizer->IsEmpty())
        return;
    m_cards_signature = signature;

    m_cards_area->Freeze();
    m_cards_sizer->Clear(true /*delete windows*/);
    if (printers.empty()) {
        auto* empty = new wxStaticText(m_cards_area, wxID_ANY, _L("No printers synced yet."));
        empty->SetForegroundColour(wxColour(120, 120, 120));
        m_cards_sizer->Add(empty, 0, wxALL, FromDIP(8));
    } else {
        for (const auto& p : printers)
            m_cards_sizer->Add(make_printer_card(m_cards_area, p), 0, wxALL, FromDIP(6));
    }
    m_cards_area->FitInside();
    m_cards_area->Layout();
    m_cards_area->Thaw();
}

void PrintFarmPanel::refresh_jobs()
{
    if (!m_jobs)
        return;
    auto* client = PrintFarmManager::instance().client();
    m_jobs->DeleteAllItems();
    m_jobs_cache.clear();
    if (!client)
        return;
    std::vector<PfJob> jobs;
    PfResult res = client->get_jobs(jobs);
    if (!res.ok) {
        set_status(wxString::FromUTF8(res.error), true);
        return;
    }
    m_jobs_cache = jobs;
    long row = 0;
    for (const auto& j : jobs) {
        m_jobs->InsertItem(row, wxString::FromUTF8(j.name));
        m_jobs->SetItem(row, 1, wxString::FromUTF8(to_string(j.status)));
        m_jobs->SetItem(row, 2, wxString::FromUTF8(j.printer_name.empty() ? j.printer_id : j.printer_name));
        m_jobs->SetItem(row, 3, wxString::FromUTF8(j.submitter));
        m_jobs->SetItem(row, 4, wxString::FromUTF8(j.submitted_at));
        ++row;
    }
}

void PrintFarmPanel::refresh_all()
{
    if (!PrintFarmManager::instance().is_logged_in()) {
        update_auth_view();
        return;
    }

    auto& mgr = PrintFarmManager::instance();
    PfResult res = mgr.refresh_printers();
    if (!res.ok) {
        set_status(wxString::FromUTF8(res.error), true);
        rebuild_printer_cards({});
    } else {
        const auto printers = mgr.printers();
        rebuild_printer_cards(printers);
        set_status(wxString::Format(_L("%d printer(s) synced."), (int) printers.size()));
    }
    refresh_jobs();
}

void PrintFarmPanel::update_auth_view()
{
    const bool logged_in = PrintFarmManager::instance().is_logged_in();
    if (m_book)
        m_book->SetSelection(logged_in ? 1 : 0);
    m_logged_in_view = logged_in;
    if (logged_in)
        refresh_all();
}

void PrintFarmPanel::set_status(const wxString& text, bool error)
{
    if (!m_status)
        return;
    m_status->SetForegroundColour(error ? wxColour(200, 60, 60) : wxColour(120, 120, 120));
    m_status->SetLabel(text);
    m_status->GetParent()->Layout();
}

void PrintFarmPanel::on_timer(wxTimerEvent& /*evt*/)
{
    // Auth state can change while the tab is open (login overlay / logout), so
    // re-evaluate which page to show, then refresh if signed in.
    if (PrintFarmManager::instance().is_logged_in() != m_logged_in_view) {
        update_auth_view();
        return;
    }
    if (m_logged_in_view)
        refresh_all();
}

bool PrintFarmPanel::Show(bool show)
{
    const bool ret = wxPanel::Show(show);
    if (show) {
        update_auth_view();
        const int interval = PrintFarmManager::instance().config().refresh_interval_s;
        if (!m_timer.IsRunning())
            m_timer.Start(std::max(5, interval) * 1000);
    } else if (m_timer.IsRunning()) {
        m_timer.Stop();
    }
    return ret;
}

} // namespace GUI
} // namespace Slic3r
// <<< PRINTFARM
