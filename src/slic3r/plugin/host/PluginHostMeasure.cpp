#include "PluginHostBindings.hpp"
#include "slic3r/plugin/PluginBindingUtils.hpp"
#include "slic3r/plugin/PluginUiThread.hpp"

#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/Layer.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Point.hpp>
#include <libslic3r/Print.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/Plater.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace py = pybind11;

namespace Slic3r {
namespace {

// --------------------------------------------------------------------------
// Measuring the plate.
//
// host.model() hands out a live reference into the scene graph, which a plugin
// thread cannot safely walk: the user moving or deleting an object mid-walk
// frees the very nodes being read. So the whole walk happens on the UI thread
// via run_on_ui_blocking and the caller gets a value snapshot back — plain
// numbers, no handles into anything that can go away.
//
// run_on_ui_blocking releases the GIL for the duration of the call, so NOTHING
// below the `collect_*` line may touch a py:: object: the structs here are pure
// C++ and every Python object is built in measure_objects(), after the call has
// returned and the GIL is back.
// --------------------------------------------------------------------------

// Overhang angle measured from vertical, the way slicers and their users talk
// about it: 0 deg is a vertical wall, 45 deg the classic printable limit,
// 90 deg a flat ceiling that has to be bridged.
constexpr double DEFAULT_OVERHANG_THRESHOLD_DEG = 45.0;

// A face whose highest point is this close to the plate is lying on it, and is
// held up by the bed rather than by air. Without this every object that has a
// flat bottom would report a 90 deg overhang and the number would mean nothing.
// Deliberately z-absolute: an object floating above the bed keeps its bottom
// face in the analysis, because there it really is an unsupported ceiling.
constexpr double BED_CONTACT_TOLERANCE_MM = 0.05;

struct OverhangMeasurement
{
    double threshold_deg     = 0.0;
    double area_mm2          = 0.0; // downward-facing area steeper than the threshold
    double downward_area_mm2 = 0.0; // all downward-facing area, threshold or not
    double steepest_deg      = 0.0;
};

struct ObjectMeasurement
{
    std::string         name;
    size_t              instance_count = 0;
    size_t              triangle_count = 0;
    size_t              vertex_count   = 0;
    double              volume_mm3     = 0.0;
    bool                is_manifold    = true;
    bool                has_size       = false;
    Vec3d               size           = Vec3d::Zero();
    double              min_z          = 0.0;
    OverhangMeasurement overhangs;
};

// Accumulate one volume's downward-facing triangles, in world space.
//
// The face normal is taken from the world-space triangle rather than
// transformed from the local one: that is correct under non-uniform scale,
// where the local normal and the world normal are not parallel. A mirrored
// (left-handed) transform reverses the winding, and with it the sign of the
// cross product, so the normal is flipped back when the determinant is
// negative.
void accumulate_overhangs(const indexed_triangle_set& its,
                          const Transform3d&          world,
                          bool                        winding_flipped,
                          double                      threshold_deg,
                          OverhangMeasurement&        out)
{
    for (const stl_triangle_vertex_indices& face : its.indices) {
        const Vec3d a = world * its.vertices[face(0)].cast<double>();
        const Vec3d b = world * its.vertices[face(1)].cast<double>();
        const Vec3d c = world * its.vertices[face(2)].cast<double>();

        const Vec3d  cross = (b - a).cross(c - a);
        const double norm  = cross.norm();
        if (norm <= 0.0)
            continue; // degenerate triangle: no area, no normal

        Vec3d normal = cross / norm;
        if (winding_flipped)
            normal = -normal;

        // Component along straight down (0, 0, -1); positive means the face
        // points downwards, i.e. there is nothing under it but air or bed.
        const double downward = -normal.z();
        if (downward <= 0.0)
            continue;

        // Resting on the build plate, so not an overhang.
        if (std::max({ a.z(), b.z(), c.z() }) <= BED_CONTACT_TOLERANCE_MM)
            continue;

        const double area = 0.5 * norm;
        out.downward_area_mm2 += area;

        // acos(downward) is the normal's angle from straight down; the overhang
        // angle is its complement, so it reduces to asin(downward).
        const double overhang_deg = Geometry::rad2deg(std::asin(std::clamp(downward, -1.0, 1.0)));
        out.steepest_deg          = std::max(out.steepest_deg, overhang_deg);
        if (overhang_deg > threshold_deg)
            out.area_mm2 += area;
    }
}

// Runs on the UI thread with the GIL released — pure C++ only, no py:: here.
std::vector<ObjectMeasurement> collect_measurements(double threshold_deg)
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    GUI::Plater* plater = GUI::wxGetApp().plater();
    if (plater == nullptr)
        throw std::runtime_error("Plater is not available");

