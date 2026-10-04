#include "sketch/wall_split.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/room_relationships.hpp"
#include "support/noninteractive_errors.hpp"
#include <sqlite3.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct OwnedDirectory {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("vertex-wall-split-storage-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    OwnedDirectory(){std::filesystem::create_directory(path);}
    ~OwnedDirectory(){std::error_code error;std::filesystem::remove_all(path,error);}
};
void check_snapshot(const sketch::DocumentSnapshot& snapshot,const std::filesystem::path& root,int index,
                    int native_version=37,int exchange_version=35){
    using namespace sketch;
    require(ProjectStore::required_format_version(snapshot)==native_version,"retained split and logical wall semantics require their exact native floor");
    const auto file=root/(std::to_string(index)+".bldproj");
    (void)ProjectStore::save(file,snapshot);
    auto loaded=ProjectStore::load(file);
    const auto reopened=loaded.document.snapshot();
    require(reopened.entities()==snapshot.entities() && reopened.history().size()==snapshot.history().size(),"native37 restores exact state and history");
    for(std::size_t revision=0;revision<snapshot.history().size();++revision){
        const auto& expected=snapshot.history()[revision];const auto& actual=reopened.history()[revision];
        require(expected.entities==actual.entities && expected.action==actual.action,"native37 restores every retained state");
        if(expected.boundary_constraint_changes)require(actual.boundary_constraint_changes &&
            command_to_json(*expected.boundary_constraint_changes)==command_to_json(*actual.boundary_constraint_changes),"native37 restores exact typed split proof");
    }
    const auto extraction=root/("extract-"+std::to_string(index));
    extract_project(snapshot,extraction);
    std::ifstream input(extraction/"project.json");
    const auto encoded=nlohmann::json::parse(input);
    require(encoded.at("exchange_version")==exchange_version,"logical export advertises the exact retained semantic floor");
    require(encoded.at("revisions").size()==snapshot.history().size(),"logical export retains all revisions");
    sqlite3* database=nullptr;
    require(sqlite3_open(file.string().c_str(),&database)==SQLITE_OK,"controlled storage test opens its exact fixture");
    const auto code=sqlite3_exec(database,"PRAGMA user_version=36; UPDATE metadata SET value='36' WHERE key='format_version'",nullptr,nullptr,nullptr);
    sqlite3_close(database);
    require(code==SQLITE_OK,"controlled reader-floor mutation succeeds");
    const auto before=ProjectStore::file_sha256(file);
    bool refused=false;
    try{(void)ProjectStore::load(file);}catch(const StorageError&){refused=true;}
    require(refused,"lowering both markers cannot admit split semantics");
    require(ProjectStore::file_sha256(file)==before,"rejected old reader leaves original bytes unchanged");
}
}
int main(){
    sketch::testing::noninteractive_errors();
    try{
        using namespace sketch;
        OwnedDirectory temporary;
        Entity wall{"wall","wall",{{"baseline",{{"start",{0,0}},{"end",{10,0}},{"sweep_radians",0}}},
            {"thickness_m",0.2},{"height_m",3},{"elevation_m",0},{"classification","partition"}}};
        const auto relationships=RoomRelationshipSnapshot::create({{"wall",RoomReferenceKind::architectural_wall}},{});
        Entity relationship_entity{"relationships","room_relationships",{{"model",relationships.to_json()},{"vendor","retain"}},false,{{"vendor","retain"}}};
        for(const bool extension : {false,true}) {
            auto unsupported=relationship_entity;
            (extension ? unsupported.extensions : unsupported.properties)["vendor_reference"]={{"wall_members",{"wall","other"}}};
            auto guarded=Document::create({wall,unsupported});
            const auto before=guarded.snapshot();bool refused=false;
            try{(void)make_wall_split_command(before,{"wall","second",0.4,"seam",{}});}catch(const std::invalid_argument&){refused=true;}
            require(refused && guarded.snapshot().entities()==before.entities(),"handled room model hid an unsupported whole-wall reference or mutated source");
        }
        auto related=Document::create({wall,relationship_entity});
        related.apply(make_wall_split_command(related.snapshot(),{"wall","second",0.4,"seam",{}}));
        const auto logical=related.snapshot().entities().at("relationships");
        require(logical.properties.at("model").at("schema_version")==2 &&
            logical.properties.at("model").at("references").at(0).at("wall_members")==nlohmann::json::array({"wall","second"}),
            "wall split must retain the whole logical wall as ordered physical members");
        require(logical.properties.at("vendor")=="retain" && logical.extensions==relationship_entity.extensions,
            "room-reference split must preserve unrelated model entity metadata");
        check_snapshot(related.snapshot(),temporary.path,10,38,36);
        related.apply(make_wall_split_command(related.snapshot(),{"second","third",0.5,"seam-two",{}}));
        const auto recursive=related.snapshot();
        require(recursive.entities().at("relationships").properties.at("model").at("references").at(0).at("wall_members")==
            nlohmann::json::array({"wall","second","third"}),"re-splitting a physical member lost logical traversal order");
        check_snapshot(recursive,temporary.path,11,38,36);
        related.undo(related.revision());related.redo(related.revision());
        require(related.snapshot().entities()==recursive.entities(),"recursive logical wall split did not Undo/Redo exactly");
        related.undo(related.revision());related.undo(related.revision());
        require(related.snapshot().entities().at("relationships")==relationship_entity,"logical membership split Undo lost original schema one bytes");
        check_snapshot(related.snapshot(),temporary.path,12,38,36);
        for(const auto kind:{RoomRelationKind::follows,RoomRelationKind::derived_from,RoomRelationKind::independent}) {
            auto curve=wall;curve.properties["baseline"]["sweep_radians"]=1.0;
            const auto room=encode_identified_boundary_entity(IdentifiedBoundary{"room","room_boundary",{
                {"ab","a","b",{{0,1},{10,1},0}},{"bc","b","c",{{10,1},{10,4},0}},
                {"cd","c","d",{{10,4},{0,4},0}},{"da","d","a",{{0,4},{0,1},0}}}});
            const auto model=RoomRelationshipSnapshot::create({{"wall",RoomReferenceKind::architectural_wall},
                {"room",RoomReferenceKind::room_boundary}},{{"room","wall",kind}});
            auto graph=relationship_entity;graph.properties["model"]=model.to_json();
            auto curved=Document::create({curve,room,graph});
            const auto original_graph=curved.snapshot().entities().at("relationships");
            curved.apply(make_wall_split_command(curved.snapshot(),{"wall","second",0.3,"seam",{}}));
            curved.apply(make_wall_split_command(curved.snapshot(),{"wall","interior",0.5,"second-seam",{}}));
            const auto retained=curved.snapshot();
            const auto migrated=RoomRelationshipSnapshot::from_json(retained.entities().at("relationships").properties.at("model"));
            require(migrated.relations()==model.relations() && room_reference_wall_ids(migrated.references().back())==
                std::vector<std::string>{"wall","interior","second"},"curved recursive split altered relation kinds or native member order");
            require(retained.entities().at("relationships").extensions==original_graph.extensions &&
                retained.entities().at("relationships").properties.at("vendor")=="retain","curved split lost unrelated relationship metadata");
            check_snapshot(retained,temporary.path,20+static_cast<int>(kind),38,36);
            curved.undo(curved.revision());curved.undo(curved.revision());
            require(curved.snapshot().entities().at("relationships")==original_graph,"curved logical split Undo changed original graph");
        }
        auto document=Document::create({wall});
        document.apply(make_wall_split_command(document.snapshot(),{"wall","second",0.4,"seam",{}}));
        const auto split=document.snapshot();
        check_snapshot(split,temporary.path,0);
        auto removed=Document::fork(split);
        removed.apply(ApplyEntityChanges{removed.revision(),{EntityChange::erase("seam"),EntityChange::erase("wall"),EntityChange::erase("second")},{},"Remove split pieces"});
        check_snapshot(removed.snapshot(),temporary.path,1);
        require(document.undo(document.revision()),"split Undo is available");
        check_snapshot(document.snapshot(),temporary.path,2);
        std::vector<Entity> pieces;for(const auto& [id,entity]:split.entities())pieces.push_back(entity);
        check_snapshot(Document::create(pieces).snapshot(),temporary.path,3);
        Entity owner{"outline","measurement_boundary",{{"boundary",nlohmann::json::array({
            {{"start",{0,0}},{"end",{4,0}},{"sweep_radians",0}},
            {{"start",{4,0}},{"end",{4,3}},{"sweep_radians",0}},
            {{"start",{4,3}},{"end",{0,3}},{"sweep_radians",0}},
            {{"start",{0,3}},{"end",{0,0}},{"sweep_radians",0}}})}}};
        owner=upgrade_legacy_boundary_entity(owner);
        const auto geometry=decode_identified_boundary_entity(owner);
        BoundaryDimension dimension{"span","outline",geometry.segments[0].segment_id,{1,-1}};
        dimension.segment_chain_ids={geometry.segments[0].segment_id,geometry.segments[1].segment_id};
        check_snapshot(Document::create({owner,encode_boundary_dimension_entity(dimension)}).snapshot(),temporary.path,4);
        std::cout<<"wall_split_storage_tests passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<"wall_split_storage_tests: "<<error.what()<<'\n';return 1;}
}
