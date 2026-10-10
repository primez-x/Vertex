#include "sketch/desktop/appraisal_report_dialog.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/area_arithmetic.hpp"
#include "plan_canvas.hpp"

#include <QAbstractTextDocumentLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPageLayout>
#include <QPainter>
#include <QPdfWriter>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTreeWidget>
#include <QTableWidget>
#include <QLineEdit>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

namespace sketch::desktop {
namespace {
QString text(const std::string& value) { return QString::fromStdString(value); }
QString escaped(const std::string& value) { return text(value).toHtmlEscaped(); }
QFont report_font(int size) {
    QFont font(QStringLiteral("Inter"),size);font.setFeature("calt",0);return font;
}
QString name(const DocumentSnapshot& source, const std::string& id) {
    const auto found=source.entities().find(id);
    if (found!=source.entities().end()) {
        const auto value=found->second.properties.find("name");
        if (value!=found->second.properties.end() && value->is_string() && !value->get_ref<const std::string&>().empty())
            return text(value->get<std::string>());
    }
    return text(id);
}
QString words(std::string_view token) {
    auto result=QString::fromUtf8(token.data(),static_cast<qsizetype>(token.size()));
    result.replace(QLatin1Char('_'),QLatin1Char(' '));
    if (!result.isEmpty()) result[0]=result[0].toUpper();
    return result;
}
QString room_use_label(AppraisalRoomUse use) {
    if(use==AppraisalRoomUse::bathroom_full)return QStringLiteral("Bath — Full");
    if(use==AppraisalRoomUse::bathroom_half)return QStringLiteral("Bath — Half");
    return words(appraisal_room_use_name(use));
}
bool ansi(const AppraisalDocumentReport& report) {
    return report.policy && report.policy->kind==AppraisalPolicyKind::ansi_z765_2021;
}
CalculationProfile profile(const AppraisalDocumentReport& report,bool metric) {
    if(ansi(report))return ansi_appraisal_profile(report.policy->version);
    auto value=builtin_appraisal_profile();
    value.display_unit=metric ? AreaUnit::square_metre : AreaUnit::square_foot;
    value.decimal_places=report.display_decimal_places;
    return value;
}
QString area(double square_metres,const AppraisalDocumentReport& report,bool metric) {
    return text(display_area(square_metres,profile(report,metric)).text)+(metric && !ansi(report) ? QStringLiteral(" m²") : QStringLiteral(" sq ft"));
}
QString signed_area_contribution(double square_metres,const AppraisalDocumentReport& report,bool metric) {
    auto intermediate=profile(report,metric);
    intermediate.decimal_places=std::max(intermediate.decimal_places,2U);
    return (square_metres<0 ? QStringLiteral("-") : QString{})+
        text(display_area(std::abs(square_metres),intermediate).text)+
        (metric && !ansi(report) ? QStringLiteral(" m²") : QStringLiteral(" sq ft"));
}
QString length(double metres,const AppraisalDocumentReport& report,bool metric) {
    if (!ansi(report)) return PlanCanvas::drawingLengthText(metres, metric);
    return QString::number(metres/0.3048,'f',1)+QStringLiteral(" ft");
}
QString boolean(const std::optional<bool>& value) {
    return value?(*value?QStringLiteral("Yes"):QStringLiteral("No")):QStringLiteral("Undeclared");
}
QString factor(const ExactRational& value) {
    return QStringLiteral("%1/%2").arg(value.numerator).arg(value.denominator);
}
QString category(const AppraisalBoundaryStatus& status) {
    if (!status.qualification.qualified) return QStringLiteral("Unqualified");
    if (status.exclusion) return QStringLiteral("Deduction only");
    if (status.qualification.derived_category) return words(appraisal_category_name(*status.qualification.derived_category));
    return QStringLiteral("Unavailable");
}
QString row(const QString& label,const QString& value) {
    return QStringLiteral("<tr><td>%1</td><td align='right'>%2</td></tr>").arg(label.toHtmlEscaped(),value.toHtmlEscaped());
}
QString measurement_declarations(const AnsiMeasurementDeclarations& value) {
    return row(QStringLiteral("Interior inspected"),boolean(value.interior_inspected))+
        row(QStringLiteral("Direct measurement"),boolean(value.direct_measurement))+
        row(QStringLiteral("Acquisition increment"),value.acquisition_increment?words(acquisition_increment_name(*value.acquisition_increment)):QStringLiteral("Undeclared"))+
        row(QStringLiteral("Supplemental limitations statement"),value.limitations_statement.empty()?QStringLiteral("Undeclared"):text(value.limitations_statement))+
        appraisal_ansi_declaration_rows(value);
}
QString ansi_facts(const AppraisalFacts& facts,const AppraisalDocumentReport& report,bool metric) {
    if(!facts.ansi)return {};
    const auto& value=*facts.ansi;const auto& ceiling=value.ceiling;
    auto rows=row(QStringLiteral("Any part below grade"),boolean(value.any_part_below_grade))+
        row(QStringLiteral("Year-round suitable"),boolean(value.year_round_suitable))+
        row(QStringLiteral("Finish matches dwelling"),boolean(value.finish_matches_dwelling))+
        row(QStringLiteral("Dwelling identity"),value.dwelling_identity?words(dwelling_identity_name(*value.dwelling_identity)):QStringLiteral("Undeclared"))+
        row(QStringLiteral("Ceiling type"),ceiling.kind?words(ceiling_kind_name(*ceiling.kind)):QStringLiteral("Undeclared"));
    rows+=appraisal_ceiling_height_rows(value);
    if(ceiling.at_least_7ft_area_m2)rows+=row(QStringLiteral("Ceiling area at least 7 ft"),area(*ceiling.at_least_7ft_area_m2,report,metric));
    if(ceiling.room_floor_area_m2)rows+=row(QStringLiteral("Room floor area"),area(*ceiling.room_floor_area_m2,report,metric));
    if(ceiling.kind==CeilingKind::sloped) {
        QStringList ids;for(const auto& id:ceiling.below_5ft_deduction_ids)ids.push_back(text(id));
        rows+=row(QStringLiteral("Actual below-5-ft deduction sources"),ids.isEmpty()?QStringLiteral("None declared"):ids.join(QStringLiteral(", ")))+
            row(QStringLiteral("Room boundary source"),text(ceiling.room_boundary_id))+
            row(QStringLiteral("Ceiling geometry SHA-256"),text(ceiling.source_geometry_sha256));
    }
    if(ceiling.kind==CeilingKind::stairs)rows+=row(QStringLiteral("Stair from floor source"),text(ceiling.stair_from_floor_id));
    return QStringLiteral("<h3>ANSI source facts</h3><table width='100%' border='1' cellspacing='0'>%1</table>").arg(rows);
}
QString shell(const QString& content) {
    return QStringLiteral("<html><head><style>body{font-family:Inter;font-size:10pt;color:#17212b;}"
        "h1{font-size:18pt;}h2{font-size:13pt;margin-top:18px;}h3{font-size:11pt;margin-top:12px;}"
        "th{background-color:#edf1f5;}td,th{padding:5px;}p{margin-top:6px;margin-bottom:6px;}"
        "table{border-collapse:collapse;} .muted{color:#536171;}</style></head><body>%1</body></html>").arg(content);
}
void check_revision(const DocumentSnapshot& source,const AppraisalDocumentReport& report) {
    if (report.revision!=source.revision() || report.source_document_id!=source.document_id() ||
        report.source_entities_sha256!=entity_map_digest(source.entities()))
        throw std::invalid_argument("The appraisal report belongs to another project state. Refresh it.");
    const auto owner=source.entities().find(report.property_id);
    if (owner==source.entities().end() || owner->second.type!="property")
        throw std::invalid_argument("The appraisal report's property is unavailable.");
    if (report.qualified && (!report.configured || !report.calculation || !report.issues.empty()))
        throw std::invalid_argument("The appraisal report has inconsistent qualification data.");
}
QString declaration(const DocumentSnapshot& source,const std::string& owner_id,const char* field,
    const QString& title,std::initializer_list<const char*> keys) {
    auto html=QStringLiteral("<h3>%1: %2</h3><p class='muted'>Source: %3 / %4</p><table width='100%' border='1' cellspacing='0'>")
        .arg(title.toHtmlEscaped(),name(source,owner_id).toHtmlEscaped(),escaped(owner_id),QString::fromLatin1(field).toHtmlEscaped());
    const auto owner=source.entities().find(owner_id);
    for(const auto* key:keys) {
        QString value=QStringLiteral("Undeclared");
        if(owner!=source.entities().end()) {
            const auto object=owner->second.properties.find(field);
            if(object!=owner->second.properties.end() && object->is_object()) {
                const auto entry=object->find(key);
                if(entry!=object->end()) {
                    if(entry->is_string())value=words(entry->get<std::string>());
                    else if(entry->is_number_integer())value=text(entry->dump());
                    else value=QStringLiteral("Malformed declaration");
                }
            } else if(object!=owner->second.properties.end())value=QStringLiteral("Malformed declaration");
        }
        html+=row(words(key),value);
    }
    return html+QStringLiteral("</table>");
}
QString summary(const DocumentSnapshot& source,const AppraisalDocumentReport& report,bool metric) {
    auto html=QStringLiteral("<h1>%1</h1><h2>%2</h2>").arg(ansi(report)?QStringLiteral("MEASUREMENT SUMMARY"):QStringLiteral("Appraisal area report"),name(source,report.property_id).toHtmlEscaped());
    const auto& property=source.entities().at(report.property_id);
    if (const auto subject=property.properties.find("subject");subject!=property.properties.end() && subject->is_object()) {
        const auto address=subject->find("address");
        if(address!=subject->end() && address->is_string()) html+=QStringLiteral("<p>%1</p>").arg(escaped(address->get<std::string>()));
    }
    if(ansi(report))html+=QStringLiteral("<p class='muted'>Project revision %1 · Canonical whole square feet · Dimensions to 0.1 ft</p>").arg(source.revision());
    else html+=QStringLiteral("<p class='muted'>Project revision %1 · %2 · Display precision %3 decimal places</p>")
        .arg(source.revision()).arg(metric ? QStringLiteral("Metric") : QStringLiteral("Imperial")).arg(report.display_decimal_places);
    html+=QStringLiteral("<p class='muted'>Document: %1 · Property: %2</p>").arg(escaped(source.document_id()),escaped(report.property_id));
    html+=QStringLiteral("<p class='muted'>Source entities SHA-256: %1</p>").arg(escaped(report.source_entities_sha256));
    for(const auto& [id,entity]:source.entities()) {
        (void)id;if(entity.type!="model_phases")continue;
        try {
            const auto phases=ModelPhases::from_json(entity.properties.at("model"));
            QString label=QStringLiteral("Existing baseline");
            if(phases.active_alternative()) {
                label=text(*phases.active_alternative());
                for(const auto& alternative:phases.alternatives()) if(alternative.id==*phases.active_alternative()) label=text(alternative.name);
            }
            html+=QStringLiteral("<p>Design alternative: %1</p>").arg(label.toHtmlEscaped());
        } catch(const std::exception&) {html+=QStringLiteral("<p>Design alternative unavailable; qualification is withheld.</p>");}
    }
    if (!report.configured) {
        html+=QStringLiteral("<p>Appraisal workflow is not enabled for this property. Enable Appraisal and declare the property, floor and area facts to calculate totals.</p>");
        return html;
    }
    if (!report.qualified || !report.calculation) {
        html+=QStringLiteral("<p><b>Unqualified — automatic property totals withheld.</b></p><p>Individual measurements below are diagnostics and do not constitute qualified totals.</p>");
    } else {
        html+=ansi(report)?QStringLiteral("<p>Vertex rule checks passed for the declared ANSI Z765-2021 profile. This measurement summary is not a full UAD report or ANSI approval / certification.</p>"):
            QStringLiteral("<p>Qualified under the declared Vertex policy v1. This is not ANSI/BOMA certification.</p>");
        if(ansi(report))html+=QStringLiteral("<p><b>Primary dwelling above-grade finished area (GLA): %1</b></p>").arg(area(report.calculation->property.gla().total.square_metres,report,metric).toHtmlEscaped());
        html+=QStringLiteral("<table width='100%' border='1' cellspacing='0'><tr><th align='left'>Category</th><th align='right'>Area</th></tr>");
        double total=0.0;
        for (const auto& [kind,bucket]:report.calculation->property.by_category) {
            if(bucket.total.square_metres==0.0) continue;
            html+=row(words(appraisal_category_name(kind)),area(bucket.total.square_metres,report,metric));
            total+=bucket.total.square_metres;
        }
        html+=row(QStringLiteral("All measured categories total"),area(total,report,metric))+QStringLiteral("</table>");
        html+=QStringLiteral("<p class='muted'>Finished above-grade area is a separate category. The all-categories total includes garages and other measured uses; it is not a living-area total. Totals sum unrounded contributions and round once.</p>");
        html+=QStringLiteral("<table width='100%' border='1' cellspacing='0'><tr><th align='left'>Building / floor%1</th><th align='right'>%2</th></tr>")
            .arg(ansi(report)?QStringLiteral(" / category"):QString{},ansi(report)?QStringLiteral("Area"):QStringLiteral("All categories"));
        for(const auto& [scope,totals]:report.calculation->by_floor) {
            if(ansi(report)) {
                for(const auto& [kind,bucket]:totals.by_category)if(bucket.total.square_metres!=0)
                    html+=row(name(source,scope.first)+QStringLiteral(" / ")+name(source,scope.second)+QStringLiteral(" / ")+words(appraisal_category_name(kind)),area(bucket.total.square_metres,report,metric));
            } else {
                double amount=0.0;for(const auto& [kind,bucket]:totals.by_category){(void)kind;amount+=bucket.total.square_metres;}
                html+=row(name(source,scope.first)+QStringLiteral(" / ")+name(source,scope.second),area(amount,report,metric));
            }
        }
        html+=QStringLiteral("</table>");
        if(ansi(report) && metric) {
            auto diagnostic=builtin_appraisal_profile();diagnostic.display_unit=AreaUnit::square_metre;
            html+=QStringLiteral("<p class='muted'>Supplemental metric diagnostic — primary dwelling GLA: %1 m². Canonical report remains in square feet.</p>")
                .arg(text(display_area(report.calculation->property.gla().total.square_metres,diagnostic).text));
        }
    }
    html+=appraisal_form_projection_html(report);
    if(ansi(report)) {
        html+=QStringLiteral("<h3>ANSI profile and unresolved normative validation</h3><p>Profile: %1 v%2. ADU and detached-other identities remain separate from primary dwelling GLA.</p>")
            .arg(text(ansi_appraisal_profile(report.policy->version).id).toHtmlEscaped()).arg(report.policy->version);
        if(report.ansi_measurement)html+=QStringLiteral("<table width='100%' border='1' cellspacing='0'>%1</table>").arg(measurement_declarations(*report.ansi_measurement));
        for(const auto& limitation:report.policy_limitations)html+=QStringLiteral("<p>%1</p>").arg(escaped(limitation));
        if(!report.policy_evidence.empty()) {
            html+=QStringLiteral("<p>Public rule guidance:</p><ul>");
            for(const auto& evidence:report.policy_evidence)html+=QStringLiteral("<li><a href='%1'>%1</a></li>").arg(escaped(evidence));
            html+=QStringLiteral("</ul>");
        }
    }
    if(!report.issues.empty()) {
        html+=QStringLiteral("<h2>Issues to resolve</h2><ul>");
        for(const auto& issue:report.issues) html+=QStringLiteral("<li>%1</li>").arg(escaped(issue));
        html+=QStringLiteral("</ul>");
    }
    html+=declaration(source,report.property_id,"appraisal_policy",QStringLiteral("Property policy"),
        {"policy_kind","version","property_kind","measurement_basis"});
    return html;
}
QString boundary_details(const DocumentSnapshot& source,const AppraisalDocumentReport& report,
    const AppraisalBoundaryStatus& status,bool metric) {
    auto html=QStringLiteral("<h2>%1</h2><p>%2 · Source: %3</p>")
        .arg(name(source,status.boundary_id).toHtmlEscaped(),category(status).toHtmlEscaped(),escaped(status.boundary_id));
    const auto entity=source.entities().find(status.boundary_id);
    if(entity!=source.entities().end()) {
        const auto floor=entity->second.properties.find("floor_id");
        if(floor!=entity->second.properties.end() && floor->is_string())
            html+=declaration(source,floor->get<std::string>(),"appraisal_facts",QStringLiteral("Floor grade"),{"grade"});
        const auto facts=entity->second.properties.find("appraisal_facts");
        if(facts!=entity->second.properties.end() && facts->is_object()) {
            QStringList values;
            for(const auto& [key,value]:facts->items()) if(value.is_string())
                values.push_back(words(key)+QStringLiteral(": ")+words(value.get<std::string>()));
            html+=QStringLiteral("<p>%1</p>").arg(values.join(QStringLiteral(" · ")).toHtmlEscaped());
        }
    }
    if(ansi(report) && status.facts)html+=ansi_facts(*status.facts,report,metric);
    if(status.measurement) {
        const auto& value=*status.measurement;
        html+=QStringLiteral("<p>%1 / %2</p>").arg(name(source,value.building_id).toHtmlEscaped(),name(source,value.floor_id).toHtmlEscaped());
        if(status.exclusion) html+=QStringLiteral("<p>Deduction source only; no standalone contribution to totals.</p>");
        else if(!report.qualified || !status.qualification.qualified)
            html+=QStringLiteral("<p>Diagnostic geometry only; qualified totals are withheld.</p>");
        html+=QStringLiteral("<table width='100%' border='1' cellspacing='0'>");
        html+=row(QStringLiteral("Gross boundary area"),area(value.base_square_metres,report,metric));
        if(entity!=source.entities().end())html+=appraisal_area_arithmetic_rows(entity->second,value.base_square_metres,report,metric);
        html+=row(QStringLiteral("Applied deductions (union)"),area(value.deducted_square_metres,report,metric));
        html+=row(QStringLiteral("Physical net: gross − deductions"),area(value.net_square_metres,report,metric));
        if(ansi(report))html+=appraisal_sloped_ceiling_rows(status,report);
        html+=row(QStringLiteral("Exact factor"),factor(value.factor));
        html+=row(QStringLiteral("Adjusted area: physical net × factor"),area(value.factored_square_metres,report,metric));
        const auto displayed=display_area(value.factored_square_metres,profile(report,metric));
        const auto suffix=metric && !ansi(report) ? QStringLiteral(" m²") : QStringLiteral(" sq ft");
        html+=row(QStringLiteral("Unrounded adjusted value"),QString::number(displayed.unrounded,'g',17)+suffix);
        html+=row(QStringLiteral("Display rounding change"),QString::number(displayed.rounding_delta,'g',12)+suffix);
        html+=row(QStringLiteral("Boundary perimeter"),length(value.perimeter_metres,report,metric));
        html+=QStringLiteral("</table>");
        if(ansi(report) && entity!=source.entities().end()) {
            auto geometry=entity->second.properties.find("segments");
            if(geometry==entity->second.properties.end())geometry=entity->second.properties.find("boundary");
            if(geometry!=entity->second.properties.end() && geometry->is_array()) {
                html+=QStringLiteral("<h3>Boundary dimensions</h3><p>");std::size_t index=0;
                try {for(const auto& edge:*geometry) {
                    const auto& start=edge.at("start");const auto& end=edge.at("end");
                    const Segment segment{{start.at(0).get<double>(),start.at(1).get<double>()},
                        {end.at(0).get<double>(),end.at(1).get<double>()},edge.at("sweep_radians").get<double>()};
                    html+=QStringLiteral("%1%2: %3<br>").arg(segment.sweep_radians==0?QStringLiteral("Edge "):QStringLiteral("Arc "))
                        .arg(++index).arg(length(segment_length(segment),report,metric));
                }}catch(const std::exception&) {html+=QStringLiteral("Dimensions unavailable.");}
                html+=QStringLiteral("</p>");
            }
        }
        if(!value.deductions.empty()) {
            html+=QStringLiteral("<h3>Deduction provenance</h3><table width='100%' border='1' cellspacing='0'><tr><th align='left'>Source</th><th align='right'>Requested</th><th align='right'>Applied</th></tr>");
            for(const auto& deduction:value.deductions) {
                html+=QStringLiteral("<tr><td>%1<br/>%2</td><td align='right'>%3</td><td align='right'>%4</td></tr>")
                    .arg(name(source,deduction.id).toHtmlEscaped(),escaped(deduction.id),area(deduction.requested_square_metres,report,metric).toHtmlEscaped(),area(deduction.applied_square_metres,report,metric).toHtmlEscaped());
            }
            html+=QStringLiteral("</table><p class='muted'>Applied amounts are marginal contributions in source-ID order. Overlapping deductions remove shared area once.</p>");
        }
    } else html+=QStringLiteral("<p>Current measurement unavailable. Resolve the qualification or source-geometry issue before using a numeric value.</p>");
    QStringList parents;
    for(const auto& owner:report.boundaries) if(owner.measurement)
        for(const auto& deduction:owner.measurement->deductions) if(deduction.id==status.boundary_id)
            parents.push_back(name(source,owner.boundary_id)+QStringLiteral(" [")+text(owner.boundary_id)+QLatin1Char(']'));
    if(!parents.isEmpty()) html+=QStringLiteral("<p>Deducted from: %1</p>").arg(parents.join(QStringLiteral(", ")).toHtmlEscaped());
    if(!status.qualification.issues.empty()) {
        html+=QStringLiteral("<ul>");for(const auto& issue:status.qualification.issues)
            html+=QStringLiteral("<li>%1: %2</li>").arg(escaped(issue.code),escaped(issue.message));
        html+=QStringLiteral("</ul>");
    }
    if(ansi(report) && !status.qualification.rule_notes.empty()) {
        html+=QStringLiteral("<h3>Classification reasons and rule limitations</h3><ul>");
        for(const auto& note:status.qualification.rule_notes)html+=QStringLiteral("<li>%1</li>").arg(escaped(note));
        html+=QStringLiteral("</ul>");
    }
    return html;
}
} // namespace

QString appraisal_area_arithmetic_rows(const Entity& owner,double gross_square_metres,
    const AppraisalDocumentReport& report,bool metric,bool compact) {
    auto geometry=owner.properties.find("segments");
    if(geometry==owner.properties.end())geometry=owner.properties.find("boundary");
    if(geometry==owner.properties.end() || !geometry->is_array())return {};
    try {
        Boundary boundary;
        for(const auto& edge:*geometry) {
            const auto& start=edge.at("start");const auto& end=edge.at("end");
            boundary.push_back({{start.at(0).get<double>(),start.at(1).get<double>()},
                {end.at(0).get<double>(),end.at(1).get<double>()},edge.at("sweep_radians").get<double>()});
        }
        const auto derivation=derive_area_arithmetic(boundary);
        if(std::abs(derivation.gross_square_metres-gross_square_metres)>std::max(1e-10,std::abs(gross_square_metres)*1e-10))
            return row(QStringLiteral("Gross area arithmetic"),QStringLiteral("Unavailable: source and current gross measurement differ."));
        QString result;const bool metres=metric && !ansi(report);
        const auto dimension=[&](double value){return QString::number(value/(metres?1.0:0.3048),'g',10);};
        if(derivation.method==AreaArithmeticMethod::rectangular_components) {
            QStringList terms;
            // More digits than the ordinary dimension display keep the
            // multiplication useful; rounding never changes its source values.
            for(const auto& component:derivation.rectangles)
                terms.push_back(QStringLiteral("%1 × %2 %3 = %4")
                    .arg(QString::number(component.width_metres/(metres?1.0:0.3048),'g',10),
                         QString::number(component.depth_metres/(metres?1.0:0.3048),'g',10),
                         metres?QStringLiteral("m"):QStringLiteral("ft"),signed_area_contribution(component.area_square_metres,report,metric)));
            if(terms.size()<=6 || !compact)result+=row(QStringLiteral("Rectangle components"),terms.join(QStringLiteral("; ")));
            else result+=row(QStringLiteral("Strip components"),QStringLiteral("%1 rectangles; total %2").arg(terms.size()).arg(area(gross_square_metres,report,metric)));
        } else if(derivation.method==AreaArithmeticMethod::triangle) {
            const auto size=*derivation.triangle_base_and_height_metres;
            result+=row(QStringLiteral("Triangle base × height ÷ 2"),QStringLiteral("%1 × %2 %3 ÷ 2 = %4")
                .arg(dimension(size.x),dimension(size.y),metres?QStringLiteral("m"):QStringLiteral("ft"),area(gross_square_metres,report,metric)));
        } else {
            result+=row(derivation.method==AreaArithmeticMethod::chord_and_arcs ? QStringLiteral("Signed chord contribution") : QStringLiteral("Polygon coordinate integral"),
                signed_area_contribution(derivation.chord_contribution_square_metres,report,metric));
            if(derivation.method==AreaArithmeticMethod::chord_and_arcs)
                result+=row(QStringLiteral("Analytical curve adjustment"),signed_area_contribution(derivation.curve_adjustment_square_metres,report,metric));
        }
        return result+row(QStringLiteral("Geometric gross result"),area(derivation.gross_square_metres,report,metric))+
            row(QStringLiteral("Arithmetic precision"),QStringLiteral("Stored unrounded geometry; intermediate values keep extra precision; final totals follow the selected policy."));
    }catch(const std::exception&) {return row(QStringLiteral("Gross area arithmetic"),QStringLiteral("Unavailable for this source geometry."));}
}

QString appraisal_rounded_ceiling_height_text(double observed_metres, AcquisitionIncrement increment) {
    const auto rounded=rounded_ansi_ceiling_height_metres(observed_metres,increment);
    if(increment==AcquisitionIncrement::tenth_foot)
        return QString::number(rounded/.3048,'f',1)+QStringLiteral(" ft (nearest tenth foot)");
    const auto inches=std::round(rounded/.0254);
    return QStringLiteral("%1 ft %2 in (nearest inch)")
        .arg(QString::number(std::floor(inches/12),'f',0),QString::number(std::fmod(inches,12),'f',0));
}

QString appraisal_sloped_ceiling_rows(const AppraisalBoundaryStatus& boundary,
    const AppraisalDocumentReport& report) {
    if (!report.policy || !boundary.facts || !boundary.facts->ansi || !boundary.measurement ||
        boundary.facts->ansi->ceiling.kind != CeilingKind::sloped) return {};
    const auto& ceiling=boundary.facts->ansi->ceiling;
    const auto& measurement=*boundary.measurement;
    const auto unrounded=[](double square_metres) {
        return QString::number(square_metres/0.09290304,'g',12)+QStringLiteral(" sq ft (unrounded)");
    };
    const auto denominator=report.policy->version==2 ? measurement.net_square_metres : measurement.base_square_metres;
    QString result=row(QStringLiteral("Sloped-room rule"),report.policy->version==2 ?
        QStringLiteral("V2: countable finished room after exclusions") : QStringLiteral("V1: legacy gross room, provisional"));
    result+=row(QStringLiteral("Complete physical room confirmed"),ceiling.complete_room_observed ?
        (*ceiling.complete_room_observed ? QStringLiteral("Yes") : QStringLiteral("No")) : QStringLiteral("Undeclared"));
    result+=row(QStringLiteral("Gross room footprint"),unrounded(measurement.base_square_metres));
    result+=row(QStringLiteral("Excluded area (union)"),unrounded(measurement.deducted_square_metres));
    result+=row(QStringLiteral("Ceiling threshold denominator"),unrounded(denominator));
    if(ceiling.at_least_7ft_area_m2) {
        result+=row(QStringLiteral("Observed area at least 7 ft high"),unrounded(*ceiling.at_least_7ft_area_m2));
        if(denominator>0)result+=row(QStringLiteral("Seven-foot share (50% required)"),
            QString::number(100.0*(*ceiling.at_least_7ft_area_m2/denominator),'g',12)+QStringLiteral("%"));
    }
    return result;
}

QString appraisal_ceiling_height_rows(const AnsiAppraisalFacts& facts) {
    if(!facts.ceiling.minimum_height_m)return {};
    const auto observed=*facts.ceiling.minimum_height_m;
    const auto feet=observed/.3048;
    auto rows=row(QStringLiteral("Recorded minimum ceiling height"),
        QString::number(std::isfinite(feet)?feet:observed,'g',12)+
        (std::isfinite(feet)?QStringLiteral(" ft"):QStringLiteral(" m")));
    QString rounded=QStringLiteral("Acquisition precision undeclared");
    if(facts.measurement.acquisition_increment) {
        try {rounded=appraisal_rounded_ceiling_height_text(observed,*facts.measurement.acquisition_increment);}
        catch(const std::exception&) {rounded=QStringLiteral("Unavailable: invalid ceiling measurement");}
    }
    return rows+row(QStringLiteral("Rounded minimum ceiling height"),rounded);
}

QString appraisal_schedule_area_text(const ScheduleRow& row, bool metric) {
    const auto amount=row.cells.find("area");
    if(amount==row.cells.end()) return {};
    const auto* quantity=std::get_if<ScheduleQuantity>(&amount->second.value);
    if(!quantity || quantity->unit!=ScheduleUnit::square_metre) return {};
    AppraisalDocumentReport report;report.display_decimal_places=amount->second.display_decimal_places.value_or(2);
    const auto policy=row.cells.find("policy_kind");
    if(policy!=row.cells.end() && std::holds_alternative<std::string>(policy->second.value) &&
        std::get<std::string>(policy->second.value)=="ansi_z765_2021")
        report.policy=AppraisalPolicy{AppraisalPolicyKind::ansi_z765_2021,1};
    auto displayed=area(quantity->value,report,metric);
    displayed.replace(QStringLiteral(" sq ft"),QStringLiteral(" ft²"));
    if(metric && ansi(report)) {
        auto supplemental=builtin_appraisal_profile();supplemental.display_unit=AreaUnit::square_metre;
        displayed+=QStringLiteral(" (supplemental: %1 m²)")
            .arg(text(display_area(quantity->value,supplemental).text));
    }
    return displayed;
}

void render_appraisal_summary_schedule(QPainter& painter,const QRectF& bounds,
    double pixels_per_mm,const std::vector<const ScheduleRow*>& rows,bool metric) {
    if(!(pixels_per_mm>0) || bounds.width()<=0 || bounds.height()<=0)return;
    painter.save();painter.setClipRect(bounds);painter.fillRect(bounds,Qt::white);
    const auto padding=3*pixels_per_mm;const auto header=9*pixels_per_mm;
    auto font=report_font(8);font.setPixelSize(std::max(1,static_cast<int>(std::lround(8*25.4/72*pixels_per_mm))));painter.setFont(font);
    painter.fillRect(QRectF(bounds.left(),bounds.top(),bounds.width(),header),QColor(237,241,245));
    painter.setPen(QColor(23,33,43));painter.drawText(QRectF(bounds.left()+padding,bounds.top(),bounds.width()-2*padding,header),Qt::AlignLeft|Qt::AlignVCenter,QStringLiteral("APPRAISAL AREA SUMMARY"));
    const QFontMetricsF metrics(font,painter.device());const auto width=std::max(1.0,bounds.width()-2*padding);
    auto y=bounds.top()+header;std::size_t drawn=0;
    for(const auto* row_value:rows) {
        QString content;
        const auto label=row_value->cells.find("label");if(label!=row_value->cells.end() && std::holds_alternative<std::string>(label->second.value))content=text(std::get<std::string>(label->second.value));
        const auto amount=row_value->cells.find("area");const auto status=row_value->cells.find("status");
        if(amount!=row_value->cells.end() && std::holds_alternative<ScheduleQuantity>(amount->second.value)) {
            content+=QStringLiteral("  ")+appraisal_schedule_area_text(*row_value,metric);
        } else if(status!=row_value->cells.end() && std::holds_alternative<std::string>(status->second.value))content+=QStringLiteral("  ")+text(std::get<std::string>(status->second.value));
        const auto measured=metrics.boundingRect(QRectF(0,0,width,100000),Qt::TextWordWrap|Qt::AlignLeft,content);
        const auto height=std::max(6*pixels_per_mm,measured.height()+2*pixels_per_mm);
        const auto reserve=drawn+1<rows.size() ? 9*pixels_per_mm : 0.0;
        if(y+height+reserve>bounds.bottom())break;
        painter.drawText(QRectF(bounds.left()+padding,y+pixels_per_mm,width,height-2*pixels_per_mm),Qt::TextWordWrap|Qt::AlignLeft|Qt::AlignVCenter,content);
        y+=height;painter.setPen(QPen(QColor(210,217,225),.2*pixels_per_mm));painter.drawLine(QPointF(bounds.left(),y),QPointF(bounds.right(),y));painter.setPen(QColor(23,33,43));++drawn;
    }
    if(drawn<rows.size()) {
        painter.setPen(QColor(151,94,18));painter.drawText(QRectF(bounds.left()+padding,y,width,bounds.bottom()-y),Qt::TextWordWrap|Qt::AlignLeft|Qt::AlignTop,
            QStringLiteral("%1 more rows — see the complete Appraisal area report.").arg(rows.size()-drawn));
    } else if(rows.empty())painter.drawText(QRectF(bounds.left()+padding,y,width,bounds.bottom()-y),Qt::TextWordWrap|Qt::AlignLeft|Qt::AlignTop,QStringLiteral("No configured appraisal workflow."));
    painter.setPen(QPen(QColor(115,125,138),.3*pixels_per_mm));painter.drawRect(bounds);painter.restore();
}

QString appraisal_report_html(const DocumentSnapshot& source,const AppraisalDocumentReport& report,bool metric,bool include_details) {
    check_revision(source,report);
    auto html=summary(source,report,metric);
    if(include_details) {
        html+=QStringLiteral("<h1>Boundary calculation audit</h1>");
        for(const auto& status:report.boundaries) html+=boundary_details(source,report,status,metric);
        if(report.boundaries.empty()) html+=QStringLiteral("<p>No current building boundary measurements are available.</p>");
    }
    return shell(html);
}

bool write_appraisal_report_pdf(const DocumentSnapshot& source,const AppraisalDocumentReport& report,bool metric,
    const QString& path,QString& error) {
    try {
        if(path.trimmed().isEmpty()) throw std::invalid_argument("Choose a PDF destination.");
        const auto html=appraisal_report_html(source,report,metric);
        const QFileInfo destination_info(path);
        QTemporaryFile staged(destination_info.dir().filePath(QStringLiteral(".%1.vertex-appraisal-XXXXXX").arg(destination_info.fileName())));
        if(!staged.open()) throw std::runtime_error("Could not create the local appraisal PDF staging file.");
        {
            QPdfWriter writer(&staged);writer.setResolution(144);writer.setTitle(QStringLiteral("Appraisal area report — ")+name(source,report.property_id));
            if(!writer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4),QPageLayout::Portrait,QMarginsF(12,12,12,12),QPageLayout::Millimeter)))
                throw std::runtime_error("Could not configure appraisal PDF pages.");
            QTextDocument content;content.setDefaultFont(report_font(10));
            content.setDefaultTextOption(QTextOption(Qt::AlignLeft));
            auto option=content.defaultTextOption();option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);content.setDefaultTextOption(option);
            content.setHtml(html);content.setDocumentMargin(0);
            content.documentLayout()->setPaintDevice(&writer);
            const double footer_height=36.0;
            content.setPageSize(QSizeF(writer.width(),writer.height()-footer_height));
            const auto pages=content.pageCount();
            if(pages<=0) throw std::runtime_error("The appraisal report has no renderable pages.");
            QPainter painter(&writer);if(!painter.isActive()) throw std::runtime_error("Could not start appraisal PDF rendering.");
            for(int page=0;page<pages;++page) {
                if(page>0 && !writer.newPage()) throw std::runtime_error("Could not append an appraisal PDF page.");
                painter.fillRect(QRectF(0,0,writer.width(),writer.height()),Qt::white);
                painter.save();
                painter.setClipRect(QRectF(0,0,writer.width(),writer.height()-footer_height));
                painter.translate(0,-page*content.pageSize().height());
                QAbstractTextDocumentLayout::PaintContext context;
                context.clip=QRectF(0,page*content.pageSize().height(),writer.width(),content.pageSize().height());
                content.documentLayout()->draw(&painter,context);painter.restore();
                painter.setPen(QColor(83,97,113));painter.setFont(report_font(8));
                painter.drawText(QRectF(0,writer.height()-footer_height,writer.width(),footer_height),Qt::AlignRight|Qt::AlignVCenter,
                    QStringLiteral("Project revision %1 · %2 / %3").arg(source.revision()).arg(page+1).arg(pages));
            }
            if(!painter.end()) throw std::runtime_error("Could not finish appraisal PDF rendering.");
        }
        if(!staged.flush() || staged.error()!=QFileDevice::NoError || staged.size()<=0 || !staged.seek(0))
            throw std::runtime_error("Could not complete the staged appraisal PDF.");
        const auto bytes=staged.readAll();if(bytes.isEmpty() || staged.error()!=QFileDevice::NoError)
            throw std::runtime_error("Could not read the staged appraisal PDF.");
        QSaveFile destination(path);destination.setDirectWriteFallback(false);
        if(!destination.open(QIODevice::WriteOnly) || destination.write(bytes)!=bytes.size() || !destination.commit())
            throw std::runtime_error("Could not atomically save the appraisal PDF.");
        error.clear();return true;
    } catch(const std::exception& failure) {error=QString::fromUtf8(failure.what());return false;}
}