    const Model& model = plater->model();

    std::vector<ObjectMeasurement> measured;
    measured.reserve(model.objects.size());

    for (const ModelObject* object : model.objects) {
        // An object with no instances is not on the plate (it exists only
        // transiently, mid-import), and has no world placement to report.
        if (object == nullptr || object->instances.empty())
            continue;

        ObjectMeasurement out;
        out.name                    = object->name;
        out.instance_count          = object->instances.size();
        out.overhangs.threshold_deg = threshold_deg;

        // Placement-dependent numbers are measured on the first instance.
        // Copies share the object's geometry and, as OrcaSlicer arranges them,
        // its orientation — only their XY offset differs.
        const ModelInstance& instance    = *object->instances.front();
        const Transform3d    inst_matrix = instance.get_transformation().get_matrix();

        for (const ModelVolume* volume : object->volumes) {
            // Modifiers, support blockers and negative volumes are not printed
            // material, and instance_bounding_box() ignores them too.
            if (volume == nullptr || !volume->is_model_part())
                continue;

            const TriangleMesh&         mesh = volume->mesh();
            const indexed_triangle_set& its  = mesh.its;

            out.triangle_count += its.indices.size();
            out.vertex_count += its.vertices.size();
            if (!mesh.stats().manifold())
                out.is_manifold = false;

            const Transform3d world = inst_matrix * volume->get_matrix();
            // An affine map scales volume by |det| of its linear part, which
            // covers non-uniform scaling and mirroring alike. The mesh's own
            // volume is in local mm^3 (stats are filled at construction;
            // recompute on the -1 "never computed" sentinel).
            const double determinant  = world.linear().determinant();
            double       local_volume = mesh.stats().volume;
            if (local_volume < 0.0)
                local_volume = its_volume(its);
            out.volume_mm3 += local_volume * std::abs(determinant);

            accumulate_overhangs(its, world, determinant < 0.0, threshold_deg, out.overhangs);
        }

        const BoundingBoxf3 bbox = object->instance_bounding_box(instance);
        if (bbox.defined) {
            out.has_size = true;
            out.size     = bbox.size();
            out.min_z    = bbox.min.z();
        }

        measured.push_back(std::move(out));
    }

    return measured;
}

py::list measure_objects(double threshold_deg)
{
    if (!(threshold_deg >= 0.0) || threshold_deg > 90.0)
        throw std::invalid_argument("overhang_threshold_deg must be between 0 and 90 "
                                    "(degrees from vertical: 0 is a vertical wall, "
                                    "90 a flat ceiling)");

    // The model walk happens entirely on the UI thread and hands back plain C++
    // values. No py:: object is built inside it: the GIL is released there.
    const std::vector<ObjectMeasurement> measured =
        run_on_ui_blocking([threshold_deg]() { return collect_measurements(threshold_deg); });

    // GIL reacquired — Python objects from here on.
    py::list objects;
    for (const ObjectMeasurement& measurement : measured) {
        py::dict overhangs;
        overhangs["threshold_deg"]     = measurement.overhangs.threshold_deg;
        overhangs["area_mm2"]          = measurement.overhangs.area_mm2;
        overhangs["downward_area_mm2"] = measurement.overhangs.downward_area_mm2;
        overhangs["steepest_deg"]      = measurement.overhangs.steepest_deg;

        py::dict object;
        object["name"]        = measurement.name;
        object["instances"]   = measurement.instance_count;
        object["triangles"]   = measurement.triangle_count;
        object["vertices"]    = measurement.vertex_count;
        object["volume_mm3"]  = measurement.volume_mm3;
        object["is_manifold"] = measurement.is_manifold;
        object["size_mm"]     = measurement.has_size ? vec3_to_tuple(measurement.size)
                                                     : py::make_tuple(0.0, 0.0, 0.0);
        object["min_z_mm"]    = measurement.min_z;
        object["overhangs"]   = overhangs;
        objects.append(object);
    }
    return objects;
}

// --------------------------------------------------------------------------
// Watching the plate.
//
// measure_objects reads every triangle, so it is not something a plugin can sit
// and poll. plate_signature answers the far cheaper question asked in between —
// "has anything moved?" — from identity and placement alone: object and volume
// ids, volume types and matrices, instance transforms and printable flags. No
// mesh is touched, so the call costs the same on a cube as on a 10M-triangle
// scan, and a plugin can run it every couple of seconds and only measure when
// the answer differs.
//
// Same threading rules as above: the walk is marshaled to the UI thread with
// the GIL released, and the dict is built afterwards.
// --------------------------------------------------------------------------

// Transform components are snapped to this grid before they are hashed, so that
// floating-point noise on its own cannot churn the signature. 1e-4 mm is 0.1 um:
// roughly a thousandth of a fine layer and a four-thousandth of a nozzle width,
// far under anything the slicer or the motion system can act on. The same step
// covers the dimensionless linear part, where 1e-4 is 0.01 mm of drift across a
// 100 mm part — still invisible in the G-code.
constexpr double SIGNATURE_QUANTUM = 1e-4;

// Quantised components go through int64. Plate coordinates are three digits of
// mm, so this bound is never approached; it is here so that a corrupt transform
// saturates instead of overflowing the conversion.
constexpr double SIGNATURE_QUANTUM_LIMIT = 9.0e18;

// FNV-1a, 64-bit, rather than std::hash: std::hash is only required to be
// consistent within one run of one implementation, and this value is a plugin's
// notion of "the same plate", which has to survive being compared against itself.
// Feeding a byte at a time keeps the result independent of host endianness.
// 64 bits is ample for the job — a plugin compares one signature against the
// previous one, so a collision has to strike two *consecutive* plate states.
class SignatureHasher
{
public:
    void feed_word(uint64_t word)
    {
        for (int shift = 0; shift < 64; shift += 8) {
            m_state ^= (word >> shift) & 0xFFull;
            m_state *= FNV_PRIME;
        }
    }

