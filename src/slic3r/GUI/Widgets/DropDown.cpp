#include "DropDown.hpp"
#include "Label.hpp"
#include "Button.hpp"
#include "TextInput.hpp"
#include "slic3r/GUI/DesignTokens.hpp"
#include "slic3r/GUI/Typography.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/Utils.hpp"
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/brush.h>
#include <wx/control.h>
#include <wx/dialog.h>
#include <wx/frame.h>
#include <wx/graphics.h>
#include <wx/pen.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/weakref.h>


#include <cassert>
#include <algorithm>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <cstdio>
#include "slic3r/GUI/Widgets/PopupWindow.hpp"
#include <vector>
#include <utility>
#include <iterator>
#include <wx/dc.h>
#include <wx/dcclient.h>
#include <wx/display.h>
#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>
#include <wx/event.h>
#include <wx/popupwin.h>
#include <wx/stattext.h>
#include <wx/gdicmn.h>
#include <wx/window.h>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif

#include <set>
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/Widgets/StateHandler.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

// A transient popup cannot reliably take typing focus: macOS shows a non-activating
// panel and GTK maps an unmanaged window under pointer grabs. A frameless modeless
// dialog activates normally on every port, so the search field gets keyboard focus,
// and it closes like a popup by hiding on deactivation. The item container and
// event relay stay in DropDown. Wayland places top-level windows itself, so there
// the dialog may not sit directly beside its anchor.
class PrinterDropDownDialog final : public Slic3r::GUI::DPIDialog
{
    struct Row { int index; wxString heading; };
    DropDown* m_owner;
    wxWeakRef<wxWindow> m_frame;
    TextInput* m_search;
    wxScrolledWindow* m_list;
    Button* m_clear;
    Button* m_edit;
    Button* m_manage;
    std::vector<Row> m_rows;
    int m_active = -1;
    int m_hover = -1;
    bool m_opening = false;
    bool m_dismissing = false;
    // Status providers read cached state and send no change notification, so an
    // open list repaints once a second while any row reports a status.
    wxTimer m_status_timer;
    // Resolved once per theme or DPI change instead of for every painted row.
    wxFont m_body_font;
    wxFont m_caption_font;
    wxFont m_strong_font;

