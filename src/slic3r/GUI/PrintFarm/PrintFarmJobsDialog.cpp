// >>> PRINTFARM
#include "PrintFarmJobsDialog.hpp"

#include <algorithm>
#include <vector>

#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/progdlg.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PrintFarm/PrintFarmManager.hpp"

namespace Slic3r {
namespace GUI {

enum { PF_TIMER_ID = wxID_HIGHEST + 4201 };

PrintFarmJobsDialog::PrintFarmJobsDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Print Farm"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_timer(this, PF_TIMER_ID)
{
    const int border = FromDIP(10);
    auto* root = new wxBoxSizer(wxVERTICAL);

    // Printers
    root->Add(new wxStaticText(this, wxID_ANY, _L("Printers")), 0, wxLEFT | wxTOP, border);
    m_printers = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(560), FromDIP(160)),
                                wxLC_REPORT | wxLC_SINGLE_SEL);
    m_printers->InsertColumn(0, _L("Name"),   wxLIST_FORMAT_LEFT, FromDIP(180));
    m_printers->InsertColumn(1, _L("Model"),  wxLIST_FORMAT_LEFT, FromDIP(150));
    m_printers->InsertColumn(2, _L("Status"), wxLIST_FORMAT_LEFT, FromDIP(110));
    m_printers->InsertColumn(3, _L("Note"),   wxLIST_FORMAT_LEFT, FromDIP(110));
    root->Add(m_printers, 1, wxEXPAND | wxLEFT | wxRIGHT, border);

    // Jobs
    root->Add(new wxStaticText(this, wxID_ANY, _L("Jobs")), 0, wxLEFT | wxTOP, border);
    m_jobs = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(560), FromDIP(160)),
                            wxLC_REPORT | wxLC_SINGLE_SEL);
    m_jobs->InsertColumn(0, _L("Job"),       wxLIST_FORMAT_LEFT, FromDIP(220));
    m_jobs->InsertColumn(1, _L("Status"),    wxLIST_FORMAT_LEFT, FromDIP(110));
    m_jobs->InsertColumn(2, _L("Submitter"), wxLIST_FORMAT_LEFT, FromDIP(120));
    m_jobs->InsertColumn(3, _L("Submitted"), wxLIST_FORMAT_LEFT, FromDIP(110));
    root->Add(m_jobs, 1, wxEXPAND | wxLEFT | wxRIGHT, border);

    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    root->Add(m_status, 0, wxALL, border);

    root->Add(new wxStaticLine(this, wxID_ANY), 0, wxEXPAND | wxLEFT | wxRIGHT, border);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* refresh = new wxButton(this, wxID_REFRESH, _L("Refresh"));
    auto* upload  = new wxButton(this, wxID_ANY, _L("Upload file to farm…"));
    auto* cancel  = new wxButton(this, wxID_ANY, _L("Cancel job"));
    auto* close   = new wxButton(this, wxID_CANCEL, _L("Close"));
    buttons->Add(refresh, 0, wxRIGHT, FromDIP(8));
    buttons->Add(upload, 0, wxRIGHT, FromDIP(8));
    buttons->Add(cancel, 0, wxRIGHT, FromDIP(8));
    buttons->AddStretchSpacer(1);
    buttons->Add(close, 0);
    root->Add(buttons, 0, wxEXPAND | wxALL, border);

    SetSizerAndFit(root);
    SetMinSize(GetSize());
    CentreOnParent();

    refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh_printers(); refresh_jobs(); });
    upload->Bind(wxEVT_BUTTON, &PrintFarmJobsDialog::on_upload, this);
    cancel->Bind(wxEVT_BUTTON, &PrintFarmJobsDialog::on_cancel_job, this);
    Bind(wxEVT_TIMER, &PrintFarmJobsDialog::on_timer, this, PF_TIMER_ID);

    refresh_printers();
    refresh_jobs();

    const int interval = PrintFarmManager::instance().config().refresh_interval_s;
    m_timer.Start(std::max(5, interval) * 1000);
}

PrintFarmJobsDialog::~PrintFarmJobsDialog()
{
    if (m_timer.IsRunning())
        m_timer.Stop();
}

void PrintFarmJobsDialog::set_status(const wxString& text, bool error)
{
    m_status->SetForegroundColour(error ? wxColour(200, 60, 60) : wxColour(120, 120, 120));
    m_status->SetLabel(text);
    Layout();
}