    void feed_flag(bool flag) { feed_word(flag ? 1ull : 0ull); }

    // Snap onto the quantum grid, then hash the integer. Rounding also folds
    // -0.0 onto +0.0, which hashing the raw bit pattern would not.
    void feed_scalar(double value)
    {
        // llround of a non-finite value is undefined. Hash a fixed marker so a
        // broken transform still gives a stable, repeatable signature.
        if (!std::isfinite(value)) {
            feed_word(NON_FINITE_MARKER);
            return;
        }
        const double scaled = std::clamp(value / SIGNATURE_QUANTUM, -SIGNATURE_QUANTUM_LIMIT,
                                         SIGNATURE_QUANTUM_LIMIT);
        feed_word(static_cast<uint64_t>(std::llround(scaled)));
    }

    // Rows 0..2 carry the whole affine map, linear part and translation both;
    // row 3 of an Eigen affine transform is always (0, 0, 0, 1).
    void feed_transform(const Transform3d& transform)
    {
        const auto& matrix = transform.matrix();
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 4; ++col)
                feed_scalar(matrix(row, col));
    }

    std::string hex() const
    {
        static constexpr char DIGITS[] = "0123456789abcdef";

        std::string out(16, '0');
        uint64_t    state = m_state;
        for (int i = 15; i >= 0; --i) {
            out[i] = DIGITS[state & 0xFull];
            state >>= 4;
        }
        return out;
    }

private:
    static constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ull;
    static constexpr uint64_t FNV_PRIME        = 1099511628211ull;
    // Not a value any quantised plate coordinate can produce.
    static constexpr uint64_t NON_FINITE_MARKER = 0x4e6f6e46696e6974ull;

    uint64_t m_state = FNV_OFFSET_BASIS;
};

struct PlateSignature
{
    size_t      object_count = 0;
    std::string digest;
};

