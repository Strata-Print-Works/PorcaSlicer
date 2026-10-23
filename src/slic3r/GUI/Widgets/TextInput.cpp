#include "TextInput.hpp"
#include "TextCtrl.h"
#include "slic3r/GUI/Widgets/Label.hpp"

#include "slic3r/GUI/Widgets/StaticBox.hpp"
#include <utility>
#include <cassert>
#include "slic3r/GUI/Widgets/StateHandler.hpp"
#include <wx/containr.h>
#include <wx/dc.h>
#include <algorithm>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/brush.h>
#include <wx/control.h>
#include <wx/font.h>
#include <wx/pen.h>
#include <wx/graphics.h>
#include <wx/string.h>
#include <wx/gdicmn.h>
#include <wx/textctrl.h>
#include <wx/event.h>
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#ifdef __WXGTK__
#include "../GUI_Utils.hpp"
#endif

BEGIN_EVENT_TABLE(TextInput, StaticBox)

EVT_PAINT(TextInput::paintEvent)

END_EVENT_TABLE()

/*
 * Called by the system of by wxWidgets when the panel needs
 * to be redrawn. You can also trigger this call by
 * calling Refresh()/Update().
 */

TextInput::TextInput()
    : label_color(std::make_pair(0x6B6B6B, (int) StateColor::Disabled),
                 std::make_pair(0x6B6B6B, (int) StateColor::Normal))
    , text_color(std::make_pair(0x6B6B6B, (int) StateColor::Disabled),
                 std::make_pair(0x262E30, (int) StateColor::Normal))
{
    radius = 0;
    border_width = 1;
    border_color = StateColor(std::make_pair(0xDBDBDB, (int) StateColor::Disabled), std::make_pair(0x009688, (int) StateColor::Hovered),
                              std::make_pair(0xDBDBDB, (int) StateColor::Normal));
    background_color = StateColor(std::make_pair(0xF0F0F1, (int) StateColor::Disabled), std::make_pair(*wxWHITE, (int) StateColor::Normal));
    SetFont(Label::Body_12);
}

TextInput::TextInput(wxWindow *     parent,
                     wxString       text,
                     wxString       label,
                     wxString       icon,
                     const wxPoint &pos,
                     const wxSize & size,
                     long           style)
    : TextInput()
{
    Create(parent, text, label, icon, pos, size, style);
}

void TextInput::Create(wxWindow *     parent,
                       wxString       text,
                       wxString       label,
                       wxString       icon,
                       const wxPoint &pos,
                       const wxSize & size,
                       long           style)
{
        text_ctrl = nullptr;
    StaticBox::Create(parent, wxID_ANY, pos, size, style);
    wxWindow::SetLabel(label);
    assert((style & wxRIGHT) == 0);
    style &= ~wxALIGN_MASK;
    state_handler.attach({&label_color, & text_color});
    state_handler.update_binds();
    text_ctrl = new TextCtrl(this, wxID_ANY, text, {4, 4}, wxDefaultSize, style | wxBORDER_NONE | wxTE_PROCESS_ENTER);

#ifdef __WXGTK__
    Slic3r::GUI::RemoveInputBorder(text_ctrl);
#endif

    text_ctrl->SetFont(Label::Body_14);
    text_ctrl->SetInitialSize(text_ctrl->GetBestSize());
    text_ctrl->SetBackgroundColour(background_color.colorForStates(state_handler.states()));
    text_ctrl->SetForegroundColour(text_color.colorForStates(state_handler.states()));
    state_handler.attach_child(text_ctrl);
    text_ctrl->Bind(wxEVT_KILL_FOCUS, [this](auto &e) {
        OnEdit();
        e.SetId(GetId());
        ProcessEventLocally(e);
        e.Skip();
    });
    text_ctrl->Bind(wxEVT_TEXT_ENTER, [this](auto &e) {
        OnEdit();
        e.SetId(GetId());
        ProcessEventLocally(e);
    });
    text_ctrl->Bind(wxEVT_RIGHT_DOWN, [](auto &e) {}); // disable context menu
    if (!icon.IsEmpty()) {
        this->icon = ScalableBitmap(this, icon.ToStdString(), 16);
    }
    messureSize();
}

