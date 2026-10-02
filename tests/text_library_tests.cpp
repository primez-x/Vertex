#include "sketch/text_library.hpp"
#include "sketch/desktop/text_library_dialog.hpp"

#include <QApplication>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QLabel>
#include <QLockFile>
#include <QTemporaryDir>

#include <iostream>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class F> void rejected(F action) {
    try { action(); } catch(const std::exception&) { return; }
    throw std::runtime_error("Invalid operation was accepted");
}
QByteArray bytes(const QString& path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly),"fixture must read"); return file.readAll();
}
void write(const QString& path,const QByteArray& value) {
    QFile file(path); require(file.open(QIODevice::WriteOnly|QIODevice::Truncate),"fixture must write");
    require(file.write(value)==value.size(),"fixture write must complete");
}
sketch::TextLibraryEntry entry() {
    sketch::TextLibraryEntry value{"user-text-fixture","Site note","notes","Verify dimensions\nOn site",{}};
    value.style.font_family="Inter"; value.style.text_height_metres=0.08;
    value.style.stroke_color="#123456"; value.style.bold=true; value.style.italic=true;
    return value;
}
void codec_checks() {
    const sketch::TextLibraryDocument source{1,{entry()}};
    const auto wire=sketch::encode_text_library(source);
    require(sketch::encode_text_library(sketch::decode_text_library(wire))==wire,"styled text must round-trip");
    for(const auto key:{"version","entries"}) {auto bad=wire;bad.erase(key);rejected([&]{(void)sketch::decode_text_library(bad);});}
    auto bad=wire;bad["version"]=2;rejected([&]{(void)sketch::decode_text_library(bad);});
    bad=wire;bad["opaque"]=true;rejected([&]{(void)sketch::decode_text_library(bad);});
    bad=wire;bad["entries"][0]["style"]["opaque"]=true;rejected([&]{(void)sketch::decode_text_library(bad);});
    bad=wire;bad["entries"].push_back(bad["entries"][0]);rejected([&]{(void)sketch::decode_text_library(bad);});
    bad=wire;bad["entries"][0]["style"]["bold"]="true";rejected([&]{(void)sketch::decode_text_library(bad);});
    bad=wire;bad["entries"][0]["style"]["stroke_color"]="not-a-color";rejected([&]{(void)sketch::decode_text_library(bad);});
    bad=wire;bad["entries"][0]["style"]["text_height_metres"]=-1;rejected([&]{(void)sketch::decode_text_library(bad);});
    bad=wire;bad["entries"][0]["vendor"]=17;rejected([&]{(void)sketch::decode_text_library(bad);});
    bad=wire;bad["entries"][0]["id"]="note";rejected([&]{(void)sketch::decode_text_library(bad);});
    for(const auto field:{"id","name","category","content"}) {bad=wire;bad["entries"][0][field]=" ";rejected([&]{(void)sketch::decode_text_library(bad);});}
    auto invalid=source;invalid.entries[0].style.text_height_metres=std::numeric_limits<double>::infinity();
    rejected([&]{(void)sketch::encode_text_library(invalid);});
    invalid=source;invalid.entries[0].content.assign(65537,'a');rejected([&]{(void)sketch::encode_text_library(invalid);});
    invalid=source;invalid.entries[0].content="embedded";invalid.entries[0].content.push_back('\0');rejected([&]{(void)sketch::encode_text_library(invalid);});
    invalid.entries.assign(1001,entry());rejected([&]{(void)sketch::encode_text_library(invalid);});
    invalid.entries.clear();
    for(int i=0;i<70;++i){auto item=entry();item.id+=std::to_string(i);item.content.assign(65536,'a');invalid.entries.push_back(item);}
    rejected([&]{(void)sketch::encode_text_library(invalid);});
}
void store_checks() {
    QTemporaryDir directory;require(directory.isValid(),"store fixture needs directory");
    const auto path=directory.filePath("text.json");
    sketch::desktop::TextLibraryStore store(path);
    require(store.entries().empty() && !QFile::exists(path),"absent file must load without silently creating a head");
    store.upsert(entry());const auto original=bytes(path);
    {
        QLockFile lock(path+".lock");require(lock.tryLock(0),"fixture must acquire cooperating writer lock");
        rejected([&]{store.remove(entry().id);});
        require(bytes(path)==original && store.entries().size()==1,"locked save must preserve head and entries");
    }
    sketch::desktop::TextLibraryStore second(path);
    auto update=entry();update.content="new external content";second.upsert(update);
    const auto newer=bytes(path);
    rejected([&]{store.upsert(entry());});
    require(bytes(path)==newer && store.entries()[0].content==entry().content,"stale save must preserve external head and retained data");
    store.reload();require(store.entries()[0].content==update.content,"reload must adopt a validated external update");
    write(path,"{\"version\":99,\"entries\":[]}");
    rejected([&]{store.reload();});rejected([&]{store.remove(entry().id);});
    require(bytes(path).contains("99") && store.entries()[0].content==update.content,"future library must survive reload/write refusal");
    rejected([&]{sketch::desktop::TextLibraryStore malformed(path);});
    write(path,original);store.reload();store.remove(entry().id);
    require(store.entries().empty(),"remove must persist user deletion");
    const auto blocker=directory.filePath("parent-file");write(blocker,"keep");
    rejected([&]{sketch::desktop::TextLibraryStore blocked(blocker+"/text.json");blocked.upsert(entry());});
    require(bytes(blocker)=="keep","unwritable destination must preserve existing bytes");
    write(path,original);sketch::desktop::TextLibraryStore readonly(path);
    const auto permissions=QFile::permissions(path);
    require(QFile::setPermissions(path,QFileDevice::ReadOwner|QFileDevice::ReadUser),"fixture must mark file read-only");
    rejected([&]{readonly.upsert(update);});require(bytes(path)==original,"read-only save must preserve file");
    require(QFile::setPermissions(path,permissions),"restore fixture permissions");
    const auto absent=directory.filePath("absent.json");
    sketch::desktop::TextLibraryStore absent_first(absent),absent_second(absent);
    absent_first.upsert(entry());const auto created=bytes(absent);
    rejected([&]{absent_second.upsert(update);});
    require(bytes(absent)==created,"an initially absent head must refuse replacing another writer's new file");
}
void dialog_checks() {
    QTemporaryDir directory;sketch::desktop::TextLibraryStore store(directory.filePath("text.json"));
    sketch::desktop::TextLibraryDialog dialog(store,true);
    const auto button=[&](const char* name){auto* value=dialog.findChild<QPushButton*>(QString::fromLatin1(name));require(value,"dialog needs button");return value;};
    auto* list=dialog.findChild<QListWidget*>("textLibraryList");require(list && list->count()>0,"built-ins must be available offline");
    list->setCurrentRow(0);
    auto* content=dialog.findChild<QPlainTextEdit*>("textLibraryContent");
    require(content && content->isReadOnly(),"built-ins must be read-only");
    button("textLibrarySave")->click();
    require(store.entries().size()==1 && store.entries()[0].id.starts_with("user-text-"),"Save copy must create a user entry");
    require(!content->isReadOnly(),"saved copy must be editable");
    dialog.findChild<QLineEdit*>("textLibraryName")->setText("Personal note");
    content->setPlainText("Authored reusable note\nSecond line");
    dialog.findChild<QLineEdit*>("textLibraryHeight")->setText("3 in");
    dialog.findChild<QLineEdit*>("textLibraryColor")->setText("#345678");
    dialog.findChild<QCheckBox*>("textLibraryBold")->setChecked(true);
    button("textLibrarySave")->click();
    require(store.entries().size()==1 && store.entries()[0].content==content->toPlainText().toStdString() &&
        std::abs(store.entries()[0].style.text_height_metres-.0762)<1e-12 && store.entries()[0].style.bold,"actual Save must persist content and parsed style");
    content->setPlainText("Unsaved insertion draft");button("textLibraryInsert")->click();
    auto placed=dialog.selectedEntry();require(placed && placed->content=="Unsaved insertion draft" &&
        store.entries()[0].content!="Unsaved insertion draft","Insert must return current draft without changing reusable entry");
    auto modified=store.entries()[0];modified.content="Later library edit";store.upsert(modified);
    require(placed->content=="Unsaved insertion draft","returned label content must be independent of later library edits");
    sketch::desktop::TextLibraryDialog failed(store,false);
    auto* failed_list=failed.findChild<QListWidget*>("textLibraryList");
    for(int row=0;row<failed_list->count();++row) if(failed_list->item(row)->data(Qt::UserRole).toString()==QString::fromStdString(modified.id)) failed_list->setCurrentRow(row);
    failed.findChild<QPlainTextEdit*>("textLibraryContent")->setPlainText("Must not overwrite");
    const auto external=bytes(store.path())+"\n";write(store.path(),external);
    failed.findChild<QPushButton*>("textLibrarySave")->click();
    auto* error=failed.findChild<QLabel*>("textLibraryError");
    require(bytes(store.path())==external && store.entries()[0].content==modified.content && !failed.selectedEntry() &&
        error && error->text().contains("Reload"),"failed dialog Save must report error and preserve newer file and prior entries");
    store.reload();
    failed.findChild<QPushButton*>("textLibraryDelete")->click();
    require(store.entries().empty(),"actual Delete must remove only the saved user entry");
    failed.findChild<QPushButton*>("textLibraryNew")->click();
    auto* fresh=failed.findChild<QPlainTextEdit*>("textLibraryContent");fresh->setPlainText("New unsaved note");
    failed.findChild<QPushButton*>("textLibraryInsert")->click();
    require(failed.selectedEntry() && failed.selectedEntry()->content=="New unsaved note" && store.entries().empty(),
        "New draft Insert must remain independent of the reusable file");
}
}

int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try{codec_checks();store_checks();dialog_checks();std::cout<<"text_library_tests passed\n";return 0;}
    catch(const std::exception& error){std::cerr<<"text_library_tests: "<<error.what()<<'\n';return 1;}
}
