#include "ProjectSettingsReviewDialog.hpp"

#include "DesignTokens.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Typography.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/ProjectPlateImport.hpp"

#include <wx/colour.h>
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/textctrl.h>

namespace Slic3r {
namespace GUI {

ProjectSettingsReviewDialog::ProjectSettingsReviewDialog(wxWindow* parent, const ProjectSettingsReview& review)
    : MsgDialog(parent, _L("Import project"), wxEmptyString, wxYES | wxNO | wxCANCEL | wxICON_WARNING)
{
    SetButtonLabel(wxID_YES, _L("Import with available settings"));
    SetButtonLabel(wxID_NO, _L("Import models only"));

    m_summary = new Label(this, DesignSystem::font(DesignSystem::TypeStyle::Body),
        _L("Some of this project's print settings aren't available in Porca. You can import its plates with the settings "
           "that are, or import only its models and use your current printer and print settings."), LB_AUTO_WRAP);
    content_sizer->Add(m_summary, 0, wxEXPAND | wxBOTTOM, FromDIP(DesignSystem::space_12));

    wxString details;
    if (!review.unavailable.empty()) {
        details += _L("Not available in Porca:");
        for (const auto& name : review.unavailable) details += "\n    " + from_u8(name);
    }
    if (!review.changed.empty()) {
        if (!details.empty()) details += "\n\n";
        details += _L("Adjusted settings:");
        for (const auto& change : review.changed)
            // TRN %1% is a print setting, %2% its value in the project and %3% the value used instead.
            details += "\n    " + format_wxstr(_L("%1%: “%2%” becomes “%3%”"),
                                               change.label, change.file_value, change.used_value);
    }
    m_toggle = new Button(this, _L("Show details"));
    m_toggle->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    content_sizer->Add(m_toggle, 0, wxBOTTOM, FromDIP(DesignSystem::space_8));
    m_details = new wxTextCtrl(this, wxID_ANY, details, wxDefaultPosition, wxDefaultSize,
                               wxTE_MULTILINE | wxTE_READONLY | wxBORDER_NONE);
    m_details->SetFont(DesignSystem::font(DesignSystem::TypeStyle::Body));
    m_details->Hide();
    content_sizer->Add(m_details, 0, wxEXPAND | wxBOTTOM, FromDIP(DesignSystem::space_12));
    m_toggle->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_details->Show(!m_details->IsShown());
        m_toggle->SetLabel(m_details->IsShown() ? _L("Hide details") : _L("Show details"));
        GetSizer()->SetSizeHints(this);
        Layout();
    });

    apply_sizes();
    finalize();
    // After the dialog's theme pass, which would remap these palette colours.
    const bool dark = wxGetApp().dark_mode();
    m_details->SetBackgroundColour(wxColour(DesignSystem::color_hex(DesignSystem::ColorRole::Bg, dark)));
    m_details->SetForegroundColour(wxColour(DesignSystem::color_hex(DesignSystem::ColorRole::Ink, dark)));
}

void ProjectSettingsReviewDialog::apply_sizes()
{
    const int width = FromDIP(420);
    m_summary->SetMinSize(wxSize(width, -1));
    m_summary->Wrap(width);
    m_details->SetMinSize(wxSize(width, FromDIP(160)));
}

void ProjectSettingsReviewDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    MsgDialog::on_dpi_changed(suggested_rect);
    m_toggle->Rescale();
    apply_sizes();
    GetSizer()->SetSizeHints(this);
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