void TextInput::SetCornerRadius(double radius)
{
    this->radius = radius;
    Refresh();
}

void TextInput::SetLabel(const wxString& label)
{
    wxWindow::SetLabel(label);
    messureSize();
    Refresh();
}

void TextInput::SetStaticTips(const wxString& tips, const wxBitmap& bitmap)
{
    static_tips = tips;
    static_tips_icon = bitmap;
    messureSize();
    Refresh();
}

void TextInput::SetIcon(const wxBitmap &icon)
{
    this->icon = ScalableBitmap();
    this->icon.bmp() = icon;
    Rescale();
}

void TextInput::SetIcon(const wxString &icon)
{
    if (this->icon.name() == icon.ToStdString())
        return;
    this->icon = ScalableBitmap(this, icon.ToStdString(), 16);
    Rescale();
}

void TextInput::SetIcon_1(const wxString &icon) {
    if (this->icon_1.name() == icon.ToStdString())
        return;
    if (icon.empty()) {
        this->icon_1 = ScalableBitmap();
        return;
    }
    this->icon_1 = ScalableBitmap(this, icon.ToStdString(), 14);
    Rescale();
}

// Set icon_1 from a raw bitmap. Note: won't auto-rescale on DPI change
// since ScalableBitmap::name() will be empty. Caller should re-set after DPI change.
void TextInput::SetIcon_1(const wxBitmap &icon) {
    this->icon_1 = ScalableBitmap();
    if (icon.IsOk())
        this->icon_1.bmp() = icon;
    Rescale();
}

void TextInput::SetLabelColor(StateColor const &color)
{
    label_color = color;
    state_handler.update_binds();
}

void TextInput::SetTextColor(StateColor const& color)
{
    text_color= color;
    state_handler.update_binds();
    if (m_sidebar_style && text_ctrl)
        text_ctrl->SetForegroundColour(text_color.colorForStates(state_handler.states()));
}

void TextInput::SetSidebarStyle(bool enabled)
{
    m_sidebar_style = enabled;
    // The sidebar field paints its whole client area through a buffered DC.
    SetBackgroundStyle(enabled ? wxBG_STYLE_PAINT : wxBG_STYLE_ERASE);
    if (enabled && text_ctrl) {
        text_ctrl->SetFont(GetFont());
        text_ctrl->SetBackgroundColour(background_color.colorForStates(state_handler.states()));
        text_ctrl->SetForegroundColour(text_color.colorForStates(state_handler.states()));
    }
    messureSize();
    const wxSize size = GetSize();
    DoSetSize(-1, -1, size.x, size.y);
    Refresh();
}

void TextInput::SetSidebarItemIcons(bool show)
{
    m_sidebar_item_icons = show;
    if (m_sidebar_style) {
        const wxSize size = GetSize();
        DoSetSize(-1, -1, size.x, size.y);
        Refresh();
    }
}

bool TextInput::SetFont(const wxFont& font)
{
    const bool changed = wxWindow::SetFont(font);
    if (m_sidebar_style && text_ctrl) {
        text_ctrl->SetFont(font);
        text_ctrl->InvalidateBestSize();
        messureSize();
        const wxSize size = GetSize();
        DoSetSize(-1, -1, size.x, size.y);
        Refresh();
    }
    return changed;
}

void TextInput::Rescale()
{
    if (!this->icon.name().empty())
        this->icon.msw_rescale();
    if (!this->icon_1.name().empty())
        this->icon_1.msw_rescale();
    messureSize();
    Refresh();
}

bool TextInput::Enable(bool enable)
{
    bool result = text_ctrl->Enable(enable) && wxWindow::Enable(enable);
    if (result) {
        wxCommandEvent e(EVT_ENABLE_CHANGED);
        e.SetEventObject(this);
        GetEventHandler()->ProcessEvent(e);
        text_ctrl->SetBackgroundColour(background_color.colorForStates(state_handler.states()));
        text_ctrl->SetForegroundColour(text_color.colorForStates(state_handler.states()));
    }
    return result;
}

void TextInput::SetMinSize(const wxSize& size)
{
    wxSize size2 = size;
    if (size2.y < 0) {
#ifdef __WXMAC__
        if (GetPeer()) // peer is not ready in Create on mac
#endif
        size2.y = GetSize().y;
    }
    wxWindow::SetMinSize(size2);
}