struct AppraisalReportDialog::Impl {
    DocumentSnapshot source;
    std::vector<AppraisalDocumentReport> reports;
    bool metric;
    QComboBox* properties{};QLabel* state{};QLabel* error{};
    QTextBrowser* summary_view{};QTextBrowser* detail{};QTextBrowser* issues{};QTreeWidget* areas{};
    QPushButton* export_pdf{};QPushButton* locate{};
    std::function<void()> refresh_requested;
    std::function<void(const QString&,Revision)> export_requested,locate_requested;
    Impl(const DocumentSnapshot& value,std::vector<AppraisalDocumentReport> projections,bool units)
        :source(value),reports(std::move(projections)),metric(units) {}
    const AppraisalDocumentReport* selected() const {
        const auto id=properties->currentData().toString().toStdString();
        const auto found=std::find_if(reports.begin(),reports.end(),[&](const auto& item){return item.property_id==id;});
        return found==reports.end() ? nullptr : &*found;
    }
    void select_area() {
        const auto* report=selected();const auto* item=areas->currentItem();
        const auto id=item ? item->data(0,Qt::UserRole).toString().toStdString() : std::string{};
        locate->setEnabled(report && !id.empty());
        if(!report || id.empty()) {detail->clear();return;}
        const auto found=std::find_if(report->boundaries.begin(),report->boundaries.end(),[&](const auto& status){return status.boundary_id==id;});
        if(found!=report->boundaries.end()) detail->setHtml(shell(boundary_details(source,*report,*found,metric)));
        else detail->setHtml(shell(QStringLiteral("<h2>%1</h2><p>Source: %2</p><p>Current measurement unavailable. This deduction has no validated boundary trace in this report.</p>")
            .arg(name(source,id).toHtmlEscaped(),escaped(id))));
    }
    void show_report() {
        areas->clear();detail->clear();error->clear();error->hide();locate->setEnabled(false);
        const auto* report=selected();export_pdf->setEnabled(report!=nullptr);
        if(!report) {summary_view->clear();issues->clear();state->setText(QStringLiteral("No property available"));return;}
        state->setText(QStringLiteral("Revision %1 · %2").arg(source.revision()).arg(!report->configured ? QStringLiteral("Workflow not enabled") :
            report->qualified ? (ansi(*report)?QStringLiteral("Vertex rule checks passed"):QStringLiteral("Qualified")) : QStringLiteral("Totals withheld")));
        summary_view->setHtml(appraisal_report_html(source,*report,metric,false));
        QString issue_text=QStringLiteral("<h2>Issues</h2>");
        if(report->issues.empty()) issue_text+=QStringLiteral("<p>No qualification issues in this projection.</p>");
        else {issue_text+=QStringLiteral("<ul>");for(const auto& value:report->issues) issue_text+=QStringLiteral("<li>%1</li>").arg(escaped(value));issue_text+=QStringLiteral("</ul>");}
        issues->setHtml(shell(issue_text));
        for(const auto& status:report->boundaries) {
            auto* item=new QTreeWidgetItem(areas);item->setData(0,Qt::UserRole,text(status.boundary_id));
            item->setText(0,name(source,status.boundary_id));item->setToolTip(0,text(status.boundary_id));item->setText(1,category(status));
            if(status.measurement) {
                const auto& value=*status.measurement;
                item->setText(2,area(value.base_square_metres,*report,metric));item->setText(3,area(value.deducted_square_metres,*report,metric));
                item->setText(4,area(value.net_square_metres,*report,metric));item->setText(5,factor(value.factor));item->setText(6,area(value.factored_square_metres,*report,metric));
                for(const auto& deduction:value.deductions) {
                    auto* child=new QTreeWidgetItem(item);child->setData(0,Qt::UserRole,text(deduction.id));child->setText(0,name(source,deduction.id));
                    child->setToolTip(0,text(deduction.id));child->setText(1,QStringLiteral("Deduction: requested / applied"));
                    child->setText(2,area(deduction.requested_square_metres,*report,metric));child->setText(3,area(deduction.applied_square_metres,*report,metric));
                }
            } else for(int column=2;column<7;++column) item->setText(column,QStringLiteral("—"));
        }
        if(areas->topLevelItemCount()>0) areas->setCurrentItem(areas->topLevelItem(0));
    }
};

