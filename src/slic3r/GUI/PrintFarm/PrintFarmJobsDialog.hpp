#ifndef slic3r_GUI_PrintFarmJobsDialog_hpp_
#define slic3r_GUI_PrintFarmJobsDialog_hpp_

// >>> PRINTFARM
// Print Farm dashboard: synchronized printers (read-only from the backend) and the
// job queue, with manual + automatic refresh, and an "Upload to Farm" action that
// pushes a sliced file to a selected printer via the abstraction layer.

#include <vector>

#include <wx/wx.h>
#include <wx/listctrl.h>
#include <wx/timer.h>

#include "slic3r/Utils/PrintFarm/PrintFarmClient.hpp"

#include "slic3r/GUI/GUI_Utils.hpp"

namespace Slic3r {
namespace GUI {

class PrintFarmJobsDialog : public DPIDialog
{
public:
    explicit PrintFarmJobsDialog(wxWindow* parent);
    ~PrintFarmJobsDialog() override;

    // Upload an already-sliced file to a farm printer chosen by the user.
    // Returns true if the upload was accepted by the backend.
    static bool upload_to_farm(wxWindow* parent, const std::string& sliced_file_path);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void refresh_printers();
    void refresh_jobs();
    void on_upload(wxCommandEvent& evt);
    void on_cancel_job(wxCommandEvent& evt);
    void on_timer(wxTimerEvent& evt);
    void set_status(const wxString& text, bool error = false);

    wxListCtrl*        m_printers = nullptr;
    wxListCtrl*        m_jobs     = nullptr;
    wxStaticText*      m_status   = nullptr;
    wxTimer            m_timer;
    std::vector<PfJob> m_jobs_cache; // row-aligned with m_jobs for selection lookup
};

} // namespace GUI
} // namespace Slic3r
// <<< PRINTFARM

#endif // slic3r_GUI_PrintFarmJobsDialog_hpp_
