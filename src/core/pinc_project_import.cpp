#include "sketch/pinc_project_import.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {
using Json=nlohmann::json;
constexpr double feet_to_metres=.3048;
[[noreturn]] void fail(std::string_view pointer, std::string_view message) {
    throw std::invalid_argument("Pinc import "+std::string(pointer.empty()?"/":pointer)+": "+std::string(message));
}
std::string token(std::string_view text) {
    std::string result;for(char c:text) { if(c=='~')result+="~0";else if(c=='/')result+="~1";else result+=c; }return result;
}
std::string child(std::string_view path, std::string_view field) { return std::string(path)+"/"+token(field); }
void object(const Json& value, std::string_view path) { if(!value.is_object())fail(path,"expected object"); }
void array(const Json& value, std::string_view path) { if(!value.is_array())fail(path,"expected array"); }
const Json& field(const Json& value, std::string_view key, std::string_view path) {
    const auto it=value.find(std::string(key));if(it==value.end())fail(child(path,key),"required field missing");return *it;
}
double number(const Json& value, std::string_view path) {
    if(!value.is_number())fail(path,"expected finite number");const double n=value.get<double>();
    if(!std::isfinite(n))fail(path,"expected finite number");return n;
}
double numeric(const Json& value, const char* key, std::string_view path, double fallback) {
    const auto it=value.find(key);return it==value.end()?fallback:number(*it,child(path,key));
}
double positive(double value, std::string_view path) { if(!(value>0))fail(path,"must be positive");return value; }
double length_metres(double feet, std::string_view path, bool must_be_positive=false) {
    const double n=feet*feet_to_metres;
    if(!std::isfinite(n) || std::abs(n)>1e6)fail(path,"length or coordinate exceeds one million metres");
    if(must_be_positive)positive(n,path);return n;
}
std::string string(const Json& value, std::string_view path, bool nonempty=false) {
    if(!value.is_string())fail(path,"expected string");auto s=value.get<std::string>();
    if(s.find('\0')!=std::string::npos || (nonempty&&s.empty()))fail(path,"invalid string");return s;
}
std::string text(const Json& value, const char* key, std::string_view path, std::string fallback, bool nonempty=false) {
    const auto it=value.find(key);return it==value.end()?fallback:string(*it,child(path,key),nonempty);
}
bool boolean(const Json& value, const char* key, std::string_view path, bool fallback) {
    const auto it=value.find(key);if(it==value.end())return fallback;
    if(!it->is_boolean())fail(child(path,key),"expected boolean");return it->get<bool>();
}
std::string choice(std::string value, std::initializer_list<const char*> options, std::string_view path) {
    for(const auto* option:options)if(value==option)return value;fail(path,"unsupported presentation value");
}
std::string color(std::string value, std::string_view path) {
    if(value.size()!=7 || value.front()!='#')fail(path,"expected #RRGGBB color");
    for(std::size_t i=1;i<value.size();++i)if(!((value[i]>='0'&&value[i]<='9') ||
        (value[i]>='a'&&value[i]<='f') || (value[i]>='A'&&value[i]<='F')))fail(path,"expected #RRGGBB color");return value;
}
double unit_interval(double value, std::string_view path) { if(value<0 || value>1)fail(path,"must be between zero and one");return value; }
Vec2 point(const Json& value, std::string_view path) {
    object(value,path);return {length_metres(number(field(value,"x",path),child(path,"x")),child(path,"x")),
        -length_metres(number(field(value,"y",path),child(path,"y")),child(path,"y"))};
}
Vec2 xy(const Json& value, std::string_view path) { return point(value,path); }
std::optional<Vec2> optional_point(const Json& value, const char* key, std::string_view path) {
    const auto it=value.find(key);if(it==value.end() || it->is_null())return {};return point(*it,child(path,key));
}
std::string canonical_scalar(const Json& value) {
    // JSON has one numeric scalar category; JS source identity does not
    // distinguish integral 1.0 from 1 or negative numeric zero from zero.
    if(value.is_number_float()) {
        const double n=value.get<double>();
        if(std::floor(n)==n && std::abs(n)<=9007199254740991.0)return Json(static_cast<std::int64_t>(n)).dump();
    }
    return value.dump();
}
std::optional<std::string> scalar_id(const Json& value, std::string_view path) {
    const auto it=value.find("id");if(it==value.end() || it->is_null())return {};
    if(!it->is_string() && !it->is_number() && !it->is_boolean())fail(child(path,"id"),"identity must be scalar");
    if(it->is_string()) { const auto s=string(*it,child(path,"id"),true);if(s.size()>4096)fail(child(path,"id"),"source identity exceeds 4096 bytes"); }
    if(it->is_number())(void)number(*it,child(path,"id"));return canonical_scalar(*it);
}
std::string reference_scalar(const Json& value, std::string_view path) {
    if(!value.is_string() && !value.is_number() && !value.is_boolean())fail(path,"reference identity must be scalar");
    if(value.is_string())(void)string(value,path,true);if(value.is_number())(void)number(value,path);return canonical_scalar(value);
}
void validate_limits(const PincImportLimits& l) {
    const PincImportLimits hard;
    if(l.max_bytes>hard.max_bytes || l.max_json_depth>hard.max_json_depth || l.max_json_nodes>hard.max_json_nodes ||
       l.max_string_bytes>hard.max_string_bytes || l.max_pages>hard.max_pages ||
       l.max_calculation_edges_per_page>hard.max_calculation_edges_per_page ||
       l.max_interior_edges_per_page>hard.max_interior_edges_per_page || l.max_total_edges>hard.max_total_edges ||
       l.max_records>hard.max_records || l.max_calculation_pairs>hard.max_calculation_pairs)
        fail("/","caller limits exceed hard ceilings");
}
// SAX allocates no DOM. Key sets, depth, strings and finite numbers are checked
// before a second bounded pass constructs the detached JSON representation.
class Preflight final:public nlohmann::json_sax<Json> {
public:
    explicit Preflight(const PincImportLimits& limits):limits_(limits){}
    bool null() override{return scalar();}bool boolean(bool) override{return scalar();}
    bool number_integer(number_integer_t) override{return scalar();}
    bool number_unsigned(number_unsigned_t) override{return scalar();}
    bool number_float(number_float_t n,const string_t&) override{if(!std::isfinite(n))fail("/","non-finite JSON number");return scalar();}
    bool string(string_t& s) override{charge_string(s);return scalar();}
    bool binary(binary_t&) override{return false;}
    bool start_object(std::size_t) override{return start();}
    bool start_array(std::size_t) override{return start();}
    bool key(string_t& s) override {
        charge_string(s);if(keys_.empty() || !keys_.back().insert(s).second)fail("/","duplicate JSON object key");return true;
    }
    bool end_object() override{keys_.pop_back();return true;}bool end_array() override{keys_.pop_back();return true;}
    bool parse_error(std::size_t,const std::string&,const nlohmann::detail::exception&) override{return false;}
private:
    bool scalar(){if(++nodes_>limits_.max_json_nodes)fail("/","JSON node budget exceeded");return true;}
    bool start(){if(keys_.size()>=limits_.max_json_depth)fail("/","JSON depth budget exceeded");scalar();keys_.emplace_back();return true;}
    void charge_string(const std::string& s) {
        if(s.size()>limits_.max_string_bytes-strings_)fail("/","aggregate JSON string budget exceeded");
        strings_+=s.size();if(s.find('\0')!=std::string::npos)fail("/","NUL in JSON string");
    }
    const PincImportLimits& limits_;std::size_t nodes_{},strings_{};
    std::vector<std::set<std::string,std::less<>>> keys_;
};
struct Category { const char* code;const char* name;const char* color; };
constexpr Category categories[]={
    {"GLA1","First Floor","#c9edf0"},{"GLA2","Second Floor","#cfe7f8"},{"GLA3","Third Floor","#d8e5fa"},{"GLA4","Fourth Floor","#e1e4fb"},
    {"GBA","Gross Building Area","#dbe9d5"},{"BSMT-F","Finished Basement","#d9e6f7"},{"BSMT-U","Unfinished Basement","#e8ecef"},
    {"GAR","Garage","#d5ead0"},{"DGAR","Detached Garage","#cfe2c9"},{"ADU","Accessory Dwelling / ADU","#d9e5f4"},
    {"OUT","Shed / Outbuilding","#e7e0cf"},{"CAR","Carport","#e4efd7"},{"PORCH","Porch","#f1d9df"},{"PATIO","Patio","#f4d2dc"},
    {"DECK","Wood Deck","#eadbbd"},{"BALC","Balcony","#f0e2c8"},{"STG","Storage","#dde7ea"},{"LOW","Low Ceiling / Non-GLA","#f5e3d3"},
    {"OPEN","Open to Below","#f0eeee"},{"NCA","Non-Calculated Area","#e8e8e8"},{"SITE","Subject Site","#e6ead7"},{"UND","Undefined / Clear","#eeeeee"}};
