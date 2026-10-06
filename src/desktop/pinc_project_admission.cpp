#include "pinc_project_admission.hpp"

#include "sketch/area_type_presets.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/sheet_view_entity_codec.hpp"

#include <QBuffer>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace sketch::desktop {
namespace {
using Json = nlohmann::json;
void require(bool value, const char* message) {
    if (!value) throw std::invalid_argument(message);
}
void portable_basename(const std::string& name) {
    require(!name.empty() && name.size() <= 255 && name != "." && name != ".." &&
        name.back() != '.' && name.back() != ' ' &&
        QString::fromUtf8(name.data(),static_cast<qsizetype>(name.size())).toUtf8().toStdString() == name &&
        std::none_of(name.begin(),name.end(),[](unsigned char c) {
            return c < 32 || c == 127 || std::string_view("/\\:<>\"|?*").find(static_cast<char>(c)) != std::string_view::npos;
        }), "Pinc source name must be a bounded portable basename.");
    auto stem=name.substr(0,name.find('.'));
    std::transform(stem.begin(),stem.end(),stem.begin(),[](unsigned char c) {
        return static_cast<char>(c>='a'&&c<='z'?c-'a'+'A':c);
    });
    require(stem!="CON"&&stem!="PRN"&&stem!="AUX"&&stem!="NUL"&&
        !(stem.size()==4&&(stem.starts_with("COM")||stem.starts_with("LPT"))&&stem[3]>='1'&&stem[3]<='9'),
        "Pinc source name must not use a reserved device basename.");
}
std::vector<std::byte> byte_vector(const QByteArray& value) {
    const auto* start = reinterpret_cast<const std::byte*>(value.constData());
    return {start,start+value.size()};
}
Json measurement_profile() {
    Json classifications = Json::object();
    for (const auto& preset : area_type_presets) if (!preset.classification.empty())
        classifications[std::string(preset.classification)] = {{"building_total",preset.building_total},
            {"living_total",preset.living_total},{"appraisal_category","none"}};
    return {{"id","vertex-pinc-measurement"},{"version",2u},{"display_unit","square_foot"},
        {"decimal_places",2},{"classifications",std::move(classifications)}};
}
void context_properties(Entity& entity,const DrawingContext& context) {
    entity.properties["property_id"] = context.property_id;
    entity.properties["building_id"] = context.building_id;
    entity.properties["floor_id"] = context.floor_id;
    entity.properties["layer_id"] = context.layer_id;
}
void charge(std::uint64_t& total,std::uint64_t count,std::uint64_t maximum) {
    require(count <= maximum && total <= maximum-count,"Pinc underlay aggregate budget exceeded.");
    total += count;
}
PincImportProject verify_source(const PincWorkerProject& worker,std::span<const std::byte> original) {
    require(worker.isolation_controls_attested,"Pinc admission requires the attested isolated worker.");
    auto parsed = parse_pinc_project(original);
    auto supplied = worker.project;
    std::set<std::string> failures;
    supplied.diagnostics.clear();
    for (const auto& diagnostic : worker.project.diagnostics) {
        if (diagnostic.code != "underlay_decode_failed") { supplied.diagnostics.push_back(diagnostic);continue; }
        const auto found = std::find_if(parsed.pages.begin(),parsed.pages.end(),[&](const auto& page) {
            return page.underlay && page.underlay->supported_raster_descriptor &&
                page.underlay->source_pointer == diagnostic.source_pointer;
        });
        require(found != parsed.pages.end() && failures.insert(diagnostic.source_pointer).second &&
            diagnostic.message == "Underlay could not be decoded within supported image budgets; original content is retained.",
            "Pinc worker has an invalid underlay failure diagnostic.");
    }
    require(encode_pinc_import_candidate(parsed) == encode_pinc_import_candidate(supplied),
        "Pinc worker candidate differs from the exact original source.");
    std::set<std::size_t> seen;
    std::uint64_t encoded = 0,pixels = 0;
    for (const auto& decoded : worker.underlays) {
        require(decoded.page_index < parsed.pages.size() && seen.insert(decoded.page_index).second,
            "Pinc decoded underlay has duplicate or invalid page identity.");
        const auto& page = parsed.pages[decoded.page_index];const auto& reference = decoded.reference;
        require(page.underlay && page.underlay->supported_raster_descriptor &&
            !failures.contains(page.underlay->source_pointer) &&
            reference.source == pincUnderlaySourceBytes(*page.underlay) &&
            reference.mime.toStdString() == page.underlay->mime_type && reference.page_count == 1 &&
            reference.source_text.isEmpty() && reference.text_runs.empty() && !reference.image.isNull() &&
            reference.image.format() == QImage::Format_RGBA8888 && reference.image.width() > 0 && reference.image.height() > 0 &&
            reference.image.width() <= referenceDimensionLimit && reference.image.height() <= referenceDimensionLimit,
            "Pinc decoded underlay is inconsistent with its original descriptor.");
        charge(encoded,static_cast<std::uint64_t>(reference.source.size()),pincImageSourceLimit);
        charge(pixels,static_cast<std::uint64_t>(reference.image.width())*reference.image.height(),pincDecodedPixelLimit);
    }
    for (std::size_t p=0;p<parsed.pages.size();++p) {
        const auto& image = parsed.pages[p].underlay;
        if (image && image->supported_raster_descriptor)
            require(seen.contains(p) != failures.contains(image->source_pointer),
                "Pinc worker must account for every supported underlay exactly once.");
    }
    return parsed;
}
struct PageBounds {
    bool present{};double left{},right{},bottom{},top{};
    void point(Vec2 value) {
        if (!present) { left=right=value.x;bottom=top=value.y;present=true; }
        else {left=(std::min)(left,value.x);right=(std::max)(right,value.x);
            bottom=(std::min)(bottom,value.y);top=(std::max)(top,value.y);}
    }
    void segment(const Segment& value) {const auto bounds=segment_bounds(value);point(bounds.minimum);point(bounds.maximum);}
};
std::vector<Entity> exact_source_endpoint_joints(std::span<const Entity> strokes,std::string_view prefix) {
    using Key=std::tuple<std::string,double,double>;
    std::map<Key,std::vector<ConstraintEndpointBinding>> endpoints;
    std::map<std::string,const Entity*,std::less<>> owners;
    for(const auto& stroke:strokes) {
        if(stroke.type!="measurement_linework")continue;
        owners.emplace(stroke.id,&stroke);
        const auto decoded=decode_measurement_linework_model(stroke.properties.at("model"));
        require(decoded.supported(),"Imported source joint requires a supported analytical stroke.");
        const auto layer=stroke.properties.at("layer_id").get<std::string>();
        for(const auto& edge:replay_measurement_linework(*decoded.model).edges) {
            endpoints[{layer,edge.segment.start.x,edge.segment.start.y}].push_back(
                {stroke.id,WallEndpointRole::start,edge.segment_id,edge.start_vertex_id});
            endpoints[{layer,edge.segment.end.x,edge.segment.end.y}].push_back(
                {stroke.id,WallEndpointRole::end,edge.segment_id,edge.end_vertex_id});
        }
    }
    std::vector<Entity> joints;
    // Exact endpoint equality preserves the source topology. No tolerance,
    // cross-page relation, orthogonality or fixed length is inferred.
    for(const auto& [point,bindings]:endpoints) for(std::size_t i=1;i<bindings.size();++i) {
        if(bindings.front().owner_id==bindings[i].owner_id)continue;
        require(joints.size()<PincImportLimits{}.max_records,"Pinc source joint record budget exceeded.");
        auto joint=encode_constraint_entity(PersistentConstraint{std::string(prefix)+":joint:"+std::to_string(joints.size()),
            ConstraintRelationKind::coincident,{bindings.front(),bindings[i]},std::nullopt,std::nullopt});
        const auto& owner=*owners.at(bindings.front().owner_id);
        for(const auto* key:{"property_id","building_id","floor_id","layer_id"})joint.properties[key]=owner.properties.at(key);
        joint.extensions["pinc_joint"]={{"version",1},{"basis","exact_source_endpoints"},
            {"position_m",{std::get<1>(point),std::get<2>(point)}}};
        joints.push_back(std::move(joint));
    }
    return joints;
}
} // namespace