    static wxColour color(Slic3r::GUI::DesignSystem::ColorRole role)
    {
        return wxColour(Slic3r::GUI::DesignSystem::color_hex(role, Slic3r::GUI::wxGetApp().dark_mode()));
    }
    int height(const Row& row) const { return FromDIP(row.index < 0 ? 28 : 54); }
    int row_y(int row) const
    {
        int y = 0;
        for (int i = 0; i < row; ++i) y += height(m_rows[i]);
        return y;
    }
    bool selectable(int row) const
    {
        return m_owner && row >= 0 && row < int(m_rows.size()) && m_rows[row].index >= 0 &&
               !(m_owner->items[m_rows[row].index].style & (DD_ITEM_STYLE_DISABLED | DD_ITEM_STYLE_SPLIT_ITEM));
    }
    int hit(const wxPoint& point) const
    {
        const auto pt = m_list->CalcUnscrolledPosition(point);
        if (point.x < 0 || point.x >= m_list->GetClientSize().x || point.y < 0 || point.y >= m_list->GetClientSize().y)
            return -1;
        int y = 0;
        for (int i = 0; i < int(m_rows.size()); ++i) {
            y += height(m_rows[i]);
            if (pt.y < y) return i;
        }
        return -1;
    }
    void ensure_visible()
    {
        if (m_active < 0) return;
        const int top = row_y(m_active), bottom = top + height(m_rows[m_active]);
        const int scroll = m_list->GetViewStart().y * FromDIP(4);
        const int visible = m_list->GetClientSize().y;
        if (top < scroll) m_list->Scroll(0, top / FromDIP(4));
        else if (bottom > scroll + visible) m_list->Scroll(0, (bottom - visible + FromDIP(3)) / FromDIP(4));
    }
    void step(int direction)
    {
        int candidate = m_active < 0 && direction < 0 ? 0 : m_active;
        for (int i = 0; i < int(m_rows.size()); ++i) {
            candidate = (candidate + direction + int(m_rows.size())) % int(m_rows.size());
            if (selectable(candidate)) {
                m_active = candidate;
                ensure_visible();
                m_list->Refresh();
                return;
            }
        }
    }
    void choose(int row, bool action = false)
    {
        if (!selectable(row)) return;
        const int index = m_rows[row].index;
        const wxString identity = m_owner->items[index].text;
        wxWeakRef<DropDown> owner(m_owner);
        if (!action) dismiss(true);
        // Dismissal notifies the owner, which may destroy it.
        if (!owner) return;
        // A selection can rebuild the whole sidebar. Never dispatch on the mouse stack.
        owner->CallAfter([owner, index, identity, action] {
            if (!owner || owner->IsBeingDeleted() || index >= int(owner->items.size()) || owner->items[index].text != identity)
                return;
            if (action) owner->sendDropDownActionEvent(index);
            else {
                wxCommandEvent event(wxEVT_COMBOBOX, owner->GetId());
                event.SetEventObject(owner.get());
                event.SetInt(index);
                event.SetString(identity);
                owner->GetEventHandler()->ProcessEvent(event);
            }
            if (owner && owner->printer_popup && owner->printer_popup->IsShown())
                owner->printer_popup->rebuild();
        });
    }
    void footer(bool edit)
    {
        if (!m_owner) return;
        auto callback = edit ? m_owner->printer_edit_action : m_owner->printer_manage_action;
        wxWeakRef<wxWindow> opener(m_owner->GetParent());
        dismiss(false);
        if (callback && opener) opener->CallAfter([opener, callback] {
            if (opener && !opener->IsBeingDeleted()) callback();
        });
    }
    void key(wxKeyEvent& event)
    {
        if (event.HasAnyModifiers()) { event.Skip(); return; }
        const bool list_focus = wxWindow::FindFocus() == m_search->GetTextCtrl() || wxWindow::FindFocus() == m_list;
        switch (event.GetKeyCode()) {
        case WXK_ESCAPE: dismiss(true); return;
        case WXK_DOWN: if (list_focus) { step(1); return; } break;
        case WXK_UP: if (list_focus) { step(-1); return; } break;
        case WXK_RETURN:
        case WXK_NUMPAD_ENTER:
            // Let focused footer buttons handle their own command.
            if (list_focus) {
                choose(m_active); return;
            }
            break;
        default: break;
        }
        event.Skip();
    }
    void paint_list(wxPaintEvent&)
    {
        using namespace Slic3r::GUI::DesignSystem;
        wxAutoBufferedPaintDC paint(m_list);
        m_list->PrepareDC(paint);
        wxGCDC dc(paint);
        dc.SetBackground(wxBrush(color(ColorRole::SurfaceRaised)));
        dc.Clear();
        if (!m_owner) return;
        const int width = m_list->GetClientSize().x;
        int y = 0;
        const int visible_top = m_list->CalcUnscrolledPosition(wxPoint(0, 0)).y;
        const int visible_bottom = visible_top + m_list->GetClientSize().y;
        if (m_rows.empty()) {
            dc.SetFont(m_body_font);
            dc.SetTextForeground(color(ColorRole::InkMuted));
            dc.DrawText(m_search->GetTextCtrl()->GetValue().empty() ? _L("No printers available.") : _L("No printers match your search."),
                        FromDIP(10), FromDIP(18));
        }
        for (int r = 0; r < int(m_rows.size()); ++r) {
            const Row& row = m_rows[r];
            if (y > visible_bottom) break;
            if (y + height(row) < visible_top) { y += height(row); continue; }
            if (row.index < 0) {
                dc.SetFont(m_caption_font);
                dc.SetTextForeground(color(ColorRole::InkMuted));
                dc.DrawText(wxControl::Ellipsize(row.heading, dc, wxELLIPSIZE_END, width - FromDIP(20)), FromDIP(10), y + FromDIP(6));
                y += height(row); continue;
            }
            const auto& item = m_owner->items[row.index];
            const bool selected = row.index == m_owner->selection;
            const bool disabled = item.style & DD_ITEM_STYLE_DISABLED;
            const bool dimmed = item.style & DD_ITEM_STYLE_DIMMED;
            if (selected || r == m_hover || r == m_active) {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(color(selected ? ColorRole::AccentSoft : ColorRole::SurfaceHover)));
                dc.DrawRoundedRectangle(0, y + FromDIP(2), width, height(row) - FromDIP(4), FromDIP(7));
            }
            if (r == m_active && !selected) {
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.SetPen(wxPen(color(ColorRole::Focus), FromDIP(1)));
                dc.DrawRoundedRectangle(FromDIP(1), y + FromDIP(3), width - FromDIP(2), height(row) - FromDIP(6), FromDIP(6));
            }
            const wxColour ink = color(disabled ? ColorRole::InkDisabled : selected ? ColorRole::AccentInk : dimmed ? ColorRole::InkMuted : ColorRole::Ink);
            int left = FromDIP(10);
            if (item.icon.IsOk()) {
                const wxSize size = item.icon.GetLogicalSize();
                const int slot = FromDIP(40);
                dc.DrawBitmap(item.icon, left + (slot - size.x) / 2, y + (height(row) - size.y) / 2, true);
                left += slot + FromDIP(10);
            }
            const int right = FromDIP(item.action_icon.IsOk() ? 60 : 30);
            const wxString label = item.popup_label.empty() ? item.text : item.popup_label;
            const auto status = item.status_provider ? item.status_provider() : DropDown::ItemStatus{item.text_static_tips};
            const wxString& subtitle = status.subtitle;
            dc.SetFont(m_strong_font);
            dc.SetTextForeground(ink);
            dc.DrawText(wxControl::Ellipsize(label, dc, wxELLIPSIZE_END, std::max(1, width - left - right)),
                        left, y + FromDIP(subtitle.empty() ? 17 : 9));
            if (!subtitle.empty()) {
                int subtitle_left = left;
                if (item.status_provider) {
                    using Indicator = DropDown::ItemStatus::Indicator;
                    // Connection indicators use the requested green/red convention;
                    // the general success/danger palette is teal/vermilion.
                    const bool dark = Slic3r::GUI::wxGetApp().dark_mode();
                    const wxColour dot = status.indicator == Indicator::positive ? wxColour(dark ? "#4ADE80" : "#15803D") :
                        status.indicator == Indicator::negative ? wxColour(dark ? "#F87171" : "#DC2626") : color(ColorRole::InkMuted);
                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.SetBrush(wxBrush(dot));
                    dc.DrawCircle(left + FromDIP(4), y + FromDIP(37), FromDIP(3));
                    subtitle_left += FromDIP(13);
                }
                dc.SetFont(m_caption_font);
                dc.SetTextForeground(selected ? ink : color(disabled ? ColorRole::InkDisabled : ColorRole::InkMuted));
                dc.DrawText(wxControl::Ellipsize(subtitle, dc, wxELLIPSIZE_END, std::max(1, width - subtitle_left - right)), subtitle_left, y + FromDIP(30));
            }
            if (selected) {
                const int x = width - FromDIP(item.action_icon.IsOk() ? 46 : 18), mid = y + height(row) / 2;
                dc.SetPen(wxPen(ink, FromDIP(2)));
                dc.DrawLine(x - FromDIP(4), mid, x - FromDIP(1), mid + FromDIP(3));
                dc.DrawLine(x - FromDIP(1), mid + FromDIP(3), x + FromDIP(5), mid - FromDIP(4));
            }
            if (item.action_icon.IsOk()) {
                const wxSize size = item.action_icon.GetLogicalSize();
                dc.DrawBitmap(item.action_icon, width - FromDIP(20) - size.x / 2, y + (height(row) - size.y) / 2, true);
            }
            y += height(row);
        }
    }
    void frame_show(wxShowEvent& event) { if (!event.IsShown()) dismiss(false); event.Skip(); }
    void frame_iconize(wxIconizeEvent& event) { if (event.IsIconized()) dismiss(false); event.Skip(); }
    void apply_theme()
    {
        using namespace Slic3r::GUI::DesignSystem;
        m_body_font = font(TypeStyle::Body);
        m_caption_font = font(TypeStyle::Caption);
        m_strong_font = font(TypeStyle::BodyStrong);
        const wxColour raised = color(ColorRole::SurfaceRaised);
        const auto icon = [this](const char* name) {
            const wxBitmap source = create_scaled_bitmap(Slic3r::resources_dir() + "/design-system/icons/" + name, this, 16);
            if (!source.IsOk()) return wxNullBitmap;
            wxImage image = source.ConvertToImage();
            const wxColour tint = color(ColorRole::InkMuted);
            image.SetRGB(wxRect(0, 0, image.GetWidth(), image.GetHeight()), tint.Red(), tint.Green(), tint.Blue());
            wxBitmap result(image);
            result.SetScaleFactor(source.GetScaleFactor());
            return result;
        };
        m_search->SetIcon_1(icon("search"));
        m_edit->SetIcon(icon("settings-2"));
        m_manage->SetIcon(icon("sliders-horizontal"));
        SetBackgroundColour(raised);
        m_list->SetBackgroundColour(raised);
        StateColor bg(raised), ink(color(ColorRole::Ink));
        bg = StateColor::resolved(bg); ink = StateColor::resolved(ink);
        m_search->SetBackgroundColor(bg);
        m_search->SetBorderColor(StateColor::resolved(StateColor(
            std::make_pair(color(ColorRole::Focus), int(StateColor::Focused)),
            std::make_pair(color(ColorRole::Line), int(StateColor::Normal)))));
        m_search->SetTextColor(ink);
        m_search->GetTextCtrl()->SetBackgroundColour(raised);
        m_search->GetTextCtrl()->SetForegroundColour(color(ColorRole::Ink));
        m_search->SetFont(m_body_font);
        for (Button* button : {m_clear, m_edit, m_manage}) {
            StateColor background(std::make_pair(color(ColorRole::SurfaceHover), int(StateColor::Hovered)),
                                  std::make_pair(raised, int(StateColor::Normal)));
            background = StateColor::resolved(background);
            button->SetBackgroundColor(background);
            button->SetBorderWidth(0);
            button->SetCornerRadius(FromDIP(6));
            button->SetTextColor(ink);
            button->SetFont(m_body_font);
            button->SetMinSize(FromDIP(wxSize(0, 34)));
        }
        m_clear->SetTextColor(StateColor::resolved(color(ColorRole::AccentInk)));
        m_search->SetCornerRadius(FromDIP(7));
        m_list->Refresh(); Refresh();
    }
    void on_dpi_changed(const wxRect&) override
    {
        m_search->Rescale();
        m_list->SetScrollRate(0, FromDIP(4));
        apply_theme(); rebuild();
        GetSizer()->SetSizeHints(this);
    }
    void on_sys_color_changed() override { apply_theme(); }

