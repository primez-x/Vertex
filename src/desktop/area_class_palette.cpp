#include "sketch/desktop/area_class_palette.hpp"
#include <QComboBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <set>
#include <utility>

namespace sketch::desktop {
namespace {
class ClassList final : public QListWidget {
public:using QListWidget::QListWidget;
protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override {
        if(items.size()!=1)return nullptr;
        auto* payload=new QMimeData;payload->setData(area_class_mime_type,encode_area_class_drag(items.front()->data(Qt::UserRole).toString()));return payload;
    }
    QStringList mimeTypes()const override {return {QString::fromLatin1(area_class_mime_type)};}
    Qt::DropActions supportedDropActions()const override {return Qt::CopyAction;}
    void startDrag(Qt::DropActions)override {
        if(!currentItem())return;auto* payload=mimeData({currentItem()});if(!payload)return;
        QDrag drag(this);drag.setMimeData(payload);drag.exec(Qt::CopyAction,Qt::CopyAction);
    }
};
class TargetList final : public QListWidget {
public:
    using QListWidget::QListWidget;
    std::function<bool(QString,QString)> dropped;
protected:
    void dragEnterEvent(QDragEnterEvent* event)override {
        if(decode_area_class_drag(event->mimeData()))event->acceptProposedAction();else event->ignore();
    }
    void dragMoveEvent(QDragMoveEvent* event)override {
        if(decode_area_class_drag(event->mimeData()) && itemAt(event->position().toPoint()))event->acceptProposedAction();else event->ignore();
    }
    void dropEvent(QDropEvent* event)override {
        const auto classification=decode_area_class_drag(event->mimeData());const auto* item=itemAt(event->position().toPoint());
        if(!classification || !item || !dropped) {event->ignore();return;}
        const auto id=item->data(Qt::UserRole).toString();
        // Copy IDs before the host refresh can rebuild this list.
        if(dropped(*classification,id))event->acceptProposedAction();else event->ignore();
    }
};
}
AreaClassPalette::AreaClassPalette(QWidget* parent):QWidget(parent) {
    setObjectName(QStringLiteral("areaClassPalette"));auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,8,0,0);layout->setSpacing(6);
    auto* filters=new QHBoxLayout;category_=new QComboBox(this);category_->setObjectName(QStringLiteral("areaClassCategory"));
    category_->setMinimumWidth(150);
    search_=new QLineEdit(this);search_->setObjectName(QStringLiteral("areaClassSearch"));search_->setPlaceholderText(QStringLiteral("Search area classes"));
    filters->addWidget(category_,1);filters->addWidget(search_,2);layout->addLayout(filters);
    classes_=new ClassList(this);classes_->setObjectName(QStringLiteral("areaClassItems"));classes_->setAccessibleName(QStringLiteral("Drag an area class or click to apply repeatedly"));
    classes_->setDragEnabled(true);classes_->setDragDropMode(QAbstractItemView::DragOnly);classes_->setAcceptDrops(false);layout->addWidget(classes_,2);
    auto* actions=new QHBoxLayout;auto* clear=new QPushButton(QStringLiteral("Clear class"),this);clear->setObjectName(QStringLiteral("areaClassClear"));
    auto* cancel=new QPushButton(QStringLiteral("Cancel"),this);cancel->setObjectName(QStringLiteral("areaClassCancel"));actions->addWidget(clear);actions->addWidget(cancel);
    add_types_=new QPushButton(QStringLiteral("Add types"),this);add_types_->setObjectName(QStringLiteral("areaClassAddTypes"));
    add_types_->setToolTip(QStringLiteral("Add missing drawing types to this measurement profile. Existing rules are preserved."));
    add_types_->hide(); actions->addWidget(add_types_); layout->addLayout(actions);
    status_=new QLabel(this);status_->setObjectName(QStringLiteral("areaClassStatus"));status_->setWordWrap(true);status_->setMinimumWidth(0);layout->addWidget(status_);
    targets_=new TargetList(this);targets_->setObjectName(QStringLiteral("areaClassTargets"));targets_->setAccessibleName(QStringLiteral("Drop an area class onto an area"));
    targets_->setWordWrap(true);targets_->setTextElideMode(Qt::ElideNone);
    targets_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);targets_->setResizeMode(QListView::Adjust);
    targets_->setAcceptDrops(true);targets_->setDragDropMode(QAbstractItemView::DropOnly);layout->addWidget(targets_,1);
    QObject::connect(search_,&QLineEdit::textChanged,this,[this]{filter();});QObject::connect(category_,&QComboBox::currentIndexChanged,this,[this]{filter();});
    QObject::connect(classes_,&QListWidget::itemClicked,this,[this](QListWidgetItem* item){if(arm_requested_)arm_requested_(item->data(Qt::UserRole).toString());});
    QObject::connect(classes_,&QListWidget::itemActivated,this,[this](QListWidgetItem* item){if(arm_requested_)arm_requested_(item->data(Qt::UserRole).toString());});
    QObject::connect(clear,&QPushButton::clicked,this,[this]{if(arm_requested_)arm_requested_({});});
    QObject::connect(cancel,&QPushButton::clicked,this,[this]{if(cancel_requested_)cancel_requested_();});setArmedClass({});
    QObject::connect(add_types_,&QPushButton::clicked,this,[this]{if(add_types_requested_)add_types_requested_();});
}
void AreaClassPalette::setClasses(std::vector<AreaClassEntry> entries) {
    entries_=std::move(entries);const auto previous=category_->currentData();const QSignalBlocker blocker(category_);
    category_->clear();category_->addItem(QStringLiteral("All categories"),QString{});std::set<QString> categories;
    for(const auto& entry:entries_)categories.insert(entry.category);
    for(const auto& category:categories)category_->addItem(category,category);
    const auto index=category_->findData(previous);category_->setCurrentIndex(index<0?0:index);filter();
}
void AreaClassPalette::filter() {
    const auto query=search_->text().trimmed();const auto category=category_->currentData().toString();classes_->clear();
    for(const auto& entry:entries_)if((category.isEmpty() || entry.category==category) &&
        (query.isEmpty() || entry.label.contains(query,Qt::CaseInsensitive) || entry.key.contains(query,Qt::CaseInsensitive))) {
        auto* item=new QListWidgetItem(entry.label,classes_);item->setData(Qt::UserRole,entry.key);
        item->setToolTip(entry.key.startsWith(QStringLiteral("area-type:"))
            ? QStringLiteral("Drawing type only. Does not change appraisal facts, totals, or the assigned floor.") : entry.category);
    }
}
void AreaClassPalette::setTargets(const std::vector<AreaClassTarget>& targets) {
    targets_->clear();for(const auto& target:targets) {auto* item=new QListWidgetItem(target.label,targets_);item->setData(Qt::UserRole,target.id);item->setToolTip(target.label);}
}
void AreaClassPalette::setArmedClass(std::optional<QString> classification) {
    auto label=classification.value_or(QString{});
    if(classification)for(const auto& entry:entries_)if(entry.key==*classification){label=entry.label;break;}
    setStatus(classification ? (classification->isEmpty()?QStringLiteral("Clear armed · click areas · Esc cancels"):
        QStringLiteral("%1 armed · click areas · Esc cancels").arg(label)):QStringLiteral("Drag a class onto an area, or click a class."));
}
void AreaClassPalette::setStatus(const QString& message) {status_->setText(message);}
void AreaClassPalette::setArmRequested(std::function<void(QString)> callback) {arm_requested_=std::move(callback);}
void AreaClassPalette::setCancelRequested(std::function<void()> callback) {cancel_requested_=std::move(callback);}
void AreaClassPalette::setAddTypesRequested(std::function<void()> callback) {add_types_requested_=std::move(callback);}
void AreaClassPalette::setMissingDrawingTypes(bool missing) {add_types_->setVisible(missing);}
void AreaClassPalette::setDropRequested(std::function<bool(QString,QString)> callback) {static_cast<TargetList*>(targets_)->dropped=std::move(callback);}
} // namespace sketch::desktop
