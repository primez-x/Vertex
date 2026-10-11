#pragma once

#include "sketch/roof_opening_group_edit.hpp"
#include "sketch/assembly_instance_property_edit.hpp"
#include "sketch/selection_presentation_property_edit.hpp"
#include "sketch/selection_property_edit.hpp"

#include <QDialog>
#include <QString>
#include <memory>
#include <optional>
#include <vector>

namespace sketch::desktop {

// Captured-source input only. The enclosing mixed Properties command owns
// source fencing, joint physical admission, room review and publication.
class SelectionPropertiesDialog final : public QDialog {
public:
    SelectionPropertiesDialog(const DocumentSnapshot& captured,
        std::vector<SelectionPresentationPropertyTarget> presentation_targets,
        std::vector<SelectionPropertyTarget> ordinary_targets,
        std::vector<RoofOpeningGroupMember> skylight_members,
        bool metric, QWidget* parent = nullptr);
    SelectionPropertiesDialog(const DocumentSnapshot& captured,
        std::vector<SelectionPresentationPropertyTarget> presentation_targets,
        std::vector<SelectionPropertyTarget> ordinary_targets,
        std::vector<RoofOpeningGroupMember> skylight_members,
        std::vector<AssemblyInstancePropertyEditTarget> assembly_targets,
        bool metric, QWidget* parent = nullptr);
    ~SelectionPropertiesDialog() override;

    SelectionPropertiesDialog(const SelectionPropertiesDialog&) = delete;
    SelectionPropertiesDialog& operator=(const SelectionPropertiesDialog&) = delete;

    [[nodiscard]] std::optional<SelectionPropertyEditRequest> ordinaryRequest() const;
    [[nodiscard]] std::optional<SelectionPresentationPropertyEditRequest> presentationRequest() const;
    [[nodiscard]] std::optional<RoofOpeningGroupProperties> roofOpeningProperties() const;
    [[nodiscard]] std::vector<AssemblyInstancePropertyEditIntent> assemblyIntents() const;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] bool submit();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sketch::desktop
