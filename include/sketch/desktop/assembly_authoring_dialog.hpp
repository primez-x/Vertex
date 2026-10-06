#pragma once

#include "sketch/assembly_document_adapter.hpp"

#include <QDialog>
#include <QString>
#include <memory>
#include <optional>

namespace sketch::desktop {

// Captured source is retained for the controller's full-snapshot guard. These
// dialogs produce detached drafts only; they never own or mutate a Document.
struct AssemblyTypeDraft {
    Revision expected_revision{};
    Entity expected_catalog;
    AssemblyType replacement;
    std::vector<AssemblyTypeUpdateImpact> catalog_impacts;
    std::vector<AssemblyDocumentTypeUpdateImpact> document_impacts;
};

class AssemblyTypeDialog final : public QDialog {
public:
    AssemblyTypeDialog(DocumentSnapshot source, std::string catalog_id,
                       std::optional<AssemblyType> expected_type = std::nullopt,
                       bool metric_units = false, QWidget* parent = nullptr);
    ~AssemblyTypeDialog() override;
    [[nodiscard]] bool previewUpdate();
    // Editing requires previewUpdate() for the exact draft before accepting.
    [[nodiscard]] bool submit();
    [[nodiscard]] std::optional<AssemblyTypeDraft> candidate() const;
    [[nodiscard]] QString lastError() const;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

struct AssemblyInstanceDraft {
    Revision expected_revision{};
    std::optional<Entity> expected_entity;
    std::string name;
    AssemblyDocumentInstance value;
};

class AssemblyInstanceDialog final : public QDialog {
public:
    AssemblyInstanceDialog(DocumentSnapshot source,
                           std::optional<Entity> expected_instance = std::nullopt,
                           std::string preferred_catalog_id = {},
                           bool metric_units = false, QWidget* parent = nullptr);
    ~AssemblyInstanceDialog() override;
    [[nodiscard]] bool submit();
    [[nodiscard]] std::optional<AssemblyInstanceDraft> candidate() const;
    [[nodiscard]] QString lastError() const;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sketch::desktop
