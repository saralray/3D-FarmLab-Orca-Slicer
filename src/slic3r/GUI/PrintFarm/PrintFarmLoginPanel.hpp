#ifndef slic3r_GUI_PrintFarmLoginPanel_hpp_
#define slic3r_GUI_PrintFarmLoginPanel_hpp_

// >>> PRINTFARM
// In-window Print Farm login screen. Unlike a modal dialog, this is a full-window
// panel embedded in MainFrame and shown as an overlay at startup, so the login is
// part of the OrcaSlicer UI rather than a separate system popup. It blocks access
// to the app until the user signs in (or quits).

#include <functional>

#include <wx/wx.h>
#include <wx/panel.h>
#include <wx/checkbox.h>

class Button;
class TextInput;

namespace Slic3r {
namespace GUI {

class PrintFarmLoginPanel : public wxPanel
{
public:
    // on_success: invoked after a successful login (session + printers + token).
    // on_skip:    invoked when the user chooses to use the app without logging in.
    // on_quit:    invoked when the user chooses to quit the app.
    PrintFarmLoginPanel(wxWindow*             parent,
                        std::function<void()> on_success,
                        std::function<void()> on_skip,
                        std::function<void()> on_quit);

private:
    void on_sign_in(wxCommandEvent& evt);
    void set_busy(bool busy);
    void set_error(const wxString& msg);
    void save_credentials(const std::string& email, const std::string& pwd);
    void clear_credentials();
    bool load_credentials(std::string& email, std::string& pwd);

    TextInput*    m_server_url = nullptr; // editable so the backend can be re-pointed
    TextInput*    m_email      = nullptr;
    TextInput*    m_password   = nullptr;
    wxCheckBox*   m_remember   = nullptr;
    wxStaticText* m_error      = nullptr;
    Button*       m_sign_in    = nullptr;
    Button*       m_skip       = nullptr;
    Button*       m_quit       = nullptr;

    std::function<void()> m_on_success;
    std::function<void()> m_on_skip;
    std::function<void()> m_on_quit;
};

} // namespace GUI
} // namespace Slic3r
// <<< PRINTFARM

#endif // slic3r_GUI_PrintFarmLoginPanel_hpp_
