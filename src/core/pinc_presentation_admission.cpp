#include "sketch/pinc_presentation_admission.hpp"

#include "sketch/boundary_entity.hpp"
#include "sketch/measurement_area_definition.hpp"
#include "sketch/measurement_linework.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace sketch {
namespace {
using Json=nlohmann::json;
[[noreturn]] void fail(std::string_view pointer,std::string_view message) {
    throw std::invalid_argument("Pinc presentation admission "+std::string(pointer.empty()?"/":pointer)+": "+std::string(message));
}
void require(bool value,std::string_view pointer,std::string_view message) {if(!value)fail(pointer,message);}
bool same_source(const PincSourceReference& a,const PincSourceReference& b) {
    return a.page_index==b.page_index && a.collection==b.collection && a.scalar_id_json==b.scalar_id_json &&
        a.json_pointer==b.json_pointer && a.identity==b.identity;
}
Json provenance(const PincSourceReference& source) {
    return {{"source_pointer",source.json_pointer},{"source_identity",source.identity}};
}
void source_reference(const PincSourceReference& source,std::size_t page,std::string_view collection) {
    require(source.page_index==page && source.collection==collection && !source.json_pointer.empty() &&
        source.identity==pinc_source_identity(source),source.json_pointer,"invalid source occurrence identity");
}
void finite_point(Vec2 point,std::string_view path) {
    require(std::isfinite(point.x)&&std::isfinite(point.y)&&std::abs(point.x)<=1e6&&std::abs(point.y)<=1e6,
        path,"invalid or excessive presentation coordinate");
}
void positive_size(double size,std::string_view path) {
    require(std::isfinite(size)&&size>0&&size<=1e6,path,"invalid or excessive presentation size");
}
void rotation(double angle,std::string_view path) {
    require(std::isfinite(angle)&&std::abs(angle)<=1e6,path,"invalid presentation rotation");
}
class Budget {
public:
    explicit Budget(const PincPresentationAdmissionLimits& supplied):limits(supplied),geometry_limits(supplied.geometry_verification) {
        const PincPresentationAdmissionLimits hard;
        require(limits.max_bindings<=hard.max_bindings && limits.max_symbols<=hard.max_symbols &&
            limits.max_labels<=hard.max_labels && limits.max_annotation_records<=hard.max_annotation_records &&
            limits.max_pinned_svg_bytes<=hard.max_pinned_svg_bytes && limits.max_string_bytes<=hard.max_string_bytes,
            "/limits","caller limits exceed hard ceilings");
        validate_pinc_import_limits(geometry_limits.source);const PincGeometryAdmissionLimits geometry_hard;
        require(geometry_limits.max_graph_edges<=geometry_hard.max_graph_edges&&
            geometry_limits.max_face_edge_uses<=geometry_hard.max_face_edge_uses&&
            geometry_limits.max_correspondence_work<=geometry_hard.max_correspondence_work,
            "/limits","caller geometry verification limits exceed hard ceilings");
        geometry_limits.max_graph_edges/=2;geometry_limits.max_face_edge_uses/=2;
        geometry_limits.max_correspondence_work/=2;
    }
    void charge(std::size_t& total,std::size_t count,std::size_t ceiling,std::string_view path) {
        require(total<=ceiling && count<=ceiling-total,path,"aggregate presentation budget exceeded");total+=count;
    }
    void text(std::string_view text,std::string_view path,std::size_t individual=65536) {
        require(text.size()<=individual&&text.find('\0')==std::string_view::npos,path,"invalid or excessive presentation string");
        charge(strings,text.size(),limits.max_string_bytes,path);
    }
    void record(std::size_t count,std::string_view path){charge(records,count,limits.max_annotation_records,path);}
    void work(std::uint64_t amount,std::string_view path) {
        const auto ceiling=limits.geometry_verification.max_correspondence_work-geometry_limits.max_correspondence_work;
        require(work_used<=ceiling&&amount<=ceiling-work_used,path,"aggregate presentation verification work exceeded");work_used+=amount;
    }
    void reserve_pairs(const PincImportProject& source) {
        std::uint64_t extra=0;
        for(const auto& page:source.pages) {
            const auto n=static_cast<std::uint64_t>(page.calculation_segments.size());
            require(n<=geometry_limits.source.max_calculation_edges_per_page,page.source.json_pointer,"per-page geometry edge limit exceeded");
            const auto pairs=n*(n?n-1:0); // Two native detector graph passes.
            require(pairs<=geometry_limits.source.max_calculation_pairs-extra,page.source.json_pointer,
                "aggregate presentation verification pair budget exceeded");extra+=pairs;
        }
        geometry_limits.source.max_calculation_pairs-=extra;
    }
    void reserve_detection(const MeasurementAreaGraph& graph,const std::vector<const Entity*>& owners,std::string_view path) {
        charge(graph_edges,2*graph.edges.size(),limits.geometry_verification.max_graph_edges-geometry_limits.max_graph_edges,path);
        for(const auto& face:graph.faces)
            charge(face_uses,2*face.edge_uses.size(),limits.geometry_verification.max_face_edge_uses-geometry_limits.max_face_edge_uses,path);
        // Native detection verifies all current lineage then cyclic/reversed
        // analytical ownership. Reserve worst-case copying, sorting, equality,
        // and closed-boundary validation before the native helper executes.
        for(const auto* owner:owners) {
            require(!owner->extensions.contains("measurement_linework_group"),path,"imported assignment cannot claim combined native ownership");
            const auto n=static_cast<std::uint64_t>(decode_identified_boundary_entity(*owner).segments.size());
            require(n<=16384,path,"native owner exceeds bounded face traversal");
            work(12*n*n,path);
            const auto lineage=owner->extensions.find("measurement_linework_sources");
            require(lineage!=owner->extensions.end()&&lineage->is_array()&&lineage->size()==n,path,"native owner has malformed source lineage");
            for(const auto& edge:*lineage) {
                require(edge.is_array()&&!edge.empty()&&edge.size()<=geometry_limits.source.max_calculation_edges_per_page,
                    path,"native owner lineage exceeds retained source budget");
                const auto uses=static_cast<std::uint64_t>(edge.size());work(4*uses*uses+2*uses,path);
            }
            for(const auto& face:graph.faces) {
                work(1,path);if(face.edge_uses.size()!=n)continue;
                std::uint64_t weight=n;
                for(const auto& edge:face.edge_uses) {
                    const auto uses=static_cast<std::uint64_t>(graph.edges.at(edge.edge_index).source_uses.size());
                    weight+=4*uses*uses+2*uses;
                }
                work((2*n+1)*weight+2*n*n,path);
            }
        }
    }
    PincPresentationAdmissionLimits limits;
    PincGeometryAdmissionLimits geometry_limits;
    std::size_t symbols{},labels{},records{},strings{},svg{};
    std::size_t graph_edges{},face_uses{};
    std::uint64_t work_used{};
};
using SourceMap=std::map<std::string,const PincSourceGeometryMapping*,std::less<>>;
struct Verified {
    PincGeometryAdmission geometry;
    SourceMap sources;
    std::map<std::string,const Entity*,std::less<>> areas;
};
Verified verify_measurement(const PincImportProject& source,const PincMeasurementAdmission& supplied,
    const DocumentSnapshot& candidate,Budget& budget) {
    budget.reserve_pairs(source);
    // Validate retained history and native candidate before inspecting metadata.
    (void)Document::fork(candidate);
    require(supplied.geometry.pages.size()==source.pages.size(),"/mappings","page contexts do not cover source");
    std::vector<PincPageGeometryContext> contexts;
    for(const auto& page:supplied.geometry.pages)contexts.push_back({page.page_index,page.calculation_context,page.interior_context});
    std::set<std::string,std::less<>> removed;
    for(const auto& entity:supplied.entities) {
        const auto actual=candidate.entities().find(entity.id);
        require(removed.insert(entity.id).second&&actual!=candidate.entities().end()&&actual->second==entity,
            "/measurement_candidate","measurement admission does not match private preview");
        require(entity.type=="measurement_linework"||entity.type=="measurement_boundary", "/measurement_candidate","unexpected measurement entity type");
    }
    std::vector<Entity> base_entities;
    for(const auto& [id,entity]:candidate.entities())if(!removed.contains(id))base_entities.push_back(entity);
    std::vector<Asset> assets;for(const auto& [id,asset]:candidate.assets()){(void)id;assets.push_back(asset);}
    const auto base=Document::create(std::move(base_entities),std::move(assets));
    std::string geometry_namespace="pinc_empty_geometry_validation";
    if(!supplied.geometry.source_mappings.empty()) {
        const auto& id=supplied.geometry.source_mappings.front().stroke_id;const auto separator=id.find(":p");
        require(separator!=std::string::npos,"/mappings","unrecognized imported stroke identity");
        geometry_namespace=id.substr(0,separator);
    }
    Verified result;result.geometry=admit_pinc_geometry(source,base.snapshot(),contexts,geometry_namespace,budget.geometry_limits);
    require(result.geometry.entities==supplied.geometry.entities,"/mappings","geometry entities differ from independent source replay");
    require(result.geometry.source_mappings.size()==supplied.geometry.source_mappings.size(),"/mappings","incomplete source mapping");
    for(std::size_t i=0;i<result.geometry.source_mappings.size();++i) {
        const auto& expected=result.geometry.source_mappings[i];const auto& actual=supplied.geometry.source_mappings[i];
        require(same_source(expected.source,actual.source)&&expected.stroke_id==actual.stroke_id&&
            expected.segment_id==actual.segment_id&&expected.start_vertex_id==actual.start_vertex_id&&
            expected.end_vertex_id==actual.end_vertex_id&&expected.reversed==actual.reversed,
            actual.source.json_pointer,"forged source-to-native mapping");
        const auto found=candidate.entities().find(expected.stroke_id);
        require(found!=candidate.entities().end()&&removed.contains(expected.stroke_id),expected.source.json_pointer,"mapped stroke absent from complete measurement admission");
        result.sources.emplace(expected.source.identity,&expected);
    }
    // Geometry entities can acquire ordinary source provenance, but their native
    // geometry, type and context must still equal independently admitted records.
    for(const auto& expected:result.geometry.entities) {
        const auto& actual=candidate.entities().at(expected.id);
        require(actual.type==expected.type&&actual.properties==expected.properties&&actual.required==expected.required,
            "/mappings","mapped measured stroke geometry or context changed");
    }
    const auto organization=organize_project(candidate);
    std::set<std::string,std::less<>> area_ids;
    std::size_t supplied_areas=0;for(const auto& entity:supplied.entities)if(entity.type=="measurement_boundary")++supplied_areas;
    require(supplied_areas==supplied.area_mappings.size(),"/mappings","area mapping coverage differs from measurement admission");
    std::vector<std::optional<DetectedMeasurementAreas>> detections(source.pages.size());
    std::vector<std::vector<const Entity*>> page_owners(source.pages.size());
    for(const auto& mapping:supplied.area_mappings) {
        require(mapping.source.page_index<source.pages.size(),mapping.source.json_pointer,"area mapping page outside source");
        const auto actual=candidate.entities().find(mapping.area_id);
        require(actual!=candidate.entities().end()&&actual->second.type=="measurement_boundary"&&removed.contains(mapping.area_id),
            mapping.source.json_pointer,"mapped native area is absent from complete measurement candidate");
        page_owners[mapping.source.page_index].push_back(&actual->second);
    }
    for(std::size_t p=0;p<source.pages.size();++p)if(!page_owners[p].empty()) {
        const auto& page=result.geometry.pages[p];const auto& context=page.calculation_context;
        require(!page.calculation_stroke_ids.empty(),source.pages[p].source.json_pointer,"area has no native calculation source");
        budget.reserve_detection(page.graph,page_owners[p],source.pages[p].source.json_pointer);
        std::vector<Entity> scoped;
        for(const auto& id:{context.property_id,context.building_id,context.floor_id,context.layer_id})scoped.push_back(candidate.entities().at(id));
        for(const auto& id:page.calculation_stroke_ids)scoped.push_back(candidate.entities().at(id));
        for(const auto* owner:page_owners[p])scoped.push_back(*owner);
        // All records come from the validated complete candidate. A page's
        // native detector cannot repeatedly rebuild unrelated project graphs.
        const auto detached=Document::create(std::move(scoped));
        detections[p]=detect_measurement_areas(detached.snapshot(),page.calculation_stroke_ids.front());
        require(detections[p]->context==context&&detections[p]->graph.edges.size()==page.graph.edges.size()&&
            detections[p]->graph.faces.size()==page.graph.faces.size(),source.pages[p].source.json_pointer,
            "independent page detection changed admitted native graph resources or context");
    }
    for(const auto& mapping:supplied.area_mappings) {
        const auto p=mapping.source.page_index;
        require(p<source.pages.size(),mapping.source.json_pointer,"area mapping page outside source");
        const auto& assignments=source.pages[p].assignments;
        const auto assignment=std::find_if(assignments.begin(),assignments.end(),[&](const auto& a){return same_source(a.source,mapping.source);});
        require(assignment!=assignments.end(),mapping.source.json_pointer,"area mapping lacks source assignment");
        const auto ordinal=static_cast<std::size_t>(assignment-assignments.begin());
        const auto& page=result.geometry.pages[p];
        const auto correspondence=std::find_if(page.assignments.begin(),page.assignments.end(),[&](const auto& a){return a.assignment_index==ordinal;});
        require(correspondence!=page.assignments.end()&&correspondence->face_index,mapping.source.json_pointer,"area mapping lacks unique independently verified correspondence");
        require(!page.calculation_stroke_ids.empty(),mapping.source.json_pointer,"area has no native calculation source");
        require(detections[p].has_value(),mapping.source.json_pointer,"native page owner detection is missing");
        const auto& detected=*detections[p];
        const auto face=*correspondence->face_index;
        require(face<detected.existing_area_ids.size()&&detected.existing_area_ids[face]==mapping.area_id,
            mapping.source.json_pointer,"source assignment targets a different native face owner");
        const auto actual=candidate.entities().find(mapping.area_id);
        require(actual!=candidate.entities().end()&&actual->second.type=="measurement_boundary"&&removed.contains(mapping.area_id)&&
            organization.drawing_context(mapping.area_id)==page.calculation_context&&
            area_ids.insert(mapping.area_id).second&&result.areas.emplace(mapping.source.identity,&actual->second).second,
            mapping.source.json_pointer,"area mapping has invalid owner, lineage, context or duplicate identity");
        require(actual->second.properties.value("name",std::string{})==assignment->name,
            mapping.source.json_pointer,"native area name does not match its source assignment");
    }
    return result;
}
void collect_occupied(const DocumentSnapshot& candidate,std::set<std::string,std::less<>>& ids,std::string_view prefix) {
    const auto add=[&](const std::string& id) {
        require(id!=prefix&&!id.starts_with(std::string(prefix)+":"),"/namespace","namespace already occupied");ids.insert(id);
    };
    for(const auto& [id,entity]:candidate.entities()) {
        add(id);
        if(entity.type==kAnnotationEntityType) {
            const auto state=decode_annotation_entity(entity);
            for(const auto& label:state.labels)add(label.id);for(const auto& symbol:state.symbols)add(symbol.id);
        } else if(entity.type=="measurement_linework") {
            const auto model=decode_measurement_linework_model(entity.properties.at("model"));
            require(model.supported(),"/namespace","unsupported native topology");
            for(const auto& edge:model.model->edges){add(edge.segment_id);add(edge.start_vertex_id);add(edge.end_vertex_id);}
        } else if(can_recognize_boundary_entity_type(entity.type)&&inspect_boundary_entity_version(entity).format==BoundaryEntityFormat::identified_v1) {
            for(const auto& edge:decode_identified_boundary_entity(entity).segments){add(edge.segment_id);add(edge.start_vertex_id);add(edge.end_vertex_id);}
        }
    }
    for(const auto& [id,asset]:candidate.assets()){(void)asset;add(id);}
}
double paper_width(double pixels,std::string_view pointer) {
    positive_size(pixels,pointer);const auto mm=pixels*25.4/96;
    require(mm>=.05&&mm<=10,pointer,"source line weight exceeds native paper-line range");return mm;
}
AnnotationStyle text_style(std::string font,double height,std::string color,std::string_view path) {
    positive_size(height,path);AnnotationStyle style;style.font_family=std::move(font);
    style.text_height_metres=height;style.stroke_color=std::move(color);return style;
}
} // namespace

