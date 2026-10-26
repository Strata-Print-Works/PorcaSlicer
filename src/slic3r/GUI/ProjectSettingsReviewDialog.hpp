#pragma once

#include "MsgDialog.hpp"

#include <wx/gdicmn.h>
#include <wx/window.h>

class Button;
class Label;
class wxTextCtrl;

namespace Slic3r {
struct ProjectSettingsReview;

namespace GUI {

// Asks how to import plates whose saved print settings can't all be restored.
// ShowModal() returns wxID_YES to use the available settings, wxID_NO to import
// only the models, or wxID_CANCEL.
class ProjectSettingsReviewDialog : public MsgDialog
{
public:
    ProjectSettingsReviewDialog(wxWindow* parent, const ProjectSettingsReview& review);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void apply_sizes();

    Label*      m_summary{nullptr};
    Button*     m_toggle{nullptr};
    wxTextCtrl* m_details{nullptr};
};

} // namespace GUI
} // namespace Slic3r