AppraisalReportDialog::AppraisalReportDialog(const DocumentSnapshot& source,std::vector<AppraisalDocumentReport> reports,bool metric,QWidget* parent)
    :QDialog(parent),impl_(std::make_unique<Impl>(source,std::move(reports),metric)) {
    setObjectName(QStringLiteral("appraisalReportDialog"));setWindowTitle(QStringLiteral("Appraisal area report"));resize(1100,780);
    auto& p=*impl_;auto* layout=new QVBoxLayout(this);auto* toolbar=new QHBoxLayout;
    p.properties=new QComboBox(this);p.properties->setObjectName(QStringLiteral("appraisalReportProperty"));p.properties->setMinimumWidth(220);
    toolbar->addWidget(p.properties);p.state=new QLabel(this);p.state->setObjectName(QStringLiteral("appraisalReportState"));toolbar->addWidget(p.state);toolbar->addStretch();
    auto* refresh=new QPushButton(QStringLiteral("Refresh"),this);refresh->setObjectName(QStringLiteral("refreshAppraisalReport"));toolbar->addWidget(refresh);
    p.export_pdf=new QPushButton(QStringLiteral("Export PDF…"),this);p.export_pdf->setObjectName(QStringLiteral("exportAppraisalReportPdf"));toolbar->addWidget(p.export_pdf);layout->addLayout(toolbar);
    auto* tabs=new QTabWidget(this);tabs->setObjectName(QStringLiteral("appraisalReportTabs"));
    p.summary_view=new QTextBrowser(tabs);p.summary_view->setObjectName(QStringLiteral("appraisalReportSummary"));tabs->addTab(p.summary_view,QStringLiteral("Summary"));
    auto* splitter=new QSplitter(Qt::Vertical,tabs);p.areas=new QTreeWidget(splitter);p.areas->setObjectName(QStringLiteral("appraisalReportAreas"));
    p.areas->setHeaderLabels({QStringLiteral("Area"),QStringLiteral("Category / status"),QStringLiteral("Gross"),QStringLiteral("Deductions"),QStringLiteral("Net"),QStringLiteral("Factor"),QStringLiteral("Adjusted")});
    p.areas->setAlternatingRowColors(true);p.areas->setUniformRowHeights(true);p.areas->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    p.areas->header()->setStretchLastSection(true);p.detail=new QTextBrowser(splitter);p.detail->setObjectName(QStringLiteral("appraisalReportDetail"));
    splitter->setSizes({280,350});tabs->addTab(splitter,QStringLiteral("Areas and deductions"));
    p.issues=new QTextBrowser(tabs);p.issues->setObjectName(QStringLiteral("appraisalReportIssues"));tabs->addTab(p.issues,QStringLiteral("Issues"));layout->addWidget(tabs);
    for(auto* browser:{p.summary_view,p.detail,p.issues}) {
        browser->setFont(report_font(10));
        browser->setOpenLinks(false);browser->setOpenExternalLinks(false);browser->setAcceptDrops(false);
        auto palette=browser->palette();palette.setColor(QPalette::Base,Qt::white);palette.setColor(QPalette::Text,QColor(23,33,43));browser->setPalette(palette);
    }
    p.error=new QLabel(this);p.error->setObjectName(QStringLiteral("appraisalReportError"));p.error->setWordWrap(true);layout->addWidget(p.error);
    auto* bottom=new QHBoxLayout;p.locate=new QPushButton(QStringLiteral("Show on canvas"),this);p.locate->setObjectName(QStringLiteral("locateAppraisalBoundary"));bottom->addWidget(p.locate);bottom->addStretch();
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close,this);bottom->addWidget(buttons);layout->addLayout(bottom);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(p.properties,&QComboBox::currentIndexChanged,this,[this]{impl_->show_report();});
    connect(p.areas,&QTreeWidget::currentItemChanged,this,[this]{impl_->select_area();});
    connect(refresh,&QPushButton::clicked,this,[this]{if(impl_->refresh_requested) impl_->refresh_requested();});
    connect(p.export_pdf,&QPushButton::clicked,this,[this]{if(const auto* report=impl_->selected();report && impl_->export_requested) impl_->export_requested(text(report->property_id),report->revision);});
    connect(p.locate,&QPushButton::clicked,this,[this]{const auto* report=impl_->selected();const auto* item=impl_->areas->currentItem();
        if(report && item && impl_->locate_requested) impl_->locate_requested(item->data(0,Qt::UserRole).toString(),report->revision);});
    setReports(source,std::move(impl_->reports),metric);
}
AppraisalReportDialog::~AppraisalReportDialog()=default;
void AppraisalReportDialog::setReports(const DocumentSnapshot& source,std::vector<AppraisalDocumentReport> reports,bool metric) {
    for(const auto& report:reports) check_revision(source,report);
    const auto previous=selectedPropertyId();auto& p=*impl_;p.source=source;p.reports=std::move(reports);p.metric=metric;
    {const QSignalBlocker blocker(p.properties);p.properties->clear();for(const auto& report:p.reports)
        p.properties->addItem(name(source,report.property_id),text(report.property_id));
        const auto index=p.properties->findData(previous);if(index>=0)p.properties->setCurrentIndex(index);}
    p.show_report();
}
QString AppraisalReportDialog::selectedPropertyId() const{return impl_->properties->currentData().toString();}
void AppraisalReportDialog::showError(const QString& message){impl_->error->setText(message);impl_->error->setVisible(!message.isEmpty());}
void AppraisalReportDialog::setRefreshRequested(std::function<void()> callback){impl_->refresh_requested=std::move(callback);}
void AppraisalReportDialog::setExportRequested(std::function<void(const QString&,Revision)> callback){impl_->export_requested=std::move(callback);}
void AppraisalReportDialog::setLocateRequested(std::function<void(const QString&,Revision)> callback){impl_->locate_requested=std::move(callback);}

