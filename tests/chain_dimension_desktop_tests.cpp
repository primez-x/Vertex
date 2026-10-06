#include "sketch/desktop/main_window.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/document_digest.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <cmath>
#include <functional>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void require_near(double actual, double expected) {
    require(std::abs(actual-expected)<1e-9, "chain quantity must equal independent analytical length");
}
Entity stroke(const std::string& id, const std::vector<Vec2>& points, bool curved=false, bool closed=false) {
    MeasurementLinework model; model.stroke_id=id; model.anchor=points.front(); model.closed=closed;
    model.extensions={{"opaque",{{"keep",42}}}};
    for (std::size_t i=1;i<points.size();++i) {
        ConstructionReceipt receipt; receipt.segment_id=id+":e"+std::to_string(i);
        receipt.start=points[i-1]; receipt.chord_end=points[i];
        receipt.kind=curved&&i==2 ? BoundaryConstructionKind::arc_chord_angle : BoundaryConstructionKind::line_to_point;
        if (curved&&i==2) receipt.angle=parse_angle("90 deg");
        model.edges.push_back({receipt.segment_id,id+":v"+std::to_string(i-1),
            closed&&i+1==points.size()?id+":v0":id+":v"+std::to_string(i),receipt});
    }
    return {id,"measurement_linework",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},
        {"model",encode_measurement_linework_model(model)}},true};
}
std::shared_ptr<Document> fixture() {
    return std::make_shared<Document>(Document::create({{"p","property",Json::object(),false},
        {"b","building",{{"property_id","p"}},false},{"f","floor",{{"building_id","b"}},false},
        {"l","layer",{{"floor_id","f"}},false},
        stroke("mixed",{{0,0},{3,0},{4,1},{4,3}},true),
        stroke("square",{{10,0},{12,0},{12,2},{10,2},{10,0}},false,true)},
        {Asset::create("opaque-asset","application/octet-stream",{std::byte{3},std::byte{7}},{{"vendor","retain"}})}));
}
template<class T> T& control(QWidget& owner,const char* name) {
    auto* value=owner.findChild<T*>(name);
    if (!value) throw std::runtime_error(std::string("real chain creator control must exist: ") + name);
    return *value;
}
void choose(QComboBox& box,const QString& id) {
    const auto index=box.findData(id); require(index>=0,"stable chain source must be offered");
    box.setCurrentIndex(index); QApplication::processEvents();
}
void prepare(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen); window.resize(1200,800); window.setMetricUnits(true);
    window.show(); QApplication::processEvents();
}
void creator(MainWindow& window,const std::function<void(QDialog&)>& callback) {
    std::exception_ptr failure; bool opened=false;
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QDialog*>("dimensionCreatorDialog");
        try { require(dialog,"actual Add dimensions modal opens"); opened=true;
            dialog->setAttribute(Qt::WA_DontShowOnScreen); callback(*dialog);
        } catch (...) { failure=std::current_exception(); }
        if(dialog) dialog->reject();
    });
    auto& action=control<QAction>(window,"dimensionCreator"); require(action.isEnabled(),"creator is available"); action.trigger();
    if(failure) std::rethrow_exception(failure); require(opened,"modal test callback executed");
}
BoundaryDimension dimension(const DocumentSnapshot& source,const QString& id) {
    const auto decoded=decode_boundary_dimension_entity(source.entities().at(id.toStdString()));
    require(decoded.supported(),"persisted chain decodes"); return *decoded.dimension;
}
std::vector<BoundaryDimension> dimensions(const DocumentSnapshot& source) {
    std::vector<BoundaryDimension> result;
    for(const auto& [id,entity]:source.entities()) { (void)id;
        if(entity.type=="dimension") result.push_back(*decode_boundary_dimension_entity(entity).dimension);
    } return result;
}
void unchanged(const DocumentSnapshot& before,const DocumentSnapshot& after) {
    require(document_snapshot_digest(before)==document_snapshot_digest(after),"refusal preserves complete history, assets, identity and revision");
}
void atomic(MainWindow& window,const DocumentSnapshot& before,const DocumentSnapshot& after) {
    require(after.revision()==before.revision()+1&&after.history().size()==before.history().size()+1,"one chain operation is one command and revision");
    require(before.assets()==after.assets(),"chain operation preserves asset bytes and metadata");
    require(window.undoCommand(),"chain command undoes"); const auto undone=window.document().snapshot();
    require(undone.entities()==before.entities()&&undone.assets()==before.assets(),"undo restores exact entity and asset maps");
    require(undone.revision()==after.revision()+1 && undone.history().size()==after.history().size()+1 &&
        undone.history().back().action=="undo" && undone.history().back().source_revision==before.revision(),
        "undo appends one exact-source navigation record");
    require(window.redoCommand(),"chain command redoes");
    const auto redone=window.document().snapshot();
    require(redone.document_id()==after.document_id() && redone.entities()==after.entities() && redone.assets()==after.assets(),
        "redo restores exact chain source maps");
    require(redone.revision()==after.revision()+2 && redone.history().size()==after.history().size()+2 &&
        redone.history().back().action=="redo" && redone.history().back().source_revision==after.revision(),
        "redo appends one exact-committed-source navigation record");
}
void api_lifecycle() {
    MainWindow window(fixture()); prepare(window);
    const auto initial=window.document().snapshot();
    for(const QStringList& ids: {QStringList{"mixed:e1"},QStringList{"mixed:e1","absent"},
        QStringList{"mixed:e1","mixed:e3"},QStringList{"mixed:e1","mixed:e1"},QStringList{"mixed:e2","mixed:e1"}}) {
        require(window.createChainDimension("mixed",ids,{5,5}).isEmpty(),"invalid chain references must refuse");
        unchanged(initial,window.document().snapshot());
    }
    require(window.createChainDimension("absent",{"mixed:e1","mixed:e2"},{5,5}).isEmpty(),"missing source refuses");
    unchanged(initial,window.document().snapshot());
    require(window.createChainDimension("mixed",{"mixed:e1","mixed:e2"},{5,5},initial.revision()+1).isEmpty(),"stale expected revision refuses");
    unchanged(initial,window.document().snapshot());
    const auto id=window.createChainDimension("mixed",{"mixed:e1","mixed:e2"},{5,5},initial.revision());
    require(!id.isEmpty(),"real API creates straight plus arc chain"); const auto created=window.document().snapshot();
    auto chain=dimension(created,id); require_near(chain.resolve(created).segment_length_metres,3+std::numbers::pi/2);
    require(chain.segment_chain_ids==std::vector<std::string>{"mixed:e1","mixed:e2"}&&chain.placement==BoundaryDimensionPlacement::manual,
        "chain retains ordered IDs and manual position");
    atomic(window,initial,created);
    auto entity=created.entities().at(id.toStdString()); entity.extensions["vendor"]={{"literal","retain-chain"}};
    window.document().apply(ApplyEntityChanges{window.document().revision(),{EntityChange::upsert(entity)}, {},"Retain opaque chain metadata"});
    const auto before_style=window.document().snapshot();
    require(window.editBoundaryDimension(id,"6 m","7 m","4.2","#713ba2",true,false,true,"0"),"chain presentation edits through real UI API");
    const auto styled=window.document().snapshot(); atomic(window,before_style,styled);
    require(styled.entities().at(id.toStdString()).extensions==entity.extensions,"style preserves opaque metadata");
    chain=dimension(styled,id); require(chain.text_position.x==6&&chain.text_position.y==7,"manual position persists");
    const auto before_vertex=window.document().snapshot();
    require(window.selectEntity("mixed")&&window.moveSelectedBoundaryVertex("mixed:v0",{-1,0},before_vertex.revision()),"real vertex edit changes chain geometry");
    const auto moved=window.document().snapshot(); require_near(dimension(moved,id).resolve(moved).segment_length_metres,4+std::numbers::pi/2);
    require(moved.entities().at(id.toStdString())==styled.entities().at(id.toStdString()),"vertex edit retains exact placed chain entity");
    atomic(window,before_vertex,moved);
    const auto before_resize=window.document().snapshot();
    require(window.editSelectedBoundaryEdgeLength("mixed:e1","6 m",BoundaryFixedEndpoint::end,false,before_resize.revision()),
        "real exact length entry changes chain source while retaining the adjoining arc endpoint");
    const auto resized=window.document().snapshot(); require_near(dimension(resized,id).resolve(resized).segment_length_metres,6+std::numbers::pi/2);
    require(resized.entities().at(id.toStdString())==moved.entities().at(id.toStdString()),"length entry preserves chain placement and opaque metadata");
    atomic(window,before_resize,resized);
    const auto wrap=window.createChainDimension("square",{"square:e4","square:e1"},{9,-1});
    require(!wrap.isEmpty(),"closed boundary wraparound chain creates"); require_near(dimension(window.document().snapshot(),wrap).resolve(window.document().snapshot()).segment_length_metres,4);
    const auto saved=window.document().snapshot(); QTemporaryDir directory; require(directory.isValid(),"isolated save directory");
    const auto path=directory.filePath("chains.bldproj"); require(window.saveProjectAs(path)&&window.createNewProject(),"save chains and release writer lease");
    MainWindow reopened; prepare(reopened); require(reopened.openProject(path)&&reopened.document().is_editable(),"chain archive reopens editable");
    const auto restored=reopened.document().snapshot();
    require(restored.entities()==saved.entities()&&restored.assets()==saved.assets()&&restored.history().size()==saved.history().size(),"reopen retains full chain maps, assets and command count");
    require(reopened.undoCommand()&&reopened.document().snapshot().entities()==resized.entities()&&reopened.redoCommand()&&reopened.document().snapshot().entities()==saved.entities(),"reopened chain UndoRedo restores exact maps");
}
void select_chain(QDialog& dialog,const QString& owner,const QString& first,const QString& last) {
    choose(control<QComboBox>(dialog,"dimensionSourceBoundary"),owner);
    choose(control<QComboBox>(dialog,"dimensionCreateKind"),"chain");
    choose(control<QComboBox>(dialog,"dimensionFirstSegment"),first);
    choose(control<QComboBox>(dialog,"dimensionSecondSegment"),last);
}
void ui_lifecycle() {
    MainWindow window(fixture()); prepare(window); require(window.selectEntity("mixed"),"select measured chain source");
    const auto before=window.document().snapshot();
    creator(window,[&](QDialog& dialog) { select_chain(dialog,"mixed","mixed:e1","mixed:e2"); });
    unchanged(before,window.document().snapshot());
    creator(window,[&](QDialog& dialog) {
        select_chain(dialog,"mixed","mixed:e1","mixed:e2");
        auto* preview_widget=dynamic_cast<PlanCanvas*>(&control<QWidget>(dialog,"dimensionChainPreview"));
        require(preview_widget,"chain preview is the real plan canvas"); auto& preview=*preview_widget;
        if (preview.entities().size()!=2 || preview.entities()[1].segments.size()!=2 || preview.labels().size()!=2)
            throw std::runtime_error(QStringLiteral("real highlighted path has selected two edges and edge labels: entities=%1, path edges=%2, labels=%3, summary=%4")
                .arg(preview.entities().size())
                .arg(preview.entities().size()>1 ? preview.entities()[1].segments.size() : 0)
                .arg(preview.labels().size())
                .arg(control<QLabel>(dialog,"dimensionChainSummary").text()).toStdString());
        require(preview.labels()[0].id=="mixed:e1"&&preview.labels()[1].id=="mixed:e2"&&
            preview.labels()[0].text=="1"&&preview.labels()[1].text=="2","highlight edge labels preserve source order");
        require(preview.entities()[0].type=="measurement_linework"&&preview.entities()[0].segments.size()==3,
            "open measured source preview must retain all measured edges");
        require_near(preview.entities()[1].segments[1].sweep_radians,std::numbers::pi/2);
        require(control<QLabel>(dialog,"dimensionChainSummary").text().contains("2 edges"),"summary reports selected edge count");
        auto& save=control<QPushButton>(dialog,"addLengthDimension"); require(save.text()=="Add chain length"&&save.isEnabled(),"real chain save action is offered");
        save.click(); auto first=window.document().snapshot(); require(first.revision()==before.revision()+1,"modal save commits once");
        auto list=dimensions(first); require(list.size()==1&&list[0].placement==BoundaryDimensionPlacement::manual,"automatic initial chain placement persists as manual");
        require_near(list[0].resolve(first).segment_length_metres,3+std::numbers::pi/2);
        select_chain(dialog,"square","square:e4","square:e1");
        control<QCheckBox>(dialog,"dimensionAutomaticPlacement").setChecked(false);
        control<QLineEdit>(dialog,"dimensionCreateX").setText("8"); control<QLineEdit>(dialog,"dimensionCreateY").setText("-2");
        save.click(); const auto second=window.document().snapshot(); list=dimensions(second);
        require(list.size()==2&&second.revision()==first.revision()+1,"second chain in same modal refreshes retained source");
        for(const auto& item:list) if(item.boundary_id=="square") { require_near(item.resolve(second).segment_length_metres,4);
            require(item.segment_chain_ids==std::vector<std::string>{"square:e4","square:e1"}&&item.text_position.x==8&&item.text_position.y==-2,"modal keeps wraparound order and metric position"); }
        const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if(!directory.isEmpty()) require(QDir().mkpath(directory)&&dialog.grab().save(QDir(directory).filePath("chain-dimension-creator.png")),"capture actual chain dialog");
    });
}
void modal_source_guards() {
    for(int change=0;change<3;++change) {
        MainWindow window(fixture()); prepare(window); require(window.selectEntity("mixed"),"guard source selection");
        const auto source=window.document().snapshot();
        creator(window,[&](QDialog& dialog) {
            select_chain(dialog,"mixed","mixed:e1","mixed:e2");
            if(change<2) {
                auto altered=source; auto& record=const_cast<std::vector<RevisionRecord>&>(altered.history()).front();
                if(change==0) record.entities.at("mixed").extensions["replacement"]="same identity/revision";
                else record.assets.at("opaque-asset")=Asset::create("opaque-asset","application/octet-stream",{std::byte{9}},{{"vendor","changed"}});
                window.document()=Document::fork(altered);
                require(window.document().snapshot().document_id()==source.document_id()&&window.document().revision()==source.revision(),"replacement really retains identity and revision");
                require(document_snapshot_digest(window.document().snapshot())!=document_snapshot_digest(source),"replacement changes full source digest");
            } else window.setWorkspace(Workspace::architectural);
            const auto changed=window.document().snapshot(); control<QPushButton>(dialog,"addLengthDimension").click();
            unchanged(changed,window.document().snapshot()); require(dimensions(window.document().snapshot()).empty(),"stale modal creates no dimension");
            require(!control<QLabel>(dialog,"dimensionCreatorStatus").text().isEmpty(),"stale modal supplies refusal status");
        });
    }
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors(); QStandardPaths::setTestModeEnabled(true); QApplication app(argc,argv);
    QCoreApplication::setApplicationName("Vertex-chain-dimensions-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    try { api_lifecycle(); ui_lifecycle(); modal_source_guards(); }
    catch(const std::exception& error) { std::cerr<<"chain_dimension_desktop_tests: "<<error.what()<<'\n'; return 1; }
    std::cout<<"Chain dimension desktop tests passed\n"; return 0;
}