public:
    explicit PrinterDropDownDialog(DropDown* owner)
        : Slic3r::GUI::DPIDialog(owner->GetParent(), wxID_ANY, _L("Choose printer"), wxDefaultPosition, wxDefaultSize,
                                 wxBORDER_NONE | wxFRAME_NO_TASKBAR | wxFRAME_FLOAT_ON_PARENT | wxFRAME_SHAPED), m_owner(owner)
    {
        m_status_timer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (m_owner && IsShown() && !Slic3r::GUI::wxGetApp().is_closing()) m_list->Refresh();
        }, m_status_timer.GetId());
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(color(Slic3r::GUI::DesignSystem::ColorRole::SurfaceRaised));
        auto* layout = new wxBoxSizer(wxVERTICAL);
        m_search = new TextInput(this, "", "", "", wxDefaultPosition, FromDIP(wxSize(280, 36)), wxTE_PROCESS_ENTER);
        m_search->SetSidebarStyle();
        m_search->GetTextCtrl()->SetHint(_L("Find a printer…"));
        m_search->GetTextCtrl()->SetName(_L("Find a printer"));
        layout->Add(m_search, 0, wxEXPAND | wxALL, FromDIP(8));
        m_list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxVSCROLL);
        m_list->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_list->SetScrollRate(0, FromDIP(4));
        m_list->SetName(_L("Choose printer"));
        layout->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
        m_clear = new Button(this, _L("Clear search"));
        m_edit = new Button(this, _L("Edit printer preset…"));
        m_manage = new Button(this, _L("Select/remove printers…"));
        for (Button* button : {m_clear, m_edit, m_manage}) {
            button->SetStyle(ButtonStyle::Regular, ButtonType::Expanded);
            button->SetCenter(false);
            layout->Add(button, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
        }
        layout->AddSpacer(FromDIP(8));
        SetSizerAndFit(layout);
        Bind(wxEVT_CHAR_HOOK, &PrinterDropDownDialog::key, this);
        Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
            if (event.GetActive() && IsShown()) m_search->GetTextCtrl()->SetFocus();
            else if (!event.GetActive() && !m_opening && !m_dismissing) dismiss(false);
            event.Skip();
        });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
            if (event.CanVeto()) { event.Veto(); dismiss(true); }
            else event.Skip();
        });
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC paint(this);
            wxGCDC dc(paint);
            dc.SetBackground(wxBrush(GetBackgroundColour())); dc.Clear();
            dc.SetPen(wxPen(color(Slic3r::GUI::DesignSystem::ColorRole::Line), FromDIP(1)));
            dc.SetBrush(wxBrush(GetBackgroundColour()));
            dc.DrawRoundedRectangle(0, 0, GetClientSize().x, GetClientSize().y, FromDIP(12));
        });
        m_search->Bind(wxEVT_TEXT, [this](wxCommandEvent& event) { rebuild(); event.Skip(); });
        m_list->Bind(wxEVT_PAINT, &PrinterDropDownDialog::paint_list, this);
        m_list->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (!m_owner || !IsShown()) { event.Skip(); return; }
            const int row = hit(event.GetPosition());
            if (row != m_hover) {
                m_hover = selectable(row) ? row : -1;
                if (row >= 0 && m_rows[row].index >= 0) {
                    const auto& item = m_owner->items[m_rows[row].index];
                    const wxString tip = item.status_provider ? item.text + "\n" + item.status_provider().subtitle : item.tip;
                    if (!tip.empty()) m_list->SetToolTip(tip);
                    else m_list->UnsetToolTip();
                } else m_list->UnsetToolTip();
                m_list->Refresh();
            }
            event.Skip();
        });
        m_list->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) { m_hover = -1; m_list->Refresh(); event.Skip(); });
        m_list->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            if (!IsShown() || !m_owner) { event.Skip(); return; }
            const int row = hit(event.GetPosition());
            if (selectable(row)) {
                const bool action = m_owner->items[m_rows[row].index].action_icon.IsOk() && event.GetX() >= m_list->GetClientSize().x - FromDIP(40);
                choose(row, action);
            }
            event.Skip();
        });
        m_clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) { m_search->GetTextCtrl()->SetValue(""); m_search->GetTextCtrl()->SetFocus(); event.Skip(); });
        m_edit->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) { footer(true); event.Skip(); });
        m_manage->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) { footer(false); event.Skip(); });
        m_frame = wxGetTopLevelParent(owner->GetParent());
        if (m_frame) {
            m_frame->Bind(wxEVT_SHOW, &PrinterDropDownDialog::frame_show, this);
            m_frame->Bind(wxEVT_ICONIZE, &PrinterDropDownDialog::frame_iconize, this);
        }
        Slic3r::GUI::wxGetApp().UpdateDlgDarkUI(this);
        apply_theme();
    }
    ~PrinterDropDownDialog() override
    {
        if (m_owner && m_owner->printer_popup == this) m_owner->printer_popup = nullptr;
        detach();
    }
    void detach()
    {
        m_status_timer.Stop();
        if (m_frame) {
            m_frame->Unbind(wxEVT_SHOW, &PrinterDropDownDialog::frame_show, this);
            m_frame->Unbind(wxEVT_ICONIZE, &PrinterDropDownDialog::frame_iconize, this);
        }
        m_frame = nullptr;
        m_owner = nullptr;
    }
    void hide_without_notify()
    {
        m_status_timer.Stop();
        m_dismissing = true;
        Hide();
        m_dismissing = false;
    }
    void dismiss(bool focus)
    {
        if (!IsShown() || m_dismissing) return;
        wxWeakRef<wxWindow> opener(m_owner ? m_owner->GetParent() : nullptr);
        hide_without_notify();
        if (m_owner) m_owner->OnDismiss();
        if (focus && opener && !opener->IsBeingDeleted() && opener->IsShownOnScreen()) opener->SetFocus();
    }
    void rebuild()
    {
        if (!m_owner) return;
        const wxString query = m_search->GetTextCtrl()->GetValue().Lower().Strip(wxString::both);
        const int old_index = m_active >= 0 && m_active < int(m_rows.size()) ? m_rows[m_active].index : m_owner->selection;
        m_rows.clear(); m_active = m_hover = -1;
        wxString pending_heading, last_group;
        for (int i = 0; i < int(m_owner->items.size()); ++i) {
            const auto& item = m_owner->items[i];
            if (i == m_owner->printer_footer_index) continue;
            if (item.style & DD_ITEM_STYLE_SPLIT_ITEM) { pending_heading = item.text; continue; }
            const wxString match = (item.text + " " + item.popup_label + " " + item.alias + " " + item.group_label + " " + item.text_static_tips).Lower();
            if (!query.empty() && !match.Contains(query)) continue;
            if (!pending_heading.empty()) { m_rows.push_back({-1, pending_heading}); pending_heading.clear(); last_group.clear(); }
            if (!item.group_label.empty() && item.group_key != last_group) {
                m_rows.push_back({-1, item.group_label}); last_group = item.group_key;
            }
            m_rows.push_back({i, {}});
            if (i == old_index) m_active = int(m_rows.size()) - 1;
        }
        if (!selectable(m_active)) { m_active = -1; step(1); }
        const int total = m_rows.empty() ? FromDIP(64) : row_y(int(m_rows.size()));
        m_list->SetVirtualSize(wxSize(std::max(1, m_list->GetClientSize().x), total));
        m_clear->Show(m_rows.empty() && !query.empty());
        m_edit->Show(bool(m_owner->printer_edit_action));
        m_manage->Show(bool(m_owner->printer_manage_action));
        Layout(); m_list->Refresh();
    }
    void refresh_style() { apply_theme(); }
    void open()
    {
        if (!m_owner || IsShown() || !m_owner->GetParent()->IsShownOnScreen()) return;
        apply_theme();
        m_search->GetTextCtrl()->ChangeValue("");
        rebuild();
        wxWindow* anchor = m_owner->printer_popup_anchor ? m_owner->printer_popup_anchor.get() : m_owner->GetParent();
        const wxRect screen = wxDisplay(anchor).GetClientArea();
        const int width = std::min(screen.width - FromDIP(16), std::max(anchor->GetSize().x, FromDIP(280)));
        const int footer = FromDIP(60 + (m_edit->IsShown() ? 34 : 0) + (m_manage->IsShown() ? 34 : 0));
        const int list_height = std::min(FromDIP(360), std::max(FromDIP(64), row_y(int(m_rows.size()))));
        const int height = std::min(screen.height - FromDIP(16), list_height + footer);
        SetMinSize(wxSize(width, height)); SetSize(wxSize(width, height));
#ifdef __WXGTK__
        // A reopened window was mapped at its previous size; let it shrink as well as grow.
        if (width > 0 && height > 0)
            gtk_window_resize(GTK_WINDOW(m_widget), width, height);
#endif
        Layout();
        wxPoint origin = anchor->ClientToScreen(wxPoint(0, anchor->GetSize().y + FromDIP(6)));
        if (origin.y + height > screen.GetBottom()) origin.y = anchor->ClientToScreen(wxPoint(0, 0)).y - height - FromDIP(6);
        origin.x = std::clamp(origin.x, screen.x, screen.GetRight() - width + 1);
        origin.y = std::clamp(origin.y, screen.y, screen.GetBottom() - height + 1);
        SetPosition(origin);
        if (auto* renderer = wxGraphicsRenderer::GetDefaultRenderer()) {
            auto shape = renderer->CreatePath();
            shape.AddRoundedRectangle(0, 0, width, height, FromDIP(12)); SetShape(shape);
        }
        ensure_visible();
        m_opening = true;
        Show();
        m_opening = false;
        if (std::any_of(m_owner->items.begin(), m_owner->items.end(), [](const auto& item) { return bool(item.status_provider); }))
            m_status_timer.Start(1000);
        m_search->GetTextCtrl()->SetFocus();
    }
};