void PrintFarmJobsDialog::refresh_printers()
{
    auto& mgr = PrintFarmManager::instance();
    PfResult res = mgr.refresh_printers();
    m_printers->DeleteAllItems();
    if (!res.ok) {
        set_status(wxString::FromUTF8(res.error), true);
        return;
    }
    const auto printers = mgr.printers();
    long row = 0;
    for (const auto& p : printers) {
        m_printers->InsertItem(row, wxString::FromUTF8(p.name));
        m_printers->SetItem(row, 1, wxString::FromUTF8(p.model.empty() ? p.profile : p.model));
        m_printers->SetItem(row, 2, wxString::FromUTF8(p.status));
        m_printers->SetItem(row, 3, wxString::FromUTF8(p.error_message));
        ++row;
    }
    set_status(wxString::Format(_L("%d printer(s) synced."), (int) printers.size()));
}

void PrintFarmJobsDialog::refresh_jobs()
{
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
        m_jobs->SetItem(row, 2, wxString::FromUTF8(j.submitter));
        m_jobs->SetItem(row, 3, wxString::FromUTF8(j.submitted_at));
        ++row;
    }
}

void PrintFarmJobsDialog::on_cancel_job(wxCommandEvent& /*evt*/)
{
    const long sel = m_jobs->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (sel < 0 || sel >= static_cast<long>(m_jobs_cache.size())) {
        set_status(_L("Select a job to cancel."), true);
        return;
    }
    const PfJob& job = m_jobs_cache[sel];

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

void PrintFarmJobsDialog::on_timer(wxTimerEvent& /*evt*/)
{
    refresh_printers();
    refresh_jobs();
}

void PrintFarmJobsDialog::on_upload(wxCommandEvent& /*evt*/)
{
    wxFileDialog fd(this, _L("Select a sliced file to upload"), wxEmptyString, wxEmptyString,
                    "Sliced files (*.3mf;*.gcode;*.gcode.3mf)|*.3mf;*.gcode;*.gcode.3mf|All files|*.*",
                    wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (fd.ShowModal() != wxID_OK)
        return;
    upload_to_farm(this, into_u8(fd.GetPath()));
    refresh_jobs();
}

bool PrintFarmJobsDialog::upload_to_farm(wxWindow* parent, const std::string& sliced_file_path)
{
    auto& mgr = PrintFarmManager::instance();

    // Ensure the printer list is available to pick from.
    if (mgr.printers().empty())
        mgr.refresh_printers();
    auto printers = mgr.printers();

    // Only printers whose profile actually supports slicer uploads can be targets.
    std::vector<PfPrinter> targets;
    wxArrayString labels;
    for (const auto& p : printers) {
        if (!p.can_upload)
            continue;
        targets.push_back(p);
        labels.Add(wxString::FromUTF8(p.name + "  (" + (p.model.empty() ? p.profile : p.model) + ")"));
    }
    if (targets.empty()) {
        wxMessageBox(_L("No farm printers support slicer uploads right now."),
                     _L("Upload to Farm"), wxOK | wxICON_INFORMATION, parent);
        return false;
    }

    // If a target was already chosen (e.g. in the Prepare printer dropdown), upload
    // to it directly without prompting; otherwise let the user pick one.
    int sel = -1;
    const std::string preselected = mgr.upload_target();
    if (!preselected.empty()) {
        for (size_t i = 0; i < targets.size(); ++i)
            if (targets[i].id == preselected) { sel = (int) i; break; }
    }
    if (sel < 0) {
        sel = wxGetSingleChoiceIndex(_L("Select a printer to print on:"),
                                     _L("Upload to Farm"), labels, parent);
        if (sel < 0)
            return false;
    }

    wxProgressDialog progress(_L("Uploading to Farm"), _L("Uploading sliced file…"), 100, parent,
                              wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_SMOOTH);
    PfJob job;
    PfResult res = mgr.client()->upload_job(
        targets[sel].id, sliced_file_path,
        [&progress](int pct) { progress.Update(pct); },
        job);
    progress.Update(100);

    if (res.ok) {
        wxMessageBox(_L("Upload accepted. The farm will start the print."),
                     _L("Upload to Farm"), wxOK | wxICON_INFORMATION, parent);
        return true;
    }
    wxMessageBox(wxString::FromUTF8(res.error), _L("Upload failed"), wxOK | wxICON_ERROR, parent);
    return false;
}

void PrintFarmJobsDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
// <<< PRINTFARM
