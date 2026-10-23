#include "ProductionPanel.hpp"

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "GUI_Utils.hpp"
#include "3DScene.hpp"
#include "Camera.hpp"
#include "GLCanvas3D.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/SpinInput.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/ProgressBar.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PlatePrintSetup.hpp"
#include "libslic3r/ProductionLedger.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <wx/image.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/wrapsizer.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <functional>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {
namespace {

namespace fs = boost::filesystem;

// Edge of a part preview, in DIP.
constexpr int preview_edge = 112;
// Journal snapshots kept per project. Only the newest restores unsaved progress;
// the others cover a newest snapshot that cannot be read.
constexpr size_t kept_journal_snapshots = 3;

std::string new_id()
{
    // Seeding a generator reads the system entropy source; do it once per thread.
    static thread_local boost::uuids::random_generator generator;
    return boost::uuids::to_string(generator());
}

Label* production_label(wxWindow* parent, const wxString& text, long style = 0)
{
    auto* label = new Label(parent, text, style);
    label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    return label;
}

Label* production_label(wxWindow* parent, const wxFont& font, const wxString& text,
                        long style = 0, wxSize size = wxDefaultSize)
{
    auto* label = new Label(parent, font, text, style, size);
    label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    return label;
}

int physical_edge(int edge, double scale) { return std::max(1, int(std::lround(edge * scale))); }

// Previews borrow the shared OpenGL context through the Prepare canvas, which is
// hidden while this tab is shown. Every port renders offscreen through a hidden
// canvas once it has been realized and its GL functions were loaded, which
// is_initialized() reports. Before that a card keeps its placeholder.
bool can_render_previews(Plater& plater)
{
    GLCanvas3D* canvas = plater.get_view3D_canvas3D();
    return canvas && canvas->get_wxglcanvas() && canvas->is_initialized() && !wxGetApp().is_closing();
}

wxBitmap render_part_preview(Plater& plater, ModelObject& object, size_t instance_index, double scale)
{
    if (!can_render_previews(plater)) return {};
    GLCanvas3D* canvas = plater.get_view3D_canvas3D();
    GLVolumeCollection volumes;
    for (size_t index = 0; index < object.volumes.size(); ++index)
        if (object.volumes[index]->is_model_part())
            volumes.load_object_volume(&object, 0, int(index), int(instance_index), "object", true, false, false, false);
    if (volumes.volumes.empty()) return {};

    ThumbnailData thumbnail;
    ModelObjectPtrs objects{&object};
    ThumbnailsParams params{{}, false, true, false, true, 0, false};
    // Render larger than shown and filter down for smooth edges.
    const int edge = physical_edge(preview_edge, scale);
    const unsigned int render_size = unsigned(std::clamp(edge * 2, 192, 512));
    canvas->render_thumbnail(thumbnail, render_size, render_size, params, objects, volumes,
                             Camera::EType::Ortho, Camera::ViewAngleType::Iso);
    if (!thumbnail.is_valid()) return {};

    const wxColour background = StateColor::darkModeColorFor(wxColour("#F4F4F4"));
    wxImage image{int(render_size), int(render_size)};
    bool has_shape = false;
    bool has_background = false;
    for (unsigned int row = 0; row < render_size; ++row) {
        for (unsigned int column = 0; column < render_size; ++column) {
            const size_t offset = 4 * ((render_size - 1 - row) * render_size + column);
            const unsigned char* pixel = thumbnail.pixels.data() + offset;
            const unsigned int alpha = pixel[3];
            has_shape |= alpha > 0;
            has_background |= alpha == 0;
            const auto blend = [alpha](unsigned char foreground, unsigned char back) {
                return static_cast<unsigned char>((unsigned(foreground) * alpha + unsigned(back) * (255 - alpha)) / 255);
            };
            image.SetRGB(int(column), int(row), blend(pixel[0], background.Red()),
                         blend(pixel[1], background.Green()), blend(pixel[2], background.Blue()));
        }
    }
    if (!has_shape || !has_background) return {};
    return wxBitmap(image.Scale(edge, edge, wxIMAGE_QUALITY_HIGH), -1, scale);
}

// The preview at another DIP edge for the given window's display.
wxBitmap scaled_preview(const wxBitmap& bitmap, const wxWindow& window, int edge)
{
    const double scale = window.GetDPIScaleFactor();
    const int pixels = physical_edge(edge, scale);
    return wxBitmap(bitmap.ConvertToImage().Scale(pixels, pixels, wxIMAGE_QUALITY_HIGH), -1, scale);
}

struct PreviewSource
{
    ModelObject* object = nullptr;
    size_t instance = 0;
    std::string fingerprint;
};

// Identifies a part's geometry and orientation by stable model identities, so
// matching copies share a preview and a reused memory address never does.
PreviewSource preview_source(Plater& plater, const std::string& part_key, double scale)
{
    for (ModelObject* object : plater.model().objects) {
        for (size_t index = 0; index < object->instances.size(); ++index) {
            const ModelInstance* instance = object->instances[index];
            if (instance->production_id != part_key) continue;
            std::ostringstream signature;
            signature << object->id().id << ':' << wxGetApp().dark_mode() << ':' << scale << ':' << std::setprecision(17);
            const auto matrix = instance->get_matrix().matrix();
            for (int row = 0; row < 3; ++row)
                for (int column = 0; column < 3; ++column)
                    signature << ',' << matrix(row, column);
            for (const ModelVolume* volume : object->volumes) {
                const auto mesh = volume->mesh_ptr();
                signature << ':' << volume->id().id << ':' << reinterpret_cast<std::uintptr_t>(mesh.get());
                if (mesh) signature << ':' << mesh->its.vertices.size() << ':' << mesh->its.indices.size();
                const auto volume_matrix = volume->get_matrix().matrix();
                for (int row = 0; row < 4; ++row)
                    for (int column = 0; column < 4; ++column)
                        signature << ',' << volume_matrix(row, column);
            }
            return {object, index, signature.str()};
        }
    }
    return {};
}

ProductionLedger read_ledger(const Plater& plater)
{
    const auto& info = plater.model().model_info;
    if (!info) return {};
    const auto it = info->metadata_items.find(ProductionLedger::metadata_key);
    return it == info->metadata_items.end() ? ProductionLedger{} : ProductionLedger::deserialize(it->second);
}

// Journals belong to one saved project file. Save As keeps the project UUID, so
// a copy's unsaved progress must not restore or overwrite the original's record.
fs::path journal_directory(const Plater& plater, const std::string& project_id)
{
    const fs::path file = into_path(plater.get_project_filename(".3mf")).lexically_normal();
    return fs::path(data_dir()) / "production" / project_id / ProductionLedger::journal_location(into_u8(from_path(file)));
}

// Numbered journal snapshots, newest first.
std::vector<std::pair<uint64_t, fs::path>> journal_snapshots(const fs::path& directory)
{
    std::vector<std::pair<uint64_t, fs::path>> result;
    for (fs::directory_iterator it(directory), end; it != end; ++it) {
        if (it->path().extension() != ".json") continue;
        const std::string stem = it->path().stem().string();
        if (stem.empty() || stem.size() > 19 ||
            !std::all_of(stem.begin(), stem.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) continue;
        result.emplace_back(std::stoull(stem), it->path());
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    return result;
}

ProductionLedger read_latest_journal(const Plater& plater, const ProductionLedger& saved)
{
    if (saved.project_id.empty()) return saved;
    try {
        const fs::path directory = journal_directory(plater, saved.project_id);
        if (!fs::is_directory(directory)) return saved;
        for (const auto& [revision, path] : journal_snapshots(directory)) {
            if (revision <= saved.revision) break;
            boost::nowide::ifstream stream(path.string(), std::ios::binary);
            if (!stream) continue;
            try {
                const ProductionLedger candidate = ProductionLedger::deserialize(
                    std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()));
                if (candidate.project_id == saved.project_id && candidate.revision > saved.revision)
                    return candidate;
            } catch (const std::exception&) { /* A partial or unrelated file never overrides a saved record. */ }
        }
        return saved;
    } catch (const fs::filesystem_error&) {
        return saved;
    }
}

void journal(const Plater& plater, const ProductionLedger& ledger)
{
    const fs::path directory = journal_directory(plater, ledger.project_id);
    fs::create_directories(directory);
    const fs::path target = directory / (std::to_string(ledger.revision) + ".json");
    const fs::path pending = directory / (std::to_string(ledger.revision) + ".tmp");
    {
        boost::nowide::ofstream stream(pending.string(), std::ios::binary | std::ios::trunc);
        if (!stream) throw std::runtime_error("Could not save production progress.");
        stream << ledger.serialize();
        stream.flush();
        if (!stream) throw std::runtime_error("Could not save production progress.");
    }
    fs::rename(pending, target);
    try {
        const auto snapshots = journal_snapshots(directory);
        for (size_t i = kept_journal_snapshots; i < snapshots.size(); ++i) fs::remove(snapshots[i].second);
    } catch (const fs::filesystem_error&) { /* The new snapshot is saved; older ones are removed next time. */ }
}

// Saves the next revision of the record and, on success, updates ledger to it.
bool write_ledger(Plater& plater, ProductionLedger& ledger)
{
    ProductionLedger next = ledger;
    ++next.revision;
    try { journal(plater, next); }
    catch (const std::exception&) {
        show_error(&plater, _L("Could not save production progress. Check available disk space and try again."));
        return false;
    }
    if (!plater.model().model_info) plater.model().model_info = std::make_shared<ModelInfo>();
    plater.model().model_info->metadata_items[ProductionLedger::metadata_key] = next.serialize();
    ledger = std::move(next);
    plater.set_plater_dirty(true);
    plater.update_title_dirty_status();
    if (auto* canvas = plater.get_view3D_canvas3D()) canvas->set_as_dirty();
    return true;
}

std::vector<ProductionRunPart> plate_parts(Plater& plater, int plate_index)
{
    std::vector<ProductionRunPart> result;
    PartPlate* plate = plater.get_partplate_list().get_plate(plate_index);
    if (!plate || !plate->is_printable()) return result;
    const auto& objects = plater.model().objects;
    for (size_t object = 0; object < objects.size(); ++object) {
        if (!objects[object]->printable) continue;
        for (size_t instance = 0; instance < objects[object]->instances.size(); ++instance) {
            const ModelInstance* item = objects[object]->instances[instance];
            if (item->printable && !item->production_id.empty() && plate->contain_instance(int(object), int(instance))) {
                std::ostringstream transform;
                transform << std::setprecision(17);
                const auto matrix = item->get_matrix().matrix();
                for (int row = 0; row < 4; ++row)
                    for (int column = 0; column < 4; ++column)
                        transform << (row || column ? "," : "") << matrix(row, column);
                result.push_back({item->production_id, 1, 0, transform.str()});
            }
        }
    }
    return result;
}

std::vector<std::vector<ProductionRunPart>> all_plate_parts(Plater& plater)
{
    std::vector<std::vector<ProductionRunPart>> result;
    const size_t count = plater.get_partplate_list().get_plate_list().size();
    for (size_t index = 0; index < count; ++index) result.push_back(plate_parts(plater, int(index)));
    return result;
}

std::vector<ProductionRunPart> current_parts_for_run(const std::vector<std::vector<ProductionRunPart>>& plates,
                                                     const ProductionRun& run)
{
    const std::vector<ProductionRunPart>* match = nullptr;
    for (const auto& parts : plates) {
        if (!ProductionLedger::same_plate_parts(run.parts, parts)) continue;
        if (match) return {}; // An ambiguous identity cannot select a plate for undo.
        match = &parts;
    }
    return match ? *match : std::vector<ProductionRunPart>{};
}

std::string part_name(const ModelObject& object, size_t instance)
{
    return object.instances.size() > 1 ? object.name + " #" + std::to_string(instance + 1) : object.name;
}

// Parts on the model that the record does not count yet, and copies that share
// another part's identity. Reading them never changes the project.
struct PartIdentity
{
    size_t uncounted = 0;
    size_t shared = 0;
    std::map<std::string, std::string> names;
};

PartIdentity part_identity(const Plater& plater, const ProductionLedger& ledger)
{
    PartIdentity result;
    const auto counted = [&ledger](const std::string& key) {
        return std::any_of(ledger.parts.begin(), ledger.parts.end(), [&key](const ProductionPart& part) { return part.key == key; });
    };
    for (const ModelObject* object : plater.model().objects) {
        if (!object->printable) continue;
        for (size_t instance = 0; instance < object->instances.size(); ++instance) {
            const ModelInstance* item = object->instances[instance];
            if (!item->printable) continue;
            if (item->production_id.empty() || !counted(item->production_id)) ++result.uncounted;
            else if (!result.names.emplace(item->production_id, part_name(*object, instance)).second) ++result.shared;
        }
    }
    return result;
}

// Gives every printable instance its own identity and adds new parts to the
// record. Only explicit production actions call this, and a change to the
// instances is its own undo step.
bool count_model_parts(Plater& plater, ProductionLedger& ledger)
{
    const auto& objects = plater.model().objects;
    std::set<std::string> seen;
    bool needs_ids = false;
    for (const ModelObject* object : objects) {
        if (!object->printable) continue;
        for (const ModelInstance* item : object->instances)
            needs_ids |= item->printable && (item->production_id.empty() || !seen.insert(item->production_id).second);
    }
    if (needs_ids) plater.take_snapshot(_u8L("Count parts for production"));
    bool changed = false;
    seen.clear();
    for (ModelObject* object : objects) {
        if (!object->printable) continue;
        for (size_t instance = 0; instance < object->instances.size(); ++instance) {
            ModelInstance* item = object->instances[instance];
            if (!item->printable) continue;
            if (item->production_id.empty() || !seen.insert(item->production_id).second) {
                item->production_id = new_id();
                seen.insert(item->production_id);
                changed = true;
            }
            const std::string name = part_name(*object, instance);
            if (ProductionPart* existing = ledger.find(item->production_id)) {
                if (existing->name != name) { existing->name = name; changed = true; }
            } else {
                ledger.parts.push_back({item->production_id, name, 1});
                changed = true;
            }
        }
    }
    return changed;
}

// Everything the view shows. An unchanged signature means the controls on
// screen are still current.
std::string view_signature(Plater& plater, double scale, bool readable, const ProductionLedger& ledger,
                           const std::vector<std::vector<ProductionRunPart>>& parts, const PartIdentity& identity)
{
    std::ostringstream out;
    out << readable << '|' << wxGetApp().dark_mode() << '|' << scale << '|' << plater.is_project_dirty() << '|'
        << plater.undo_redo_stack_main().active_snapshot_time() << '|' << plater.model().id().id << '|'
        << identity.uncounted << '|' << identity.shared << '|';
    if (readable) out << ledger.serialize() << '|';
    for (const auto& [key, name] : identity.names) out << key << '=' << name << ';';
    auto& plates = plater.get_partplate_list();
    for (const WorkspaceStage& stage : plates.stages()) out << '|' << stage.id << '=' << stage.name;
    const auto& list = plates.get_plate_list();
    for (size_t index = 0; index < list.size(); ++index) {
        out << '|' << list[index]->get_plate_name() << '@' << list[index]->print_setup().destination_name << '@'
            << list[index]->stage_id() << '@' << list[index]->empty();
        if (index < parts.size())
            for (const ProductionRunPart& part : parts[index]) out << ';' << part.key << '/' << part.transform;
    }
    return out.str();
}

std::string time_now()
{
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &timestamp);
#else
    gmtime_r(&timestamp, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

class RecordDialog final : public DPIDialog {
public:
    RecordDialog(wxWindow* parent, const ProductionLedger& ledger, const std::vector<ProductionRunPart>& parts,
                 const wxString& destination, const std::function<wxBitmap(const std::string&)>& preview,
                 bool allow_running = true)
        : DPIDialog(parent, wxID_ANY, _L("Record a print"), wxDefaultPosition, wxDefaultSize,
                    wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    {
        SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
        auto* layout = new wxBoxSizer(wxVERTICAL);
        layout->Add(production_label(this, _L("Choose the usable parts from this print.")), 0,
                    wxALL, FromDIP(16));
        auto* printer_row = new wxBoxSizer(wxHORIZONTAL);
        printer_row->Add(production_label(this, _L("Printer")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
        m_printer = new ::TextInput(this, destination, "", "", wxDefaultPosition, FromDIP(wxSize(250, 32)));
        printer_row->Add(m_printer, 1);
        layout->Add(printer_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        if (allow_running) {
            auto* running_row = new wxBoxSizer(wxHORIZONTAL);
            m_running = new ::CheckBox(this);
            m_running->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) {
                for (const auto& item : m_results) item.second->Enable(!m_running->GetValue());
            });
            running_row->Add(m_running, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
            running_row->Add(production_label(this, _L("This print is still running")), 1, wxALIGN_CENTER_VERTICAL);
            layout->Add(running_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        }
        auto* list = m_list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        list->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
        list->SetScrollRate(0, FromDIP(12));
        auto* part_rows = new wxBoxSizer(wxVERTICAL);
        for (const ProductionRunPart& part : parts) {
            const ProductionPart* match = nullptr;
            for (const ProductionPart& item : ledger.parts)
                if (item.key == part.key) { match = &item; break; }
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            wxBitmap bitmap = preview(part.key);
            if (bitmap.IsOk()) {
                row->Add(new wxStaticBitmap(list, wxID_ANY, scaled_preview(bitmap, *list, 64)),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
            }
            row->Add(production_label(list, match ? from_u8(match->name) : _L("Part"), LB_AUTO_WRAP), 1,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
            auto* input = new ::SpinInput(list, "", "", wxDefaultPosition, FromDIP(wxSize(90, 32)),
                                          0, 0, part.printed, part.accepted);
            row->Add(input, 0);
            part_rows->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
            m_results.emplace_back(part, input);
        }
        list->SetSizer(part_rows);
        list->SetMinSize(FromDIP(wxSize(420, std::min(480, int(parts.size()) * 84))));
        list->FitInside();
        layout->Add(list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
        auto* buttons = new DialogButtons(this, {L("Cancel"), L("Save")}, _L("Save"));
        buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        buttons->GetButtonFromID(wxID_SAVE)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        layout->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(12));
        SetSizerAndFit(layout);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

    std::string printer() const { return into_u8(m_printer->GetTextCtrl()->GetValue()); }
    bool is_running() const { return m_running && m_running->GetValue(); }
    std::vector<ProductionRunPart> result() const
    {
        std::vector<ProductionRunPart> result;
        for (const auto& item : m_results) {
            ProductionRunPart part = item.first;
            part.accepted = item.second->GetValue();
            result.push_back(std::move(part));
        }
        return result;
    }

protected:
    void on_dpi_changed(const wxRect&) override
    {
        m_printer->Rescale();
        if (m_running) m_running->Rescale();
        for (const auto& item : m_results) item.second->Rescale();
        m_list->SetMinSize(FromDIP(wxSize(420, std::min(480, int(m_results.size()) * 84))));
        m_list->FitInside();
        GetSizer()->SetSizeHints(this);
        Layout();
    }

private:
    ::TextInput* m_printer = nullptr;
    ::CheckBox* m_running = nullptr;
    wxScrolledWindow* m_list = nullptr;
    std::vector<std::pair<ProductionRunPart, ::SpinInput*>> m_results;
};


} // namespace

ProductionPanel::ProductionPanel(wxWindow* parent) : wxPanel(parent)
{
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    auto* outer = new wxBoxSizer(wxVERTICAL);
    auto* scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    scroll->SetScrollRate(0, FromDIP(16));
    scroll->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    m_content = scroll;
    m_content->SetSizer(new wxBoxSizer(wxVERTICAL));
    outer->Add(m_content, 1, wxEXPAND);
    SetSizer(outer);
    refresh();
}

wxBitmap ProductionPanel::preview_for(Plater& plater, const std::string& part_key)
{
    const double scale = GetDPIScaleFactor();
    const PreviewSource source = preview_source(plater, part_key, scale);
    if (!source.object) return {};
    const auto found = m_previews.find(source.fingerprint);
    if (found != m_previews.end()) return found->second;
    wxBitmap bitmap = render_part_preview(plater, *source.object, source.instance, scale);
    if (bitmap.IsOk()) m_previews.emplace(source.fingerprint, bitmap);
    return bitmap;
}

void ProductionPanel::render_pending_previews(unsigned generation)
{
    Plater* plater = wxGetApp().plater();
    if (generation != m_generation || m_pending_previews.empty() || !plater || wxGetApp().is_closing()) return;
    const PendingPreview pending = m_pending_previews.front();
    m_pending_previews.erase(m_pending_previews.begin());
    const wxBitmap bitmap = preview_for(*plater, pending.key);
    if (bitmap.IsOk()) {
        pending.image->SetBitmap(bitmap);
        pending.image->Show();
    } else {
        pending.unavailable->Show();
    }
    pending.card->Layout();
    if (!m_pending_previews.empty()) CallAfter([this, generation] { render_pending_previews(generation); });
}

bool ProductionPanel::Show(bool show)
{
    const bool changed = wxPanel::Show(show);
    if (show) refresh();
    return changed;
}

void ProductionPanel::refresh()
{
    Plater* plater = wxGetApp().plater();
    if (!plater || !m_content) return;
    ProductionLedger ledger;
    bool readable = true;
    try { ledger = read_latest_journal(*plater, read_ledger(*plater)); }
    catch (const std::exception&) { readable = false; }
    const bool active = readable && !ledger.project_id.empty();
    const auto parts_by_plate = active ? all_plate_parts(*plater) : std::vector<std::vector<ProductionRunPart>>{};
    const PartIdentity identity = active ? part_identity(*plater, ledger) : PartIdentity{};
    const double scale = GetDPIScaleFactor();
    std::string signature = view_signature(*plater, scale, readable, ledger, parts_by_plate, identity);
    if (signature == m_shown_signature) return;
    m_shown_signature = std::move(signature);
    // Cards from the previous view are destroyed below; queued previews must not reach them.
    ++m_generation;
    m_pending_previews.clear();

    auto* layout = static_cast<wxBoxSizer*>(m_content->GetSizer());
    layout->Clear(true);
    layout->Add(production_label(m_content, Label::Head_24, _L("Production")), 0, wxALL, FromDIP(20));
    if (!readable) {
        layout->Add(production_label(m_content, _L("This project's production record can't be opened. Try a newer Porca version or restore a saved copy.")),
                    0, wxLEFT | wxRIGHT, FromDIP(20));
    } else if (!active) {
        layout->Add(production_label(m_content, _L("Track what has been printed and what is still needed.")), 0,
                    wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
        auto* start = new Button(m_content, _L("Start production"));
        start->SetStyle(ButtonStyle::Confirm, ButtonType::Window);
        start->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CallAfter([this] { start_production(); }); });
        layout->Add(start, 0, wxLEFT | wxBOTTOM, FromDIP(20));
    } else {
        if (ledger.project_id != m_preview_project_id) {
            m_previews.clear();
            m_preview_project_id = ledger.project_id;
        }
        if (plater->is_project_dirty()) {
            auto* save = new Button(m_content, _L("Save progress to project"));
            save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                CallAfter([this] {
                    if (Plater* current = wxGetApp().plater()) current->save_project();
                    refresh();
                });
            });
            layout->Add(save, 0, wxLEFT | wxBOTTOM, FromDIP(20));
        }
        if (identity.uncounted > 0 || identity.shared > 0) {
            auto* attention = new wxBoxSizer(wxVERTICAL);
            if (identity.uncounted > 0)
                attention->Add(production_label(m_content, format_wxstr(_L_PLURAL("%1% part isn't counted yet.",
                    "%1% parts aren't counted yet.", identity.uncounted), identity.uncounted)), 0, wxBOTTOM, FromDIP(6));
            if (identity.shared > 0)
                attention->Add(production_label(m_content, format_wxstr(_L_PLURAL("%1% copy is counted together with another part.",
                    "%1% copies are counted together with other parts.", identity.shared), identity.shared)), 0, wxBOTTOM, FromDIP(6));
            auto* count = new Button(m_content, _L("Count these parts"));
            count->SetStyle(ButtonStyle::Regular, ButtonType::Window);
            count->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CallAfter([this] { count_parts(); }); });
            attention->Add(count, 0, wxTOP, FromDIP(4));
            layout->Add(attention, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
        }
        const int ready = ledger.ready_total();
        const int total = ledger.required_total();
        const int remaining = total - ready;
        layout->Add(production_label(m_content, Label::Head_20,
                        format_wxstr(_L("%1% of %2% ready"), ready, total)), 0,
                    wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
        auto* overall = new ProgressBar(m_content, wxID_ANY, std::max(1, total), wxDefaultPosition,
                                         FromDIP(wxSize(600, 14)));
        overall->m_progress_background_colour = StateColor::darkModeColorFor(wxColour("#DFDFDF"));
        overall->m_progress_colour = StateColor::darkModeColorFor(wxColour("#009688"));
        overall->SetBackgroundColour(m_content->GetBackgroundColour());
        overall->SetValue(ready);
        layout->Add(overall, 0, wxLEFT | wxBOTTOM, FromDIP(20));
        layout->Add(production_label(m_content, format_wxstr(
                        _L_PLURAL("%1% part still needed", "%1% parts still needed", remaining), remaining)), 0,
                    wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));

        const bool render_previews = can_render_previews(*plater);
        std::set<std::string> displayed;
        const auto add_part = [&](wxWrapSizer* cards, const ProductionPart& part) {
            displayed.insert(part.key);
            auto* surface = new wxPanel(m_content);
            surface->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F8F8")));
            surface->SetMinSize(FromDIP(wxSize(200, 262)));
            auto* card = new wxBoxSizer(wxVERTICAL);
            // The preview area keeps its size while the image renders after the view is shown.
            auto* preview = new wxBoxSizer(wxVERTICAL);
            preview->SetMinSize(FromDIP(wxSize(preview_edge, preview_edge)));
            auto* image = new wxStaticBitmap(surface, wxID_ANY, wxNullBitmap);
            auto* unavailable = production_label(surface, _L("Preview unavailable"));
            preview->AddStretchSpacer();
            preview->Add(image, 0, wxALIGN_CENTER_HORIZONTAL);
            preview->Add(unavailable, 0, wxALIGN_CENTER_HORIZONTAL);
            preview->AddStretchSpacer();
            const PreviewSource source = preview_source(*plater, part.key, scale);
            const auto cached = source.object ? m_previews.find(source.fingerprint) : m_previews.end();
            if (cached != m_previews.end()) {
                image->SetBitmap(cached->second);
                unavailable->Hide();
            } else if (source.object && render_previews) {
                image->Hide();
                unavailable->Hide();
                m_pending_previews.push_back({part.key, image, unavailable, surface});
            } else {
                image->Hide();
            }
            card->Add(preview, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(10));
            const auto current_name = identity.names.find(part.key);
            card->Add(production_label(surface, Label::Head_14,
                                       from_u8(current_name != identity.names.end() ? current_name->second : part.name),
                                       LB_AUTO_WRAP, FromDIP(wxSize(176, -1))), 0, wxBOTTOM, FromDIP(6));
            const int part_ready = std::min(part.required, ledger.accepted(part.key));
            card->Add(production_label(surface, part.required == 0 ? _L("Not needed") :
                      format_wxstr(_L("%1% of %2% ready"), part_ready, part.required)), 0,
                      wxBOTTOM, FromDIP(6));
            auto* bar = new ProgressBar(surface, wxID_ANY, std::max(1, part.required), wxDefaultPosition,
                                        FromDIP(wxSize(176, 10)));
            bar->m_progress_background_colour = StateColor::darkModeColorFor(wxColour("#DFDFDF"));
            bar->m_progress_colour = StateColor::darkModeColorFor(wxColour("#009688"));
            bar->SetBackgroundColour(surface->GetBackgroundColour());
            bar->SetValue(part_ready);
            card->Add(bar, 0, wxBOTTOM, FromDIP(10));
            card->AddStretchSpacer();
            auto* target = new wxBoxSizer(wxHORIZONTAL);
            target->Add(production_label(surface, _L("Need")), 0,
                        wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            auto* quantity = new ::SpinInput(surface, "", "", wxDefaultPosition, FromDIP(wxSize(84, 30)),
                                             0, 0, 9999, part.required);
            const std::string key = part.key;
            quantity->Bind(wxEVT_SPINCTRL, [this, key, quantity](wxCommandEvent&) {
                Plater* current = wxGetApp().plater();
                if (!current) return;
                ProductionLedger edited = read_latest_journal(*current, read_ledger(*current));
                if (ProductionPart* selected = edited.find(key)) {
                    selected->required = std::max(0, quantity->GetValue());
                    if (write_ledger(*current, edited)) CallAfter([this] { refresh(); });
                }
            });
            target->Add(quantity, 0, wxALIGN_CENTER_VERTICAL);
            card->Add(target, 0);
            auto* inset = new wxBoxSizer(wxVERTICAL);
            inset->Add(card, 1, wxEXPAND | wxALL, FromDIP(12));
            surface->SetSizer(inset);
            cards->Add(surface, 0, wxRIGHT | wxBOTTOM, FromDIP(12));
        };
        auto& plate_list = plater->get_partplate_list();
        const auto& plates = plate_list.get_plate_list();
        const auto add_plate = [&](size_t index) {
            const auto& parts = parts_by_plate[index];
            if (parts.empty()) return;
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            const wxString name = plates[index]->get_plate_name().empty()
                ? format_wxstr(_L("Plate %1%"), index + 1) : from_u8(plates[index]->get_plate_name());
            const wxString destination = from_u8(plates[index]->print_setup().destination_name);
            row->Add(production_label(m_content, Label::Head_18,
                     destination.empty() || destination == name ? name : name + L" · " + destination), 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
            auto* record = new Button(m_content, _L("Record a print"));
            record->SetStyle(ButtonStyle::Regular, ButtonType::Window);
            record->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
                CallAfter([this, index] { record_print(int(index)); });
            });
            row->Add(record, 0, wxRIGHT, FromDIP(8));
            auto* done = new Button(m_content, _L("Mark plate done"));
            done->SetStyle(ButtonStyle::Regular, ButtonType::Window);
            const bool still_needed = std::any_of(parts.begin(), parts.end(), [&](const ProductionRunPart& part) {
                const ProductionPart* target = ledger.find(part.key);
                return target && ledger.accepted(part.key) < target->required;
            });
            done->Enable(still_needed);
            done->SetToolTip(still_needed ? _L("Record every part on this plate as ready.") :
                             _L("All parts on this plate are already ready."));
            done->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
                CallAfter([this, index] { mark_plate_done(int(index)); });
            });
            row->Add(done, 0);
            const auto undoable = std::find_if(ledger.runs.rbegin(), ledger.runs.rend(), [&](const ProductionRun& run) {
                return ledger.mark_done_undo_status(run.id, parts) == ProductionLedger::MarkDoneUndoStatus::Available;
            });
            if (undoable != ledger.runs.rend()) {
                auto* undo = new Button(m_content, _L("Undo mark done"));
                undo->SetStyle(ButtonStyle::Regular, ButtonType::Window);
                const std::string run_id = undoable->id;
                undo->Bind(wxEVT_BUTTON, [this, run_id](wxCommandEvent&) {
                    CallAfter([this, run_id] { undo_mark_plate_done(run_id); });
                });
                row->Add(undo, 0, wxLEFT, FromDIP(8));
            }
            layout->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
            auto* cards = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);
            for (const ProductionRunPart& on_plate : parts)
                if (const ProductionPart* part = ledger.find(on_plate.key)) add_part(cards, *part);
            layout->Add(cards, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
        };
        for (const WorkspaceStage& stage : plate_list.stages()) {
            const bool has_parts = std::any_of(plates.begin(), plates.end(), [&](const PartPlate* plate) {
                return plate->stage_id() == stage.id && !plate->empty();
            });
            if (!has_parts) continue;
            layout->Add(production_label(m_content, Label::Head_18, from_u8(stage.name)), 0,
                        wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
            for (size_t index = 0; index < plates.size(); ++index)
                if (plates[index]->stage_id() == stage.id) add_plate(index);
        }
        const bool has_outside = std::any_of(plates.begin(), plates.end(), [](const PartPlate* plate) {
            return plate->stage_id().empty() && !plate->empty();
        });
        if (!plate_list.stages().empty() && has_outside)
            layout->Add(production_label(m_content, Label::Head_18, _L("Outside stages")), 0,
                        wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
        for (size_t index = 0; index < plates.size(); ++index) {
            if (plates[index]->stage_id().empty()) add_plate(index);
        }
        if (displayed.size() < ledger.parts.size()) {
            layout->Add(production_label(m_content, Label::Head_18, _L("Not on a plate")), 0,
                        wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
            auto* cards = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);
            for (const ProductionPart& part : ledger.parts)
                if (!displayed.count(part.key)) add_part(cards, part);
            layout->Add(cards, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
        }
        if (!ledger.runs.empty()) {
            layout->Add(production_label(m_content, Label::Head_18, _L("Print history")), 0,
                        wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
            for (auto it = ledger.runs.rbegin(); it != ledger.runs.rend(); ++it) {
                const wxString status = it->state == "sent" ? _L("Sent — check the parts when the print finishes") :
                    it->state == "printing" ? _L("Printing — check the parts when it finishes") :
                    it->state == "accepted" ? _L("Parts accepted") : _L("No usable parts recorded");
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                const wxString outcome = it->plate_name.empty() ? status :
                    format_wxstr(_L("%1%: %2%"), from_u8(it->plate_name), status);
                const wxString when = it->source == "manual" ?
                    format_wxstr(_L("Entered manually on %1%."), from_u8(it->recorded_at)) :
                    format_wxstr(_L("Recorded on %1%."), from_u8(it->recorded_at));
                row->Add(production_label(m_content, format_wxstr(_L("%1%. %2%"), outcome, when)), 1,
                         wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
                if (it->state == "printing" || it->source == "porca_send" || !it->reviews.empty()) {
                    auto* review = new Button(m_content, it->reviews.empty() ? _L("Choose good parts") : _L("Correct result"));
                    const std::string run_id = it->id;
                    review->Bind(wxEVT_BUTTON, [this, run_id](wxCommandEvent&) {
                        CallAfter([this, run_id] { review_run(run_id); });
                    });
                    row->Add(review, 0);
                }
                const auto undo_status = ledger.mark_done_undo_status(it->id, current_parts_for_run(parts_by_plate, *it));
                if (undo_status == ProductionLedger::MarkDoneUndoStatus::Available) {
                    auto* undo = new Button(m_content, _L("Undo mark done"));
                    const std::string run_id = it->id;
                    undo->Bind(wxEVT_BUTTON, [this, run_id](wxCommandEvent&) {
                        CallAfter([this, run_id] { undo_mark_plate_done(run_id); });
                    });
                    row->Add(undo, 0, wxLEFT, FromDIP(8));
                } else if (undo_status == ProductionLedger::MarkDoneUndoStatus::PlateChanged) {
                    row->Add(production_label(m_content, _L("Undo unavailable — plate changed")), 0,
                             wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
                }
                layout->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(20));
                for (const ProductionReview& review : it->reviews) {
                    const int accepted = std::accumulate(review.parts.begin(), review.parts.end(), 0,
                        [](int count, const ProductionRunPart& part) { return count + part.accepted; });
                    layout->Add(production_label(m_content, format_wxstr(
                        _L_PLURAL("%1% good part recorded on %2%", "%1% good parts recorded on %2%", accepted),
                        accepted, from_u8(review.recorded_at))), 0,
                        wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(36));
                }
            }
        }
    }
    m_content->Layout();
    static_cast<wxScrolledWindow*>(m_content)->FitInside();
    if (!m_pending_previews.empty()) {
        const unsigned generation = m_generation;
        CallAfter([this, generation] { render_pending_previews(generation); });
    }
}

void ProductionPanel::start_production()
{
    Plater* plater = wxGetApp().plater();
    if (!plater) return;
    if (plater->get_project_filename(".3mf").empty() && plater->save_project() != wxID_YES) return;
    ProductionLedger ledger = read_latest_journal(*plater, read_ledger(*plater));
    if (!ledger.project_id.empty()) return;
    ledger.project_id = new_id();
    count_model_parts(*plater, ledger);
    if (!write_ledger(*plater, ledger)) return;
    plater->save_project();
    CallAfter([this] { refresh(); });
}

void ProductionPanel::count_parts()
{
    Plater* plater = wxGetApp().plater();
    if (!plater) return;
    ProductionLedger ledger = read_latest_journal(*plater, read_ledger(*plater));
    if (ledger.project_id.empty()) return;
    if (count_model_parts(*plater, ledger) && !write_ledger(*plater, ledger)) return;
    CallAfter([this] { refresh(); });
}

void ProductionPanel::record_print(int plate_index)
{
    Plater* plater = wxGetApp().plater();
    if (!plater) return;
    ProductionLedger ledger = read_latest_journal(*plater, read_ledger(*plater));
    if (ledger.project_id.empty()) return;
    // Recording a print counts any new parts on the plate first.
    if (count_model_parts(*plater, ledger) && !write_ledger(*plater, ledger)) return;
    const auto parts = plate_parts(*plater, plate_index);
    if (parts.empty()) return;
    PartPlate* plate = plater->get_partplate_list().get_plate(plate_index);
    auto initial = parts;
    for (ProductionRunPart& part : initial) part.accepted = part.printed;
    RecordDialog dialog(this, ledger, initial, from_u8(plate->print_setup().destination_name),
                        [this, plater](const std::string& key) { return preview_for(*plater, key); });
    if (dialog.ShowModal() != wxID_OK) { refresh(); return; }
    ProductionRun run{new_id(), plate->get_plate_name(), dialog.printer(), time_now(),
                      "manual", dialog.is_running() ? "printing" : "accepted", parts, {}};
    if (!dialog.is_running()) run.reviews.push_back({time_now(), dialog.result()});
    if (!dialog.is_running() && std::none_of(run.reviews.back().parts.begin(), run.reviews.back().parts.end(),
                                            [](const ProductionRunPart& part) { return part.accepted > 0; }))
        run.state = "failed";
    ledger.runs.push_back(std::move(run));
    if (!write_ledger(*plater, ledger)) return;
    CallAfter([this] { refresh(); });
}

void ProductionPanel::mark_plate_done(int plate_index)
{
    Plater* plater = wxGetApp().plater();
    if (!plater || plate_index < 0 ||
        plate_index >= int(plater->get_partplate_list().get_plate_list().size())) return;
    ProductionLedger ledger = read_latest_journal(*plater, read_ledger(*plater));
    if (ledger.project_id.empty()) return;
    if (count_model_parts(*plater, ledger) && !write_ledger(*plater, ledger)) return;
    const auto parts = plate_parts(*plater, plate_index);
    if (parts.empty()) return;
    const bool still_needed = std::any_of(parts.begin(), parts.end(), [&](const ProductionRunPart& part) {
        const ProductionPart* target = ledger.find(part.key);
        return target && ledger.accepted(part.key) < target->required;
    });
    if (!still_needed) { refresh(); return; }

    const PartPlate* plate = plater->get_partplate_list().get_plate(plate_index);
    const std::string plate_name = plate->get_plate_name();
    const std::string destination_name = plate->print_setup().destination_name;
    const wxString name = plate->get_plate_name().empty()
        ? format_wxstr(_L("Plate %1%"), plate_index + 1) : from_u8(plate->get_plate_name());
    RichMessageDialog confirm(this, format_wxstr(_L("Did every part on %1% print successfully? Use Record a print if some parts need another try."), name),
                              _L("Mark plate done"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
    if (confirm.ShowModal() != wxID_YES) { refresh(); return; }
    const ProductionLedger latest = read_latest_journal(*plater, read_ledger(*plater));
    const auto& current_plates = plater->get_partplate_list().get_plate_list();
    if (latest.project_id != ledger.project_id || latest.revision != ledger.revision ||
        plate_index >= int(current_plates.size()) ||
        current_plates[plate_index]->get_plate_name() != plate_name ||
        current_plates[plate_index]->print_setup().destination_name != destination_name ||
        !ProductionLedger::same_plate_parts(parts, plate_parts(*plater, plate_index))) {
        show_info(this, _L("This plate changed while you were confirming it. Check the plate and try again."));
        refresh();
        return;
    }
    ledger.record_plate_done(parts, plate_name, destination_name,
                             new_id(), time_now());
    if (write_ledger(*plater, ledger)) CallAfter([this] { refresh(); });
}

void ProductionPanel::undo_mark_plate_done(const std::string& run_id)
{
    Plater* plater = wxGetApp().plater();
    if (!plater) return;
    ProductionLedger ledger = read_latest_journal(*plater, read_ledger(*plater));
    const auto run = std::find_if(ledger.runs.begin(), ledger.runs.end(),
                                  [&](const ProductionRun& item) { return item.id == run_id; });
    if (run == ledger.runs.end()) {
        show_info(this, _L("This completed print is no longer available to undo."));
        refresh();
        return;
    }
    const auto parts = current_parts_for_run(all_plate_parts(*plater), *run);
    if (ledger.mark_done_undo_status(run_id, parts) == ProductionLedger::MarkDoneUndoStatus::PlateChanged) {
        show_info(this, _L("This plate has changed since it was marked done. Check Print history before changing its result."));
        refresh();
        return;
    }
    if (!ledger.undo_mark_done(run_id, parts)) {
        show_info(this, _L("This completed print is no longer available to undo."));
        refresh();
        return;
    }
    if (write_ledger(*plater, ledger)) CallAfter([this] { refresh(); });
}

void ProductionPanel::review_run(const std::string& run_id)
{
    Plater* plater = wxGetApp().plater();
    if (!plater) return;
    ProductionLedger ledger = read_latest_journal(*plater, read_ledger(*plater));
    auto it = std::find_if(ledger.runs.begin(), ledger.runs.end(),
                           [&](const ProductionRun& run) { return run.id == run_id; });
    if (it == ledger.runs.end()) return;
    auto initial = it->reviews.empty() ? it->parts : it->reviews.back().parts;
    if (it->reviews.empty())
        for (ProductionRunPart& part : initial) part.accepted = part.printed;
    RecordDialog dialog(this, ledger, initial, from_u8(it->printer_name),
                        [this, plater](const std::string& key) { return preview_for(*plater, key); },
                        it->reviews.empty());
    if (dialog.ShowModal() != wxID_OK) return;
    if (dialog.is_running()) {
        it->state = "printing";
    } else {
        it->reviews.push_back({time_now(), dialog.result()});
        it->state = std::any_of(it->reviews.back().parts.begin(), it->reviews.back().parts.end(),
                                [](const ProductionRunPart& part) { return part.accepted > 0; }) ? "accepted" : "failed";
    }
    if (!write_ledger(*plater, ledger)) return;
    CallAfter([this] { refresh(); });
}

void ProductionPanel::record_sent(int plate_index, const std::string& printer_name)
{
    Plater* plater = wxGetApp().plater();
    if (!plater || plate_index < 0 || plate_index >= int(plater->get_partplate_list().get_plate_list().size())) return;
    ProductionLedger ledger;
    try { ledger = read_latest_journal(*plater, read_ledger(*plater)); } catch (const std::exception&) { return; }
    if (ledger.project_id.empty()) return;
    count_model_parts(*plater, ledger);
    auto parts = plate_parts(*plater, plate_index);
    if (parts.empty()) return;
    PartPlate* plate = plater->get_partplate_list().get_plate(plate_index);
    ledger.runs.push_back({new_id(), plate->get_plate_name(), printer_name, time_now(),
                           "porca_send", "sent", std::move(parts)});
    if (!write_ledger(*plater, ledger)) return;
    if (ProductionPanel* panel = ProductionPanel::if_built()) panel->refresh();
}

void ProductionPanel::restore_journal()
{
    Plater* plater = wxGetApp().plater();
    if (!plater) return;
    try {
        const ProductionLedger saved = read_ledger(*plater);
        if (saved.project_id.empty()) return;
        const ProductionLedger latest = read_latest_journal(*plater, saved);
        if (latest.revision == saved.revision) return;
        plater->model().model_info->metadata_items[ProductionLedger::metadata_key] = latest.serialize();
        plater->set_plater_dirty(true);
        plater->update_title_dirty_status();
    } catch (const std::exception&) { /* Keep the saved project's record visible. */ }
}

} // namespace Slic3r::GUI
