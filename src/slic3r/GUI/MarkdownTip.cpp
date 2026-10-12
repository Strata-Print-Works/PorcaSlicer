#include "MarkdownTip.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "MainFrame.hpp"
#include "Widgets/WebView.hpp"

#include "libslic3r/Utils.hpp"
#include "I18N.hpp"

#include <string>
#include <sstream>
#include <ios>
#include <cctype>
#include <iomanip>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <cstddef>
#include <wx/display.h>
#include <wx/gdicmn.h>
#include <wx/popupwin.h>
#include <wx/sizer.h>
#include <wx/timer.h>
#include <wx/file.h>
#include <wx/webview.h>
#include <wx/window.h>

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

// Fixed width; the height follows the rendered content (see OnTitleChanged).
static const int TIP_WIDTH = 400;

// How long the tip waits before deciding the pointer has really gone, and — since
// OnTimer re-checks by restarting — how often it looks again while the pointer is
// still on its way. It is short on purpose: what buys the pointer time to reach the
// tip is the region it is allowed to be in (PointerInPlay), not the length of this
// wait, so a tip abandoned over the 3D view is still gone within a third of a second.
static const int HIDE_DELAY = 300;

// Slack around that region. The tip stands 8px clear of the row (the `gap` in Place),
// and a hand crossing that gap does not travel in a straight line — it overshoots the
// row it started on by a few pixels either way. Half a row's height covers the drift
// while leaving the rows above and below out of it.
static const int TRAVEL_SLACK = 16;

// Usable bounds of whichever display holds `at`. The rect matters, not just its
// size: a second monitor — or the menu bar on the first — puts the origin well
// away from zero, and clamping against a bare size lands the tip in the wrong place.
static wxRect display_area(wxPoint const &at)
{
    const int index = wxDisplay::GetFromPoint(at);
    return wxDisplay(index == wxNOT_FOUND ? 0u : unsigned(index)).GetClientArea();
}

// CMGUO

static std::string url_encode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (std::string::const_iterator i = value.begin(), n = value.end(); i != n; ++i) {
        std::string::value_type c = (*i);

        // Keep alphanumeric and other accepted characters intact
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
            continue;
        }

        // Any other characters are percent-encoded
        escaped << std::uppercase;
        escaped << '%' << std::setw(2) << int((unsigned char)c);
        escaped << std::nouppercase;
    }
    return escaped.str();
}
/*
 * Edge browser not support WebViewHandler
 * 
class MyWebViewHandler : public wxWebViewArchiveHandler
{
public:
    MyWebViewHandler() : wxWebViewArchiveHandler("tooltip") {}
    wxFSFile* GetFile(const wxString& uri) override {
        // file:///resources/tooltip/test.md
        wxFSFile* direct = wxWebViewArchiveHandler::GetFile(uri);
        if (direct)
            return direct;
        // file:///data/tooltips.zip;protocol=zip/test.md
        int n = uri.Find("resources/tooltip");
        if (n == wxString::npos)
            return direct;
        set_var_dir(data_dir());
        auto url = var("tooltips.zip");
        std::replace(url.begin(), url.end(), '\\', '/');
        auto uri2 = "file:///" + wxString(url) + ";protocol=zip" + uri.substr(n + 17);
        return wxWebViewArchiveHandler::GetFile(uri2);
    } 
};
*/

/*
TODO:
1. Fix height correctly now h * 1.25 + 50
2. Async RunScript avoid long call stack risc
3. Fetch markdown content in javascript (*)
4. Use scheme handler to support zip archive & make code tidy
*/

MarkdownTip::MarkdownTip()
    : wxPopupTransientWindow(wxGetApp().mainframe, wxBORDER_NONE)
{
    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

    _tipView = CreateTipView(this);
    topsizer->Add(_tipView, wxSizerFlags().Expand().Proportion(1));

    SetSizer(topsizer);
    SetSize({TIP_WIDTH, 300});

    LoadStyle();

    _timer = new wxTimer;
    _timer->Bind(wxEVT_TIMER, &MarkdownTip::OnTimer, this);
}

