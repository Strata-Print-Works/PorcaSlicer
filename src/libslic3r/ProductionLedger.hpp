#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

// Portable, optional project record. A part key identifies a model instance
// independently of its position in the model or workspace.
struct ProductionPart {
    std::string key;
    std::string name;
    int required = 1;
};

struct ProductionRunPart {
    std::string key;
    int printed = 1;
    int accepted = 0;
    std::string transform;
};

struct ProductionReview {
    std::string recorded_at;
    std::vector<ProductionRunPart> parts;
    // Empty for ordinary reviews; "manual" means Mark plate done created the run.
    std::string mark_done_previous_state;
};

struct ProductionRun {
    std::string id;
    std::string plate_name;
    std::string printer_name;
    std::string recorded_at;
    std::string source; // manual or porca_send
    std::string state;  // sent, printing, accepted, failed
    std::vector<ProductionRunPart> parts; // immutable plate snapshot
    std::vector<ProductionReview> reviews; // corrections append rather than changing the snapshot
};

struct ProductionLedger {
    static constexpr const char* metadata_key = "porca_production_v1";
    std::string project_id;
    uint64_t revision = 0;
    std::vector<ProductionPart> parts;
    std::vector<ProductionRun> runs;

    enum class MarkDoneUndoStatus { Available, PlateChanged, Unavailable };

    static bool same_plate_parts(const std::vector<ProductionRunPart>& a,
                                 const std::vector<ProductionRunPart>& b)
    {
        return a.size() == b.size() && std::is_permutation(a.begin(), a.end(), b.begin(), b.end(),
            [](const ProductionRunPart& left, const ProductionRunPart& right) {
                return left.key == right.key && left.printed == right.printed &&
                       left.transform == right.transform;
            });
    }

    std::string record_plate_done(const std::vector<ProductionRunPart>& on_plate,
                                  const std::string& plate_name, const std::string& printer_name,
                                  const std::string& new_run_id, const std::string& now)
    {
        if (on_plate.empty()) return {};
        auto accepted_parts = on_plate;
        for (ProductionRunPart& part : accepted_parts) part.accepted = part.printed;
        auto pending = std::find_if(runs.rbegin(), runs.rend(), [&](const ProductionRun& run) {
            return (run.state == "sent" || run.state == "printing") && run.reviews.empty() &&
                   same_plate_parts(run.parts, on_plate);
        });
        if (pending != runs.rend()) {
            const std::string previous_state = pending->state;
            pending->reviews.push_back({now, std::move(accepted_parts), previous_state});
            pending->state = "accepted";
            return pending->id;
        }
        if (new_run_id.empty() || std::any_of(runs.begin(), runs.end(),
                [&](const ProductionRun& run) { return run.id == new_run_id; }))
            return {};
        runs.push_back({new_run_id, plate_name, printer_name, now, "manual", "accepted", on_plate,
                        {{now, std::move(accepted_parts), "manual"}}});
        return new_run_id;
    }

    MarkDoneUndoStatus mark_done_undo_status(const std::string& run_id,
                                              const std::vector<ProductionRunPart>& current_parts) const
    {
        const auto it = std::find_if(runs.begin(), runs.end(), [&](const ProductionRun& run) { return run.id == run_id; });
        if (it == runs.end() || it->state != "accepted" || it->reviews.empty())
            return MarkDoneUndoStatus::Unavailable;
        const std::string& previous = it->reviews.back().mark_done_previous_state;
        if (previous != "sent" && previous != "printing" &&
            !(previous == "manual" && it->source == "manual" && it->reviews.size() == 1))
            return MarkDoneUndoStatus::Unavailable;
        return same_plate_parts(it->parts, current_parts) ? MarkDoneUndoStatus::Available : MarkDoneUndoStatus::PlateChanged;
    }

    bool undo_mark_done(const std::string& run_id, const std::vector<ProductionRunPart>& current_parts)
    {
        if (mark_done_undo_status(run_id, current_parts) != MarkDoneUndoStatus::Available) return false;
        auto it = std::find_if(runs.begin(), runs.end(), [&](const ProductionRun& run) { return run.id == run_id; });
        const std::string previous = it->reviews.back().mark_done_previous_state;
        if (previous == "manual") {
            runs.erase(it);
        } else {
            it->reviews.pop_back();
            it->state = previous;
        }
        return true;
    }

    int accepted(const std::string& key) const
    {
        int count = 0;
        for (const ProductionRun& run : runs)
            for (const ProductionRunPart& part : run.reviews.empty() ? run.parts : run.reviews.back().parts)
                if (part.key == key) count += part.accepted;
        return count;
    }

    int required_total() const
    {
        int count = 0;
        for (const ProductionPart& part : parts) count += part.required;
        return count;
    }

    int ready_total() const
    {
        int count = 0;
        for (const ProductionPart& part : parts)
            count += std::min(part.required, accepted(part.key));
        return count;
    }

    ProductionPart* find(const std::string& key)
    {
        auto it = std::find_if(parts.begin(), parts.end(), [&](const ProductionPart& part) { return part.key == key; });
        return it == parts.end() ? nullptr : &*it;
    }

    std::string serialize() const;
    // Throws for unsupported versions and invalid records.
    static ProductionLedger deserialize(const std::string& text);

    // Names the local journal of one saved project file. Save As keeps the
    // project identity, so a copy's unsaved progress must never restore the
    // original. The result is a fixed-length lowercase hex name that never
    // contains the path itself.
    static std::string journal_location(const std::string& project_file);
};

} // namespace Slic3r
