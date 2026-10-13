#pragma once

// Presentation helpers for the workspace's left panel (Layers, Library and
// Details). The panel itself is assembled by MainWindow; this file owns the
// naming rules and small reusable widgets so the panel stays consistent with
// the Vertex design system (sentence case, readable names, no storage IDs).

#include <QString>
#include <QWidget>

#include <string_view>

class QToolButton;

namespace sketch::desktop {

// Sentence-cases a catalog, family or record name for display while keeping
// common acronyms upright: "Ac Condenser" -> "AC condenser",
// "sofa-three-seat" -> "Sofa three seat".
[[nodiscard]] QString library_display_name(const QString& raw);

// Project hierarchy rows (property, building, floor, layer).
[[nodiscard]] bool navigator_is_hierarchy_type(std::string_view type) noexcept;
// Drawing objects a person places and recognizes. Everything else is an
// internal record (joins, constraints, phase registries, source records...)
// and is not listed as a drawing object.
[[nodiscard]] bool navigator_is_drawing_type(std::string_view type) noexcept;
// Singular readable type name, e.g. "measurement_boundary" -> "Area".
[[nodiscard]] QString navigator_object_label(std::string_view type);
// Stable key and plural label of the kind group that lists a drawing object
// beneath its layer, e.g. "areas" / "Areas".
[[nodiscard]] QString navigator_kind_key(std::string_view type);
[[nodiscard]] QString navigator_kind_label(std::string_view type);

// The left panel's tab selection is remembered for the installed application
// only. Tests and other hosts always start on the first tab.
[[nodiscard]] int remembered_left_panel_tab(int fallback);
void remember_left_panel_tab(int index);

// A titled section whose body is shown on demand. Used for advanced controls
// that the default panel keeps out of the way.
class CollapsibleSection final : public QWidget {
public:
    explicit CollapsibleSection(const QString& title, QWidget* parent = nullptr);

    [[nodiscard]] QWidget* body() const noexcept { return m_body; }
    [[nodiscard]] QToolButton* header() const noexcept { return m_header; }
    [[nodiscard]] bool isExpanded() const noexcept;
    void setExpanded(bool expanded);

private:
    QToolButton* m_header{};
    QWidget* m_body{};
};

} // namespace sketch::desktop
