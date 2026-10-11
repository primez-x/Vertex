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
    [[nodiscard]] std::vector<AssemblyInstancePropertyEditIntent> intents() const;
    [[nodiscard]] QString lastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sketch::desktop
