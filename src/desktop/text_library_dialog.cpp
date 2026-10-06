#include "sketch/desktop/text_library_dialog.hpp"
#include "sketch/quantity.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QUuid>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch::desktop {
namespace {
using json=nlohmann::json;

std::runtime_error file_error(const QString& path,const QString& detail) {
    return std::runtime_error(QStringLiteral("Text library %1: %2").arg(QDir::toNativeSeparators(path),detail).toStdString());
}
std::optional<QByteArray> read_head(const QString& path) {
    const QFileInfo info(path);
    if(!info.exists()) {
        if(info.isSymLink()) throw file_error(path,QStringLiteral("the symbolic link target is unavailable."));
        return std::nullopt;
    }
    if(!info.isFile()) throw file_error(path,QStringLiteral("the destination is not a regular file."));
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) throw file_error(path,QStringLiteral("cannot read the file: %1").arg(file.errorString()));
    if(file.size()>static_cast<qint64>(kTextLibraryByteLimit)) throw file_error(path,QStringLiteral("the file exceeds 4 MiB."));
    auto bytes=file.read(static_cast<qint64>(kTextLibraryByteLimit)+1);
    if(file.error()!=QFileDevice::NoError) throw file_error(path,QStringLiteral("cannot read the complete file: %1").arg(file.errorString()));
    if(bytes.size()>static_cast<qsizetype>(kTextLibraryByteLimit) || !file.atEnd())
        throw file_error(path,QStringLiteral("the file exceeds 4 MiB."));
    return bytes;
}
std::string new_id() {
    return "user-text-"+QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}
QString qtext(const std::string& value) {return QString::fromUtf8(value.data(),static_cast<qsizetype>(value.size()));}
}

TextLibraryStore::TextLibraryStore(QString path):m_path(std::move(path)) {
    if(m_path.trimmed().isEmpty()) throw std::invalid_argument("Text library path is empty.");
    m_path=QFileInfo(m_path).absoluteFilePath();
    reload();
}
const std::vector<TextLibraryEntry>& TextLibraryStore::entries() const noexcept {return m_document.entries;}
const QString& TextLibraryStore::path() const noexcept {return m_path;}
void TextLibraryStore::reload() {
    const auto head=read_head(m_path);
    TextLibraryDocument candidate;
    if(head) {
        try {candidate=decode_text_library(json::parse(head->constData(),head->constData()+head->size()));}
        catch(const std::exception& error) {throw file_error(m_path,QStringLiteral("cannot load; existing file is preserved. %1").arg(QString::fromUtf8(error.what())));}
    }
    m_document=std::move(candidate);
    m_loaded_head=head;
}
void TextLibraryStore::commit(TextLibraryDocument candidate) {
    const auto encoded=encode_text_library(candidate).dump();
    const QByteArray bytes(encoded.data(),static_cast<qsizetype>(encoded.size()));
    const QFileInfo parent(QFileInfo(m_path).absolutePath());
    if(!parent.exists() || !parent.isDir()) throw file_error(m_path,QStringLiteral("its parent directory does not exist or is not a directory."));
    QLockFile lock(m_path+QStringLiteral(".lock"));
    if(!lock.tryLock(0)) throw file_error(m_path,QStringLiteral("cannot acquire the writer lock. Close another library editor or check folder permissions."));
    if(read_head(m_path)!=m_loaded_head) throw file_error(m_path,QStringLiteral("the file changed outside this editor. Reload the library before saving."));
    const QFileInfo current(m_path);
    if(current.exists() && (!current.isWritable() ||
        !(current.permissions()&(QFileDevice::WriteOwner|QFileDevice::WriteUser|QFileDevice::WriteGroup|QFileDevice::WriteOther))))
        throw file_error(m_path,QStringLiteral("the file is read-only. Choose a writable library file."));
    QSaveFile file(m_path);
    file.setDirectWriteFallback(false);
    if(!file.open(QIODevice::WriteOnly)) throw file_error(m_path,QStringLiteral("cannot open an atomic save: %1").arg(file.errorString()));
    if(file.write(bytes)!=bytes.size()) {
        const auto message=file.errorString();file.cancelWriting();
        throw file_error(m_path,QStringLiteral("cannot write the complete library: %1").arg(message));
    }
    // A non-cooperating writer may have changed the file while we prepared
    // the temporary output. Refuse replacement at the last available check.
    if(read_head(m_path)!=m_loaded_head) {file.cancelWriting();throw file_error(m_path,QStringLiteral("the file changed during save. Reload before saving."));}
    if(!file.commit()) throw file_error(m_path,QStringLiteral("cannot commit the atomic save: %1").arg(file.errorString()));
    m_document=std::move(candidate);
    m_loaded_head=bytes;
}
void TextLibraryStore::upsert(TextLibraryEntry entry) {
    auto candidate=m_document;
    const auto found=std::find_if(candidate.entries.begin(),candidate.entries.end(),[&](const auto& value){return value.id==entry.id;});
    if(found==candidate.entries.end()) candidate.entries.push_back(std::move(entry));
    else *found=std::move(entry);
    if (std::any_of(candidate.entries.begin(),candidate.entries.end(),[](const auto& value){
        return value.style.fill_opacity.has_value() || value.style.line_pattern!="solid" ||
            value.style.fill_pattern=="cross" || value.style.fill_pattern=="horizontal" || value.style.fill_pattern=="dots";
    })) candidate.version=3;
    commit(std::move(candidate));
}
void TextLibraryStore::remove(std::string_view id) {
    auto candidate=m_document;
    const auto found=std::find_if(candidate.entries.begin(),candidate.entries.end(),[&](const auto& value){return value.id==id;});
    if(found==candidate.entries.end()) throw std::invalid_argument("The reusable text entry is no longer in this library. Reload and select it again.");
    candidate.entries.erase(found);commit(std::move(candidate));
}