// Runs on the UI thread with the GIL released — pure C++ only, no py:: here.
PlateSignature collect_plate_signature()
{
    if (wxTheApp == nullptr)
        throw std::runtime_error("OrcaSlicer application is not initialized");

    GUI::Plater* plater = GUI::wxGetApp().plater();
    if (plater == nullptr)
        throw std::runtime_error("Plater is not available");

    const Model& model = plater->model();

    SignatureHasher hasher;
    PlateSignature  out;

    // Every list is length-prefixed, and a missing node feeds the zero that no
    // valid id can be. Without that, "two volumes and no instances" and "one
    // volume and one instance" could hand the hasher the same words and collide
    // by construction rather than by luck.
    hasher.feed_word(model.objects.size());

    for (const ModelObject* object : model.objects) {
        if (object == nullptr) {
            hasher.feed_word(0ull);
            continue;
        }

        // Counted the way measure_objects counts, so this is a preview of the
        // length of its list; an object with no instances is not on the plate.
        // It still goes into the hash, so an import in flight reads as a change.
        if (!object->instances.empty())
            ++out.object_count;

        hasher.feed_word(object->id().id);
        hasher.feed_flag(object->printable);

        hasher.feed_word(object->volumes.size());
        for (const ModelVolume* volume : object->volumes) {
            if (volume == nullptr) {
                hasher.feed_word(0ull);
                continue;
            }
            hasher.feed_word(volume->id().id);
            // A part demoted to a modifier keeps both its id and its placement,
            // and set_type() does not reissue the id, so the type has to be in
            // here or the material vanishing from the print goes unnoticed.
            hasher.feed_word(static_cast<uint64_t>(static_cast<int>(volume->type())));
            hasher.feed_transform(volume->get_matrix());
        }

        hasher.feed_word(object->instances.size());
        for (const ModelInstance* instance : object->instances) {
            if (instance == nullptr) {
                hasher.feed_word(0ull);
                continue;
            }
            hasher.feed_flag(instance->printable);
            hasher.feed_transform(instance->get_matrix());
        }
    }

    out.digest = hasher.hex();
    return out;
}

py::dict plate_signature()
{
    // Same discipline as measure_objects: the walk runs on the UI thread with
    // the GIL released and hands back plain C++ values.
    const PlateSignature signature = run_on_ui_blocking([]() { return collect_plate_signature(); });

    // GIL reacquired — Python objects from here on.
    py::dict result;
    result["objects"]   = signature.object_count;
    result["signature"] = signature.digest;
    return result;
}

// --------------------------------------------------------------------------
// Measuring the slice.
//
// measure_objects above reads the model — what the user put on the plate.
// This reads the result of slicing it, which is where the failures actually
// show. An overhang is only a problem once you know the layer that has to span
// it; a footprint is only too small once it has been cut into islands. Reading
// the mesh can only ever approximate both.
//
// Meaningful only after a slice: before one this reports sliced = false and
// stops. That is not an error, it is the normal state of a plate someone is
// still arranging.
//
// Same threading rule as the model walk, for a sharper reason: the background
// slicer replaces these layers wholesale when it re-runs, so a plugin thread
// walking them races a free. Everything happens on the UI thread and comes
// back as plain numbers.
// --------------------------------------------------------------------------

struct SliceMeasurement
{
    bool   sliced                        = false;
    int    object_count                  = 0;
    int    layer_count                   = 0;
    double max_z_mm                      = 0.0;
    double first_layer_area_mm2          = 0.0;
    int    first_layer_islands           = 0;
    double smallest_first_layer_island_mm2 = 0.0;
    double overhang_area_mm2             = 0.0;
    double max_layer_overhang_mm2        = 0.0;
    double max_overhang_layer_z_mm       = 0.0;
    double bridge_area_mm2               = 0.0;
    bool   has_support                   = false;
    double support_top_z_mm              = 0.0;
    double tallest_aspect_ratio          = 0.0;
};

