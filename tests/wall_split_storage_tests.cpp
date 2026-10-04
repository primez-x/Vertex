#include "sketch/wall_split.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_dimension.hpp"
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
void check_snapshot(const sketch::DocumentSnapshot& snapshot,const std::filesystem::path& root,int index){
    using namespace sketch;
    require(ProjectStore::required_format_version(snapshot)==37,"all retained split semantics require native37");
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
    require(encoded.at("exchange_version")==35,"logical export advertises exchange35");
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