PincProjectAdmission preparePincProjectAdmission(const PincWorkerProject& worker,
    std::vector<std::byte> original_bytes,std::string source_basename,
    std::span<const PincPageFloorChoice> floor_choices,std::span<const PincSymbolBinding> trusted_symbol_bindings,
    std::string_view fresh_namespace) {
    portable_basename(source_basename);
    require(!fresh_namespace.empty() && fresh_namespace.size() <= 99 &&
        std::all_of(fresh_namespace.begin(),fresh_namespace.end(),[](char c) {
            return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';
        }),"Pinc admission requires a fresh ASCII namespace of 1 to 99 bytes.");
    const auto source = verify_source(worker,original_bytes);
    require(floor_choices.size() == source.pages.size(),"An explicit floor choice is required for every Pinc page.");
    const auto prefix = std::string(fresh_namespace);
    const auto property_id = prefix+":property",building_id = prefix+":building";
    std::vector<Entity> scaffold{{property_id,"property",{{"name",source_basename},{"calculation_workflow","measurement"},
        {"calculation_profile",measurement_profile()}}},{building_id,"building",{{"name","Imported building"},{"property_id",property_id}}}};
    std::vector<const PincPageFloorChoice*> choices(source.pages.size());
    std::map<std::size_t,std::pair<std::string,std::string>> floors;
    for (const auto& choice : floor_choices) {
        require(choice.page_index < choices.size() && !choices[choice.page_index] && !choice.floor_name.empty() &&
            choice.floor_name.size() <= 1024 && choice.floor_name.find('\0') == std::string::npos &&
            QString::fromUtf8(choice.floor_name.data(),static_cast<qsizetype>(choice.floor_name.size())).toUtf8().toStdString()==choice.floor_name,
            "Pinc floor choices contain a duplicate page or invalid reviewed name.");
        choices[choice.page_index] = &choice;
        auto found=floors.find(choice.floor_group);
        if (found==floors.end()) {
            const auto id=prefix+":floor:"+std::to_string(floors.size());
            found=floors.emplace(choice.floor_group,std::pair{id,choice.floor_name}).first;
            scaffold.push_back({id,"floor",{{"name",choice.floor_name},{"building_id",building_id},{"property_id",property_id}}});
        }
        require(found->second.second == choice.floor_name,"Pages sharing a floor must agree on its reviewed name.");
    }
    std::vector<PincPageGeometryContext> contexts;
    for (std::size_t p=0;p<source.pages.size();++p) {
        const auto floor_id=floors.at(choices[p]->floor_group).first;
        const auto stem=prefix+":p"+std::to_string(p);
        contexts.push_back({p,{property_id,building_id,floor_id,stem+":calc"},
            {property_id,building_id,floor_id,stem+":interior"}});
        for (const auto& context : {contexts.back().calculation,contexts.back().interior})
            scaffold.push_back({context.layer_id,"layer",{{"floor_id",floor_id},
                {"name",source.pages[p].name+(context.layer_id.ends_with(":calc")?" / Calculation":" / Interior")}}});
    }
    scaffold.push_back(make_annotation_entity(prefix+":annotations",AnnotationState{}));
    auto document=Document::create(std::move(scaffold));const auto base=document.snapshot();
    std::vector<PincAreaReview> reviews;
    for (std::size_t p=0;p<source.pages.size();++p) for(std::size_t a=0;a<source.pages[p].assignments.size();++a) {
        const auto* preset=area_type_for_code(source.pages[p].assignments[a].code);
        reviews.push_back({p,a,preset!=nullptr,preset?(preset->classification.empty()?"non_calculated":std::string(preset->classification)):""});
    }
    // Both admission modules independently reconstruct the same graph. Reserve
    // their combined five plus four passes before either starts any graph work.
    PincGeometryAdmissionLimits measurement_limits;
    PincPresentationAdmissionLimits presentation_limits;
    const auto maximum_pairs=measurement_limits.source.max_calculation_pairs;
    std::uint64_t pairs=0;
    for(const auto& page:source.pages) {
        const auto n=static_cast<std::uint64_t>(page.calculation_segments.size());
        const auto count=n*(n ? n-1 : 0)/2;
        require(count<=maximum_pairs/9 && pairs<=maximum_pairs/9-count,
            "Pinc combined geometry verification pair budget exceeded.");
        pairs+=count;
    }
    measurement_limits.source.max_calculation_pairs=maximum_pairs*5/9;
    presentation_limits.geometry_verification.source.max_calculation_pairs=maximum_pairs-maximum_pairs*5/9;
    measurement_limits.max_correspondence_work/=2;
    presentation_limits.geometry_verification.max_correspondence_work/=2;
    const auto edge_budget=measurement_limits.max_graph_edges;
    measurement_limits.max_graph_edges=edge_budget*5/9;
    presentation_limits.geometry_verification.max_graph_edges=edge_budget-edge_budget*5/9;
    const auto face_budget=measurement_limits.max_face_edge_uses;
    measurement_limits.max_face_edge_uses=face_budget*5/9;
    presentation_limits.geometry_verification.max_face_edge_uses=face_budget-face_budget*5/9;
    const auto measurement=prepare_pinc_measurement_admission(source,base,contexts,prefix+"_g",reviews,measurement_limits);
    ApplyEntityChanges command{base.revision(),{}, {},"Import Pinc project"};
    for (const auto& entity : measurement.entities)command.entity_changes.push_back(EntityChange::upsert(entity));
    const auto measurement_preview=command.entity_changes.empty()?base:Document::preview_command(base,command);
    const auto presentation=prepare_pinc_presentation_admission(source,measurement,measurement_preview,trusted_symbol_bindings,prefix+"_a",presentation_limits);
    for (const auto& entity : presentation.entities)command.entity_changes.push_back(EntityChange::upsert(entity));
    for(auto& joint:exact_source_endpoint_joints(measurement.entities,prefix))
        command.entity_changes.push_back(EntityChange::upsert(std::move(joint)));
    std::vector<PincImportDiagnostic> diagnostics=measurement.diagnostics;
    diagnostics.insert(diagnostics.end(),presentation.diagnostics.begin(),presentation.diagnostics.end());
    for(const auto& diagnostic : worker.project.diagnostics)if(diagnostic.code=="underlay_decode_failed")diagnostics.push_back(diagnostic);
    std::vector<std::string> reference_ids(source.pages.size());
    std::vector<PageBounds> bounds(source.pages.size());
    std::vector<std::vector<std::string>> page_objects(source.pages.size());
    for(std::size_t p=0;p<source.pages.size();++p) {
        for(const auto& segment:source.pages[p].calculation_segments)bounds[p].segment(segment.geometry);
        for(const auto& segment:source.pages[p].interior_segments)bounds[p].segment(segment.geometry);
        for(const auto& text:source.pages[p].texts)bounds[p].point(text.position_metres);
        for(const auto& symbol:source.pages[p].symbols) {
            const auto radius=std::hypot(symbol.width_metres,symbol.depth_metres)/2;
            bounds[p].point({symbol.centre_metres.x-radius,symbol.centre_metres.y-radius});
            bounds[p].point({symbol.centre_metres.x+radius,symbol.centre_metres.y+radius});
        }
    }
    for(const auto& change:command.entity_changes) {
        const auto& entity=change.entity;
        for(std::size_t p=0;p<contexts.size();++p)if(entity.properties.value("layer_id",std::string{})==contexts[p].calculation.layer_id ||
            entity.properties.value("layer_id",std::string{})==contexts[p].interior.layer_id)page_objects[p].push_back(entity.id);
    }
    std::uint64_t raster_asset_bytes=0;
    for(const auto& decoded:worker.underlays) {
        const auto p=decoded.page_index;const auto& image=decoded.reference.image;const auto& descriptor=*source.pages[p].underlay;
        const auto stem=prefix+":p"+std::to_string(p);
        QByteArray png;QBuffer buffer(&png);
        require(buffer.open(QIODevice::WriteOnly)&&image.save(&buffer,"PNG")&&!png.isEmpty(),"Pinc preview PNG encoding failed.");
        charge(raster_asset_bytes,static_cast<std::uint64_t>(png.size())+static_cast<std::uint64_t>(decoded.reference.source.size()),pincReplyLimit);
        const auto source_name="pinc-page-"+std::to_string(p+1)+"."+pincUnderlaySuffix(descriptor).toStdString();
        const auto original=Asset::create(stem+":underlay",descriptor.mime_type,byte_vector(decoded.reference.source),
            {{"source_path",source_name},{"width_px",image.width()},{"height_px",image.height()},{"content","pinc-underlay-original"}});
        const auto preview=Asset::create(stem+":preview","image/png",byte_vector(png),
            {{"source_path",source_name},{"width_px",image.width()},{"height_px",image.height()},{"content","validated-reference-render"}});
        const double scale=descriptor.width_metres/image.width(),height=scale*image.height();
        require(std::isfinite(scale)&&scale>0&&std::isfinite(height)&&height>0,"Pinc underlay has invalid world extent.");
        Entity reference{stem+":reference","reference_asset",{{"asset_id",original.id},{"render_asset_id",preview.id},
            {"source_path",source_name},{"mime_type",descriptor.mime_type},{"source_format",descriptor.mime_type},
            {"position_m",{descriptor.top_left_metres.x+descriptor.width_metres/2,descriptor.top_left_metres.y-height/2}},
            {"metres_per_source_unit",scale},{"scale",1.0},{"rotation_degrees",0.0},{"flip_horizontal",false},{"flip_vertical",true},
            {"intensity",descriptor.opacity},{"visible",true},{"content_mode","traceable-reference"},
            {"editable_extraction",false},{"source_preserved",true},{"fidelity_mode","decoded-raster"},
            {"page_index",0},{"page_count",1},{"source_width_px",image.width()},{"source_height_px",image.height()},
            {"pinc_source_width_metres",descriptor.width_metres}}};
        context_properties(reference,contexts[p].calculation);
        reference_ids[p]=reference.id;page_objects[p].push_back(reference.id);
        bounds[p].point(descriptor.top_left_metres);bounds[p].point({descriptor.top_left_metres.x+descriptor.width_metres,descriptor.top_left_metres.y-height});
        command.entity_changes.push_back(EntityChange::upsert(std::move(reference)));
        command.asset_changes.push_back(AssetChange::upsert(original));command.asset_changes.push_back(AssetChange::upsert(preview));
    }
    std::vector<CoordinatedView> views;std::vector<DrawingSheet> sheets;std::vector<std::string> order;Json pages=Json::array();
    for(std::size_t p=0;p<source.pages.size();++p) {
        const auto stem=prefix+":p"+std::to_string(p);const auto& page=source.pages[p];
        CoordinatedView view;view.id=stem+":view";view.name=page.name;view.object_ids=page_objects[p];view.restrict_to_objects=true;
        double scale=100;
        if(bounds[p].present) {
            auto& b=bounds[p];const double padding=(std::max)(.1,(std::max)(b.right-b.left,b.top-b.bottom)*.05);
            b.left-=padding;b.right+=padding;b.bottom-=padding;b.top+=padding;
            require(b.left>=-1e6&&b.right<=1e6&&b.bottom>=-1e6&&b.top<=1e6,"Pinc content exceeds native sheet crop bounds.");
            view.presentation.crop=ViewCrop{b.left,b.right,b.bottom,b.top};
            scale=(std::max)((b.right-b.left)*1000/190,(b.top-b.bottom)*1000/251);
        }
        DrawingSheet sheet;sheet.id=stem+":sheet";sheet.number=std::to_string(p+1);sheet.width_mm=210;sheet.height_mm=297;
        sheet.title_block.project=source_basename;sheet.title_block.title=page.name;
        // Leave a 10 mm gap above the 26 mm title block, in addition to
        // the top/side margins. Drawing content must not overlap the footer.
        sheet.viewports.push_back({stem+":viewport",view.id,{10,10,190,251},scale});
        order.push_back(sheet.id);views.push_back(std::move(view));sheets.push_back(std::move(sheet));
        pages.push_back({{"page_index",p},{"name",page.name},{"view_id",stem+":view"},{"sheet_id",stem+":sheet"},
            {"floor_id",contexts[p].calculation.floor_id},
            {"calculation_layer_id",contexts[p].calculation.layer_id},{"interior_layer_id",contexts[p].interior.layer_id},
            {"ghost_previous",page.ghost_previous},{"show_print_guide",page.show_print_guide}});
    }
    auto model=SheetViewModel::create(std::move(views),std::move(sheets),{},std::move(order));
    const auto sheet_id=prefix+":sheets",asset_id=prefix+":source";
    Json persisted_diagnostics=Json::array();std::uint64_t diagnostic_bytes=0;
    require(diagnostics.size()<=PincImportLimits{}.max_records,"Pinc diagnostic count exceeds admission budget.");
    for(const auto& diagnostic:diagnostics) {
        charge(diagnostic_bytes,diagnostic.source_pointer.size()+diagnostic.code.size()+diagnostic.message.size(),
            PincImportLimits{}.max_string_bytes);
        persisted_diagnostics.push_back({{"source_pointer",diagnostic.source_pointer},{"code",diagnostic.code},{"message",diagnostic.message}});
    }
    auto sheet_entity=make_sheet_view_entity(sheet_id,model);
    sheet_entity.extensions["pinc_import"]={{"version",1},{"current_page",source.current_page},{"pages",pages},
        {"source_asset_id",asset_id},{"diagnostics",std::move(persisted_diagnostics)}};
    command.entity_changes.push_back(EntityChange::upsert(std::move(sheet_entity)));
    command.asset_changes.push_back(AssetChange::upsert(Asset::create(asset_id,"application/x-pincsketch",std::move(original_bytes),
        {{"source_path",source_basename},{"content","pinc-project-original"},{"current_page",source.current_page},{"pages",std::move(pages)}})));
    (void)Document::preview_command(base,command);document.apply(command);
    return {std::move(document),std::move(contexts),std::move(model),source.current_page,std::move(diagnostics),
        asset_id,sheet_id,std::move(reference_ids)};
}
} // namespace sketch::desktop
