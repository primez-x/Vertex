#include "sketch/desktop/appraisal_details_panel.hpp"
#include "sketch/desktop/appraisal_report_dialog.hpp"
#include "plan_canvas.hpp"
#include "measurement_format.hpp"
#include "bundled_fonts.hpp"
#include "vertex_design_tokens.hpp"

#include <QFont>
#include <QHeaderView>
#include <QTimer>
#include <QHBoxLayout>
#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QToolButton>
#include <QTextDocument>
#include <QTextOption>
#include <QEvent>
#include <QStyle>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace sketch::desktop {
namespace {
QString text(const std::string& value) { return QString::fromStdString(value); }
QString words(std::string_view token) {
    auto value=QString::fromUtf8(token.data(),static_cast<qsizetype>(token.size()));
    value.replace(QLatin1Char('_'),QLatin1Char(' '));
    if(!value.isEmpty()) value[0]=value[0].toUpper();return value;
}
// People-facing source names: declared name, else type, ordinal and floor.
// Raw IDs stay in the collapsed provenance view only.
QString name(const DocumentSnapshot& source,const std::string& id) {
    return appraisal_source_label(source,id);
}
std::string field(const Entity& entity,const char* key) {
    const auto found=entity.properties.find(key);
    return found!=entity.properties.end() && found->is_string() ? found->get<std::string>() : std::string{};
}
QString declaration(const Entity* entity,const char* object,const char* key) {
    if(entity) {
        const auto facts=entity->properties.find(object);
        if(facts!=entity->properties.end() && facts->is_object()) {
            const auto value=facts->find(key);
            if(value!=facts->end()) {
                if(value->is_string()) return words(value->get<std::string>());
                if(value->is_number_integer()) return text(value->dump());
                return QStringLiteral("Malformed declaration");
            }
        }
    }
    return QStringLiteral("Undeclared");
}
// True while a declared fact still holds the value recorded from the area
// type chosen when the area was drawn (appraisal_fact_defaults).
bool from_classification(const Entity* entity,const char* key) {
    if(!entity)return false;
    const auto record=entity->properties.find("appraisal_fact_defaults");
    const auto facts=entity->properties.find("appraisal_facts");
    if(record==entity->properties.end() || facts==entity->properties.end() || !record->is_object() || !facts->is_object())return false;
    const auto recorded=record->find("facts");
    if(recorded==record->end() || !recorded->is_object() || !recorded->contains(key) || !facts->contains(key))return false;
    return recorded->at(key)==facts->at(key);
}
QString fact(const Entity* entity,const char* key,bool& defaulted) {
    auto value=declaration(entity,"appraisal_facts",key);
    if(from_classification(entity,key)) {defaulted=true;value+=QStringLiteral(" · from classification");}
    return value;
}
QString row(const QString& label,const QString& value) {
    return QStringLiteral("<tr><td style='padding:3px 8px 3px 0'>%1</td><td style='padding:3px 0'>%2</td></tr>")
        .arg(label.toHtmlEscaped(),value.toHtmlEscaped());
}
QString table(const QString& content) {
    // Details has a narrow canvas-side viewport. Keep the shared report rows,
    // but place each escaped value below its label instead of forcing columns
    // to the minimum width of a long classification or observed quantity.
    static const QRegularExpression cells(QStringLiteral("<tr><td[^>]*>(.*?)</td><td[^>]*>(.*?)</td></tr>"),
        QRegularExpression::DotMatchesEverythingOption);
    QString result;auto matches=cells.globalMatch(content);
    while(matches.hasNext()) {
        const auto match=matches.next();
        result+=QStringLiteral("<p style='margin-top:0; margin-bottom:6px'><b>%1</b><br>%2</p>")
            .arg(match.captured(1),match.captured(2));
    }
    return result;
}
QString boolean(const std::optional<bool>& value) {
    return value?(*value?QStringLiteral("Yes"):QStringLiteral("No")):QStringLiteral("Undeclared");
}
QString measurement_declarations(const AnsiMeasurementDeclarations& value) {
    return row(QStringLiteral("Interior inspected"),boolean(value.interior_inspected))+
        row(QStringLiteral("Direct measurement"),boolean(value.direct_measurement))+
        row(QStringLiteral("Acquisition increment"),value.acquisition_increment?words(acquisition_increment_name(*value.acquisition_increment)):QStringLiteral("Undeclared"))+
        row(QStringLiteral("Supplemental limitations statement"),value.limitations_statement.empty()?QStringLiteral("Undeclared"):text(value.limitations_statement))+
        appraisal_ansi_declaration_rows(value);
}
QLabel* label(QWidget* parent,const char* object,const QString& value={}) {
    auto* result=new QLabel(value,parent);result->setObjectName(QString::fromLatin1(object));
    result->setWordWrap(true);result->setMinimumWidth(0);
    result->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    result->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    return result;
}
void heading(QVBoxLayout* layout,const QString& title) {
    auto* value=label(layout->parentWidget(),"",title);auto font=value->font();font.setBold(true);value->setFont(font);
    layout->addSpacing(6);layout->addWidget(value);
}
// The GLA headline: sans for words ("Totals unavailable"), mono only for a
// figure. The workspace style sheet keys the face on the "figure" property.
void set_gla(QLabel* gla,const QString& value,bool figure) {
    gla->setText(value);
    auto font=gla->font();
    if(figure) {font.setFamilies({QStringLiteral("IBM Plex Mono"),QStringLiteral("Cascadia Mono"),QStringLiteral("Consolas")});font.setPointSize(23);}
    else {font.setFamilies({bundled_sans_family()});font.setPointSize(13);}
    font.setBold(true);gla->setFont(font);
    if(gla->property("figure").isValid() && gla->property("figure").toBool()==figure) return;
    gla->setProperty("figure",figure);
    gla->style()->unpolish(gla);gla->style()->polish(gla);
}
double relative_luminance(const QColor& color) {
    const auto channel=[](double value){return value<=0.03928?value/12.92:std::pow((value+0.055)/1.055,2.4);};
    return 0.2126*channel(color.redF())+0.7152*channel(color.greenF())+0.0722*channel(color.blueF());
}
// Rich-text links use the theme's accent-text token, which the workspace
// theme publishes as palette Link. Where Link is Qt's default blue or would not
// read on this surface (under 4.5:1), the surface's design token is used.
QColor link_color(const QWidget* widget) {
    const auto surface=widget->palette().color(QPalette::Window);
    auto color=widget->palette().color(QPalette::Link);
    const auto a=relative_luminance(color),b=relative_luminance(surface);
    if(color==QColor(0,0,0xee) || color==QColor(0,0,0xff) || (std::max(a,b)+0.05)/(std::min(a,b)+0.05)<4.5)
        color=design::for_surface(surface.lightness()>=128).accent_text;
    return color;
}
class LinkColorSync final : public QObject {
public:
    explicit LinkColorSync(QWidget* panel):QObject(panel),panel_(panel) {panel->installEventFilter(this);}
    bool eventFilter(QObject* watched,QEvent* event) override {
        if(watched==panel_ && (event->type()==QEvent::PaletteChange || event->type()==QEvent::StyleChange ||
                               event->type()==QEvent::Show))sync();
        return false;
    }
    void sync() {
        const auto color=link_color(panel_);
        for(auto* value:panel_->findChildren<QLabel*>()) {
            if(value->palette().color(QPalette::Link)==color)continue;
            auto palette=value->palette();palette.setColor(QPalette::Link,color);value->setPalette(palette);
        }
    }
private:
    QWidget* panel_;
};
QString category(const AppraisalBoundaryStatus& status) {
    if(!status.qualification.qualified) return QStringLiteral("Unqualified");
    if(status.exclusion) return QStringLiteral("Deduction only");
    return status.qualification.derived_category ? appraisal_category_label(*status.qualification.derived_category) : QStringLiteral("Unavailable");
}
// AreaTable legend chip: the area's design-system fill (area-gla,
// area-basement, area-garage, area-porch or area-other) on a light or dark
// surface, outlined so a translucent fill still reads as a swatch.
QIcon area_chip(const AppraisalBoundaryStatus& status,bool light_surface) {
    const auto& tokens=design::for_surface(light_surface);
    QColor fill=tokens.area_other;
    if(status.qualification.qualified && !status.exclusion && status.qualification.derived_category) {
        switch(*status.qualification.derived_category) {
        case AppraisalAreaCategory::above_grade_finished: fill=tokens.area_gla;break;
        case AppraisalAreaCategory::below_grade_finished: case AppraisalAreaCategory::below_grade_unfinished:
        case AppraisalAreaCategory::below_grade_nonstandard_finished:
        case AppraisalAreaCategory::adu_below_grade_finished: case AppraisalAreaCategory::adu_below_grade_unfinished:
        case AppraisalAreaCategory::adu_below_grade_nonstandard_finished:
        case AppraisalAreaCategory::detached_other_below_grade_finished: case AppraisalAreaCategory::detached_other_below_grade_unfinished:
        case AppraisalAreaCategory::detached_other_below_grade_nonstandard_finished: fill=tokens.area_basement;break;
        case AppraisalAreaCategory::garage: case AppraisalAreaCategory::carport: fill=tokens.area_garage;break;
        case AppraisalAreaCategory::porch: case AppraisalAreaCategory::patio: case AppraisalAreaCategory::deck: fill=tokens.area_porch;break;
        default: break;
        }
    }
    QPixmap chip(12,12);chip.fill(Qt::transparent);
    QPainter painter(&chip);painter.setRenderHint(QPainter::Antialiasing);
    // A filled chip: the opaque area colour, so it reads as a swatch rather
    // than an outline at legend size.
    fill.setAlpha(255);
    painter.setPen(Qt::NoPen);painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(1.5,1.5,9,9),design::radius_sm/2.0,design::radius_sm/2.0);
    painter.end();
    return QIcon(chip);
}
}

