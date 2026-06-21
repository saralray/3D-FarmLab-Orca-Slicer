#ifndef slic3r_GUI_PrintFarmLoginDialog_hpp_
#define slic3r_GUI_PrintFarmLoginDialog_hpp_

// >>> PRINTFARM
// Modal login gate shown at startup when a Print Farm URL is configured.
// Collects email + password and authenticates against the backend session API.
// On success the in-memory session lives in PrintFarmManager; nothing is persisted.

#include <wx/wx.h>

#include "slic3r/GUI/GUI_Utils.hpp"

namespace Slic3r {
namespace GUI {

class PrintFarmLoginDialog : public DPIDialog
{
public:
    explicit PrintFarmLoginDialog(wxWindow* parent);
    ~PrintFarmLoginDialog() override = default;

    // Convenience: run the gate. Returns true if the user is authenticated and the
    // app should proceed, false if the user chose to quit. No-op (returns true) when
    // the farm is not configured.
    static bool run_login_gate(wxWindow* parent);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void on_sign_in(wxCommandEvent& evt);
    void set_busy(bool busy);

    wxTextCtrl*   m_email   = nullptr;
    wxTextCtrl*   m_password = nullptr;
    wxStaticText* m_error   = nullptr;
    wxButton*     m_sign_in = nullptr;
    wxButton*     m_quit    = nullptr;
};

} // namespace GUI
} // namespace Slic3r
// <<< PRINTFARM

#endif // slic3r_GUI_PrintFarmLoginDialog_hpp_
