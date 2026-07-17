// >>> PRINTFARM
#include "PrintFarmPanel.hpp"

#include <algorithm>

#include <wx/sizer.h>
#include <wx/wrapsizer.h>
#include <wx/scrolwin.h>
#include <wx/listctrl.h>
#include <wx/stattext.h>
#include <wx/simplebook.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>

#include <boost/algorithm/string/predicate.hpp>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PrintFarm/PrintFarmManager.hpp"

#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/StaticBox.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/ProgressBar.hpp"
#include "slic3r/GUI/Widgets/RoundedRectangle.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"

namespace Slic3r {
namespace GUI {

enum { PF_PANEL_TIMER_ID = wxID_HIGHEST + 4301 };

// Status vocabulary is "idle | printing | offline"; anything else is unknown/offline.
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

// minutes -> "1h 20m" / "45m"
static wxString format_eta(int minutes)
{
    if (minutes <= 0)
        return wxEmptyString;
    const int h = minutes / 60;
    const int m = minutes % 60;
    if (h > 0)
        return wxString::Format(_L("%dh %dm left"), h, m);
    return wxString::Format(_L("%dm left"), m);
}

PrintFarmPanel::PrintFarmPanel(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style)
    : wxPanel(parent, id, pos, size, style)
    , m_timer(this, PF_PANEL_TIMER_ID)
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour(0xEE, 0xEE, 0xEE)));
    build_ui();
    Bind(wxEVT_TIMER, &PrintFarmPanel::on_timer, this, PF_PANEL_TIMER_ID);
    wxGetApp().UpdateDarkUIWin(this);
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
    login_page->SetBackgroundColour(GetBackgroundColour());
    {
        auto* col = new wxBoxSizer(wxVERTICAL);
        col->AddStretchSpacer(1);
        auto* title = new wxStaticText(login_page, wxID_ANY, _L("Sign in to Print Farm"));
        title->SetFont(Label::Head_20);
        title->SetForegroundColour(StateColor::darkModeColorFor(wxColour(50, 58, 61)));
        col->Add(title, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(8));

        auto* hint = new wxStaticText(login_page, wxID_ANY,
            _L("Connect to your 3D-FarmLab account to view printer status and the print queue."));
        hint->SetFont(Label::Body_13);
        hint->SetForegroundColour(wxColour(120, 120, 120));
        col->Add(hint, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(16));

        auto* sign_in = new Button(login_page, _L("Sign in"));
        sign_in->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
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
    dash->SetBackgroundColour(GetBackgroundColour());
    auto* dash_outer = new wxBoxSizer(wxVERTICAL);

    // White inner card, like the Multi-device page.
    auto* card = new wxPanel(dash, wxID_ANY);
    card->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    auto* dash_sizer = new wxBoxSizer(wxVERTICAL);

    // Header: title + refresh + sync status.
    auto* header = new wxBoxSizer(wxHORIZONTAL);
    auto* title = new wxStaticText(card, wxID_ANY, _L("Printers"));
    title->SetFont(Label::Head_16);
    title->SetForegroundColour(StateColor::darkModeColorFor(wxColour(38, 46, 48)));
    header->Add(title, 0, wxALIGN_CENTER_VERTICAL);
    header->AddStretchSpacer(1);
    auto* refresh = new Button(card, _L("Refresh"));
    refresh->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh_all(); });
    header->Add(refresh, 0, wxALIGN_CENTER_VERTICAL);
    dash_sizer->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, border);

    m_status = new wxStaticText(card, wxID_ANY, wxEmptyString);
    m_status->SetFont(Label::Body_12);
    m_status->SetForegroundColour(wxColour(120, 120, 120));
    dash_sizer->Add(m_status, 0, wxLEFT | wxRIGHT | wxTOP, border);

    // Printer status cards in a wrapping, scrollable area.
    m_cards_area = new wxScrolledWindow(card, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_cards_area->SetBackgroundColour(card->GetBackgroundColour());
    m_cards_area->SetScrollRate(0, FromDIP(10));
    m_cards_sizer = new wxWrapSizer(wxHORIZONTAL);
    m_cards_area->SetSizer(m_cards_sizer);
    dash_sizer->Add(m_cards_area, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, border);

    // Job queue.
    auto* jobs_title = new wxStaticText(card, wxID_ANY, _L("Queue"));
    jobs_title->SetFont(Label::Head_16);
    jobs_title->SetForegroundColour(StateColor::darkModeColorFor(wxColour(38, 46, 48)));
    dash_sizer->Add(jobs_title, 0, wxLEFT | wxRIGHT | wxTOP, border);

    m_jobs = new wxListCtrl(card, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(200)),
                            wxLC_REPORT | wxLC_SINGLE_SEL);
    m_jobs->SetFont(Label::Body_13);
    m_jobs->InsertColumn(0, _L("Filename"),  wxLIST_FORMAT_LEFT, FromDIP(240));
    m_jobs->InsertColumn(1, _L("Status"),    wxLIST_FORMAT_LEFT, FromDIP(110));
    m_jobs->InsertColumn(2, _L("Submitter"), wxLIST_FORMAT_LEFT, FromDIP(140));
    m_jobs->InsertColumn(3, _L("Submitted"), wxLIST_FORMAT_LEFT, FromDIP(150));
    m_jobs->InsertColumn(4, _L("Priority"),  wxLIST_FORMAT_LEFT, FromDIP(80));
    m_jobs->Bind(wxEVT_LIST_ITEM_SELECTED,   [this](wxListEvent&) { update_job_buttons(); });
    m_jobs->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) { update_job_buttons(); });
    dash_sizer->Add(m_jobs, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, border);

    auto* job_buttons = new wxBoxSizer(wxHORIZONTAL);
    m_send_btn = new Button(card, _L("Send to Prepare"));
    m_send_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
    m_send_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_send_to_prepare(); });
    m_send_btn->Enable(false);
    job_buttons->Add(m_send_btn, 0, wxRIGHT, FromDIP(8));

    m_cancel_btn = new Button(card, _L("Cancel job"));
    m_cancel_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    m_cancel_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_cancel_job(); });
    m_cancel_btn->Enable(false);
    job_buttons->Add(m_cancel_btn, 0);
    dash_sizer->Add(job_buttons, 0, wxALL, border);

    card->SetSizer(dash_sizer);
    dash_outer->Add(card, 1, wxEXPAND | wxALL, FromDIP(10));
    dash->SetSizer(dash_outer);
    m_book->AddPage(dash, wxEmptyString);
}

