#pragma once

#include "sketch/document.hpp"
#include "sketch/document_schedule_adapter.hpp"
#include "sketch/geometry.hpp"
#include "sketch/door_operation.hpp"
#include "sketch/boundary_authoring_session.hpp"
#include "sketch/assistance_engine.hpp"
#include "sketch/workspace_accessibility.hpp"

#include <QMainWindow>
#include <QString>
#include <QStringList>

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
    // Updates the persisted property subject record used by multipage output.
    // Attributes must be a JSON object whose values are strings; the complete
    // edit is one normal, undoable Document command.
    [[nodiscard]] bool editProjectSubject(const QString& name,
                                          const QString& address,
                                          const QString& reference,
                                          const QString& attributes_json);
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
    // Adds, edits, and removes revision rows in a drawing sheet through typed
    // Document history. The returned ID is empty when the add is rejected.
    [[nodiscard]] QString addSheetRevision(const QString& sheet_id,
                                           const QString& date,
                                           const QString& description);
    [[nodiscard]] bool editSheetRevision(const QString& sheet_id,
                                         const QString& revision_id,
                                         const QString& date,
                                         const QString& description);
    [[nodiscard]] bool removeSheetRevision(const QString& sheet_id,
                                           const QString& revision_id);
    // Adds, edits, and removes cross-sheet callout markers. Coordinates are
    // millimetres in the owning sheet; target IDs are validated by the graph.
    [[nodiscard]] QString addSheetCallout(const QString& sheet_id,
                                          const QString& label,
                                          const QString& target_sheet_id,
                                          const QString& target_viewport_id,
                                          const QString& x_mm,
                                          const QString& y_mm);
    [[nodiscard]] bool editSheetCallout(const QString& sheet_id,
                                        const QString& callout_id,
                                        const QString& label,
                                        const QString& target_sheet_id,
                                        const QString& target_viewport_id,
                                        const QString& x_mm,
                                        const QString& y_mm);
    [[nodiscard]] bool removeSheetCallout(const QString& sheet_id,
                                          const QString& callout_id);
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
    // Moves one persisted drawing sheet by one page position. Reordering is a
    // normal undoable document command and is retained by save/reopen.
    [[nodiscard]] bool moveDrawingSheet(const QString& sheet_id, int offset);
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
        const QString& detail,
        const QString& object_ids = {});

    [[nodiscard]] Workspace workspace() const noexcept;
    void setWorkspace(Workspace workspace);
    void setWorkspaceTheme(WorkspaceTheme theme);
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
    // Applies the exact automatic-closure helper to an open analytical chain
    // and commits the resulting identified measurement boundary as one
    // undoable command. The source chain is never modified.
    [[nodiscard]] QString createClosedBoundaryFromOpenChain(
        const Boundary& open_chain,
        QString classification = QStringLiteral("measurement"),
        std::optional<Revision> expected_revision = std::nullopt);
    // Builds the documented three-edge bay-window profile, closes it with the
    // same exact closure rule, and commits one identified measurement
    // boundary. Coordinates are model metres.
    [[nodiscard]] QString createBayWindowBoundary(
        Vec2 start,
        Vec2 shoulder1,
        Vec2 shoulder2,
        Vec2 end,
        QString classification = QStringLiteral("measurement"),
        std::optional<Revision> expected_revision = std::nullopt);
    // Creates a room boundary as a distinct architectural semantic object.
    // Its geometry remains analytical and independent from appraisal
    // measurement boundaries, while explicit relationships can connect them.
    [[nodiscard]] QString createRoomBoundary(
        const Boundary& boundary,
        QString classification = QStringLiteral("room"),
        std::optional<Revision> expected_revision = std::nullopt);
    // Builds a room boundary from the connected component of the selected
    // architectural wall geometry. Source walls remain unchanged; the new
    // room boundary is a distinct semantic object and normal undoable command.
    [[nodiscard]] QString createRoomBoundaryFromExistingGeometry(
        QString classification = QStringLiteral("room"),
        std::optional<Revision> expected_revision = std::nullopt);
    // Creates a true architectural room volume from a closed analytical
    // boundary.  The room remains a distinct semantic entity from appraisal
    // measurement boundaries while exposing explicit height/elevation for
    // coordinated elevation, section, native 3D, and quantity output.
    [[nodiscard]] QString createRoomVolumeFromSelectedBoundary(
        QString height,
        QString elevation,
        std::optional<Revision> expected_revision = std::nullopt);
    [[nodiscard]] QString createRoomVolumeFromBoundary(
        const Boundary& boundary,
        QString height,
        QString elevation,
        std::vector<Boundary> holes = {},
        std::optional<Revision> expected_revision = std::nullopt);
    // Updates the selected architectural room volume's explicit height and
    // base elevation through one validated, undoable Document command.
    [[nodiscard]] bool editSelectedRoomVolume(
        QString height,
        QString elevation,
        std::optional<Revision> expected_revision = std::nullopt);
    // Detects all simple bounded faces in the selected wall's floor/layer
    // graph and creates one independent room boundary per face in one atomic
    // Document command. Open wall stubs are ignored; invalid segment graphs
    // fail before mutation.
    [[nodiscard]] QStringList detectRoomBoundariesFromExistingWalls(
        QString classification = QStringLiteral("room"),
        std::optional<Revision> expected_revision = std::nullopt);
    [[nodiscard]] QString createStraightWall(
        Vec2 start,
        Vec2 end,
        QString classification = QStringLiteral("interior"),
        std::optional<Revision> expected_revision = std::nullopt);
    // Creates a straight wall whose top rises (or falls) linearly from the
    // baseline start to its end. The signed rise uses the active unit system;
    // hosted openings are checked against the local sloped top.
    [[nodiscard]] QString createSlopedWall(
        Vec2 start,
        Vec2 end,
        QString rise,
        QString classification = QStringLiteral("interior"),
        std::optional<Revision> expected_revision = std::nullopt);
    // Creates one analytical arc wall from two world endpoints and an
    // explicit sweep expression (for example, "90 deg" or "pi/2"). The
    // geometry is validated by the same wall kernel used for openings,
    // projection, schedules, and output before the atomic Document command.
    [[nodiscard]] QString createCurvedWall(
        Vec2 start,
        Vec2 end,
        QString sweep,
        QString classification = QStringLiteral("interior"),
        std::optional<Revision> expected_revision = std::nullopt);
    // Creates one analytical arc wall from a chord and a defining measure.
    // `construction` accepts `angle`, `arc_length`, or `arc_height` (the
    // endpoint chord is always the authoritative chord). Length and height
    // expressions use the active unit system; a signed arc length selects
    // clockwise orientation. The original defining expression is retained
    // beside the derived sweep for inspection and migration.
    [[nodiscard]] QString createCurvedWallFromConstruction(
        Vec2 start,
        Vec2 end,
        QString construction,
        QString measure,
        QString classification = QStringLiteral("interior"),
        std::optional<Revision> expected_revision = std::nullopt);
    // Replaces the selected analytical arc wall through the same validated,
    // revision-fenced command used by the curved-wall editor. Hosted openings
    // remain attached and are revalidated against the proposed baseline.
    [[nodiscard]] bool editSelectedCurvedWall(
        Vec2 start,
        Vec2 end,
        QString sweep,
        std::optional<Revision> expected_revision = std::nullopt);
    // Replaces the selected arc wall using the same angle, arc-length, or
    // arc-height construction semantics as creation. Hosted openings are
    // revalidated before the one revision-fenced command is committed.
    [[nodiscard]] bool editSelectedCurvedWallFromConstruction(
        Vec2 start,
        Vec2 end,
        QString construction,
        QString measure,
        std::optional<Revision> expected_revision = std::nullopt);
    // Replaces the selected wall's optional composite layer stack. The JSON
    // array follows the project-format contract; validation, material links,
    // geometry previews, and undo all use the ordinary document command path.
    [[nodiscard]] bool editSelectedWallLayers(
        const QString& layers_json,
        std::optional<Revision> expected_revision = std::nullopt);
    // Replaces the selected slab/floor/ceiling/foundation layer stack. The
    // JSON array follows the same versioned material-reference contract as
    // wall assemblies and is committed through the revision-fenced command.
    [[nodiscard]] bool editSelectedSlabLayers(
        const QString& layers_json,
        std::optional<Revision> expected_revision = std::nullopt);
    // Replaces the selected straight wall's signed top rise through the same
    // revision-checked geometry and opening validation as creation.
    [[nodiscard]] bool editSelectedWallSlope(
        QString rise,
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
        std::optional<Revision> expected_revision = std::nullopt,
        std::optional<DoorOperation> door_operation = std::nullopt);
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
    // Creates a semantic horizontal assembly using the shared slab geometry
    // kernel.  `element_kind` accepts slab, floor, ceiling, or foundation;
    // the entity remains type `slab` for project and schedule compatibility.
    [[nodiscard]] QString createSurfaceFromSelectedBoundary(
        QString element_kind,
        QString thickness,
        QString elevation,
        std::optional<Revision> expected_revision = std::nullopt);
    [[nodiscard]] QString createSurfaceFromBoundary(
        const Boundary& boundary,
        QString element_kind,
        QString thickness,
        QString elevation,
        std::vector<Boundary> holes = {},
        std::optional<Revision> expected_revision = std::nullopt);
    // Creates a deterministic local TIN terrain surface from the selected
    // closed boundary. Elevations are comma-separated quantities in metres
    // (explicit units are accepted); the selected boundary remains unchanged.
    [[nodiscard]] QString createTerrainSurfaceFromSelectedBoundary(
        QString elevations,
        std::optional<Revision> expected_revision = std::nullopt);
    // Toggle adds/removes one root without replacing the ordered selection.
    [[nodiscard]] bool selectEntity(const QString& entity_id, bool toggle = false);
    [[nodiscard]] QString selectedEntityId() const;
    [[nodiscard]] QStringList selectedEntityIds() const;
    // Copies the selected geometry graph to the local system clipboard using
    // a bounded, versioned JSON payload. Clipboard operations never contact a
    // service and do not change document history until paste or cut commits.
    [[nodiscard]] bool copySelection();
    [[nodiscard]] bool cutSelection();
    [[nodiscard]] bool pasteSelection();
    // Deletes the selected semantic graph in one guarded, undoable command.
    // Hosted openings and boundary dimensions are removed with their owner;
    // referenced objects are rejected by the document validator.
    [[nodiscard]] bool deleteSelection();
    // Splits one identified boundary edge at a strict interior fraction,
    // preserving the original edge identity for the first piece and creating
    // fresh identities for the inserted vertex and second piece.
    [[nodiscard]] bool insertSelectedBoundaryVertex(const QString& segment_id,
                                                     const QString& fraction);
    // Direct stable-ID coordinate editing used by canvas vertex handles.
    [[nodiscard]] bool moveSelectedBoundaryVertex(
        const QString& vertex_id, Vec2 position,
        std::optional<Revision> expected_revision = std::nullopt);
    // Changes one analytical edge length while explicitly retaining its start
    // or end point. Connected mode translates the complementary boundary chain.
    [[nodiscard]] bool editSelectedBoundaryEdgeLength(
        const QString& segment_id, const QString& expression,
        BoundaryFixedEndpoint fixed_endpoint, bool move_connected,
        std::optional<Revision> expected_revision = std::nullopt);
    // Moves the precision pointer to a selected boundary vertex without
    // changing document history. When a boundary draft is active, the same
    // pointer is handed to its authoring session for the next anchor/edge.
    [[nodiscard]] bool jumpSelectedBoundaryVertex(const QString& vertex_id);
    // Explicitly invokes the exact automatic-closure operation on the active
    // boundary draft and publishes one named document command.
    [[nodiscard]] bool autoCloseBoundaryDraft();
    // Adds the three validated bay-window edges to an active draft, closes the
    // profile, and publishes one named document command.
    [[nodiscard]] bool completeBayWindowDraft(Vec2 shoulder1, Vec2 shoulder2,
                                               Vec2 end);
    // Replaces the selected identified boundary's analytical geometry while
    // retaining its entity, segment, and vertex identities. The replacement
    // must have the same edge count so existing typed references remain valid.
    // An optional nonempty classification updates the stored classification.
    [[nodiscard]] bool redefineSelectedBoundary(const Boundary& boundary,
                                                const QString& classification = {});
    [[nodiscard]] bool editSelectedClassification(const QString& classification);
    [[nodiscard]] bool editSelectedLength(const QString& expression);
    // Updates the selected hosted door/window's typed frame, panel/glazing,
    // and signed inset dimensions. All values use the active input units and
    // are committed as one revision-checked, undoable command after the wall
    // and every sibling opening pass the architecture preview.
    [[nodiscard]] bool editSelectedOpeningAssembly(
        const QString& frame_width,
        const QString& frame_depth,
        const QString& panel_thickness,
        const QString& glazing_thickness,
        const QString& inset,
        std::optional<Revision> expected_revision = std::nullopt);
    [[nodiscard]] bool editSelectedHeight(const QString& expression);
    // Edits an architectural object's base elevation through the same
    // validated, revision-checked geometry path as its other dimensions.
    [[nodiscard]] bool editSelectedElevation(const QString& expression);
    [[nodiscard]] bool editSelectedThickness(const QString& expression);
    [[nodiscard]] bool editSelectedFactor(const QString& expression);
    // Applies a validated local DISTO reading to the explicitly selected
    // compatible field. The reading's original unit and capture provenance
    // are retained in the selected entity's extension metadata.
    [[nodiscard]] bool importDistoMeasurement(const QString& payload);
    // Edits only the dimension's placement and presentation, preserving its
    // stable source target. X/Y use the current input units unless suffixed.
    [[nodiscard]] bool editBoundaryDimension(const QString& id, const QString& x,
        const QString& y, const QString& height_mm, const QString& color,
        bool bold, bool italic, bool visible, const QString& rotation_degrees);
    // Creates a segment-length dimension from a stable identified edge. The
    // displayed value continues to resolve from the current boundary geometry.
    [[nodiscard]] QString createLengthDimension(const QString& boundary_id,
                                                const QString& segment_id,
                                                Vec2 text_position,
                                                std::optional<Revision> expected_revision = std::nullopt);
    // Creates a semantic angle dimension from two identified boundary edges
    // and their shared vertex. The analytical angle is resolved from current
    // geometry; the text position is stored in model metres.
    [[nodiscard]] QString createAngleDimension(const QString& boundary_id,
                                               const QString& first_segment_id,
                                               const QString& second_segment_id,
                                               const QString& vertex_id,
                                               Vec2 text_position,
                                               std::optional<Revision> expected_revision = std::nullopt);
    // Creates a semantic area dimension for a complete identified closed
    // boundary. The value is derived from current geometry on every refresh.
    [[nodiscard]] QString createAreaDimension(const QString& boundary_id,
                                              Vec2 text_position,
                                              std::optional<Revision> expected_revision = std::nullopt);
    // Stores bounded string attributes on the selected closed boundary. The
    // JSON object remains inspectable in the native project format and the
    // update uses the normal undoable Document history.
    [[nodiscard]] bool editSelectedAreaAttributes(const QString& attributes_json);
    // Atomically declares property policy, selected floor grade and boundary facts.
    // JSON keys: appraisal_policy {policy_kind, version, property_kind, measurement_basis},
    // grade, appraisal_facts {finish, access, ceiling_eligibility, area_use, boundary_role}.
    // Missing facts remain undeclared; unknown tokens and versions are rejected.
    [[nodiscard]] bool editSelectedAppraisalFacts(
        const QString& declarations_json, std::optional<Revision> expected_revision = std::nullopt);
    void showAppraisalFacts();
    [[nodiscard]] bool setSelectedCalculationRule(bool include_in_building,
                                                   bool include_in_living);
    // Applies an analytic transform to a selected identified boundary or wall.
    // Pivot: analytical boundary bounds center or wall endpoint midpoint. Offsets
    // use the current input unit. Wall clones include hosted openings; originals
    // remain unchanged. The name is retained for existing callers.
    [[nodiscard]] bool transformSelectedBoundary(const QString& rotation_degrees,
                                                 bool flip_horizontal,
                                                 bool flip_vertical,
                                                 const QString& offset_x,
                                                 const QString& offset_y,
                                                 bool clone);
    // Applies a semantic transform to a selected column, beam, stair, or roof
    // entity. Translation uses the current input units; rotation is Z-axis
    // degrees and scale is a positive uniform factor. Clone mode leaves the
    // source unchanged and selects the transformed copy.
    [[nodiscard]] bool transformSelectedArchitecturalObject(
        const QString& rotation_degrees,
        const QString& offset_x,
        const QString& offset_y,
        const QString& offset_z,
        const QString& uniform_scale,
        bool clone);
    // Presentation annotations are persisted inside the typed annotation
    // entity. These commands keep labels/symbols undoable and portable rather
    // than creating renderer-only state.
    [[nodiscard]] QString createAnnotationLabel(const QString& template_id,
                                                const QString& content,
                                                Vec2 position);
    // `symbol_id` may be a full catalog variant ID or a case-insensitive
    // family alias; aliases resolve to the deterministic nominal `-w2-d2`
    // variant before the symbol instance is persisted.
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
                                      bool visible,
                                      QString font_family = {},
                                      QString text_height_mm = {},
                                      QString stroke_color = {},
                                      QString fill_color = {},
                                      bool bold = false,
                                      bool italic = false,
                                      bool style_enabled = false);
    [[nodiscard]] bool deleteAnnotation(const QString& annotation_id);
    // Imports a local PNG/JPEG/BMP/TIFF raster or first-page PDF into the
    // project Asset store and creates a reference_asset entity with an
    // explicit, editable calibration scale. PDF source bytes remain alongside
    // a deterministic local preview.
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
    // Offers local startup recovery only when an unsaved recovery copy is
    // available. Returns true when the selected copy was opened.
    [[nodiscard]] bool offerStartupRecovery();
    [[nodiscard]] bool exportDraftPdf(const QString& path);
    // Exports every persisted drawing sheet as one ordered multipage PDF.
    [[nodiscard]] bool exportDrawingSetPdf(const QString& path);
    [[nodiscard]] bool exportDraftSvg(const QString& path);
    [[nodiscard]] bool exportDraftImage(const QString& path);
    [[nodiscard]] bool exportNativeViewImage(const QString& path);
    // Controls the optional native 3D pane in the architectural workspace.
    // Hiding it is useful when a host cannot capture native child surfaces;
    // it does not alter the document or the exported 3D view.
    void setNativeModelViewVisible(bool visible);
    // True after actual semantic 3D geometry has been prepared for the current
    // document revision. Polling collects work on the desktop thread even when
    // the native pane is hidden; it does not certify native frame presentation.
    [[nodiscard]] bool regenerationReadyForCurrentRevision() noexcept;
    // Returns process-local timing samples collected from the real desktop
    // input, edit, open, save and navigation paths. The report is diagnostic
    // evidence only and never asserts reference-hardware qualification.
    [[nodiscard]] QString performanceReportJson() const;
    // Start a fresh diagnostic run without changing the current document.
    // Pending canvas paints from the previous run are excluded.
    void beginPerformanceRun() noexcept;
    // Local DXF R2013 interchange. Export writes an adjacent fidelity report;
    // import commits mapped geometry in one undoable command and retains the
    // original source bytes as a project asset for any reported gaps.
    [[nodiscard]] bool exportDxf(const QString& path);
    [[nodiscard]] bool importDxf(const QString& path);
    // Local IFC4 STEP interchange. Export/import use the same fidelity-report
    // and source-retention rules as DXF.
    [[nodiscard]] bool exportIfc(const QString& path);
    [[nodiscard]] bool importIfc(const QString& path);
    [[nodiscard]] bool showPrintPreview();
    [[nodiscard]] bool showDrawingSetPrintPreview();

    void showCommandPalette();
    void showDistoImport();
    void showAnnotationEditor();
    void showReferenceImport();
    // Registers a verified local project package without changing the active
    // document. The names are deterministic and remain available to the
    // secondary resource view until the next project transition.
    [[nodiscard]] bool registerProjectPackageResources(const QString& path);
    [[nodiscard]] QStringList registeredProjectResourceNames() const;
    void showProjectResources();
    void showReferenceCalibration();
    void showAssistance();
    void showRemodelingAlternatives();
    void showRoomRelationships();
    void showVerticalLevels();
    void showAssemblies();
    void showConstraintEditor();
    void showWorkspaceProfiles();
    void showRevisionHistory();
    void showBoundaryTransformEditor();
    void showBoundaryRedefinition();
    void showAutomaticAreaDetection();
    void showTerrainSurfaceDialog();
    // Writes an immutable copy of a named revision without changing the
    // current document or its later history.
    [[nodiscard]] bool restoreNamedRevision(const QString& name, const QString& path);
    void fitView();

    [[nodiscard]] QString lastError() const;

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace sketch::desktop
