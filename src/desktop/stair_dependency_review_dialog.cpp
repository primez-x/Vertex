#include "sketch/desktop/stair_dependency_review_dialog.hpp"
#include "sketch/document_digest.hpp"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sketch::desktop {
namespace {
QString text(const std::string& value) { return QString::fromStdString(value); }
QString object_name(const DocumentSnapshot& source, const std::string& id,
    const QString& fallback) {
    const auto owner = source.entities().find(id);
    if (owner == source.entities().end()) return fallback;
    const auto name = owner->second.properties.find("name");
    return name != owner->second.properties.end() && name->is_string() &&
        !name->get_ref<const std::string&>().empty() ? text(name->get<std::string>()) : fallback;
}
QString current_attachment(const DocumentSnapshot& source,
    const PhaseStairReplacementDependency& dependency) {
    auto attachment = dependency.host_role == "landing" ? QStringLiteral("Landing") :
        dependency.host_role == "flight" ? QStringLiteral("Flight") : QStringLiteral("Stair top");
    const auto stair = source.entities().find(dependency.stair_id);
    if (stair != source.entities().end() && !dependency.current_child_id.empty()) {
        const char* collection = dependency.host_role == "landing" ? "landings" : "flights";
        const auto children = stair->second.properties.find(collection);
        if (children != stair->second.properties.end() && children->is_array()) {
            for (std::size_t index = 0; index < children->size(); ++index) {
                const auto& child = children->at(index);
                if (child.is_object() && child.value("id", std::string{}) == dependency.current_child_id) {
                    attachment += QStringLiteral(" %1").arg(static_cast<qulonglong>(index + 1));
                    break;
                }
            }
        }
    }
    return object_name(source, dependency.stair_id, QStringLiteral("Stair")) +
        QStringLiteral(" / ") + attachment;
}
QString target_name(const PhaseStairReplacementDependencyTarget& target) {
    if (!target.display_name.empty()) return text(target.display_name);
    if (target.role == "top") return QStringLiteral("Stair top");
    return (target.role == "landing" ? QStringLiteral("Landing") : QStringLiteral("Flight")) +
        QStringLiteral(" ") + text(target.child_id);
}
}

class StairDependencyReviewDialog::Impl {
public:
    StairDependencyReviewDialog* dialog;
    DocumentSnapshot source;
    PhaseStairReplacementDependencyPlan plan;
    std::function<DocumentSnapshot()> current_source;
    std::function<void(const std::vector<PhaseStairReplacementDependencyDisposition>&)> disposition_validator;
    std::string source_digest;
    std::vector<QComboBox*> choices;
    QLabel *error_label{}, *consequences{};
    QPushButton* apply{};
    QString error;
    bool invalidated{};
    std::optional<std::vector<PhaseStairReplacementDependencyDisposition>> accepted;

