#ifndef slic3r_GUI_PrintFarmPanel_hpp_
#define slic3r_GUI_PrintFarmPanel_hpp_

// >>> PRINTFARM
// Top-level "Print Farm" tab body (registered alongside Prepare/Preview/Device).
// Shows a richer, card-based printer-status grid and the backend job queue, with
// manual + automatic refresh. When the user is not signed in, a sign-in prompt is
// shown instead and routes to the existing in-window login overlay.
//
// This reuses PrintFarmManager (printers + status) and IPrintFarmClient (jobs)
// rather than adding any new transport; it is the panel form of the logic in
// PrintFarmJobsDialog.

#include <vector>

#include <wx/panel.h>
#include <wx/timer.h>

#include "slic3r/Utils/PrintFarm/PrintFarmClient.hpp"

class wxBoxSizer;
class wxWrapSizer;
class wxScrolledWindow;
class wxListCtrl;
class wxStaticText;
class wxSimplebook;

namespace Slic3r {
namespace GUI {

class PrintFarmPanel : public wxPanel
{
public:
    PrintFarmPanel(wxWindow* parent,
                   wxWindowID id       = wxID_ANY,
                   const wxPoint& pos  = wxDefaultPosition,
                   const wxSize& size  = wxDefaultSize,
                   long style          = wxTAB_TRAVERSAL);
    ~PrintFarmPanel() override;

    // Start/stop the auto-refresh timer with visibility, and refresh on show.
    bool Show(bool show = true) override;

    // Pull printers + jobs from the backend and rebuild the views.
    void refresh_all();

    // Re-evaluate login state and switch between the sign-in prompt and the
    // dashboard. Call after a Print Farm login/logout so the change is immediate
    // (the timer would otherwise pick it up on the next tick).
    void update_auth_view();

private:
    void build_ui();
    void rebuild_printer_cards(const std::vector<PfPrinter>& printers);
    void refresh_jobs();
    void set_status(const wxString& text, bool error = false);
    void on_timer(wxTimerEvent& evt);

    wxWindow* make_printer_card(wxWindow* parent, const PfPrinter& p);

    // Root switcher: page 0 = sign-in prompt, page 1 = dashboard.
    wxSimplebook*     m_book        = nullptr;

    // Dashboard widgets.
    wxScrolledWindow* m_cards_area  = nullptr;
    wxWrapSizer*      m_cards_sizer = nullptr;
    wxListCtrl*       m_jobs        = nullptr;
    wxStaticText*     m_status      = nullptr;

    wxTimer            m_timer;
    std::vector<PfJob> m_jobs_cache;      // row-aligned with m_jobs for selection
    std::string        m_cards_signature; // rebuild cards only when this changes
    bool               m_logged_in_view = false;
};

} // namespace GUI
} // namespace Slic3r
// <<< PRINTFARM

#endif // slic3r_GUI_PrintFarmPanel_hpp_