DropDown::~DropDown()
{
    if (printer_popup) {
        printer_popup->hide_without_notify();
        printer_popup->detach();
        printer_popup->Destroy();
        printer_popup = nullptr;
    }
}

void DropDown::SetPrinterPopupStyle(bool enabled)
{
    if (printer_popup_style == enabled) return;
    if (IsPopupShown()) DismissAndNotify();
    printer_popup_style = enabled;
}

void DropDown::SetPrinterPopupActions(std::function<void()> edit, std::function<void()> manage)
{
    printer_edit_action = std::move(edit);
    printer_manage_action = std::move(manage);
}

bool DropDown::IsPopupShown() const
{
    return printer_popup_style ? printer_popup && printer_popup->IsShown() : IsShown();
}

wxDEFINE_EVENT(EVT_DISMISS, wxCommandEvent);
wxDEFINE_EVENT(EVT_DROPDOWN_ITEM_ACTION, wxCommandEvent);

BEGIN_EVENT_TABLE(DropDown, PopupWindow)

EVT_LEFT_DOWN(DropDown::mouseDown)
EVT_LEFT_UP(DropDown::mouseReleased)
EVT_MOUSE_CAPTURE_LOST(DropDown::mouseCaptureLost)
EVT_MOTION(DropDown::mouseMove)
EVT_MOUSEWHEEL(DropDown::mouseWheelMoved)

// catch paint events
EVT_PAINT(DropDown::paintEvent)

END_EVENT_TABLE()

/*
 * Called by the system of by wxWidgets when the panel needs
 * to be redrawn. You can also trigger this call by
 * calling Refresh()/Update().
 */

DropDown::DropDown(std::vector<Item> &items)
    : items(items)
    , state_handler(this)
    , border_color(0xDBDBDB)
    , text_color(std::make_pair(0x909090, (int) StateColor::Disabled),
        std::make_pair(0x363636, (int) StateColor::Normal))
    , selector_border_color(std::make_pair(0x009688, (int) StateColor::Hovered),
        std::make_pair(*wxWHITE, (int) StateColor::Normal))
    , selector_background_color(std::make_pair(0xBFE1DE, (int) StateColor::Checked), // ORCA updated background color for checked item
        std::make_pair(*wxWHITE, (int) StateColor::Normal))
{
}

DropDown::DropDown(wxWindow *parent, std::vector<Item> &items, long style)
    : DropDown(items)
{
    Create(parent, style);
}

void DropDown::Create(wxWindow *parent, long style)
{
    PopupWindow::Create(parent, wxPU_CONTAINS_CONTROLS);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetBackgroundColour(*wxWHITE);
    state_handler.attach({&border_color, &text_color, &selector_border_color, &selector_background_color});
    state_handler.update_binds();
    if ((style & DD_NO_CHECK_ICON) == 0)
        check_bitmap = ScalableBitmap(this, "checked", 16);
    arrow_bitmap = ScalableBitmap(this, "hms_arrow", 16);
    text_off = style & DD_NO_TEXT;

    // BBS set default font
    SetFont(Label::Body_14);
#ifdef __WXOSX__
    // PopupWindow releases mouse on idle, which may cause various problems,
    //  such as losting mouse move, and dismissing soon on first LEFT_DOWN event.
    Bind(wxEVT_IDLE, [] (wxIdleEvent & evt) {});
#endif
}

void DropDown::Invalidate(bool clear)
{
    if (clear) {
        selection = hover_item = -1;
        offset = wxPoint();
    }
    assert(selection < (int) items.size());
    need_sync = true;
    if (printer_popup && printer_popup->IsShown()) printer_popup->rebuild();
}

void DropDown::SetSelection(int n)
{
    if (n >= (int) items.size())
        n = -1;
    if (selection == n) return;
    selection = n;
    if (need_sync) { // for icon Size
        messureSize();
        need_sync = true;
    }
    if (subDropDown)
        subDropDown->SetSelection(n);
    if (printer_popup && printer_popup->IsShown()) printer_popup->rebuild();
    paintNow();
}

wxString DropDown::GetValue() const
{
    return selection >= 0 ? items[selection].text : wxString();
}

