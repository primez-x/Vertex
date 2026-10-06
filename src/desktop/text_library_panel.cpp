#include "sketch/desktop/text_library_panel.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <set>
#include <utility>

namespace sketch::desktop {
namespace {
QString qtext(const std::string& value) {
    return QString::fromUtf8(value.data(),static_cast<qsizetype>(value.size()));
}
}

class TextLibraryPanel::Impl {
public:
    explicit Impl(TextLibraryPanel* owner):owner(owner) {
        owner->setObjectName(QStringLiteral("textLibraryPanel"));
        auto* layout=new QVBoxLayout(owner);
        layout->setContentsMargins(0,0,0,0);
        layout->setSpacing(4);
        auto* filters=new QHBoxLayout;
        filters->setSpacing(4);
        categories=new QComboBox(owner);categories->setObjectName(QStringLiteral("textLibraryPanelCategory"));
        categories->setAccessibleName(QStringLiteral("Label category"));
        categories->setMaximumWidth(150);
        categories->addItem(QStringLiteral("All categories"),QString{});
        search=new QLineEdit(owner);search->setObjectName(QStringLiteral("textLibraryPanelSearch"));
        search->setPlaceholderText(QStringLiteral("Search labels"));
        search->setAccessibleName(QStringLiteral("Search label names, categories and content"));
        search->setClearButtonEnabled(true);
        filters->addWidget(categories);filters->addWidget(search,1);layout->addLayout(filters);
        list=new QListWidget(owner);list->setObjectName(QStringLiteral("textLibraryPanelList"));
        list->setAccessibleName(QStringLiteral("Labels: click or press Enter to place"));
        list->setWordWrap(true);list->setTextElideMode(Qt::ElideNone);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        list->installEventFilter(owner);layout->addWidget(list,1);
        empty=new QLabel(owner);empty->setObjectName(QStringLiteral("textLibraryPanelEmpty"));
        empty->setTextFormat(Qt::PlainText);empty->setWordWrap(true);layout->addWidget(empty);
        status=new QLabel(owner);status->setObjectName(QStringLiteral("textLibraryPanelStatus"));
        status->setTextFormat(Qt::PlainText);status->setWordWrap(true);status->hide();layout->addWidget(status);
        auto* actions=new QHBoxLayout;
        auto* hint=new QLabel(QStringLiteral("Click a label to place it."),owner);
        hint->setTextFormat(Qt::PlainText);hint->setWordWrap(true);actions->addWidget(hint,1);
        auto* manage=new QPushButton(QStringLiteral("Manage…"),owner);
        manage->setObjectName(QStringLiteral("textLibraryPanelManage"));actions->addWidget(manage);layout->addLayout(actions);
        QObject::connect(search,&QLineEdit::textChanged,owner,[this]{refreshList();});
        QObject::connect(categories,&QComboBox::currentIndexChanged,owner,[this]{refreshList();});
        QObject::connect(list,&QListWidget::itemClicked,owner,[this](QListWidgetItem* item){activate(item);});
        // itemActivated is deliberately unused: Return is handled once below,
        // independently of platform single/double-click activation preferences.
        QObject::connect(manage,&QPushButton::clicked,owner,[this]{if(manageRequested) manageRequested();});
        refreshList();
    }
    void activate(QListWidgetItem* item) {
        if(!item || !placeRequested) return;
        const auto id=item->data(Qt::UserRole).toString();
        const auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& entry){return qtext(entry.id)==id;});
        if(found==entries.end()) return;
        // A callback may refresh the panel synchronously. Keep its argument
        // alive independently of the retained vector and row.
        const auto entry=*found;
        const auto callback=placeRequested;
        callback(entry);
    }
    void setEntries(const std::vector<TextLibraryEntry>& values) {
        entries=values;
        const auto previous=categories->currentData().toString();
        const QSignalBlocker blocker(categories);
        categories->clear();categories->addItem(QStringLiteral("All categories"),QString{});
        std::set<QString> valuesByCategory;
        for(const auto& entry:entries) if(!entry.category.empty()) valuesByCategory.insert(qtext(entry.category));
        for(const auto& category:valuesByCategory) categories->addItem(category,category);
        const auto index=categories->findData(previous);categories->setCurrentIndex(index<0 ? 0 : index);
        refreshList();
    }
    void refreshList() {
        const auto previous=list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString{};
        const QSignalBlocker blocker(list);list->clear();
        const auto query=search->text().trimmed();
        const auto category=categories->currentData().toString();
        int selected=-1;
        for(const auto& entry:entries) {
            if(!category.isEmpty() && qtext(entry.category)!=category) continue;
            if(!query.isEmpty() && !(qtext(entry.name)+QLatin1Char(' ')+qtext(entry.category)+QLatin1Char(' ')+qtext(entry.content)).contains(query,Qt::CaseInsensitive)) continue;
            const auto name=qtext(entry.name);
            const auto content=qtext(entry.content);
            auto* item=new QListWidgetItem(name==content ? name : name+QLatin1Char('\n')+content,list);
            item->setData(Qt::UserRole,qtext(entry.id));
            item->setToolTip(qtext(entry.category));
            if(qtext(entry.id)==previous) selected=list->count()-1;
        }
        list->setCurrentRow(selected<0 && list->count()>0 ? 0 : selected);
        empty->setText(entries.empty() ? QStringLiteral("No labels yet. Use Manage to create reusable text.") : QStringLiteral("No labels match this search and category."));
        empty->setVisible(list->count()==0);
    }
    TextLibraryPanel* owner;
    QLineEdit* search{};
    QComboBox* categories{};
    QListWidget* list{};
    QLabel *empty{},*status{};
    std::vector<TextLibraryEntry> entries;
    std::function<void(const TextLibraryEntry&)> placeRequested;
    std::function<void()> manageRequested;
};

TextLibraryPanel::TextLibraryPanel(QWidget* parent):QWidget(parent),m_impl(std::make_unique<Impl>(this)) {}
TextLibraryPanel::~TextLibraryPanel()=default;
void TextLibraryPanel::setEntries(const std::vector<TextLibraryEntry>& entries) {m_impl->setEntries(entries);}
void TextLibraryPanel::setPlaceRequested(std::function<void(const TextLibraryEntry&)> callback) {m_impl->placeRequested=std::move(callback);}
void TextLibraryPanel::setManageRequested(std::function<void()> callback) {m_impl->manageRequested=std::move(callback);}
void TextLibraryPanel::setStatus(const QString& status) {m_impl->status->setText(status);m_impl->status->setVisible(!status.isEmpty());}
bool TextLibraryPanel::eventFilter(QObject* watched,QEvent* event) {
    if(watched==m_impl->list && event->type()==QEvent::KeyPress) {
        const auto* key=static_cast<QKeyEvent*>(event);
        if(key->key()==Qt::Key_Return || key->key()==Qt::Key_Enter) {
            if(!key->isAutoRepeat()) m_impl->activate(m_impl->list->currentItem());
            return true;
        }
    }
    return QWidget::eventFilter(watched,event);
}
}
