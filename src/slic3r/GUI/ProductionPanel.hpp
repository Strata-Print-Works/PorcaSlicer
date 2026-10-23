#pragma once

#include <wx/panel.h>
#include <wx/bitmap.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "Lazy.hpp"

class wxStaticBitmap;
class wxWindow;

namespace Slic3r::GUI {

class Plater;

class ProductionPanel final : public wxPanel, public LazyInstance<ProductionPanel>
{
public:
    explicit ProductionPanel(wxWindow* parent);
    bool Show(bool show = true) override;
    // Rebuilds the view when the record, the plates or the model changed since
    // it was last shown. Never changes the project.
    void refresh();
    static void record_sent(int plate_index, const std::string& printer_name);
    static void restore_journal();

private:
    // A card waiting for its preview; rendered one per idle pass after the view is shown.
    struct PendingPreview
    {
        std::string key;
        wxStaticBitmap* image;
        wxWindow* unavailable;
        wxWindow* card;
    };

    wxPanel* m_content = nullptr;
    std::string m_preview_project_id;
    std::string m_shown_signature;
    unsigned m_generation = 0;
    std::vector<PendingPreview> m_pending_previews;
    std::unordered_map<std::string, wxBitmap> m_previews;
    wxBitmap preview_for(Plater& plater, const std::string& part_key);
    void render_pending_previews(unsigned generation);
    void start_production();
    void count_parts();
    void record_print(int plate_index);
    void mark_plate_done(int plate_index);
    void undo_mark_plate_done(const std::string& run_id);
    void review_run(const std::string& run_id);
};

} // namespace Slic3r::GUI