void TextInput::DoSetSize(int x, int y, int width, int height, int sizeFlags)
{
    wxWindow::DoSetSize(x, y, width, height, sizeFlags);
    if (sizeFlags & wxSIZE_USE_EXISTING) return;
    wxSize size = GetSize();
    if (m_sidebar_style && text_ctrl) {
        const int left = sidebar_content_left();
        int right = FromDIP(10);
        if (m_sidebar_dropdown && m_sidebar_drop_arrow)
            right += FromDIP(22);
        else if (!m_sidebar_dropdown && !wxWindow::GetLabel().empty())
            right += GetTextExtent(wxWindow::GetLabel()).x + FromDIP(8);
        const int editor_height = std::min(text_ctrl->GetBestSize().y, std::max(1, size.y - FromDIP(4)));
        text_ctrl->SetSize(left, (size.y - editor_height) / 2, std::max(1, size.x - left - right), editor_height);
        return;
    }
    wxPoint textPos = {5, 0};
    if (this->icon.bmp().IsOk()) {
        wxSize szIcon = this->icon.GetBmpSize();
        textPos.x += szIcon.x;
    }
    if (this->icon_1.bmp().IsOk()) {
        wxSize szIcon = this->icon_1.GetBmpSize();
        textPos.x += (szIcon.x);
    }
    bool align_right = GetWindowStyle() & wxALIGN_RIGHT;
    if (align_right)
        textPos.x += labelSize.x;
    if (text_ctrl) {
        wxSize textSize = text_ctrl->GetSize();
        textSize.x = size.x - textPos.x - labelSize.x - 10;
        if(textSize.x < -1) textSize.x = -1;
        text_ctrl->SetSize(textSize);
        text_ctrl->SetPosition({textPos.x, (size.y - textSize.y) / 2});
    }
}

void TextInput::DoSetToolTipText(wxString const &tip)
{
    wxWindow::DoSetToolTipText(tip);
    text_ctrl->SetToolTip(tip);
}

void TextInput::paintEvent(wxPaintEvent &evt)
{
    if (m_sidebar_style) {
        // Buffered on MSW so the rounded field does not flicker; native paint DC elsewhere.
        wxAutoBufferedPaintDC dc(this);
        wxGCDC graphics_dc(dc);
        render_sidebar(graphics_dc);
        return;
    }
    // depending on your system you may need to look at double-buffered dcs
    wxPaintDC dc(this);
    render(dc);
}

int TextInput::sidebar_content_left() const
{
    int left = FromDIP(10);
    if (m_sidebar_item_icons && icon.bmp().IsOk() && !(m_sidebar_dropdown && icon.name() == "drop_down"))
        left += icon.GetBmpSize().x + FromDIP(6);
    if (m_sidebar_item_icons && icon_1.bmp().IsOk())
        left += icon_1.GetBmpSize().x + FromDIP(6);
    return left;
}

