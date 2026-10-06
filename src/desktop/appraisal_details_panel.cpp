#include "sketch/desktop/appraisal_details_panel.hpp"
#include "sketch/desktop/appraisal_report_dialog.hpp"

#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
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
#include <algorithm>
#include <map>
#include <utility>

namespace sketch::desktop {
namespace {
QString text(const std::string& value) { return QString::fromStdString(value); }
QString words(std::string_view token) {
    auto value=QString::fromUtf8(token.data(),static_cast<qsizetype>(token.size()));
    value.replace(QLatin1Char('_'),QLatin1Char(' '));
    if(!value.isEmpty()) value[0]=value[0].toUpper();return value;
}
QString name(const DocumentSnapshot& source,const std::string& id) {
    const auto found=source.entities().find(id);
    if(found!=source.entities().end()) {
        const auto value=found->second.properties.find("name");
        if(value!=found->second.properties.end() && value->is_string() && !value->get_ref<const std::string&>().empty())
            return text(value->get<std::string>());
        if(found->second.type=="property") return QStringLiteral("Untitled property");
        if(found->second.type=="building") return QStringLiteral("Building");
        if(found->second.type=="floor") return QStringLiteral("Floor");
        return QStringLiteral("Measured area");
    }
    return text(id);
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
QString category(const AppraisalBoundaryStatus& status) {
    if(!status.qualification.qualified) return QStringLiteral("Unqualified");
    if(status.exclusion) return QStringLiteral("Deduction only");
    return status.qualification.derived_category ? words(appraisal_category_name(*status.qualification.derived_category)) : QStringLiteral("Unavailable");
}
}

struct AppraisalDetailsPanel::Impl {
    std::optional<DocumentSnapshot> source;
    std::optional<AppraisalDocumentReport> report;
    std::string property;
    bool metric{};
    QLabel *property_name{},*gla{},*status{},*standards{},*policy{},*totals{},*issues{},*trace{};
    QTreeWidget* areas{};
    QToolButton* provenance_toggle{};
    QPlainTextEdit* provenance{};
    QPushButton *setup{},*facts{},*full_report{},*locate{},*review_sources{},*reporting_settings{};
    std::function<void(const QString&,Revision)> locate_requested,facts_requested,source_review_requested;
    std::function<void(const QString&)> setup_requested,report_requested;
    std::function<void(const QString&,Revision)> reporting_requested;

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
        return text(display_area(square_metres,profile()).text)+(metric && !ansi()?QStringLiteral(" m²"):QStringLiteral(" sq ft"));
    }
    QString length(double metres) const {
        return QString::number(metric && !ansi()?metres:metres/0.3048,'f',ansi()?1:report?report->display_decimal_places:2)+
            (metric && !ansi()?QStringLiteral(" m"):QStringLiteral(" ft"));
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
    void show_trace() {
        const auto id=selected();const AppraisalBoundaryStatus* boundary=nullptr;
        if(report)for(const auto& value:report->boundaries)if(text(value.boundary_id)==id){boundary=&value;break;}
        locate->setEnabled(boundary!=nullptr);facts->setEnabled(boundary!=nullptr);
        const auto* selected_owner=boundary?entity(boundary->boundary_id):nullptr;
        const bool has_linework_sources=selected_owner && selected_owner->type=="measurement_boundary" &&
            (selected_owner->extensions.contains("measurement_linework_sources") ||
             selected_owner->extensions.contains("measurement_linework_group"));
        review_sources->setVisible(has_linework_sources);
        review_sources->setEnabled(has_linework_sources);
        if(!boundary || !source) {
            provenance->clear();provenance_toggle->setEnabled(false);
            provenance_toggle->setChecked(false);
            trace->setText(report && report->boundaries.empty() ?
                QStringLiteral("Draw a measured area, then use Edit facts to declare its finish, access, ceiling eligibility and use.") :
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
        auto facts_html=table(row(QStringLiteral("Floor grade"),declaration(entity(floor_id),"appraisal_facts","grade"))+
            row(QStringLiteral("Finish"),declaration(owner,"appraisal_facts","finish"))+
            row(QStringLiteral("Access"),declaration(owner,"appraisal_facts","access"))+
            row(QStringLiteral("Ceiling eligibility"),declaration(owner,"appraisal_facts","ceiling_eligibility"))+
            row(QStringLiteral("Use"),declaration(owner,"appraisal_facts","area_use"))+
            row(QStringLiteral("Boundary role"),declaration(owner,"appraisal_facts","boundary_role")));
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
                row(QStringLiteral("Unrounded adjusted area"),QString::number(display.unrounded,'g',17)+(metric && !ansi()?QStringLiteral(" m²"):QStringLiteral(" sq ft")))+
                row(QStringLiteral("Rounding change"),QString::number(display.rounding_delta,'g',12)+(metric && !ansi()?QStringLiteral(" m²"):QStringLiteral(" sq ft"))));
        } else html+=QStringLiteral("<p><b>Current measurement unavailable.</b> Resolve the source geometry or dependency issues before using numeric dimensions.</p>");
        html+=QStringLiteral("<p><b>Area declarations</b></p>")+facts_html;
        if(!boundary->qualification.issues.empty()) {
            html+=QStringLiteral("<p><b>Issues to resolve with Setup or Edit facts</b></p><ul>");
            for(const auto& issue:boundary->qualification.issues)html+=QStringLiteral("<li>%1</li>").arg(text(issue.message).toHtmlEscaped());
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
        html+=QStringLiteral("<p>Document revision: %1</p>").arg(report->revision);
        trace->setText(html);
    }
    void show_report(const QString& previous) {
        const QSignalBlocker blocker(areas);areas->clear();
        const bool available=report && source;
        setup->setEnabled(entity(property)!=nullptr);full_report->setEnabled(available);
        reporting_settings->setEnabled(available && bool(reporting_requested));
        property_name->setText(source && entity(property)?name(*source,property):QStringLiteral("Property details"));
        totals->clear();issues->clear();policy->clear();standards->clear();
        gla->setText(QStringLiteral("Totals unavailable"));
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
                QStringLiteral("<p>Use Setup to enable and validate the appraisal policy.</p>"));
            standards->setText(QStringLiteral("ANSI review not verified. A measurement standard has not been verified for this property."));
        }
        if(!report->configured)status->setText(QStringLiteral("Use Setup to enable Appraisal and declare the property, floor and area facts. Automatic totals will then appear here."));
        else if(!report->qualified || !report->calculation)status->setText(QStringLiteral("Totals withheld. Resolve the issues below with Setup or Edit facts. Individual measurements are diagnostics."));
        else {
            status->setText(ansi()?QStringLiteral("ANSI Z765-2021 profile · rules v%1. Whole sq ft; dimensions: 0.1 ft. Final standards validation pending.").arg(report->policy->version):
                QStringLiteral("Qualified under the declared property policy. Totals sum unrounded contributions and round once."));
            if(ansi() && std::any_of(report->boundaries.begin(),report->boundaries.end(),[](const auto& boundary) {
                return !boundary.exclusion && boundary.facts && boundary.facts->ansi &&
                    boundary.facts->ansi->ceiling.kind==CeilingKind::sloped;
            }))status->setText(status->text()+(report->policy->version==1 ?
                QStringLiteral(" Legacy V1 sloped-room totals are provisional. Setup offers the finished-room V2 rule.") :
                QStringLiteral(" V2 sloped rooms use countable finished area. Final ANSI validation remains pending.")));
            const auto& aggregate=report->calculation->property;
            gla->setText(area(aggregate.gla().total.square_metres));
            double all=0;QString residential,nonstandard,other;
            for(const auto& [kind,bucket]:aggregate.by_category) {
                all+=bucket.total.square_metres;
                if(kind==AppraisalAreaCategory::above_grade_finished || bucket.total.square_metres==0)continue;
                const auto line=row(words(appraisal_category_name(kind)),area(bucket.total.square_metres));
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
                    html+=QStringLiteral("<p><b>Supplemental metric diagnostic</b><br>Primary dwelling GLA: %1 m²</p>")
                        .arg(text(display_area(aggregate.gla().total.square_metres,diagnostic).text));
                }
            }
            totals->setText(html+appraisal_form_projection_html(*report,true));
        }
        if(!report->qualified && report->reporting)totals->setText(appraisal_form_projection_html(*report,true));
        if(!report->issues.empty()) {
            QString html=QStringLiteral("<b>Issues to resolve</b><ul>");
            for(const auto& issue:report->issues)html+=QStringLiteral("<li>%1</li>").arg(text(issue).toHtmlEscaped());
            issues->setText(html+QStringLiteral("</ul>"));
        }
        std::map<std::string,QTreeWidgetItem*> buildings;
        std::map<std::pair<std::string,std::string>,QTreeWidgetItem*> floors;
        QTreeWidgetItem* selected_item=nullptr;
        for(const auto& boundary:report->boundaries) {
            const auto* area_entity=entity(boundary.boundary_id);
            const auto floor_id=area_entity?field(*area_entity,"floor_id"):std::string{};
            const auto* floor_entity=entity(floor_id);
            const auto building_id=boundary.measurement?boundary.measurement->building_id:
                floor_entity?field(*floor_entity,"building_id"):area_entity?field(*area_entity,"building_id"):std::string{};
            auto& building=buildings[building_id];
            if(!building) {building=new QTreeWidgetItem(areas);building->setText(0,building_id.empty()?QStringLiteral("Unassigned building"):name(*source,building_id));
                auto font=areas->font();font.setBold(true);building->setFont(0,font);}
            auto& floor=floors[{building_id,floor_id}];
            if(!floor) {floor=new QTreeWidgetItem(building);floor->setText(0,floor_id.empty()?QStringLiteral("Unassigned floor"):name(*source,floor_id));
                if(report->calculation) {
                    const auto found=report->calculation->by_floor.find({building_id,floor_id});
                    if(found!=report->calculation->by_floor.end())floor->setText(1,QStringLiteral("GLA %1").arg(area(found->second.gla().total.square_metres)));
                }}
            auto* item=new QTreeWidgetItem(floor);item->setData(0,Qt::UserRole,text(boundary.boundary_id));
            item->setText(0,name(*source,boundary.boundary_id)+QLatin1Char('\n')+category(boundary));item->setToolTip(0,text(boundary.boundary_id));
            if(boundary.measurement)item->setText(1,QStringLiteral("Net %1\nGross %2\nLess %3").arg(area(boundary.measurement->net_square_metres),area(boundary.measurement->base_square_metres),area(boundary.measurement->deducted_square_metres)));
            else item->setText(1,QStringLiteral("Unavailable"));
            if(text(boundary.boundary_id)==previous)selected_item=item;
        }
        areas->expandAll();areas->setCurrentItem(selected_item);show_trace();
    }
};

