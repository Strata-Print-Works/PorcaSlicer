#ifndef slic3r_MarkdownTip_hpp_
#define slic3r_MarkdownTip_hpp_

#include <string>
#include <wx/gdicmn.h>
#include <wx/popupwin.h>
#include <wx/timer.h>
#include <wx/webview.h>

#include <functional>

class wxTimer;
class wxTimerEvent;
class wxWebView;
class wxWebViewEvent;
class wxWindow;


namespace Slic3r { namespace GUI {

class MarkdownTip : public wxPopupTransientWindow
{
public:
    // `anchor` is the screen rect the tip should sit beside — the hovered row,
    // spanning the width of its panel, so the tip never lands on the fields.
    static bool ShowTip(std::string const &tip, std::string const &tooltip, wxRect const &anchor, const std::string& availability = {});

    static void ExitTip();

    static void Reload();

    static void Recreate(wxWindow *parent);

    static wxWindow* AttachTo(wxWindow * parent);

    static wxWindow* DetachFrom(wxWindow * parent);

    // Called with the setting key when the tip's page reports a click on its
    // "ask the copilot" link, on the UI thread. One handler at a time; pass {}
    // to clear it, which the owner must do before it goes away.
    static void SetAskHandler(std::function<void(std::string)> handler);

private:
    static MarkdownTip* markdownTip(bool create = true);

    MarkdownTip();

    ~MarkdownTip();

    void LoadStyle();

    bool ShowTip(wxRect const &anchor, std::string const &tip, std::string const & tooltip, const std::string& availability);

    // Where a tip of `size` goes for the current anchor, kept on one display.
    wxPoint Place(wxSize const &size) const;

    // Whether the pointer is still on its way to the tip — on it, on the row it
    // belongs to, or in the gap between the two — rather than gone elsewhere.
    bool PointerInPlay() const;

    std::string LoadTip(std::string const &tip, std::string const &tooltip);

    void RunScript(std::string const& script);

private:
    wxWebView* CreateTipView(wxWindow* parent);

    void OnLoaded(wxWebViewEvent& event);

    void OnTitleChanged(wxWebViewEvent& event);

    void OnError(wxWebViewEvent& event);

    void OnTimer(wxTimerEvent& event);
    
private:
    wxWebView * _tipView = nullptr;
    std::string _lastTip;
    std::string _lastTooltip;
    std::string _lastAvailability;
    std::string _pendingScript = " ";
    std::string _language;
    wxRect _anchor;
    double _lastHeight = 0;
    wxTimer* _timer = nullptr;
    bool _hide = false;
    bool _data_dir = false;
};

}
}

#endif