QString appraisal_form_projection_html(const AppraisalDocumentReport& report, bool compact) {
    if (!report.reporting) return {};
    const auto& value = *report.reporting;
    if(!value.configuration_valid) {
        QString html=QStringLiteral("<h3>Reporting configuration invalid · fields withheld</h3>");
        for(const auto& issue:value.issues)html+=QStringLiteral("<p>%1</p>").arg(escaped(issue));
        return html;
    }
    const bool legacy = value.contract == AppraisalReportingContract::legacy_uad_2_6;
    QString html = QStringLiteral("<h3>%1 reporting</h3>")
        .arg(legacy ? QStringLiteral("Legacy UAD 2.6") : QStringLiteral("UAD 3.6"));
    auto rows = row(QStringLiteral("Primary above-grade finished area (GLA)"), value.area_fields_available ?
        text(display_area(value.primary_above_grade_finished_square_metres, ansi_appraisal_profile()).text)+QStringLiteral(" sq ft") : QStringLiteral("Withheld"));
    for(const auto& [category,amount]:value.primary_area_fields)if(category!=AppraisalAreaCategory::above_grade_finished)
        rows+=row(words(appraisal_category_name(category)),value.area_fields_available ? text(display_area(amount,ansi_appraisal_profile()).text)+QStringLiteral(" sq ft") : QStringLiteral("Withheld"));
    const auto counts = [&](const QString& title, const AppraisalRoomCounts& counts, bool total, bool primary) {
        if (!(primary ? value.room_counts_available : value.room_summaries_available)) return row(title, QStringLiteral("Withheld"));
        QString amount = QStringLiteral("%1 bedrooms; %2 full / %3 half bathrooms")
            .arg(counts.bedrooms).arg(counts.bathrooms_full).arg(counts.bathrooms_half);
        if (total) amount += QStringLiteral("; %1 explicitly declared total rooms").arg(counts.total_rooms);
        return row(title, amount);
    };
    rows += counts(legacy ? QStringLiteral("Primary above-grade room counts") : QStringLiteral("Primary all-grade room counts"), value.primary_counts, legacy, true);
    rows += counts(QStringLiteral("Primary below-grade room summary"), value.below_grade_counts, false, false);
    rows += counts(QStringLiteral("Separate noncontinuous room summary"), value.noncontinuous_counts, false, false);
    if(value.living_units.empty()) rows += counts(QStringLiteral("Combined ADU room summary"), value.adu_counts, false, false);
    if(compact) {
        rows.replace(QStringLiteral("<tr><td>"),QStringLiteral("<p><b>"));
        rows.replace(QStringLiteral("</td><td align='right'>"),QStringLiteral("</b><br>"));
        rows.replace(QStringLiteral("</td></tr>"),QStringLiteral("</p>"));
        html+=rows;
    } else html += QStringLiteral("<table width='100%' border='1' cellspacing='0'>%1</table>").arg(rows);
    html += legacy ? QStringLiteral("<p>Eligible contained ADU area may combine in the legacy area field. Primary room counts are withheld where legacy room mapping is unresolved; current separate summaries remain available.</p>") :
        QStringLiteral("<p>Room counts include declared bedrooms and bathrooms across grades and finish categories. Individual ADUs remain separately addressed. Noncontinuous space has separate detail.</p>");
    for(const auto& unit:value.living_units) {
        html+=QStringLiteral("<h4>%1 · %2</h4>").arg(escaped(unit.living_unit.identifier),words(dwelling_identity_name(unit.living_unit.role)));
        QString unit_rows;
        if(unit.room_counts_available) unit_rows+=row(QStringLiteral("Unit bedroom / bathroom counts"),QStringLiteral("%1 bedrooms; %2 full / %3 half bathrooms")
            .arg(unit.counts.bedrooms).arg(unit.counts.bathrooms_full).arg(unit.counts.bathrooms_half));
        else unit_rows+=row(QStringLiteral("Unit bedroom / bathroom counts"),QStringLiteral("Withheld"));
        if(unit.area_fields_available) for(const auto& [category,amount]:unit.area_fields)
            unit_rows+=row(words(appraisal_category_name(category)),text(display_area(amount,ansi_appraisal_profile()).text)+QStringLiteral(" sq ft"));
        else unit_rows+=row(QStringLiteral("Unit area fields"),QStringLiteral("Withheld"));
        html+=QStringLiteral("<table width='100%' border='1' cellspacing='0'>%1</table>").arg(unit_rows);
        for(const auto& level:unit.levels) {
            QString label=QStringLiteral("Measured level · form level withheld");
            if(level.declaration && level.form_fields_available) label=QStringLiteral("Level %1%2 · %3")
                .arg(level.declaration->grade_level_type=="above_grade"?QString{}:QStringLiteral("B"))
                .arg(level.declaration->level_number).arg(words(*level.declaration->grade_level_type));
            if(level.noncontinuous)label+=QStringLiteral(" · noncontinuous detail");
            QStringList room_types;
            for(const auto& [use,count]:level.room_types)if(use!=AppraisalRoomUse::other || level.other_room_descriptions.empty())
                room_types.push_back(QStringLiteral("%1 — %2").arg(count).arg(room_use_label(use)));
            for(const auto& [description,count]:level.other_room_descriptions)room_types.push_back(QStringLiteral("%1 — Other: %2").arg(count).arg(text(description)));
            html+=QStringLiteral("<p><b>%1</b><br>%2</p>").arg(label.toHtmlEscaped(),
                unit.room_counts_available?room_types.join(QStringLiteral("; ")).toHtmlEscaped():QStringLiteral("Room summary withheld"));
            if(level.form_fields_available && level.declaration->below_grade_access) {
                html+=QStringLiteral("<p>Below-grade access: %1%2</p>").arg(words(*level.declaration->below_grade_access).toHtmlEscaped(),
                    level.declaration->exterior_access?QStringLiteral(" · ")+words(*level.declaration->exterior_access).toHtmlEscaped()+
                        (level.declaration->exterior_access_description.empty()?QString{}:QStringLiteral(" · ")+escaped(level.declaration->exterior_access_description)):QString{});
            }
            double finished=0,unfinished=0;
            for(const auto& [category,amount]:level.area_fields) {
                if(category==AppraisalAreaCategory::above_grade_unfinished || category==AppraisalAreaCategory::below_grade_unfinished)unfinished+=amount;
                else finished+=amount;
            }
            if(unit.area_fields_available)html+=QStringLiteral("<p>Measured level finished: %1 sq ft · unfinished: %2 sq ft</p>")
                .arg(text(display_area(finished,ansi_appraisal_profile()).text),text(display_area(unfinished,ansi_appraisal_profile()).text));
            if(!compact) {
                QStringList boundaries;for(const auto& id:level.boundary_ids)boundaries.push_back(escaped(id));
                html+=QStringLiteral("<p class='muted'>Floor source: %1 · Boundary sources: %2</p>").arg(escaped(level.floor_id),boundaries.join(QStringLiteral(", ")));
            }
        }
    }
    if(!value.living_units.empty())html+=QStringLiteral("<p>Unit categories retain separate standard and nonstandard areas; measured level detail combines their finished areas. This is a local reporting projection, not MISMO delivery or full URAR certification.</p>");
    html += QStringLiteral("<p>Room types reflect explicit original-design declarations. A full bathroom has a sink, toilet and tub or shower; a half bathroom has a sink and toilet. Confirmation does not independently verify room geometry or fixtures.</p>");
    for (const auto& issue : value.issues) html += QStringLiteral("<p><b>%1</b></p>").arg(escaped(issue));
    if (!compact) {
        html += QStringLiteral("<h4>Level and declared room detail</h4><table width='100%' border='1' cellspacing='0'><tr><th>Room / source</th><th>Use / level / category</th></tr>");
        for (const auto& room : value.rooms) {
            html += row(text(room.room_id)+QStringLiteral(" / ")+text(room.boundary_id),
                room_use_label(room.use)+(room.other_description.empty()?QString{}:QStringLiteral(": ")+text(room.other_description))+QStringLiteral(" / ")+text(room.floor_id)+QStringLiteral(" / ")+words(appraisal_category_name(room.category))+
                (room.included_in_primary_counts ? QStringLiteral(" / primary counts") : QStringLiteral(" / separate summary")));
        }
        html += QStringLiteral("</table><h4>Public guidance editions reviewed</h4><ul>");
        for (const auto& source : value.evidence) html += QStringLiteral("<li><a href='%1'>%2</a>: %3</li>").arg(escaped(source.url), escaped(source.edition), escaped(source.sections));
        html += QStringLiteral("</ul>");
    }
    return html;
}

