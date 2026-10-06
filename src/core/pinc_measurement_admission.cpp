#include "sketch/pinc_measurement_admission.hpp"

#include "sketch/area_type_presets.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/measurement_area_definition.hpp"
#include "sketch/measurement_linework.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace sketch {
namespace {
[[noreturn]] void fail(std::string_view path,std::string_view message) {
    throw std::invalid_argument("Pinc measurement admission "+std::string(path.empty()?"/":path)+": "+std::string(message));
}
void charge(std::uint64_t& total,std::uint64_t amount,std::uint64_t ceiling,std::string_view path,const char* message) {
    if(amount>ceiling-total)fail(path,message);total+=amount;
}
Segment reverse_segment(Segment value){std::swap(value.start,value.end);value.sweep_radians=-value.sweep_radians;return value;}
bool same_segment(const Segment& a,const Segment& b) {
    return a.start.x==b.start.x&&a.start.y==b.start.y&&a.end.x==b.end.x&&a.end.y==b.end.y&&a.sweep_radians==b.sweep_radians;
}
using Use=std::tuple<std::string_view,std::string_view,double,double,bool>;
std::vector<Use> oriented_uses(const DerivedMeasurementEdge& edge,bool reversed) {
    std::vector<Use> result;result.reserve(edge.source_uses.size());
    for(const auto& use:edge.source_uses)result.emplace_back(use.owner_id,use.segment_id,
        use.parameter_start,use.parameter_end,use.reversed!=reversed);
    std::sort(result.begin(),result.end());return result;
}
class ContentBudget {
public:
    explicit ContentBudget(const PincGeometryAdmissionLimits& value):limits(value),geometry_limits(value) {
        validate_pinc_import_limits(value.source);const PincGeometryAdmissionLimits hard;
        if(value.max_graph_edges>hard.max_graph_edges||value.max_face_edge_uses>hard.max_face_edge_uses||
            value.max_correspondence_work>hard.max_correspondence_work)fail("/limits","caller limits exceed hard ceilings");
        // Geometry has its own independently bounded counters. Partition rather
        // than reset those counters: two admission graphs, three authoring graphs
        // (detect, prepare's detect, prepare's complete source-lineage check).
        geometry_limits.max_graph_edges=value.max_graph_edges*2/5;
        geometry_limits.max_face_edge_uses=value.max_face_edge_uses*2/5;
        geometry_limits.max_correspondence_work=value.max_correspondence_work/2;
        geometry_limits.source.max_records=value.source.max_records/2;
    }
    void record(std::uint64_t count,std::string_view path) {
        charge(records,count,limits.source.max_records-geometry_limits.source.max_records,path,"aggregate content record budget exceeded");
    }
    void text(std::string_view value,std::string_view path) {
        charge(bytes,value.size(),limits.source.max_string_bytes,path,"aggregate descriptive string budget exceeded");
        if(value.find('\0')!=std::string_view::npos)fail(path,"NUL in descriptive or source-reference string");
    }
    void work(std::uint64_t count,std::string_view path) {
        charge(work_used,count,limits.max_correspondence_work-geometry_limits.max_correspondence_work,path,"aggregate content matching work budget exceeded");
    }
    void reserve_pairs(const PincImportProject& source) {
        std::uint64_t extra=0;
        for(const auto& page:source.pages) {
            const auto n=static_cast<std::uint64_t>(page.calculation_segments.size());
            if(n>limits.source.max_calculation_edges_per_page)fail(page.source.json_pointer,"per-page edge limit exceeded");
            // Reserve before admission and before any repeated O(n^2) noding.
            charge(extra,3*n*(n?n-1:0)/2,limits.source.max_calculation_pairs,page.source.json_pointer,"aggregate area-authoring pair budget exceeded");
        }
        geometry_limits.source.max_calculation_pairs-=extra;
    }
    void reserve_graph(const MeasurementAreaGraph& graph,std::string_view path) {
        charge(graph_edges,3*graph.edges.size(),limits.max_graph_edges-geometry_limits.max_graph_edges,path,"aggregate area-authoring graph budget exceeded");
        for(const auto& face:graph.faces) {
            charge(face_uses,3*face.edge_uses.size(),limits.max_face_edge_uses-geometry_limits.max_face_edge_uses,path,"aggregate area-authoring traversal budget exceeded");
            record(3,path);
        }
    }
    const PincGeometryAdmissionLimits& limits;
    PincGeometryAdmissionLimits geometry_limits;
private:
    std::uint64_t records{},bytes{},work_used{},graph_edges{},face_uses{};
};
bool same_face(const MeasurementAreaGraph& a,std::size_t ai,const MeasurementAreaGraph& b,std::size_t bi,
    ContentBudget& budget,std::string_view path) {
    budget.work(1,path);const auto& left=a.faces.at(ai);const auto& right=b.faces.at(bi);
    const auto n=left.edge_uses.size();if(!n||n!=right.edge_uses.size()||left.boundary.size()!=n||right.boundary.size()!=n)return false;
    for(std::size_t start=0;start<n;++start)for(const bool backwards:{false,true}) {
        bool match=true;
        for(std::size_t i=0;i<n;++i) {
            budget.work(1,path);const auto j=backwards?(start+n-i)%n:(start+i)%n;
            const auto& l=left.edge_uses[i];const auto& r=right.edge_uses[j];
            const auto& le=a.edges.at(l.edge_index);const auto& re=b.edges.at(r.edge_index);
            const auto lg=l.reversed?reverse_segment(le.geometry):le.geometry;
            const bool rr=r.reversed!=backwards;const auto rg=rr?reverse_segment(re.geometry):re.geometry;
            if(!same_segment(left.boundary[i],lg)||!same_segment(right.boundary[j],r.reversed?reverse_segment(re.geometry):re.geometry)||
                !same_segment(lg,rg)||le.source_uses.size()!=re.source_uses.size()){match=false;break;}
            const auto uses=static_cast<std::uint64_t>(le.source_uses.size());
            // Bound copying, sorting and equality before temporary allocations.
            budget.work(4*uses*uses+2*uses,path);
            if(oriented_uses(le,l.reversed)!=oriented_uses(re,rr)){match=false;break;}
        }
        if(match)return true;
    }
    return false;
}
void validate_authored_area(const Entity& area,const DetectedMeasurementAreas& detection,std::size_t index,
    std::string_view classification,ContentBudget& budget,std::string_view path) {
    const auto& face=detection.graph.faces.at(index);const auto boundary=boundary_geometry(decode_identified_boundary_entity(area));
    if(boundary.size()!=face.boundary.size()||area.properties.at("classification")!=std::string(classification)||
        area.properties.at("measurement_classification")!=std::string(classification)||
        area.properties.at("property_id")!=detection.context.property_id||area.properties.at("building_id")!=detection.context.building_id||
        area.properties.at("floor_id")!=detection.context.floor_id||area.properties.at("layer_id")!=detection.context.layer_id)
        fail(path,"native area authoring changed reviewed geometry, classification or drawing context");
    auto lineage=nlohmann::json::array();
    for(std::size_t i=0;i<boundary.size();++i) {
        budget.work(1,path);if(!same_segment(boundary[i],face.boundary[i]))fail(path,"native area authoring changed reviewed analytical cycle");
        const auto& edge=face.edge_uses.at(i);auto uses=nlohmann::json::array();
        for(const auto& use:detection.graph.edges.at(edge.edge_index).source_uses) {
            budget.work(1,path);uses.push_back({{"owner_id",use.owner_id},{"segment_id",use.segment_id},
                {"parameter_start",use.parameter_start},{"parameter_end",use.parameter_end},{"reversed",use.reversed!=edge.reversed}});
        }
        lineage.push_back(std::move(uses));
    }
    if(area.extensions.at("measurement_linework_sources")!=lineage)
        fail(path,"native area authoring changed reviewed exact source intervals");
}
std::vector<std::vector<const PincAreaReview*>> validate_reviews(const PincImportProject& source,
    std::span<const PincAreaReview> reviews,ContentBudget& budget) {
    if(source.pages.empty()||source.pages.size()>budget.limits.source.max_pages)fail("/pages","invalid page count");
    std::vector<std::vector<const PincAreaReview*>> result(source.pages.size());std::size_t count=0;
    const auto reference_text=[&](const PincSourceReference& reference) {
        budget.text(reference.collection,reference.json_pointer);budget.text(reference.json_pointer,reference.json_pointer);
        budget.text(reference.identity,reference.json_pointer);
        if(reference.scalar_id_json)budget.text(*reference.scalar_id_json,reference.json_pointer);
    };
    budget.text(source.source_version,"/version");
    budget.record(source.diagnostics.size(),"/diagnostics");
    for(const auto& diagnostic:source.diagnostics) {
        budget.text(diagnostic.source_pointer,"/diagnostics");budget.text(diagnostic.code,"/diagnostics");budget.text(diagnostic.message,"/diagnostics");
    }
    for(std::size_t p=0;p<source.pages.size();++p) {
        const auto& page=source.pages[p];budget.record(page.assignments.size(),page.source.json_pointer);
        if(page.calculation_segments.size()>budget.limits.source.max_calculation_edges_per_page||
            page.interior_segments.size()>budget.limits.source.max_interior_edges_per_page)
            fail(page.source.json_pointer,"per-page edge count exceeds source budget");
        budget.record(page.calculation_segments.size(),page.source.json_pointer);
        budget.record(page.interior_segments.size(),page.source.json_pointer);
        budget.record(page.legacy_areas.size(),page.source.json_pointer);
        if(page.assignments.size()>budget.limits.source.max_records-count)fail(page.source.json_pointer,"assignment count exceeds record budget");
        count+=page.assignments.size();result[p].resize(page.assignments.size());
        reference_text(page.source);
        const auto segment_text=[&](const PincImportSegment& segment) {
            budget.record(segment.equivalent_sources.size(),segment.source.json_pointer);
            reference_text(segment.source);for(const auto& reference:segment.equivalent_sources)reference_text(reference);
        };
        for(const auto& segment:page.calculation_segments)segment_text(segment);
        for(const auto& segment:page.interior_segments)segment_text(segment);
        for(const auto& area:page.legacy_areas) {
            if(area.segments.size()>budget.limits.source.max_calculation_edges_per_page)fail(area.source.json_pointer,"legacy cycle exceeds source budget");
            budget.record(area.segments.size(),area.source.json_pointer);
            reference_text(area.source);for(const auto& segment:area.segments)segment_text(segment);
        }
        std::set<std::string> identities;
        for(const auto& assignment:page.assignments) {
            budget.text(assignment.name,assignment.source.json_pointer);budget.text(assignment.code,assignment.source.json_pointer);
            reference_text(assignment.source);
            if(assignment.source_segment_references.size()>budget.limits.source.max_calculation_edges_per_page)
                fail(assignment.source.json_pointer,"assignment source references exceed source budget");
            budget.record(assignment.source_segment_references.size(),assignment.source.json_pointer);
            for(const auto& reference:assignment.source_segment_references)reference_text(reference);
            if(!identities.insert(assignment.source.identity).second)fail(assignment.source.json_pointer,"duplicate source assignment occurrence");
        }
    }
    if(reviews.size()!=count)fail("/reviews","exactly one explicit review is required per source assignment");
    budget.record(reviews.size(),"/reviews");
    for(const auto& review:reviews) {
        if(review.page_index>=result.size()||review.assignment_index>=result[review.page_index].size())fail("/reviews","out-of-range assignment review");
        auto& slot=result[review.page_index][review.assignment_index];if(slot)fail("/reviews","duplicate assignment review");slot=&review;
        budget.text(review.classification,"/reviews");
        if((review.import_area||!review.classification.empty())&&!area_type_for_classification(review.classification))
            fail("/reviews","choose an exact native descriptive classification; roles and appraisal categories are unsupported");
    }
    return result;
}
void validate_measurement_contexts(const DocumentSnapshot& base,std::span<const PincPageGeometryContext> contexts) {
    for(const auto& context:contexts) {
        const auto property=base.entities().find(context.calculation.property_id);
        if(property==base.entities().end()||property->second.type!="property")fail("/contexts","measurement property context required");
        const auto workflow=property->second.properties.find("calculation_workflow");
        if(workflow==property->second.properties.end()||!workflow->is_string()||*workflow!="measurement")
            fail("/contexts","area import requires an explicit generic measurement workflow");
    }
    // The independently-created import target must not make authoring rebuild
    // unrelated retained source graphs outside this invocation's pair budget.
    for(const auto& [id,entity]:base.entities())if(entity.extensions.contains("measurement_linework_sources")||entity.extensions.contains("measurement_linework_group"))
        fail("/private_base","independent import target already contains source-derived measured areas: "+id);
}
void collect_ids(const Entity& entity,std::set<std::string>& occupied,bool inserting) {
    const auto add=[&](const std::string& id) {
        if(inserting&&(id.empty()||id.size()>128||occupied.contains(id)))fail("/native_candidate","native area/topology identity collision or length violation");
        occupied.insert(id);
    };
    add(entity.id);
    if(entity.type=="measurement_linework") {
        const auto model=decode_measurement_linework_model(entity.properties.at("model"));
        if(!model.supported())fail("/private_base","unsupported measured topology");
        for(const auto& edge:model.model->edges){add(edge.segment_id);add(edge.start_vertex_id);add(edge.end_vertex_id);}
    } else if(can_recognize_boundary_entity_type(entity.type)&&inspect_boundary_entity_version(entity).format==BoundaryEntityFormat::identified_v1) {
        std::set<std::string> vertices;
        for(const auto& edge:decode_identified_boundary_entity(entity).segments) {
            add(edge.segment_id);vertices.insert(edge.start_vertex_id);vertices.insert(edge.end_vertex_id);
        }
        for(const auto& id:vertices)add(id);
    }
}
}
PincMeasurementAdmission prepare_pinc_measurement_admission(const PincImportProject& source,
    const DocumentSnapshot& private_base,std::span<const PincPageGeometryContext> contexts,std::string_view fresh_namespace,
    std::span<const PincAreaReview> reviews,const PincGeometryAdmissionLimits& limits) {
    try {
        ContentBudget budget(limits);const auto reviewed=validate_reviews(source,reviews,budget);
        validate_measurement_contexts(private_base,contexts);budget.reserve_pairs(source);
        PincMeasurementAdmission result;result.geometry=admit_pinc_geometry(source,private_base,contexts,fresh_namespace,budget.geometry_limits);
        result.entities=result.geometry.entities;result.diagnostics=source.diagnostics;
        result.diagnostics.insert(result.diagnostics.end(),result.geometry.diagnostics.begin(),result.geometry.diagnostics.end());
        for(const auto& diagnostic:result.diagnostics) {
            budget.text(diagnostic.source_pointer,"/diagnostics");budget.text(diagnostic.code,"/diagnostics");budget.text(diagnostic.message,"/diagnostics");
        }
        std::set<std::string> occupied;
        for(const auto& [id,entity]:private_base.entities()){(void)id;collect_ids(entity,occupied,false);}
        for(const auto& [id,asset]:private_base.assets()){(void)asset;occupied.insert(id);}
        for(const auto& entity:result.geometry.entities)collect_ids(entity,occupied,true);
        ApplyEntityChanges geometry_command{private_base.revision(),{}, {},"Preview imported measured strokes"};
        for(const auto& entity:result.geometry.entities)geometry_command.entity_changes.push_back(EntityChange::upsert(entity));
        const auto preview=geometry_command.entity_changes.empty()?private_base:Document::preview_command(private_base,geometry_command);
        const auto diagnostic=[&](const PincImportAssignment& assignment,std::string code,std::string message) {
            budget.record(1,assignment.source.json_pointer);budget.text(code,assignment.source.json_pointer);budget.text(message,assignment.source.json_pointer);
            result.diagnostics.push_back({assignment.source.json_pointer,std::move(code),std::move(message)});
        };
        for(const auto& page:result.geometry.pages) {
            const auto& input=source.pages.at(page.page_index);
            std::vector<const PincAssignmentCorrespondence*> accepted;
            for(const auto& correspondence:page.assignments) {
                const auto& assignment=input.assignments.at(correspondence.assignment_index);const auto& review=*reviewed[page.page_index][correspondence.assignment_index];
                if(!review.import_area){diagnostic(assignment,"assignment_reference_only","Explicit review retained this assignment as source reference only.");continue;}
                const auto* category=area_type_for_code(assignment.code);
                if(!category||(category->classification.empty()&&review.classification!="non_calculated")) {
                    diagnostic(assignment,"assignment_unknown_category","Source category has no supported descriptive area type; assignment remains in original source.");continue;
                }
                if(!correspondence.face_index)continue; // Geometry diagnostic is already retained.
                accepted.push_back(&correspondence);
            }
            if(accepted.empty())continue;
            if(page.calculation_stroke_ids.empty())fail(input.source.json_pointer,"matched assignment has no calculation source");
            budget.reserve_graph(page.graph,input.source.json_pointer);
            const auto detection=detect_measurement_areas(preview,page.calculation_stroke_ids.front());
            if(detection.context!=page.calculation_context||detection.graph.edges.size()!=page.graph.edges.size()||
                detection.graph.faces.size()!=page.graph.faces.size())fail(input.source.json_pointer,"independent native detection changed admitted graph resources or context");
            std::vector<MeasurementAreaChoice> choices(detection.graph.faces.size());
            std::map<std::size_t,const PincAssignmentCorrespondence*> matched;
            for(const auto* correspondence:accepted) {
                const auto& assignment=input.assignments[correspondence->assignment_index];std::optional<std::size_t> found;bool ambiguous=false;
                for(std::size_t f=0;f<detection.graph.faces.size();++f)if(same_face(page.graph,*correspondence->face_index,detection.graph,f,budget,assignment.source.json_pointer)) {
                    if(found){ambiguous=true;break;}found=f;
                }
                if(!found||ambiguous) {diagnostic(assignment,"assignment_no_unique_rederived_face","No unique independently rederived analytic cycle with the exact native source intervals.");continue;}
                if(!matched.emplace(*found,correspondence).second)fail(assignment.source.json_pointer,"multiple reviewed assignments claim a rederived native face");
                choices[*found]={MeasurementAreaDisposition::define_area,reviewed[page.page_index][correspondence->assignment_index]->classification};
            }
            if(matched.empty())continue;
            // The native author performs a complete lineage check over every
            // new area and graph face. Reserve its cyclic copying/sorting work
            // before calling it; no per-assignment graph reconstruction occurs.
            for(const auto& [face,correspondence]:matched) {
                (void)correspondence;const auto n=static_cast<std::uint64_t>(detection.graph.faces[face].edge_uses.size());
                for(const auto& candidate:detection.graph.faces) {
                    if(candidate.edge_uses.size()!=n)continue;std::uint64_t weight=n;
                    for(const auto& edge:candidate.edge_uses) {
                        const auto uses=static_cast<std::uint64_t>(detection.graph.edges.at(edge.edge_index).source_uses.size());
                        weight+=4*uses*uses+2*uses;
                    }
                    budget.work((2*n+1)*weight,input.source.json_pointer);
                }
                // Native identity upgrade and detached preview validation repeat
                // closed-boundary checks; reserve those passes before authoring.
                budget.work(8*n*n,input.source.json_pointer);
            }
            const auto definition=prepare_measurement_area_definition(preview,page.calculation_stroke_ids.front(),choices);
            if(definition.face_area_ids.size()!=detection.graph.faces.size()||definition.command.entity_changes.size()!=matched.size())
                fail(input.source.json_pointer,"native authoring did not define exactly the reviewed independent faces");
            std::map<std::string,Entity> areas;
            for(const auto& change:definition.command.entity_changes) {
                if(change.kind!=EntityChangeKind::upsert||change.entity.type!="measurement_boundary"||
                    !change.entity.extensions.contains("measurement_linework_sources")||change.entity.properties.contains("appraisal_facts")||
                    change.entity.properties.contains("appraisal_category")||change.entity.properties.contains("deduction_ids")||
                    change.entity.properties.contains("wall_measurement_source"))fail(input.source.json_pointer,"native authoring supplied unsupported area authority");
                collect_ids(change.entity,occupied,true);areas.emplace(change.entity.id,change.entity);
            }
            for(const auto& [face,correspondence]:matched) {
                const auto& id=definition.face_area_ids[face];if(!id||!areas.contains(*id))fail(input.source.json_pointer,"native face mapping is missing");
                const auto& assignment=input.assignments[correspondence->assignment_index];auto& area=areas.at(*id);
                validate_authored_area(area,detection,face,reviewed[page.page_index][correspondence->assignment_index]->classification,
                    budget,assignment.source.json_pointer);
                area.properties["name"]=assignment.name;
                budget.record(2,assignment.source.json_pointer);result.area_mappings.push_back({assignment.source,*id});
            }
            for(auto& [id,area]:areas){(void)id;result.entities.push_back(std::move(area));}
        }
        ApplyEntityChanges complete{private_base.revision(),{}, {},"Preview complete reviewed Pinc measurement content"};
        for(const auto& entity:result.entities)complete.entity_changes.push_back(EntityChange::upsert(entity));
        if(!complete.entity_changes.empty())(void)Document::preview_command(private_base,complete);
        return result;
    }catch(const std::invalid_argument&){throw;}
    catch(const std::exception& error){fail("/native_candidate",error.what());}
}
} // namespace sketch
