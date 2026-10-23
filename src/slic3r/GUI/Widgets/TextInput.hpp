#ifndef slic3r_GUI_TextInput_hpp_
#define slic3r_GUI_TextInput_hpp_

#include <wx/containr.h>
#include <wx/gdicmn.h>
#include <wx/font.h>
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include <wx/string.h>
#include <wx/event.h>
#include <wx/dc.h>
#include <wx/textctrl.h>
#include "StaticBox.hpp"

class wxGCDC;

class TextInput : public wxNavigationEnabled<StaticBox>
{

    wxSize labelSize;
    ScalableBitmap icon;
    ScalableBitmap icon_1;
    StateColor     label_color;
    StateColor     text_color;
    wxTextCtrl * text_ctrl = nullptr;

    bool m_sidebar_style = false;
    bool m_sidebar_item_icons = true;
    bool m_sidebar_dropdown = false;
    bool m_sidebar_drop_arrow = true;
    wxFont m_sidebar_secondary_font;

    wxString  static_tips;
    wxSize    static_tips_size;
    wxBitmap  static_tips_icon;

    static const int TextInputWidth = 200;
    static const int TextInputHeight = 50;

public:
    TextInput();

    TextInput(wxWindow *     parent,
              wxString       text,
              wxString       label = "",
              wxString       icon  = "",
              const wxPoint &pos   = wxDefaultPosition,
              const wxSize & size  = wxDefaultSize,
              long           style = 0);

public:
    void Create(wxWindow *     parent,
              wxString       text,
              wxString       label = "",
              wxString       icon  = "",
              const wxPoint &pos   = wxDefaultPosition,
              const wxSize & size  = wxDefaultSize,
              long           style = 0);

    void SetCornerRadius(double radius);

    // Only meant to be used by inspector, not public API
    int GetCornerRadius() const { return static_cast<int>(radius); }

    void SetLabel(const wxString& label) override;

    void SetStaticTips(const wxString& tips, const wxBitmap& bitmap);

    void SetIcon(const wxBitmap & icon);
    void SetIcon(const wxString & icon);

    void SetIcon_1(const wxString &icon);
    void SetIcon_1(const wxBitmap &icon);

    void SetLabelColor(StateColor const &color);

    void SetTextColor(StateColor const &color);

    // Opt-in presentation for compact Prepare controls; editing and events stay native.
    void SetSidebarStyle(bool enabled = true);
    bool HasSidebarStyle() const { return m_sidebar_style; }
    void SetSidebarItemIcons(bool show = true);
    void SetSidebarSecondaryFont(const wxFont& font) { m_sidebar_secondary_font = font; Refresh(); }

    bool SetFont(const wxFont& font) override;

    virtual void Rescale();

    virtual bool Enable(bool enable = true) override;

    virtual void SetMinSize(const wxSize& size) override;

    wxTextCtrl *GetTextCtrl() { return text_ctrl; }

    wxTextCtrl const *GetTextCtrl() const { return text_ctrl; }

protected:
    virtual void OnEdit() {}

    void SetSidebarDropdown(bool draw_arrow) { m_sidebar_dropdown = true; m_sidebar_drop_arrow = draw_arrow; }

    virtual void DoSetSize(
        int x, int y, int width, int height, int sizeFlags = wxSIZE_AUTO) override;

    void DoSetToolTipText(wxString const &tip) override;

private:
    void paintEvent(wxPaintEvent& evt);

    void render(wxDC& dc);

    void render_sidebar(wxGCDC& dc);
    int sidebar_content_left() const;

    void messureSize();

    DECLARE_EVENT_TABLE()
};

#endif // !slic3r_GUI_TextInput_hpp_
