#ifndef slic3r_GUI_DropDown_hpp_
#define slic3r_GUI_DropDown_hpp_

#include <boost/date_time/posix_time/posix_time.hpp>
#include <wx/event.h>
#include <vector>
#include <cstddef>
#include <utility>
#include <functional>
#include <wx/gdicmn.h>
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include <boost/date_time/posix_time/ptime.hpp>
#include <wx/dc.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/weakref.h>
#include <wx/window.h>
#include "../wxExtensions.hpp"
#include "StateHandler.hpp"
#include "PopupWindow.hpp"

class wxDC;
class wxWindow;
class PrinterDropDownDialog;

#define DD_NO_CHECK_ICON    0x0001
#define DD_NO_TEXT          0x0002
#define DD_STYLE_MASK       0x0003

#define DD_ITEM_STYLE_SPLIT_ITEM  0x0001 // ----text----, text with horizontal line arounds
#define DD_ITEM_STYLE_DISABLED    0x0002 // ----text----, text with horizontal line arounds
#define DD_ITEM_STYLE_DIMMED      0x0004 // gray text, but still selectable

wxDECLARE_EVENT(EVT_DISMISS, wxCommandEvent);
wxDECLARE_EVENT(EVT_DROPDOWN_ITEM_ACTION, wxCommandEvent);

class DropDown : public PopupWindow
{
public:
    struct ItemStatus {
        enum class Indicator { neutral, positive, negative };
        wxString subtitle;
        Indicator indicator = Indicator::neutral;
    };
    struct Item
    {
        wxString text;
        wxString text_static_tips;// display static tips for TextInput.eg.PrinterInfoBox
        wxBitmap icon;
        wxBitmap icon_textctrl;// display icon for TextInput.eg.PrinterInfoBox
        wxBitmap action_icon;
        void *   data{nullptr};
        wxString group_key{};
        wxString group_label{};
        wxString alias{};
        wxString tip{};
        int      flag{0};
        int      style{ 0 };// the style of item
        wxString popup_label{}; // presentation only; selection still uses text and original index
        // Optional cached-state reader, called on the UI thread. Never performs I/O.
        std::function<ItemStatus()> status_provider;
    };

private:
    std::vector<Item> &items;
    size_t             count = 0;
    wxString           group;
    bool               need_sync  = false;
    int                selection  = -1;
    int                hover_item = -1;

    DropDown * subDropDown { nullptr };
    DropDown * mainDropDown { nullptr };

    double radius = 0;
    bool   use_content_width = false;
    bool   limit_max_content_width = false;
    bool   align_icon        = false;
    bool   text_off          = false;

    wxSize textSize;
    wxSize iconSize;
    int    actionIconWidth = 0;
    wxSize rowSize;

    StateHandler state_handler;
    StateColor   text_color;
    StateColor   border_color;
    StateColor   selector_border_color;
    StateColor   selector_background_color;
    ScalableBitmap check_bitmap;
    ScalableBitmap arrow_bitmap;

    bool pressedDown = false;
    boost::posix_time::ptime dismissTime;
    wxPoint                  offset; // x not used
    wxPoint                  dragStart;

    bool printer_popup_style = false;
    PrinterDropDownDialog* printer_popup = nullptr;
    wxWeakRef<wxWindow> printer_popup_anchor;
    int printer_footer_index = -1;
    std::function<void()> printer_edit_action;
    std::function<void()> printer_manage_action;

public:
    DropDown(std::vector<Item> &items);

    DropDown(wxWindow *parent, std::vector<Item> &items, long style = 0);
    ~DropDown() override;

    void Create(wxWindow * parent, long style = 0);

public:
    void Invalidate(bool clear = false);

    int GetSelection() const { return selection; }

    void SetSelection(int n);

    wxString GetValue() const;
    void     SetValue(const wxString &value);

public:
    void SetCornerRadius(double radius);

    void SetBorderColor(StateColor const & color);

    void SetSelectorBorderColor(StateColor const & color);

    void SetTextColor(StateColor const &color);

    void SetSelectorBackgroundColor(StateColor const &color);

    void SetUseContentWidth(bool use, bool limit_max_content_width = false);

    void SetAlignIcon(bool align);

    // Opt-in search host: a frameless dialog gives the search field typing focus on every port.
    void SetPrinterPopupStyle(bool enabled = true);
    bool HasPrinterPopupStyle() const { return printer_popup_style; }
    void SetPrinterPopupActions(std::function<void()> edit, std::function<void()> manage);
    void SetPrinterPopupFooterIndex(int index) { printer_footer_index = index; }
    void SetPrinterPopupAnchor(wxWindow* anchor) { printer_popup_anchor = anchor; }
    bool IsPopupShown() const;

public:
    void Rescale();

    bool HasDismissLongTime();

    void Popup(wxWindow *focus = nullptr) override;

protected:
    void Dismiss() override;

    void OnDismiss() override;

    bool ShouldDismissOnTopWindowDeactivate() override;

private:
    void paintEvent(wxPaintEvent& evt);
    void paintNow();

    void render(wxDC& dc);

    int hoverIndex();

    int selectedItem();
    std::pair<int, int> actionColumn() const;

    friend class ComboBox;
    friend class PrinterDropDownDialog;
    void messureSize();
    void autoPosition();

    // some useful events
    void mouseDown(wxMouseEvent& event);
    void mouseReleased(wxMouseEvent &event);
    void mouseCaptureLost(wxMouseCaptureLostEvent &event);
    void mouseMove(wxMouseEvent &event);
    void mouseWheelMoved(wxMouseEvent &event);

    void sendDropDownEvent();
    void sendDropDownActionEvent(int index);


    DECLARE_EVENT_TABLE()
};

#endif // !slic3r_GUI_DropDown_hpp_
