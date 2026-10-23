#include "Branding.hpp"
#include "DesignTokens.hpp"
#include "Typography.hpp"
///////////////////////////////////////////////////////////////////////////
// C++ code generated with wxFormBuilder (version 3.10.0-4761b0c)
// http://www.wxformbuilder.org/
//
// PLEASE DO *NOT* EDIT THIS FILE!
///////////////////////////////////////////////////////////////////////////
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PlatePrintSetup.hpp"
#include "ParamsPanel.hpp"
#include "Tab.hpp"
#include "MainFrame.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"

#include "Widgets/Label.hpp"
#include "Widgets/SwitchButton.hpp"
#include "Widgets/TabCtrl.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/StateColor.hpp"
#include "GUI_Factories.hpp"
#include "I18N.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Utils.hpp"
#include <string>
#include <map>
#include <wx/string.h>
#include "slic3r/GUI/GUI_Utils.hpp"
#include <wx/gdicmn.h>
#include <wx/toplevel.h>
#include <wx/sizer.h>
#include <wx/panel.h>
#include <wx/menu.h>
#include <wx/colour.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include "slic3r/GUI/Widgets/CheckBox.hpp"
#include <wx/tglbtn.h>
#include <wx/event.h>
#include <utility>
#include "slic3r/GUI/wxExtensions.hpp"
#include <wx/anybutton.h>
#include <wx/timer.h>
#include <cstddef>
#include <boost/log/trivial.hpp>
#include <wx/wupdlock.h>
#include <vector>


namespace Slic3r {
namespace GUI {

namespace
{
int mode_to_selection(ConfigOptionMode mode)
{
    return mode == comExpert ? 2 :
           mode == comAdvanced ? 1 :
           0;
}

// Programmatic scope changes already call set_active_tab explicitly.
void set_scope_selection(TabCtrl* tabs, int selection)
{
    wxEventBlocker block(tabs, wxEVT_TAB_SEL_CHANGED);
    tabs->SelectItem(selection);
}

void set_sidebar_action_icon(ScalableButton* button, const char* name)
{
    if (!button) return;
    const bool dark = wxGetApp().dark_mode();
    const wxBitmap source = create_scaled_bitmap(resources_dir() + "/design-system/icons/" + name, button, 16);
    const auto tinted = [&](DesignSystem::ColorRole role) {
        wxImage image = source.ConvertToImage();
        const wxColour tint(DesignSystem::color_hex(role, dark));
        image.SetRGB(wxRect(0, 0, image.GetWidth(), image.GetHeight()), tint.Red(), tint.Green(), tint.Blue());
        wxBitmap result(image);
        result.SetScaleFactor(source.GetScaleFactor());
        return result;
    };
    // Clear the cached legacy asset names too, so a subsequent native rescale
    // cannot restore the old glyph after the compact sidebar's palette is applied.
    ScalableBitmap normal;
    normal.bmp() = tinted(DesignSystem::ColorRole::InkMuted);
    button->SetBitmap_(normal);
    button->SetBitmapCurrent(normal.bmp());
    button->SetBitmapPressed(normal.bmp());
    button->SetBitmapFocus(normal.bmp());
    ScalableBitmap disabled;
    disabled.bmp() = tinted(DesignSystem::ColorRole::InkDisabled);
    button->SetBitmapDisabled_(disabled);
}
}


TipsDialog::TipsDialog(wxWindow *parent, const wxString &title, const wxString &description, std::string app_key, long style,std::map<wxStandardID,wxString> option_map)
    : DPIDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX),
    m_app_key(app_key)
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);

    m_top_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_top_line->SetBackgroundColour(wxColour(166, 169, 170));

    m_sizer_main->Add(m_top_line, 0, wxEXPAND, 0);

    m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(20));

    m_msg = new wxStaticText(this, wxID_ANY, description, wxDefaultPosition, wxDefaultSize, 0);
    m_msg->Wrap(-1);
    m_msg->SetFont(::Label::Body_13);
    m_msg->SetForegroundColour(wxColour(107, 107, 107));
    m_msg->SetBackgroundColour(wxColour(255, 255, 255));

    m_sizer_main->Add(m_msg, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(40));

    m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(5));

    wxBoxSizer *m_sizer_bottom = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *m_sizer_left   = new wxBoxSizer(wxHORIZONTAL);

    auto dont_show_again = create_item_checkbox(_L("Don't show again"), this, _L("Don't show again"), "do_not_show_tips");
    m_sizer_left->Add(dont_show_again, 1, wxALL, FromDIP(5));

    m_sizer_bottom->Add(m_sizer_left, 1, wxEXPAND, FromDIP(5));

    wxBoxSizer *m_sizer_right = new wxBoxSizer(wxHORIZONTAL);

    if (style & wxOK) {
        wxString str = _L("OK");
        if (auto iter = option_map.find(wxID_OK); iter != option_map.end())
            str = iter->second;
        Button* btn = add_button(wxID_OK, str, true);
        m_sizer_right->Add(btn, 0, wxALL, FromDIP(5));
    }
    if (style & wxYES) {
        wxString str = _L("Yes");
        if (auto iter = option_map.find(wxID_YES); iter != option_map.end())
            str = iter->second;
        Button *btn = add_button(wxID_YES, str, true);
        m_sizer_right->Add(btn, 0, wxALL, FromDIP(5));
    }
    if (style & wxNO) {
        wxString str = _L("No");
        if (auto iter = option_map.find(wxID_NO); iter != option_map.end())
            str = iter->second;
        Button *btn = add_button(wxID_NO, str, false);
        m_sizer_right->Add(btn, 0, wxALL, FromDIP(5));
    }
    if (style & wxCANCEL) {
        wxString str = _L("Cancel");
        if (auto iter = option_map.find(wxID_CANCEL); iter != option_map.end())
            str = iter->second;
        Button *btn = add_button(wxID_CANCEL, str, false);
        m_sizer_right->Add(btn, 0, wxALL, FromDIP(5));
    }

    m_sizer_bottom->Add(m_sizer_right, 0, wxEXPAND, FromDIP(5));
    m_sizer_main->Add(m_sizer_bottom, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(40));
    m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, FromDIP(20));

    SetSizer(m_sizer_main);
    Layout();
    Fit();
    Centre(wxBOTH);

    wxGetApp().UpdateDlgDarkUI(this);
}

