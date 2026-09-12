#pragma once

#include "sketch/document.hpp"
#include "sketch/document_schedule_adapter.hpp"
#include "sketch/geometry.hpp"
#include "sketch/boundary_authoring_session.hpp"
#include "sketch/assistance_engine.hpp"

#include <QMainWindow>
#include <QString>

#include <memory>
#include <vector>

namespace sketch::desktop {

enum class Workspace {
    measurement,
    architectural,
};

// Reusable native workspace shell. Both tabs render and edit the same Document.
// Recovery archives retain a separate workspace authority for lossless save;
// direct Document edits to those archives cannot yet be saved through this shell.
class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(std::shared_ptr<Document> document = {}, QWidget* parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    [[nodiscard]] Document& document() noexcept;
    [[nodiscard]] const Document& document() const noexcept;
    // Read-only schedule projection bound to the current shared document
    // revision. Malformed rows are returned as explicit diagnostics.
    [[nodiscard]] DocumentScheduleProjection scheduleSnapshot() const;
    // Applies an editable schedule source-cell change through the ordinary
    // Document history. Calculated cells, malformed values, and stale edits
    // are rejected and reported through lastError().
    [[nodiscard]] bool editScheduleCell(const QString& object_id,
                                        const QString& column,
                                        const QString& replacement);
    // Updates the active drawing sheet title block through the typed sheet
    // codec and normal Document history.
    [[nodiscard]] bool editSheetMetadata(const QString& sheet_id,
                                         const QString& number,
                                         const QString& project,
                                         const QString& title,
                                         const QString& author,
                                         const QString& issue_date);
    // Updates one persisted sheet viewport through typed validation and normal
    // Document history. Coordinates and dimensions are millimetres; the scale
    // denominator is the model-to-paper ratio (100 means 1:100).
    [[nodiscard]] bool editSheetViewport(const QString& sheet_id,
                                         const QString& viewport_id,
                                         const QString& x_mm,
                                         const QString& y_mm,
                                         const QString& width_mm,
                                         const QString& height_mm,
                                         const QString& scale_denominator);
    // Updates one persisted schedule placement's bounds through typed
    // validation and normal Document history. Coordinates and dimensions are
    // millimetres; its schedule registry identity is preserved.
    [[nodiscard]] bool editSheetSchedulePlacement(const QString& sheet_id,
                                                  const QString& placement_id,
                                                  const QString& x_mm,
                                                  const QString& y_mm,
                                                  const QString& width_mm,
                                                  const QString& height_mm);
    // Creates a validated drawing sheet with one independently scaled
    // viewport per coordinated view. The returned ID is stable in the
    // document and the new page becomes the selected output sheet.
    [[nodiscard]] QString createDrawingSheet(const QString& number,
                                             const QString& width_mm,
                                             const QString& height_mm,
                                             const QString& title);
    // Removes a drawing sheet through typed graph validation. The last sheet
    // and sheets targeted by surviving callouts cannot be removed.
    [[nodiscard]] bool removeDrawingSheet(const QString& sheet_id);
    // Selects which persisted sheet is used by draft PDF/SVG/print output.
    // Selection is local presentation state and does not dirty the document.
    [[nodiscard]] bool selectOutputSheet(const QString& sheet_id);
    [[nodiscard]] QString outputSheetId() const;
    // Updates persisted architectural view presentation settings through
    // typed validation and normal Document history.
    [[nodiscard]] bool editArchitecturalViewPresentation(
        const QString& view_id,
        const QString& cut_depth_m,
        const QString& far_depth_m,
        const QString& cut_line_mm,
        const QString& projection_line_mm,
        bool hatch_enabled,
        const QString& hatch_pattern,
        const QString& hatch_scale,
        const QString& detail);

    [[nodiscard]] Workspace workspace() const noexcept;
    void setWorkspace(Workspace workspace);
    [[nodiscard]] bool workspaceDocumentsShareDocument() const noexcept;

    [[nodiscard]] bool metricUnits() const noexcept;
    void setMetricUnits(bool metric);

    [[nodiscard]] QString activeLayerId() const;
    [[nodiscard]] bool setActiveLayer(const QString& layer_id);
    [[nodiscard]] bool setContainerVisible(const QString& entity_id, bool visible);
    void showAllContainers();
    [[nodiscard]] bool entityVisible(const QString& entity_id) const;
    // The active design phase is a persisted semantic selection.  An empty
    // value denotes the shared existing baseline.
    [[nodiscard]] QString activeRemodelingAlternative() const;
    [[nodiscard]] bool selectRemodelingAlternative(const QString& alternative_id);
    [[nodiscard]] QString createBuilding(const QString& property_id, const QString& name,
        std::optional<Revision> expected_revision = std::nullopt);
    [[nodiscard]] QString createFloor(const QString& building_id, const QString& name,
        std::optional<Revision> expected_revision = std::nullopt);
    [[nodiscard]] QString createLayer(const QString& floor_id, const QString& name,
        std::optional<Revision> expected_revision = std::nullopt);
    [[nodiscard]] bool renameOrganizationEntity(const QString& entity_id, const QString& name,
        std::optional<Revision> expected_revision = std::nullopt);

    // Starts the same session used by the drawing tools. Define First requests
    // classification if none is supplied; Draw First classifies after closure.
    [[nodiscard]] bool beginBoundaryDrawing(BoundaryAuthoringMode mode,
                                            QString classification = {});
    // Explicit geometry creation remains available to adapters and smoke tests.
    // Interactive boundary tools use the receipt-bearing authoring session.
    [[nodiscard]] QString createBoundary(
        const Boundary& boundary,
        QString classification = QStringLiteral("measurement"),
        std::optional<Revision> expected_revision = std::nullopt);
    // Creates a room boundary as a distinct architectural semantic object.
    // Its geometry remains analytical and independent from appraisal
    // measurement boundaries, while explicit relationships can connect them.
    [[nodiscard]] QString createRoomBoundary(
        const Boundary& boundary,
        QString classification = QStringLiteral("room"),
        std::optional<Revision> expected_revision = std::nullopt);
    [[nodiscard]] QString createStraightWall(
        Vec2 start,
        Vec2 end,
        QString classification = QStringLiteral("interior"),
        std::optional<Revision> expected_revision = std::nullopt);
    // The object dialog and smoke tests share this atomic, validated command.
    // A stale dialog revision cannot overwrite intervening document edits.
    [[nodiscard]] QString commitBuildingObject(
        Entity candidate, std::uint64_t expected_revision, bool replace_selected = false);
    // Creates one opening hosted by the currently selected wall. All numeric
    // values accept the same imperial/metric quantity syntax as the inspector
    // (for example, "3 ft" or "900 mm"). The command previews the complete
    // wall in the architecture kernel before it reaches the document.
    [[nodiscard]] QString createHostedOpening(
        QString kind,
        QString offset,
        QString width,
        QString sill,
        QString height,
        std::optional<Revision> expected_revision = std::nullopt);
    // Creates a slab from the currently selected closed boundary. The direct
    // boundary overload is used by deterministic visual smoke fixtures while
    // keeping the interactive selected-boundary workflow on the same command.
    [[nodiscard]] QString createSlabFromSelectedBoundary(
        QString thickness,
        QString elevation,
        std::optional<Revision> expected_revision = std::nullopt);
    [[nodiscard]] QString createSlabFromBoundary(
        const Boundary& boundary,
        QString thickness,
        QString elevation,
        std::vector<Boundary> holes = {},
        std::optional<Revision> expected_revision = std::nullopt);
    [[nodiscard]] bool selectEntity(const QString& entity_id);
    [[nodiscard]] QString selectedEntityId() const;
    [[nodiscard]] bool editSelectedClassification(const QString& classification);
    [[nodiscard]] bool editSelectedLength(const QString& expression);
    [[nodiscard]] bool editSelectedHeight(const QString& expression);
    [[nodiscard]] bool editSelectedThickness(const QString& expression);
    [[nodiscard]] bool editSelectedFactor(const QString& expression);
    [[nodiscard]] bool setSelectedCalculationRule(bool include_in_building,
                                                   bool include_in_living);
    // Presentation annotations are persisted inside the typed annotation
    // entity. These commands keep labels/symbols undoable and portable rather
    // than creating renderer-only state.
    [[nodiscard]] QString createAnnotationLabel(const QString& template_id,
                                                const QString& content,
                                                Vec2 position);
    [[nodiscard]] QString createAnnotationSymbol(const QString& symbol_id,
                                                 Vec2 position);
    // Updates one persisted annotation child through the same typed Document
    // command/history path as creation and deletion. Coordinates are metres;
    // rotation is expressed in degrees for UI/API ergonomics.
    [[nodiscard]] bool editAnnotation(const QString& annotation_id,
                                      const QString& content,
                                      const QString& x_metres,
                                      const QString& y_metres,
                                      const QString& rotation_degrees,
                                      const QString& scale,
                                      bool visible);
    [[nodiscard]] bool deleteAnnotation(const QString& annotation_id);
    // Imports a local raster or first-page PDF into the project Asset store and
    // creates a reference_asset entity with an explicit, editable calibration
    // scale. PDF source bytes remain alongside a deterministic local preview.
    [[nodiscard]] QString importReferenceImage(const QString& path);
    // Calibrates a reference from two source-pixel points and a known local
    // distance expression (for example, "12 ft" or "3.5 m"). The source
    // points and original expression remain inspectable project metadata.
    [[nodiscard]] bool calibrateReference(const QString& reference_id,
                                          const QString& first_x,
                                          const QString& first_y,
                                          const QString& second_x,
                                          const QString& second_y,
                                          const QString& known_distance);
    [[nodiscard]] bool editReferenceTransform(const QString& reference_id,
                                              const QString& x_metres,
                                              const QString& y_metres,
                                              const QString& metres_per_source_unit,
                                              const QString& scale,
                                              const QString& rotation_degrees,
                                              const QString& intensity,
                                              bool flip_horizontal,
                                              bool flip_vertical,
                                              bool visible);
    // Starts the normal receipt-bearing boundary authoring workflow with the
    // selected reference retained as the tracing context. Geometry remains
    // authored model data; the reference image is never a measurement source.
    [[nodiscard]] bool beginReferenceTrace();
    // Optional deterministic assistance is session-scoped and starts disabled.
    // Suggestions are unverified values until this API accepts one through the
    // normal typed command/history path.
    [[nodiscard]] bool assistanceEnabled() const noexcept;
    void setAssistanceEnabled(bool enabled);
    [[nodiscard]] std::vector<AssistanceProposal> suggestReferenceAssistance(
        const QString& reference_id, AssistanceKind kind);
    [[nodiscard]] std::vector<AssistanceProposal> suggestLabelAssistance();
    [[nodiscard]] std::vector<AssistanceProposal> parseAssistanceCommand(
        const QString& command);
    [[nodiscard]] bool acceptAssistanceProposal(const AssistanceProposal& proposal);
    [[nodiscard]] bool undoCommand();
    [[nodiscard]] bool redoCommand();

    [[nodiscard]] bool createNewProject();
    [[nodiscard]] bool openProject(const QString& path);
    [[nodiscard]] bool saveProject();
    [[nodiscard]] bool saveProjectAs(const QString& path);
    // Session-specific recovery copy, separate from the ordinary destination.
    // Empty until scheduling begins for a workspace-backed recovery project.
    [[nodiscard]] QString recoveryCopyPath() const;
    [[nodiscard]] bool exportDraftPdf(const QString& path);
    [[nodiscard]] bool exportDraftSvg(const QString& path);
    [[nodiscard]] bool exportNativeViewImage(const QString& path);
    [[nodiscard]] bool showPrintPreview();

    void showCommandPalette();
    void showAnnotationEditor();
    void showReferenceImport();
    void showReferenceCalibration();
    void showAssistance();
    void showRemodelingAlternatives();
    void showRoomRelationships();
    void showVerticalLevels();
    void showAssemblies();
    void showConstraintEditor();
    void fitView();

    [[nodiscard]] QString lastError() const;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace sketch::desktop
