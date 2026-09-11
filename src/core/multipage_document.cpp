#include "sketch/multipage_document.hpp"

#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {
using json = nlohmann::json;
void check(bool valid, const char* message) {
    if (!valid) throw std::invalid_argument(message);
}
void text(const std::string& value) {
    check(value.size() <= 16384 && value.find('\0') == std::string::npos, "Invalid project text");
}
void attributes(const ProjectAttributes& values) {
    check(values.size() <= 256, "Too many project attributes");
    for (const auto& [key, value] : values) {
        check(!key.empty() && key.size() <= 256, "Invalid project attribute key");
        text(key); text(value);
    }
}
void id(std::set<std::string>& ids, const std::string& value) {
    check(!value.empty() && value.size() <= 256, "Invalid project ID");
    text(value);
    check(ids.insert(value).second, "Duplicate project ID");
}
void fields(const json& j, std::initializer_list<std::string_view> names) {
    check(j.is_object() && j.size() == names.size(), "Invalid multipage JSON fields");
    for (auto name : names) check(j.contains(std::string(name)), "Missing multipage JSON field");
}
void array(const json& j, std::size_t limit) {
    check(j.is_array() && j.size() <= limit, "Invalid multipage JSON collection");
}
ProjectAttributes read_attributes(const json& j) {
    check(j.is_object() && j.size() <= 256, "Invalid project attributes");
    return j.get<ProjectAttributes>();
}
}

void validate_multipage_project(const MultipageProject& p) {
    check(p.models.size() <= 10000 && p.areas.size() <= 10000 &&
          !p.pages.empty() && p.pages.size() <= 1000, "Invalid project collection size");
    std::set<std::string> ids, model_ids;
    id(ids, p.id);
    text(p.subject.name); text(p.subject.address); text(p.subject.reference);
    attributes(p.subject.attributes); attributes(p.shared_data);
    for (const auto& m : p.models) {
        id(ids, m.id); model_ids.insert(m.id); text(m.name); attributes(m.attributes);
    }
    for (const auto& a : p.areas) {
        id(ids, a.id); text(a.name); attributes(a.attributes);
        check(model_ids.contains(a.model_id), "Area refers to unknown shared model");
    }
    for (const auto& page : p.pages) {
        id(ids, page.id); text(page.title); attributes(page.attributes);
        for (double v : {page.sheet_width_mm, page.sheet_height_mm, page.scale_denominator, page.zoom})
            check(std::isfinite(v) && v > 0, "Invalid page size, scale, or zoom");
        check(std::isfinite(page.center_x_metres) && std::isfinite(page.center_y_metres), "Invalid page center");
        check(page.models.size() <= 10000, "Too many page model links");
        std::set<std::string> linked;
        for (const auto& view : page.models) {
            check(model_ids.contains(view.model_id), "Page refers to unknown shared model");
            check(linked.insert(view.model_id).second, "Duplicate page model link");
        }
    }
}

json multipage_project_to_json(const MultipageProject& p) {
    validate_multipage_project(p);
    json result = {{"schema", "sketch.multipage-project"}, {"version", 1}, {"id", p.id},
        {"subject", {{"name", p.subject.name}, {"address", p.subject.address},
            {"reference", p.subject.reference}, {"attributes", p.subject.attributes}}},
        {"shared_data", p.shared_data}, {"models", json::array()},
        {"areas", json::array()}, {"pages", json::array()}};
    for (const auto& m : p.models)
        result["models"].push_back({{"id", m.id}, {"name", m.name}, {"attributes", m.attributes}});
    for (const auto& a : p.areas)
        result["areas"].push_back({{"id", a.id}, {"model_id", a.model_id}, {"name", a.name}, {"attributes", a.attributes}});
    for (const auto& page : p.pages) {
        json views = json::array();
        for (const auto& view : page.models)
            views.push_back({{"model_id", view.model_id}, {"visible", view.visible}});
        result["pages"].push_back({{"id", page.id}, {"title", page.title},
            {"sheet_width_mm", page.sheet_width_mm}, {"sheet_height_mm", page.sheet_height_mm},
            {"scale_denominator", page.scale_denominator}, {"zoom", page.zoom},
            {"center_x_metres", page.center_x_metres}, {"center_y_metres", page.center_y_metres},
            {"show_grid", page.show_grid}, {"models", views}, {"attributes", page.attributes}});
    }
    return result;
}

MultipageProject multipage_project_from_json(const json& j) {
    try {
        fields(j, {"schema", "version", "id", "subject", "shared_data", "models", "areas", "pages"});
        check(j.at("schema") == "sketch.multipage-project" && j.at("version").is_number_integer() &&
              j.at("version") == 1, "Unsupported multipage project schema");
        MultipageProject p;
        p.id = j.at("id").get<std::string>();
        const auto& s = j.at("subject");
        fields(s, {"name", "address", "reference", "attributes"});
        p.subject = {s.at("name").get<std::string>(), s.at("address").get<std::string>(),
            s.at("reference").get<std::string>(), read_attributes(s.at("attributes"))};
        p.shared_data = read_attributes(j.at("shared_data"));
        array(j.at("models"), 10000); array(j.at("areas"), 10000); array(j.at("pages"), 1000);
        for (const auto& m : j.at("models")) {
            fields(m, {"id", "name", "attributes"});
            p.models.push_back({m.at("id").get<std::string>(), m.at("name").get<std::string>(), read_attributes(m.at("attributes"))});
        }
        for (const auto& a : j.at("areas")) {
            fields(a, {"id", "model_id", "name", "attributes"});
            p.areas.push_back({a.at("id").get<std::string>(), a.at("model_id").get<std::string>(),
                a.at("name").get<std::string>(), read_attributes(a.at("attributes"))});
        }
        for (const auto& v : j.at("pages")) {
            fields(v, {"id", "title", "sheet_width_mm", "sheet_height_mm", "scale_denominator", "zoom",
                "center_x_metres", "center_y_metres", "show_grid", "models", "attributes"});
            ProjectPage page;
            page.id = v.at("id").get<std::string>(); page.title = v.at("title").get<std::string>();
            page.sheet_width_mm = v.at("sheet_width_mm").get<double>();
            page.sheet_height_mm = v.at("sheet_height_mm").get<double>();
            page.scale_denominator = v.at("scale_denominator").get<double>(); page.zoom = v.at("zoom").get<double>();
            page.center_x_metres = v.at("center_x_metres").get<double>(); page.center_y_metres = v.at("center_y_metres").get<double>();
            page.show_grid = v.at("show_grid").get<bool>(); page.attributes = read_attributes(v.at("attributes"));
            array(v.at("models"), 10000);
            for (const auto& m : v.at("models")) {
                fields(m, {"model_id", "visible"});
                page.models.push_back({m.at("model_id").get<std::string>(), m.at("visible").get<bool>()});
            }
            p.pages.push_back(std::move(page));
        }
        validate_multipage_project(p);
        return p;
    } catch (const json::exception& error) {
        throw std::invalid_argument(std::string("Invalid multipage project JSON: ") + error.what());
    }
}
} // namespace sketch