wxBoxSizer *TipsDialog::create_item_checkbox(wxString title, wxWindow *parent, wxString tooltip, std::string param)
{
    wxBoxSizer *m_sizer_checkbox = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 5);

    auto checkbox = new ::CheckBox(parent);
    m_sizer_checkbox->Add(checkbox, 0, wxALIGN_CENTER, 0);
    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 8);

    auto checkbox_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxSize(-1, -1), 0);
    checkbox_title->SetForegroundColour(wxColour(144, 144, 144));
    checkbox_title->SetFont(::Label::Body_13);
    checkbox_title->Wrap(-1);
    m_sizer_checkbox->Add(checkbox_title, 0, wxALIGN_CENTER | wxALL, 3);

    m_show_again = wxGetApp().app_config->has(param);
    checkbox->SetValue(m_show_again);

    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, param](wxCommandEvent &e) {
        m_show_again = m_show_again ? false : true;
        e.Skip();
    });

    return m_sizer_checkbox;
}

Button *TipsDialog::add_button(wxWindowID btn_id, const wxString &label, bool set_focus /*= false*/)
{
    Button* btn = new Button(this, label, "", 0, 0, btn_id);

    if (btn_id == wxID_OK || btn_id == wxID_YES)
        btn->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);

    if (btn_id == wxID_CANCEL || btn_id == wxID_NO)
        btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);

    if (set_focus)
        btn->SetFocus();

    btn->Bind(wxEVT_BUTTON, [this, btn_id](wxCommandEvent &) {
        if (m_show_again) {
            if (!m_app_key.empty()) {
                if (btn_id == wxID_OK || btn_id == wxID_YES) {
                    wxGetApp().app_config->set_bool(m_app_key, true);
                }

                if (btn_id == wxID_NO) {
                    wxGetApp().app_config->set_bool(m_app_key, false);
                }
            }
        }
        EndModal(btn_id);
    });
    return btn;
}

void TipsDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    if (m_confirm) m_confirm->Rescale(); // ORCA
    Fit();
    Refresh();
}

void ParamsPanel::Highlighter::set_timer_owner(wxEvtHandler *owner, int timerid /* = wxID_ANY*/)
{
    m_timer.SetOwner(owner, timerid);
}

void ParamsPanel::Highlighter::init(std::pair<wxWindow *, bool *> params, wxWindow *parent)
    {
    if (m_timer.IsRunning()) invalidate();
    if (!params.first || !params.second) return;

    m_timer.Start(300, false);

    m_bitmap         = params.first;
    m_show_blink_ptr = params.second;
    m_parent         = parent;

    *m_show_blink_ptr = true;
    }

void ParamsPanel::Highlighter::invalidate()
{
    m_timer.Stop();

    if (m_bitmap && m_show_blink_ptr) {
        *m_show_blink_ptr = false;
        m_bitmap->Show(*m_show_blink_ptr);
        if (m_parent) {
            m_parent->Layout();
            m_parent->Refresh();
        }
        m_show_blink_ptr = nullptr;
        m_bitmap         = nullptr;
        m_parent         = nullptr;
    }

    m_blink_counter = 0;
}

void ParamsPanel::Highlighter::blink()
{
    if (m_bitmap && m_show_blink_ptr) {
        *m_show_blink_ptr = !*m_show_blink_ptr;
        m_bitmap->Show(*m_show_blink_ptr);
        if (m_parent) {
            m_parent->Layout();
            m_parent->Refresh();
        }
    } else
        return;

    if ((++m_blink_counter) == 11) invalidate();
}

ParamsPanel::ParamsPanel( wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, long style, const wxString& name )
    : wxPanel( parent, id, pos, size, style, name )
{
    // BBS: new layout
    SetBackgroundColour(*wxWHITE);
#if __WXOSX__
    m_top_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_top_sizer->SetSizeHints(this);
    this->SetSizer(m_top_sizer);

    // Create additional panel to Fit() it from OnActivate()
    // It's needed for tooltip showing on OSX
    m_tmp_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL);
    auto  sizer = new wxBoxSizer(wxHORIZONTAL);
    m_tmp_panel->SetSizer(sizer);
    m_tmp_panel->Layout();