    Impl(StairDependencyReviewDialog* owner, DocumentSnapshot captured,
        PhaseStairReplacementDependencyPlan dependencies,
        std::function<DocumentSnapshot()> current)
        : dialog(owner), source(std::move(captured)), plan(std::move(dependencies)),
          current_source(std::move(current)), source_digest(document_snapshot_digest(source)) {
        if (!source.is_editable() || !current_source || !plan.ready() || plan.dependencies.empty() ||
            plan.dependencies.size() > 4096)
            throw std::invalid_argument("Stair attachment review requires an editable source and a bounded ready dependency plan.");
        dialog->setObjectName(QStringLiteral("stairDependencyReviewDialog"));
        dialog->setWindowTitle(QStringLiteral("Update stair railings"));
        dialog->resize(720, std::clamp(170 + static_cast<int>(plan.dependencies.size()) * 54, 300, 620));
        auto* layout = new QVBoxLayout(dialog);
        auto* explanation = new QLabel(QStringLiteral(
            "This stair edit changes railing attachments. Choose where to move each railing, or remove it with the edit."), dialog);
        explanation->setWordWrap(true);
        layout->addWidget(explanation);

        auto* table = new QTableWidget(static_cast<int>(plan.dependencies.size()), 3, dialog);
        table->setObjectName(QStringLiteral("stairDependencyTable"));
        table->setHorizontalHeaderLabels({QStringLiteral("Railing"), QStringLiteral("Current attachment"), QStringLiteral("Action")});
        table->verticalHeader()->hide();
        table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->setWordWrap(true);
        for (std::size_t index = 0; index < plan.dependencies.size(); ++index) {
            const auto& dependency = plan.dependencies[index];
            const auto row = static_cast<int>(index);
            const auto name = dependency.rail_name.empty() ?
                object_name(source, dependency.rail_id, QStringLiteral("Railing %1").arg(row + 1)) : text(dependency.rail_name);
            table->setItem(row, 0, new QTableWidgetItem(name));
            table->setItem(row, 1, new QTableWidgetItem(current_attachment(source, dependency)));
            auto* choice = new QComboBox(table);
            choice->setObjectName(QStringLiteral("stairDependencyChoice_%1").arg(row));
            choice->setAccessibleName(QStringLiteral("Action for %1").arg(name));
            choice->addItem(QStringLiteral("Choose an action"));
            for (const auto& target : dependency.valid_targets) {
                choice->addItem(QStringLiteral("Move to %1").arg(target_name(target)),
                    static_cast<int>(PhaseStairReplacementDependencyAction::rehost));
                choice->setItemData(choice->count() - 1, text(target.target_key), Qt::UserRole + 1);
            }
            if (dependency.retirement_eligible) {
                choice->addItem(QStringLiteral("Remove railing and attached components"),
                    static_cast<int>(PhaseStairReplacementDependencyAction::retire));
                if (!dependency.retirement_reason.empty())
                    choice->setItemData(choice->count()-1,text(dependency.retirement_reason),Qt::ToolTipRole);
            }
            if (!dependency.retirement_reason.empty()) choice->setToolTip(text(dependency.retirement_reason));
            choices.push_back(choice);
            table->setCellWidget(row, 2, choice);
            QObject::connect(choice, qOverload<int>(&QComboBox::currentIndexChanged), dialog,
                [this](int) { update(); });
        }
        table->resizeRowsToContents();
        layout->addWidget(table, 1);
        consequences = new QLabel(dialog);
        consequences->setObjectName(QStringLiteral("stairDependencyConsequences"));
        consequences->setWordWrap(true);
        layout->addWidget(consequences);
        error_label = new QLabel(dialog);
        error_label->setObjectName(QStringLiteral("stairDependencyError"));
        error_label->setWordWrap(true);
        error_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(error_label);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, dialog);
        apply = buttons->button(QDialogButtonBox::Apply);
        apply->setText(QStringLiteral("Continue stair edit"));
        QObject::connect(apply, &QPushButton::clicked, dialog, &StairDependencyReviewDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &StairDependencyReviewDialog::reject);
        layout->addWidget(buttons);
        update();
    }

    void update() {
        if (!apply) return;
        std::size_t removed{};
        QStringList requirements;
        bool complete = !invalidated;
        for (std::size_t index=0;index<choices.size();++index) {
            const auto* choice=choices[index];
            complete = complete && choice->currentIndex() > 0;
            if (choice->currentIndex() > 0 && choice->currentData().toInt() ==
                static_cast<int>(PhaseStairReplacementDependencyAction::retire)) {
                ++removed;
                const auto reason=text(plan.dependencies[index].retirement_reason);
                if (!reason.isEmpty() && !requirements.contains(reason)) requirements.push_back(reason);
            }
        }
        apply->setEnabled(complete);
        consequences->setVisible(removed > 0);
        consequences->setText(QStringLiteral(
            "%1 railing(s) and their attached components will be removed with this stair edit. Undo restores the whole edit.")
            .arg(static_cast<qulonglong>(removed))+
            (requirements.isEmpty() ? QString{} : QStringLiteral("\n")+requirements.join(QStringLiteral("\n"))));
    }

    bool collect() {
        accepted.reset(); error.clear(); error_label->clear();
        try {
            if (invalidated || document_snapshot_digest(current_source()) != source_digest) {
                invalidated = true;
                throw std::invalid_argument("The project changed. Cancel and reopen the stair edit.");
            }
            std::vector<PhaseStairReplacementDependencyDisposition> result;
            for (std::size_t index = 0; index < choices.size(); ++index) {
                const auto* choice = choices[index];
                if (choice->currentIndex() <= 0) throw std::invalid_argument("Choose an action for every railing.");
                const auto& dependency = plan.dependencies[index];
                const auto action = static_cast<PhaseStairReplacementDependencyAction>(choice->currentData().toInt());
                const auto target = choice->currentData(Qt::UserRole + 1).toString().toStdString();
                if (action == PhaseStairReplacementDependencyAction::retire) {
                    if (!dependency.retirement_eligible || !target.empty())
                        throw std::invalid_argument("This railing cannot be removed in the current stair edit.");
                } else if (action != PhaseStairReplacementDependencyAction::rehost ||
                    std::none_of(dependency.valid_targets.begin(), dependency.valid_targets.end(),
                        [&](const auto& value) { return value.target_key == target; }))
                    throw std::invalid_argument("Choose an available attachment for this railing.");
                result.push_back({dependency.rail_id, action, target});
            }
            std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
                return left.rail_id < right.rail_id;
            });
            if (disposition_validator) disposition_validator(result);
            if (document_snapshot_digest(current_source())!=source_digest) {
                invalidated=true;
                throw std::invalid_argument("The project changed. Cancel and reopen the stair edit.");
            }
            accepted = std::move(result);
            return true;
        } catch (const std::exception& failure) {
            error = QString::fromUtf8(failure.what());
            error_label->setText(error);
            update();
            return false;
        }
    }
};

StairDependencyReviewDialog::StairDependencyReviewDialog(DocumentSnapshot source,
    PhaseStairReplacementDependencyPlan plan, std::function<DocumentSnapshot()> current_source, QWidget* parent)
    : QDialog(parent), m_impl(std::make_unique<Impl>(this, std::move(source), std::move(plan), std::move(current_source))) {}
StairDependencyReviewDialog::~StairDependencyReviewDialog() = default;
const std::optional<std::vector<PhaseStairReplacementDependencyDisposition>>&
StairDependencyReviewDialog::acceptedDispositions() const { return m_impl->accepted; }
QString StairDependencyReviewDialog::lastError() const { return m_impl->error; }
void StairDependencyReviewDialog::setDispositionValidator(std::function<void(
    const std::vector<PhaseStairReplacementDependencyDisposition>&)> validator) {
    m_impl->disposition_validator=std::move(validator);
}
void StairDependencyReviewDialog::accept() { if (m_impl->collect()) QDialog::accept(); }
void StairDependencyReviewDialog::reject() { m_impl->accepted.reset(); QDialog::reject(); }

} // namespace sketch::desktop
