#pragma once

#include "sketch/site_frame.hpp"
#include <QDialog>
#include <QString>
#include <memory>
#include <optional>

namespace sketch::desktop {

struct SitePlacementImpact {
    std::string owner_entity_id;
    // Annotation children have an owner-local namespace, never a fabricated
    // document entity ID. Other owners leave this empty.
    std::optional<std::string> annotation_child_id;
    std::string display_name;
    SitePresentationPlacement before;
    SitePresentationPlacement after;
};

struct SitePlacementDraft {
    Revision expected_revision{};
    std::string source_snapshot_digest;
    std::string target_entity_id;
    ApplyEntityChanges command;
    std::vector<SitePlacementImpact> impacts;
};

// Immutable captured source, typed fields and a detached validated command.
// The controller must recheck source_snapshot_digest, selection and workspace
// authority before applying. The dialog never owns or mutates a Document.
class SitePlacementDialog final : public QDialog {
public:
    [[nodiscard]] static bool supportsTarget(const Entity& entity);
    SitePlacementDialog(DocumentSnapshot source, std::string target_entity_id,
                        bool metric_units = false, QWidget* parent = nullptr);
    ~SitePlacementDialog() override;
    [[nodiscard]] bool previewUpdate();
    // Save accepts only the exact currently displayed impact preview.
    [[nodiscard]] bool submit();
    [[nodiscard]] std::optional<SitePlacementDraft> candidate() const;
    [[nodiscard]] QString lastError() const;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sketch::desktop
