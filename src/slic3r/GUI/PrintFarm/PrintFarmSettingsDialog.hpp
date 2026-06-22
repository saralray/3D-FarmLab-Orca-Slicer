#ifndef slic3r_GUI_PrintFarmSettingsDialog_hpp_
#define slic3r_GUI_PrintFarmSettingsDialog_hpp_

// >>> PRINTFARM
// Print Farm settings: URL, slicer-proxy URL, auth mode, Print API Key (+remember),
// refresh interval, allow self-signed TLS. Persists non-secret config via AppConfig;
// the API key is stored only when "remember" is ticked. No session tokens are stored.

#include <wx/wx.h>

#include "slic3r/GUI/GUI_Utils.hpp"

namespace Slic3r {
namespace GUI {

class PrintFarmSettingsDialog : public DPIDialog
{
public:
    explicit PrintFarmSettingsDialog(wxWindow* parent);
    ~PrintFarmSettingsDialog() override = default;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void load_from_manager();
    void on_save(wxCommandEvent& evt);
    void on_test(wxCommandEvent& evt);

    wxTextCtrl* m_url            = nullptr;
    wxTextCtrl* m_proxy_url      = nullptr;
    wxChoice*   m_auth_mode      = nullptr;
    wxTextCtrl* m_api_key        = nullptr;
    wxCheckBox* m_remember_key   = nullptr;
    wxSpinCtrl* m_refresh        = nullptr;
    wxCheckBox* m_insecure_tls   = nullptr;
    wxStaticText* m_status       = nullptr;
};

} // namespace GUI
} // namespace Slic3r
// <<< PRINTFARM

#endif // slic3r_GUI_PrintFarmSettingsDialog_hpp_
