#pragma once

#include "sketch/assembly_instance_property_edit.hpp"

#include <QString>
#include <QWidget>
#include <memory>
#include <vector>

namespace sketch::desktop {

// Immutable captured-source projection. The enclosing Properties workflow owns
// admission, source fencing, preview, dependency completion and publication.
class AssemblyPropertiesPanel final : public QWidget {
public:
    AssemblyPropertiesPanel(const DocumentSnapshot& captured,
        std::vector<AssemblyInstancePropertyEditTarget> targets,
        bool metric, QWidget* parent = nullptr);
    ~AssemblyPropertiesPanel() override;

    AssemblyPropertiesPanel(const AssemblyPropertiesPanel&) = delete;
    AssemblyPropertiesPanel& operator=(const AssemblyPropertiesPanel&) = delete;

    // Throws on invalid input. Always retains every originally selected target.
    // Geometry edits replace only a node's own effective source profiles. Bulk
    // dimensions resize each source separately using pending ancestor pose
    // scales. Detailed outline/cutout, local extrusion, material slot and
    // collection edits keep a separate draft for each selected node. New local
    // profile IDs are fresh across current, type and removed dialog profiles.
    // Keep and Use type geometry ignore those drafts. Dependency completion
    // and refusal of last-solid removal remain the enclosing workflow's job.
    // Enable edge dimensions initializes only unchanged anonymous own-source
    // profiles. That pending Set freezes profile edits and dimensions until
    // applied; its explicit initialization flag survives intent collection.
    // Existing edge identities permit local extrusion/material-slot edits;
    // redraw, roster, XY resize and clear-to-type await reviewed mapping.
    [[nodiscard]] std::vector<AssemblyInstancePropertyEditIntent> intents() const;
    [[nodiscard]] QString lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sketch::desktop