void DropDown::SetValue(const wxString &value)
{
    auto i    = std::find_if(items.begin(), items.end(), [&value](Item & item) { return item.text == value; });
    selection = i == items.end() ? -1 : std::distance(items.begin(), i);
}

void DropDown::SetCornerRadius(double radius)
{
    this->radius = radius;
    paintNow();
}

void DropDown::SetBorderColor(StateColor const &color)
{
    if (printer_popup) printer_popup->refresh_style();
    border_color = color;
    state_handler.update_binds();
    paintNow();
}

void DropDown::SetSelectorBorderColor(StateColor const &color)
{
    selector_border_color = color;
    state_handler.update_binds();
    paintNow();
}

void DropDown::SetTextColor(StateColor const &color)
{
    text_color = color;
    state_handler.update_binds();
    paintNow();
}

void DropDown::SetSelectorBackgroundColor(StateColor const &color)
{
    selector_background_color = color;
    state_handler.update_binds();
    paintNow();
}

void DropDown::SetUseContentWidth(bool use, bool limit_max_content_width)
{
    if (use_content_width == use)
        return;
    use_content_width = use;
    this->limit_max_content_width = limit_max_content_width;
    need_sync = true;
    messureSize();
}

void DropDown::SetAlignIcon(bool align) { align_icon = align; }

void DropDown::Rescale()
{
    if (printer_popup) printer_popup->refresh_style();
    need_sync = true;
}

bool DropDown::HasDismissLongTime()
{
    auto now = boost::posix_time::microsec_clock::universal_time();
    return !IsPopupShown() &&
        (now - dismissTime).total_milliseconds() >= 20;
}

void DropDown::Popup(wxWindow *focus)
{
    if (printer_popup_style) {
        if (!printer_popup) printer_popup = new PrinterDropDownDialog(this);
        printer_popup->open();
        if (!printer_popup->IsShown()) OnDismiss();
        return;
    }
#ifdef __WXGTK__
    if (!mainDropDown && m_widget) {
        // Data-view cell editors can receive focus before wxGTK infers a
        // native popup parent, so provide the current toplevel explicitly.
        GtkWindow *transient_parent = nullptr;
        for (wxWindow *win = GetParent(); win; win = win->GetParent()) {
            GtkWidget *widget = static_cast<GtkWidget *>(win->GetHandle());
            if (!widget)
                continue;
            GtkWidget *top = gtk_widget_get_toplevel(widget);
            if (GTK_IS_WINDOW(top)) {
                transient_parent = GTK_WINDOW(top);
                break;
            }
        }
        if (transient_parent)
            gtk_window_set_transient_for(GTK_WINDOW(m_widget), transient_parent);
    }
#endif
    PopupWindow::Popup(focus);
}

void DropDown::paintEvent(wxPaintEvent& evt)
{
    // depending on your system you may need to look at double-buffered dcs
    wxBufferedPaintDC dc(this);
    render(dc);
}

/*
 * Alternatively, you can use a clientDC to paint on the panel
 * at any time. Using this generally does not free you from
 * catching paint events, since it is possible that e.g. the window
 * manager throws away your drawing when the window comes to the
 * background, and expects you will redraw it when the window comes
 * back (by sending a paint event).
 */
void DropDown::paintNow()
{
    // depending on your system you may need to look at double-buffered dcs
    //wxClientDC dc(this);
    //render(dc);
    Refresh();
}

static wxSize GetBmpSize(wxBitmap & bmp)
{
    if (!bmp.IsOk())
        return wxSize(0, 0);
#ifdef __APPLE__
    return bmp.GetScaledSize();
#else
    return bmp.GetSize();
#endif
}

// The x-range [first, second) of the column at the right of the rows that holds the items' action
// icons, empty when no item has one. render() draws the icons in it and mouseReleased() hit-tests
// clicks against it, so the drawn icon and its clickable area always match.
std::pair<int, int> DropDown::actionColumn() const
{
    if (actionIconWidth <= 0)
        return {0, 0};
    int right = rowSize.x;
    if (rowSize.y * count > GetSize().y) // position bar
        right -= 6;
    return {right - actionIconWidth - FromDIP(16), right};
}

static void _DrawSplitItem(const wxWindow* w, wxDC& dc, wxString split_text, wxPoint start_pt, int item_width, int item_height)
{
    // save dc
    auto pre_clr = dc.GetTextForeground();
    auto pre_pen = dc.GetPen();
    dc.SetTextForeground(wxColour(172, 172, 172));//GRAY 500
    dc.SetPen(wxColour(166, 169, 170));//GRAY 400
    // miner font
    auto font = w->GetFont();
    font.SetPointSize(font.GetPointSize() - 3);
    dc.SetFont(font);

    int spacing = w->FromDIP(8);

    if (!split_text.empty())// Paiting: text + spacing + line + spacing
    {
        int max_content_width = item_width - start_pt.x - 2 * spacing;
        wxSize tSize = dc.GetMultiLineTextExtent(split_text);
        if (tSize.x > max_content_width)
        {
            split_text = wxControl::Ellipsize(split_text, dc, wxELLIPSIZE_END, max_content_width);
            tSize = dc.GetMultiLineTextExtent(split_text);
        }

        dc.SetFont(font);
        dc.DrawText(split_text, start_pt);

        int line_width = item_width - start_pt.x - tSize.x - 2 * spacing;
        int line_y = start_pt.y + (tSize.GetHeight() / 2);
        dc.DrawLine(start_pt.x + tSize.x + spacing, line_y, start_pt.x + tSize.x + line_width + spacing, line_y);// draw right line
    }
    else// Paiting: line + spacing
    {
        int line_y = start_pt.y + (item_height / 2);
        int line_width = item_width - start_pt.x - spacing;
        dc.DrawLine(start_pt.x, line_y, start_pt.x + line_width, line_y);// draw line
    }

    // restore dc
    dc.SetTextForeground(pre_clr);
    dc.SetPen(pre_pen);
    dc.SetFont(w->GetFont());
}

/*
 * Here we do the actual rendering. I put it in a separate
 * method so that it can work no matter what type of DC
 * (e.g. wxPaintDC or wxClientDC) is used.
 */