MarkdownTip::~MarkdownTip() { delete _timer; }

void MarkdownTip::LoadStyle()
{
    _language = GUI::into_u8(GUI::wxGetApp().current_language_code());
    fs::path ph(data_dir());
    ph /= "resources/tooltip/common/styled.html";
    _data_dir = true;
    if (!fs::exists(ph)) {
        ph = resources_dir();
        ph /= "tooltip/styled.html";
        _data_dir = false;
    }
    _tipView->LoadURL(file_url_from_path(ph));
    _lastTip.clear();
}

// The tip sits beside the hovered row, not over it, and stays on one display.
// Preferred side is to the right of the row; it flips left when the display has
// no room there, and clamps as a last resort so it is never partly offscreen.
wxPoint MarkdownTip::Place(wxSize const &size) const
{
    const wxRect area = display_area(_anchor.GetTopLeft());
    const int gap = 8;

    int x = _anchor.GetRight() + gap;
    if (x + size.x > area.GetRight())
        x = _anchor.GetLeft() - gap - size.x;
    // Both GetRight()/GetBottom() are inclusive, hence the +1 on the far edge.
    x = std::min(std::max(x, area.GetLeft()), std::max(area.GetLeft(), area.GetRight() + 1 - size.x));

    int y = _anchor.GetTop();
    y = std::min(std::max(y, area.GetTop()), std::max(area.GetTop(), area.GetBottom() + 1 - size.y));

    return {x, y};
}

// Whether the pointer is somewhere that still counts as on its way to the tip, which
// it has to be able to reach: the tip carries a link at its foot, and the pointer only
// gets there by leaving the label it hovered. Three places count.
//
//   - the tip itself, where the pointer was heading;
//   - the row the tip belongs to. The anchor spans the whole panel, so walking right
//     from the label across the fields and the revert icons never leaves it;
//   - the gap between the two, which belongs to neither rect. Only the columns
//     strictly between them: over the panel's own columns the row is all that counts,
//     or a tall tip would keep itself alive while the pointer worked further down the
//     same page.
//
// Anywhere else — another panel, the 3D view — is a departure and the tip goes.
bool MarkdownTip::PointerInPlay() const
{
    if (!IsShown())
        return false; // nothing on screen to be walking towards

    const wxPoint pointer = wxGetMousePosition();
    const wxRect  tip     = GetScreenRect();

    if (wxRect(tip).Inflate(TRAVEL_SLACK).Contains(pointer))
        return true;
    if (wxRect(_anchor).Inflate(TRAVEL_SLACK).Contains(pointer))
        return true;

    // Both GetRight()/GetBottom() are inclusive, hence the ±1 on the far edges.
    const int left  = std::min(_anchor.GetRight(), tip.GetRight()) + 1;
    const int right = std::max(_anchor.GetLeft(), tip.GetLeft()) - 1;
    if (left > right)
        return false; // no gap: the tip could not be placed clear of the row
    const wxRect span = _anchor.Union(tip);
    return wxRect(left, span.GetTop(), right - left + 1, span.GetHeight()).Inflate(0, TRAVEL_SLACK).Contains(pointer);
}

