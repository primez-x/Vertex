#pragma once

#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <vector>

namespace sketch {

using ProjectAttributes = std::map<std::string, std::string>;

struct ProjectSubject {
    std::string name;
    std::string address;
    std::string reference;
    ProjectAttributes attributes;
    bool operator==(const ProjectSubject&) const = default;
};

// Caller-owned stable IDs are preserved verbatim; these are logical model links,
// not embedded geometry or references validated against a Document snapshot.
struct SharedProjectModel {
    std::string id;
    std::string name;
    ProjectAttributes attributes;
    bool operator==(const SharedProjectModel&) const = default;
};

struct ProjectArea {
    std::string id;
    std::string model_id;
    std::string name;
    ProjectAttributes attributes;
    bool operator==(const ProjectArea&) const = default;
};

struct PageModelView {
    std::string model_id;
    bool visible = true;
    bool operator==(const PageModelView&) const = default;
};

struct ProjectPage {
    std::string id;
    std::string title;
    double sheet_width_mm = 210;
    double sheet_height_mm = 297;
    // A value of 100 means 1:100. This is independent of navigation zoom.
    double scale_denominator = 100;
    double zoom = 1;
    double center_x_metres = 0;
    double center_y_metres = 0;
    bool show_grid = true;
    std::vector<PageModelView> models;
    ProjectAttributes attributes;
    bool operator==(const ProjectPage&) const = default;
};

struct MultipageProject {
    std::string id;
    ProjectSubject subject;
    ProjectAttributes shared_data;
    std::vector<SharedProjectModel> models;
    std::vector<ProjectArea> areas;
    // Vector order is presentation order; IDs do not depend on this order.
    std::vector<ProjectPage> pages;
    bool operator==(const MultipageProject&) const = default;
};

// Throws std::invalid_argument for invalid state or an unsupported JSON schema.
void validate_multipage_project(const MultipageProject& project);
nlohmann::json multipage_project_to_json(const MultipageProject& project);
MultipageProject multipage_project_from_json(const nlohmann::json& value);

} // namespace sketch
