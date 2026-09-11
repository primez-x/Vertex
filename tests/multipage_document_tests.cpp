#include "sketch/multipage_document.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void invalid(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Expected invalid_argument");
}
sketch::MultipageProject fixture() {
    sketch::MultipageProject p;
    p.id = "project-1";
    p.subject = {"Example dwelling", "123 Example Street", "JOB-42", {{"owner", "Example owner"}}};
    p.shared_data = {{"author", "Example surveyor"}, {"units", "metres"}};
    p.models = {{"ground", "Ground floor", {{"level", "0"}}}, {"upper", "Upper floor", {}}};
    p.areas = {{"living", "ground", "Living area", {{"classification", "finished"}, {"method", "interior"}}}};
    sketch::ProjectPage first;
    first.id = "sheet-1"; first.title = "Overview";
    first.models = {{"ground", true}, {"upper", false}};
    sketch::ProjectPage second = first;
    second.id = "sheet-2"; second.title = "Detail"; second.scale_denominator = 50;
    second.zoom = 2; second.center_x_metres = -3; second.show_grid = false;
    second.models = {{"ground", false}, {"upper", true}};
    p.pages = {first, second};
    return p;
}
}

int main() {
    try {
        auto p = fixture();
        auto encoded = sketch::multipage_project_to_json(p);
        auto reopened = sketch::multipage_project_from_json(nlohmann::json::parse(encoded.dump()));
        require(reopened == p, "Roundtrip loses metadata, links, order, or view settings");
        require(sketch::multipage_project_to_json(reopened).dump() == encoded.dump(), "JSON is not deterministic");
        std::swap(p.pages[0], p.pages[1]);
        require(sketch::multipage_project_from_json(sketch::multipage_project_to_json(p)) == p, "Reordered pages lost IDs");
        p.pages[0].scale_denominator = 25;
        p.pages[0].models[0].visible = true;
        require(p.pages[1].scale_denominator == 100 && p.models == reopened.models, "Page edit mutates shared model or another view");
        auto bad = fixture(); bad.pages[1].id = bad.pages[0].id;
        invalid([&] { sketch::validate_multipage_project(bad); });
        bad = fixture(); bad.areas[0].model_id = "missing";
        invalid([&] { sketch::validate_multipage_project(bad); });
        bad = fixture(); bad.pages[0].models[0].model_id = "missing";
        invalid([&] { sketch::validate_multipage_project(bad); });
        bad = fixture(); bad.pages[0].models.push_back(bad.pages[0].models[0]);
        invalid([&] { sketch::validate_multipage_project(bad); });
        for (double value : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            bad = fixture(); bad.pages[0].scale_denominator = value;
            invalid([&] { sketch::validate_multipage_project(bad); });
            bad = fixture(); bad.pages[0].zoom = value;
            invalid([&] { sketch::validate_multipage_project(bad); });
            bad = fixture(); bad.pages[0].sheet_width_mm = value;
            invalid([&] { sketch::validate_multipage_project(bad); });
        }
        bad = fixture(); bad.pages.clear();
        invalid([&] { sketch::validate_multipage_project(bad); });
        bad = fixture(); bad.subject.attributes[""] = "empty key";
        invalid([&] { sketch::multipage_project_to_json(bad); });
        for (const auto& key : {"schema", "version", "pages", "subject"}) {
            auto malformed = encoded; malformed.erase(key);
            invalid([&] { sketch::multipage_project_from_json(malformed); });
        }
        auto malformed = encoded; malformed["version"] = 2;
        invalid([&] { sketch::multipage_project_from_json(malformed); });
        malformed = encoded; malformed["version"] = 1.0;
        invalid([&] { sketch::multipage_project_from_json(malformed); });
        malformed = encoded; malformed["extra"] = true;
        invalid([&] { sketch::multipage_project_from_json(malformed); });
        malformed = encoded; malformed["pages"][0]["models"][0]["visible"] = "true";
        invalid([&] { sketch::multipage_project_from_json(malformed); });
        malformed = encoded; malformed["models"] = nlohmann::json::object();
        invalid([&] { sketch::multipage_project_from_json(malformed); });
        std::cout << "multipage_document_tests: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "multipage_document_tests: " << error.what() << '\n';
        return 1;
    }
}