struct AppraisalDetailsPanel::Impl {
    std::optional<DocumentSnapshot> source;
    std::optional<AppraisalDocumentReport> report;
    // Own the mask: callers commonly pass a refresh-local set. Null means all
    // semantic entities; an empty set means none, so they must stay distinct.
    std::optional<std::set<std::string,std::less<>>> semantic_phase_ids;
    std::string property;
    bool metric{};
    QLabel *property_name{},*gla{},*rounding{},*status{},*standards{},*policy{},*totals{},*issues{},*trace{};
    QTreeWidget* areas{};
    QToolButton* provenance_toggle{};
    QPlainTextEdit* provenance{};
    QPushButton *setup{},*facts{},*full_report{},*locate{},*review_sources{},*reporting_settings{};
    std::function<void(const QString&,Revision)> locate_requested,facts_requested,source_review_requested;
    std::function<void(const QString&)> setup_requested,report_requested;
    std::function<void(const QString&,Revision)> reporting_requested;
    std::vector<AppraisalIssuePresentation> issue_items;

    const Entity* entity(const std::string& id) const {
        if(!source)return nullptr;const auto found=source->entities().find(id);
        return found==source->entities().end()?nullptr:&found->second;
    }
    bool ansi() const {return report && report->policy && report->policy->kind==AppraisalPolicyKind::ansi_z765_2021;}
    CalculationProfile profile() const {
        if(ansi())return ansi_appraisal_profile(report->policy->version);
        auto value=builtin_appraisal_profile();value.display_unit=metric?AreaUnit::square_metre:AreaUnit::square_foot;
        if(report)value.decimal_places=report->display_decimal_places;return value;
    }
    QString area(double square_metres) const {
        return units::area_text(display_area(square_metres,profile()));
    }
    QString length(double metres) const {
        if (!ansi()) return PlanCanvas::drawingLengthText(metres, metric);
        return units::ansi_length_text(metres);
    }
    QString ansi_facts(const AppraisalFacts& facts) const {
        if(!facts.ansi)return {};
        const auto& value=*facts.ansi;const auto& ceiling=value.ceiling;
        auto rows=row(QStringLiteral("Any part below grade"),boolean(value.any_part_below_grade))+
            row(QStringLiteral("Year-round suitable"),boolean(value.year_round_suitable))+
            row(QStringLiteral("Finish matches dwelling"),boolean(value.finish_matches_dwelling))+
            row(QStringLiteral("Dwelling identity"),value.dwelling_identity?words(dwelling_identity_name(*value.dwelling_identity)):QStringLiteral("Undeclared"))+
            row(QStringLiteral("Ceiling type"),ceiling.kind?words(ceiling_kind_name(*ceiling.kind)):QStringLiteral("Undeclared"));
        rows+=appraisal_ceiling_height_rows(value);
        if(ceiling.at_least_7ft_area_m2)rows+=row(QStringLiteral("Ceiling area at least 7 ft"),area(*ceiling.at_least_7ft_area_m2));
        if(ceiling.room_floor_area_m2)rows+=row(QStringLiteral("Room floor area"),area(*ceiling.room_floor_area_m2));
        if(ceiling.kind==CeilingKind::sloped) {
            QStringList ids;for(const auto& id:ceiling.below_5ft_deduction_ids)ids.push_back(name(*source,id));
            rows+=row(QStringLiteral("Actual below-5-ft deduction sources"),ids.isEmpty()?QStringLiteral("None declared"):ids.join(QStringLiteral(", ")))+
                row(QStringLiteral("Room boundary source"),name(*source,ceiling.room_boundary_id));
        }
        if(ceiling.kind==CeilingKind::stairs)rows+=row(QStringLiteral("Stair from floor source"),name(*source,ceiling.stair_from_floor_id));
        return QStringLiteral("<p><b>ANSI source facts</b></p>")+table(rows);
    }
    QString selected() const {
        const auto* item=areas->currentItem();return item?item->data(0,Qt::UserRole).toString():QString{};
    }
    void locate_current() {
        const auto id=selected();if(report && !id.isEmpty() && locate_requested)locate_requested(id,report->revision);
    }
    // Issue links reuse the panel's existing actions: select the area (row and
    // canvas), Edit facts, Appraisal setup, or Rooms and units.
    void issue_activated(const QString& link) {
        const auto parts=link.split(QLatin1Char(':'));
        bool valid=false;const auto index=parts.size()==3 ? parts[1].toULongLong(&valid) : 0;
        if(!report || !valid || index>=issue_items.size())return;
        const auto& value=issue_items[static_cast<std::size_t>(index)];
        const auto action=parts[2];const auto revision=report->revision;
        const auto boundary=text(value.source_id);
        if(!boundary.isEmpty() && (action==QStringLiteral("select") || action==QStringLiteral("facts"))) {
            QTreeWidgetItemIterator item(areas);
            while(*item) {if((*item)->data(0,Qt::UserRole).toString()==boundary){areas->setCurrentItem(*item);areas->scrollToItem(*item);break;}++item;}
        }
        if(action==QStringLiteral("select") && !boundary.isEmpty() && locate_requested)locate_requested(boundary,revision);
        else if(action==QStringLiteral("facts") && !boundary.isEmpty() && facts_requested)facts_requested(boundary,revision);
        else if(action==QStringLiteral("setup") && setup_requested && entity(property))setup_requested(text(property));
        else if(action==QStringLiteral("reporting") && reporting_requested)reporting_requested(text(property),revision);
    }
    void show_trace() {
        const auto id=selected();const AppraisalBoundaryStatus* boundary=nullptr;
        if(report)for(const auto& value:report->boundaries)if(text(value.boundary_id)==id){boundary=&value;break;}
        locate->setEnabled(boundary!=nullptr);facts->setEnabled(boundary!=nullptr);
        // Disabled actions say why.
        const auto pick_first=QStringLiteral("Select an area in the table first.");
        locate->setToolTip(boundary ? QStringLiteral("Select this area on the drawing") : pick_first);
        facts->setToolTip(boundary ? QStringLiteral("Declare finish, access, ceiling eligibility and use for this area") : pick_first);
        const auto* selected_owner=boundary?entity(boundary->boundary_id):nullptr;
        const bool has_linework_sources=selected_owner && selected_owner->type=="measurement_boundary" &&
            (selected_owner->extensions.contains("measurement_linework_sources") ||
             selected_owner->extensions.contains("measurement_linework_group"));
        review_sources->setVisible(has_linework_sources);
        review_sources->setEnabled(has_linework_sources);
        if(!boundary || !source) {
            provenance->clear();provenance_toggle->setEnabled(false);
            provenance_toggle->setChecked(false);
            trace->setText(report && !report->configured ?
                QStringLiteral("Areas appear here after Appraisal setup. Then select an area and choose Edit facts to declare its finish, access, ceiling eligibility and use.") :
                report && report->boundaries.empty() ?
                QStringLiteral("Draw a measured area, then select it here and choose Edit facts to declare its finish, access, ceiling eligibility and use.") :
                QStringLiteral("Select an area above to inspect its dimensions, declarations and deductions."));return;
        }
        const auto* owner=entity(boundary->boundary_id);
        const auto floor_id=owner?field(*owner,"floor_id"):std::string{};
        QStringList raw_sources{QStringLiteral("Boundary ID: %1").arg(text(boundary->boundary_id)),
            QStringLiteral("Floor ID: %1").arg(text(floor_id)),
            QStringLiteral("Document revision: %1").arg(report->revision)};
        if(boundary->facts && boundary->facts->ansi) {
            const auto& ceiling=boundary->facts->ansi->ceiling;
            if(!ceiling.room_boundary_id.empty())raw_sources.push_back(QStringLiteral("Room boundary ID: %1").arg(text(ceiling.room_boundary_id)));
            if(!ceiling.source_geometry_sha256.empty())raw_sources.push_back(QStringLiteral("Ceiling geometry SHA-256: %1").arg(text(ceiling.source_geometry_sha256)));
            if(!ceiling.stair_from_floor_id.empty())raw_sources.push_back(QStringLiteral("Stair from floor ID: %1").arg(text(ceiling.stair_from_floor_id)));
            for(const auto& deduction_id:ceiling.below_5ft_deduction_ids)raw_sources.push_back(QStringLiteral("Below-5-ft deduction ID: %1").arg(text(deduction_id)));
        }
        if(boundary->measurement)for(const auto& deduction:boundary->measurement->deductions)
            raw_sources.push_back(QStringLiteral("Deduction ID: %1").arg(text(deduction.id)));
        provenance->setPlainText(raw_sources.join(QLatin1Char('\n')));provenance_toggle->setEnabled(true);
        auto html=QStringLiteral("<p><b>%1</b><br>%2</p>").arg(name(*source,boundary->boundary_id).toHtmlEscaped(),category(*boundary).toHtmlEscaped());
        bool defaulted=false;
        auto facts_html=table(row(QStringLiteral("Floor grade"),fact(entity(floor_id),"grade",defaulted))+
            row(QStringLiteral("Finish"),fact(owner,"finish",defaulted))+
            row(QStringLiteral("Access"),fact(owner,"access",defaulted))+
            row(QStringLiteral("Ceiling eligibility"),fact(owner,"ceiling_eligibility",defaulted))+
            row(QStringLiteral("Use"),fact(owner,"area_use",defaulted))+
            row(QStringLiteral("Boundary role"),fact(owner,"boundary_role",defaulted)));
        // Facts recorded from the area type are explicit and editable.
        if(defaulted)facts_html=QStringLiteral("<p>From classification · edit facts</p>")+facts_html;
        if(ansi() && boundary->facts)facts_html+=ansi_facts(*boundary->facts);
        if(boundary->measurement) {
            const auto& value=*boundary->measurement;
            if(boundary->exclusion)html+=QStringLiteral("<p>Deduction source only; no standalone contribution to totals.</p>");
            else if(!report->qualified || !boundary->qualification.qualified)
                html+=QStringLiteral("<p><b>Diagnostic geometry.</b> Property totals are withheld until qualification issues are resolved.</p>");
            html+=table(row(QStringLiteral("Gross boundary"),area(value.base_square_metres))+
                row(QStringLiteral("Deductions (union)"),area(value.deducted_square_metres))+
                row(QStringLiteral("Physical net"),area(value.net_square_metres))+
                row(QStringLiteral("Exact factor"),QStringLiteral("%1/%2").arg(value.factor.numerator).arg(value.factor.denominator))+
                row(QStringLiteral("Adjusted area"),area(value.factored_square_metres))+
                row(QStringLiteral("Boundary perimeter"),length(value.perimeter_metres)));
            if(owner)html+=table(appraisal_area_arithmetic_rows(*owner,value.base_square_metres,*report,metric,true));
            if(ansi())html+=table(appraisal_sloped_ceiling_rows(*boundary,*report));
            // A current validated trace authorizes showing these analytical edge
            // lengths; stale/invalid sources never expose this branch.
            if(owner) {
                // Current editable areas store their authoritative analytical
                // edges in segments; anonymous legacy areas may use boundary.
                auto geometry=owner->properties.find("segments");
                if(geometry==owner->properties.end())geometry=owner->properties.find("boundary");
                if(geometry!=owner->properties.end() && geometry->is_array()) {
                    QStringList edges;std::size_t index=0;
                    try {
                        for(const auto& edge:*geometry) {
                            const auto& start=edge.at("start");const auto& end=edge.at("end");
                            Segment segment{{start.at(0).get<double>(),start.at(1).get<double>()},
                                {end.at(0).get<double>(),end.at(1).get<double>()},edge.at("sweep_radians").get<double>()};
                            edges.push_back(QStringLiteral("%1%2: %3").arg(segment.sweep_radians==0?QStringLiteral("Edge "):QStringLiteral("Arc "))
                                .arg(++index).arg(length(segment_length(segment))));
                        }
                        if(!edges.isEmpty())html+=QStringLiteral("<p><b>Boundary dimensions</b><br>%1</p>").arg(edges.join(QStringLiteral("<br>")));
                    } catch(const std::exception&) {html+=QStringLiteral("<p>Edge dimensions unavailable.</p>");}
                }
            }
            if(!value.deductions.empty()) {
                html+=QStringLiteral("<p><b>Deduction reasons and sources</b></p>");
                for(const auto& deduction:value.deductions) {
                    html+=QStringLiteral("<p>%1 · %2<br>Requested: %3<br>Applied: %4</p>")
                        .arg(name(*source,deduction.id).toHtmlEscaped(),declaration(entity(deduction.id),"appraisal_facts","boundary_role").toHtmlEscaped(),
                            area(deduction.requested_square_metres).toHtmlEscaped(),area(deduction.applied_square_metres).toHtmlEscaped());
                }
                html+=QStringLiteral("<p>Overlapping deductions remove shared area once. Applied amounts are marginal contributions in source-ID order.</p>");
            }
            const auto display=display_area(value.factored_square_metres,profile());
            html+=QStringLiteral("<p><b>Calculation trace</b></p>")+table(
                row(QStringLiteral("Profile"),text(value.profile_id)+QStringLiteral(" v%1").arg(value.profile_version))+
                row(QStringLiteral("Classification"),words(value.classification))+
                row(QStringLiteral("Unrounded adjusted area"),QString::number(display.unrounded,'g',17)+QLatin1Char(' ')+units::area_unit_suffix(display.unit))+
                row(QStringLiteral("Rounding change"),QString::number(display.rounding_delta,'g',12)+QLatin1Char(' ')+units::area_unit_suffix(display.unit)));
        } else html+=QStringLiteral("<p><b>Current measurement unavailable.</b> Resolve the source geometry or dependency issues before using numeric dimensions.</p>");
        html+=QStringLiteral("<p><b>Area declarations</b></p>")+facts_html;
        if(!boundary->qualification.issues.empty()) {
            html+=QStringLiteral("<p><b>Issues to resolve</b><br>Use Edit facts below for this area's declarations, or Appraisal setup above for the property policy.</p><ul>");
            for(const auto& issue:boundary->qualification.issues)html+=QStringLiteral("<li>%1</li>").arg(appraisal_issue_message(*source,issue.message).toHtmlEscaped());
            html+=QStringLiteral("</ul>");
        }
        if(ansi() && !boundary->qualification.rule_notes.empty()) {
            html+=QStringLiteral("<p><b>Classification reasons and rule limitations</b></p><ul>");
            for(const auto& note:boundary->qualification.rule_notes)html+=QStringLiteral("<li>%1</li>").arg(text(note).toHtmlEscaped());
            html+=QStringLiteral("</ul>");
        }
        QStringList parents;
        for(const auto& candidate:report->boundaries)if(candidate.measurement)
            for(const auto& deduction:candidate.measurement->deductions)if(deduction.id==boundary->boundary_id)parents.push_back(name(*source,candidate.boundary_id));
        if(!parents.isEmpty())html+=QStringLiteral("<p>Deducted from: %1</p>").arg(parents.join(QStringLiteral(", ")).toHtmlEscaped());
        // The document revision is listed with the source IDs in provenance.
        trace->setText(html);
    }
    // Size the area table to its rows (up to its maximum) so a short list
    // does not leave a large empty block above the totals.
    void fit_areas() {
        int height=areas->header()->isVisible() ? areas->header()->sizeHint().height() : 0;
        int rows=0;
        for(QTreeWidgetItemIterator item(areas);*item;++item) {
            const auto bounds=areas->visualItemRect(*item);
            height+=bounds.height()>0 ? bounds.height() : std::max(28,areas->fontMetrics().height()+10);
            ++rows;
        }
        height+=2*areas->frameWidth()+4;
        if(rows==0)height=areas->minimumHeight();
        areas->setFixedHeight(std::clamp(height,72,320));
    }
    void show_report(const QString& previous) {
        const QSignalBlocker blocker(areas);areas->clear();
        const bool available=report && source;
        setup->setEnabled(entity(property)!=nullptr);full_report->setEnabled(available);
        reporting_settings->setEnabled(available && bool(reporting_requested));
        property_name->setText(source && entity(property)?name(*source,property):QStringLiteral("Property details"));
        totals->clear();issues->clear();policy->clear();standards->clear();rounding->clear();rounding->hide();issue_items.clear();
        set_gla(gla,QStringLiteral("Totals unavailable"),false);
        if(!available) {
            status->setText(QStringLiteral("Choose a property to inspect its appraisal areas and declarations."));show_trace();return;
        }
        const auto* owner=entity(property);
        if(report->policy) {
            policy->setText(table(row(QStringLiteral("Measurement basis"),declaration(owner,"appraisal_policy","measurement_basis"))+
                row(QStringLiteral("Declared policy"),words(appraisal_policy_kind_name(report->policy->kind))+QStringLiteral(" v%1").arg(report->policy->version))+
                row(QStringLiteral("Calculation profile"),text(profile().id)+QStringLiteral(" v%1").arg(profile().version))));
            if(ansi()) {
                if(report->ansi_measurement)policy->setText(policy->text()+table(measurement_declarations(*report->ansi_measurement)));
                QString statement=QStringLiteral("ANSI profile: final-standard validation pending (unresolved normative validation).");
                if(!report->policy_limitations.empty())statement+=QLatin1Char('\n')+text(report->policy_limitations.front());
                standards->setText(statement);
            } else standards->setText(report->policy->kind==AppraisalPolicyKind::residential_declared ?
                QStringLiteral("ANSI review not verified. These results use the declared Vertex policy and do not establish ANSI certification.") :
                QStringLiteral("ANSI residential standard not applicable. These results use the declared commercial policy and do not establish BOMA certification."));
        } else {
            policy->setText(table(row(QStringLiteral("Measurement basis"),declaration(owner,"appraisal_policy","measurement_basis"))+
                row(QStringLiteral("Declared policy"),declaration(owner,"appraisal_policy","policy_kind"))+
                row(QStringLiteral("Policy version"),declaration(owner,"appraisal_policy","version")))+
                QStringLiteral("<p>Use Appraisal setup to enable and validate the appraisal policy.</p>"));
            standards->setText(QStringLiteral("ANSI review not verified. A measurement standard has not been verified for this property."));
        }
        // GLA always states its current state: the figure, or why it is
        // withheld and what to do. Missing facts never produce a total.
        rounding->setVisible(report->configured);
        if(report->configured)rounding->setText(QStringLiteral("Rounding: ")+appraisal_rounding_basis(*report,metric));
        if(!report->configured)status->setText(QStringLiteral("GLA withheld: Appraisal is not set up for this property, so no area has a declared basis for GLA. "
            "Use Appraisal setup to choose the policy and measurement basis, then declare each area's facts with Edit facts."));
        else if(!report->qualified || !report->calculation)status->setText(report->issues.empty() ?
            QStringLiteral("GLA withheld: the current measurements cannot be totaled. Individual measurements are diagnostics.") :
            QStringLiteral("GLA withheld: %1 %2 below must be resolved first. Select an issue to open its area. Individual measurements are diagnostics.")
                .arg(report->issues.size()).arg(report->issues.size()==1 ? QStringLiteral("issue") : QStringLiteral("issues")));
        else {
            status->setText(ansi()?QStringLiteral("ANSI Z765-2021 profile · rules v%1. Final standards validation pending.").arg(report->policy->version):
                QStringLiteral("Qualified under the declared property policy."));
            if(ansi() && std::any_of(report->boundaries.begin(),report->boundaries.end(),[](const auto& boundary) {
                return !boundary.exclusion && boundary.facts && boundary.facts->ansi &&
                    boundary.facts->ansi->ceiling.kind==CeilingKind::sloped;
            }))status->setText(status->text()+(report->policy->version==1 ?
                QStringLiteral(" Legacy V1 sloped-room totals are provisional. Appraisal setup offers the finished-room V2 rule.") :
                QStringLiteral(" V2 sloped rooms use countable finished area. Final ANSI validation remains pending.")));
            const auto& aggregate=report->calculation->property;
            set_gla(gla,area(aggregate.gla().total.square_metres),true);
            double all=0;QString residential,nonstandard,other;
            for(const auto& [kind,bucket]:aggregate.by_category) {
                all+=bucket.total.square_metres;
                if(kind==AppraisalAreaCategory::above_grade_finished || bucket.total.square_metres==0)continue;
                const auto line=row(appraisal_category_label(kind),area(bucket.total.square_metres));
                if(kind==AppraisalAreaCategory::below_grade_finished || kind==AppraisalAreaCategory::above_grade_unfinished || kind==AppraisalAreaCategory::below_grade_unfinished)residential+=line;
                else if(kind==AppraisalAreaCategory::above_grade_nonstandard_finished || kind==AppraisalAreaCategory::below_grade_nonstandard_finished || kind==AppraisalAreaCategory::noncontinuous_finished)nonstandard+=line;
                else other+=line;
            }
            QString html;
            if(!residential.isEmpty())html+=QStringLiteral("<p><b>Other floor areas</b></p>")+table(residential);
            if(!nonstandard.isEmpty())html+=QStringLiteral("<p><b>Nonstandard / noncontinuous finished</b></p>")+table(nonstandard);
            if(!other.isEmpty())html+=(ansi()?QStringLiteral("<p><b>ADU, detached areas and other uses</b></p>"):QStringLiteral("<p><b>Garage and other uses</b></p>"))+table(other);
            html+=table(row(QStringLiteral("All measured categories"),area(all)))+
                QStringLiteral("<p>This all-categories total includes garages and other uses; it is not a living-area total.</p>");
            if(ansi()) {
                html+=QStringLiteral("<p>ADU and detached-other identities remain separate from primary dwelling GLA.</p>");
                if(metric) {
                    auto diagnostic=builtin_appraisal_profile();diagnostic.display_unit=AreaUnit::square_metre;
                    html+=QStringLiteral("<p><b>Supplemental metric diagnostic</b><br>Primary dwelling GLA: %1</p>")
                        .arg(units::area_text(display_area(aggregate.gla().total.square_metres,diagnostic)));
                }
            }
            totals->setText(html+appraisal_form_projection_html(*report,true));
        }
        if(!report->qualified && report->reporting)totals->setText(appraisal_form_projection_html(*report,true));
        issue_items.clear();
        if(!report->issues.empty()) {
            // Name each source for people and link to the action that fixes it.
            using Fix=AppraisalIssuePresentation::Fix;
            QString html=QStringLiteral("<b>Issues to resolve</b><ul>");
            for(const auto& issue:report->issues) {
                const auto index=issue_items.size();
                issue_items.push_back(appraisal_issue_presentation(*source,issue));
                const auto& value=issue_items.back();
                const bool listed=!value.source_id.empty() && std::any_of(report->boundaries.begin(),report->boundaries.end(),
                    [&](const auto& boundary){return boundary.boundary_id==value.source_id;});
                QString item;
                if(listed)item=QStringLiteral("<a href='issue:%1:select'>%2</a>: ").arg(index).arg(value.subject.toHtmlEscaped());
                else if(!value.subject.isEmpty())item=QStringLiteral("<b>%1</b>: ").arg(value.subject.toHtmlEscaped());
                item+=value.text.toHtmlEscaped();
                if(value.fix==Fix::facts && listed)item+=QStringLiteral(" <a href='issue:%1:facts'>Edit facts</a>").arg(index);
                else if(value.fix==Fix::setup)item+=QStringLiteral(" <a href='issue:%1:setup'>Open Appraisal setup</a>").arg(index);
                else if(value.fix==Fix::reporting && reporting_requested)item+=QStringLiteral(" <a href='issue:%1:reporting'>Rooms and units</a>").arg(index);
                else if(value.fix==Fix::locate && listed)item+=QStringLiteral(" <a href='issue:%1:select'>Show on canvas</a>").arg(index);
                html+=QStringLiteral("<li>%1</li>").arg(item);
            }
            issues->setText(html+QStringLiteral("</ul>"));
        }
        // Design-system AreaTable: one row per area (name, type with its
        // area-* legend chip, area), grouped by floor, then computed GLA and
        // non-GLA totals. The chip is never the only cue: the type is in words.
        const bool light_surface=areas->palette().color(QPalette::Base).lightness()>=128;
        QFont mono=areas->font();mono.setFamilies({QStringLiteral("IBM Plex Mono"),QStringLiteral("Cascadia Mono"),QStringLiteral("Consolas")});
        mono.setStyleHint(QFont::Monospace);
        const auto figure=[&](QTreeWidgetItem* item,const QString& value) {
            item->setText(2,value);item->setFont(2,mono);item->setTextAlignment(2,Qt::AlignRight|Qt::AlignVCenter);
        };
        const bool several_buildings=[&] {
            std::set<std::string> ids;
            for(const auto& boundary:report->boundaries)if(boundary.measurement)ids.insert(boundary.measurement->building_id);
            return ids.size()>1;
        }();
        std::map<std::pair<std::string,std::string>,QTreeWidgetItem*> floors;
        QTreeWidgetItem* selected_item=nullptr;
        for(const auto& boundary:report->boundaries) {
            const auto* area_entity=entity(boundary.boundary_id);
            const auto floor_id=area_entity?field(*area_entity,"floor_id"):std::string{};
            const auto* floor_entity=entity(floor_id);
            const auto building_id=boundary.measurement?boundary.measurement->building_id:
                floor_entity?field(*floor_entity,"building_id"):area_entity?field(*area_entity,"building_id"):std::string{};
            auto& floor=floors[{building_id,floor_id}];
            if(!floor) {
                floor=new QTreeWidgetItem(areas);
                auto floor_name=floor_id.empty()?QStringLiteral("Unassigned floor"):name(*source,floor_id);
                if(several_buildings)floor_name=(building_id.empty()?QStringLiteral("Unassigned building"):name(*source,building_id))+
                    QStringLiteral(" › ")+floor_name;
                floor->setText(0,floor_name);
                auto font=areas->font();font.setBold(true);floor->setFont(0,font);
                floor->setFlags(Qt::ItemIsEnabled);
                if(report->calculation) {
                    const auto found=report->calculation->by_floor.find({building_id,floor_id});
                    if(found!=report->calculation->by_floor.end()) {
                        floor->setText(1,QStringLiteral("GLA"));figure(floor,area(found->second.gla().total.square_metres));
                        floor->setToolTip(2,QStringLiteral("Floor GLA"));
                    }
                }
            }
            auto* item=new QTreeWidgetItem(floor);item->setData(0,Qt::UserRole,text(boundary.boundary_id));
            // Rows sit under their floor, so drop the repeated " · <floor>".
            auto area_name=name(*source,boundary.boundary_id);
            if(!floor_id.empty()) {
                const auto suffix=QStringLiteral(" · ")+name(*source,floor_id);
                if(area_name.endsWith(suffix) && area_name.size()>suffix.size())area_name.chop(suffix.size());
            }
            item->setText(0,area_name);item->setToolTip(0,name(*source,boundary.boundary_id));
            const auto type=category(boundary);
            item->setText(1,type);item->setIcon(1,area_chip(boundary,light_surface));
            item->setToolTip(1,type);
            if(boundary.measurement) {
                const auto& value=*boundary.measurement;
                figure(item,area(boundary.exclusion?value.net_square_metres:value.factored_square_metres));
                item->setToolTip(2,QStringLiteral("Net %1 · Gross %2 · Less %3")
                    .arg(area(value.net_square_metres),area(value.base_square_metres),area(value.deducted_square_metres)));
            } else figure(item,QStringLiteral("Unavailable"));
            if(text(boundary.boundary_id)==previous)selected_item=item;
        }
        // Totals are computed, never typed: Total GLA counts GLA areas only;
        // every other measured category rolls into Non-GLA.
        if(!report->boundaries.empty()) {
            const bool totaled=report->qualified && report->calculation;
            double all=0;
            if(totaled)for(const auto& [kind,bucket]:report->calculation->property.by_category)all+=bucket.total.square_metres;
            const double gla_total=totaled?report->calculation->property.gla().total.square_metres:0;
            for(const auto& [title,value]:{std::pair{QStringLiteral("Total GLA"),gla_total},std::pair{QStringLiteral("Non-GLA"),all-gla_total}}) {
                auto* total=new QTreeWidgetItem(areas);total->setText(0,title);total->setFlags(Qt::ItemIsEnabled);
                auto font=areas->font();font.setBold(true);total->setFont(0,font);
                figure(total,totaled?area(value):QStringLiteral("Withheld"));
                if(!totaled)total->setToolTip(2,QStringLiteral("Totals are withheld until the issues above are resolved."));
            }
        }
        areas->expandAll();areas->setCurrentItem(selected_item);
        fit_areas();show_trace();
    }
};