AppraisalDetailsPanel::AppraisalDetailsPanel(QWidget* parent):QWidget(parent),impl_(std::make_unique<Impl>()) {
    setObjectName(QStringLiteral("appraisalDetailsPanel"));setMinimumWidth(0);
    QFont font(QStringLiteral("Inter"),10);font.setFeature("calt",0);setFont(font);
    auto* outer=new QVBoxLayout(this);outer->setContentsMargins(0,0,0,0);
    auto* scroll=new QScrollArea(this);scroll->setObjectName(QStringLiteral("appraisalDetailsScroll"));
    scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content=new QWidget(scroll);content->setObjectName(QStringLiteral("appraisalDetailsContent"));content->setMinimumWidth(0);
    scroll->viewport()->setObjectName(QStringLiteral("appraisalDetailsViewport"));
    auto* layout=new QVBoxLayout(content);layout->setContentsMargins(12,12,12,12);layout->setSpacing(8);
    auto& p=*impl_;p.property_name=label(content,"appraisalDetailsProperty");layout->addWidget(p.property_name);
    p.property_name->setTextFormat(Qt::PlainText);
    heading(layout,QStringLiteral("Above-grade finished area (GLA)"));p.gla=label(content,"appraisalDetailsGla",QStringLiteral("Totals unavailable"));
    auto total_font=font;total_font.setPointSize(23);total_font.setBold(true);p.gla->setFont(total_font);layout->addWidget(p.gla);
    p.gla->setTextFormat(Qt::PlainText);
    p.status=label(content,"appraisalDetailsStatus");layout->addWidget(p.status);
    p.status->setTextFormat(Qt::PlainText);
    auto* actions=new QHBoxLayout;actions->setSpacing(6);
    p.setup=new QPushButton(QStringLiteral("Setup"),content);p.setup->setObjectName(QStringLiteral("appraisalDetailsSetup"));actions->addWidget(p.setup);
    p.full_report=new QPushButton(QStringLiteral("Full report"),content);p.full_report->setObjectName(QStringLiteral("appraisalDetailsReport"));actions->addWidget(p.full_report);layout->addLayout(actions);
    p.reporting_settings=new QPushButton(QStringLiteral("Reporting and rooms…"),content);p.reporting_settings->setObjectName(QStringLiteral("appraisalDetailsReporting"));layout->addWidget(p.reporting_settings);
    heading(layout,QStringLiteral("Property totals"));p.totals=label(content,"appraisalDetailsTotals");layout->addWidget(p.totals);
    p.issues=label(content,"appraisalDetailsIssues");layout->addWidget(p.issues);
    heading(layout,QStringLiteral("Buildings, floors and areas"));p.areas=new QTreeWidget(content);p.areas->setObjectName(QStringLiteral("appraisalDetailsAreas"));
    p.areas->setHeaderLabels({QStringLiteral("Area / qualification"),QStringLiteral("Measurement")});
    p.areas->setMinimumWidth(0);p.areas->setMinimumHeight(180);p.areas->setMaximumHeight(300);
    p.areas->setColumnCount(2);p.areas->setIndentation(10);p.areas->setWordWrap(true);
    p.areas->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);p.areas->setSelectionMode(QAbstractItemView::SingleSelection);
    p.areas->header()->setMinimumSectionSize(45);p.areas->header()->setSectionResizeMode(QHeaderView::Stretch);layout->addWidget(p.areas);
    heading(layout,QStringLiteral("Measurement basis and standard"));p.policy=label(content,"appraisalDetailsPolicy");layout->addWidget(p.policy);
    p.standards=label(content,"appraisalDetailsStandards");layout->addWidget(p.standards);
    p.standards->setTextFormat(Qt::PlainText);
    auto* source_actions=new QVBoxLayout;source_actions->setSpacing(6);
    p.locate=new QPushButton(QStringLiteral("Show on canvas"),content);p.locate->setObjectName(QStringLiteral("appraisalDetailsLocate"));source_actions->addWidget(p.locate);
    p.facts=new QPushButton(QStringLiteral("Edit facts"),content);p.facts->setObjectName(QStringLiteral("appraisalDetailsEditFacts"));source_actions->addWidget(p.facts);layout->addLayout(source_actions);
    p.review_sources=new QPushButton(QStringLiteral("Review measured sources…"),content);
    p.review_sources->setObjectName(QStringLiteral("appraisalDetailsReviewSources"));layout->addWidget(p.review_sources);
    heading(layout,QStringLiteral("Area dimensions and trace"));p.trace=label(content,"appraisalDetailsTrace");layout->addWidget(p.trace);
    p.provenance_toggle=new QToolButton(content);p.provenance_toggle->setObjectName(QStringLiteral("appraisalDetailsProvenanceToggle"));
    p.provenance_toggle->setText(QStringLiteral("Source IDs and fingerprint"));p.provenance_toggle->setCheckable(true);
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
    connect(p.areas,&QTreeWidget::itemActivated,this,[this]{impl_->locate_current();});
    connect(p.locate,&QPushButton::clicked,this,[this]{impl_->locate_current();});
    connect(p.facts,&QPushButton::clicked,this,[this]{const auto id=impl_->selected();if(impl_->report && !id.isEmpty() && impl_->facts_requested)impl_->facts_requested(id,impl_->report->revision);});
    connect(p.setup,&QPushButton::clicked,this,[this]{if(impl_->setup_requested && impl_->entity(impl_->property))impl_->setup_requested(text(impl_->property));});
    connect(p.full_report,&QPushButton::clicked,this,[this]{if(impl_->report && impl_->report_requested)impl_->report_requested(text(impl_->property));});
    connect(p.reporting_settings,&QPushButton::clicked,this,[this]{if(impl_->report && impl_->reporting_requested)impl_->reporting_requested(text(impl_->property),impl_->report->revision);});
    connect(p.review_sources,&QPushButton::clicked,this,[this]{const auto id=impl_->selected();if(impl_->report && !id.isEmpty() && impl_->source_review_requested)impl_->source_review_requested(id,impl_->report->revision);});
    p.show_report({});
}
AppraisalDetailsPanel::~AppraisalDetailsPanel()=default;
void AppraisalDetailsPanel::setDocument(const DocumentSnapshot& source,const std::string& property_id,bool metric,
    const std::set<std::string,std::less<>>* semantic_phase_ids) {
    auto& p=*impl_;const auto selected=p.property==property_id?p.selected():QString{};
    p.source=source;p.property=property_id;p.metric=metric;p.report.reset();
    const auto found=source.entities().find(property_id);
    if(found==source.entities().end() || found->second.type!="property") {p.show_report({});return;}
    try {p.report=build_appraisal_document_report(source,property_id,metric?AreaUnit::square_metre:AreaUnit::square_foot,semantic_phase_ids);p.show_report(selected);}
    catch(const std::exception& failure) {p.report.reset();p.show_report({});p.status->setText(QStringLiteral("Details unavailable. Use Setup to correct the property declarations. %1").arg(text(failure.what())));}
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