#else
    ParamsPanel*panel = this;
    m_top_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_top_sizer->SetSizeHints(panel);
    panel->SetSizer(m_top_sizer);
#endif //__WXOSX__

    if (dynamic_cast<Notebook*>(parent)) {
        // BBS: new layout
        m_top_panel = new StaticBox(this, wxID_ANY, wxDefaultPosition);
        m_top_panel->SetBackgroundColor(0xF8F8F8);
        m_top_panel->SetBackgroundColor2(0xF1F1F1);

        m_process_icon = new ScalableButton(m_top_panel, wxID_ANY, "process");

        m_title_label = new Label(m_top_panel, _L("Process"));

        //int width, height;
        // BBS: new layout
        m_mode_region = new TabCtrl(m_top_panel, wxID_ANY);
        m_mode_region->AppendItem(_L("Global"));
        m_mode_region->AppendItem(_L("Objects"));
        set_scope_selection(m_mode_region, 0);
        //m_mode_region->GetSize(&width, &height);
        m_tips_arrow = new ScalableButton(m_top_panel, wxID_ANY, "tips_arrow");
        m_tips_arrow->Hide();

        m_mode_view = new ModeSwitchButton(m_top_panel);
        bool isDevMode = wxGetApp().get_mode() == comDevelop;
        m_mode_view->SetSelection(mode_to_selection(isDevMode ? comExpert : wxGetApp().get_saved_mode()));
        m_mode_view->SetDevMode(isDevMode);

        m_visibility_choice = new Button(m_top_panel, wxEmptyString);
        m_visibility_choice->SetToolTip(_L("More process actions"));
        m_visibility_choice->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
            event.Skip();
            wxMenu menu;
            const int compare = wxWindow::NewControlId();
            const int table = wxWindow::NewControlId();
            menu.Append(compare, _L("Compare presets…"));
            menu.Append(table, _L("View all object settings…"));
            menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
                CallAfter([]() { DiffPresetDialog::ensure()->show(); });
            }, compare);
            menu.Bind(wxEVT_MENU, [this](wxCommandEvent&) {
                CallAfter([]() { wxGetApp().plater()->PopupObjectTable(-1, -1, {0, 0}); });
            }, table);
            m_visibility_choice->PopupMenu(&menu, wxPoint(0, m_visibility_choice->GetSize().y));
        });
        m_mode_view->Hide();

        m_highlighter.set_timer_owner(this, 0);
        this->Bind(wxEVT_TIMER, [this](wxTimerEvent &)
        {
            m_highlighter.blink();
        });
    }



    //m_export_to_file = new Button( this, _L("Export To File"), "");
    //m_import_from_file = new Button( this, _L("Import From File") );

    // Initialize the page.
#if __WXOSX__
    auto page_parent = m_tmp_panel;
#else
    auto page_parent = this;
#endif

    // BBS: fix scroll to tip view
    class PageScrolledWindow : public wxScrolledWindow
    {
    public:
        PageScrolledWindow(wxWindow *parent)
            : wxScrolledWindow(parent,
                               wxID_ANY,
                               wxDefaultPosition,
                               wxDefaultSize,
                               wxVSCROLL            // hide hori-bar will cause hidden field mis-position
                               | wxTAB_TRAVERSAL    // Allows for traversal via tab key
                            ) 
        {
            // ShowScrollBar(GetHandle(), SB_BOTH, FALSE);
            Bind(wxEVT_SCROLL_CHANGED, [this](auto &e) {
                wxWindow *child = dynamic_cast<wxWindow *>(e.GetEventObject());
                if (child != this)
                    EnsureVisible(child);
            });
        }
        virtual bool ShouldScrollToChildOnFocus(wxWindow *child)
        {
            EnsureVisible(child);
            return false;
        }
        void EnsureVisible(wxWindow* win)
        {
            const wxRect viewRect(m_targetWindow->GetClientRect());
            const wxRect winRect(m_targetWindow->ScreenToClient(win->GetScreenPosition()), win->GetSize());
            if (viewRect.Contains(winRect)) {
                return;
            }
            if (winRect.GetWidth() > viewRect.GetWidth() || winRect.GetHeight() > viewRect.GetHeight()) {
                return;
            }
            int stepx, stepy;
            GetScrollPixelsPerUnit(&stepx, &stepy);

            int startx, starty;
            GetViewStart(&startx, &starty);
            // first in vertical direction:
            if (stepy > 0) {
                int diff = 0;

                if (winRect.GetTop() < 0) {
                    diff = winRect.GetTop();
                } else if (winRect.GetBottom() > viewRect.GetHeight()) {
                    diff = winRect.GetBottom() - viewRect.GetHeight() + 1;
                    // round up to next scroll step if we can't get exact position,
                    // so that the window is fully visible:
                    diff += stepy - 1;
                }
                starty = (starty * stepy + diff) / stepy;
            }
            // then horizontal:
            if (stepx > 0) {
                int diff = 0;
                if (winRect.GetLeft() < 0) {
                    diff = winRect.GetLeft();
                } else if (winRect.GetRight() > viewRect.GetWidth()) {
                    diff = winRect.GetRight() - viewRect.GetWidth() + 1;
                    // round up to next scroll step if we can't get exact position,
                    // so that the window is fully visible:
                    diff += stepx - 1;
                }
                startx = (startx * stepx + diff) / stepx;
            }
            Scroll(startx, starty);
        }
    };

    m_page_view = new PageScrolledWindow(page_parent);
    m_page_view->SetBackgroundColour(*wxWHITE);
    m_page_sizer = new wxBoxSizer(wxVERTICAL);

    m_page_view->SetSizer(m_page_sizer);
    m_page_view->SetScrollbars(1, FromDIP(20), 1, 2);
    //m_page_view->SetScrollRate( 5, 5 );

    if (m_mode_region)
        m_mode_region->Bind(wxEVT_TAB_SEL_CHANGED, &ParamsPanel::OnToggled, this);
    //Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { wxGetApp().plater()->search(false); }, wxID_FIND);
    //m_export_to_file->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { wxGetApp().mainframe->export_config(); });
    //m_import_from_file->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { wxGetApp().mainframe->load_config_file(); });
}