SliceMeasurement collect_slice_measurement()
{
    SliceMeasurement out;

    GUI::Plater* plater = GUI::wxGetApp().plater();
    if (plater == nullptr)
        return out;

    const Print& print = plater->fff_print();
    // Scaled area -> mm^2, the conversion the slicer's own statistics use.
    const double sf2 = SCALING_FACTOR * SCALING_FACTOR;

    for (const PrintObject* po : print.objects()) {
        // Adaptors, not vectors: layers() on a const PrintObject hands back a view.
        const auto& layers = po->layers();
        if (layers.empty())
            continue;                    // this object is not sliced yet
        out.sliced = true;
        ++out.object_count;
        out.layer_count = std::max(out.layer_count, (int) layers.size());

        // The first layer is what bed adhesion rests on, and it is a single
        // small island that lifts — so the smallest one is worth more than the
        // total.
        const Layer* first = layers.front();
        double       object_min_xy = 0.0;
        BoundingBox  footprint;
        bool         have_footprint = false;
        for (const ExPolygon& island : first->lslices) {
            const double area_mm2 = island.area() * sf2;
            out.first_layer_area_mm2 += area_mm2;
            ++out.first_layer_islands;
            if (out.smallest_first_layer_island_mm2 == 0.0 ||
                area_mm2 < out.smallest_first_layer_island_mm2)
                out.smallest_first_layer_island_mm2 = area_mm2;
            const BoundingBox island_bb = get_extents(island);
            if (have_footprint)
                footprint.merge(island_bb);
            else {
                footprint      = island_bb;
                have_footprint = true;
            }
        }
        if (have_footprint)
            object_min_xy = std::min(unscale<double>(footprint.size().x()),
                                     unscale<double>(footprint.size().y()));

        double       object_max_z = 0.0;
        const Layer* below        = nullptr;
        for (const Layer* layer : layers) {
            object_max_z = std::max(object_max_z, (double) layer->print_z);

            // Overhang as the slicer sees it: the part of this layer with
            // nothing beneath it. That is the area which has to be bridged,
            // supported or drooped over — a truer number than any face angle
            // measured off the mesh, because it already accounts for the layer
            // height that produced it.
            if (below != nullptr) {
                double            layer_overhang_mm2 = 0.0;
                const ExPolygons  over_air = diff_ex(layer->lslices, below->lslices);
                for (const ExPolygon& e : over_air)
                    layer_overhang_mm2 += e.area() * sf2;
                out.overhang_area_mm2 += layer_overhang_mm2;
                if (layer_overhang_mm2 > out.max_layer_overhang_mm2) {
                    out.max_layer_overhang_mm2  = layer_overhang_mm2;
                    out.max_overhang_layer_z_mm = layer->print_z;
                }
            }
            below = layer;

            // Bridges the slicer itself flagged: the overhang it already
            // decided to span rather than support.
            for (const LayerRegion* region : layer->regions())
                for (const Surface& surface : region->slices.surfaces)
                    if (surface.is_bridge())
                        out.bridge_area_mm2 += surface.area() * sf2;
        }
        out.max_z_mm = std::max(out.max_z_mm, object_max_z);

        // Height against the NARROWER footprint dimension: a tall thin part
        // topples across its narrow side, so that is the ratio that matters.
        if (object_min_xy > 0.0)
            out.tallest_aspect_ratio = std::max(out.tallest_aspect_ratio,
                                                object_max_z / object_min_xy);

        const auto& support_layers = po->support_layers();
        if (!support_layers.empty()) {
            out.has_support     = true;
            out.support_top_z_mm = std::max(out.support_top_z_mm,
                                            (double) support_layers.back()->print_z);
        }
    }
    return out;
}

py::dict measure_slice()
{
    // Same discipline as measure_objects: the walk runs on the UI thread with
    // the GIL released and hands back plain C++ values.
    const SliceMeasurement m = run_on_ui_blocking([]() { return collect_slice_measurement(); });

    // GIL reacquired — Python objects from here on.
    py::dict result;
    result["sliced"] = m.sliced;
    if (!m.sliced)
        return result;                   // nothing else is meaningful yet

    result["objects"]                         = m.object_count;
    result["layers"]                          = m.layer_count;
    result["max_z_mm"]                        = m.max_z_mm;
    result["first_layer_area_mm2"]            = m.first_layer_area_mm2;
    result["first_layer_islands"]             = m.first_layer_islands;
    result["smallest_first_layer_island_mm2"] = m.smallest_first_layer_island_mm2;
    result["overhang_area_mm2"]               = m.overhang_area_mm2;
    result["max_layer_overhang_mm2"]          = m.max_layer_overhang_mm2;
    result["max_overhang_layer_z_mm"]         = m.max_overhang_layer_z_mm;
    result["bridge_area_mm2"]                 = m.bridge_area_mm2;
    result["has_support"]                     = m.has_support;
    result["support_top_z_mm"]                = m.support_top_z_mm;
    result["tallest_aspect_ratio"]            = m.tallest_aspect_ratio;
    return result;
}

} // namespace