void TextInput::render_sidebar(wxGCDC& dc)
{
    const wxSize size = GetClientSize();
    const int states = state_handler.states();
    const wxColour foreground = label_color.colorForStates(states);
    dc.SetBackground(wxBrush(GetBackgroundColour()));
    dc.Clear();
    if (size.x <= 1 || size.y <= 1)
        return;

    dc.SetPen(border_width > 0 ? wxPen(border_color.colorForStates(states), border_width, border_style) : *wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(background_color.colorForStates(states)));
    const double inset = border_width > 0 ? border_width / 2.0 : 0.0;
    if (auto* graphics = dc.GetGraphicsContext())
        graphics->DrawRoundedRectangle(inset, inset, size.x - inset * 2.0, size.y - inset * 2.0, std::max(0.0, radius));
    else
        dc.DrawRoundedRectangle(0, 0, size.x - 1, size.y - 1, std::max(0.0, radius));
    dc.SetFont(GetFont());
    dc.SetTextForeground(foreground);

    int left = FromDIP(10);
    auto draw_icon = [&](const ScalableBitmap& bitmap) {
        const wxSize icon_size = bitmap.GetBmpSize();
        dc.DrawBitmap(bitmap.bmp(), left, (size.y - icon_size.y) / 2, true);
        left += icon_size.x + FromDIP(6);
    };
    if (m_sidebar_item_icons && icon.bmp().IsOk() && !(m_sidebar_dropdown && icon.name() == "drop_down"))
        draw_icon(icon);
    if (m_sidebar_item_icons && icon_1.bmp().IsOk())
        draw_icon(icon_1);

    int right = size.x - FromDIP(10);
    if (m_sidebar_dropdown && m_sidebar_drop_arrow) {
        // A small vector chevron stays sharp at every display scale.
        const int center_x = right - FromDIP(8);
        const int center_y = size.y / 2;
        const int half_width = FromDIP(4);
        const int half_height = FromDIP(2);
        wxPen chevron_pen(foreground, std::max(1, FromDIP(1)));
        chevron_pen.SetCap(wxCAP_ROUND);
        chevron_pen.SetJoin(wxJOIN_ROUND);
        dc.SetPen(chevron_pen);
        const wxPoint points[] = {{center_x - half_width, center_y - half_height},
                                  {center_x, center_y + half_height},
                                  {center_x + half_width, center_y - half_height}};
        dc.DrawLines(3, points);
        right -= FromDIP(22);
    }

    const wxString label = wxWindow::GetLabel();
    if (!label.empty() && (!m_sidebar_dropdown || !text_ctrl->IsShown())) {
        const int available_width = std::max(0, right - left);
        const wxString value = wxControl::Ellipsize(label, dc, wxELLIPSIZE_END, available_width);
        const wxSize value_size = dc.GetTextExtent(value);
        if (m_sidebar_dropdown) {
            if (static_tips.empty()) {
                dc.DrawText(value, left, (size.y - value_size.y) / 2);
            } else {
                wxFont secondary_font = m_sidebar_secondary_font.IsOk() ? m_sidebar_secondary_font : GetFont();
                if (!m_sidebar_secondary_font.IsOk())
                    secondary_font.SetFractionalPointSize(std::max(1.0, secondary_font.GetFractionalPointSize() - 1.0));
                dc.SetFont(secondary_font);
                const wxString tips = wxControl::Ellipsize(static_tips, dc, wxELLIPSIZE_END, available_width);
                const wxSize tips_size = dc.GetTextExtent(tips);
                const int gap = FromDIP(2);
                const int top = (size.y - value_size.y - tips_size.y - gap) / 2;
                dc.SetFont(GetFont());
                dc.DrawText(value, left, top);
                dc.SetFont(secondary_font);
                dc.SetTextForeground(text_color.colorForStates(states));
                dc.DrawText(tips, left, top + value_size.y + gap);
            }
        } else {
            // Units belong to the trailing gutter, independently of editor height.
            dc.DrawText(value, right - value_size.x, (size.y - value_size.y) / 2);
        }
    }
    if (badge.bmp().IsOk()) {
        const wxSize badge_size = badge.GetBmpSize();
        dc.DrawBitmap(badge.bmp(), size.x - badge_size.x, 0, true);
    }
}

/*
 * Here we do the actual rendering. I put it in a separate
 * method so that it can work no matter what type of DC
 * (e.g. wxPaintDC or wxClientDC) is used.
 */
