#pragma once

#include <nlohmann/json.hpp>
#include <array>
#include <string>
#include <vector>

namespace sketch {

enum class CoordinatedViewKind { plan, elevation, section };
enum class ViewDetail { coarse, medium, fine };
struct ViewPresentation {
    double cut_depth_m{1.2};
    double far_depth_m{100.0};
    double cut_line_mm{0.5};
    double projection_line_mm{0.18};
    bool hatch_enabled{true};
    std::string hatch_pattern{"solid"};
    double hatch_scale{1.0};
    ViewDetail detail{ViewDetail::medium};
    bool operator==(const ViewPresentation&) const = default;
};
struct CoordinatedView {
    std::string id;
    std::string name;
    CoordinatedViewKind kind{CoordinatedViewKind::plan};
    std::array<double, 3> origin_m{0, 0, 0};
    std::array<double, 3> direction{0, 0, -1};
    std::array<double, 3> up{0, 1, 0};
    ViewPresentation presentation;
    bool operator==(const CoordinatedView&) const = default;
};
struct SheetRect {
    double x_mm{};
    double y_mm{};
    double width_mm{100};
    double height_mm{100};
    bool operator==(const SheetRect&) const = default;
};
struct SheetViewport {
    std::string id;
    std::string view_id;
    SheetRect bounds;
    // Model/paper ratio: 100 means 1:100. Independent of every other viewport.
    double scale_denominator{100};
    bool operator==(const SheetViewport&) const = default;
};
struct SheetTitleBlock {
    std::string project;
    std::string title;
    std::string author;
    std::string issue_date;
    bool operator==(const SheetTitleBlock&) const = default;
};
struct SheetRevision {
    std::string id;
    std::string date;
    std::string description;
    bool operator==(const SheetRevision&) const = default;
};
struct SheetCallout {
    std::string id;
    std::string label;
    std::string target_sheet_id;
    std::string target_viewport_id;
    double x_mm{};
    double y_mm{};
    bool operator==(const SheetCallout&) const = default;
};
struct SheetSchedulePlacement {
    std::string id;
    std::string schedule_id;
    SheetRect bounds;
    bool operator==(const SheetSchedulePlacement&) const = default;
};
struct DrawingSheet {
    std::string id;
    std::string number;
    double width_mm{420};
    double height_mm{297};
    SheetTitleBlock title_block;
    std::vector<SheetRevision> revisions;
    std::vector<SheetViewport> viewports;
    std::vector<SheetCallout> callouts;
    std::vector<SheetSchedulePlacement> schedules;
    bool operator==(const DrawingSheet&) const = default;
};

// Validated, immutable semantic snapshot. Input and output values are detached.
class SheetViewModel final {
public:
    [[nodiscard]] static SheetViewModel create(std::vector<CoordinatedView> views,
        std::vector<DrawingSheet> sheets, std::vector<std::string> schedule_ids = {});
    [[nodiscard]] static SheetViewModel from_json(const nlohmann::json& value);
    [[nodiscard]] nlohmann::json to_json() const;
    [[nodiscard]] const std::vector<CoordinatedView>& views() const noexcept { return views_; }
    [[nodiscard]] const std::vector<DrawingSheet>& sheets() const noexcept { return sheets_; }
    // Updates the shared view definition; viewport references and scales survive.
    [[nodiscard]] SheetViewModel with_view(CoordinatedView replacement) const;
    // Updates one sheet definition while revalidating every viewport, callout,
    // schedule placement and cross-sheet reference in the detached snapshot.
    [[nodiscard]] SheetViewModel with_sheet(DrawingSheet replacement) const;
    // Appends a new drawing sheet after validating all of its viewports,
    // callouts, schedules, and identity/number uniqueness against the graph.
    [[nodiscard]] SheetViewModel with_added_sheet(DrawingSheet addition) const;
    // Removes one drawing sheet. The graph must retain at least one sheet and
    // no surviving callout may target the removed sheet.
    [[nodiscard]] SheetViewModel with_removed_sheet(const std::string& sheet_id) const;
    // Updates one viewport within a sheet while preserving its linked view and
    // revalidating all page and callout references.
    [[nodiscard]] SheetViewModel with_viewport(const std::string& sheet_id,
                                               SheetViewport replacement) const;
    // Updates one schedule placement within a sheet while preserving its
    // registry identity and revalidating the page bounds and schedule link.
    [[nodiscard]] SheetViewModel with_schedule_placement(
        const std::string& sheet_id, SheetSchedulePlacement replacement) const;
    // Updates one revision within a sheet while preserving its stable ID.
    [[nodiscard]] SheetViewModel with_revision(const std::string& sheet_id,
                                               SheetRevision replacement) const;
    // Appends a validated revision to a sheet.
    [[nodiscard]] SheetViewModel with_added_revision(const std::string& sheet_id,
                                                     SheetRevision addition) const;
    // Removes one revision from a sheet.
    [[nodiscard]] SheetViewModel with_removed_revision(const std::string& sheet_id,
                                                       const std::string& revision_id) const;
    // Updates one cross-sheet callout while preserving its stable ID.
    [[nodiscard]] SheetViewModel with_callout(const std::string& sheet_id,
                                              SheetCallout replacement) const;
    // Appends a validated cross-sheet callout to a sheet.
    [[nodiscard]] SheetViewModel with_added_callout(const std::string& sheet_id,
                                                    SheetCallout addition) const;
    // Removes one cross-sheet callout from a sheet.
    [[nodiscard]] SheetViewModel with_removed_callout(const std::string& sheet_id,
                                                      const std::string& callout_id) const;
private:
    SheetViewModel() = default;
    std::vector<CoordinatedView> views_;
    std::vector<DrawingSheet> sheets_;
    std::vector<std::string> schedule_ids_;
};

} // namespace sketch