void DropDown::render(wxDC &dc)
{
    if (items.size() == 0) return;
    int states = state_handler.states();
    if (subDropDown)
        states |= subDropDown->state_handler.states();
    dc.SetPen(wxPen(border_color.colorForStates(states)));
    dc.SetBrush(wxBrush(StateColor::darkModeColorFor(GetBackgroundColour())));
    // if (GetWindowStyle() & wxBORDER_NONE)
    //    dc.SetPen(wxNullPen);

    // draw background
    wxSize size = GetSize();
    if (radius == 0)
        dc.DrawRectangle(0, 0, size.x, size.y);
    else
        dc.DrawRoundedRectangle(0, 0, size.x, size.y, radius);

    int selected_item = selectedItem();
    int hover_index   = hoverIndex();

    // draw hover rectangle
    wxRect rcContent = {{0, offset.y}, rowSize};
    if (hover_item >= 0 && (states & StateColor::Hovered) && (hover_index < 0 || !(items[hover_index].style & DD_ITEM_STYLE_SPLIT_ITEM))) {
        rcContent.y += rowSize.y * hover_item;
        if (rcContent.GetBottom() > 0 && rcContent.y < size.y) {
            if (selected_item == hover_item)
                dc.SetBrush(wxBrush(selector_background_color.colorForStates(states | StateColor::Checked)));
            dc.SetPen(wxPen(selector_border_color.colorForStates(states)));
            rcContent.Deflate(4, 1);
            dc.DrawRectangle(rcContent);
            rcContent.Inflate(4, 1);
        }
        rcContent.y = offset.y;
    }
    // draw checked rectangle
    if (selected_item >= 0 && (selected_item != hover_item || (states & StateColor::Hovered) == 0)) {
        rcContent.y += rowSize.y * selected_item;
        if (rcContent.GetBottom() > 0 && rcContent.y < size.y) {
            dc.SetBrush(wxBrush(selector_background_color.colorForStates(states | StateColor::Checked)));
            dc.SetPen(wxPen(selector_background_color.colorForStates(states)));
            rcContent.Deflate(4, 1);
            dc.DrawRectangle(rcContent);
            rcContent.Inflate(4, 1);
        }
        rcContent.y = offset.y;
    }
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    {
        wxSize offset = (rowSize - textSize) / 2;
        rcContent.Deflate(0, offset.y);
    }

    // draw position bar
    if (rowSize.y * count > size.y) {
        int    height = rowSize.y * count;
        wxRect rect = {size.x - 6, -offset.y * size.y / height, 4,
                       size.y * size.y / height};
        dc.SetPen(wxPen(border_color.defaultColor()));
        dc.SetBrush(wxBrush(*wxLIGHT_GREY));
        dc.DrawRoundedRectangle(rect, 2);
        rcContent.width -= 6;
    }

    // draw check icon
    rcContent.x += 5;
    rcContent.width -= 5;
    if (check_bitmap.bmp().IsOk()) {
        auto szBmp = check_bitmap.GetBmpSize();
        if (selected_item >= 0) {
            wxPoint pt = rcContent.GetLeftTop();
            pt.y += (rcContent.height - szBmp.y) / 2;
            pt.y += rowSize.y * selected_item;
            if (pt.y + szBmp.y > 0 && pt.y < size.y)
                dc.DrawBitmap(check_bitmap.bmp(), pt);
        }
        rcContent.x += szBmp.x + 5;
        rcContent.width -= szBmp.x + 5;
    }

    const auto action_column = actionColumn();
    if (action_column.second > action_column.first)
        rcContent.width = action_column.first - rcContent.x;

    std::set<wxString> groups;
    // draw texts & icons
    int index = 0;
    for (int i = 0; i < items.size(); ++i) {
        auto &item = items[i];
        int states2 = states;
        if ((item.style & DD_ITEM_STYLE_DISABLED) != 0)
            states2 &= ~StateColor::Enabled;
        // Skip by group
        if (group.IsEmpty()) {
            if (!item.group_key.IsEmpty()) {
                if (groups.find(item.group_key) != groups.end())
                    continue;
                groups.insert(item.group_key);
                if (!item.group_key.IsEmpty()) {
                    bool disabled = true;
                    for (int j = i + 1; j < items.size(); ++j) {
                        if (items[j].group_key == item.group_key && (items[j].style & DD_ITEM_STYLE_DISABLED) == 0) {
                            disabled = false;
                            break;
                        }
                    }
                    if (!disabled)
                        states2 |= StateColor::Enabled;
                }
            }
        } else {
            if (item.group_key != group)
                continue;
        }
        bool is_hover = index == hover_item;
        ++index;
        if (rcContent.GetBottom() < 0) {
            rcContent.y += rowSize.y;
            continue;
        }
        if (rcContent.y > size.y) break;
        wxPoint pt   = rcContent.GetLeftTop();

        if (item.style & DD_ITEM_STYLE_SPLIT_ITEM) {
            _DrawSplitItem(this, dc, item.text, pt, rowSize.GetWidth(), rowSize.GetHeight());
            rcContent.y += rowSize.GetHeight();
            continue;
        }

        auto &  icon  = item.icon;
        auto size2 = GetBmpSize(icon);
        if (iconSize.x > 0) {
            if (icon.IsOk()) {
                pt.y += (rcContent.height - size2.y) / 2;
                dc.DrawBitmap(icon, pt);
            }
            pt.x += iconSize.x + 5;
            pt.y = rcContent.y;
        } else if (icon.IsOk()) {
            pt.y += (rcContent.height - size2.y) / 2;
            dc.DrawBitmap(icon, pt);
            pt.x += size2.x + 5;
            pt.y = rcContent.y;
        }
        auto text = group.IsEmpty()
                        ? (item.group_key.IsEmpty() ? item.text : item.group_label)
                        : item.text;
        if (item.action_icon.IsOk()) {
            auto action_size = GetBmpSize(item.action_icon);
            wxPoint action_pt(action_column.second - action_size.x - FromDIP(8),
                              rcContent.y + (rcContent.height - action_size.y) / 2);
            dc.DrawBitmap(item.action_icon, action_pt);
        }

        if (!text_off && !text.IsEmpty()) {
            wxSize tSize = dc.GetMultiLineTextExtent(text);
            if (pt.x + tSize.x > rcContent.GetRight()) {
                int max_text_width = rcContent.GetRight() - pt.x;
                if (max_text_width <= 0) {
                    text.clear();
                } else {
                    if (is_hover && item.tip.IsEmpty())
                        SetToolTip(text);
                    text = wxControl::Ellipsize(text, dc, wxELLIPSIZE_END, max_text_width);
                }
            }
            pt.y += (rcContent.height - textSize.y) / 2;
            dc.SetFont(GetFont());
            // Dimmed items stay selectable, so they only borrow the disabled text tone rather
            // than taking the disabled state itself.
            const int text_states = (item.style & DD_ITEM_STYLE_DIMMED) ? (states2 & ~StateColor::Enabled) : states2;
            dc.SetTextForeground(text_color.colorForStates(text_states));
            dc.DrawText(text, pt);
            if (group.IsEmpty() && !item.group_key.IsEmpty()) {
                auto szBmp = arrow_bitmap.GetBmpSize();
                pt.x = rcContent.GetRight() - szBmp.x - 5;
                pt.y = rcContent.y + (rcContent.height - szBmp.y) / 2;
                dc.DrawBitmap(arrow_bitmap.bmp(), pt);
            }
        }
        rcContent.y += rowSize.y;
    }
}

int DropDown::hoverIndex()
{
    if (hover_item < 0)
        return -1;
    // BUG FIX: Can't take the shortcut if subDropDown exists (which means there are groups)
    // because we need to detect group headers which return negative indices
    if (count == items.size() && subDropDown == nullptr)
        return hover_item;
    int index = -1;
    std::set<wxString> groups;
    for (int i = 0; i < items.size(); ++i) {
        auto &item = items[i];
        // Skip by group
        if (group.IsEmpty()) {
            if (!item.group_key.IsEmpty()) {
                if (groups.find(item.group_key) == groups.end())
                    groups.insert(item.group_key);
                else
                    continue;
            }
        } else {
            if (item.group_key != group)
                continue;
        }
        if (++index == hover_item)
            return (item.group_key.IsEmpty() || !group.IsEmpty()) ? i : -i - 2;
    }
    return -1;
}

