#pragma once

#include "sketch/text_library.hpp"
#include <QWidget>
#include <functional>
#include <memory>

namespace sketch::desktop {
class TextLibraryPanel final : public QWidget {
public:
    explicit TextLibraryPanel(QWidget* parent = nullptr);
    ~TextLibraryPanel() override;
    void setEntries(const std::vector<TextLibraryEntry>& entries);
    void setPlaceRequested(std::function<void(const TextLibraryEntry&)> callback);
    void setManageRequested(std::function<void()> callback);
    void setStatus(const QString& status);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
}