void ParamsPanel::create_layout()
{
#ifdef __WINDOWS__
    this->SetDoubleBuffered(true);
    m_page_view->SetDoubleBuffered(true);
#endif //__WINDOWS__

    m_left_sizer = new wxBoxSizer( wxVERTICAL );
    // BBS: new layout
    m_left_sizer->SetMinSize( wxSize(40 * em_unit(this), -1 ) );

    if (m_top_panel) {
        // Scope uses the same underline tabs as the settings categories.
        // Its own row leaves room for translated labels beside the process actions.
        m_mode_sizer = new wxBoxSizer(wxVERTICAL);
        m_scope_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_scope_sizer->Add(m_process_icon, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
        m_scope_sizer->Add(m_title_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        m_scope_sizer->AddStretchSpacer();
        m_scope_sizer->Add(m_tips_arrow, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
        m_scope_sizer->Add(m_visibility_choice, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
        m_mode_sizer->Add(m_scope_sizer, 0, wxEXPAND);
        m_mode_sizer->Add(m_mode_region, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
        m_top_panel->SetSizer(m_mode_sizer);
        //m_left_sizer->Add( m_top_panel, 0, wxEXPAND );
    }

    if (m_tab_print) {
        //m_print_sizer = new wxBoxSizer( wxHORIZONTAL );
        //m_print_sizer->Add( m_tab_print, 1, wxEXPAND | wxALL, 5 );
        //m_left_sizer->Add( m_print_sizer, 1, wxEXPAND, 5 );
        m_left_sizer->Add( m_tab_print, 0, wxEXPAND );
    }

    if (m_tab_scope) m_left_sizer->Add(m_tab_scope, 0, wxEXPAND);
    if (m_tab_print_plate) {
        m_left_sizer->Add(m_tab_print_plate, 0, wxEXPAND);
    }

    if (m_tab_print_object) {
        m_left_sizer->Add( m_tab_print_object, 0, wxEXPAND );
    }

    if (m_tab_print_part) {
        m_left_sizer->Add( m_tab_print_part, 0, wxEXPAND );
    }

    if (m_tab_print_layer) {
        m_left_sizer->Add(m_tab_print_layer, 0, wxEXPAND);
    }

    if (m_tab_filament) {
        //m_filament_sizer = new wxBoxSizer( wxVERTICAL );
        //m_filament_sizer->Add( m_tab_filament, 1, wxEXPAND | wxALL, 5 );
       // m_left_sizer->Add( m_filament_sizer, 1, wxEXPAND, 5 );
        m_left_sizer->Add( m_tab_filament, 0, wxEXPAND );
    }

    if (m_tab_printer) {
        //m_printer_sizer = new wxBoxSizer( wxVERTICAL );
        //m_printer_sizer->Add( m_tab_printer, 1, wxEXPAND | wxALL, 5 );
        m_left_sizer->Add( m_tab_printer, 0, wxEXPAND );
    }

    //m_left_sizer->Add( m_printer_sizer, 1, wxEXPAND, 1 );

    //m_button_sizer = new wxBoxSizer( wxHORIZONTAL );

    //m_button_sizer->Add( m_export_to_file, 0, wxALL, 5 );

    //m_button_sizer->Add( m_import_from_file, 0, wxALL, 5 );

    //m_left_sizer->Add( m_button_sizer, 0, wxALIGN_CENTER, 5 );

    m_top_sizer->Add(m_left_sizer, 1, wxEXPAND);

    //m_right_sizer = new wxBoxSizer( wxVERTICAL );

    //m_right_sizer->Add( m_page_view, 1, wxEXPAND | wxALL, 5 );

    //m_top_sizer->Add( m_right_sizer, 1, wxEXPAND, 5 );
    // BBS: new layout
    m_left_sizer->AddSpacer(6 * em_unit(this) / 10);
#if __WXOSX__
    m_left_sizer->Add(m_tmp_panel, 1, wxEXPAND | wxALL, 0);
    m_tmp_panel->GetSizer()->Add( m_page_view, 1, wxEXPAND );
#else
    m_left_sizer->Add( m_page_view, 1, wxEXPAND );
#endif

    //this->SetSizer( m_top_sizer );
    this->Layout();
    apply_sidebar_style();
}

void ParamsPanel::apply_sidebar_style()
{
    if (!is_prepare_sidebar()) return;
    using namespace DesignSystem;
    const bool dark = wxGetApp().dark_mode();
    const wxColour surface(color_hex(ColorRole::Surface, dark));
    const wxColour ink(color_hex(ColorRole::Ink, dark));
    const wxColour muted(color_hex(ColorRole::InkMuted, dark));
    SetBackgroundColour(surface);
    m_page_view->SetBackgroundColour(surface);
#if __WXOSX__
    m_tmp_panel->SetBackgroundColour(surface);
#endif
    m_top_panel->SetBackgroundColour(surface);
    m_top_panel->SetBackgroundColor(StateColor::resolved(surface));
    m_top_panel->SetBackgroundColor2(StateColor());
    m_top_panel->SetBorderColor(StateColor::resolved(wxColour(color_hex(ColorRole::Line, dark))));
    m_top_panel->SetCornerRadius(0);
    m_title_label->SetFont(font(TypeStyle::BodyStrong));
    m_title_label->SetForegroundColour(ink);
    set_sidebar_action_icon(m_process_icon, "layers");
    set_sidebar_action_icon(m_mode_icon, "eye");
    set_sidebar_action_icon(m_setting_btn, "list");
    set_sidebar_action_icon(m_compare_btn, "arrow-left-right");
    m_mode_region->SetFont(font(TypeStyle::Caption));
    m_mode_region->SetBackgroundColour(surface);
    m_mode_region->SetBackgroundColor(StateColor::resolved(surface));
    m_mode_region->SetBorderWidth(FromDIP(1));
    m_mode_region->SetBorderColor(StateColor::resolved(wxColour(color_hex(ColorRole::Line, dark))));
    m_mode_region->SetSidebarStyle(ColorRole::Primary);
    for (unsigned int i = 0; i < m_mode_region->GetCount(); ++i)
        m_mode_region->SetItemTextColour(i, StateColor::resolved(StateColor(
            std::make_pair(ink, int(StateColor::Checked)),
            std::make_pair(i == 1 && m_has_object_config ? wxColour(color_hex(ColorRole::AccentInk, dark)) : muted,
                           int(StateColor::Normal)))));
    m_mode_region->SetItemIndicator(1, m_has_object_config);
    m_mode_region->SetMinSize(wxSize(-1, FromDIP(row_height)));
    for (wxWindow* child : m_mode_region->GetChildren()) {
        if (auto* button = dynamic_cast<Button*>(child)) {
            button->SetPaddingSize(FromDIP(wxSize(2, 6)));
            button->SetBackgroundColour(surface);
            button->SetBackgroundColor(StateColor());
            button->SetBorderColor(StateColor::resolved(StateColor(
                std::make_pair(wxColour(color_hex(ColorRole::Focus, dark)), int(StateColor::Focused)),
                std::make_pair(surface, int(StateColor::Normal)))));
        }
    }
    m_mode_region->Rescale();
    m_mode_view->SetFont(font(TypeStyle::Caption));
    m_mode_view->Rescale();
    const wxBitmap source = create_scaled_bitmap(
        resources_dir() + "/design-system/icons/ellipsis", m_visibility_choice, 16);
    wxImage icon = source.ConvertToImage();
    icon.SetRGB(wxRect(0, 0, icon.GetWidth(), icon.GetHeight()), muted.Red(), muted.Green(), muted.Blue());
    wxBitmap tinted_icon(icon);
    tinted_icon.SetScaleFactor(source.GetScaleFactor());
    m_visibility_choice->SetIcon(tinted_icon);
    m_visibility_choice->SetFont(font(TypeStyle::Label));
    m_visibility_choice->SetPaddingSize(FromDIP(wxSize(4, 3)));
    m_visibility_choice->SetCornerRadius(FromDIP(radius_sm));
    m_visibility_choice->SetBackgroundColour(surface);
    m_visibility_choice->SetBackgroundColor(StateColor::resolved(StateColor(
        std::make_pair(wxColour(color_hex(ColorRole::SurfaceHover, dark)), static_cast<int>(StateColor::Hovered)),
        std::make_pair(surface, static_cast<int>(StateColor::Normal)))));
    m_visibility_choice->SetBorderColor(StateColor::resolved(StateColor(
        std::make_pair(wxColour(color_hex(ColorRole::Focus, dark)), int(StateColor::Focused)),
        std::make_pair(surface, int(StateColor::Normal)))));
    m_visibility_choice->SetTextColor(StateColor::resolved(StateColor(
        std::make_pair(ink, static_cast<int>(StateColor::Hovered)),
        std::make_pair(muted, static_cast<int>(StateColor::Normal)))));
    m_visibility_choice->SetMinSize(FromDIP(wxSize(28, 28)));
    m_visibility_choice->SetMaxSize(FromDIP(wxSize(28, 28)));
    update_mode();
    if (m_left_sizer) m_left_sizer->SetMinSize(wxSize(FromDIP(sidebar_width), -1));
    if (m_scope_sizer) m_scope_sizer->SetMinSize(-1, FromDIP(40));
    if (m_tools_sizer) m_tools_sizer->SetMinSize(-1, FromDIP(24));
    m_top_panel->Layout();
    m_page_view->FitInside();
    Refresh();
}

void ParamsPanel::rebuild_panels()
{
    refresh_tabs();
    free_sizers();
    create_layout();
}

void ParamsPanel::refresh_tabs()
{
    auto& tabs_list = wxGetApp().tabs_list;
    auto print_tech = wxGetApp().preset_bundle->printers.get_selected_preset().printer_technology();
    for (auto tab : tabs_list)
        if (tab->supports_printer_technology(print_tech))
        {
            if (tab->GetParent() != this) continue;
            switch (tab->type())
            {
                case Preset::TYPE_PRINT:
                case Preset::TYPE_SLA_PRINT:
                    m_tab_print = tab;
                    break;

                case Preset::TYPE_FILAMENT:
                case Preset::TYPE_SLA_MATERIAL:
                    m_tab_filament = tab;
                    break;

                case Preset::TYPE_PRINTER:
                    m_tab_printer = tab;
                    break;
                default:
                    break;
            }
        }
    if (m_top_panel) {
        m_tab_print_plate = wxGetApp().get_plate_tab();
        m_tab_print_object = wxGetApp().get_model_tab();
        m_tab_print_part = wxGetApp().get_model_tab(true);
        m_tab_print_layer = wxGetApp().get_layer_tab();
        if (!m_tab_scope && m_tab_print && static_cast<Tab*>(m_tab_print)->completed()) {
            m_tab_scope = new TabPrintScope(this);
            m_tab_scope->create_preset_tab();
            // Preset loading and unsaved-change checks must skip this detached editor.
            // The print tab forwards mode, colour and label updates to it.
            auto& tabs = wxGetApp().tabs_list;
            tabs.erase(std::remove(tabs.begin(), tabs.end(), m_tab_scope), tabs.end());
        }
    }
    return;
}

void ParamsPanel::clear_page()
{
    if (m_page_sizer)
        m_page_sizer->Clear(true);
}


void ParamsPanel::OnActivate()
{
    if (m_current_tab == NULL)
        select_default_tab();
    Tab* cur_tab = dynamic_cast<Tab *> (m_current_tab);
    if (cur_tab)
        cur_tab->OnActivate();
}

void ParamsPanel::select_default_tab()
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": first time opened, set current tab to print");
    // BBS: open/close tab
    //m_current_tab = m_tab_print;
    set_active_tab(m_tab_print ? m_tab_print : m_tab_filament);
}

bool ParamsPanel::SettingsPagePrebuild::built() const
{
    Tab* tab = dynamic_cast<Tab*>(m_panel.m_current_tab);
    return tab != nullptr && !tab->page_build_pending();
}

bool ParamsPanel::SettingsPagePrebuild::build_step()
{
    if (m_panel.m_current_tab == nullptr) {
        m_panel.select_default_tab();
        return !built();
    }
    Tab* tab = dynamic_cast<Tab*>(m_panel.m_current_tab);
    return tab != nullptr && tab->page_build_step();
}

void ParamsPanel::OnToggled(wxCommandEvent& event)
{
    if (m_mode_region && m_mode_region->GetId() == event.GetId()) {
        wxWindowUpdateLocker locker(GetParent());
        set_active_tab(nullptr);
        event.Skip();
        return;
    }

    event.Skip();
}

// This is special, DO NOT call it from outer except from Tab
void ParamsPanel::set_active_tab(wxPanel* tab)
{
    if (m_mode_region) {
        auto* plater = wxGetApp().plater();
        m_mode_region->SetItemText(0, plater && plater->get_partplate_list().independent_setups() ?
            _L("This plate") : _L("Global"));
    }
    Tab* cur_tab = dynamic_cast<Tab *> (tab);
    if (cur_tab == m_tab_print && wxGetApp().plater()->get_partplate_list().independent_setups() &&
        wxGetApp().sidebar().settings_scope() <= ProcessSettingsScope::Stage) {
        show_cascade_scope();
        return;
    }

    if (cur_tab == nullptr) {
        if (wxGetApp().plater()->get_partplate_list().independent_setups() &&
            wxGetApp().sidebar().settings_scope() <= ProcessSettingsScope::Stage) {
            show_cascade_scope();
            return;
        }
        if (m_mode_region->GetSelection() == 0) {
            cur_tab = (Tab*) m_tab_print;
        } else if (m_tab_print_part && ((TabPrintModel*) m_tab_print_part)->has_model_config()) {
            cur_tab = (Tab*) m_tab_print_part;
        } else if (m_tab_print_layer && ((TabPrintModel*)m_tab_print_layer)->has_model_config()) {
            cur_tab = (Tab*)m_tab_print_layer;
        } else if (m_tab_print_object && ((TabPrintModel*) m_tab_print_object)->has_model_config()) {
            cur_tab = (Tab*) m_tab_print_object;
        } else if (m_tab_print_plate && ((TabPrintPlate*)m_tab_print_plate)->has_model_config()) {
            cur_tab = (Tab*)m_tab_print_plate;
        }
        Show(cur_tab != nullptr);
        wxGetApp().sidebar().show_object_list(m_mode_region->GetSelection() == 1);
        if (m_current_tab == cur_tab)
            return;
        if (cur_tab)
            cur_tab->restore_last_select_item();
        return;
    }

    m_current_tab = tab;
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": set current to %1%, type=%2%") % cur_tab % cur_tab?cur_tab->type():-1;
    update_mode();

    // BBS: open/close tab
    for (auto t : std::vector<std::pair<wxPanel*, wxStaticLine*>>({
            {m_tab_scope, nullptr},
            {m_tab_print, m_staticline_print},
            {m_tab_print_object, m_staticline_print_object},
            {m_tab_print_part, m_staticline_print_part},
            {m_tab_print_layer, nullptr},
            {m_tab_print_plate, nullptr},
            {m_tab_filament, m_staticline_filament},
            {m_tab_printer, m_staticline_printer}})) {
        if (!t.first) continue;
        t.first->Show(tab == t.first);
        if (!t.second) continue;
        t.second->Show(tab == t.first);
        //m_left_sizer->GetItem(t)->SetProportion(tab == t ? 1 : 0);
    }
    m_left_sizer->Layout();
    if (auto dialog = dynamic_cast<wxDialog*>(GetParent())) {
        wxString title = cur_tab->type() == Preset::TYPE_FILAMENT ? _L("Material settings") : _L("Printer settings");
        dialog->SetTitle(title);
    }

    auto tab_print = dynamic_cast<Tab *>(m_tab_print);
    if (cur_tab == m_tab_print) {
        if (tab_print)
            tab_print->toggle_line("print_flow_ratio", false);
    } else {
        if (tab_print)
            tab_print->toggle_line("print_flow_ratio", false);
    }
}

bool ParamsPanel::is_active_and_shown_tab(wxPanel* tab)
{
    if (m_current_tab == tab)
        return true;
    else
        return false;
}

void ParamsPanel::update_mode()
{
    int app_mode = Slic3r::GUI::wxGetApp().get_mode();
    auto sync_mode_view = [&](ModeSwitchButton* mode_view) {
        if (mode_view == nullptr)
            return;

        if (app_mode == comDevelop) {
            mode_view->SetSelection(mode_to_selection(comExpert));
            mode_view->SetDevMode(true);
            return;
        }

        mode_view->SetSelection(mode_to_selection(Slic3r::GUI::wxGetApp().get_saved_mode()));
        if (mode_view->GetDevMode())
            mode_view->SetDevMode(false);
    };

    sync_mode_view(m_mode_view);
    sync_mode_view(m_current_tab ? dynamic_cast<Tab*>(m_current_tab)->m_mode_view : nullptr);

    auto sync_mode_icon = [&](ScalableButton* mode_icon) {
        if (mode_icon == nullptr)
            return;
        mode_icon->Show(app_mode != comDevelop);
    };
    sync_mode_icon(m_mode_icon);
    sync_mode_icon(m_current_tab ? dynamic_cast<Tab*>(m_current_tab)->m_mode_icon : nullptr);
}

void ParamsPanel::msw_rescale()
{
    if (m_process_icon) m_process_icon->msw_rescale();
    if (m_setting_btn) m_setting_btn->msw_rescale();
    if (m_search_btn) m_search_btn->msw_rescale();
    if (m_compare_btn) m_compare_btn->msw_rescale();
    if (m_tips_arrow) m_tips_arrow->msw_rescale();
    m_left_sizer->SetMinSize(wxSize(40 * em_unit(this), -1));
    if (m_mode_sizer)
        m_mode_sizer->SetMinSize(-1, 3 * em_unit(this));
    if (m_mode_region)
        m_mode_region->Rescale();
    if (m_mode_icon) m_mode_icon->msw_rescale();
    if (m_mode_view)
    {
        m_mode_view->Rescale();
    }
    for (auto tab : {m_tab_print, m_tab_print_plate, m_tab_print_object, m_tab_print_part, m_tab_print_layer, static_cast<wxPanel*>(m_tab_scope), m_tab_filament, m_tab_printer}) {
        if (tab) dynamic_cast<Tab*>(tab)->msw_rescale();
    }
    apply_sidebar_style();
    //((Button*)m_export_to_file)->Rescale();
    //((Button*)m_import_from_file)->Rescale();
}

void ParamsPanel::show_cascade_scope()
{
    if (!m_tab_scope || !m_tab_print) return;
    auto& plates = wxGetApp().plater()->get_partplate_list();
    if (!plates.independent_setups()) { m_mode_region->Show(); return; }
    const auto scope = wxGetApp().sidebar().settings_scope();
    m_mode_region->Hide();
    if (scope == ProcessSettingsScope::Object) {
        set_scope_selection(m_mode_region, 1);
        set_active_tab(nullptr);
    } else if (scope == ProcessSettingsScope::Plate) {
        set_scope_selection(m_mode_region, 0);
        set_active_tab(nullptr);
        wxGetApp().sidebar().show_object_list(false);
    } else {
        const auto& scopes = plates.process_scopes();
        const auto scope_selection = plates.workspace_selection();
        const auto stage = scope != ProcessSettingsScope::Stage || scope_selection.empty() ? std::string{} :
            plates.get_plate(scope_selection.front())->stage_id();
        DynamicPrintConfig overrides = scopes.project;
        if (!stage.empty()) {
            const auto found = scopes.stages.find(stage);
            overrides = found == scopes.stages.end() ? DynamicPrintConfig{} : found->second;
        }
        // Native plate options do not take part in the Project and Stage values.
        std::vector<PlateProcessCascade> cascade;
        for (int i = 0; i < plates.get_plate_count(); ++i)
            cascade.push_back({plates.process_setup(i), {}, plates.get_plate(i)->stage_id()});
        const auto view = process_scope_view(cascade, scopes, stage);
        if (view.values.empty()) return;
        m_tab_scope->load_scope(view.values, view.inherited, overrides, view.mixed, view.mixed_inherited);
        m_tab_scope->restore_last_select_item();
        wxGetApp().sidebar().show_object_list(false);
    }
    Layout();
}

void ParamsPanel::switch_to_global()
{
    set_scope_selection(m_mode_region, 0);
    set_active_tab(nullptr);
}

void ParamsPanel::switch_to_object(bool with_tips)
{
    set_scope_selection(m_mode_region, 1);
    set_active_tab(nullptr);
    if (with_tips) {
        m_highlighter.init(std::pair(m_tips_arrow, &m_tips_arror_blink), m_top_panel);
        m_highlighter.blink();
    }
}

void ParamsPanel::notify_object_config_changed()
{
    auto & model = wxGetApp().model();
    bool has_config = false;
    for (auto obj : model.objects) {
        if (!obj->config.empty()) {
            SettingsFactory::Bundle cat_options = SettingsFactory::get_bundle(&obj->config.get(), true);
            if (cat_options.size() > 0) {
                has_config = true;
                break;
            }
        }
        for (auto volume : obj->volumes) {
            if (!volume->config.empty()) {
                SettingsFactory::Bundle cat_options = SettingsFactory::get_bundle(&volume->config.get(), true);
                if (cat_options.size() > 0) {
                    has_config = true;
                    break;
                }
            }
        }
        if (has_config) break;
    }
    if (has_config == m_has_object_config) return;
    m_has_object_config = has_config;
    apply_sidebar_style();
}

void ParamsPanel::switch_to_object_if_has_object_configs()
{
    if (m_has_object_config)
        set_scope_selection(m_mode_region, 1);
    set_active_tab(nullptr);
}

void ParamsPanel::free_sizers()
{
    if (m_top_sizer)
    {
        m_top_sizer->Clear(false);
        //m_top_sizer = nullptr;
    }

    m_left_sizer = nullptr;
    //m_right_sizer = nullptr;
    m_mode_sizer = nullptr;
    m_scope_sizer = nullptr;
    m_tools_sizer = nullptr;
    //m_print_sizer = nullptr;
    //m_filament_sizer = nullptr;
    //m_printer_sizer = nullptr;
    m_button_sizer = nullptr;
}

void ParamsPanel::delete_subwindows()
{
    if (m_title_label)
    {
        delete m_title_label;
        m_title_label = nullptr;
    }

    if (m_mode_region)
    {
        delete m_mode_region;
        m_mode_region = nullptr;
    }

    if (m_mode_view)
    {
        m_mode_view = nullptr;
    }
    m_visibility_choice = nullptr;

    if (m_mode_icon) // ORCA m_title_view replacement
    {
        delete m_mode_icon;
        m_mode_icon = nullptr;
    }

    if (m_search_btn)
    {
        delete m_search_btn;
        m_search_btn = nullptr;
    }

    if (m_staticline_print)
    {
        delete m_staticline_print;
        m_staticline_print = nullptr;
    }

    if (m_staticline_print_part)
    {
        delete m_staticline_print_part;
        m_staticline_print_part = nullptr;
    }

    if (m_staticline_print_object)
    {
        delete m_staticline_print_object;
        m_staticline_print_object = nullptr;
    }

    if (m_staticline_filament)
    {
        delete m_staticline_filament;
        m_staticline_filament = nullptr;
    }

    if (m_staticline_printer)
    {
        delete m_staticline_printer;
        m_staticline_printer = nullptr;
    }

    if (m_export_to_file)
    {
        delete m_export_to_file;
        m_export_to_file = nullptr;
    }

    if (m_import_from_file)
    {
        delete m_import_from_file;
        m_import_from_file = nullptr;
    }

    if (m_page_view)
    {
        delete m_page_view;
        m_page_view = nullptr;
    }
}

ParamsPanel::~ParamsPanel()
{
#if 0
    free_sizers();
    delete m_top_sizer;

    delete_subwindows();
#endif
    // BBS: fix double destruct of OG_CustomCtrl
    Tab* cur_tab = dynamic_cast<Tab*> (m_current_tab);
    if (cur_tab)
        cur_tab->clear_pages();
}

} // GUI
} // Slic3r