int DropDown::selectedItem()
{
    if (selection < 0)
        return -1;
    // BUG FIX: Can't take the shortcut if subDropDown exists (which means there are groups)
    // because the visual position differs from the actual item index when groups are shown
    if (count == items.size() && subDropDown == nullptr)
        return selection;
    auto & sel = items[selection];
    if (group.IsEmpty() ? !sel.group_key.IsEmpty() : sel.group_key != group)
        return -1;
    if (selection == 0)
        return 0;
    int                index = 0;
    std::set<wxString> groups;
    for (size_t i = 0; i < selection; ++i) {
        auto &item = items[i];
        // Skip by group
        if (group.IsEmpty()) {
            if (!item.group_key.IsEmpty()) {
                if (groups.find(item.group_key) == groups.end())
                    groups.insert(item.group_key);
                else
                    continue;
            }
        } else {
            if (item.group_key != group)
                continue;
        }
        ++index;
    }
    return index;
}

void DropDown::messureSize()
{
    if (printer_popup_style) return;
    if (!need_sync) return;
    textSize = wxSize();
    iconSize = wxSize();
    actionIconWidth = 0;
    count = 0;
    wxClientDC dc(GetParent() ? GetParent() : this);
    dc.SetFont(GetFont());
    std::set<wxString> groups;
    for (size_t i = 0; i < items.size(); ++i) {
        auto &item = items[i];
        // Skip by group
        if (group.IsEmpty()) {
            if (!item.group_key.IsEmpty()) {
                if (groups.find(item.group_key) == groups.end())
                    groups.insert(item.group_key);
                else
                    continue;
            }
        } else {
            if (item.group_key != group)
                continue;
        }
        ++count;
        wxSize size1;
        if (!text_off) {
            auto text = group.IsEmpty()
                        ? (item.group_key.IsEmpty() ? item.text : item.group_label)
                        : item.text;
            size1 = dc.GetMultiLineTextExtent(text);
            if (group.IsEmpty() && !item.group_key.IsEmpty())
                size1.x += 5 + arrow_bitmap.GetBmpWidth();
        }
        if (item.icon.IsOk()) {
            wxSize size2 = GetBmpSize(item.icon);
            if (size2.x > iconSize.x)
                iconSize = size2;
            if (!align_icon) {
                size1.x += size2.x + (text_off ? 0 : 5);
            }
        }
        if (item.action_icon.IsOk())
            actionIconWidth = std::max(actionIconWidth, GetBmpSize(item.action_icon).x);
        if (size1.x > textSize.x) textSize = size1;
    }
    // Every row leaves room for the action column, as render() and actionColumn() lay it out.
    if (actionIconWidth > 0)
        textSize.x += actionIconWidth + FromDIP(16);
    if (!align_icon) iconSize.x = 0;
    wxSize szContent = textSize;
    if (szContent.x < FromDIP(120) && !use_content_width)
        szContent.x = FromDIP(120);
    szContent.x += 10;
    if (check_bitmap.bmp().IsOk()) {
        auto szBmp = check_bitmap.GetBmpSize();
        szContent.x += szBmp.x + 5;
    }
    if (iconSize.x > 0) szContent.x += iconSize.x + (text_off ? 0 : 5);
    if (iconSize.y > szContent.y) szContent.y = iconSize.y;
    szContent.y += 10;
    if (count > 15) szContent.x += 6;
    if (GetParent() && group.IsEmpty()) {
        auto x = GetParent()->GetSize().x;
        if (x > 0 && (!use_content_width || x > szContent.x))
            szContent.x = x;
    }
    rowSize = szContent;
    if (limit_max_content_width) {
        wxSize parent_size = GetParent()->GetSize();
        if (rowSize.x > parent_size.x * 2) {
            rowSize.x = 2 * parent_size.x;
            szContent = rowSize;
        }
    }
    szContent.y *= std::min((size_t) 15, std::max(count, (size_t) 1));
    szContent.y += items.size() > 15 ? rowSize.y / 2 : 0;
    wxWindow::SetSize(szContent);
#ifdef __WXGTK__
    // Gtk has a wrapper window for popup widget
    if (szContent.x > 0 && szContent.y > 0)
        gtk_window_resize (GTK_WINDOW (m_widget), szContent.x, szContent.y);
#endif
    if (!groups.empty() && subDropDown == nullptr) {
        subDropDown = new DropDown(items);
        subDropDown->mainDropDown = this;
        subDropDown->check_bitmap      = check_bitmap;
        subDropDown->text_off          = text_off;
        subDropDown->use_content_width = true;
        subDropDown->Create(GetParent());
#ifdef __WXGTK__
        // Orca: On Wayland, while the sub holds an xdg_popup grab, motion events for
        // the cursor over main may not be delivered (Mutter drops motion
        // outside the grabbing surface). Poll on idle and synthesize a
        // mouseMove on main so its hover highlight tracks and it can dismiss
        // the sub when the cursor leaves the parent (group) item.
        DropDown* sub = subDropDown;
        sub->Bind(wxEVT_IDLE, [sub](wxIdleEvent& e) {
            e.Skip();
            if (!sub->IsShown() || !sub->mainDropDown->IsShown())
                return;
            wxPoint screen_pt = wxGetMousePosition();
            if (sub->GetScreenRect().Contains(screen_pt) || !sub->mainDropDown->GetScreenRect().Contains(screen_pt))
                return;
            wxPoint main_pt = sub->mainDropDown->ScreenToClient(screen_pt);
            wxMouseEvent ev(wxEVT_MOTION);
            ev.SetEventObject(sub->mainDropDown);
            ev.m_x = main_pt.x;
            ev.m_y = main_pt.y;
            sub->mainDropDown->mouseMove(ev);
            e.RequestMore();
        });
#endif
        subDropDown->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
            e.SetEventObject(this);
            e.SetId(GetId());
            GetEventHandler()->ProcessEvent(e);
        });
        subDropDown->Bind(EVT_DROPDOWN_ITEM_ACTION, [this](wxCommandEvent &e) {
            e.SetEventObject(this);
            e.SetId(GetId());
            GetEventHandler()->ProcessEvent(e);
        });
    }
    need_sync = false;
}

void DropDown::autoPosition()
{
    if (printer_popup_style) return;
    messureSize();
    wxPoint pos;
    wxSize  off;
    if (mainDropDown) {
        pos = mainDropDown->ClientToScreen(wxPoint(0, 0));
        off = mainDropDown->GetSize();
        pos.x += 6;
        pos.y += mainDropDown->hover_item * rowSize.y + rowSize.y + mainDropDown->offset.y;
        off.x -= 12;
        off.y = -rowSize.y;
    } else {
        pos = GetParent()->ClientToScreen(wxPoint(0, 0));
        off = GetParent()->GetSize();
        pos.y -= 6;
        off.x = 0;
        off.y += 12;
    }
    wxPoint old = GetPosition();
    wxSize size = GetSize();
    Position(pos, off);
    if (old != GetPosition()) {
        size = rowSize;
        size.y *= std::min((size_t) 15, count);
        size.y += count > 15 ? rowSize.y / 2 : 0;
        if (size != GetSize()) {
            wxWindow::SetSize(size);
            offset = wxPoint();
            Position(pos, off);
        }
    }
    if (GetPosition().y > pos.y) {
        // may exceed
        auto drect = wxDisplay(GetParent()).GetGeometry();
        if (GetPosition().y + size.y + 10 > drect.GetBottom()) {
            int available_height = drect.GetBottom() - GetPosition().y - 10;
            if (available_height < rowSize.y * 2)
                return;
            if (use_content_width && count <= 15) size.x += 6;
            size.y = available_height;
            wxWindow::SetSize(size);
            if (selection >= 0) {
                if (offset.y + rowSize.y * (selection + 1) > size.y)
                    offset.y = size.y - rowSize.y * (selection + 1);
                else if (offset.y + rowSize.y * selection < 0)
                    offset.y = -rowSize.y * selection;
            }
        }
    }
}

