#pragma once

#include "sketch/document.hpp"
#include "sketch/geometry.hpp"
#include "sketch/boundary_authoring_session.hpp"

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
    [[nodiscard]] bool undoCommand();
    [[nodiscard]] bool redoCommand();

    [[nodiscard]] bool createNewProject();
    [[nodiscard]] bool openProject(const QString& path);
    [[nodiscard]] bool saveProject();
    [[nodiscard]] bool saveProjectAs(const QString& path);
    [[nodiscard]] bool exportDraftPdf(const QString& path);
    [[nodiscard]] bool exportNativeViewImage(const QString& path);
    [[nodiscard]] bool showPrintPreview();

    void showCommandPalette();
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