// Measurement of the objects on the live plate as a plain-data snapshot, and
// the cheap signature that says when it is worth taking one again.
void host_bindings::register_measure(py::module_& host)
{
    host.def("measure_slice", &measure_slice,
             "Measure the CURRENT SLICE and return a plain dict. Always has `sliced`; when "
             "that is False nothing else is present and there is nothing to read yet — the "
             "plate has not been sliced since it last changed. When True it also has:\n"
             "  objects, layers, max_z_mm,\n"
             "  first_layer_area_mm2, first_layer_islands, smallest_first_layer_island_mm2,\n"
             "  overhang_area_mm2, max_layer_overhang_mm2, max_overhang_layer_z_mm,\n"
             "  bridge_area_mm2, has_support, support_top_z_mm, tallest_aspect_ratio\n"
             "\n"
             "This is the companion to measure_objects, and the better of the two once a "
             "slice exists. measure_objects reads the mesh and can only estimate: it reports "
             "a face angle without knowing the layer height that has to span it. These numbers "
             "come off the sliced layers, so an overhang here is the area actually printed "
             "over air, and the first-layer figures are the islands really laid on the bed. "
             "smallest_first_layer_island_mm2 is usually the interesting one — a plate lifts "
             "at its smallest island, not its total area.\n"
             "\n"
             "Overhang area is per-layer difference against the layer below, summed over the "
             "plate; bridge_area_mm2 is the subset the slicer already decided to bridge. "
             "tallest_aspect_ratio is height over the NARROWER footprint dimension, the axis a "
             "tall part topples across.\n"
             "\n"
             "Like measure_objects the walk runs on the main/UI thread (marshaled from the "
             "plugin thread) — the background slicer replaces these layers when it re-runs, so "
             "reading them off-thread races a free. Do not call it from a SlicingPipeline hook.");

    host.def("measure_objects", &measure_objects,
             py::arg("overhang_threshold_deg") = DEFAULT_OVERHANG_THRESHOLD_DEG,
             "Measure every object on the live plate and return a list of plain dicts:\n"
             "  name, instances, triangles, vertices, volume_mm3, is_manifold,\n"
             "  size_mm (x, y, z), min_z_mm, overhangs\n"
             "where overhangs is {threshold_deg, area_mm2, downward_area_mm2, steepest_deg}.\n"
             "\n"
             "Sizes, min_z and overhangs are world-space, measured on the object's first "
             "instance with its transform applied — rotating a part is the usual fix for an "
             "overhang, so an object-local answer would mislead. volume_mm3 is the mesh "
             "volume scaled by the instance transform. Only model parts count; modifiers, "
             "negative volumes and support blockers are skipped.\n"
             "\n"
             "Overhang angles are measured from vertical: 0 is a vertical wall, 45 the usual "
             "printable limit, 90 a flat ceiling. Faces lying on the build plate are excluded "
             "as bed-supported. steepest_deg is 0 when the object has no overhangs at all.\n"
             "\n"
             "The whole walk runs on the main/UI thread (marshaled from the plugin thread), "
             "so the result cannot be torn by the user editing the plate. Do not call this "
             "from a SlicingPipeline hook, which runs on the slicing worker the UI thread can "
             "be waiting on.");

    host.def("plate_signature", &plate_signature,
             "Cheap change-detector for measure_objects. Returns a dict:\n"
             "  objects   - number of objects on the plate, counted as measure_objects counts them\n"
             "  signature - opaque 16-character string, equal for equal plate states\n"
             "\n"
             "measure_objects reads every triangle and is far too expensive to poll. This reads "
             "no mesh at all — only object and volume ids, volume types and matrices, instance "
             "transforms and printable flags — so it costs the same on a cube as on a "
             "10M-triangle scan. Poll it every couple of seconds and call measure_objects only "
             "when the signature differs from the one you kept.\n"
             "\n"
             "It changes when objects are added or removed, when a volume is added, removed, "
             "retyped (part <-> modifier) or moved within its object, when an instance is added, "
             "removed, moved, rotated, scaled or mirrored, and when a printable flag is toggled "
             "on an object or an instance. It does NOT change when a mesh is mutated in place "
             "without the volume being reissued — writing through the mesh bindings' arrays does "
             "that, though the slicer's own mesh edits (simplify, boolean, cut, split, reload) "
             "all give the volume a new id and so do show up. Renaming an object is invisible "
             "too, even though measure_objects reports the name, and so is painting supports, "
             "seams or filaments — that changes the G-code but no number measure_objects "
             "returns.\n"
             "\n"
             "Transform components are rounded to 1e-4 before hashing, so motion below 0.1 um "
             "does not register and floating-point noise alone cannot churn the value. An empty "
             "plate is not an error: it returns objects = 0 and its own stable signature.\n"
             "\n"
             "The signature is opaque — compare it for equality, do not parse it or keep it "
             "across sessions. It is built from ids handed out per session, so reopening the "
             "same project gives a different string for the same plate.\n"
             "\n"
             "Like measure_objects, the walk runs on the main/UI thread (marshaled from the "
             "plugin thread), so it cannot be torn by the user editing the plate. Do not call it "
             "from a SlicingPipeline hook, which runs on the slicing worker the UI thread can be "
             "waiting on.");
}

} // namespace Slic3r