void TextInput::render(wxDC& dc)
{
    StaticBox::render(dc);
    int states = state_handler.states();
    wxSize size = GetSize();
    bool   align_center = GetWindowStyle() & wxALIGN_CENTER_HORIZONTAL;
    bool   align_right = GetWindowStyle() & wxALIGN_RIGHT;
    // start draw
    wxPoint pt = {5, 0};
    if (icon.bmp().IsOk()) {
        wxSize szIcon = icon.GetBmpSize();
        pt.y = (size.y - szIcon.y) / 2;
        if (align_center) {
            if (pt.x * 2 + szIcon.x + 0 + labelSize.x < size.x)
                pt.x = (size.x - (szIcon.x + 0 + labelSize.x)) / 2;
        }
        dc.DrawBitmap(icon.bmp(), pt);
        pt.x += (szIcon.x + szIcon.x * 0.2);
    }
    if (icon_1.bmp().IsOk()) {
        wxSize szIcon = icon_1.GetBmpSize();
        pt.y          = (size.y - szIcon.y) / 2;
        if (align_center) {
            if (pt.x * 2 + szIcon.x + 0 + labelSize.x < size.x)
                pt.x = (size.x - (szIcon.x + 0 + labelSize.x)) / 2;
        }
        pt.x += szIcon.x / 4.f;
        dc.DrawBitmap(icon_1.bmp(), pt);
        pt.x += szIcon.x + 0;
    }
    auto text = wxWindow::GetLabel();
    if (!text.IsEmpty()) {
        if (static_tips.IsEmpty()) {
            wxSize textSize = text_ctrl->GetSize();
            if (align_right || align_center)
            {
                if (pt.x + labelSize.x + 5 > size.x)
                    text = wxControl::Ellipsize(text, dc, wxELLIPSIZE_END, size.x - pt.x - 5);
                pt.y = (size.y - labelSize.y) / 2;
            }
            else
            {
                pt.x += textSize.x;
                pt.y = (size.y + textSize.y) / 2 - labelSize.y;
            }
            dc.SetTextForeground(label_color.colorForStates(states));
            if(align_right)
                dc.SetFont(GetFont());
            else
                dc.SetFont(Label::Body_12);
            dc.DrawText(text, pt);
        } else {
            wxSize textSize = text_ctrl->GetSize();
            if (align_right || align_center) {
                if (pt.x + labelSize.x + 5 > size.x)
                    text = wxControl::Ellipsize(text, dc, wxELLIPSIZE_END, size.x - pt.x - 5);
                pt.y = (size.y - labelSize.y - static_tips_size.y - 8) / 2;
            } else {
                pt.x += textSize.x;
                pt.y = (size.y - labelSize.y - static_tips_size.y - 8) / 2;
            }
            dc.SetTextForeground(label_color.colorForStates(states));
            if(align_right)
                dc.SetFont(GetFont());
            else
                dc.SetFont(Label::Body_12);
            dc.DrawText(text, pt);

            if (align_right || align_center) {
                if (pt.x + static_tips_size.x + 5 > size.x) {
                    text = wxControl::Ellipsize(static_tips, dc, wxELLIPSIZE_END, size.x - pt.x - 5);
                }

                pt.y += (labelSize.y + 8);
            } else {
                pt.x += static_tips_size.x;
                pt.y += (labelSize.y + 8);
            }

            dc.SetTextForeground(wxColour(144, 144, 144));

            wxFont font = GetFont();
            font.SetPointSize(font.GetPointSize() - 1);// use smaller font
            dc.SetFont(font);
            dc.DrawText(static_tips, pt);
        }
    }
}

void TextInput::messureSize()
{
    wxSize size = GetSize();
    if (m_sidebar_style) {
        labelSize = GetTextExtent(wxWindow::GetLabel());
        wxFont secondary_font = GetFont();
        secondary_font.SetFractionalPointSize(std::max(1.0, secondary_font.GetFractionalPointSize() - 1.0));
        GetTextExtent(static_tips, &static_tips_size.x, &static_tips_size.y, nullptr, nullptr, &secondary_font);
        int content_height = GetTextExtent("Ag").y;
        if (m_sidebar_dropdown && !static_tips.empty())
            content_height = labelSize.y + static_tips_size.y + FromDIP(2);
        const int minimum_height = std::max(FromDIP(28), content_height + FromDIP(8));
        size.y = std::max(size.y, minimum_height);
        SetMinSize({GetMinWidth(), minimum_height});
        SetSize(size);
        return;
    }
    wxClientDC dc(this);
    bool   align_right = GetWindowStyle() & wxALIGN_RIGHT;
    if (align_right)
        dc.SetFont(GetFont());
    else
        dc.SetFont(Label::Body_12);
    labelSize = dc.GetTextExtent(wxWindow::GetLabel());
    wxSize textSize = text_ctrl->GetSize();

    if (!static_tips.empty()) {
        static_tips_size = dc.GetTextExtent(static_tips);
        textSize.x = std::max(labelSize.GetWidth(), static_tips_size.GetWidth());
        textSize.y += static_tips_size.y;
        textSize.y += 8;
    }

    int h = textSize.y + 8;
    if (size.y < h) {
        size.y = h;
    }

    wxSize minSize = size;
    minSize.x = GetMinWidth();
    SetMinSize(minSize);
    SetSize(size);
}