wxWindow* PrintFarmPanel::make_printer_card(wxWindow* parent, const PfPrinter& p)
{
    const bool printing = (p.status == "printing") || p.progress > 0.0;

    auto* card = new StaticBox(parent);
    card->SetCornerRadius(FromDIP(8));
    card->SetBorderWidth(0);
    card->SetBackgroundColorNormal(StateColor::darkModeColorFor(wxColour(0xF6, 0xF6, 0xF6)));
    card->SetMinSize(wxSize(FromDIP(260), -1));

    auto* col = new wxBoxSizer(wxVERTICAL);
    const int pad = FromDIP(12);

    // Name (bold).
    auto* name = new wxStaticText(card, wxID_ANY, wxString::FromUTF8(p.name),
                                  wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    name->SetFont(Label::Head_15);
    name->SetForegroundColour(StateColor::darkModeColorFor(wxColour(38, 46, 48)));
    col->Add(name, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, pad);

    // Model / profile.
    const std::string model = p.model.empty() ? p.profile : p.model;
    auto* sub = new wxStaticText(card, wxID_ANY, wxString::FromUTF8(model),
                                 wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    sub->SetFont(Label::Body_12);
    sub->SetForegroundColour(wxColour(120, 120, 120));
    col->Add(sub, 0, wxLEFT | wxRIGHT | wxEXPAND, pad);

    // Status badge: coloured circle dot + label.
    auto* status_row = new wxBoxSizer(wxHORIZONTAL);
    const int dot = FromDIP(10);
    const wxColour card_fill = StateColor::darkModeColorFor(wxColour(0xF6, 0xF6, 0xF6));
    auto* dot_win = new RoundedRectangle(card, status_colour(p.status), wxDefaultPosition,
                                         wxSize(dot, dot), dot / 2.0);
    dot_win->SetMinSize(wxSize(dot, dot));
    dot_win->SetBackgroundColour(card_fill); // match the card so the dot's corners blend in
    status_row->Add(dot_win, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    auto* status_txt = new wxStaticText(card, wxID_ANY, status_label(p.status));
    status_txt->SetFont(Label::Body_13);
    status_txt->SetForegroundColour(status_colour(p.status));
    status_row->Add(status_txt, 0, wxALIGN_CENTER_VERTICAL);
    col->Add(status_row, 0, wxLEFT | wxRIGHT | wxTOP, pad);

    // Live print progress (only while printing).
    if (printing) {
        int pct = static_cast<int>(p.progress + 0.5);
        pct = std::max(0, std::min(100, pct));
        auto* bar = new ProgressBar(card, wxID_ANY, 100);
        bar->ShowNumber(true);
        bar->SetProgress(pct);
        bar->SetMinSize(wxSize(-1, FromDIP(14)));
        col->Add(bar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);

        wxString job_line = wxString::FromUTF8(p.current_job);
        const wxString eta = format_eta(p.time_remaining_min);
        if (!eta.empty())
            job_line = job_line.empty() ? eta : (job_line + "  ·  " + eta);
        if (!job_line.empty()) {
            auto* jl = new wxStaticText(card, wxID_ANY, job_line,
                                        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
            jl->SetFont(Label::Body_11);
            jl->SetForegroundColour(wxColour(120, 120, 120));
            col->Add(jl, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, pad);
        }
    }

    // Error message, if any.
    if (!p.error_message.empty()) {
        auto* err = new wxStaticText(card, wxID_ANY, wxString::FromUTF8(p.error_message),
                                     wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
        err->SetFont(Label::Body_11);
        err->SetForegroundColour(wxColour(208, 27, 27));
        col->Add(err, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, pad);
    }

    col->AddSpacer(pad);
    card->SetSizer(col);
    return card;
}

void PrintFarmPanel::rebuild_printer_cards(const std::vector<PfPrinter>& printers)
{
    // Only rebuild when the printer set/status/progress actually changed, to avoid
    // flicker and keep scrolling stable across the refresh cadence.
    std::string signature;
    for (const auto& p : printers)
        signature += p.id + '|' + p.status + '|' + std::to_string((int) p.progress) + '|'
                   + p.current_job + '|' + p.error_message + '\n';
    if (signature == m_cards_signature && !m_cards_sizer->IsEmpty())
        return;
    m_cards_signature = signature;

    m_cards_area->Freeze();
    m_cards_sizer->Clear(true /*delete windows*/);
    if (printers.empty()) {
        auto* empty = new wxStaticText(m_cards_area, wxID_ANY, _L("No printers synced yet."));
        empty->SetFont(Label::Body_13);
        empty->SetForegroundColour(wxColour(120, 120, 120));
        m_cards_sizer->Add(empty, 0, wxALL, FromDIP(8));
    } else {
        for (const auto& p : printers)
            m_cards_sizer->Add(make_printer_card(m_cards_area, p), 0, wxALL, FromDIP(6));
    }
    m_cards_area->FitInside();
    m_cards_area->Layout();
    wxGetApp().UpdateDarkUIWin(m_cards_area);
    m_cards_area->Thaw();
}

void PrintFarmPanel::refresh_jobs()
{
    if (!m_jobs)
        return;
    auto* client = PrintFarmManager::instance().client();
    m_jobs->DeleteAllItems();
    m_jobs_cache.clear();
    if (!client) {
        update_job_buttons();
        return;
    }
    std::vector<PfJob> jobs;
    PfResult res = client->get_jobs(jobs);
    if (!res.ok) {
        set_status(wxString::FromUTF8(res.error), true);
        update_job_buttons();
        return;
    }
    m_jobs_cache = jobs;
    long row = 0;
    for (const auto& j : jobs) {
        m_jobs->InsertItem(row, wxString::FromUTF8(j.name));
        m_jobs->SetItem(row, 1, wxString::FromUTF8(to_string(j.status)));
        m_jobs->SetItem(row, 2, wxString::FromUTF8(j.submitter));
        m_jobs->SetItem(row, 3, wxString::FromUTF8(j.submitted_at));
        m_jobs->SetItem(row, 4, j.priority > 0 ? wxString::Format("%d", j.priority) : wxString());
        ++row;
    }
    update_job_buttons();
}

void PrintFarmPanel::update_job_buttons()
{
    if (!m_jobs || !m_send_btn || !m_cancel_btn)
        return;
    const long sel = m_jobs->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    const bool has_sel = sel >= 0 && sel < static_cast<long>(m_jobs_cache.size());
    m_cancel_btn->Enable(has_sel);
    const bool can_send = has_sel && m_jobs_cache[sel].has_file;
    m_send_btn->Enable(can_send);
    m_send_btn->SetToolTip(has_sel && !m_jobs_cache[sel].has_file
                               ? _L("This job has no downloadable file on the farm.")
                               : wxString());
}

void PrintFarmPanel::on_cancel_job()
{
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
}

void PrintFarmPanel::on_send_to_prepare()
{
    const long sel = m_jobs->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || sel >= static_cast<long>(m_jobs_cache.size()))
        return;
    const PfJob job = m_jobs_cache[sel];
    if (!job.has_file) {
        set_status(_L("This job has no downloadable file on the farm."), true);
        return;
    }
    auto* client = PrintFarmManager::instance().client();
    if (!client)
        return;

    // Temp path preserving the original extension (.3mf / .stl / .obj).
    wxString base = wxString::FromUTF8(job.name);
    if (base.empty())
        base = wxString::FromUTF8(job.id) + ".3mf";
    wxFileName fn(wxStandardPaths::Get().GetTempDir(), base);
    const wxString dest = fn.GetFullPath();

    wxProgressDialog progress(_L("Downloading from Farm"), _L("Downloading the model file..."),
                              100, this, wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH);
    PfResult res = client->download_job(job.id, into_u8(dest),
                                        [&progress](int pct) { progress.Update(pct); });
    progress.Update(100);

    if (!res.ok) {
        set_status(wxString::FromUTF8(res.error), true);
        wxMessageBox(wxString::FromUTF8(res.error), _L("Send to Prepare"), wxOK | wxICON_ERROR, this);
        return;
    }

    // Load into the Prepare tab. A .3mf opens as a project (auto-switches to Prepare);
    // a bare model is imported onto the current bed and we switch tabs ourselves.
    Plater* plater = wxGetApp().plater();
    if (!plater)
        return;
    if (boost::iends_with(into_u8(dest), ".3mf")) {
        plater->load_project(dest);
    } else {
        wxArrayString files;
        files.Add(dest);
        plater->load_files(files);
        if (wxGetApp().mainframe)
            wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
    }
    set_status(wxString::Format(_L("Opened \"%s\" in Prepare."), base));
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
    m_status->SetForegroundColour(error ? wxColour(208, 27, 27) : wxColour(120, 120, 120));
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

void PrintFarmPanel::msw_rescale()
{
    // Recreate the cards at the new DPI, then relayout.
    m_cards_signature.clear();
    if (m_logged_in_view)
        refresh_all();
    Layout();
    Refresh();
}

void PrintFarmPanel::on_sys_color_changed()
{
    wxGetApp().UpdateDarkUIWin(this);
    m_cards_signature.clear();
    if (m_logged_in_view)
        rebuild_printer_cards(PrintFarmManager::instance().printers());
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
// <<< PRINTFARM
