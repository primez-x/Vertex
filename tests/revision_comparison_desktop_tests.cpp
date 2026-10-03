#include "sketch/desktop/main_window.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"
#include "support/trusted_reference_fixture.hpp"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPainter>
#include <QPushButton>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> T& child(QObject& parent, const char* name) {
    auto* value = dynamic_cast<T*>(parent.findChild<QObject*>(QString::fromLatin1(name)));
    if (!value) throw std::runtime_error(std::string("missing native revision comparison control: ") + name);
    return *value;
}
void history_dialog(MainWindow& window, const std::function<void(QDialog&)>& inspect) {
    std::exception_ptr failure;
    bool found{};
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>(QStringLiteral("revisionHistoryDialog"));
        try {
            require(dialog, "actual revision history dialog opens"); found = true;
            dialog->setAttribute(Qt::WA_DontShowOnScreen); inspect(*dialog);
        } catch (...) { failure = std::current_exception(); }
        if (dialog) dialog->reject();
    });
    child<QAction>(window, "revisionHistory").trigger();
    if (failure) std::rethrow_exception(failure);
    require(found, "actual revision history action enters its dialog");
}
void name_revision(MainWindow& window, const QString& name) {
    history_dialog(window, [&](QDialog& dialog) {
        child<QLineEdit>(dialog, "revisionName").setText(name);
        child<QPushButton>(dialog, "nameCurrentRevision").click();
        require(window.document().snapshot().named_revisions().contains(name.toStdString()),
            "actual naming button records the requested revision");
    });
}
void select_state(QComboBox& selector, Revision revision, bool current) {
    for (int index = 0; index < selector.count(); ++index) {
        if (selector.itemData(index).toULongLong() == revision &&
            selector.itemData(index, Qt::UserRole + 1).toBool() == current) {
            selector.setCurrentIndex(index); return;
        }
    }
    throw std::runtime_error("requested named/current state is absent from the actual selector");
}
bool same_point(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }
bool same_geometry(const Boundary& a, const Boundary& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!same_point(a[i].start,b[i].start) || !same_point(a[i].end,b[i].end) ||
            a[i].sweep_radians != b[i].sweep_radians) return false;
    return true;
}
bool same_draft(const BoundaryDraftPreview& a,const BoundaryDraftPreview& b) {
    const auto optional_point=[](const std::optional<Vec2>& left,const std::optional<Vec2>& right) {
        return left.has_value()==right.has_value() && (!left || same_point(*left,*right));
    };
    if (!same_geometry(a.segments,b.segments) || !optional_point(a.anchor,b.anchor) ||
        !optional_point(a.pen_position,b.pen_position) || a.rubber_band.has_value()!=b.rubber_band.has_value() ||
        a.instruction!=b.instruction || a.can_close_on_anchor!=b.can_close_on_anchor ||
        a.length_snap_active!=b.length_snap_active || a.labels.size()!=b.labels.size()) return false;
    if (a.rubber_band && !same_geometry({*a.rubber_band},{*b.rubber_band})) return false;
    for (std::size_t i=0;i<a.labels.size();++i)
        if (!same_point(a.labels[i].position,b.labels[i].position) || a.labels[i].text!=b.labels[i].text) return false;
    return true;
}
const CanvasEntity* entity(const PlanCanvas& canvas, const QString& id) {
    const auto found = std::find_if(canvas.entities().begin(),canvas.entities().end(),
        [&](const auto& value) { return value.id == id; });
    return found == canvas.entities().end() ? nullptr : &*found;
}
const CanvasLabel& label(const PlanCanvas& canvas, const QString& id) {
    const auto found = std::find_if(canvas.labels().begin(),canvas.labels().end(),
        [&](const auto& value) { return value.id == id; });
    require(found != canvas.labels().end(), "historical scene retains its authored dimension label");
    return *found;
}
void unchanged_document(MainWindow& window, const DocumentSnapshot& before) {
    const auto after = window.document().snapshot();
    require(after.revision() == before.revision() && after.saved_revision_optional() == before.saved_revision_optional() &&
        after.dirty() == before.dirty() && after.entities() == before.entities() && after.assets() == before.assets() &&
        after.named_revisions() == before.named_revisions() && after.history().size() == before.history().size(),
        "visual comparison preserves current entities, assets, head, save marker, dirty state and named history");
    for (std::size_t i = 0; i < before.history().size(); ++i) {
        const auto& original = before.history()[i]; const auto& current = after.history()[i];
        require(current.revision == original.revision && current.parent_revision == original.parent_revision &&
            current.source_revision == original.source_revision && current.action == original.action &&
            current.name == original.name && current.entities == original.entities && current.assets == original.assets &&
            current.undo_stack == original.undo_stack && current.redo_stack == original.redo_stack,
            "visual comparison never rewrites retained historical states or navigation");
    }
}
void mouse(PlanCanvas& canvas, QEvent::Type type, QPointF point, Qt::MouseButton button, Qt::MouseButtons buttons) {
    QMouseEvent event(type,point,canvas.mapToGlobal(point.toPoint()),button,buttons,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&event);
}
bool close_point(Vec2 a,Vec2 b) { return std::hypot(a.x-b.x,a.y-b.y)<1e-9; }
void test_navigation_notifications() {
    PlanCanvas canvas; canvas.setAttribute(Qt::WA_DontShowOnScreen); canvas.resize(640,480);
    canvas.setEntities({CanvasEntity{"navigation-owner","boundary",{{{-2,-2},{2,-2},0},
        {{2,-2},{2,2},0},{{2,2},{-2,2},0},{{-2,2},{-2,-2},0}}}});
    int notifications{}; Vec2 last_center{}; double last_scale{};
    canvas.setNavigationChanged([&](Vec2 center,double scale) {
        ++notifications; last_center=center; last_scale=scale;
        require(std::isfinite(center.x) && std::isfinite(center.y) && scale>=0.0001 && scale<=4000,
            "navigation observers receive only valid bounded transforms");
    });
    require(notifications==0,"navigation callback registration does not synthesize a view change");
    canvas.setViewTransform(canvas.viewCenter(),canvas.viewScale());
    const auto initial_center=canvas.viewCenter(); const auto initial_scale=canvas.viewScale();
    canvas.setViewTransform({std::numeric_limits<double>::quiet_NaN(),0},80);
    canvas.setViewTransform({0,std::numeric_limits<double>::infinity()},80);
    for (double scale : {0.0,-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        canvas.setViewTransform({1,2},scale);
    require(notifications==0 && same_point(canvas.viewCenter(),initial_center) && canvas.viewScale()==initial_scale,
        "unchanged or invalid navigation transforms are ignored without notification");
    canvas.setViewTransform({1,2},1e-10);
    require(notifications==1 && canvas.viewScale()==0.0001,"valid small navigation scale clamps and notifies once");
    canvas.setViewTransform({1,2},1e10);
    require(notifications==2 && canvas.viewScale()==4000,"valid large navigation scale clamps and notifies once");
    canvas.setViewTransform({1,2},4000);
    require(notifications==2,"setting an already clamped view never re-emits");
    canvas.fitView(); const auto fitted_notifications=notifications;
    require(fitted_notifications==3,"fitView publishes its actual transform change");
    canvas.fitView(); require(notifications==fitted_notifications,"repeated fitView never emits an unchanged transform");
    canvas.zoomBy(1.2,QRectF(canvas.rect()).center());
    require(notifications==fitted_notifications+1,"anchor zoom publishes its actual transform change");
    const auto before_pan=notifications;
    mouse(canvas,QEvent::MouseButtonPress,{320,240},Qt::RightButton,Qt::RightButton);
    mouse(canvas,QEvent::MouseMove,{350,260},Qt::NoButton,Qt::RightButton);
    mouse(canvas,QEvent::MouseButtonRelease,{360,270},Qt::RightButton,Qt::NoButton);
    require(notifications>before_pan && close_point(last_center,canvas.viewCenter()) && last_scale==canvas.viewScale(),
        "actual pan events publish the final navigation transform");
    const auto before_overview=notifications;
    const auto map=canvas.overviewMapRect();
    require(!map.isEmpty(),"standalone navigation fixture exposes its actual overview map");
    const auto target=map.topLeft()+QPointF(map.width()*0.2,map.height()*0.8);
    mouse(canvas,QEvent::MouseButtonPress,target,Qt::LeftButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseButtonRelease,target,Qt::LeftButton,Qt::NoButton);
    require(notifications>before_overview && close_point(last_center,canvas.viewCenter()) && last_scale==canvas.viewScale(),
        "actual overview navigation publishes its final transform");
}
const CanvasReference& reference(const PlanCanvas& canvas,const QString& id) {
    const auto found=std::find_if(canvas.references().begin(),canvas.references().end(),
        [&](const auto& value) { return value.id==id; });
    require(found!=canvas.references().end(),"comparison pane retains the historical reference owner");
    return *found;
}
const Entity& symbol_owner(const DocumentSnapshot& snapshot,const QString& id) {
    for (const auto& [key,owner] : snapshot.entities()) {
        (void)key;
        if (owner.type!=kAnnotationEntityType) continue;
        for (const auto& symbol : decode_annotation_entity(owner).symbols)
            if (symbol.id==id.toStdString()) return owner;
    }
    throw std::runtime_error("historical SVG fixture has no annotation owner");
}
int colored_pixels(const QImage& image,QColor expected) {
    int result{};
    for (int y=0;y<image.height();++y) for (int x=0;x<image.width();++x) {
        const auto color=image.pixelColor(x,y);
        if (std::abs(color.red()-expected.red())<18 && std::abs(color.green()-expected.green())<18 &&
            std::abs(color.blue()-expected.blue())<18) ++result;
    }
    return result;
}
void test_native_visual_revision_comparison() {
    QTemporaryDir directory; require(directory.isValid(), "revision comparison fixture is isolated");
    MainWindow window({},nullptr,directory.filePath(QStringLiteral("text-library.json")));
    window.setAttribute(Qt::WA_DontShowOnScreen); window.resize(1280,900);
    window.setMetricUnits(true); window.show(); QApplication::processEvents();
    const Boundary original{{{0,0},{4,0},0},{{4,0},{4,3},0},{{4,3},{0,3},0.7},{{0,3},{0,0},0}};
    const auto area = window.createBoundary(original);
    require(!area.isEmpty(), "historical fixture authors an exact line-and-arc boundary");
    const auto identified = decode_identified_boundary_entity(window.document().snapshot().entities().at(area.toStdString()));
    const auto dimension = window.createLengthDimension(area,QString::fromStdString(identified.segments.front().segment_id),{2,-1});
    require(!dimension.isEmpty() && window.editBoundaryDimension(dimension,"2 m","-1 m","4","#112233",true,false,true,"15"),
        "historical fixture authors a styled dependent dimension");
    const auto derived_dimension=window.createLengthDimension(area,QString::fromStdString(identified.segments.front().segment_id),{2,-2});
    require(!derived_dimension.isEmpty(),"historical fixture retains an unchanged dependent dimension owner");
    QImage red(8,8,QImage::Format_ARGB32); red.fill(Qt::red);
    const auto reference_path=directory.filePath("historical-reference.png");
    require(red.save(reference_path,"PNG"),"generate caller-owned historical raster pixels");
    const auto reference_id=testing::importOrSeedTrustedReferenceFixture(window,reference_path,red);
    auto placed_reference=window.document().snapshot().entities().at(reference_id.toStdString());
    placed_reference.properties["property_id"]="property-1";
    placed_reference.properties["building_id"]="building-1";
    placed_reference.properties["floor_id"]="floor-1";
    placed_reference.properties["layer_id"]="layer-1";
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(std::move(placed_reference))},{},"Place the trusted reference on its fixture floor"});
    require(window.editReferenceTransform(reference_id,"20","10","0.01","1","0","1",false,false,true),
        "historical raster is independently positioned and fully visible for pixel verification");
    const auto historical_floor=window.createFloor("building-1","Historical loft");
    const auto historical_layer=window.activeLayerId();
    const auto historical_wall=window.createStraightWall({0,0},{2,0});
    require(!historical_floor.isEmpty() && !historical_wall.isEmpty() && window.setActiveLayer("layer-1"),
        "historical fixture retains an independent floor and returns to the original drawing context");
    const auto removed = window.createBoundary({{{8,0},{10,0},0},{{10,0},{9,2},0},{{9,2},{8,0},0}});
    require(!removed.isEmpty(), "historical fixture authors a subsequently removed owner");
    const auto colored_symbol=window.createAnnotationSymbol("svg-v2-04_living-sofa-three-seat",{-5,0});
    const auto unchanged_symbol=window.createAnnotationSymbol("svg-v2-04_living-sofa-three-seat",{-5,3});
    require(!colored_symbol.isEmpty() && !unchanged_symbol.isEmpty() &&
        symbol_owner(window.document().snapshot(),colored_symbol).id==symbol_owner(window.document().snapshot(),unchanged_symbol).id,
        "historical fixture retains two SVG siblings in the same annotation parent");
    name_revision(window,QStringLiteral("Existing geometry"));
    const auto existing = window.document().snapshot();
    const auto existing_revision = existing.named_revisions().at("Existing geometry");
    QImage blue(8,8,QImage::Format_ARGB32); blue.fill(Qt::blue);
    QByteArray png; QBuffer buffer(&png);
    require(buffer.open(QIODevice::WriteOnly) && blue.save(&buffer,"PNG"),"encode replacement retained raster pixels");
    const auto& reference_owner=existing.entities().at(reference_id.toStdString());
    const auto render_id=reference_owner.properties.at("render_asset_id").get<std::string>();
    const auto& old_asset=existing.assets().at(render_id);
    std::vector<std::byte> pixels(static_cast<std::size_t>(png.size()));
    std::memcpy(pixels.data(),png.constData(),pixels.size());
    window.document().apply(ApplyEntityChanges{window.document().revision(),{},
        {AssetChange::upsert(Asset::create(render_id,old_asset.media_type,std::move(pixels),old_asset.metadata))},
        "Change retained reference pixels without changing the owner"});
    require(window.document().snapshot().entities().at(reference_id.toStdString())==reference_owner,
        "asset-only fixture preserves identical reference owner JSON");
    require(window.editSymbolSvgPalette(colored_symbol,SymbolSvgPalette{"white-outline-2","#2040aa","#ff0000"}),
        "normal component colors API changes only the first historical SVG instance palette");
    require(window.selectEntity(area) && window.editSelectedBoundaryEdgeLength(
        QString::fromStdString(identified.segments.front().segment_id),"5 m",BoundaryFixedEndpoint::start,false),
        "design revision changes the exact boundary through normal geometry authoring");
    require(window.editBoundaryDimension(dimension,"2.5 m","-1 m","5","#aa5500",false,true,true,"25") &&
        window.selectEntity(removed) && window.deleteSelection(), "design revision changes dimension appearance and removes an owner");
    require(window.selectEntity(historical_wall) && window.deleteSelection(),"design revision removes the historical floor's geometry");
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::erase(historical_layer.toStdString()),EntityChange::erase(historical_floor.toStdString())},{},
        "Remove the now-empty historical floor"});
    require(window.setActiveLayer("layer-1"),"removed historical context does not retarget subsequent authoring");
    const auto added = window.createBoundary({{{8,4},{10,4},0},{{10,4},{10,6},0},{{10,6},{8,6},0},{{8,6},{8,4},0}});
    require(!added.isEmpty(), "design revision adds an independent owner");
    name_revision(window,QStringLiteral("Design geometry"));
    const auto design = window.document().snapshot();
    const auto design_revision = design.named_revisions().at("Design geometry");
    require(design.entities().at(derived_dimension.toStdString())==existing.entities().at(derived_dimension.toStdString()),
        "changed derived measurement retains its exact original dimension owner JSON");
    const auto live_only = window.createStraightWall({12,0},{12,3});
    require(!live_only.isEmpty() && window.saveProjectAs(directory.filePath("revision-comparison.bldproj")),
        "current head has independent later work and a saved marker");
    require(window.selectEntity(area) && window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,"measurement"),
        "comparison fixture retains a real unfinished drawing session");
    auto& live_canvas = child<PlanCanvas>(window,"measurementPlanCanvas");
    live_canvas.setSnapEnabled(false); live_canvas.setOverviewMapEnabled(false);
    mouse(live_canvas,QEvent::MouseButtonPress,{20,20},Qt::LeftButton,Qt::LeftButton);
    mouse(live_canvas,QEvent::MouseButtonRelease,{20,20},Qt::LeftButton,Qt::NoButton);
    require(live_canvas.boundaryDraftPreview().has_value(), "actual drawing click establishes a draft before comparison");
    const auto draft = *live_canvas.boundaryDraftPreview();
    const auto selection = window.selectedEntityIds();
    const auto current = window.document().snapshot();
    history_dialog(window,[&](QDialog& dialog) {
        auto& from = child<QComboBox>(dialog,"revisionCompareFrom");
        auto& to = child<QComboBox>(dialog,"revisionCompareTo");
        select_state(from,existing_revision,false); select_state(to,current.revision(),true);
        child<QPushButton>(dialog,"compareRevisions").click();
        auto& before_canvas = child<PlanCanvas>(dialog,"revisionBeforeCanvas");
        auto& after_canvas = child<PlanCanvas>(dialog,"revisionAfterCanvas");
        auto& overlay = child<PlanCanvas>(dialog,"revisionOverlayCanvas");
        auto& mode = child<QComboBox>(dialog,"revisionVisualMode");
        auto& context=child<QComboBox>(dialog,"revisionDrawingContext");
        auto& before_diagnostics=child<QLabel>(dialog,"revisionBeforeDiagnostics");
        auto& after_diagnostics=child<QLabel>(dialog,"revisionAfterDiagnostics");
        require(context.findData(QStringLiteral("floor-1"))>=0 && context.findData(historical_floor)>=0 &&
            context.findData(QStringLiteral("building-1"))>=0 && context.findData(QStringLiteral("property-1"))>=0,
            "drawing context selector retains the union of historical/current property, building and floor identities");
        context.setCurrentIndex(context.findData(QStringLiteral("floor-1")));
        require(mode.findData(QStringLiteral("side_by_side")) >= 0 && mode.findData(QStringLiteral("overlay")) >= 0,
            "actual revision comparison exposes side-by-side and overlay modes");
        const auto verify_scene = [&](const PlanCanvas& canvas,const DocumentSnapshot& snapshot) {
            const auto* geometry = entity(canvas,area);
            const auto expected = boundary_geometry(decode_identified_boundary_entity(snapshot.entities().at(area.toStdString())));
            require(geometry && same_geometry(geometry->segments,expected),
                "comparison scenes retain exact historical line endpoints and circular sweeps");
            const auto& annotation = label(canvas,dimension);
            const bool old = snapshot.revision() == existing.revision();
            require(annotation.text == (old ? QStringLiteral("4.000 m") : QStringLiteral("5.000 m")) &&
                annotation.color == QColor(old ? "#112233" : "#aa5500") &&
                annotation.bold == old && annotation.italic != old && annotation.paper_height_mm==(old ? 4.0 : 5.0) &&
                same_point(annotation.position,old ? Vec2{2,-1} : Vec2{2.5,-1}) &&
                std::abs(annotation.rotation_radians - (old ? 15.0 : 25.0)*std::acos(-1.0)/180)<1e-12,
                "each historical scene resolves its own dependent dimension and authored appearance");
        };
        verify_scene(before_canvas,existing); verify_scene(after_canvas,current);
        const auto* old_symbol=entity(before_canvas,colored_symbol);
        const auto* new_symbol=entity(after_canvas,colored_symbol);
        const auto* old_sibling=entity(before_canvas,unchanged_symbol);
        const auto* new_sibling=entity(after_canvas,unchanged_symbol);
        require(old_symbol && new_symbol && old_sibling && new_sibling && old_symbol->svg_symbol && new_symbol->svg_symbol &&
            old_sibling->svg_symbol && new_sibling->svg_symbol,
            "side-by-side panes retain both historical SVG siblings as real artwork records");
        require(old_symbol->svg_symbol->document==new_symbol->svg_symbol->document &&
            old_symbol->svg_symbol->artwork_sha256==new_symbol->svg_symbol->artwork_sha256 &&
            !old_symbol->svg_symbol->svg_palette && new_symbol->svg_symbol->svg_palette==
                std::optional<SymbolSvgPalette>{SymbolSvgPalette{"white-outline-2","#2040aa","#ff0000"}} &&
            same_geometry(old_symbol->segments,new_symbol->segments),
            "palette-only revision preserves exact SVG source bytes, digest and physical geometry");
        require(old_sibling->svg_symbol->document==new_sibling->svg_symbol->document &&
            old_sibling->svg_symbol->svg_palette==new_sibling->svg_symbol->svg_palette,
            "palette revision leaves the sibling's exact historical artwork and paint intent unchanged");
        const auto symbol_image=[](const PlanCanvas& canvas) {
            QImage image(360,240,QImage::Format_ARGB32_Premultiplied); image.fill(Qt::white);
            QPainter painter(&image); canvas.renderSceneAt(painter,QRectF(image.rect()),130,{-5,0},Qt::white); painter.end();
            return image;
        };
        require(colored_pixels(symbol_image(before_canvas),QColor("#ff0000"))==0 &&
            colored_pixels(symbol_image(after_canvas),QColor("#ff0000"))>20,
            "actual historical SVG pane rendering preserves the old palette and paints the new surface color");
        require(reference(before_canvas,reference_id).image.pixelColor(0,0)==QColor(Qt::red) &&
            reference(after_canvas,reference_id).image.pixelColor(0,0)==QColor(Qt::blue),
            "historical/current panes decode each retained raster asset instead of reusing current pixels");
        const auto raster_pixel=[](const PlanCanvas& canvas) {
            QImage image(64,64,QImage::Format_ARGB32_Premultiplied); image.fill(Qt::white);
            QPainter painter(&image);
            canvas.renderSceneAt(painter,QRectF(image.rect()),1000,{20,10},Qt::white); painter.end();
            return image.pixelColor(32,32);
        };
        require(raster_pixel(before_canvas)==QColor(Qt::red) && raster_pixel(after_canvas)==QColor(Qt::blue),
            "actual historical pane rendering paints each retained asset's own pixels");
        require(label(before_canvas,derived_dimension).text==QStringLiteral("4.000 m") &&
            label(after_canvas,derived_dimension).text==QStringLiteral("5.000 m"),
            "unchanged dependent dimension owners resolve from each selected state's actual geometry");
        require(entity(before_canvas,removed) && !entity(after_canvas,removed) &&
            !entity(before_canvas,added) && entity(after_canvas,added) && entity(after_canvas,live_only),
            "named/current comparison distinguishes removed and added owners from current-only work");
        require(!overlay.entities().empty(), "revision overlay projects actual comparison geometry");
        const auto* removed_overlay=entity(overlay,QStringLiteral("revision-before:")+removed);
        const auto* added_overlay=entity(overlay,QStringLiteral("revision-after:")+added);
        const auto* old_changed=entity(overlay,QStringLiteral("revision-before:")+area);
        const auto* new_changed=entity(overlay,QStringLiteral("revision-after:")+area);
        require(removed_overlay && added_overlay && old_changed && new_changed,
            "overlay retains separate removed, added and both changed analytical owners");
        const auto red_tone=[](QColor color) { return color.red()>color.green() && color.red()>color.blue(); };
        const auto green_tone=[](QColor color) { return color.green()>color.red() && color.green()>color.blue(); };
        const auto amber_tone=[](QColor color) { return color.red()>color.blue() && color.green()>color.blue(); };
        const auto* old_colored_overlay=entity(overlay,QStringLiteral("revision-before:")+colored_symbol);
        const auto* new_colored_overlay=entity(overlay,QStringLiteral("revision-after:")+colored_symbol);
        require(old_colored_overlay && new_colored_overlay && amber_tone(old_colored_overlay->stroke_color) &&
            amber_tone(new_colored_overlay->stroke_color),
            "overlay marks both retained versions of a palette-only symbol edit as changed");
        const auto* unchanged_overlay=entity(overlay,QStringLiteral("revision-after:")+unchanged_symbol);
        require(unchanged_overlay && !entity(overlay,QStringLiteral("revision-before:")+unchanged_symbol) &&
            unchanged_overlay->stroke_color==new_sibling->stroke_color && unchanged_overlay->svg_symbol &&
            unchanged_overlay->svg_symbol->svg_palette==new_sibling->svg_symbol->svg_palette &&
            !unchanged_overlay->dashed_stroke &&
            std::count_if(overlay.entities().begin(),overlay.entities().end(),[&](const auto& value) {
                return value.id==QStringLiteral("revision-after:")+unchanged_symbol;
            })==1,
            "unchanged sibling in the edited annotation parent appears once with authored appearance");
        require(red_tone(removed_overlay->stroke_color) && green_tone(added_overlay->stroke_color) &&
            amber_tone(old_changed->stroke_color) && amber_tone(new_changed->stroke_color) && removed_overlay->dashed_stroke,
            "actual overlay gives removed, added and changed owners distinct red, green and amber indications");
        require(amber_tone(label(overlay,QStringLiteral("revision-before:")+derived_dimension).color) &&
            amber_tone(label(overlay,QStringLiteral("revision-after:")+derived_dimension).color),
            "overlay highlights a changed derived dimension even when its owner JSON is unchanged");
        const auto synced=[&] {
            require(close_point(before_canvas.viewCenter(),after_canvas.viewCenter()) &&
                close_point(before_canvas.viewCenter(),overlay.viewCenter()) &&
                before_canvas.viewScale()==after_canvas.viewScale() && before_canvas.viewScale()==overlay.viewScale(),
                "comparison panes synchronize actual model-space navigation without drift");
        };
        before_canvas.zoomBy(1.3,QRectF(before_canvas.rect()).center()); synced();
        const auto before_pan=after_canvas.viewCenter();
        const auto pan_start=QRectF(after_canvas.rect()).center();
        mouse(after_canvas,QEvent::MouseButtonPress,pan_start,Qt::RightButton,Qt::RightButton);
        mouse(after_canvas,QEvent::MouseMove,pan_start+QPointF(35,20),Qt::NoButton,Qt::RightButton);
        mouse(after_canvas,QEvent::MouseButtonRelease,pan_start+QPointF(40,25),Qt::RightButton,Qt::NoButton);
        require(!close_point(before_pan,after_canvas.viewCenter()),"actual comparison pan moves its viewport"); synced();
        const auto panned_center=after_canvas.viewCenter(); const auto panned_scale=after_canvas.viewScale();
        child<QPushButton>(dialog,"revisionFitDrawings").click();
        require(!close_point(panned_center,after_canvas.viewCenter()) || panned_scale!=after_canvas.viewScale(),
            "actual Fit drawings action changes the panned view to the retained drawing union");
        synced(); unchanged_document(window,current);
        before_canvas.setOverviewMapEnabled(true);
        const auto map=before_canvas.overviewMapRect();
        require(!map.isEmpty(),"comparison pane exposes an actual navigation overview");
        const auto map_target=map.topLeft()+QPointF(map.width()*0.2,map.height()*0.8);
        mouse(before_canvas,QEvent::MouseButtonPress,map_target,Qt::LeftButton,Qt::LeftButton);
        mouse(before_canvas,QEvent::MouseButtonRelease,map_target,Qt::LeftButton,Qt::NoButton); synced();
        const auto click=QRectF(before_canvas.rect()).center();
        mouse(before_canvas,QEvent::MouseButtonPress,click,Qt::LeftButton,Qt::LeftButton);
        mouse(before_canvas,QEvent::MouseMove,click+QPointF(30,20),Qt::NoButton,Qt::LeftButton);
        mouse(before_canvas,QEvent::MouseButtonRelease,click+QPointF(30,20),Qt::LeftButton,Qt::NoButton);
        require(window.selectedEntityIds()==selection && !before_canvas.entitiesMovePreviewPending() &&
            before_canvas.entitiesMovePreview().empty(),"comparison pointer input has no live selection or edit authority");
        const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!capture.isEmpty()) require(QDir().mkpath(capture) && dialog.grab().save(
            QDir(capture).filePath("revision-comparison-side-by-side.png")), "capture actual visual revision comparison");
        select_state(to,design_revision,false); child<QPushButton>(dialog,"compareRevisions").click();
        verify_scene(before_canvas,existing); verify_scene(after_canvas,design);
        require(!entity(after_canvas,live_only), "named/named comparison never substitutes later current-head geometry");
        const auto& report = child<QPlainTextEdit>(dialog,"revisionComparison");
        require(report.toPlainText().contains("From: Existing geometry") && report.toPlainText().contains("To: Design geometry"),
            "visual comparison retains its categorized text report and selected state names");
        context.setCurrentIndex(context.findData(historical_floor)); QApplication::processEvents();
        const auto missing_context=[](const QString& text) {
            return text.contains("unavailable",Qt::CaseInsensitive) || text.contains("absent",Qt::CaseInsensitive) ||
                text.contains("no matching floor",Qt::CaseInsensitive);
        };
        require(entity(before_canvas,historical_wall) && after_canvas.entities().empty() && after_canvas.labels().empty() &&
            after_canvas.references().empty() && missing_context(after_diagnostics.text()),
            "historical-only floor renders its retained scene and explicitly diagnoses its absence in the later state");
        require(!missing_context(before_diagnostics.text()),
            "missing later floor does not hide the valid historical drawing context");
        context.setCurrentIndex(context.findData(QStringLiteral("building-1"))); QApplication::processEvents();
        require(before_canvas.entities().empty() && before_diagnostics.text().contains("floor",Qt::CaseInsensitive),
            "multi-floor historical building asks for a floor rather than stacking unrelated levels");
        context.setCurrentIndex(context.findData(QStringLiteral("floor-1"))); QApplication::processEvents();
        mode.setCurrentIndex(mode.findData(QStringLiteral("overlay"))); QApplication::processEvents();
        require(overlay.isVisible(), "overlay mode displays the actual overlay canvas");
        if (!capture.isEmpty()) require(dialog.grab().save(QDir(capture).filePath("revision-comparison-overlay.png")),
            "capture actual colored revision overlay");
        unchanged_document(window,current);
        const auto valid_index=to.currentIndex();
        const auto valid_revision=to.itemData(valid_index);
        const auto previous_status=child<QLabel>(dialog,"revisionStatus").text();
        // Fault-inject an unavailable retained source into the real selected item.
        // The live document remains valid; a failed request must retire the old pair.
        to.setItemData(valid_index,QVariant::fromValue<qulonglong>(std::numeric_limits<Revision>::max()));
        child<QPushButton>(dialog,"compareRevisions").click();
        for (const auto* canvas : {&before_canvas,&after_canvas,&overlay})
            require(canvas->entities().empty() && canvas->labels().empty() && canvas->references().empty(),
                "failed retained-source comparison clears every stale geometry, annotation and raster pane");
        const auto failed_status=child<QLabel>(dialog,"revisionStatus").text();
        require(report.toPlainText().trimmed().isEmpty() && !failed_status.trimmed().isEmpty() && failed_status!=previous_status,
            "failed comparison clears the previous pair report and replaces its status with an explicit failure");
        unchanged_document(window,current);
        to.setItemData(valid_index,valid_revision); select_state(to,design_revision,false);
        child<QPushButton>(dialog,"compareRevisions").click();
        verify_scene(before_canvas,existing); verify_scene(after_canvas,design);
        require(!report.toPlainText().trimmed().isEmpty(),"valid comparison recovers after an unavailable retained source");
        unchanged_document(window,current);
    });
    unchanged_document(window,current);
    require(window.selectedEntityIds() == selection && live_canvas.boundaryDraftPreview() &&
        same_draft(*live_canvas.boundaryDraftPreview(),draft),
        "closing visual history preserves the live selection and unfinished drawing draft");
    QKeyEvent cancel(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
    QApplication::sendEvent(&live_canvas,&cancel);
    require(window.selectEntity(area) && window.editSelectedBoundaryEdgeLength(
        QString::fromStdString(identified.segments.front().segment_id),"6 m",BoundaryFixedEndpoint::start,false),
        "normal editing remains available after closing visual history");
    require(window.undoCommand() && window.document().snapshot().entities() == current.entities() &&
        window.document().snapshot().assets() == current.assets(), "Undo after comparison restores exact current-head work");
    QString masked_wall;
    history_dialog(window,[&](QDialog& dialog) {
        auto& from=child<QComboBox>(dialog,"revisionCompareFrom");
        auto& to=child<QComboBox>(dialog,"revisionCompareTo");
        select_state(from,window.document().revision(),true); select_state(to,window.document().revision(),true);
        const auto intervening=window.createStraightWall({14,0},{14,2});
        masked_wall=intervening;
        require(!intervening.isEmpty(),"current-head refresh uses an explicitly authored intervening normal edit");
        const auto refreshed=window.document().snapshot();
        child<QPushButton>(dialog,"compareRevisions").click();
        require(from.currentData(Qt::UserRole+1).toBool() && to.currentData(Qt::UserRole+1).toBool() &&
            from.currentData().toULongLong()==refreshed.revision() && to.currentData().toULongLong()==refreshed.revision() &&
            from.currentText().contains(QString::number(refreshed.revision())) && to.currentText().contains(QString::number(refreshed.revision())),
            "both Current head selectors refresh IDs and captions from the same fresh snapshot");
        require(entity(child<PlanCanvas>(dialog,"revisionBeforeCanvas"),intervening) &&
            entity(child<PlanCanvas>(dialog,"revisionAfterCanvas"),intervening),
            "both Current head comparison panes use the intervening current geometry");
        unchanged_document(window,refreshed);
        require(window.setContainerVisible("floor-1",false) && !window.entityVisible(intervening),
            "current-view negative applies a real visibility mask after the drawing draft has ended");
        child<QPushButton>(dialog,"compareRevisions").click();
        require(entity(child<PlanCanvas>(dialog,"revisionBeforeCanvas"),intervening) &&
            entity(child<PlanCanvas>(dialog,"revisionAfterCanvas"),intervening),
            "historical drawing scope remains visible independently of the live floor visibility mask");
        unchanged_document(window,refreshed);
    });
    require(window.setContainerVisible("floor-1",true) && window.entityVisible(masked_wall),
        "closing history restores the live floor visibility mask for continued authoring");
}
}
int main(int argc,char** argv) {
    qputenv("QT_QPA_PLATFORM","offscreen"); testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true); QApplication application(argc,argv);
    QCoreApplication::setOrganizationName(QStringLiteral("VertexTests"));
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-revision-comparison-test-")+
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0,"bundled native font loads");
        application.setFont(QFont(QStringLiteral("Inter"),10));
        if (application.arguments().contains(QStringLiteral("--navigation-only"))) {
            test_navigation_notifications(); return 0;
        }
        test_navigation_notifications();
        test_native_visual_revision_comparison();
        std::cout << "Revision comparison desktop tests passed\n"; return 0;
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