QString appraisal_ansi_declaration_rows(const AnsiMeasurementDeclarations& value) {
    QString rows;
    for(const auto& declaration:value.limitation_declarations) {
        QString condition;
        switch(declaration.kind) {
        case AnsiDeclarationKind::interior_not_inspected:condition=QStringLiteral("Interior not inspected declaration");break;
        case AnsiDeclarationKind::based_on_plans:condition=QStringLiteral("Based on plans declaration");break;
        case AnsiDeclarationKind::direct_measurement_not_possible:condition=QStringLiteral("Direct measurement not possible declaration");break;
        }
        rows+=row(condition,text(declaration.statement));
    }
    if(!value.limitation_declarations.empty())rows+=row(QStringLiteral("Declaration verification"),QStringLiteral("Presence and conditions checked; prescribed publisher wording remains unverified."));
    return rows;
}

struct AppraisalReportingDialog::Impl {
    DocumentSnapshot source;
    AppraisalDocumentReport report;
    AppraisalReportingChanges changes;
    std::optional<std::set<std::string, std::less<>>> semantic_visibility;
    QComboBox *contract{}, *area{}, *containment{};
    QCheckBox *complete{}, *confirmed{};
    QTableWidget* rooms{};
    QTableWidget* units{};
    QComboBox *unit{}, *level_grade{}, *level_access{}, *level_exterior{};
    QSpinBox* level_number{};
    QLineEdit* level_description{};
    QFormLayout* declaration_form{};
    QLabel *error{}, *source_note{};
    QPushButton* save{};
    std::map<std::string, AppraisalAreaReportingFacts> declarations;
    QString current;
    QString active_unit;
    std::function<bool(const AppraisalReportingChanges&, QString&)> apply;
    void update_fields() {
        declaration_form->setRowVisible(containment, containment->isEnabled() && contract->currentData().toInt()==static_cast<int>(AppraisalReportingContract::legacy_uad_2_6));
        rooms->setColumnHidden(2, contract->currentData().toInt() !=
            static_cast<int>(AppraisalReportingContract::legacy_uad_2_6));
        bool describe=false;
        for(int row=0;row<rooms->rowCount();++row) {
            const auto* use=static_cast<QComboBox*>(rooms->cellWidget(row,1));
            auto* description=static_cast<QLineEdit*>(rooms->cellWidget(row,3));
            const bool other=changes.settings.version==2 && use && use->currentData().toInt()==static_cast<int>(AppraisalRoomUse::other);
            if(description)description->setEnabled(other);describe=describe||other;
        }
        rooms->setColumnHidden(3,!describe);
        const bool assigned=!unit->currentData().toString().isEmpty();
        declaration_form->setRowVisible(level_number,assigned);
        declaration_form->setRowVisible(level_grade,assigned);
        const auto grade=level_grade->currentData().toString();
        const bool below=assigned && (grade==QStringLiteral("fully_below_grade") || grade==QStringLiteral("partially_below_grade"));
        declaration_form->setRowVisible(level_access,below);
        const auto access=level_access->currentData().toString();
        const bool exterior=below && (access==QStringLiteral("interior_and_exterior") || access==QStringLiteral("exterior_only"));
        declaration_form->setRowVisible(level_exterior,exterior);
        declaration_form->setRowVisible(level_description,exterior && level_exterior->currentData().toString()==QStringLiteral("other"));
    }
    Impl(const DocumentSnapshot& s, const AppraisalDocumentReport& r,
        const std::set<std::string, std::less<>>* visible) : source(s), report(r) {
        if (visible) semantic_visibility = *visible;
        changes.revision=s.revision(); changes.source_document_id=s.document_id();
        changes.source_snapshot_sha256=document_snapshot_digest(s); changes.property_id=r.property_id;
    }
    QComboBox* choice(QWidget* parent, bool use = false) {
        auto* result = new QComboBox(parent);
        if (use) {
            result->addItem(QStringLiteral("Other room"), static_cast<int>(AppraisalRoomUse::other));
            result->addItem(QStringLiteral("Bedroom"), static_cast<int>(AppraisalRoomUse::bedroom));
            result->addItem(QStringLiteral("Full bathroom (declared)"), static_cast<int>(AppraisalRoomUse::bathroom_full));
            result->addItem(QStringLiteral("Half bathroom (declared)"), static_cast<int>(AppraisalRoomUse::bathroom_half));
            for(auto value:{AppraisalRoomUse::breakfast_room,AppraisalRoomUse::den,AppraisalRoomUse::dining_room,
                AppraisalRoomUse::family_room,AppraisalRoomUse::kitchen,AppraisalRoomUse::laundry_room,
                AppraisalRoomUse::living_room,AppraisalRoomUse::loft,AppraisalRoomUse::media_room,AppraisalRoomUse::mudroom,
                AppraisalRoomUse::recreation_room,AppraisalRoomUse::sunroom,AppraisalRoomUse::utility_room,AppraisalRoomUse::walk_in_pantry,AppraisalRoomUse::workshop})
                result->addItem(room_use_label(value),static_cast<int>(value));
        } else {
            result->addItem(QStringLiteral("Unknown"), -1); result->addItem(QStringLiteral("No"), 0); result->addItem(QStringLiteral("Yes"), 1);
        }
        return result;
    }
    void add_unit(const AppraisalLivingUnit& value) {
        const QSignalBlocker blocker(units);
        const auto row=units->rowCount();units->insertRow(row);
        auto* label=new QTableWidgetItem(text(value.identifier));label->setData(Qt::UserRole,text(value.unit_id));units->setItem(row,0,label);
        auto* role=new QComboBox(units);
        for(auto identity:{DwellingIdentity::primary,DwellingIdentity::attached_adu,DwellingIdentity::detached_adu})
            role->addItem(words(dwelling_identity_name(identity)),static_cast<int>(identity));
        role->setCurrentIndex(role->findData(static_cast<int>(value.role)));units->setCellWidget(row,1,role);
    }
    void retain_units() {
        std::vector<AppraisalLivingUnit> values;
        for(int row=0;row<units->rowCount();++row) {
            const auto* label=units->item(row,0);const auto* role=static_cast<QComboBox*>(units->cellWidget(row,1));
            AppraisalLivingUnit value{label->data(Qt::UserRole).toString().toStdString(),label->text().toStdString(),static_cast<DwellingIdentity>(role->currentData().toInt())};
            const auto previous=std::find_if(changes.settings.living_units.begin(),changes.settings.living_units.end(),[&](const auto& item){return item.unit_id==value.unit_id;});
            if(previous!=changes.settings.living_units.end())value.levels=previous->levels;
            values.push_back(std::move(value));
        }
        changes.settings.living_units=std::move(values);
        if(!changes.settings.living_units.empty())changes.settings.version=2;
    }
    void refresh_units() {
        const auto selected=unit->currentData().toString();
        const QSignalBlocker blocker(unit);unit->clear();unit->addItem(QStringLiteral("Unknown / unassigned"),QString{});
        for(int row=0;row<units->rowCount();++row) {
            const auto* label=units->item(row,0);
            unit->addItem(label->text().isEmpty()?QStringLiteral("Unnamed unit %1").arg(row+1):label->text(),label->data(Qt::UserRole));
        }
        if(!selected.isEmpty() && unit->findData(selected)<0)unit->addItem(QStringLiteral("Removed unit — reassign before Save"),selected);
        unit->setCurrentIndex(std::max(0,unit->findData(selected)));update_fields();
    }
    void load_level() {
        active_unit=unit->currentData().toString();
        level_number->setValue(0);level_grade->setCurrentIndex(0);level_access->setCurrentIndex(0);level_exterior->setCurrentIndex(0);level_description->clear();
        if(current.isEmpty()) {update_fields();return;}
        const auto selected=unit->currentData().toString().toStdString();
        const auto value=std::find_if(changes.settings.living_units.begin(),changes.settings.living_units.end(),[&](const auto& item){return item.unit_id==selected;});
        if(value!=changes.settings.living_units.end()) {
            const auto floor=source.entities().at(current.toStdString()).properties.value("floor_id",std::string{});
            const auto level=std::find_if(value->levels.begin(),value->levels.end(),[&](const auto& item){return item.floor_id==floor;});
            if(level!=value->levels.end()) {
                level_number->setValue(static_cast<int>(level->level_number));
                level_grade->setCurrentIndex(level_grade->findData(level->grade_level_type?text(*level->grade_level_type):QString{}));
                level_access->setCurrentIndex(level_access->findData(level->below_grade_access?text(*level->below_grade_access):QString{}));
                level_exterior->setCurrentIndex(level_exterior->findData(level->exterior_access?text(*level->exterior_access):QString{}));
                level_description->setText(text(level->exterior_access_description));
            }
        }
        update_fields();
    }
    void add(const AppraisalRoomDeclaration& room) {
        const auto index=rooms->rowCount(); rooms->insertRow(index);
        rooms->setItem(index,0,new QTableWidgetItem(text(room.room_id)));
        auto* use=choice(rooms,true); use->setCurrentIndex(use->findData(static_cast<int>(room.use))); rooms->setCellWidget(index,1,use);
        auto* total=choice(rooms); total->setCurrentIndex(total->findData(room.legacy_total_room ? (*room.legacy_total_room ? 1 : 0) : -1)); rooms->setCellWidget(index,2,total);
        auto* description=new QLineEdit(rooms);description->setMaxLength(1024);description->setText(text(room.other_description));rooms->setCellWidget(index,3,description);
        connect(use,&QComboBox::currentIndexChanged,rooms,[this,use]{
            if(use->currentData().toInt()>static_cast<int>(AppraisalRoomUse::other)) {
                changes.settings.version=2;
                if(!current.isEmpty())declarations.at(current.toStdString()).version=2;
            }
            update_fields();});
        update_fields();
    }
    void retain() {
        if (current.isEmpty()) return;
        auto& value=declarations.at(current.toStdString()); value.rooms.clear();
        const int contained=containment->currentData().toInt();
        value.contained_within_primary=contained<0 ? std::optional<bool>{} : std::optional<bool>{contained==1};
        retain_units();
        const auto selected=unit->currentData().toString().toStdString();
        value.living_unit_id=selected.empty()?std::optional<std::string>{}:std::optional<std::string>{selected};
        if(changes.settings.version==2)value.version=2;
        if (confirmed->isChecked()) value.source_geometry_sha256=appraisal_reporting_source_digest(source,current.toStdString());
        auto living=std::find_if(changes.settings.living_units.begin(),changes.settings.living_units.end(),[&](const auto& item){return item.unit_id==selected;});
        if(living!=changes.settings.living_units.end() && level_number->value()==0) {
            const auto floor=source.entities().at(current.toStdString()).properties.value("floor_id",std::string{});
            std::erase_if(living->levels,[&](const auto& level){return level.floor_id==floor;});
        } else if(living!=changes.settings.living_units.end()) {
            const auto floor=source.entities().at(current.toStdString()).properties.value("floor_id",std::string{});
            auto level=std::find_if(living->levels.begin(),living->levels.end(),[&](const auto& item){return item.floor_id==floor;});
            if(level==living->levels.end()){living->levels.push_back({});level=std::prev(living->levels.end());level->floor_id=floor;}
            level->level_number=static_cast<unsigned>(level_number->value());
            const auto token=[](QComboBox* box)->std::optional<std::string>{const auto value=box->currentData().toString();return value.isEmpty()?std::nullopt:std::optional<std::string>{value.toStdString()};};
            level->grade_level_type=token(level_grade);
            const bool below=level->grade_level_type && *level->grade_level_type!="above_grade";
            level->below_grade_access=below?token(level_access):std::nullopt;
            const bool exterior=level->below_grade_access=="interior_and_exterior" || level->below_grade_access=="exterior_only";
            level->exterior_access=exterior?token(level_exterior):std::nullopt;
            level->exterior_access_description=level->exterior_access=="other"?level_description->text().toStdString():std::string{};
            if(confirmed->isChecked())level->source_geometry_sha256=appraisal_reporting_level_source_digest(source,floor);
        }
        for (int row=0;row<rooms->rowCount();++row) {
            const auto* id=rooms->item(row,0);
            const auto* use=static_cast<QComboBox*>(rooms->cellWidget(row,1));
            const auto* total=static_cast<QComboBox*>(rooms->cellWidget(row,2));
            const auto* description=static_cast<QLineEdit*>(rooms->cellWidget(row,3));
            const int included=total->currentData().toInt();
            value.rooms.push_back({id ? id->text().toStdString() : std::string{}, static_cast<AppraisalRoomUse>(use->currentData().toInt()),
                included<0 ? std::optional<bool>{} : std::optional<bool>{included==1},
                use->currentData().toInt()==static_cast<int>(AppraisalRoomUse::other) && value.version==2?description->text().toStdString():std::string{}});
        }
    }
    void select() {
        try { retain(); } catch (const std::exception& failure) { error->setText(text(failure.what())); }
        current=area->currentData().toString(); rooms->setRowCount(0); confirmed->setChecked(false);
        if (current.isEmpty()) return;
        const auto& value=declarations.at(current.toStdString());
        const auto boundary=std::find_if(report.boundaries.begin(),report.boundaries.end(),[&](const auto& item){return item.boundary_id==current.toStdString();});
        containment->setEnabled(boundary!=report.boundaries.end() && boundary->facts && boundary->facts->ansi &&
            boundary->facts->ansi->dwelling_identity==DwellingIdentity::attached_adu);
        containment->setCurrentIndex(containment->findData(value.contained_within_primary ? (*value.contained_within_primary ? 1 : 0) : -1));
        refresh_units();
        {const QSignalBlocker blocker(unit);
            const auto selected=value.living_unit_id?text(*value.living_unit_id):QString{};
            if(!selected.isEmpty() && unit->findData(selected)<0)unit->addItem(QStringLiteral("Unavailable unit — reassign before Save"),selected);
            unit->setCurrentIndex(std::max(0,unit->findData(selected)));}
        load_level();
        update_fields();
        for (const auto& room:value.rooms) add(room);
        try {
            const bool valid=value.source_geometry_sha256==appraisal_reporting_source_digest(source,current.toStdString());
            source_note->setText(valid ? QStringLiteral("Declarations match the current measurements and appraisal observations.") : QStringLiteral("Review and reconfirm these rooms against the current measurements and appraisal observations."));
        } catch (const std::exception& failure) { source_note->setText(text(failure.what())); }
    }
};
AppraisalReportingDialog::AppraisalReportingDialog(const DocumentSnapshot& source,const AppraisalDocumentReport& report,QWidget* parent,
    const std::set<std::string, std::less<>>* visible_entity_ids)
    : QDialog(parent),impl_(std::make_unique<Impl>(source,report,visible_entity_ids)) {
    check_revision(source,report);
    setObjectName(QStringLiteral("appraisalReportingDialog")); setWindowTitle(QStringLiteral("Appraisal reporting and rooms")); resize(840,720);
    auto& p=*impl_;auto* outer=new QVBoxLayout(this);auto* fields=new QWidget(this);auto* layout=new QVBoxLayout(fields);
    auto* scroll=new QScrollArea(this);scroll->setObjectName(QStringLiteral("appraisalReportingFieldsScroll"));scroll->setWidgetResizable(true);scroll->setWidget(fields);outer->addWidget(scroll);
    auto* intro=new QLabel(QStringLiteral("Declare rooms by their original design. Full bathroom: sink, toilet and tub or shower. Half bathroom: sink and toilet. Room counts use these declarations; placed fixture symbols do not verify them."),this); intro->setWordWrap(true);layout->addWidget(intro);
    auto* form=new QFormLayout;p.declaration_form=form;
    p.contract=new QComboBox(this);p.contract->setObjectName(QStringLiteral("appraisalReportingContract"));
    p.contract->addItem(QStringLiteral("Legacy UAD 2.6"),static_cast<int>(AppraisalReportingContract::legacy_uad_2_6));
    p.contract->addItem(QStringLiteral("UAD 3.6"),static_cast<int>(AppraisalReportingContract::uad_3_6));
    form->addRow(QStringLiteral("Reporting contract"),p.contract);
    p.complete=new QCheckBox(QStringLiteral("I have declared the complete room inventory for these measurements"),this);p.complete->setObjectName(QStringLiteral("appraisalReportingComplete"));form->addRow(p.complete);
    p.units=new QTableWidget(0,2,this);p.units->setObjectName(QStringLiteral("appraisalReportingUnits"));
    p.units->setHorizontalHeaderLabels({QStringLiteral("Appraiser unit identifier (unique)"),QStringLiteral("Dwelling role")});
    p.units->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);p.units->setMaximumHeight(130);
    form->addRow(QStringLiteral("Living units"),p.units);
    auto* unit_actions=new QWidget(this);auto* unit_buttons=new QHBoxLayout(unit_actions);unit_buttons->setContentsMargins(0,0,0,0);
    auto* add_unit=new QPushButton(QStringLiteral("Add unit"),unit_actions);add_unit->setObjectName(QStringLiteral("appraisalReportingAddUnit"));
    auto* remove_unit=new QPushButton(QStringLiteral("Remove selected unit"),unit_actions);remove_unit->setObjectName(QStringLiteral("appraisalReportingRemoveUnit"));
    unit_buttons->addWidget(add_unit);unit_buttons->addWidget(remove_unit);unit_buttons->addStretch();form->addRow(unit_actions);
    p.area=new QComboBox(this);p.area->setObjectName(QStringLiteral("appraisalReportingArea"));form->addRow(QStringLiteral("Measured area"),p.area);
    p.unit=new QComboBox(this);p.unit->setObjectName(QStringLiteral("appraisalReportingUnit"));p.unit->addItem(QStringLiteral("Unknown / unassigned"),QString{});form->addRow(QStringLiteral("Living unit for this area"),p.unit);
    p.level_number=new QSpinBox(this);p.level_number->setObjectName(QStringLiteral("appraisalReportingLevelNumber"));p.level_number->setRange(0,99);p.level_number->setSpecialValueText(QStringLiteral("Unknown"));form->addRow(QStringLiteral("Level number within this unit"),p.level_number);
    const auto level_choice=[&](const char* object,std::initializer_list<const char*> tokens){auto* box=new QComboBox(this);box->setObjectName(QString::fromLatin1(object));box->addItem(QStringLiteral("Unknown"),QString{});for(const auto* token:tokens)box->addItem(words(token),QString::fromLatin1(token));return box;};
    p.level_grade=level_choice("appraisalReportingLevelGrade",{"above_grade","fully_below_grade","partially_below_grade"});form->addRow(QStringLiteral("Observed level grade"),p.level_grade);
    p.level_access=level_choice("appraisalReportingLevelAccess",{"interior_and_exterior","interior_only","exterior_only"});form->addRow(QStringLiteral("Below-grade access"),p.level_access);
    p.level_exterior=level_choice("appraisalReportingLevelExteriorAccess",{"cellar_door","walk_out","walk_up","other"});form->addRow(QStringLiteral("Primary exterior access"),p.level_exterior);
    p.level_description=new QLineEdit(this);p.level_description->setObjectName(QStringLiteral("appraisalReportingLevelExteriorDescription"));p.level_description->setMaxLength(1024);form->addRow(QStringLiteral("Other exterior access description"),p.level_description);
    p.containment=p.choice(this);p.containment->setObjectName(QStringLiteral("appraisalReportingContained"));form->addRow(QStringLiteral("ADU contained within / part of primary dwelling"),p.containment);
    layout->addLayout(form);
    p.source_note=new QLabel(this);p.source_note->setWordWrap(true);layout->addWidget(p.source_note);
    p.confirmed=new QCheckBox(QStringLiteral("Confirm this area's rooms, unit assignment and unit-level facts against current measurements and observations"),this);p.confirmed->setObjectName(QStringLiteral("appraisalReportingReconfirm"));layout->addWidget(p.confirmed);
    p.rooms=new QTableWidget(0,4,this);p.rooms->setObjectName(QStringLiteral("appraisalReportingRooms"));
    p.rooms->setMinimumHeight(180);
    p.rooms->setHorizontalHeaderLabels({QStringLiteral("Room identifier"),QStringLiteral("Original room type"),QStringLiteral("Legacy Total Rooms member"),QStringLiteral("Other room description")});p.rooms->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);layout->addWidget(p.rooms);
    auto* room_actions=new QHBoxLayout;auto* add=new QPushButton(QStringLiteral("Add room"),this);add->setObjectName(QStringLiteral("appraisalReportingAddRoom"));auto* remove=new QPushButton(QStringLiteral("Remove selected room"),this);room_actions->addWidget(add);room_actions->addWidget(remove);room_actions->addStretch();layout->addLayout(room_actions);
    p.error=new QLabel(this);p.error->setObjectName(QStringLiteral("appraisalReportingError"));p.error->setWordWrap(true);outer->addWidget(p.error);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this);p.save=buttons->button(QDialogButtonBox::Save);p.save->setObjectName(QStringLiteral("appraisalReportingSave"));p.save->setEnabled(false);outer->addWidget(buttons);
    const auto& property=source.entities().at(report.property_id);
    try {if(property.properties.contains("appraisal_reporting"))p.changes.settings=parse_appraisal_reporting_settings(property.properties.at("appraisal_reporting"));}
    catch(const std::exception& failure){p.error->setText(text(failure.what()));}
    p.contract->setCurrentIndex(p.contract->findData(static_cast<int>(p.changes.settings.contract)));p.complete->setChecked(p.changes.settings.room_inventory_complete);
    for(const auto& value:p.changes.settings.living_units)p.add_unit(value);
    for(const auto& boundary:report.boundaries) if(!boundary.exclusion && boundary.facts && boundary.facts->use==AreaUse::dwelling) {
        const auto& item=source.entities().at(boundary.boundary_id);AppraisalAreaReportingFacts value;
        try{if(item.properties.contains("appraisal_reporting"))value=parse_appraisal_area_reporting_facts(item.properties.at("appraisal_reporting"));}
        catch(const std::exception& failure){p.error->setText(text(failure.what()));}
        p.declarations.emplace(boundary.boundary_id,std::move(value));
        const auto floor_id = item.properties.value("floor_id", std::string{});
        auto area_name = item.properties.value("name", std::string{});
        const auto display_name = area_name.empty() || area_name == boundary.boundary_id
            ? QStringLiteral("Measured area %1").arg(p.area->count() + 1) : text(area_name);
        p.area->addItem(name(source,floor_id)+QStringLiteral(" / ")+display_name,text(boundary.boundary_id));
    }
    connect(p.area,&QComboBox::currentIndexChanged,this,[this]{impl_->select();});
    connect(p.contract,&QComboBox::currentIndexChanged,this,[this]{impl_->update_fields();});
    connect(p.units,&QTableWidget::itemChanged,this,[this]{impl_->retain_units();impl_->refresh_units();});
    connect(add_unit,&QPushButton::clicked,this,[this]{auto& p=*impl_;p.retain();p.add_unit({QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),{},p.units->rowCount()==0?DwellingIdentity::primary:DwellingIdentity::attached_adu});p.retain_units();p.refresh_units();});
    connect(remove_unit,&QPushButton::clicked,this,[this]{auto& p=*impl_;p.retain();if(p.units->currentRow()>=0)p.units->removeRow(p.units->currentRow());p.retain_units();p.refresh_units();});
    connect(p.unit,&QComboBox::currentIndexChanged,this,[this]{auto& p=*impl_;const auto selected=p.unit->currentData().toString();
        {const QSignalBlocker blocker(p.unit);p.unit->setCurrentIndex(std::max(0,p.unit->findData(p.active_unit)));p.retain();p.unit->setCurrentIndex(std::max(0,p.unit->findData(selected)));}
        p.load_level();});
    for(auto* box:{p.level_grade,p.level_access,p.level_exterior})connect(box,&QComboBox::currentIndexChanged,this,[this]{impl_->update_fields();});
    connect(add,&QPushButton::clicked,this,[this]{impl_->add({QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(),AppraisalRoomUse::other,{}});});
    connect(remove,&QPushButton::clicked,this,[this]{if(impl_->rooms->currentRow()>=0)impl_->rooms->removeRow(impl_->rooms->currentRow());});
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,this,[this]{auto& p=*impl_;try{
        p.retain();p.retain_units();p.changes.settings.contract=static_cast<AppraisalReportingContract>(p.contract->currentData().toInt());p.changes.settings.room_inventory_complete=p.complete->isChecked();p.changes.areas.clear();
        for(const auto& declaration:p.declarations)p.changes.areas.push_back(declaration);
        validate_appraisal_reporting_changes(p.source,p.changes,p.semantic_visibility ? &*p.semantic_visibility : nullptr);QString error;
        if(!p.apply || !p.apply(p.changes,error)){p.error->setText(error.isEmpty()?QStringLiteral("Reporting changes could not be applied."):error);return;}accept();
    }catch(const std::exception& failure){p.error->setText(text(failure.what()));}});
    p.select();
}
AppraisalReportingDialog::~AppraisalReportingDialog()=default;
void AppraisalReportingDialog::setApplyRequested(std::function<bool(const AppraisalReportingChanges&,QString&)> callback){impl_->apply=std::move(callback);impl_->save->setEnabled(bool(impl_->apply));}

} // namespace sketch::desktop
