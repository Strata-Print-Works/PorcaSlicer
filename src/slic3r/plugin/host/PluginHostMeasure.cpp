#include "PluginHostBindings.hpp"
#include "slic3r/plugin/PluginBindingUtils.hpp"
#include "slic3r/plugin/PluginUiThread.hpp"

#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Point.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/Plater.hpp>

#include <algorithm>
#include <cmath>
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

} // namespace

// Measurement of the objects on the live plate, as a plain-data snapshot.
void host_bindings::register_measure(py::module_& host)
{
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
}

} // namespace Slic3r
