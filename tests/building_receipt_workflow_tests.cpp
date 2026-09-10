#include "sketch/desktop/building_object_dialog.hpp"
#include "sketch/desktop/main_window.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QTemporaryDir>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void exercise_all_form_receipts() {
    using namespace sketch;
    using namespace sketch::desktop;
    struct Case { const char* type; const char* form; const char* field; const char* pointer; const char* other_field; };
    constexpr std::array cases{
        Case{"column", "rectangular_column", "buildingObjectWidth", "/width_m", "buildingObjectHeight"},
        Case{"column", "circular_column", "buildingObjectRadius", "/radius_m", "buildingObjectHeight"},
        Case{"beam", "straight_beam", "buildingObjectStartX", "/start_m/0", "buildingObjectDepth"},
        Case{"stair", "straight_stair_flight", "buildingObjectGoing", "/going_m", "buildingObjectWidth"},
        Case{"roof", "sloped_roof_panel", "buildingObjectRun", "/run_m", "buildingObjectThickness"},
        Case{"roof", "gable_roof", "buildingObjectLength", "/length_m", "buildingObjectThickness"},
    };
    MainWindow window;
    QTemporaryDir directory;
    require(directory.isValid(), "could not reserve receipt fixture directory");
    std::vector<std::pair<std::string, nlohmann::json>> expected;
    for (const auto& test : cases) {
        BuildingObjectDialog dialog(std::nullopt, false);
        auto* type = dialog.findChild<QComboBox*>("buildingObjectType");
        auto* form = dialog.findChild<QComboBox*>("buildingObjectForm");
        require(type && form, "missing form selectors");
        type->setCurrentIndex(type->findData(test.type));
        form->setCurrentIndex(form->findData(test.form));
        require(type->currentIndex() >= 0 && form->currentIndex() >= 0, "missing architectural form");
        auto* field = dialog.findChild<QLineEdit*>(test.field);
        require(field, "missing exact quantity field");
        field->setText("1/3 ft");
        require(dialog.submit() && dialog.candidate(), "fractional form failed to submit");
        auto candidate = *dialog.candidate();
        const auto& receipt = candidate.properties.at("quantity_entries").at(test.pointer);
        require(receipt.at("original_expression") == "1/3 ft" && receipt.at("entered_unit") == "ft" &&
                receipt.at("exact_metres").at("numerator") == 127 &&
                receipt.at("exact_metres").at("denominator") == 1250,
                "form lost exact fractional provenance at its canonical pointer");
        const auto id = window.commitBuildingObject(candidate, window.document().revision());
        if (id.isEmpty()) throw std::runtime_error(std::string(test.form) + ": " + window.lastError().toStdString());
        auto original = window.document().snapshot().entities().at(id.toStdString());
        // Simulate an imported future record. New authoring cannot invent
        // unknown quantity fields, but unrelated edits must preserve them.
        original.properties["future_dimension"] = 17;
        original.properties["quantity_entries"]["/future_dimension"] = {{"version", 99}, {"opaque", {1, 2, 3}}};
        window.document().apply(ApplyEntityChanges{window.document().revision(),
            {EntityChange::upsert(original)}, {}, "import future quantity metadata"});
        const auto receipts = original.properties.at("quantity_entries");
        BuildingObjectDialog edit(original, true);
        auto* other = edit.findChild<QLineEdit*>(test.other_field);
        require(other, "missing unrelated editable field");
        other->setText(std::string_view(test.type) == "column" ? "4 m" : "0.5 m");
        require(edit.submit() && edit.candidate(), "unrelated geometry edit failed");
        require(window.selectEntity(id), "cannot select created form");
        require(!window.commitBuildingObject(*edit.candidate(), window.document().revision(), true).isEmpty(),
                "unrelated dialog edit did not commit");
        const auto edited = window.document().snapshot().entities().at(id.toStdString());
        for (const auto* pointer : {test.pointer, "/future_dimension"})
            require(edited.properties.at("quantity_entries").at(pointer) == receipts.at(pointer),
                    "unrelated edit changed exact or opaque provenance");
        require(window.undoCommand() && window.document().snapshot().entities().at(id.toStdString()) == original,
                "undo failed to restore full form and receipt state");
        require(window.redoCommand() && window.document().snapshot().entities().at(id.toStdString()) == edited,
                "redo failed to restore full edited form and receipt state");
        expected.emplace_back(id.toStdString(), edited.properties.at("quantity_entries"));
    }
    const auto path = directory.filePath("all-form-receipts.bldproj");
    require(window.saveProjectAs(path) && window.openProject(path), "receipt fixture save/reopen failed");
    for (const auto& [id, receipts] : expected)
        require(window.document().snapshot().entities().at(id).properties.at("quantity_entries") == receipts,
                "save/reopen changed exact or opaque receipts");
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication app(argc, argv);
    try {
        exercise_all_form_receipts();
        std::cout << "All six form receipt workflows passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "building_receipt_workflow_tests: " << error.what() << '\n';
        return 1;
    }
}
