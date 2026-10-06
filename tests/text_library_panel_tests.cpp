#include "sketch/desktop/text_library_panel.hpp"
#include "support/noninteractive_errors.hpp"
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void click(QWidget* widget, const QPoint& point) {
    const QPointF global=widget->mapToGlobal(point);
    QMouseEvent press(QEvent::MouseButtonPress,point,global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(widget,&press);
    QMouseEvent release(QEvent::MouseButtonRelease,point,global,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(widget,&release);
    QApplication::processEvents();
}
void enter(QWidget* widget) {
    QKeyEvent press(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);
    QApplication::sendEvent(widget,&press);
    QKeyEvent release(QEvent::KeyRelease,Qt::Key_Return,Qt::NoModifier);
    QApplication::sendEvent(widget,&release);
    QApplication::processEvents();
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication app(argc,argv);
    try {
        sketch::desktop::TextLibraryPanel panel;
        auto* list=panel.findChild<QListWidget*>("textLibraryPanelList");
        auto* search=panel.findChild<QLineEdit*>("textLibraryPanelSearch");
        auto* categories=panel.findChild<QComboBox*>("textLibraryPanelCategory");
        auto* empty=panel.findChild<QLabel*>("textLibraryPanelEmpty");
        auto* manage=panel.findChild<QPushButton*>("textLibraryPanelManage");
        require(list&&search&&categories&&empty&&manage,"panel controls exist");
        sketch::TextLibraryEntry custom{"custom","<b>Inspection</b>","Notes","Caf\xc3\xa9\nMeasured <site>",{}};
        custom.style.font_family="Inter";custom.style.bold=true;custom.style.italic=true;
        custom.style.text_alignment="right";custom.style.text_height_metres=.37;
        custom.style.fill_opacity=.42;custom.style.line_pattern="dash";
        sketch::TextLibraryEntry other{"other","Door","Symbols","Entry",{}};
        int placements=0,manages=0;sketch::TextLibraryEntry placed;
        panel.setPlaceRequested([&](const auto& entry){++placements;placed=entry;});
        panel.setManageRequested([&]{++manages;});
        panel.setEntries({custom,other});panel.resize(360,480);panel.show();QApplication::processEvents();
        require(list->count()==2,"entries displayed");
        require(list->item(0)->text().contains("<b>Inspection</b>"),"names are literal text");
        click(list->viewport(),list->visualItemRect(list->item(0)).center());
        require(placements==1&&placed.id==custom.id,"single mouse click places once");
        require(placed.content==custom.content&&placed.style.font_family=="Inter"&&placed.style.bold&&placed.style.italic&&
            placed.style.text_alignment=="right"&&placed.style.text_height_metres==.37&&placed.style.fill_opacity==.42&&
            placed.style.line_pattern=="dash","exact content and custom style retained");
        enter(list);require(placements==2,"Enter places once");
        search->setText(QString::fromUtf8("CAF\xc3\x89"));
        require(list->count()==1,"unicode case-insensitive content search");
        categories->setCurrentIndex(categories->findData("Symbols"));require(list->count()==0&&!empty->isHidden(),"combined category and search empty state");
        search->clear();require(list->count()==1&&list->item(0)->data(Qt::UserRole).toString()=="other","category filtering");
        other.content="Replacement";panel.setEntries({custom,other});
        require(categories->currentData().toString()=="Symbols","category retained on refresh");
        click(list->viewport(),list->visualItemRect(list->item(0)).center());require(placed.content=="Replacement","refresh uses replacement data");
        panel.setEntries({custom});require(categories->currentData().toString().isEmpty()&&list->count()==1,"removed category resets sensibly");
        search->setText("Measured");panel.setEntries({custom});require(search->text()=="Measured"&&list->count()==1,"search retained");
        panel.setEntries({});enter(list);require(placements==3&&list->count()==0&&!empty->isHidden(),"removed entries cannot be activated");
        click(manage,manage->rect().center());require(manages==1,"Manage callback");
        panel.setStatus("<b>Ready</b>");auto* status=panel.findChild<QLabel*>("textLibraryPanelStatus");
        require(status&&status->textFormat()==Qt::PlainText&&status->text()=="<b>Ready</b>","status plain text");
        std::cout<<"Text library panel tests passed\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