class TextLibraryDialog::Impl {
public:
    Impl(TextLibraryDialog* owner,TextLibraryStore& store,bool metric):owner(owner),store(store),metric(metric) {
        owner->setObjectName(QStringLiteral("textLibraryDialog"));
        owner->setWindowTitle(QStringLiteral("Text library"));
        owner->resize(720,540);
        auto* layout=new QVBoxLayout(owner);
        auto* filter=new QHBoxLayout;
        search=new QLineEdit(owner);search->setObjectName(QStringLiteral("textLibrarySearch"));
        search->setPlaceholderText(QStringLiteral("Search name, category or content"));
        categories=new QComboBox(owner);categories->setObjectName(QStringLiteral("textLibraryCategoryFilter"));
        filter->addWidget(search,1);filter->addWidget(categories);layout->addLayout(filter);
        auto* body=new QHBoxLayout;
        list=new QListWidget(owner);list->setObjectName(QStringLiteral("textLibraryList"));
        list->setMinimumWidth(220);body->addWidget(list,1);
        auto* editor=new QWidget(owner);auto* form=new QFormLayout(editor);
        const auto field=[&](const char* object,const QString& label){
            auto* value=new QLineEdit(editor);value->setObjectName(QString::fromLatin1(object));form->addRow(label,value);return value;};
        name=field("textLibraryName",QStringLiteral("Name"));name->setMaxLength(256);
        category=field("textLibraryCategory",QStringLiteral("Category"));category->setMaxLength(256);
        content=new QPlainTextEdit(editor);content->setObjectName(QStringLiteral("textLibraryContent"));
        content->setTabChangesFocus(true);form->addRow(QStringLiteral("Text"),content);
        height=field("textLibraryHeight",QStringLiteral("Text height"));
        font=field("textLibraryFont",QStringLiteral("Font"));
        alignment=new QComboBox(editor);alignment->setObjectName(QStringLiteral("textLibraryAlignment"));
        alignment->addItem(QStringLiteral("Left"),QStringLiteral("left"));
        alignment->addItem(QStringLiteral("Center"),QStringLiteral("center"));
        alignment->addItem(QStringLiteral("Right"),QStringLiteral("right"));
        form->addRow(QStringLiteral("Alignment"),alignment);
        color=field("textLibraryColor",QStringLiteral("Color"));color->setPlaceholderText(QStringLiteral("#RRGGBB"));
        auto* emphasis=new QWidget(editor);auto* emphasis_layout=new QHBoxLayout(emphasis);
        emphasis_layout->setContentsMargins(0,0,0,0);
        bold=new QCheckBox(QStringLiteral("Bold"),emphasis);bold->setObjectName(QStringLiteral("textLibraryBold"));
        italic=new QCheckBox(QStringLiteral("Italic"),emphasis);italic->setObjectName(QStringLiteral("textLibraryItalic"));
        emphasis_layout->addWidget(bold);emphasis_layout->addWidget(italic);emphasis_layout->addStretch();
        form->addRow(QStringLiteral("Style"),emphasis);body->addWidget(editor,2);layout->addLayout(body,1);
        error=new QLabel(owner);error->setObjectName(QStringLiteral("textLibraryError"));error->setWordWrap(true);
        error->setStyleSheet(QStringLiteral("color:#b42318;"));layout->addWidget(error);
        auto* actions=new QHBoxLayout;
        const auto button=[&](const char* object,const QString& label){auto* result=new QPushButton(label,owner);result->setObjectName(QString::fromLatin1(object));actions->addWidget(result);return result;};
        auto* fresh=button("textLibraryNew",QStringLiteral("New"));
        save=button("textLibrarySave",QStringLiteral("Save"));
        erase=button("textLibraryDelete",QStringLiteral("Delete"));actions->addStretch();
        auto* insert=button("textLibraryInsert",QStringLiteral("Insert"));
        auto* close=button("textLibraryClose",QStringLiteral("Close"));layout->addLayout(actions);
        QObject::connect(close,&QPushButton::clicked,owner,&QDialog::reject);
        QObject::connect(fresh,&QPushButton::clicked,owner,[this]{
            QSignalBlocker blocker(list);list->clearSelection();list->setCurrentRow(-1);
            TextLibraryEntry value{new_id(),"New text","notes","",{}};load(value,false,false);content->setFocus();});
        QObject::connect(list,&QListWidget::currentItemChanged,owner,[this](QListWidgetItem* item){select(item);});
        QObject::connect(search,&QLineEdit::textChanged,owner,[this]{refreshList();});
        QObject::connect(categories,&QComboBox::currentIndexChanged,owner,[this]{refreshList();});
        QObject::connect(save,&QPushButton::clicked,owner,[this]{saveEntry();});
        QObject::connect(erase,&QPushButton::clicked,owner,[this]{deleteEntry();});
        QObject::connect(insert,&QPushButton::clicked,owner,[this]{
            try {selected=readEntry();error->clear();this->owner->accept();}
            catch(const std::exception& exception){showError(exception);}});
        refreshCategories();refreshList();
        if(list->count()) list->setCurrentRow(0);
        else load({new_id(),"New text","notes","",{}},false,false);
    }

