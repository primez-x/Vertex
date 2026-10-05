#include "sketch/pinc_geometry_admission.hpp"

#include "sketch/boundary_entity.hpp"
#include "sketch/closed_boundary_detection.hpp"
#include "sketch/measurement_linework.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace sketch {
namespace {
[[noreturn]] void fail(std::string_view pointer,std::string_view message) {
    throw std::invalid_argument("Pinc geometry admission "+std::string(pointer.empty()?"/":pointer)+": "+std::string(message));
}
bool same_point(Vec2 a,Vec2 b){return a.x==b.x&&a.y==b.y;}
bool same_segment(const Segment& a,const Segment& b) {
    return same_point(a.start,b.start)&&same_point(a.end,b.end)&&a.sweep_radians==b.sweep_radians;
}
Segment reversed(Segment s){std::swap(s.start,s.end);s.sweep_radians=-s.sweep_radians;return s;}
void check_source(const PincSourceReference& s,std::size_t page,std::string_view collection={}) {
    if(s.page_index!=page || (!collection.empty()&&s.collection!=collection) || s.identity!=pinc_source_identity(s))
        fail(s.json_pointer,"source identity does not match its occurrence/collection");
}
void check_segment(const PincImportSegment& s) {
    const auto& g=s.geometry;
    if(!std::isfinite(g.start.x)||!std::isfinite(g.start.y)||!std::isfinite(g.end.x)||!std::isfinite(g.end.y)||
        !std::isfinite(g.sweep_radians)||!std::isfinite(s.source_sagitta_metres)||std::abs(s.source_sagitta_metres)>1e6)
        fail(s.source.json_pointer,"nonfinite or excessive analytical input");
    if(s.source_kind!="line"&&s.source_kind!="arc")fail(s.source.json_pointer,"unknown geometry kind");
    try {
        const auto bounds=segment_bounds(g);
        if(std::abs(bounds.minimum.x)>1e6||std::abs(bounds.minimum.y)>1e6||
            std::abs(bounds.maximum.x)>1e6||std::abs(bounds.maximum.y)>1e6||
            !(segment_length(g)>default_geometry_tolerance_metres))fail(s.source.json_pointer,"geometry exceeds native precision or coordinate budget");
        Segment expected{g.start,g.end,0};
        if(s.source_kind=="arc"&&std::abs(s.source_sagitta_metres)>=.001*.3048&&
            std::hypot(g.end.x-g.start.x,g.end.y-g.start.y)>=.001*.3048)
            expected=arc_from_chord_height(g.start,g.end,s.source_sagitta_metres);
        if(!same_segment(expected,g))fail(s.source.json_pointer,"analytical geometry disagrees with retained source curve input");
    }catch(const std::invalid_argument& e){fail(s.source.json_pointer,e.what());}
}
class Budget {
public:
    explicit Budget(const PincGeometryAdmissionLimits& l):limits(l) {
        validate_pinc_import_limits(l.source);const PincGeometryAdmissionLimits hard;
        if(l.max_graph_edges>hard.max_graph_edges||l.max_face_edge_uses>hard.max_face_edge_uses||
            l.max_correspondence_work>hard.max_correspondence_work)fail("/","caller admission limits exceed hard ceilings");
    }
    void record(std::size_t n,std::string_view p){charge(records,n,limits.source.max_records,p,"aggregate record budget exceeded");}
    void edge(std::size_t n,std::string_view p){charge(edges,n,limits.source.max_total_edges,p,"aggregate retained edge budget exceeded");}
    void work(std::uint64_t n,std::string_view p) {
        if(n>limits.max_correspondence_work-work_used)fail(p,"aggregate correspondence work budget exceeded");work_used+=n;
    }
    void pairs(std::size_t n,std::string_view p) {
        const auto count=static_cast<std::uint64_t>(n)*(n?n-1:0)/2;
        if(count>limits.source.max_calculation_pairs-pairs_used)fail(p,"aggregate calculation pair budget exceeded");pairs_used+=count;
    }
    void graph(const MeasurementAreaGraph& g,std::string_view p) {
        charge(graph_edges,g.edges.size(),limits.max_graph_edges,p,"aggregate derived edge budget exceeded");
        record(g.faces.size(),p);
        for(const auto& face:g.faces)charge(face_uses,face.edge_uses.size(),limits.max_face_edge_uses,p,"aggregate face traversal budget exceeded");
    }
    const PincGeometryAdmissionLimits& limits;
private:
    static void charge(std::size_t& total,std::size_t n,std::size_t ceiling,std::string_view p,const char* message) {
        if(n>ceiling-total)fail(p,message);total+=n;
    }
    std::size_t records{},edges{},graph_edges{},face_uses{};std::uint64_t work_used{},pairs_used{};
};
struct PageSources {
    std::map<std::string,const PincImportSegment*,std::less<>> calculations;
    std::map<std::string,const PincImportSegment*,std::less<>> originals;
};
std::vector<PageSources> validate_source(const PincImportProject& source,Budget& budget) {
    if(source.pages.empty()||source.pages.size()>budget.limits.source.max_pages||source.current_page>=source.pages.size())
        fail("/pages","invalid page count or current page");
    const bool legacy=source.dialect==PincImportDialect::legacy_v2;
    if(!legacy&&source.dialect!=PincImportDialect::modern_v42)fail("/version","unsupported source dialect");
    if((!legacy&&source.source_version!="4.2")||(legacy&&!source.source_version.starts_with("2.")))
        fail("/version","source dialect and saved version disagree");
    std::vector<PageSources> result;result.reserve(source.pages.size());
    budget.record(source.diagnostics.size(),"/diagnostics");
    for(std::size_t p=0;p<source.pages.size();++p) {
        const auto& page=source.pages[p];check_source(page.source,p,"pages");const auto& path=page.source.json_pointer;
        budget.record(1,path);PageSources index;
        if(page.calculation_segments.size()>budget.limits.source.max_calculation_edges_per_page||
            page.interior_segments.size()>budget.limits.source.max_interior_edges_per_page)fail(path,"per-page source edge budget exceeded");
        budget.pairs(page.calculation_segments.size(),path);
        budget.pairs(page.calculation_segments.size(),path);
        std::size_t original_count=0;
        for(std::size_t a=0;a<page.legacy_areas.size();++a) {
            if(!legacy)fail(path,"modern source cannot carry legacy areas");const auto& area=page.legacy_areas[a];
            check_source(area.source,p,"areas");budget.record(1,area.source.json_pointer);
            if(area.segments.size()>budget.limits.source.max_calculation_edges_per_page-original_count)fail(path,"legacy original edge budget exceeded");
            original_count+=area.segments.size();
            for(const auto& s:area.segments) {
                check_source(s.source,p,"areas/"+std::to_string(a)+"/segments");check_segment(s);
                budget.edge(1,s.source.json_pointer);budget.record(1,s.source.json_pointer);
                if(!s.equivalent_sources.empty()||!index.originals.emplace(s.source.identity,&s).second)
                    fail(s.source.json_pointer,"duplicate or recursive legacy original source");
            }
        }
        const auto add=[&](const PincImportSegment& s,bool interior) {
            check_source(s.source,p,interior?(legacy?"interiors":"interiorWalls"):(legacy?"":"calcWalls"));check_segment(s);
            budget.edge(1,s.source.json_pointer);budget.record(1,s.source.json_pointer);
            if(interior) {
                if(!s.equivalent_sources.empty())fail(s.source.json_pointer,"interior source cannot carry calculation aliases");return;
            }
            if(!index.calculations.emplace(s.source.identity,&s).second)fail(s.source.json_pointer,"duplicate normalized source identity");
            if(legacy) {
                const auto original=index.originals.find(s.source.identity);
                if(original==index.originals.end()||!same_segment(original->second->geometry,s.geometry))
                    fail(s.source.json_pointer,"normalized legacy source disagrees with original");
            } else if(!s.equivalent_sources.empty())fail(s.source.json_pointer,"modern source cannot carry legacy aliases");
            for(const auto& ref:s.equivalent_sources) {
                check_source(ref,p);budget.record(1,ref.json_pointer);const auto original=index.originals.find(ref.identity);
                if(original==index.originals.end()||(!same_segment(original->second->geometry,s.geometry)&&
                    !same_segment(original->second->geometry,reversed(s.geometry)))||
                    !index.calculations.emplace(ref.identity,&s).second)fail(ref.json_pointer,"invalid exact legacy alias");
            }
        };
        for(const auto& s:page.calculation_segments)add(s,false);
        std::set<std::string,std::less<>> interior_ids;
        for(const auto& s:page.interior_segments) {
            add(s,true);if(!interior_ids.insert(s.source.identity).second)fail(s.source.json_pointer,"duplicate interior source identity");
        }
        if(legacy&&index.calculations.size()!=index.originals.size())fail(path,"legacy original source has no normalized mapping");
        for(const auto& a:page.assignments) {
            check_source(a.source,p,legacy?"areas":"assignments");budget.record(1,a.source.json_pointer);
            budget.record(a.source_segment_references.size(),a.source.json_pointer);
            for(const auto& ref:a.source_segment_references)check_source(ref,p);
            if(legacy&&(!a.legacy_area_index||*a.legacy_area_index>=page.legacy_areas.size()||
                a.source.identity!=page.legacy_areas[*a.legacy_area_index].source.identity))fail(a.source.json_pointer,"invalid legacy area assignment reference");
            if(!legacy&&a.legacy_area_index)fail(a.source.json_pointer,"modern assignment carries legacy authority");
        }
        budget.record(page.symbols.size(),path);budget.record(page.texts.size(),path);if(page.underlay)budget.record(1,path);
        result.push_back(std::move(index));
    }
    return result;
}
std::vector<const PincPageGeometryContext*> validate_contexts(const DocumentSnapshot& base,
    std::span<const PincPageGeometryContext> supplied,std::size_t pages) {
    if(!base.is_editable())fail("/contexts","private base is read-only");
    if(supplied.size()!=pages)fail("/contexts","reviewed context required for every page occurrence");
    const auto organization=organize_project(base);
    std::vector<const PincPageGeometryContext*> result(pages,nullptr);std::set<std::string,std::less<>> layers;
    for(const auto& item:supplied) {
        if(item.page_index>=pages||result[item.page_index])fail("/contexts","duplicate or out-of-range page context");
        const auto verify=[&](const DrawingContext& c) {
            if(!c.complete()||!c.level_id.empty())fail("/contexts","reviewed complete unbound page drawing context required");
            const auto layer=base.entities().find(c.layer_id);
            const auto actual=organization.drawing_context(c.layer_id);
            if(layer==base.entities().end()||layer->second.type!="layer"||!actual||*actual!=c)
                fail("/contexts","drawing context does not resolve against private base");
            if(!layers.insert(c.layer_id).second)fail("/contexts","page occurrences and lanes must use separate layers");
        };
        verify(item.calculation);verify(item.interior);
        if(item.calculation.property_id!=item.interior.property_id||item.calculation.building_id!=item.interior.building_id||
            item.calculation.floor_id!=item.interior.floor_id)
            fail("/contexts","page occurrence requires separate calculation/interior lanes in one reviewed floor");
        result[item.page_index]=&item;
    }
    for(const auto& [id,e]:base.entities())if(e.type=="measurement_linework"||e.type=="measurement_boundary"||e.type=="boundary") {
        const auto context=organization.drawing_context(id);if(!context)continue;
        for(const auto* item:result)if(*context==item->calculation||*context==item->interior)
            fail("/contexts","reviewed page geometry layers must be empty");
    }
    return result;
}
std::set<std::string,std::less<>> occupied_ids(const DocumentSnapshot& base,std::string_view prefix) {
    std::set<std::string,std::less<>> result;
    const auto add=[&](const std::string& id) {
        if(id==prefix||id.starts_with(std::string(prefix)+":"))fail("/namespace","namespace already occupied");result.insert(id);
    };
    for(const auto& [id,e]:base.entities()) {
        add(id);
        if(e.type=="measurement_linework") {
            const auto decoded=decode_measurement_linework_model(e.properties.at("model"));
            if(!decoded.supported())fail("/namespace","unsupported base topology");
            for(const auto& edge:decoded.model->edges){add(edge.segment_id);add(edge.start_vertex_id);add(edge.end_vertex_id);}
        } else if(can_recognize_boundary_entity_type(e.type)&&inspect_boundary_entity_version(e).format==BoundaryEntityFormat::identified_v1) {
            for(const auto& edge:decode_identified_boundary_entity(e).segments){add(edge.segment_id);add(edge.start_vertex_id);add(edge.end_vertex_id);}
        }
    }
    for(const auto& [id,a]:base.assets()){(void)a;add(id);}return result;
}
bool same_cycle(const Boundary& source,const Boundary& face,Budget& budget,std::string_view path) {
    budget.work(1,path);if(source.size()!=face.size()||source.empty())return false;
    // Simple native cycles have unique directed edges: locate a rotation using
    // the first edge, then compare once in each possible traversal direction.
    for(std::size_t start=0;start<face.size();++start)for(const bool reverse:{false,true}) {
        budget.work(1,path);const auto first=reverse?reversed(face[start]):face[start];
        if(!same_segment(source.front(),first))continue;
        for(std::size_t i=1;i<source.size();++i) {
            budget.work(1,path);auto candidate=face[reverse?(start+face.size()-i)%face.size():(start+i)%face.size()];
            if(reverse)candidate=reversed(candidate);if(!same_segment(source[i],candidate))return false;
        }
        return true;
    }
    return false;
}
using NativeSourceIdentities=std::map<std::pair<std::string_view,std::string_view>,std::string_view>;
struct LegacyCycleSource {
    std::string_view normalized_identity;
    Segment geometry;
    bool forward{};
};
std::vector<LegacyCycleSource> legacy_cycle_sources(const Boundary& cycle,const PincLegacyArea& area,
    const PageSources& lookup,Budget& budget,std::string_view path) {
    const auto count=cycle.size();budget.work(static_cast<std::uint64_t>(count)*count+count,path);
    std::vector<LegacyCycleSource> result;result.reserve(count);
    for(const auto& edge:cycle) {
        const PincImportSegment* original=nullptr;
        for(const auto& s:area.segments)if(same_segment(edge,s.geometry)||same_segment(edge,reversed(s.geometry))) {
            if(original)fail(path,"original cycle has ambiguous analytical source ownership");original=&s;
        }
        if(!original)fail(path,"assembled cycle has no exact original source");
        const auto& normalized=*lookup.calculations.at(original->source.identity);
        const bool forward=same_segment(edge,normalized.geometry);
        if(!forward&&!same_segment(edge,reversed(normalized.geometry)))fail(path,"original cycle disagrees with normalized source direction");
        result.push_back({normalized.source.identity,edge,forward});
    }
    return result;
}
bool same_legacy_cycle(const std::vector<LegacyCycleSource>& cycle,const MeasurementAreaGraph& graph,std::size_t face_index,
    const NativeSourceIdentities& identities,Budget& budget,std::string_view path) {
    budget.work(1,path);
    const auto& face=graph.faces[face_index];const auto count=face.edge_uses.size();
    if(cycle.empty()||count<cycle.size())return false;
    // Every candidate traversal is bounded before entering it; no expanded
    // replacement geometry or inferred segment correspondence is allocated.
    budget.work(2*static_cast<std::uint64_t>(count)*count+2*count,path);
    for(std::size_t start=0;start<count;++start)for(const bool reverse:{false,true}) {
        std::size_t source_index=0;double next_parameter=cycle.front().forward?0:1;
        Vec2 next_point=cycle.front().geometry.start;bool exact=true;
        for(std::size_t i=0;i<count;++i) {
            if(source_index==cycle.size()){exact=false;break;}
            const auto& selected=face.edge_uses[reverse?(start+count-i)%count:(start+i)%count];
            const auto& edge=graph.edges.at(selected.edge_index);const bool reverse_edge=selected.reversed!=reverse;
            const auto piece=reverse_edge?reversed(edge.geometry):edge.geometry;
            if(!same_point(piece.start,next_point)){exact=false;break;}
            const auto& original=cycle[source_index];const MeasurementSourceUse* interval=nullptr;
            for(const auto& use:edge.source_uses) {
                budget.work(1,path);const auto found=identities.find({use.owner_id,use.segment_id});
                if(found!=identities.end()&&found->second==original.normalized_identity) {
                    if(interval){exact=false;break;}interval=&use;
                }
            }
            if(!exact||!interval){exact=false;break;}
            const bool backwards=interval->reversed!=reverse_edge;
            const auto from=backwards?interval->parameter_end:interval->parameter_start;
            const auto to=backwards?interval->parameter_start:interval->parameter_end;
            if(backwards==original.forward||from!=next_parameter||
                (original.forward?!(to>from):!(to<from))){exact=false;break;}
            next_point=piece.end;next_parameter=to;
            if(to==(original.forward?1:0)) {
                if(!same_point(piece.end,original.geometry.end)){exact=false;break;}
                ++source_index;
                if(source_index<cycle.size())next_parameter=cycle[source_index].forward?0:1;
            }
        }
        if(exact&&source_index==cycle.size()&&same_point(next_point,cycle.front().geometry.start))return true;
    }
    return false;
}
bool same_face_lineage(const MeasurementAreaGraph& original,std::size_t original_face,
    const MeasurementAreaGraph& native,std::size_t native_face,const NativeSourceIdentities& identities,
    Budget& budget,std::string_view path) {
    const auto& source=original.faces[original_face];const auto& target=native.faces[native_face];
    if(source.edge_uses.size()!=target.edge_uses.size())return false;
    const auto count=source.edge_uses.size();
    budget.work(static_cast<std::uint64_t>(count)*count,path);
    using Lineage=std::tuple<std::string_view,double,double,bool>;
    for(const auto& selected:source.edge_uses) {
        const auto& a=original.edges.at(selected.edge_index);const DerivedMeasurementEdge* b=nullptr;
        for(const auto& candidate:target.edge_uses) {
            const auto& edge=native.edges.at(candidate.edge_index);
            if(same_segment(a.geometry,edge.geometry)){b=&edge;break;}
        }
        if(!b||a.source_uses.size()!=b->source_uses.size())return false;
        const auto uses=static_cast<std::uint64_t>(a.source_uses.size());
        // Charge a conservative bound for copying, sorting and exact equality
        // before allocating the temporary lineage lists.
        budget.work(4*uses*uses+2*uses,path);
        std::vector<Lineage> left,right;left.reserve(a.source_uses.size());right.reserve(b->source_uses.size());
        for(const auto& use:a.source_uses) {
            if(use.owner_id!=use.segment_id)return false;
            left.emplace_back(use.owner_id,use.parameter_start,use.parameter_end,use.reversed);
        }
        for(const auto& use:b->source_uses) {
            const auto found=identities.find({use.owner_id,use.segment_id});if(found==identities.end())return false;
            right.emplace_back(found->second,use.parameter_start,use.parameter_end,use.reversed);
        }
        std::sort(left.begin(),left.end());std::sort(right.begin(),right.end());if(left!=right)return false;
    }
    return true;
}
void correspond(const PincImportPage& source,const PageSources& lookup,const MeasurementAreaGraph& original_graph,PincAdmittedPageGeometry& page,
    const NativeSourceIdentities& identities,bool legacy,Budget& budget,std::vector<PincImportDiagnostic>& diagnostics) {
    // Membership is derived from every analytical edge's exact source intervals,
    // not a saved key, cached area or centroid. A key is only a reviewed selection
    // of source identities; several faces may have that same selection.
    std::vector<std::set<std::string_view>> memberships;
    if(!legacy)for(const auto& face:original_graph.faces) {
        std::set<std::string_view> membership;
        for(const auto& edge:face.edge_uses)for(const auto& use:original_graph.edges.at(edge.edge_index).source_uses) {
            budget.work(membership.size()+1,source.source.json_pointer);membership.insert(use.owner_id);
        }
        memberships.push_back(std::move(membership));
    }
    const auto unresolved=[&](PincAssignmentCorrespondence& c,std::string code,std::string reason) {
        c.face_index.reset();c.code=std::move(code);c.reason=std::move(reason);
        budget.record(1,c.source.json_pointer);diagnostics.push_back({c.source.json_pointer,c.code,c.reason});
    };
    for(std::size_t i=0;i<source.assignments.size();++i) {
        const auto& a=source.assignments[i];PincAssignmentCorrespondence c{i,a.source,{},"",""};budget.record(1,a.source.json_pointer);
        const auto count=legacy?source.legacy_areas[*a.legacy_area_index].segments.size():a.source_segment_references.size();
        if(count>budget.limits.source.max_calculation_edges_per_page)fail(a.source.json_pointer,"assignment cycle exceeds source limit");
        std::optional<Boundary> legacy_cycle;std::vector<LegacyCycleSource> legacy_sources;
        const Boundary* cycle=nullptr;std::optional<std::size_t> source_face;
        if(legacy) {
            // Reserve existing assembly's quadratic endpoint/validation cost
            // before copying original analytical evidence.
            budget.work(4*static_cast<std::uint64_t>(count)*count+count,a.source.json_pointer);
            Boundary geometry;geometry.reserve(count);
            for(const auto& s:source.legacy_areas[*a.legacy_area_index].segments)geometry.push_back(s.geometry);
            try {legacy_cycle=assemble_boundary_from_segments(geometry);cycle=&*legacy_cycle;}
            catch(const std::invalid_argument& e){unresolved(c,"assignment_invalid_cycle",
                std::string("Original analytical source does not form one exact cycle: ")+e.what());}
            if(cycle)legacy_sources=legacy_cycle_sources(*cycle,source.legacy_areas[*a.legacy_area_index],lookup,budget,a.source.json_pointer);
        } else {
            budget.work(static_cast<std::uint64_t>(count)*count+count,a.source.json_pointer);
            std::set<std::string_view> used;bool complete=a.references_resolved;
            for(const auto& ref:a.source_segment_references) {
                const auto found=lookup.calculations.find(ref.identity);
                if(found==lookup.calculations.end()||!used.insert(ref.identity).second)complete=false;
            }
            if(!complete||used.empty())unresolved(c,"assignment_unmatched_sources","Source references do not establish distinct calculation sources.");
            else {
                std::optional<std::size_t> selected;bool ambiguous=false;
                for(std::size_t f=0;f<memberships.size();++f) {
                    budget.work(1,a.source.json_pointer);if(memberships[f].size()!=used.size())continue;
                    budget.work(used.size(),a.source.json_pointer);
                    if(memberships[f]!=used)continue;
                    if(selected){ambiguous=true;break;}selected=f;
                }
                if(ambiguous)unresolved(c,"assignment_ambiguous_source_faces","Several original analytical faces use the exact selected sources; source key cannot choose one.");
                else if(!selected)unresolved(c,"assignment_no_source_face","No original analytical face uses exactly the selected sources.");
                else {source_face=selected;cycle=&original_graph.faces[*selected].boundary;}
            }
        }
        if(cycle) {
            std::vector<std::size_t> matches;
            for(std::size_t f=0;f<page.graph.faces.size();++f)
                if((legacy?same_legacy_cycle(legacy_sources,page.graph,f,identities,budget,a.source.json_pointer):
                    same_cycle(*cycle,page.graph.faces[f].boundary,budget,a.source.json_pointer))&&
                    (!source_face||same_face_lineage(original_graph,*source_face,page.graph,f,identities,budget,a.source.json_pointer)))matches.push_back(f);
            if(matches.size()!=1)unresolved(c,matches.empty()?"assignment_no_exact_face":"assignment_ambiguous_faces",
                "No unique current graph face has the same complete analytical cycle and mapped source lineage.");
            else {c.face_index=matches.front();c.code="assignment_exact_cycle";c.reason="Unique exact analytical cycle; classification action still requires explicit review.";}
        }
        page.assignments.push_back(std::move(c));
    }
    std::map<std::size_t,std::size_t> owners;
    for(const auto& c:page.assignments)if(c.face_index)++owners[*c.face_index];
    for(auto& c:page.assignments)if(c.face_index&&owners[*c.face_index]>1)
        unresolved(c,"assignment_multiple_owners","Multiple original assignments claim one exact face; explicit ownership review is required.");
}
} // namespace
PincGeometryAdmission admit_pinc_geometry(const PincImportProject& source,const DocumentSnapshot& private_base,
    std::span<const PincPageGeometryContext> contexts,std::string_view fresh_namespace,const PincGeometryAdmissionLimits& limits) {
    Budget budget(limits);const auto source_index=validate_source(source,budget);
    if(fresh_namespace.empty()||fresh_namespace.size()>101||!std::all_of(fresh_namespace.begin(),fresh_namespace.end(),[](char c){
        return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';}))fail("/namespace","invalid fresh namespace");
    // Fork validates the complete retained base history, not just its current map.
    Document detached=[&]() {
        try{return Document::fork(private_base);}
        catch(const std::exception& e){fail("/private_base",e.what());}
    }();
    const auto base=detached.snapshot();
    const auto reviewed=validate_contexts(base,contexts,source.pages.size());auto occupied=occupied_ids(base,fresh_namespace);
    PincGeometryAdmission result;
    for(std::size_t p=0;p<source.pages.size();++p) {
        const auto& input=source.pages[p];const auto& context=*reviewed[p];
        PincAdmittedPageGeometry page;page.page_index=p;page.calculation_context=context.calculation;page.interior_context=context.interior;
        std::vector<MeasurementGraphSource> graph_sources;graph_sources.reserve(input.calculation_segments.size());
        NativeSourceIdentities identities;
        const auto append=[&](const PincImportSegment& s,std::size_t ordinal,bool interior) {
            const auto stem=std::string(fresh_namespace)+":p"+std::to_string(p)+(interior?":interior:":":calc:")+std::to_string(ordinal);
            const auto allocate=[&](const char* suffix){auto id=stem+":"+suffix;if(!occupied.insert(id).second)fail("/namespace","native identity collision");return id;};
            const auto stroke=allocate("stroke"),segment=allocate("segment"),start=allocate("start"),end=allocate("end");
            ConstructionReceipt receipt;receipt.segment_id=segment;receipt.start=s.geometry.start;receipt.chord_end=s.geometry.end;
            if(s.geometry.sweep_radians==0)receipt.kind=BoundaryConstructionKind::line_to_point;
            else {receipt.kind=BoundaryConstructionKind::arc_chord_angle;receipt.angle=angle_from_radians(s.geometry.sweep_radians);}
            MeasurementLinework model;model.stroke_id=stroke;model.anchor=s.geometry.start;model.edges.push_back({segment,start,end,receipt});
            const auto replay=replay_measurement_linework(model);
            if(replay.edges.size()!=1||!same_segment(replay.edges[0].segment,s.geometry))fail(s.source.json_pointer,"native receipt replay changes analytical source");
            const auto& c=interior?context.interior:context.calculation;
            budget.record(1,s.source.json_pointer);
            result.entities.push_back({stroke,"measurement_linework",{{"property_id",c.property_id},{"building_id",c.building_id},
                {"floor_id",c.floor_id},{"layer_id",c.layer_id},{"model",encode_measurement_linework_model(model)}},true});
            const auto map=[&](const PincSourceReference& ref,bool reverse) {
                budget.record(1,ref.json_pointer);result.source_mappings.push_back({ref,stroke,segment,start,end,reverse});};
            map(s.source,false);
            for(const auto& alias:s.equivalent_sources) {
                const auto original=source_index[p].originals.at(alias.identity);
                map(alias,!same_segment(original->geometry,s.geometry));
            }
            if(interior)page.interior_stroke_ids.push_back(stroke);
            else {page.calculation_stroke_ids.push_back(stroke);graph_sources.push_back({stroke,segment,replay.edges[0].segment});}
        };
        for(std::size_t i=0;i<input.calculation_segments.size();++i)append(input.calculation_segments[i],i,false);
        for(std::size_t i=0;i<input.interior_segments.size();++i)append(input.interior_segments[i],i,true);
        for(std::size_t i=0;i<graph_sources.size();++i)
            identities.emplace(std::pair<std::string_view,std::string_view>{graph_sources[i].owner_id,graph_sources[i].segment_id},input.calculation_segments[i].source.identity);
        std::vector<MeasurementGraphSource> original_sources;original_sources.reserve(input.calculation_segments.size());
        for(const auto& s:input.calculation_segments)original_sources.push_back({s.source.identity,s.source.identity,s.geometry});
        MeasurementAreaGraph original_graph;
        try {
            original_graph=build_measurement_area_graph(original_sources);
            page.graph=build_measurement_area_graph(graph_sources);
        }catch(const std::invalid_argument& e){fail(input.source.json_pointer,e.what());}
        budget.graph(original_graph,input.source.json_pointer);
        budget.graph(page.graph,input.source.json_pointer);
        correspond(input,source_index[p],original_graph,page,identities,source.dialect==PincImportDialect::legacy_v2,budget,result.diagnostics);
        result.pages.push_back(std::move(page));
    }
    ApplyEntityChanges command{base.revision(),{}, {},"Prepare detached Pinc geometry"};
    for(const auto& entity:result.entities)command.entity_changes.push_back(EntityChange::upsert(entity));
    if(!command.entity_changes.empty()) {
        try{(void)Document::preview_command(base,command);}catch(const std::exception& e){fail("/native_candidate",e.what());}
    }
    return result;
}
} // namespace sketch