bool MarkdownTip::ShowTip(wxRect const &anchor, std::string const &tip, std::string const &tooltip)
{
    if (tip.empty()) {
        if (_tipView->GetParent() != this)
            return false;
        // A non-zero x means the row has a tooltip of its own to show instead (see
        // OG_CustomCtrl::OnMotion) — but not while the pointer is on its way here:
        // the revert icons sit at the right end of the row, square in the path from
        // the label to the tip, and a modified setting gives them a tooltip.
        if (anchor.x && !PointerInPlay()) {
            _hide = true;
            BOOST_LOG_TRIVIAL(info) << "MarkdownTip::ShowTip: hide soon on empty tip.";
            this->Hide();
        }
        else if (!_hide) {
            _hide = true;
            BOOST_LOG_TRIVIAL(info) << "MarkdownTip::ShowTip: start hide timer (" << HIDE_DELAY << ")...";
            _timer->StartOnce(HIDE_DELAY);
        }
        return false;
    }
    bool tipChanged = _lastTip != tip;
    if (tipChanged) {
        auto content = LoadTip(tip, tooltip);
        if (content.empty()) {
            _hide = true;
            this->Hide();
            BOOST_LOG_TRIVIAL(info) << "MarkdownTip::ShowTip: hide soon on empty content.";
            return false;
        }
        auto script = "window.showMarkdown('" + url_encode(content) + "', true);";
        if (!_pendingScript.empty()) {
            _pendingScript = script;
        }
        else {
            RunScript(script);
        }
        _lastTip = tip;
        if (_tipView->GetParent() == this)
            this->Hide();
    }
    if (_tipView->GetParent() == this) {
        _anchor = anchor;
        this->SetPosition(Place(this->GetSize()));
        if (tipChanged || _hide) {
            _hide = false;
            BOOST_LOG_TRIVIAL(info) << "MarkdownTip::ShowTip: start show timer (500)...";
            _timer->StartOnce(500);
        }
    }
    return true;
}

std::string MarkdownTip::LoadTip(std::string const &tip, std::string const &tooltip)
{
    fs::path ph;
    wxString file;
    wxFile   f;
    if (_data_dir) {
        if (!_language.empty()) {
            ph = data_dir();
            ph /= "resources/tooltip/" + _language +  "/" + tip + ".md";
            file = from_u8(ph.string());
            if (wxFile::Exists(file) && f.Open(file)) {
                std::string content(f.Length(), 0);
                f.Read(&content[0], content.size());
                return content;
            }
        }
        ph = data_dir();
        ph /= "resources/tooltip/common/" + tip + ".md";
        file = from_u8(ph.string());
        if (wxFile::Exists(file) && f.Open(file)) {
            std::string content(f.Length(), 0);
            f.Read(&content[0], content.size());
            return content;
        }
    }
    /*
    file = var("tooltips.zip");
    if (wxFile::Exists(file) && f.Open(file)) {
        wxFileInputStream fs(f);
        wxZipInputStream zip(fs);
        file = tip + ".md";
        while (auto e = zip.GetNextEntry()) {
            if (e->GetName() == file) {
                if (zip.OpenEntry(*e)) {
                    std::string content(f.Length(), 0);
                    zip.Read(&content[0], content.size());
                    return content;
                }
                break;
            }
        }
    }
    */
    ph = resources_dir();
    ph /= "tooltip/" + _language + "/" + tip + ".md";
    file = from_u8(ph.string());
    if (wxFile::Exists(file) && f.Open(file)) {
        std::string content(f.Length(), 0);
        f.Read(&content[0], content.size());
        return content;
    }
    ph = resources_dir();
    ph /= "tooltip/" + tip + ".md";
    file = from_u8(ph.string());
    if (wxFile::Exists(file) && f.Open(file)) {
        std::string content(f.Length(), 0);
        f.Read(&content[0], content.size());
        return content;
    }
    if (!tooltip.empty()) return "#### " + _utf8(tip) + "\n" + tooltip;
    return (_tipView->GetParent() == this && tip.empty()) ? "" : LoadTip("", "");
}

void MarkdownTip::RunScript(std::string const& script)
{
    WebView::RunScript(_tipView, script);
}

wxWebView* MarkdownTip::CreateTipView(wxWindow* parent)
{
    wxWebView *tipView = WebView::CreateWebView(parent, "");
    Bind(wxEVT_WEBVIEW_LOADED, &MarkdownTip::OnLoaded, this);
    Bind(wxEVT_WEBVIEW_TITLE_CHANGED, &MarkdownTip::OnTitleChanged, this);
    Bind(wxEVT_WEBVIEW_ERROR, &MarkdownTip::OnError, this);
    return tipView;
}

void MarkdownTip::OnLoaded(wxWebViewEvent& event)
{
}

// Set by the plugin host, so that the tip itself carries no dependency on plugins.
static std::function<void(std::string)> ask_handler;