void DropDown::mouseDown(wxMouseEvent& event)
{
    // Receivce unexcepted LEFT_DOWN on Mac after OnDismiss
    if (!IsShown())
        return;
    // force calc hover item again
    mouseMove(event);
    pressedDown = true;
    CaptureMouse();
    dragStart   = event.GetPosition();
}

void DropDown::mouseReleased(wxMouseEvent& event)
{
    if (pressedDown) {
        dragStart = wxPoint();
        pressedDown = false;
        if (HasCapture())
            ReleaseMouse();
        if (hover_item < 0)
            return;
        if (hover_item >= 0 && (subDropDown == nullptr || subDropDown->group.empty())) { // not moved
            int index = hoverIndex();
            if (index >= 0 && items[index].action_icon.IsOk()) {
                const auto action_column = actionColumn();
                if (event.GetX() >= action_column.first && event.GetX() < action_column.second) {
                    sendDropDownActionEvent(index);
                    paintNow();
                    return;
                }
            }
            sendDropDownEvent();
            if (mainDropDown)
                mainDropDown->hover_item = -1; // To Dismiss mainDropDown
            DismissAndNotify();
        } else if (subDropDown)
            subDropDown->Popup(subDropDown);
    }
}

void DropDown::mouseCaptureLost(wxMouseCaptureLostEvent &event)
{
    wxMouseEvent evt;
    mouseReleased(evt);
}

void DropDown::mouseMove(wxMouseEvent &event)
{
    wxPoint pt  = event.GetPosition();
#ifdef __WXOSX__
    if (mainDropDown) {
        auto size = GetSize();
        if (pt.x < 0 || pt.y < 0 || pt.x >= size.x || pt.y >= size.y) {
            auto diff = GetPosition() - mainDropDown->GetPosition();
            event.SetX(pt.x + diff.x);
            event.SetY(pt.y + diff.y);
            mainDropDown->mouseMove(event);
            return;
        }
    }
#endif
    if (pressedDown) {
        wxPoint pt2 = offset + pt - dragStart;
        wxSize  size = GetSize();
        dragStart    = pt;
        if (pt2.y > 0)
            pt2.y = 0;
        else if (pt2.y + rowSize.y * int(count) < size.y)
            pt2.y = size.y - rowSize.y * int(count);
        if (pt2.y != offset.y) {
            offset = pt2;
            hover_item = -1; // moved
        } else {
            return;
        }
    }
    if (!pressedDown || hover_item >= 0) {
        int hover = (pt.y - offset.y) / rowSize.y;
        if (hover >= (int) count) hover = -1;
        if (hover == hover_item) return;
        hover_item = hover;
        int index  = hoverIndex();
        if (index < -1) {
            auto & drop = *subDropDown;
            drop.group  = items[-index - 2].group_key;
            drop.need_sync = true;
            drop.messureSize();
#ifdef __WXGTK__
            // wxGTK wraps popup contents in a native GtkWindow. Make the submenu
            // transient for the currently mapped parent popup window before
            // positioning/showing it, so wlroots/Hyprland sees the topmost parent.
            if (m_widget && drop.m_widget)
                gtk_window_set_transient_for(GTK_WINDOW(drop.m_widget), GTK_WINDOW(m_widget));
#endif
            drop.autoPosition();
            drop.paintNow();
            if (!drop.IsShown())
                drop.Popup(&drop);
        } else if (index >= 0) {
            if (subDropDown) {
                subDropDown->group.clear();
                if (subDropDown->IsShown())
                    subDropDown->Dismiss();
            }
            SetToolTip(items[index].tip);
        }
    }
    paintNow();
}

void DropDown::mouseWheelMoved(wxMouseEvent &event)
{
    auto delta = event.GetWheelRotation();
    wxSize  size  = GetSize();
    wxPoint pt2   = offset + wxPoint{0, delta};
    if (pt2.y > 0)
        pt2.y = 0;
    else if (pt2.y + rowSize.y * int(count) < size.y)
        pt2.y = size.y - rowSize.y * int(count);
    if (pt2.y != offset.y) {
        offset = pt2;
    } else {
        return;
    }
    int hover = (event.GetPosition().y - offset.y) / rowSize.y;
    if (hover >= (int) count) hover = -1;
    if (hover != hover_item) {
        hover_item = hover;
        if (auto index = hoverIndex(); index >= 0)
            SetToolTip(items[index].tip);
    }
    paintNow();
}

// currently unused events
void DropDown::sendDropDownEvent()
{
    int index = hoverIndex();
    if (index < 0 || (items[index].style & DD_ITEM_STYLE_DISABLED))
        return;
    wxCommandEvent event(wxEVT_COMBOBOX, GetId());
    event.SetEventObject(this);
    event.SetInt(index);
    event.SetString(items[index].text);
    GetEventHandler()->ProcessEvent(event);
}

void DropDown::sendDropDownActionEvent(int index)
{
    if (index < 0 || index >= int(items.size()) || (items[index].style & DD_ITEM_STYLE_DISABLED))
        return;
    wxCommandEvent event(EVT_DROPDOWN_ITEM_ACTION, GetId());
    event.SetEventObject(this);
    event.SetInt(index);
    event.SetString(items[index].text);
    GetEventHandler()->ProcessEvent(event);
}

void DropDown::Dismiss()
{
    if (printer_popup_style) {
        // DismissAndNotify supplies OnDismiss exactly once to the normal ComboBox relay.
        if (printer_popup) printer_popup->hide_without_notify();
        return;
    }
    if (subDropDown && subDropDown->IsShown())
        return;
    PopupWindow::Dismiss();
}

bool DropDown::ShouldDismissOnTopWindowDeactivate()
{
    // On Wayland, mapping a chained xdg_popup with grab makes the parent
    // toplevel inactive, which would otherwise cascade-dismiss the whole
    // chain. Skip when our chain peer is shown.
    return !((mainDropDown && mainDropDown->IsShown()) ||
             (subDropDown  && subDropDown->IsShown()));
}

void DropDown::OnDismiss()
{
    if (mainDropDown) {

        const wxPoint& mouse_pos = wxGetMousePosition();
        if (!mainDropDown->GetScreenRect().Contains(mouse_pos))
            mainDropDown->DismissAndNotify();
        else
#ifdef __WIN32__
            SetActiveWindow(mainDropDown->GetHandle());
#else
            ;
#endif
        return;
    }
    if (subDropDown && subDropDown->IsShown())
        return;
    dismissTime = boost::posix_time::microsec_clock::universal_time();
    hover_item  = -1;
    wxCommandEvent e(EVT_DISMISS);
    GetEventHandler()->ProcessEvent(e);
}
