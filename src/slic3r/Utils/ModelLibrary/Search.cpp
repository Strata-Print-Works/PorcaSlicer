#include "Library.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::ModelLibrary {
namespace {
std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}

std::string like(const std::string& value)
{
    std::string result = "%";
    for (char c : value) {
        if (c == '%' || c == '_' || c == '\\') result += '\\';
        result += c;
    }
    return result + '%';
}

std::vector<std::pair<bool, std::string>> words(const std::string& query)
{
    std::vector<std::pair<bool, std::string>> result;
    for (size_t i = 0; i < query.size();) {
        while (i < query.size() && std::isspace(static_cast<unsigned char>(query[i]))) ++i;
        if (i == query.size()) break;
        const bool negative = query[i] == '-' && i + 1 < query.size() && !std::isspace(static_cast<unsigned char>(query[i + 1]));
        if (negative) ++i;
        bool quoted = false;
        std::string word;
        while (i < query.size()) {
            const char c = query[i++];
            if (c == '"') quoted = !quoted;
            else if (!quoted && std::isspace(static_cast<unsigned char>(c))) break;
            else word += c;
        }
        if (!word.empty()) result.emplace_back(negative, std::move(word));
    }
    return result;
}

struct Number { std::string op; double value; };
std::optional<Number> number(std::string text, bool dimension)
{
    std::string op = "=";
    if (!text.empty() && (text[0] == '<' || text[0] == '>' || text[0] == '=')) {
        op = text.substr(0, 1);
        text.erase(0, 1);
        if (!text.empty() && text[0] == '=') { if (op != "=") op += '='; text.erase(0, 1); }
    }
    double scale = 1;
    if (dimension && text.size() >= 2) {
        const auto unit = text.substr(text.size() - 2);
        if (unit == "mm" || unit == "cm") {
            text.resize(text.size() - 2);
            if (unit == "cm") scale = 10;
        }
    }
    try {
        size_t consumed = 0;
        const double value = std::stod(text, &consumed) * scale;
        if (consumed == text.size() && std::isfinite(value)) return Number{op, value};
    } catch (...) {}
    return std::nullopt;
}
} // namespace

