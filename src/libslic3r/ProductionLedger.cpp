#include "ProductionLedger.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace Slic3r {

std::string ProductionLedger::serialize() const
{
    nlohmann::json value = {{"version", 1}, {"project_id", project_id}, {"revision", revision},
                            {"parts", nlohmann::json::array()},
                            {"runs", nlohmann::json::array()}};
    for (const ProductionPart& part : parts)
        value["parts"].push_back({{"key", part.key}, {"name", part.name}, {"required", part.required}});
    for (const ProductionRun& run : runs) {
        nlohmann::json item = {{"id", run.id}, {"plate_name", run.plate_name}, {"printer_name", run.printer_name},
                               {"recorded_at", run.recorded_at}, {"source", run.source}, {"state", run.state},
                               {"parts", nlohmann::json::array()}};
        for (const ProductionRunPart& part : run.parts)
            item["parts"].push_back({{"key", part.key}, {"printed", part.printed},
                                     {"accepted", part.accepted}, {"transform", part.transform}});
        item["reviews"] = nlohmann::json::array();
        for (const ProductionReview& review : run.reviews) {
            nlohmann::json result = {{"recorded_at", review.recorded_at}, {"parts", nlohmann::json::array()}};
            if (!review.mark_done_previous_state.empty())
                result["mark_done_previous_state"] = review.mark_done_previous_state;
            for (const ProductionRunPart& part : review.parts)
                result["parts"].push_back({{"key", part.key}, {"printed", part.printed},
                                           {"accepted", part.accepted}, {"transform", part.transform}});
            item["reviews"].push_back(std::move(result));
        }
        value["runs"].push_back(std::move(item));
    }
    return value.dump();
}

ProductionLedger ProductionLedger::deserialize(const std::string& text)
{
    const nlohmann::json value = nlohmann::json::parse(text);
    if (!value.is_object() || !value.contains("version") || value.at("version") != 1)
        throw std::runtime_error("This production record requires a newer version of Porca.");
    ProductionLedger ledger;
    ledger.project_id = value.at("project_id").get<std::string>();
    if (ledger.project_id.empty() || ledger.project_id.size() > 64 ||
        !std::all_of(ledger.project_id.begin(), ledger.project_id.end(),
                     [](unsigned char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                                                    (ch >= '0' && ch <= '9') || ch == '-'; }))
        throw std::runtime_error("Invalid production project ID.");
    ledger.revision = value.value("revision", uint64_t(0));
    for (const auto& item : value.at("parts")) {
        ProductionPart part{item.at("key").get<std::string>(), item.at("name").get<std::string>(),
                            item.at("required").get<int>()};
        if (part.key.empty() || part.required < 0 || ledger.find(part.key))
            throw std::runtime_error("Invalid production part.");
        ledger.parts.push_back(std::move(part));
    }
    for (const auto& item : value.at("runs")) {
        ProductionRun run{item.at("id").get<std::string>(), item.at("plate_name").get<std::string>(),
                          item.at("printer_name").get<std::string>(), item.at("recorded_at").get<std::string>(),
                          item.at("source").get<std::string>(), item.at("state").get<std::string>(), {}};
        if (run.id.empty() || std::any_of(ledger.runs.begin(), ledger.runs.end(),
                [&](const ProductionRun& existing) { return existing.id == run.id; }) ||
            (run.state != "sent" && run.state != "printing" && run.state != "accepted" && run.state != "failed"))
            throw std::runtime_error("Invalid production run.");
        for (const auto& entry : item.at("parts")) {
            ProductionRunPart part{entry.at("key").get<std::string>(), entry.at("printed").get<int>(),
                                   entry.at("accepted").get<int>(), entry.value("transform", std::string{})};
            if (part.printed < 0 || part.accepted < 0 || part.accepted > part.printed || !ledger.find(part.key) ||
                std::any_of(run.parts.begin(), run.parts.end(),
                    [&](const ProductionRunPart& existing) { return existing.key == part.key; }))
                throw std::runtime_error("Invalid production result.");
            run.parts.push_back(std::move(part));
        }
        if (item.contains("reviews")) {
            for (const auto& review_item : item.at("reviews")) {
                ProductionReview review{review_item.at("recorded_at").get<std::string>(), {},
                                        review_item.value("mark_done_previous_state", std::string{})};
                if (!review.mark_done_previous_state.empty() && review.mark_done_previous_state != "manual" &&
                    review.mark_done_previous_state != "sent" && review.mark_done_previous_state != "printing")
                    throw std::runtime_error("Invalid production review.");
                for (const auto& entry : review_item.at("parts")) {
                    ProductionRunPart part{entry.at("key").get<std::string>(), entry.at("printed").get<int>(),
                                           entry.at("accepted").get<int>(), entry.value("transform", std::string{})};
                    const auto on_plate = std::find_if(run.parts.begin(), run.parts.end(),
                        [&](const ProductionRunPart& source) { return source.key == part.key && source.printed == part.printed; });
                    if (part.printed < 0 || part.accepted < 0 || part.accepted > part.printed || on_plate == run.parts.end() ||
                        std::any_of(review.parts.begin(), review.parts.end(),
                            [&](const ProductionRunPart& existing) { return existing.key == part.key; }))
                        throw std::runtime_error("Invalid production review.");
                    review.parts.push_back(std::move(part));
                }
                run.reviews.push_back(std::move(review));
            }
        }
        ledger.runs.push_back(std::move(run));
    }
    return ledger;
}

std::string ProductionLedger::journal_location(const std::string& project_file)
{
    // FNV-1a: stable across runs and platforms, unlike std::hash.
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char ch : project_file) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (size_t i = result.size(); i-- > 0; hash >>= 4) result[i] = digits[hash & 0xf];
    return result;
}

} // namespace Slic3r
