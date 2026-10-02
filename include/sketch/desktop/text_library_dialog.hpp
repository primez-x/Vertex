#pragma once

#include "sketch/text_library.hpp"

#include <QByteArray>
#include <QDialog>
#include <QString>

#include <memory>
#include <optional>
#include <string_view>

namespace sketch::desktop {

// Local reusable text, independent of any project or placed label. Failed
// writes leave both the retained entries and the previously loaded head intact.
class TextLibraryStore final {
public:
    explicit TextLibraryStore(QString path);
    [[nodiscard]] const std::vector<TextLibraryEntry>& entries() const noexcept;
    [[nodiscard]] const QString& path() const noexcept;
    void reload();
    void upsert(TextLibraryEntry entry);
    void remove(std::string_view id);

private:
    void commit(TextLibraryDocument candidate);
    QString m_path;
    TextLibraryDocument m_document;
    std::optional<QByteArray> m_loaded_head;
};

class TextLibraryDialog final : public QDialog {
public:
    explicit TextLibraryDialog(TextLibraryStore& store, bool metric, QWidget* parent = nullptr);
    ~TextLibraryDialog() override;
    [[nodiscard]] std::optional<TextLibraryEntry> selectedEntry() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace sketch::desktop
