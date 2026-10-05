#pragma once
#include <QByteArray>
#include <QString>
#include <QWidget>
#include <functional>
#include <optional>
#include <vector>
class QMimeData;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
namespace sketch::desktop {
inline constexpr const char* area_class_mime_type="application/x-vertex-area-class";
[[nodiscard]] QByteArray encode_area_class_drag(const QString& classification);
// Empty string is an explicit clear; malformed/oversized input has no value.
[[nodiscard]] std::optional<QString> decode_area_class_drag(const QMimeData* mime);
struct AreaClassEntry {QString key;QString label;QString category;};
struct AreaClassTarget {QString id;QString label;};
class AreaClassPalette final : public QWidget {
public:
    explicit AreaClassPalette(QWidget* parent=nullptr);
    void setClasses(std::vector<AreaClassEntry> entries);
    void setTargets(const std::vector<AreaClassTarget>& targets);
    void setArmedClass(std::optional<QString> classification);
    void setStatus(const QString& message);
    void setArmRequested(std::function<void(QString)> callback);
    void setCancelRequested(std::function<void()> callback);
    void setDropRequested(std::function<bool(QString,QString)> callback);
    void setAddTypesRequested(std::function<void()> callback);
    void setMissingDrawingTypes(bool missing);
private:
    void filter();
    std::vector<AreaClassEntry> entries_;
    QLineEdit* search_{};QComboBox* category_{};QListWidget *classes_{},*targets_{};QLabel* status_{};
    std::function<void(QString)> arm_requested_;
    std::function<void()> cancel_requested_;
    std::function<void()> add_types_requested_;
    QPushButton* add_types_{};
};
} // namespace sketch::desktop
