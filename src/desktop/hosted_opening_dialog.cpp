#include "sketch/desktop/hosted_opening_dialog.hpp"
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPolygonF>
#include <QPushButton>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace sketch::desktop {
namespace {
class OpeningPreview final : public QWidget {
public:
    explicit OpeningPreview(Wall wall, QWidget* parent) : QWidget(parent), host(std::move(wall)) {
        setMinimumSize(320,180);
        setObjectName("openingPreview");
        setAccessibleName("Opening elevation along wall");
    }
    Wall host;
    std::optional<HostedOpening> draft;
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillRect(rect(),palette().base());
        try {
            const auto length=segment_length(host.baseline);
            const auto top=wall_top_height_range(host,0,length,-host.thickness/2,host.thickness/2);
            if(!std::isfinite(length) || length<=0 || !std::isfinite(top.maximum) || top.maximum<=0 ||
                !std::isfinite(top.minimum) || top.minimum<=0)
                throw std::invalid_argument("Invalid wall elevation");
            const double scale=std::min((width()-32.0)/length,(height()-36.0)/top.maximum);
            if(!std::isfinite(scale) || scale<=0) throw std::invalid_argument("Invalid preview size");
            const QRectF wall((width()-length*scale)/2,(height()-top.maximum*scale)/2,length*scale,top.maximum*scale);
            // Sample only the painted elevation, at approximately one point per device pixel.
            // The shared analytic extrema above determine the fit, including arc interiors.
            const double pixels=wall.width()*devicePixelRatioF();
            if(!std::isfinite(pixels)) throw std::invalid_argument("Invalid preview resolution");
            const int steps=static_cast<int>(std::clamp(std::ceil(pixels),2.0,16384.0));
            QPolygonF upper,lower;
            for(int i=0;i<=steps;++i) {
                const double station=length*(static_cast<double>(i)/steps);
                const double first=wall_top_height(host,station,-host.thickness/2);
                const double second=wall_top_height(host,station,host.thickness/2);
                if(!std::isfinite(first) || !std::isfinite(second) || first<=0 || second<=0)
                    throw std::invalid_argument("Invalid wall top profile");
                const double x=wall.left()+station*scale;
                const double upperY=wall.bottom()-std::max(first,second)*scale;
                const double lowerY=wall.bottom()-std::min(first,second)*scale;
                if(!std::isfinite(x) || !std::isfinite(upperY) || !std::isfinite(lowerY))
                    throw std::invalid_argument("Invalid preview coordinates");
                upper << QPointF(x,upperY);
                lower << QPointF(x,lowerY);
            }
            QPolygonF outline=upper;
            outline << wall.bottomRight() << wall.bottomLeft();
            QPolygonF band=upper;
            for(auto i=lower.crbegin();i!=lower.crend();++i) band << *i;
            painter.setPen(QPen(palette().text().color(),1));
            painter.setBrush(palette().base());
            painter.drawPolygon(outline);
            painter.setPen(Qt::NoPen);
            painter.setBrush(palette().alternateBase());
            painter.drawPolygon(band);
            painter.setPen(QPen(palette().text().color(),1));
            painter.setBrush(Qt::NoBrush);
            painter.drawPolygon(outline);
            painter.setPen(QPen(palette().mid().color(),1));
            painter.drawPolyline(lower);
            const auto draw=[&](const HostedOpening& opening, bool active) {
                const QRectF box(wall.left()+opening.offset*scale,
                    wall.bottom()-(opening.sill+opening.height)*scale,opening.width*scale,opening.height*scale);
                if(!std::isfinite(box.x()) || !std::isfinite(box.y()) ||
                    !std::isfinite(box.width()) || !std::isfinite(box.height()))
                    throw std::invalid_argument("Invalid opening preview coordinates");
                painter.setPen(QPen(active ? palette().highlight().color() : palette().mid().color(),active?2:1));
                painter.setBrush(active ? palette().highlight().color().lighter(180) : palette().base().color());
                painter.drawRect(box);
            };
            for(const auto& opening:host.openings) draw(opening,false);
            if(draft) draw(*draft,true);
        } catch(...) {
            painter.fillRect(rect(),palette().base());
            painter.setPen(palette().text().color());
            painter.drawText(rect().adjusted(16,16,-16,-16),Qt::AlignCenter | Qt::TextWordWrap,
                "Wall elevation preview unavailable");
        }
    }
};
}
HostedOpeningDialog::HostedOpeningDialog(const Wall& host, Unit unit, bool window, QWidget* parent)
    : QDialog(parent),host_(host),unit_(unit) {
    validate_wall_semantics(host_);
    setObjectName("hostedOpeningDialog");
    setWindowTitle(window ? "Create window" : "Create door");
    resize(440,390);
    auto* layout=new QVBoxLayout(this);
    auto* form=new QFormLayout;
    const std::array<const char*,4> labels{"Offset along wall","Width","Sill height","Height"};
    const std::array<const char*,4> names{"openingOffset","openingWidth","openingSill","openingHeight"};
    const bool metric=unit==Unit::metre || unit==Unit::centimetre || unit==Unit::millimetre;
    const std::array<QString,4> defaults{metric?"0.75 m":"2 ft",metric?"0.9 m":"3 ft",
        window?(metric?"0.9 m":"3 ft"):"0 m",window?(metric?"1.2 m":"4 ft"):(metric?"2.1 m":"7 ft")};
    for(std::size_t i=0;i<fields_.size();++i) {
        fields_[i]=new QLineEdit(defaults[i],this);
        fields_[i]->setObjectName(names[i]);
        form->addRow(labels[i],fields_[i]);
    }
    layout->addLayout(form);
    if(!window) {
        swing_=new QCheckBox("Show door swing",this);
        swing_->setObjectName("showDoorSwing");
        auto* row=new QHBoxLayout;
        row->addWidget(swing_);
        hinge_=new QComboBox(this);
        hinge_->setObjectName("doorHinge");
        hinge_->addItems({"Start jamb","End jamb"});
        hinge_->setToolTip("Jamb order follows the host wall's drawing direction.");
        side_=new QComboBox(this);
        side_->setObjectName("doorSwingSide");
        side_->addItems({"Swing left","Swing right"});
        side_->setToolTip("Side relative to the host wall's drawing direction.");
        row->addWidget(hinge_); row->addWidget(side_);
        hinge_->setEnabled(false); side_->setEnabled(false);
        connect(swing_,&QCheckBox::toggled,this,[this](bool on){hinge_->setEnabled(on);side_->setEnabled(on);});
        layout->addLayout(row);
    }
    if(host.baseline.sweep_radians!=0) layout->addWidget(new QLabel("Unrolled wall elevation",this));
    preview_=new OpeningPreview(host_,this);
    layout->addWidget(preview_,1);
    error_=new QLabel(this);
    error_->setObjectName("openingError");
    error_->setWordWrap(true);
    layout->addWidget(error_);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Cancel,this);
    create_=buttons->addButton("Create",QDialogButtonBox::AcceptRole);
    create_->setObjectName("createOpening");
    create_->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,this,[this]{submit();});
    for(auto* field:fields_) connect(field,&QLineEdit::textChanged,this,[this]{validate();});
    validate();
}
bool HostedOpeningDialog::validate() {
    auto* preview=static_cast<OpeningPreview*>(preview_);
    try {
        const auto value=[&](std::size_t index){return parse_quantity(fields_[index]->text().trimmed().toStdString(),unit_).metres;};
        HostedOpening opening{QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),value(0),value(1),value(2),value(3)};
        auto candidate=host_;
        candidate.openings.push_back(opening);
        validate_wall_semantics(candidate);
        preview->draft=opening;
        error_->hide();
        create_->setEnabled(true);
        preview->update();
        return true;
    } catch(const std::exception& error) {
        preview->draft.reset();
        error_->setText(QString::fromUtf8(error.what()));
        error_->show();
        create_->setEnabled(false);
        preview->update();
        return false;
    }
}
bool HostedOpeningDialog::submit(){if(!validate())return false;accept();return true;}
QString HostedOpeningDialog::offsetExpression()const{return fields_[0]->text();}
QString HostedOpeningDialog::widthExpression()const{return fields_[1]->text();}
QString HostedOpeningDialog::sillExpression()const{return fields_[2]->text();}
QString HostedOpeningDialog::heightExpression()const{return fields_[3]->text();}
std::optional<DoorOperation> HostedOpeningDialog::doorOperation()const {
    if(!swing_ || !swing_->isChecked()) return std::nullopt;
    return DoorOperation{hinge_->currentIndex()==1,side_->currentIndex()==0,90};
}
} // namespace sketch::desktop