// The page decides what goes in the title, so only something shaped like a setting
// key is ever passed on: this ends up in a plugin, not in our own hands.
static bool is_setting_key(std::string const &key)
{
    if (key.empty())
        return false;
    for (char c : key)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}

void MarkdownTip::SetAskHandler(std::function<void(std::string)> handler) { ask_handler = std::move(handler); }

void MarkdownTip::OnTitleChanged(wxWebViewEvent& event)
{
    if (!_pendingScript.empty()) {
        RunScript(_pendingScript);
        _pendingScript.clear();
        return;
    }
    // The page reports a click on its "ask the copilot" link as "ask:<key>:<n>", the
    // title being the only channel it has back here (resources/tooltip/styled.html).
    // The counter only exists so that clicking the same link twice is two titles.
    const wxString title = event.GetString();
    if (title.StartsWith("ask:")) {
        const std::string message = into_u8(title);
        const size_t      last    = message.rfind(':');
        const std::string key     = last > 4 ? message.substr(4, last - 4) : std::string();
        if (is_setting_key(key)) {
            // The tip would otherwise sit over the panel the click is meant to reach.
            _hide = true;
            this->Hide();
            if (ask_handler)
                ask_handler(key);
        }
        return;
    }
#ifdef __linux__
    wxString str = "0";
#else
    wxString str = title;
#endif
    double height = 0;
    if (str.ToDouble(&height)) {
        if (height > _lastHeight - 10 && height < _lastHeight + 10)
            return;
        _lastHeight = height;
        height *= 1.25; height += 50;
        const wxRect area = display_area(_anchor.GetTopLeft());
        if (height > area.GetHeight())
            height = area.GetHeight();
        const wxSize size{TIP_WIDTH, int(height)};
        this->SetSize(size);
        this->SetPosition(Place(size));
    }
}
void MarkdownTip::OnError(wxWebViewEvent& event)
{
}

void MarkdownTip::OnTimer(wxTimerEvent& event)
{
    if (_hide) {
        if (PointerInPlay()) {
            BOOST_LOG_TRIVIAL(info) << "MarkdownTip::OnTimer: restart hide timer...";
            // Spelled out rather than StartOnce(): that reuses whatever interval ran
            // last, which is this one only as long as nothing else starts the timer.
            _timer->StartOnce(HIDE_DELAY);
            return;
        }
        BOOST_LOG_TRIVIAL(info) << "MarkdownTip::OnTimer: hide.";
        this->Hide();
    } else {
        BOOST_LOG_TRIVIAL(info) << "MarkdownTip::OnTimer: show.";
        this->Show();
    }
}

MarkdownTip* MarkdownTip::markdownTip(bool create)
{
    static MarkdownTip * markdownTip = nullptr;
    if (markdownTip == nullptr && create)
        markdownTip = new MarkdownTip;
    return markdownTip;
}

bool MarkdownTip::ShowTip(std::string const& tip, std::string const & tooltip, wxRect const &anchor)
{
    // Upstream returns false here under NDEBUG, so this popover has never been
    // reachable in a release build — which is why the tooltip content it reads
    // was left unwritten. The content exists now, so let it through.
    return markdownTip()->ShowTip(anchor, tip, tooltip);
}

void MarkdownTip::ExitTip()
{
    //if (auto tip = markdownTip(false))
    //    tip->Destroy();
}

void MarkdownTip::Reload()
{
    if (auto tip = markdownTip(false)) 
        tip->LoadStyle();
}

void MarkdownTip::Recreate(wxWindow *parent)
{
    if (auto tip = markdownTip(false)) {
        tip->Reparent(parent);
        tip->LoadStyle(); // switch language
    }
}

wxWindow* MarkdownTip::AttachTo(wxWindow* parent)
{
    MarkdownTip& tip = *markdownTip();
    tip._tipView = tip.CreateTipView(parent);
    tip._pendingScript = " ";
    return tip._tipView;
}

wxWindow* MarkdownTip::DetachFrom(wxWindow* parent)
{
    MarkdownTip& tip = *markdownTip();
    if (tip._tipView->GetParent() == parent) {
        tip.Destroy();
    }
    return NULL;
}

}
}