std::string search_clause(const std::string& query, Json& args, bool& archived)
{
    std::string result;
    const auto bind = [&args](Json value) { args.push_back(std::move(value)); return "?" + std::to_string(args.size()); };
    const auto exists = [](const std::string& condition) {
        return "EXISTS (SELECT 1 FROM files f WHERE f.model_id=m.id AND " + condition + ")";
    };
    const std::string tags = "EXISTS (SELECT 1 FROM model_tags mt JOIN tags t ON t.id=mt.tag_id WHERE mt.model_id=m.id AND mt.source<>'hidden'";
    for (const auto& [negative, original] : words(query)) {
        std::string word = original;
        if (word[0] == '#' && word.size() > 1) word = "tag:" + word.substr(1);
        if ((word[0] == '<' || word[0] == '>') && (word.find("mm") != std::string::npos || word.find("cm") != std::string::npos)) word = "size:" + word;
        const size_t colon = word.find(':');
        const std::string key = colon == std::string::npos ? "" : lower(word.substr(0, colon));
        const std::string value = colon == std::string::npos ? word : word.substr(colon + 1);
        const std::string v = lower(value);
        std::string clause;
        if (key == "is" && v == "archived") { archived = !negative; continue; }
        if (key == "tag") clause = tags + " AND t.name LIKE " + bind(like(value)) + " ESCAPE '\\')";
        else if (key == "by" || key == "designer") clause = "m.designer LIKE " + bind(like(value)) + " ESCAPE '\\'";
        else if (key == "from" || key == "source" || key == "site") {
            if (v == "local") clause = "(m.source_url IS NULL OR m.source_url='' OR m.source_url LIKE '%localhost%' OR m.source_url LIKE '%127.0.0.1%' OR m.source_site='Local tools')";
            else { const auto n = bind(like(value)); clause = "(m.source_site LIKE " + n + " ESCAPE '\\' OR m.source_url LIKE " + n + " ESCAPE '\\')"; }
        } else if (key == "in" || key == "collection") clause = "EXISTS (SELECT 1 FROM collection_models cm JOIN collections c ON c.id=cm.collection_id WHERE cm.model_id=m.id AND c.name LIKE " + bind(like(value)) + " ESCAPE '\\')";
        else if (key == "is") {
            if (v == "fav" || v == "favorite" || v == "favourite" || v == "starred") clause = "m.favorite=1";
            else if (v == "sliced") clause = exists("f.sliced=1");
            else if (v == "printed" || v == "unprinted") clause = std::string(v == "unprinted" ? "NOT " : "") + "EXISTS (SELECT 1 FROM prints p WHERE p.model_id=m.id)";
            else if (v == "failed") clause = "EXISTS (SELECT 1 FROM prints p WHERE p.model_id=m.id AND p.outcome='failed')";
            else if (v == "stacked" || v == "version") clause = "(SELECT count(*) FROM models s WHERE s.stack_id=m.stack_id AND s.archived_at IS NULL)>1";
            else if (v == "pack") clause = "(SELECT count(*) FROM files f WHERE f.model_id=m.id)>1";
            else if (v == "new" || v == "recent") clause = "m.added_at>=date('now','-7 days')";
            else if (v == "tagged") clause = tags + ')';
            else clause = "0";
        } else if (key == "has") {
            if (v == "images" || v == "image" || v == "photos" || v == "photo") clause = exists("f.kind='image'");
            else if (v == "docs" || v == "doc" || v == "readme") clause = exists("f.kind='doc'");
            else if (v == "notes" || v == "note") clause = "m.notes<>''";
            else if (v == "link" || v == "url" || v == "source") clause = "m.source_url IS NOT NULL";
            else if (v == "tags" || v == "tag") clause = tags + ')';
            else if (v == "designer") clause = "m.designer IS NOT NULL";
            else if (v == "errors" || v == "error") clause = exists("f.error IS NOT NULL");
            else clause = exists("f.kind='model' AND f.format=" + bind(v == "stp" ? "step" : v));
        } else if (key == "printer" || key == "filament" || key == "material" || key == "nozzle" || key == "layer") {
            const std::map<std::string, std::string> keys{{"printer", "Printer"}, {"filament", "Filament"}, {"material", "Filament"}, {"nozzle", "Nozzle"}, {"layer", "Layer height"}};
            const auto k = bind(keys.at(key));
            const auto n = bind(like(value));
            clause = "EXISTS (SELECT 1 FROM files f,json_each(f.meta) j WHERE f.model_id=m.id AND json_extract(j.value,'$.key')=" + k + " AND json_extract(j.value,'$.value') LIKE " + n + " ESCAPE '\\')";
        } else if (key == "size" || key == "parts" || key == "files") {
            if (const auto num = number(v, key == "size")) {
                const auto n = bind(num->value);
                if (key == "size") clause = exists("f.kind='model' AND f.size_x IS NOT NULL AND " + (num->op == "=" ? "abs(max(f.size_x,f.size_y,f.size_z)-" + n + ")<0.5" : "max(f.size_x,f.size_y,f.size_z)" + num->op + n));
                else if (std::floor(num->value) == num->value) clause = "(SELECT count(*) FROM files f WHERE f.model_id=m.id AND f.kind='model')" + num->op + n;
            }
        } else if (key == "added") {
            if (v.size() > 2 && (v[0] == '<' || v[0] == '>')) {
                const auto num = number(v.substr(1, v.size() - 2), false);
                const std::map<char, int> units{{'d', 1}, {'w', 7}, {'m', 30}, {'y', 365}};
                const auto unit = units.find(v.back());
                if (num && num->value >= 0 && num->value < 1000000 && unit != units.end())
                    clause = "m.added_at" + std::string(v[0] == '<' ? ">=" : "<") + "date('now'," + bind("-" + std::to_string(static_cast<long long>(num->value * unit->second)) + " days") + ")";
            } else clause = "m.added_at LIKE " + bind(value + '%');
        }
        if (clause.empty()) {
            const auto n = bind(like(original));
            clause = "(m.name LIKE " + n + " ESCAPE '\\' OR m.designer LIKE " + n + " ESCAPE '\\' OR m.description LIKE " + n + " ESCAPE '\\' OR m.notes LIKE " + n + " ESCAPE '\\' OR m.source_url LIKE " + n + " ESCAPE '\\' OR " + exists("f.original_name LIKE " + n + " ESCAPE '\\'") + " OR " + tags + " AND t.name LIKE " + n + " ESCAPE '\\'))";
        }
        result += " AND " + std::string(negative ? "NOT " : "") + "coalesce((" + clause + "),0)";
    }
    return result;
}
} // namespace Slic3r::ModelLibrary