AppraisalDetailsPanel::AppraisalDetailsPanel(QWidget* parent):QWidget(parent),impl_(std::make_unique<Impl>()) {
    setObjectName(QStringLiteral("appraisalDetailsPanel"));setMinimumWidth(0);
    ensure_bundled_fonts();
    QFont font(bundled_sans_family(),10);font.setFeature("calt",0);setFont(font);
    auto* outer=new QVBoxLayout(this);outer->setContentsMargins(0,0,0,0);
    auto* scroll=new QScrollArea(this);scroll->setObjectName(QStringLiteral("appraisalDetailsScroll"));
    scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content=new QWidget(scroll);content->setObjectName(QStringLiteral("appraisalDetailsContent"));content->setMinimumWidth(0);
    scroll->viewport()->setObjectName(QStringLiteral("appraisalDetailsViewport"));
    auto* layout=new QVBoxLayout(content);layout->setContentsMargins(12,12,12,12);layout->setSpacing(8);
    auto& p=*impl_;p.property_name=label(content,"appraisalDetailsProperty");layout->addWidget(p.property_name);
    p.property_name->setTextFormat(Qt::PlainText);
    heading(layout,QStringLiteral("Above-grade finished area (GLA)"));p.gla=label(content,"appraisalDetailsGla",QStringLiteral("Totals unavailable"));
    layout->addWidget(p.gla);
    p.gla->setTextFormat(Qt::PlainText);set_gla(p.gla,p.gla->text(),false);
    // Labels the rounding basis the displayed GLA and every area follow.
    p.rounding=label(content,"appraisalDetailsRounding");layout->addWidget(p.rounding);
    p.rounding->setTextFormat(Qt::PlainText);p.rounding->hide();
    p.status=label(content,"appraisalDetailsStatus");layout->addWidget(p.status);
    p.status->setTextFormat(Qt::PlainText);
    auto* actions=new QHBoxLayout;actions->setSpacing(6);
    p.setup=new QPushButton(QStringLiteral("Appraisal setup"),content);p.setup->setObjectName(QStringLiteral("appraisalDetailsSetup"));actions->addWidget(p.setup);
    p.full_report=new QPushButton(QStringLiteral("Open report"),content);p.full_report->setObjectName(QStringLiteral("appraisalDetailsReport"));actions->addWidget(p.full_report);layout->addLayout(actions);
    p.reporting_settings=new QPushButton(QStringLiteral("Rooms and units…"),content);p.reporting_settings->setObjectName(QStringLiteral("appraisalDetailsReporting"));layout->addWidget(p.reporting_settings);
    // Withheld-GLA issues sit beside the status that refers to them; each
    // issue links to its area and to the action that resolves it.
    p.issues=label(content,"appraisalDetailsIssues");layout->addWidget(p.issues);
    p.issues->setTextFormat(Qt::RichText);p.issues->setOpenExternalLinks(false);
    p.issues->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    connect(p.issues,&QLabel::linkActivated,this,[this](const QString& link){impl_->issue_activated(link);});
    heading(layout,QStringLiteral("Areas"));p.areas=new QTreeWidget(content);p.areas->setObjectName(QStringLiteral("appraisalDetailsAreas"));
    p.areas->setAccessibleName(QStringLiteral("Areas by floor with GLA and non-GLA totals"));
    p.areas->setColumnCount(3);p.areas->setHeaderLabels({QStringLiteral("Name"),QStringLiteral("Type"),QStringLiteral("Area")});
    p.areas->headerItem()->setTextAlignment(2,Qt::AlignRight|Qt::AlignVCenter);
    p.areas->setMinimumWidth(0);p.areas->setMinimumHeight(72);p.areas->setMaximumHeight(320);
    p.areas->setTextElideMode(Qt::ElideNone);p.areas->setUniformRowHeights(false);
    p.areas->setSelectionBehavior(QAbstractItemView::SelectRows);p.areas->setAllColumnsShowFocus(true);
    // One continuous highlight per row, not a rounded pill per cell.
    p.areas->setStyleSheet(QStringLiteral("QTreeWidget::item { border-radius: 0px; margin: 0px; } QTreeWidget::item:focus { border: 0px; }"));
    p.areas->setIndentation(10);p.areas->setWordWrap(true);p.areas->setRootIsDecorated(false);
    p.areas->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);p.areas->setSelectionMode(QAbstractItemView::SingleSelection);
    p.areas->header()->setMinimumSectionSize(45);p.areas->header()->setStretchLastSection(false);
    p.areas->header()->setSectionResizeMode(0,QHeaderView::Stretch);
    p.areas->header()->setSectionResizeMode(1,QHeaderView::Stretch);
    p.areas->header()->setSectionResizeMode(2,QHeaderView::ResizeToContents);layout->addWidget(p.areas);
    // Category totals and the form projection follow the area table.
    heading(layout,QStringLiteral("Property totals"));p.totals=label(content,"appraisalDetailsTotals");layout->addWidget(p.totals);
    // The calculation basis (policy, profile and standard) is reference
    // material: collapsed by default below the areas.
    auto* basis_toggle=new QToolButton(content);basis_toggle->setObjectName(QStringLiteral("appraisalDetailsBasisToggle"));
    basis_toggle->setText(QStringLiteral("Calculation basis"));basis_toggle->setCheckable(true);basis_toggle->setAutoRaise(true);
    basis_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);basis_toggle->setArrowType(Qt::RightArrow);
    basis_toggle->setToolTip(QStringLiteral("Measurement basis, declared policy, calculation profile and standard"));
    layout->addWidget(basis_toggle);
    auto* basis=new QWidget(content);basis->setObjectName(QStringLiteral("appraisalDetailsBasis"));basis->setMinimumWidth(0);
    auto* basis_layout=new QVBoxLayout(basis);basis_layout->setContentsMargins(0,0,0,0);basis_layout->setSpacing(8);
    p.policy=label(basis,"appraisalDetailsPolicy");basis_layout->addWidget(p.policy);
    p.standards=label(basis,"appraisalDetailsStandards");basis_layout->addWidget(p.standards);
    p.standards->setTextFormat(Qt::PlainText);basis->setVisible(false);layout->addWidget(basis);
    connect(basis_toggle,&QToolButton::toggled,this,[basis,basis_toggle](bool expanded){
        basis->setVisible(expanded);basis_toggle->setArrowType(expanded?Qt::DownArrow:Qt::RightArrow);
    });
    auto* source_actions=new QVBoxLayout;source_actions->setSpacing(6);
    p.locate=new QPushButton(QStringLiteral("Show on canvas"),content);p.locate->setObjectName(QStringLiteral("appraisalDetailsLocate"));source_actions->addWidget(p.locate);
    p.facts=new QPushButton(QStringLiteral("Edit facts"),content);p.facts->setObjectName(QStringLiteral("appraisalDetailsEditFacts"));source_actions->addWidget(p.facts);layout->addLayout(source_actions);
    p.review_sources=new QPushButton(QStringLiteral("Review measured sources…"),content);
    p.review_sources->setObjectName(QStringLiteral("appraisalDetailsReviewSources"));layout->addWidget(p.review_sources);
    heading(layout,QStringLiteral("Area dimensions and trace"));p.trace=label(content,"appraisalDetailsTrace");layout->addWidget(p.trace);
    p.provenance_toggle=new QToolButton(content);p.provenance_toggle->setObjectName(QStringLiteral("appraisalDetailsProvenanceToggle"));
    p.provenance_toggle->setText(QStringLiteral("Advanced"));p.provenance_toggle->setAutoRaise(true);
    p.provenance_toggle->setToolTip(QStringLiteral("Source record identifiers and revision, for audit"));p.provenance_toggle->setCheckable(true);
    p.provenance_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);p.provenance_toggle->setArrowType(Qt::RightArrow);
    layout->addWidget(p.provenance_toggle);
    p.provenance=new QPlainTextEdit(content);p.provenance->setObjectName(QStringLiteral("appraisalDetailsProvenance"));
    p.provenance->setReadOnly(true);p.provenance->setMinimumWidth(0);p.provenance->setMinimumHeight(140);p.provenance->setMaximumHeight(220);
    auto wrap=p.provenance->document()->defaultTextOption();wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    p.provenance->document()->setDefaultTextOption(wrap);p.provenance->setVisible(false);layout->addWidget(p.provenance);
    connect(p.provenance_toggle,&QToolButton::toggled,this,[this](bool expanded){
        impl_->provenance->setVisible(expanded);impl_->provenance_toggle->setArrowType(expanded?Qt::DownArrow:Qt::RightArrow);
    });
    layout->addStretch();scroll->setWidget(content);outer->addWidget(scroll);
    connect(p.areas,&QTreeWidget::currentItemChanged,this,[this]{impl_->show_trace();});
    connect(p.areas->header(),&QHeaderView::sectionResized,this,[this]{QTimer::singleShot(0,this,[this]{impl_->fit_areas();});});
    connect(p.areas,&QTreeWidget::itemActivated,this,[this]{impl_->locate_current();});
    connect(p.locate,&QPushButton::clicked,this,[this]{impl_->locate_current();});
    connect(p.facts,&QPushButton::clicked,this,[this]{const auto id=impl_->selected();if(impl_->report && !id.isEmpty() && impl_->facts_requested)impl_->facts_requested(id,impl_->report->revision);});
    connect(p.setup,&QPushButton::clicked,this,[this]{if(impl_->setup_requested && impl_->entity(impl_->property))impl_->setup_requested(text(impl_->property));});
    connect(p.full_report,&QPushButton::clicked,this,[this]{if(impl_->report && impl_->report_requested)impl_->report_requested(text(impl_->property));});
    connect(p.reporting_settings,&QPushButton::clicked,this,[this]{if(impl_->report && impl_->reporting_requested)impl_->reporting_requested(text(impl_->property),impl_->report->revision);});
    connect(p.review_sources,&QPushButton::clicked,this,[this]{const auto id=impl_->selected();if(impl_->report && !id.isEmpty() && impl_->source_review_requested)impl_->source_review_requested(id,impl_->report->revision);});
    p.show_report({});
    (new LinkColorSync(this))->sync();
}
AppraisalDetailsPanel::~AppraisalDetailsPanel()=default;
void AppraisalDetailsPanel::setDocument(const DocumentSnapshot& source,const std::string& property_id,bool metric,
    const std::set<std::string,std::less<>>* semantic_phase_ids) {
    auto& p=*impl_;
    // The report builder returns before reading the mask for an unconfigured
    // property. Configured reports require the exact effective semantic mask.
    // All reporting/declaration/geometry settings are guarded by the complete
    // immutable authoring source, never a document ID/revision or entity count.
    if(p.report && p.source && p.property==property_id && p.metric==metric &&
        source.shares_authoring_source_with(*p.source) &&
        (!p.report->configured ||
            (p.semantic_phase_ids.has_value()==(semantic_phase_ids!=nullptr) &&
                (!semantic_phase_ids || *p.semantic_phase_ids==*semantic_phase_ids)))) {
        // Saved/editability metadata can change without changing the report.
        p.source=source;return;
    }
    const auto selected=p.property==property_id?p.selected():QString{};
    p.source=source;p.property=property_id;p.metric=metric;p.report.reset();
    p.semantic_phase_ids.reset();
    const auto found=source.entities().find(property_id);
    if(found==source.entities().end() || found->second.type!="property") {p.show_report({});return;}
    try {
        p.report=build_appraisal_document_report(source,property_id,metric?AreaUnit::square_metre:AreaUnit::square_foot,semantic_phase_ids);
        if(p.report->configured && semantic_phase_ids)p.semantic_phase_ids=*semantic_phase_ids;
        p.show_report(selected);
    }
    catch(const std::exception& failure) {p.report.reset();p.show_report({});p.status->setText(QStringLiteral("GLA withheld: the property declarations cannot be read. %1 Use Appraisal setup to correct them.").arg(appraisal_issue_message(source,failure.what())));}
}
void AppraisalDetailsPanel::setSelectedBoundary(const QString& boundary_id) {
    QTreeWidgetItem* selected=nullptr;QTreeWidgetItemIterator item(impl_->areas);
    while(*item) {if((*item)->data(0,Qt::UserRole).toString()==boundary_id && !boundary_id.isEmpty()){selected=*item;break;}++item;}
    impl_->areas->setCurrentItem(selected);if(selected)impl_->areas->scrollToItem(selected);impl_->show_trace();
}
QString AppraisalDetailsPanel::selectedBoundaryId()const{return impl_->selected();}
QString AppraisalDetailsPanel::propertyId()const{return text(impl_->property);}
const std::optional<AppraisalDocumentReport>& AppraisalDetailsPanel::report()const{return impl_->report;}
void AppraisalDetailsPanel::setLocateRequested(std::function<void(const QString&,Revision)> callback){impl_->locate_requested=std::move(callback);}
void AppraisalDetailsPanel::setSetupRequested(std::function<void(const QString&)> callback){impl_->setup_requested=std::move(callback);}
void AppraisalDetailsPanel::setFactsRequested(std::function<void(const QString&,Revision)> callback){impl_->facts_requested=std::move(callback);}
void AppraisalDetailsPanel::setReportRequested(std::function<void(const QString&)> callback){impl_->report_requested=std::move(callback);}
void AppraisalDetailsPanel::setReportingRequested(std::function<void(const QString&,Revision)> callback){impl_->reporting_requested=std::move(callback);impl_->reporting_settings->setEnabled(bool(impl_->report) && bool(impl_->reporting_requested));}
void AppraisalDetailsPanel::setSourceReviewRequested(std::function<void(const QString&,Revision)> callback){impl_->source_review_requested=std::move(callback);}

} // namespace sketch::desktop