    void showError(const std::exception& exception) {error->setText(QString::fromUtf8(exception.what()));}
    std::vector<TextLibraryEntry> builtins() const {
        std::vector<TextLibraryEntry> result;
        for(const auto& value:default_label_templates()) {
            auto title=qtext(value.content).simplified();
            if(title.size()>64) title=title.left(61)+QStringLiteral("…");
            result.push_back({value.id,title.toStdString(),value.category,value.content,{}});
        }
        return result;
    }
    void refreshCategories() {
        const QSignalBlocker blocker(categories);
        const auto previous=categories->currentData().toString();
        categories->clear();categories->addItem(QStringLiteral("All categories"),QString{});
        std::set<QString> values;
        for(const auto& value:builtins()) values.insert(qtext(value.category));
        for(const auto& value:store.entries()) values.insert(qtext(value.category));
        for(const auto& value:values) categories->addItem(value,value);
        const auto index=categories->findData(previous);categories->setCurrentIndex(index<0 ? 0 : index);
    }
    void refreshList(const std::string& select_id={}) {
        const auto previous=select_id.empty() ? current.id : select_id;
        const QSignalBlocker blocker(list);list->clear();
        const auto query=search->text().trimmed();const auto category_filter=categories->currentData().toString();
        int row=-1;
        const auto add=[&](const TextLibraryEntry& value,bool builtin){
            if(!category_filter.isEmpty() && qtext(value.category)!=category_filter) return;
            if(!query.isEmpty() && !(qtext(value.name)+QLatin1Char(' ')+qtext(value.category)+QLatin1Char(' ')+qtext(value.content)).contains(query,Qt::CaseInsensitive)) return;
            auto* item=new QListWidgetItem(qtext(value.name)+(builtin ? QStringLiteral("  ·  Built-in") : QString{}),list);
            item->setData(Qt::UserRole,qtext(value.id));item->setData(Qt::UserRole+1,builtin);
            item->setToolTip(qtext(value.category));
            if(value.id==previous && (!select_id.empty() || builtin==current_builtin)) row=list->count()-1;
        };
        for(const auto& value:builtins()) add(value,true);
        for(const auto& value:store.entries()) add(value,false);
        if(row<0 && list->count()>0) row=0;
        list->setCurrentRow(row);
        if(row>=0) select(list->item(row));
    }
    void select(QListWidgetItem* item) {
        if(!item) return;
        const auto id=item->data(Qt::UserRole).toString().toStdString();
        const bool builtin=item->data(Qt::UserRole+1).toBool();
        if(builtin) {for(const auto& value:builtins()) if(value.id==id){load(value,true,false);return;}}
        else {for(const auto& value:store.entries()) if(value.id==id){load(value,false,true);return;}}
    }
    void load(TextLibraryEntry value,bool builtin,bool persisted) {
        current=std::move(value);current_builtin=builtin;current_persisted=persisted;
        name->setText(qtext(current.name));category->setText(qtext(current.category));content->setPlainText(qtext(current.content));
        height->setText(QString::number(current.style.text_height_metres/(metric ? 1.0 : .3048),'g',12)+(metric ? QStringLiteral(" m") : QStringLiteral(" ft")));
        original_height=height->text();
        font->setText(qtext(current.style.font_family));color->setText(qtext(current.style.stroke_color));
        alignment->setCurrentIndex(alignment->findData(qtext(current.style.text_alignment)));
        alignment->setEnabled(!builtin);
        bold->setChecked(current.style.bold);italic->setChecked(current.style.italic);
        for(auto* field:{name,category,height,font,color}) field->setReadOnly(builtin);
        content->setReadOnly(builtin);bold->setEnabled(!builtin);italic->setEnabled(!builtin);
        save->setText(builtin ? QStringLiteral("Save copy") : QStringLiteral("Save"));
        erase->setEnabled(persisted && !builtin);error->clear();
    }
    TextLibraryEntry readEntry() const {
        auto result=current;
        if(current_builtin) result.id=new_id();
        result.name=name->text().trimmed().toUtf8().toStdString();
        result.category=category->text().trimmed().toUtf8().toStdString();
        result.content=content->toPlainText().toUtf8().toStdString();
        if(height->text()!=original_height)
            result.style.text_height_metres=parse_quantity(height->text().toUtf8().toStdString(),metric ? Unit::metre : Unit::foot).metres;
        result.style.font_family=font->text().trimmed().toUtf8().toStdString();
        result.style.text_alignment=alignment->currentData().toString().toStdString();
        result.style.stroke_color=color->text().trimmed().toStdString();
        result.style.bold=bold->isChecked();result.style.italic=italic->isChecked();
        validate_text_library({1,{result}});return result;
    }
    void saveEntry() {
        try {
            auto value=readEntry();store.upsert(value);
            current_builtin=false;current_persisted=true;current=value;
            {const QSignalBlocker a(search),b(categories);search->clear();categories->setCurrentIndex(0);}
            refreshCategories();refreshList(value.id);error->clear();
        } catch(const std::exception& exception){showError(exception);}
    }
    void deleteEntry() {
        try {
            if(current_builtin || !current_persisted) throw std::invalid_argument("Only saved user text can be deleted.");
            store.remove(current.id);current={};refreshCategories();refreshList();error->clear();
            if(!list->count()) load({new_id(),"New text","notes","",{}},false,false);
        } catch(const std::exception& exception){showError(exception);}
    }

    TextLibraryDialog* owner;
    TextLibraryStore& store;
    bool metric;
    TextLibraryEntry current;
    bool current_builtin{};
    bool current_persisted{};
    QString original_height;
    std::optional<TextLibraryEntry> selected;
    QLineEdit *search{},*name{},*category{},*height{},*font{},*color{};
    QComboBox *categories{},*alignment{};
    QListWidget* list{};
    QPlainTextEdit* content{};
    QCheckBox *bold{},*italic{};
    QPushButton *save{},*erase{};
    QLabel* error{};
};

TextLibraryDialog::TextLibraryDialog(TextLibraryStore& store,bool metric,QWidget* parent)
    :QDialog(parent),m_impl(std::make_unique<Impl>(this,store,metric)) {}
TextLibraryDialog::~TextLibraryDialog()=default;
std::optional<TextLibraryEntry> TextLibraryDialog::selectedEntry() const {return m_impl->selected;}

} // namespace sketch::desktop