PincPresentationAdmission prepare_pinc_presentation_admission(const PincImportProject& source,
    const PincMeasurementAdmission& measurement,const DocumentSnapshot& candidate,
    std::span<const PincSymbolBinding> bindings,std::string_view fresh_namespace,const PincPresentationAdmissionLimits& limits) {
    Budget budget(limits);
    require(!fresh_namespace.empty()&&fresh_namespace.size()<=101&&std::all_of(fresh_namespace.begin(),fresh_namespace.end(),[](char c){
        return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';}),"/namespace","invalid fresh namespace");
    require(bindings.size()<=limits.max_bindings,"/bindings","binding catalog exceeds budget");
    std::size_t symbol_count=0,label_count=0;
    const auto source_strings=[&](const PincSourceReference& ref) {
        budget.text(ref.json_pointer,ref.json_pointer,131072);budget.text(ref.identity,ref.json_pointer,131072);
        if(ref.scalar_id_json)budget.text(*ref.scalar_id_json,ref.json_pointer);
    };
    for(std::size_t p=0;p<source.pages.size();++p) {
        const auto& page=source.pages[p];
        source_strings(page.source);
        for(const auto& segment:page.calculation_segments){source_strings(segment.source);for(const auto& alias:segment.equivalent_sources)source_strings(alias);}
        for(const auto& segment:page.interior_segments)source_strings(segment.source);
        for(const auto& assignment:page.assignments)source_strings(assignment.source);
        budget.charge(symbol_count,page.symbols.size(),limits.max_symbols,page.source.json_pointer);
        budget.charge(label_count,page.texts.size(),limits.max_labels,page.source.json_pointer);
        std::set<std::string,std::less<>> source_ids;
        for(std::size_t i=0;i<page.texts.size();++i) {
            const auto& input=page.texts[i];source_reference(input.source,p,"texts");
            source_strings(input.source);
            require(input.source.json_pointer==page.source.json_pointer+"/texts/"+std::to_string(i),input.source.json_pointer,"text pointer does not identify its source occurrence");
            require(source_ids.insert(input.source.identity).second,input.source.json_pointer,"duplicate text source identity");
        }
        source_ids.clear();
        for(std::size_t i=0;i<page.symbols.size();++i) {
            const auto& input=page.symbols[i];source_reference(input.source,p,"symbols");
            source_strings(input.source);
            require(input.source.json_pointer==page.source.json_pointer+"/symbols/"+std::to_string(i),input.source.json_pointer,"symbol pointer does not identify its source occurrence");
            require(source_ids.insert(input.source.identity).second,input.source.json_pointer,"duplicate symbol source identity");
        }
    }
    std::map<std::string,const PincSymbolBinding*,std::less<>> binding_index;
    std::map<std::string,Json,std::less<>> definitions;
    for(const auto& binding:bindings) {
        require(binding.door_transform==PincDoorArtworkTransform::none ||
            binding.door_transform==PincDoorArtworkTransform::swing_left_positive_svg_y ||
            binding.door_transform==PincDoorArtworkTransform::side_positive_svg_y ||
            binding.door_transform==PincDoorArtworkTransform::symmetric_source_garage,"/bindings","invalid canonical door transform");
        budget.text(binding.source_kind,"/bindings",256);budget.text(binding.fidelity_note,"/bindings");
        require(!binding.source_kind.empty()&&binding_index.emplace(binding.source_kind,&binding).second,"/bindings","duplicate or empty source kind");
        validate_symbol_catalog({binding.definition});
        require(binding.definition.svg_asset&&!binding.pinned_svg.empty(),"/bindings","binding requires pinned bundled SVG artwork");
        const auto bytes=std::span(reinterpret_cast<const std::byte*>(binding.pinned_svg.data()),binding.pinned_svg.size());
        require(sha256_hex(bytes)==binding.definition.svg_asset->sha256,"/bindings","pinned SVG does not match definition hash");
        budget.charge(budget.svg,binding.pinned_svg.size(),limits.max_pinned_svg_bytes,"/bindings");
        SymbolInstance trial;trial.id="validate_binding";trial.symbol_id=binding.definition.id;
        trial.definition=binding.definition;trial.pinned_svg=binding.pinned_svg;validate_annotation_state({{}, {trial}, {}},{});
        const auto manifest=encode_symbol_catalog_manifest({binding.definition});
        budget.charge(budget.strings,manifest.dump().size(),limits.max_string_bytes,"/bindings");
        const auto found=definitions.find(binding.definition.id);
        require(found==definitions.end()||found->second==manifest,"/bindings","same symbol definition ID carries different artwork");
        definitions.emplace(binding.definition.id,manifest);
    }
    const auto verified=verify_measurement(source,measurement,candidate,budget);
    std::set<std::string,std::less<>> occupied;collect_occupied(candidate,occupied,fresh_namespace);
    PincPresentationAdmission result;
    const auto diagnostic=[&](std::string path,std::string code,std::string message){
        budget.record(1,path);budget.text(path,path,131072);budget.text(code,path,256);budget.text(message,path);
        result.diagnostics.push_back({std::move(path),std::move(code),std::move(message)});
    };
    for(std::size_t p=0;p<source.pages.size();++p) {
        const auto& page=source.pages[p];const auto& context=verified.geometry.pages[p].calculation_context;
        AnnotationState state;Json provenance_records=Json::object();
        const auto allocate=[&](std::string suffix) {
            auto id=std::string(fresh_namespace)+":p"+std::to_string(p)+":"+suffix;
            require(id.size()<=128&&occupied.insert(id).second,page.source.json_pointer,"native annotation identity collision");return id;
        };
        const auto segment=[&](const PincImportSegment& input) {
            const auto& path=input.source.json_pointer;const auto& visual=input.presentation;
            const auto mapping=verified.sources.find(input.source.identity);require(mapping!=verified.sources.end(),path,"source segment is not mapped");
            budget.text(visual.color,path,256);budget.text(visual.dimension.font,path,256);budget.text(visual.dimension.color,path,256);
            PresentationOverride object;object.target_kind="object";object.target_id=mapping->second->stroke_id;
            object.style.stroke_color=visual.shared_color.value_or(visual.color);
            object.paper_line_width_mm=paper_width(visual.shared_weight_pixels.value_or(visual.weight_pixels),path);
            object.style.line_pattern=visual.shared_line_type.value_or(visual.line_type);
            state.overrides.push_back(std::move(object));
            PresentationOverride dimension;dimension.target_kind="wall_dimension";dimension.target_id=mapping->second->stroke_id;
            dimension.style=text_style(visual.dimension.font,visual.dimension.size_metres,visual.dimension.color,path);
            dimension.use_model_text_height=true;
            dimension.visible=visual.show_dimension;dimension.plan_label_offset=visual.dimension_offset_metres;
            if(dimension.plan_label_offset)finite_point(*dimension.plan_label_offset,path+"/dimOffset");
            state.overrides.push_back(std::move(dimension));budget.record(2,path);
            provenance_records[mapping->second->stroke_id]=provenance(input.source);
        };
        for(const auto& input:page.calculation_segments)segment(input);for(const auto& input:page.interior_segments)segment(input);
        for(const auto& assignment:page.assignments) {
            const auto area=verified.areas.find(assignment.source.identity);if(area==verified.areas.end())continue;
            const auto& path=assignment.source.json_pointer;const auto& visual=assignment.presentation;
            const auto anchor=area_label_anchor(boundary_geometry(decode_identified_boundary_entity(*area->second)));
            PresentationOverride appearance;appearance.target_kind="area";appearance.target_id=area->second->id;
            appearance.style.fill_color=visual.color;appearance.style.stroke_color=visual.sync_boundary_color?visual.color:visual.boundary_color;
            appearance.style.fill_pattern=visual.opacity==0?"none":"solid";
            appearance.style.fill_opacity=visual.opacity;
            appearance.style.line_pattern=visual.boundary_line_type;
            appearance.paper_line_width_mm=paper_width(visual.boundary_weight_pixels,path);
            require(std::isfinite(visual.opacity)&&visual.opacity>=0&&visual.opacity<=1,path,"invalid fill opacity");
            if(visual.hatch=="diagonal")appearance.style.fill_pattern="hatch";
            else if(visual.hatch=="cross"||visual.hatch=="horizontal"||visual.hatch=="dots")appearance.style.fill_pattern=visual.hatch;
            else require(visual.hatch=="none",path+"/hatch","unsupported source hatch pattern");
            state.overrides.push_back(std::move(appearance));
            for(const bool name:{true,false}) {
                PresentationOverride role;role.target_kind=name?"area_name":"area_calculation";role.target_id=area->second->id;
                role.style=text_style("Arial",name?visual.name_size_metres:visual.calculation_size_metres,visual.label_color,path);
                role.visible=name?visual.show_name:visual.show_calculation;
                const auto position=name?assignment.name_position_metres:assignment.calculation_position_metres;
                if(position){finite_point(*position,path);role.plan_label_offset=Vec2{position->x-anchor.x,position->y-anchor.y};}
                state.overrides.push_back(std::move(role));
            }
            budget.record(3,path);provenance_records[area->second->id]=provenance(assignment.source);
        }
        for(std::size_t i=0;i<page.texts.size();++i) {
            const auto& input=page.texts[i];const auto& path=input.source.json_pointer;
            source_reference(input.source,p,"texts");finite_point(input.position_metres,path);rotation(input.rotation_radians,path);
            budget.charge(budget.labels,1,limits.max_labels,path);budget.record(1,path);
            budget.text(input.text,path);budget.text(input.font,path,256);budget.text(input.color,path,256);
            LabelInstance label;label.id=allocate("label:"+std::to_string(i));label.content=input.text;
            label.style=text_style(input.font,input.size_metres,input.color,path);label.style.bold=input.bold;label.style.italic=input.italic;
            label.style.text_alignment=input.alignment;label.model_plan=true;
            label.placement={input.position_metres,input.rotation_radians,1,context.layer_id};
            require(!provenance_records.contains(label.id),path,"duplicate label source");provenance_records[label.id]=provenance(input.source);
            state.labels.push_back(std::move(label));
        }
        for(std::size_t i=0;i<page.symbols.size();++i) {
            const auto& input=page.symbols[i];const auto& path=input.source.json_pointer;
            source_reference(input.source,p,"symbols");finite_point(input.centre_metres,path);rotation(input.rotation_radians,path);
            positive_size(input.width_metres,path);positive_size(input.depth_metres,path);budget.text(input.kind,path,256);
            budget.charge(budget.symbols,1,limits.max_symbols,path);budget.record(1,path);
            require((input.door_hinge=="left"||input.door_hinge=="right")&&(input.door_side==1||input.door_side==-1),path,"invalid door presentation orientation");
            if(input.wall_reference) {
                const auto& ref=*input.wall_reference;const auto target=verified.sources.find(ref.target.identity);
                require((ref.type=="calc"||ref.type=="interior")&&target!=verified.sources.end()&&
                    same_source(target->second->source,ref.target)&&ref.target.page_index==p&&std::isfinite(ref.parameter)&&
                    ref.parameter>=0&&ref.parameter<=1,path+"/wallRef","invalid visual wall association");
                const auto model=decode_measurement_linework_model(candidate.entities().at(target->second->stroke_id).properties.at("model"));
                require(model.supported()&&replay_measurement_linework(*model.model).edges.front().segment.sweep_radians==0,
                    path+"/wallRef","visual association requires straight measured linework");
                const auto& lane=ref.type=="calc"?verified.geometry.pages[p].calculation_stroke_ids:verified.geometry.pages[p].interior_stroke_ids;
                require(std::find(lane.begin(),lane.end(),target->second->stroke_id)!=lane.end(),path+"/wallRef","visual association targets wrong page lane");
            }
            const auto binding=binding_index.find(input.kind);
            if(binding==binding_index.end()){diagnostic(path,"unresolved_symbol_kind","No reviewed bundled SVG binding for source kind '"+input.kind+"'; original source retained.");continue;}
            const auto& saved=*binding->second;SymbolInstance symbol;symbol.id=allocate("symbol:"+std::to_string(i));
            budget.text(saved.fidelity_note,path);
            if(std::any_of(saved.fidelity_note.begin(),saved.fidelity_note.end(),[](unsigned char c){
                return c!=' '&&c!='\t'&&c!='\r'&&c!='\n';}))
                diagnostic(path,"symbol_artwork_fidelity",saved.fidelity_note);
            symbol.symbol_id=saved.definition.id;symbol.definition=saved.definition;symbol.pinned_svg=saved.pinned_svg;
            symbol.width_scale=input.width_metres/saved.definition.width_metres;symbol.depth_scale=input.depth_metres/saved.definition.depth_metres;
            symbol.placement={input.centre_metres,input.rotation_radians,1,context.layer_id};
            symbol.flip_horizontal=input.mirror_x;symbol.flip_vertical=input.mirror_y;
            if(saved.door_transform==PincDoorArtworkTransform::swing_left_positive_svg_y)
                symbol.flip_horizontal=input.mirror_x!=(input.door_hinge=="right");
            if(saved.door_transform==PincDoorArtworkTransform::swing_left_positive_svg_y ||
               saved.door_transform==PincDoorArtworkTransform::side_positive_svg_y)
                symbol.flip_vertical=input.mirror_y!=(input.door_side==-1);
            if(saved.door_transform==PincDoorArtworkTransform::symmetric_source_garage) {
                symbol.flip_horizontal=false;symbol.flip_vertical=false;
            }
            require(std::isfinite(saved.opening_anchor_fraction.x)&&std::isfinite(saved.opening_anchor_fraction.y)&&
                std::abs(saved.opening_anchor_fraction.x)<=.5&&std::abs(saved.opening_anchor_fraction.y)<=.5,
                path,"invalid canonical symbol anchor");
            const Vec2 offset{saved.opening_anchor_fraction.x*input.width_metres*(symbol.flip_horizontal?-1:1),
                saved.opening_anchor_fraction.y*input.depth_metres*(symbol.flip_vertical?-1:1)};
            const auto cosine=std::cos(input.rotation_radians),sine=std::sin(input.rotation_radians);
            symbol.placement.position={input.centre_metres.x-(cosine*offset.x-sine*offset.y),
                input.centre_metres.y-(sine*offset.x+cosine*offset.y)};
            finite_point(symbol.placement.position,path);
            require(symbol.width_scale>=saved.definition.minimum_scale&&symbol.width_scale<=saved.definition.maximum_scale&&
                symbol.depth_scale>=saved.definition.minimum_scale&&symbol.depth_scale<=saved.definition.maximum_scale,
                path,"source physical symbol dimensions exceed saved native transform limits");
            budget.charge(budget.svg,symbol.pinned_svg.size(),limits.max_pinned_svg_bytes,path);
            auto metadata=provenance(input.source);metadata["source_kind"]=input.kind;
            metadata["door_hinge"]=input.door_hinge;metadata["door_side"]=input.door_side;
            metadata["fidelity_note"]=saved.fidelity_note;
            if(input.wall_reference) {
                const auto& ref=*input.wall_reference;const auto target=verified.sources.find(ref.target.identity);
                require((ref.type=="calc"||ref.type=="interior")&&target!=verified.sources.end()&&
                    same_source(target->second->source,ref.target)&&ref.target.page_index==p&&std::isfinite(ref.parameter)&&
                    ref.parameter>=0&&ref.parameter<=1,path+"/wallRef","invalid visual wall association");
                const auto& lane=ref.type=="calc"?verified.geometry.pages[p].calculation_stroke_ids:verified.geometry.pages[p].interior_stroke_ids;
                require(std::find(lane.begin(),lane.end(),target->second->stroke_id)!=lane.end(),path+"/wallRef","visual association targets wrong page lane");
                metadata["visual_wall_reference"]={{"stroke_id",target->second->stroke_id},{"segment_id",target->second->segment_id},
                    {"parameter",target->second->reversed?1-ref.parameter:ref.parameter},{"source_parameter",ref.parameter},
                    {"source_pointer",ref.target.json_pointer},{"source_identity",ref.target.identity},{"type",ref.type}};
            }
            provenance_records[symbol.id]=std::move(metadata);state.symbols.push_back(std::move(symbol));
        }
        budget.record(1,page.source.json_pointer);
        auto carrier=make_annotation_entity(allocate("annotations"),state,
            AnnotationEntityContext{context.property_id,context.building_id,context.floor_id,context.layer_id,{}});
        carrier.extensions["pinc_presentation"]={{"version",1},{"page_index",p},{"source_pointer",page.source.json_pointer},
            {"source_identity",page.source.identity},{"children",std::move(provenance_records)}};
        result.entities.push_back(std::move(carrier));
    }
    ApplyEntityChanges command{candidate.revision(),{}, {},"Prepare detached Pinc presentation"};
    for(const auto& entity:result.entities)command.entity_changes.push_back(EntityChange::upsert(entity));
    try{(void)Document::preview_command(candidate,command);}catch(const std::exception& e){fail("/native_candidate",e.what());}
    return result;
}
} // namespace sketch