const Category* category(std::string_view code) { for(const auto& c:categories)if(c.code==code)return &c;return nullptr; }
bool equal_point(Vec2 a,Vec2 b){return a.x==b.x&&a.y==b.y;}
bool equivalent(const Segment& a,const Segment& b) {
    return (equal_point(a.start,b.start)&&equal_point(a.end,b.end)&&a.sweep_radians==b.sweep_radians) ||
           (equal_point(a.start,b.end)&&equal_point(a.end,b.start)&&a.sweep_radians==-b.sweep_radians);
}
bool near_pair(const Segment& a,const Segment& b) {
    const auto close_point=[](Vec2 x,Vec2 y){return std::hypot(x.x-y.x,x.y-y.y)<=.08*feet_to_metres;};
    return (close_point(a.start,b.start)&&close_point(a.end,b.end)) || (close_point(a.start,b.end)&&close_point(a.end,b.start));
}
class Parser {
public:
    explicit Parser(const PincImportLimits& l):limits_(l){}
    PincImportProject run(const Json& root) {
        object(root,"");const auto& raw_version=field(root,"version","");
        if(raw_version.is_string())result_.source_version=string(raw_version,"/version",true);
        else if(raw_version.is_number())result_.source_version=canonical_scalar(raw_version);
        else fail("/version","expected source version string or legacy numeric version");
        const auto& version=result_.source_version;
        const bool legacy=version.size()>2 && version.starts_with("2.") &&
            std::all_of(version.begin()+2,version.end(),[](char c){return (c>='0'&&c<='9')||c=='.';}) &&
            version.back()>='0'&&version.back()<='9';
        if(version=="4.2") {
            if(!raw_version.is_string())fail("/version","modern saved version must be string 4.2");
            if(string(field(root,"format",""),"/format")!="PincSketch")fail("/format","unsupported project format");
            result_.dialect=PincImportDialect::modern_v42;
        } else if(legacy) {
            result_.dialect=PincImportDialect::legacy_v2;
            if(!raw_version.is_string())diagnostic("/version","legacy_numeric_version","Inspected legacy loading stringifies this numeric 2.* version; original representation is retained in source.");
            if(root.contains("format") && string(root.at("format"),"/format")!="PincSketch")fail("/format","unsupported project format");
        } else fail("/version","unsupported Pinc saved schema; known versions are 4.2 and inspected 2.*");
        if(root.contains("fileName"))result_.source_file_name=string(root.at("fileName"),"/fileName");
        if(root.contains("uidCounter")) {
            const auto& n=root.at("uidCounter");if(!n.is_number_unsigned() && !(n.is_number_integer()&&n.get<std::int64_t>()>=0))
                fail("/uidCounter","expected nonnegative integer");
        }
        if(root.contains("pages")) {
            const auto& pages=root.at("pages");array(pages,"/pages");
            if(pages.empty() || pages.size()>limits_.max_pages)fail("/pages","page count exceeds budget or is empty");
            for(std::size_t i=0;i<pages.size();++i)result_.pages.push_back(read_page(pages[i],i,"/pages/"+std::to_string(i),legacy));
        } else {
            if(!legacy)fail("/pages","required field missing");if(limits_.max_pages<1)fail("/pages","page budget exceeded");
            result_.pages.push_back(read_page(root,0,"",true,true));
        }
        if(root.contains("currentPage")) {
            const auto& n=root.at("currentPage");if(!n.is_number_unsigned() && !(n.is_number_integer()&&n.get<std::int64_t>()>=0))
                fail("/currentPage","expected nonnegative integer");
            const auto index=n.get<std::uint64_t>();if(index>=result_.pages.size())fail("/currentPage","page index out of range");
            result_.current_page=static_cast<std::size_t>(index);
        }
        if(root.contains("pages") || !legacy)unknown(root,"",{"format","version","uidCounter","pages","currentPage","fileName"});
        return std::move(result_);
    }
private:
    void record(std::string_view path) { if(records_>=limits_.max_records)fail(path,"emitted record budget exceeded");++records_; }
    void edge_charge(std::string_view path) { if(edges_>=limits_.max_total_edges)fail(path,"aggregate edge budget exceeded");++edges_; }
    void diagnostic(std::string path,std::string code,std::string message) {
        record(path);result_.diagnostics.push_back({std::move(path),std::move(code),std::move(message)});
    }
    void unknown(const Json& value,std::string_view path,std::initializer_list<const char*> known) {
        for(const auto& [key,item]:value.items()) {
            (void)item;bool found=false;for(const auto* allowed:known)if(key==allowed){found=true;break;}
            if(!found)diagnostic(child(path,key),"unsupported_source_field","Field is retained only in the original source asset; it has no native authority.");
        }
    }
    PincSourceReference source(const Json& v,std::size_t page,std::string collection,std::string path,
        std::set<std::string,std::less<>>* ids=nullptr) {
        const auto id=scalar_id(v,path);
        if(id && ids && !ids->insert(*id).second)fail(child(path,"id"),"duplicate source identity within collection");
        PincSourceReference result{page,std::move(collection),id,std::move(path),{}};
        result.identity=pinc_source_identity(result);return result;
    }
    PincDimensionPresentation dimension(const Json& v,std::string_view path,PincDimensionPresentation fallback) {
        fallback.size_metres=length_metres(numeric(v,"dimSize",path,fallback.size_metres/feet_to_metres),child(path,"dimSize"),true);
        fallback.font=text(v,"dimFont",path,std::move(fallback.font),true);
        fallback.color=color(text(v,"dimColor",path,std::move(fallback.color)),child(path,"dimColor"));return fallback;
    }
    PincImportSegment segment(const Json& v,std::size_t index,std::string collection,std::string path,
        const PincDimensionPresentation& page_dim,bool interior,bool legacy,std::set<std::string,std::less<>>& ids) {
        object(v,path);record(path);edge_charge(path);PincImportSegment r;
        r.source=source(v,index,std::move(collection),path,&ids);
        const auto a=point(field(v,"a",path),child(path,"a")),b=point(field(v,"b",path),child(path,"b"));
        r.source_kind=legacy?text(v,"kind",path,"line"):string(field(v,"kind",path),child(path,"kind"));
        choice(r.source_kind,{"line","arc"},child(path,"kind"));
        const double sagitta=!legacy && r.source_kind=="arc"?
            number(field(v,"bulge",path),child(path,"bulge")):numeric(v,"bulge",path,0);
        r.source_sagitta_metres=length_metres(sagitta,child(path,"bulge"));
        const double chord=std::hypot(b.x-a.x,b.y-a.y);
        if(chord==0)fail(path,"segment endpoints must be distinct");
        if(r.source_kind=="arc" && std::abs(sagitta)>=.001 && chord>=.001*feet_to_metres) {
            try {
                r.geometry=arc_from_chord_height(a,b,r.source_sagitta_metres);const auto bounds=segment_bounds(r.geometry);
                if(std::abs(bounds.minimum.x)>1e6 || std::abs(bounds.minimum.y)>1e6 ||
                    std::abs(bounds.maximum.x)>1e6 || std::abs(bounds.maximum.y)>1e6)fail(path,"arc extents exceed one million metres");
            }
            catch(const std::exception& e){fail(path,e.what());}
        } else r.geometry={a,b,0};
        unknown(field(v,"a",path),child(path,"a"),{"x","y"});
        unknown(field(v,"b",path),child(path,"b"),{"x","y"});
        auto& p=r.presentation;p.color=color(text(v,"color",path,interior?"#222222":"#1f5f94"),child(path,"color"));
        p.weight_pixels=positive(numeric(v,"weight",path,interior?1.4:2),child(path,"weight"));
        p.line_type=choice(text(v,"lineType",path,"solid"),{"solid","dash","dot","dashdot"},child(path,"lineType"));
        p.show_dimension=interior?boolean(v,"showDim",path,false):!boolean(v,"dimHidden",path,false);
        p.dimension=dimension(v,path,page_dim);if(interior && !v.contains("dimColor"))p.dimension.color="#555555";
        p.dimension_offset_metres=optional_point(v,"dimOffset",path);
        if(v.contains("sharedColor"))p.shared_color=color(string(v.at("sharedColor"),child(path,"sharedColor")),child(path,"sharedColor"));
        if(v.contains("sharedWeight"))p.shared_weight_pixels=positive(number(v.at("sharedWeight"),child(path,"sharedWeight")),child(path,"sharedWeight"));
        if(v.contains("sharedLineType"))p.shared_line_type=choice(string(v.at("sharedLineType"),child(path,"sharedLineType")),{"solid","dash","dot","dashdot"},child(path,"sharedLineType"));
        unknown(v,path,{"id","a","b","kind","bulge","dimOffset","color","weight","lineType","showDim","dimHidden","dimSize","dimFont","dimColor","sharedColor","sharedWeight","sharedLineType","source","angleDeg","quadrant"});
        if(v.contains("source") || v.contains("angleDeg") || v.contains("quadrant"))
            diagnostic(path,"source_authoring_hint_retained","Tentative authoring hints are retained in the original source; analytical coordinates remain the import input.");
        return r;
    }
    const Json& collection(const Json& v,const char* key,std::string_view path,bool objects=false) {
        static const Json empty_array=Json::array(),empty_object=Json::object();
        const auto it=v.find(key);if(it==v.end())return objects?empty_object:empty_array;
        if(objects)object(*it,child(path,key));else array(*it,child(path,key));return *it;
    }
    void pair_charge(std::size_t count,std::string_view path) {
        const auto pairs=static_cast<std::uint64_t>(count)*(count?count-1:0)/2;
        if(pairs>limits_.max_calculation_pairs-pairs_)fail(path,"aggregate calculation graph pair budget exceeded");pairs_+=pairs;
    }
    PincImportAssignment assignment(const Json& v,PincSourceReference ref,bool legacy) {
        const auto path=ref.json_pointer;object(v,path);record(path);PincImportAssignment r;r.source=std::move(ref);
        r.code=string(field(v,"code",path),child(path,"code"),true);const auto* c=category(r.code);r.known_category=c!=nullptr;
        r.name=text(v,"name",path,c?c->name:r.code);if(!c)diagnostic(path,"unknown_area_category","Unknown classification retained descriptively; no appraisal facts are inferred.");
        auto& p=r.presentation;p.color=color(text(v,"color",path,c?c->color:"#eeeeee"),child(path,"color"));
        const bool living=r.code=="GLA1" || r.code=="GLA2" || r.code=="GLA3" || r.code=="GLA4";
        p.opacity=unit_interval(numeric(v,"opacity",path,living?.22:.15),child(path,"opacity"));
        p.hatch=choice(text(v,"hatch",path,"none"),{"none","diagonal","cross","horizontal","dots"},child(path,"hatch"));
        p.label_color=color(text(v,"labelColor",path,"#173f62"),child(path,"labelColor"));
        p.name_size_metres=length_metres(numeric(v,"nameSize",path,.78),child(path,"nameSize"),true);
        p.calculation_size_metres=length_metres(numeric(v,"calcSize",path,.65),child(path,"calcSize"),true);
        p.show_name=boolean(v,"showName",path,true);p.show_calculation=boolean(v,"showCalc",path,true);
        p.sync_boundary_color=boolean(v,"syncBoundaryColor",path,true);
        p.boundary_color=color(text(v,"boundaryColor",path,p.color),child(path,"boundaryColor"));
        p.boundary_weight_pixels=positive(numeric(v,"boundaryWeight",path,2),child(path,"boundaryWeight"));
        p.boundary_line_type=choice(text(v,"boundaryLineType",path,"solid"),{"solid","dash","dot","dashdot"},child(path,"boundaryLineType"));
        r.name_position_metres=optional_point(v,"namePos",path);r.calculation_position_metres=optional_point(v,"calcPos",path);
        r.cached_anchor_metres=optional_point(v,"_anchor",path);
        if(v.contains("_area")) {
            const double n=number(v.at("_area"),child(path,"_area"));if(n<0 || n>1e12/(feet_to_metres*feet_to_metres))fail(child(path,"_area"),"invalid cached area");
            r.cached_area_square_metres=n*feet_to_metres*feet_to_metres;
        }
        if(legacy) {
            const auto legacy_pos=[&](const char* x,const char* y)->std::optional<Vec2>{
                if(!v.contains(x)&&!v.contains(y))return {};
                return Vec2{length_metres(number(field(v,x,path),child(path,x)),child(path,x)),
                    -length_metres(number(field(v,y,path),child(path,y)),child(path,y))};};
            if(v.contains("nameX")||v.contains("nameY"))r.name_position_metres=legacy_pos("nameX","nameY");
            if(v.contains("calcX")||v.contains("calcY"))r.calculation_position_metres=legacy_pos("calcX","calcY");
        } else {
            for(const auto* key:{"segments","nameX","nameY","calcX","calcY"})if(v.contains(key))
                diagnostic(child(path,key),"unsupported_source_field","Legacy-only assignment field retained in original source, not used as modern authority.");
        }
        unknown(v,path,{"id","code","name","color","opacity","hatch","labelColor","nameSize","calcSize","showName","showCalc","syncBoundaryColor","boundaryColor","boundaryWeight","boundaryLineType","namePos","calcPos","_anchor","_area","segments","nameX","nameY","calcX","calcY"});
        return r;
    }
    void modern_assignments(const Json& v,PincImportPage& pg,std::size_t index,std::string_view path) {
        const auto& values=collection(v,"assignments",path,true);
        std::map<std::string,std::vector<PincSourceReference>,std::less<>> ids;
        for(const auto& s:pg.calculation_segments)if(s.source.scalar_id_json) {
            const auto scalar=Json::parse(*s.source.scalar_id_json);
            const auto key=scalar.is_string()?scalar.get<std::string>():scalar.dump();ids[key].push_back(s.source);
        }
        for(const auto& [key,value]:values.items()) {
            if(key.size()>32768)fail(child(path,"assignments"),"assignment key exceeds 32768 bytes");
            const auto p=child(child(path,"assignments"),key);
            PincSourceReference ref{index,"assignments",Json(key).dump(),p,{}};ref.identity=pinc_source_identity(ref);
            auto r=assignment(value,std::move(ref),false);
            r.face_key=key;bool resolved=!key.empty();std::set<std::string,std::less<>> used;
            std::size_t start=0;
            for(;;) {
                const auto end=key.find('|',start);const auto part=key.substr(start,end==std::string::npos?end:end-start);
                const auto found=ids.find(part);
                if(part.empty() || !used.insert(part).second || found==ids.end() || found->second.size()!=1)resolved=false;
                else { record(p); r.source_segment_references.push_back(found->second.front()); }
                if(end==std::string::npos)break;start=end+1;
            }
            r.references_resolved=resolved;
            diagnostic(p,resolved?"assignment_requires_exact_correspondence":"assignment_unmatched_source_key",
                resolved?"Source key identifies edges, not verified face topology; exact reviewed correspondence is required.":
                "Source key is unmatched or ambiguous; assignment is retained for explicit review.");
            pg.assignments.push_back(std::move(r));
        }
    }
    void legacy_areas(const Json& v,PincImportPage& pg,std::size_t index,std::string_view path) {
        const auto& values=collection(v,"areas",path);std::set<std::string,std::less<>> area_ids,segment_ids;
        std::size_t raw_count=0;
        for(std::size_t i=0;i<values.size();++i) {
            const auto p=child(path,"areas")+"/"+std::to_string(i);object(values[i],p);record(p);
            PincLegacyArea area;area.source=source(values[i],index,"areas",p,&area_ids);
            const auto& segments=collection(values[i],"segments",p);
            if(segments.size()>limits_.max_calculation_edges_per_page-raw_count)fail(child(p,"segments"),"calculation source count exceeds page budget");
            raw_count+=segments.size();
            for(std::size_t j=0;j<segments.size();++j) {
                auto original=segment(segments[j],index,"areas/"+std::to_string(i)+"/segments",
                    child(p,"segments")+"/"+std::to_string(j),pg.dimension,false,true,segment_ids);
                // Legacy segment IDs are allowed to recur across original areas,
                // but not within an area's segment collection.
                bool shared=false,near_conflict=false;
                for(auto& admitted:pg.calculation_segments) {
                    if(legacy_pairs_>=limits_.max_calculation_pairs)fail(p,"aggregate legacy correspondence work budget exceeded");++legacy_pairs_;
                    if(equivalent(original.geometry,admitted.geometry)) {
                        record(original.source.json_pointer);
                        admitted.equivalent_sources.push_back(original.source);shared=true;
                        diagnostic(original.source.json_pointer,"legacy_exact_edge_shared","Exact analytical duplicate retained as another source of the first edge; original presentation stays inspectable.");
                        break;
                    }
                    if(near_pair(original.geometry,admitted.geometry))near_conflict=true;
                }
                if(!shared) {
                    record(original.source.json_pointer);edge_charge(original.source.json_pointer);
                    pg.calculation_segments.push_back(original);
                    if(near_conflict)diagnostic(original.source.json_pointer,"legacy_near_edge_conflict",
                        "Legacy endpoint-tolerance deduplication would lose distinct analytical geometry; both sources are retained.");
                }
                area.segments.push_back(std::move(original));
            }
            if(values[i].contains("code")) {
                auto r=assignment(values[i],area.source,true);r.legacy_area_index=pg.legacy_areas.size();
                for(const auto& s:area.segments) { record(p); r.source_segment_references.push_back(s.source); }
                r.references_resolved=true;
                diagnostic(p,"legacy_assignment_requires_review","Original analytic area order is retained; sampled-centroid reassignment is not applied.");
                pg.assignments.push_back(std::move(r));
            } else unknown(values[i],p,{"id","name","segments"});
            pg.legacy_areas.push_back(std::move(area));segment_ids.clear();
        }
    }
    void segments(const Json& v,PincImportPage& pg,std::size_t index,std::string_view path,bool interior,bool legacy) {
        const auto* key=interior?(legacy?"interiors":"interiorWalls"):"calcWalls";
        const auto& values=collection(v,key,path);
        const auto ceiling=interior?limits_.max_interior_edges_per_page:limits_.max_calculation_edges_per_page;
        if(values.size()>ceiling)fail(child(path,key),"segment count exceeds page budget");
        std::set<std::string,std::less<>> ids;auto& target=interior?pg.interior_segments:pg.calculation_segments;
        for(std::size_t i=0;i<values.size();++i)target.push_back(segment(values[i],index,key,
            child(path,key)+"/"+std::to_string(i),pg.dimension,interior,legacy,ids));
    }
    void symbols(const Json& v,PincImportPage& pg,std::size_t index,std::string_view path,bool legacy) {
        const auto& values=collection(v,"symbols",path);std::set<std::string,std::less<>> ids;
        struct Target { PincSourceReference source;bool curved{}; };
        std::map<std::string,std::vector<Target>,std::less<>> calculation_targets,interior_targets;
        const auto add_targets=[](const std::vector<PincImportSegment>& edges,auto& targets) {
            for(const auto& edge:edges) {
                const auto add=[&](const PincSourceReference& ref) {
                    if(ref.scalar_id_json)targets[*ref.scalar_id_json].push_back({ref,edge.geometry.sweep_radians!=0});};
                add(edge.source);for(const auto& ref:edge.equivalent_sources)add(ref);
            }
        };
        add_targets(pg.calculation_segments,calculation_targets);add_targets(pg.interior_segments,interior_targets);
        for(std::size_t i=0;i<values.size();++i) {
            const auto p=child(path,"symbols")+"/"+std::to_string(i);const auto& o=values[i];object(o,p);record(p);
            PincImportSymbol r;r.source=source(o,index,"symbols",p,&ids);r.kind=string(field(o,"kind",p),child(p,"kind"),true);
            r.centre_metres=xy(o,p);
            const auto dim=[&](const char* modern,const char* old,double fallback) {
                if(o.contains(modern))return number(o.at(modern),child(p,modern));
                if(legacy)return numeric(o,old,p,fallback);return number(field(o,modern,p),child(p,modern));};
            r.width_metres=length_metres(dim("w","width",3),child(p,"w"),true);
            r.depth_metres=length_metres(dim("h","depth",2),child(p,"h"),true);
            const double degrees=numeric(o,"rot",p,legacy?numeric(o,"rotation",p,0):0);
            r.rotation_radians=-degrees/180*std::numbers::pi;
            if(!std::isfinite(r.rotation_radians) || std::abs(r.rotation_radians)>1e6)fail(child(p,"rot"),"rotation cannot be represented");
            r.mirror_x=boolean(o,"mirrorX",p,false);r.mirror_y=boolean(o,"mirrorY",p,false);
            r.door_hinge=choice(text(o,"doorHinge",p,"left"),{"left","right"},child(p,"doorHinge"));
            const double side=numeric(o,"doorSide",p,1);if(side!=1&&side!=-1)fail(child(p,"doorSide"),"door side must be -1 or 1");r.door_side=static_cast<int>(side);
            if(o.contains("wallRef") && !o.at("wallRef").is_null()) {
                const auto q=child(p,"wallRef");const auto& w=o.at("wallRef");object(w,q);
                PincVisualWallReference ref;ref.type=choice(string(field(w,"type",q),child(q,"type")),{"calc","interior"},child(q,"type"));
                const auto id=reference_scalar(field(w,"id",q),child(q,"id"));
                const auto& targets=ref.type=="calc"?calculation_targets:interior_targets;const auto found=targets.find(id);
                if(found==targets.end())fail(child(q,"id"),"visual wall reference target missing");
                if(found->second.size()!=1)fail(child(q,"id"),"ambiguous visual wall reference");
                if(found->second.front().curved)fail(child(q,"id"),"visual wall reference requires straight linework");
                ref.target=found->second.front().source;ref.parameter=unit_interval(numeric(w,"t",q,.5),child(q,"t"));r.wall_reference=std::move(ref);
                unknown(w,q,{"type","id","t"});
            }
            unknown(o,p,{"id","kind","x","y","w","h","rot","mirrorX","mirrorY","doorHinge","doorSide","wallRef","width","depth","rotation"});
            if(!legacy)for(const auto* key:{"width","depth","rotation"})if(o.contains(key))
                diagnostic(child(p,key),"unsupported_source_field","Legacy-only symbol alias retained in original source.");
            pg.symbols.push_back(std::move(r));
        }
    }
    void texts(const Json& v,PincImportPage& pg,std::size_t index,std::string_view path,bool legacy) {
        const auto& values=collection(v,"texts",path);std::set<std::string,std::less<>> ids;
        for(std::size_t i=0;i<values.size();++i) {
            const auto p=child(path,"texts")+"/"+std::to_string(i);const auto& o=values[i];object(o,p);record(p);
            PincImportText r;r.source=source(o,index,"texts",p,&ids);r.position_metres=xy(o,p);
            if(o.contains("text"))r.text=string(o.at("text"),child(p,"text"));
            else if(legacy)r.text=text(o,"value",p,"");else fail(child(p,"text"),"required field missing");
            r.size_metres=length_metres(numeric(o,"size",p,.75),child(p,"size"),true);
            r.rotation_radians=-numeric(o,"rot",p,0)/180*std::numbers::pi;
            if(!std::isfinite(r.rotation_radians) || std::abs(r.rotation_radians)>1e6)fail(child(p,"rot"),"rotation cannot be represented");
            r.color=color(text(o,"color",p,"#163d63"),child(p,"color"));r.font=text(o,"font",p,"Segoe UI",true);
            r.alignment=choice(text(o,"align",p,"center"),{"left","center","right"},child(p,"align"));
            r.bold=boolean(o,"bold",p,false);r.italic=boolean(o,"italic",p,false);
            unknown(o,p,{"id","text","value","x","y","size","rot","color","font","align","bold","italic"});
            if(!legacy && o.contains("value"))diagnostic(child(p,"value"),"unsupported_source_field","Legacy-only text alias retained in original source.");
            pg.texts.push_back(std::move(r));
        }
    }
    void underlay(const Json& v,PincImportPage& pg,std::string_view path) {
        if(!v.contains("underlay") || v.at("underlay").is_null())return;
        const auto p=child(path,"underlay");const auto& o=v.at("underlay");object(o,p);record(p);
        PincImportUnderlay r;r.source_pointer=p;r.top_left_metres=xy(o,p);
        r.width_metres=length_metres(number(field(o,"width",p),child(p,"width")),child(p,"width"),true);
        r.opacity=unit_interval(number(field(o,"opacity",p),child(p,"opacity")),child(p,"opacity"));
        const auto data=string(field(o,"data",p),child(p,"data"),true);
        if(data.starts_with("data:")) {
            const auto comma=data.find(',');const auto descriptor=data.substr(5,comma==std::string::npos?comma:comma-5);
            const std::string suffix=";base64";
            if(descriptor.ends_with(suffix)) {
                r.mime_type=descriptor.substr(0,descriptor.size()-suffix.size());
                r.supported_raster_descriptor=r.mime_type=="image/png" || r.mime_type=="image/jpeg" || r.mime_type=="image/bmp" || r.mime_type=="image/tiff";
            }
            if(r.supported_raster_descriptor) {
                if(comma==std::string::npos || comma+1==data.size() || (data.size()-comma-1)%4!=0)fail(child(p,"data"),"invalid raster base64 descriptor");
                std::size_t padding=0;
                for(std::size_t i=comma+1;i<data.size();++i) {
                    const char c=data[i];if(c=='=') {if(++padding>2 || i<data.size()-2)fail(child(p,"data"),"invalid base64 padding");}
                    else if(padding || !((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='+'||c=='/'))
                        fail(child(p,"data"),"invalid base64 descriptor");
                }
                r.data_url=data;
            }
        }
        if(!r.supported_raster_descriptor)diagnostic(child(p,"data"),"underlay_unsupported_descriptor",
            "External, SVG, or unsupported raster descriptor is retained only in original source; no fetching or decoding is performed.");
        unknown(o,p,{"data","x","y","width","opacity"});pg.underlay=std::move(r);
    }
    PincImportPage read_page(const Json& v,std::size_t index,std::string path,bool legacy,bool root_page=false) {
        object(v,path);record(path);PincImportPage pg;pg.source=source(v,index,"pages",path);
        pg.name=text(v,"name",path,"Page "+std::to_string(index+1));pg.dimension=dimension(v,path,{});
        pg.ghost_previous=boolean(v,legacy?"ghostPrev":"ghostPrevious",path,false);
        pg.show_print_guide=boolean(v,"showPrintGuide",path,true);
        if(legacy)legacy_areas(v,pg,index,path);else segments(v,pg,index,path,false,false);
        pair_charge(pg.calculation_segments.size(),child(path,legacy?"areas":"calcWalls"));
        segments(v,pg,index,path,true,legacy);
        if(!legacy)modern_assignments(v,pg,index,path);
        symbols(v,pg,index,path,legacy);texts(v,pg,index,path,legacy);underlay(v,pg,path);
        if(root_page)unknown(v,path,{"id","name","areas","interiors","symbols","texts","underlay","ghostPrev","showPrintGuide","dimSize","dimFont","dimColor","version","format","currentPage","uidCounter","fileName"});
        else if(legacy)unknown(v,path,{"id","name","areas","interiors","symbols","texts","underlay","ghostPrev","showPrintGuide","dimSize","dimFont","dimColor"});
        else unknown(v,path,{"id","name","calcWalls","interiorWalls","assignments","symbols","texts","underlay","ghostPrevious","showPrintGuide","dimSize","dimFont","dimColor"});
        return pg;
    }
    const PincImportLimits& limits_;PincImportProject result_;std::size_t records_{},edges_{};std::uint64_t pairs_{},legacy_pairs_{};
};
} // namespace
void validate_pinc_import_limits(const PincImportLimits& limits) { validate_limits(limits); }
std::string pinc_source_identity(const PincSourceReference& source) {
    if(source.page_index>=PincImportLimits{}.max_pages || source.collection.empty() || source.collection.size()>512 ||
        source.json_pointer.size()>131072)fail(source.json_pointer,"invalid source identity components");
    if(source.scalar_id_json) {
        if(source.scalar_id_json->size()>65536)fail(source.json_pointer,"source scalar identity exceeds budget");
        PincImportLimits scalar_limits;scalar_limits.max_bytes=65536;scalar_limits.max_string_bytes=65536;
        const auto scalar=parse_pinc_json_bounded(std::span(reinterpret_cast<const std::byte*>(source.scalar_id_json->data()),source.scalar_id_json->size()),scalar_limits);
        if((!scalar.is_string()&&!scalar.is_number()&&!scalar.is_boolean()) ||
            (scalar.is_string()&&(scalar.get_ref<const std::string&>().empty() || scalar.get_ref<const std::string&>().find('\0')!=std::string::npos)) ||
            (scalar.is_number_float()&&!std::isfinite(scalar.get<double>())) ||
            canonical_scalar(scalar)!=*source.scalar_id_json)fail(source.json_pointer,"source scalar identity is not canonical");
    }
    return std::to_string(source.page_index)+"/"+source.collection+"/"+
        (source.scalar_id_json?*source.scalar_id_json:"@"+source.json_pointer);
}
Json parse_pinc_json_bounded(std::span<const std::byte> bytes,const PincImportLimits& limits) {
    validate_limits(limits);if(bytes.empty() || bytes.size()>limits.max_bytes)fail("/","input byte budget exceeded or input empty");
    const auto* begin=reinterpret_cast<const char*>(bytes.data());Preflight preflight(limits);
    if(!Json::sax_parse(begin,begin+bytes.size(),&preflight))fail("/","malformed JSON");
    try { return Json::parse(begin,begin+bytes.size()); }
    catch(const nlohmann::json::exception& e){fail("/",e.what());}
}
PincImportProject parse_pinc_project(std::span<const std::byte> bytes,const PincImportLimits& limits) {
    try { return Parser(limits).run(parse_pinc_json_bounded(bytes,limits)); }
    catch(const nlohmann::json::exception& e){fail("/",e.what());}
}
} // namespace sketch
