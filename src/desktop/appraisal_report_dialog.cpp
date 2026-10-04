#include "sketch/desktop/appraisal_report_dialog.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/area_arithmetic.hpp"

#include <QAbstractTextDocumentLayout>
#include <QComboBox>
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
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTreeWidget>
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
    return QString::number(metric && !ansi(report)?metres:metres/0.3048,'f',ansi(report)?1:report.display_decimal_places)+
        (metric && !ansi(report)?QStringLiteral(" m"):QStringLiteral(" ft"));
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
        row(QStringLiteral("Limitations statement"),value.limitations_statement.empty()?QStringLiteral("Undeclared"):text(value.limitations_statement));
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
        if(ansi(report))html+=QStringLiteral("<p><b>Primary dwelling GLA: %1</b></p>").arg(area(report.calculation->property.gla().total.square_metres,report,metric).toHtmlEscaped());
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
            const auto geometry=entity->second.properties.find("boundary");
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

} // namespace sketch::desktop
