#include "sketch/document.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/visualization/native_model_view.hpp"
#include "support/noninteractive_errors.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool parse_positive_finite(const QString& text, double& value) {
    bool ok = false;
    value = text.toDouble(&ok);
    return ok && std::isfinite(value) && value > 0.0;
}
struct Frame { QImage image; QRect bounds; QPointF centre; };
Frame capture(sketch::visualization::NativeModelView& view, const QString& path) {
    const auto began = std::chrono::steady_clock::now();
    std::cerr << "Native capture started: " << path.section('/', -1).toStdString() << std::endl;
    check(view.exportViewImage(path), "Native framebuffer export failed");
    const auto artifact_directory = qEnvironmentVariable("SKETCH_TEST_ARTIFACT_DIR");
    if (!artifact_directory.isEmpty()) {
        QDir().mkpath(artifact_directory);
        const auto retained = QDir(artifact_directory).filePath(QFileInfo(path).fileName());
        QFile::remove(retained);
        check(QFile::copy(path, retained), "Native framebuffer artifact copy failed");
    }
    QImage image(path);
    check(!image.isNull(), "Native framebuffer must be a readable image");
    const auto scan_began = std::chrono::steady_clock::now();
    // Scan normalized rows directly. Per-pixel QColor construction added
    // substantial Debug overhead at high DPI; retain the same RGB threshold and
    // original image while avoiding that test-only overhead.
    const QImage pixels = image.convertToFormat(QImage::Format_ARGB32);
    const auto background = image.pixelColor(0,0);
    int left=image.width(),right=-1,top=image.height(),bottom=-1;
    double sx=0,sy=0,count=0;
    for(int y=0;y<image.height();++y) {
      const auto* row = reinterpret_cast<const QRgb*>(pixels.constScanLine(y));
      for(int x=0;x<image.width();++x) {
        const auto pixel=row[x];
        if(std::abs(qRed(pixel)-background.red())+std::abs(qGreen(pixel)-background.green())
            +std::abs(qBlue(pixel)-background.blue())<60) continue;
        left=std::min(left,x);right=std::max(right,x);top=std::min(top,y);bottom=std::max(bottom,y);
        sx+=x;sy+=y;count+=1;
      }
    }
    check(count>1000,"Native framebuffer must contain the model");
    const auto finished = std::chrono::steady_clock::now();
    std::cerr << "Native capture finished: export/load "
              << std::chrono::duration_cast<std::chrono::milliseconds>(scan_began - began).count()
              << " ms, pixel scan "
              << std::chrono::duration_cast<std::chrono::milliseconds>(finished - scan_began).count()
              << " ms" << std::endl;
    return {image,QRect(QPoint(left,top),QPoint(right,bottom)),QPointF(sx/count,sy/count)};
}
void mouse(sketch::visualization::NativeModelView& view,QEvent::Type type,QPointF p,
           Qt::MouseButton button,Qt::MouseButtons buttons,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QMouseEvent event(type,p,view.mapToGlobal(p.toPoint()),button,buttons,modifiers);
    QApplication::sendEvent(&view,&event);
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc,argv);
    if(QGuiApplication::platformName()!=QStringLiteral("windows")) return 77;
    const auto arguments = application.arguments();
    const int expected_dpr_index = arguments.indexOf(QStringLiteral("--expected-dpr"));
    if (expected_dpr_index < 0 || expected_dpr_index + 1 >= arguments.size()) {
        std::cerr << "Native test requires --expected-dpr followed by a finite positive number\n";
        return 2;
    }
    const QString expected_dpr_text = arguments.at(expected_dpr_index + 1);
    double expected_dpr = 0.0;
    if (!parse_positive_finite(expected_dpr_text, expected_dpr)) {
        std::cerr << "Native --expected-dpr must be finite and greater than zero; got '"
                  << expected_dpr_text.toStdString() << "'\n";
        return 2;
    }
    const int scenario_index = arguments.indexOf(QStringLiteral("--scenario"));
    const QString scenario = scenario_index < 0 ? QStringLiteral("all") : arguments.value(scenario_index + 1);
    if (scenario != "all" && scenario != "geometry" && scenario != "forms") {
        std::cerr << "Native scenario must be all, geometry or forms\n";
        return 1;
    }
    QTemporaryDir temporary;
    check(temporary.isValid(),"Native test needs temporary storage");
    auto document=sketch::Document::create({sketch::Entity::create("wall",{
        {"baseline",{{"start",{-3.0,0.0}},{"end",{3.0,0.0}},{"sweep_radians",0.0}}},
        {"thickness_m",0.2},{"height_m",2.8},{"elevation_m",0.0}})});
    const auto wall_id=QString::fromStdString(document.snapshot().entities().begin()->first);
    sketch::visualization::NativeModelView view;
    view.setAttribute(Qt::WA_DontShowOnScreen,true);
    view.setAttribute(Qt::WA_ShowWithoutActivating,true);
    view.resize(700,500);
    view.setSnapshot(document.snapshot());
    QString selected;
    view.onEntitySelected=[&](QString id){selected=std::move(id);};
    view.show();
    QTimer::singleShot(100,&application,[&] {
        try {
            const double ratio=view.devicePixelRatioF();
            constexpr double dpr_tolerance = 0.001;
            if (!std::isfinite(ratio) || std::abs(ratio - expected_dpr) > dpr_tolerance) {
                std::cerr << "Native DPR mismatch: expected " << expected_dpr
                          << ", actual " << ratio << " (tolerance " << dpr_tolerance << ")\n";
                application.exit(1);
                return;
            }
            check(view.isReady(),"Native viewport must be ready");
            if (scenario != "forms") {
                view.fitAll();
                auto first=capture(view,temporary.filePath("first.png"));
                check(first.bounds.left()>2 && first.bounds.right()<first.image.width()-3
                      &&first.bounds.top()>2 && first.bounds.bottom()<first.image.height()-3,
                      "Fit must leave all geometry inside the native frame");
                QString translated_wall_id;
                double translated_wall_x = 0.0;
                double translated_wall_y = 0.0;
                double translated_wall_z = 0.0;
                view.onEntityTranslationRequested =
                    [&](QString id, double x, double y, double z) {
                        translated_wall_id = std::move(id);
                        translated_wall_x = x;
                        translated_wall_y = y;
                        translated_wall_z = z;
                    };
                const auto wall_drag_start = first.centre / view.devicePixelRatioF();
                const auto wall_drag_end = wall_drag_start + QPointF(16.0, 10.0);
                mouse(view, QEvent::MouseButtonPress, wall_drag_start, Qt::LeftButton,
                      Qt::LeftButton, Qt::ControlModifier);
                mouse(view, QEvent::MouseMove, wall_drag_end, Qt::NoButton,
                      Qt::LeftButton, Qt::ControlModifier);
                check(capture(view, temporary.filePath("wall-translation-preview.png")).image !=
                          first.image,
                      "Ctrl+left-drag must preview a shared wall-solid translation");
                mouse(view, QEvent::MouseButtonRelease, wall_drag_end, Qt::LeftButton,
                      Qt::NoButton, Qt::ControlModifier);
                mouse(view, QEvent::MouseButtonPress, {2, 2}, Qt::LeftButton,
                      Qt::LeftButton);
                mouse(view, QEvent::MouseButtonRelease, {2, 2}, Qt::LeftButton,
                      Qt::NoButton);
                check(capture(view, temporary.filePath("wall-translation-reset.png")).image ==
                          first.image && translated_wall_id == wall_id &&
                          std::isfinite(translated_wall_x) && std::isfinite(translated_wall_y) &&
                          std::isfinite(translated_wall_z) &&
                          (std::abs(translated_wall_x) > 1.0e-9 ||
                           std::abs(translated_wall_y) > 1.0e-9 ||
                           std::abs(translated_wall_z) > 1.0e-9),
                      "shared wall-solid translation must emit one finite semantic request");
                view.onEntityTranslationRequested = {};
                // Choose a solid interior pixel away from the symmetrical centre;
                // an erroneous y flip must not still pass the picking test.
                QPoint native_pick;
                bool found=false;
                for(int y=first.bounds.top()+8;y<first.bounds.center().y() && !found;++y)
                    for(int x=first.bounds.left()+8;x<first.bounds.right()-8;++x) {
                        const auto pixel=first.image.pixelColor(x,y);
                        if(pixel.red()>pixel.blue()+35 && pixel.green()>pixel.blue()+20) {
                            native_pick={x,y};found=true;break;
                        }
                    }
                check(found,"Test needs a visible wall pixel");
                const QPointF pick=QPointF(native_pick)/ratio;
                mouse(view,QEvent::MouseButtonPress,pick,Qt::LeftButton,Qt::LeftButton);
                mouse(view,QEvent::MouseButtonRelease,pick,Qt::LeftButton,Qt::NoButton);
                check(selected==wall_id,"Picking must map native pixels to the stable wall ID");
                mouse(view,QEvent::MouseButtonPress,{2,2},Qt::LeftButton,Qt::LeftButton);
                mouse(view,QEvent::MouseButtonRelease,{2,2},Qt::LeftButton,Qt::NoButton);
                check(selected.isEmpty(),"Empty-space picking must clear selection");
                const auto unchanged_source = document.snapshot();
                view.setSnapshot(unchanged_source, sketch::visualization::NativeModelView::VisibleEntityIds{});
                check(view.exportViewImage(temporary.filePath("hidden.png")), "Hidden view must export its current presentation");
                const QImage hidden(temporary.filePath("hidden.png"));
                check(!hidden.isNull(), "Hidden view export must be readable");
                const auto hidden_background = hidden.pixelColor(0, 0);
                for (int y = 0; y < hidden.height(); ++y) for (int x = 0; x < hidden.width(); ++x)
                    check(hidden.pixelColor(x, y) == hidden_background, "Visibility mask must erase native solids");
                mouse(view,QEvent::MouseButtonPress,pick,Qt::LeftButton,Qt::LeftButton);
                mouse(view,QEvent::MouseButtonRelease,pick,Qt::LeftButton,Qt::NoButton);
                check(selected.isEmpty(), "Hidden solids must not be pickable");
                const auto replacement = sketch::Document::create({sketch::Entity::create("wall",
                    unchanged_source.entities().at(wall_id.toStdString()).properties)});
                view.setSnapshot(replacement.snapshot(), sketch::visualization::NativeModelView::VisibleEntityIds{});
                check(view.exportViewImage(temporary.filePath("hidden-new.png")) &&
                      QImage(temporary.filePath("hidden-new.png")) == hidden,
                      "Newly built solids must also honor an empty visibility mask");
                view.setSnapshot(unchanged_source);
                const auto revealed = capture(view, temporary.filePath("revealed.png"));
                check(revealed.image == first.image, "Removing the filter must restore the same geometry and camera");
                check(document.revision() == unchanged_source.revision() &&
                      document.snapshot().entities() == unchanged_source.entities(), "View filtering must not edit the document");
                {
                    sketch::visualization::NativeModelView initially_hidden;
                    initially_hidden.setAttribute(Qt::WA_DontShowOnScreen, true);
                    initially_hidden.setAttribute(Qt::WA_ShowWithoutActivating, true);
                    initially_hidden.resize(view.size());
                    initially_hidden.setSnapshot(unchanged_source, sketch::visualization::NativeModelView::VisibleEntityIds{});
                    initially_hidden.show();
                    application.processEvents();
                    check(initially_hidden.exportViewImage(temporary.filePath("initially-hidden.png")) &&
                          QImage(temporary.filePath("initially-hidden.png")) == hidden,
                          "Initial native construction must honor hidden solids");
                    initially_hidden.setSnapshot(unchanged_source);
                    const auto initial_reveal = capture(initially_hidden, temporary.filePath("initial-reveal.png"));
                    check(initial_reveal.image == first.image,
                          "First visible geometry must be fitted after an initially empty view");
                }
                {
                    auto second_wall = unchanged_source.entities().at(wall_id.toStdString());
                    second_wall.id = "partial-mask-second-wall";
                    second_wall.properties["baseline"]["start"] = {12.0, 0.0};
                    second_wall.properties["baseline"]["end"] = {18.0, 0.0};
                    const auto pair = sketch::Document::create({
                        unchanged_source.entities().at(wall_id.toStdString()), second_wall});
                    sketch::visualization::NativeModelView partial;
                    partial.setAttribute(Qt::WA_DontShowOnScreen, true);
                    partial.setAttribute(Qt::WA_ShowWithoutActivating, true);
                    partial.resize(view.size());
                    partial.setSnapshot(pair.snapshot());
                    partial.show();
                    application.processEvents();
                    partial.fitAll();
                    const auto all = capture(partial, temporary.filePath("partial-all.png"));
                    partial.setSnapshot(pair.snapshot(),
                        sketch::visualization::NativeModelView::VisibleEntityIds{second_wall.id});
                    const auto second_only = capture(partial, temporary.filePath("partial-second.png"));
                    check(second_only.image != all.image, "Partial mask must remove only the first wall");
                    QPointF second_pick;
                    bool found_second = false;
                    for (int y = second_only.bounds.top() + 4; y < second_only.bounds.bottom() - 4 && !found_second; ++y)
                        for (int x = second_only.bounds.left() + 4; x < second_only.bounds.right() - 4; ++x) {
                            const auto pixel = second_only.image.pixelColor(x, y);
                            if (pixel.red() > pixel.blue() + 35 && pixel.green() > pixel.blue() + 20) {
                                second_pick = QPointF(x, y) / partial.devicePixelRatioF();
                                found_second = true;
                                break;
                            }
                        }
                    check(found_second, "Partial mask must retain a visible interior wall pixel");
                    QString partial_selected;
                    partial.onEntitySelected = [&](QString id) { partial_selected = std::move(id); };
                    mouse(partial, QEvent::MouseButtonPress, second_pick, Qt::LeftButton, Qt::LeftButton);
                    mouse(partial, QEvent::MouseButtonRelease, second_pick, Qt::LeftButton, Qt::NoButton);
                    check(partial_selected == QString::fromStdString(second_wall.id),
                          "Remaining partial-mask geometry must retain its stable picking identity");
                    partial.setSnapshot(pair.snapshot(),
                        sketch::visualization::NativeModelView::VisibleEntityIds{wall_id.toStdString()});
                    const auto first_only = capture(partial, temporary.filePath("partial-first.png"));
                    check(first_only.image != all.image && first_only.image != second_only.image,
                          "Switching partial masks must independently display each wall");
                    mouse(partial, QEvent::MouseButtonPress, second_pick, Qt::LeftButton, Qt::LeftButton);
                    mouse(partial, QEvent::MouseButtonRelease, second_pick, Qt::LeftButton, Qt::NoButton);
                    check(partial_selected.isEmpty(), "Excluded partial-mask geometry must not be pickable");
                    partial.setSnapshot(pair.snapshot());
                    check(capture(partial, temporary.filePath("partial-restored.png")).image == all.image,
                          "Removing a partial mask must restore both solids without moving the camera");
                }
                sketch::PersistentConstraint horizontal;
                horizontal.id = "native-horizontal";
                horizontal.relation = sketch::ConstraintRelationKind::horizontal;
                horizontal.bindings = {{wall_id.toStdString(), sketch::WallEndpointRole::start},
                                       {wall_id.toStdString(), sketch::WallEndpointRole::end}};
                sketch::ConstraintAuthoringIntent constrain;
                constrain.relation_mutations.push_back(sketch::ConstraintRelationMutation::upsert(horizontal));
                const auto relation_preview = sketch::preview_constraint_authoring(document.snapshot(), constrain);
                check(relation_preview.accepted(), "Native constraint fixture must preview");
                sketch::apply_constraint_authoring(document, relation_preview);
                view.setSnapshot(document.snapshot());
                check(capture(view, temporary.filePath("constrained.png")).image == first.image,
                      "Persistent relations must preserve native geometry and remain renderable");
                sketch::ConstraintAuthoringIntent resize;
                resize.wall_resize = sketch::WallResizeIntent{wall_id.toStdString(), sketch::parse_quantity("7 m"),
                    sketch::WallResizeAnchor::start, true};
                const auto resize_preview = sketch::preview_constraint_authoring(document.snapshot(), resize);
                check(resize_preview.accepted(), "Native constrained resize must preview");
                sketch::apply_constraint_authoring(document, resize_preview);
                view.setSnapshot(document.snapshot());
                check(capture(view, temporary.filePath("constrained-resize.png")).image != first.image,
                      "Constraint Apply must update the real native solid");
                document.undo(document.revision());
                view.setSnapshot(document.snapshot());
                check(capture(view, temporary.filePath("constraint-undo.png")).image == first.image,
                      "Undo of a constrained resize must restore native geometry");
                document.undo(document.revision());
                view.setSnapshot(document.snapshot());
                QPointF start(300,230),end=start+QPointF(14,12);
                mouse(view,QEvent::MouseButtonPress,start,Qt::MiddleButton,Qt::MiddleButton);
                mouse(view,QEvent::MouseMove,end,Qt::NoButton,Qt::MiddleButton);
                mouse(view,QEvent::MouseButtonRelease,end,Qt::MiddleButton,Qt::NoButton);
                auto panned=capture(view,temporary.filePath("panned.png"));
                check(panned.centre.x()>first.centre.x()+5 && panned.centre.y()>first.centre.y()+5,
                      "Pan must move the model with the pointer on both axes");
                view.resize(340,700);
                application.processEvents();
                view.fitAll();
                auto portrait=capture(view,temporary.filePath("portrait.png"));
                check(portrait.bounds.left()>2 && portrait.bounds.right()<portrait.image.width()-3,
                      "Fit must refresh aspect after a portrait resize");
                auto edited_wall=document.snapshot().entities().at(wall_id.toStdString());
                edited_wall.properties["height_m"]=1.4;
                document.apply(sketch::ApplyEntityChanges{document.revision(),
                    {sketch::EntityChange::upsert(edited_wall)},{},"Reduce wall height"});
                view.setSnapshot(document.snapshot());
                auto changed=capture(view,temporary.filePath("changed.png"));
                check(changed.image!=portrait.image,"Semantic edits must invalidate derived solids");
                document.undo(document.revision());
                view.setSnapshot(document.snapshot());
                auto undone=capture(view,temporary.filePath("undone.png"));
                check(undone.image==portrait.image,"Undo must restore the original derived geometry");
                document.redo(document.revision());
                view.setSnapshot(document.snapshot());
                auto redone=capture(view,temporary.filePath("redone.png"));
                check(redone.image==changed.image,"Redo must restore the edited derived geometry");
                auto catalog = sketch::Entity::create("assembly_model", {{"version",1},
                    {"model", sketch::AssemblyModel::create({{"finish","Finish","#e02020"}}, {}, {}).to_json()}});
                auto painted_wall = document.snapshot().entities().at(wall_id.toStdString());
                painted_wall.properties["material_assignment"] = {{"version",1},
                    {"catalog_id",catalog.id},{"material_id","finish"}};
                document.apply(sketch::ApplyEntityChanges{document.revision(),
                    {sketch::EntityChange::upsert(catalog),sketch::EntityChange::upsert(painted_wall)},{},"Assign red material"});
                view.setSnapshot(document.snapshot());
                auto red = capture(view,temporary.filePath("material-red.png"));
                check(red.image != redone.image && red.bounds == redone.bounds,
                    "material assignment changes appearance without changing geometry");
                catalog.properties["model"] = sketch::AssemblyModel::create({{"finish","Finish","#2020e0"}}, {}, {}).to_json();
                document.apply(sketch::ApplyEntityChanges{document.revision(),
                    {sketch::EntityChange::upsert(catalog)},{},"Change catalog color"});
                view.setSnapshot(document.snapshot());
                auto blue = capture(view,temporary.filePath("material-blue.png"));
                check(blue.image != red.image && blue.bounds == red.bounds,
                    "catalog color changes must refresh cached presentations");
                catalog.properties["model"] = sketch::AssemblyModel::create({{"finish","Finish"}}, {}, {}).to_json();
                document.apply(sketch::ApplyEntityChanges{document.revision(),
                    {sketch::EntityChange::upsert(catalog)},{},"Clear catalog color"});
                view.setSnapshot(document.snapshot());
                auto default_color = capture(view,temporary.filePath("material-default.png"));
                check(default_color.image == redone.image,"clearing a catalog color restores default shading");
                document.undo(document.revision());
                view.setSnapshot(document.snapshot());
                check(capture(view,temporary.filePath("material-clear-undo.png")).image == blue.image,
                    "undo restores a cleared color");
                document.undo(document.revision());
                view.setSnapshot(document.snapshot());
                auto restored_color = capture(view,temporary.filePath("material-undo.png"));
                check(restored_color.image == red.image,"undo restores the prior material appearance");
                document.undo(document.revision());
                view.setSnapshot(document.snapshot());
                auto unassigned = capture(view,temporary.filePath("material-unassigned.png"));
                check(unassigned.image == redone.image,"removing assignment restores default appearance");
            }
            if (scenario != "geometry") {
                // Keep the same portrait context as the combined sequence, even
                // when the forms scenario runs in its own guarded process.
                view.resize(340,700);
                application.processEvents();
                const std::vector<sketch::BuildingObject> building_objects{
                    sketch::RectangularColumn{"native-column", {0,0,0}, 0.4,0.5,3.0,0.2},
                    sketch::CircularColumn{"native-round-column", {0,0,0}, 0.25,3.0},
                    sketch::Beam{"native-beam", {0,0,2.5}, {4,1,3}, {0,0,1},0.2,0.35},
                    sketch::StairFlight{"native-stair", {0,0,0},0.0,12,2.4,0.25,1.2,
                        sketch::StairLanding{1.2,0.15}},
                    sketch::Railing{"native-railing", {0,3,0},0.0,4.0,1.1,0.08,1.0},
                    sketch::SlopedRoofPanel{"native-panel", {0,0,3},0.0,4,6,2,
                        std::atan(0.5),0.25,0.15},
                    sketch::GableRoof{"native-gable", {0,0,3},0.0,8,6,1.5,
                        std::atan(0.5),0.25,0.15},
                    sketch::HipRoof{"native-hip", {0,0,3},0.0,8,6,1.5,
                        std::atan(0.5),0.25,0.15},
                    sketch::HipRoof{"native-hip-opening", {0,0,3},0.0,8,6,1.5,
                        std::atan(0.5),0.25,0.15, {{"skylight", -1,-1,2,2}}},
                };
                for (const auto& object : building_objects) {
                    auto entity=sketch::encode_building_entity(object);
                    auto model=sketch::Document::create({entity});
                    view.setSnapshot(model.snapshot());
                    check(view.isReady(),"Supported building form must have native geometry");
                    view.fitAll();
                    auto frame=capture(view,temporary.filePath(QString::fromStdString(entity.id)+".png"));
                    check(!frame.bounds.isEmpty(),"Every supported form must render a solid");
                    if (entity.type == "column") {
                        QString translated_id;
                        double translated_x = 0.0;
                        double translated_y = 0.0;
                        double translated_z = 0.0;
                        view.onEntityTranslationRequested =
                            [&](QString id, double x, double y, double z) {
                                translated_id = std::move(id);
                                translated_x = x;
                                translated_y = y;
                                translated_z = z;
                            };
                        const auto start = frame.centre / view.devicePixelRatioF();
                        const auto end = start + QPointF(18.0, 12.0);
                        mouse(view, QEvent::MouseButtonPress, start, Qt::LeftButton,
                              Qt::LeftButton, Qt::ControlModifier);
                        mouse(view, QEvent::MouseMove, end, Qt::NoButton,
                              Qt::LeftButton, Qt::ControlModifier);
                        const auto preview = capture(
                            view, temporary.filePath(QString::fromStdString(entity.id) + "-translation-preview.png"));
                        check(preview.image != frame.image,
                              "Ctrl+left-drag must visibly preview the native translation");
                        mouse(view, QEvent::MouseButtonRelease, end, Qt::LeftButton,
                              Qt::NoButton, Qt::ControlModifier);
                        // The Ctrl press also selects the object. Clear that
                        // presentation highlight before comparing the restored
                        // frame with the pre-drag capture.
                        mouse(view, QEvent::MouseButtonPress, {2, 2}, Qt::LeftButton,
                              Qt::LeftButton);
                        mouse(view, QEvent::MouseButtonRelease, {2, 2}, Qt::LeftButton,
                              Qt::NoButton);
                        check(capture(view, temporary.filePath(QString::fromStdString(entity.id) +
                                                               "-translation-reset.png"))
                                  .image == frame.image,
                              "Releasing a native translation must clear the presentation preview");
                        check(translated_id == QString::fromStdString(entity.id),
                              "Ctrl+left-drag must request translation of the selected architectural object");
                        check(std::isfinite(translated_x) && std::isfinite(translated_y) &&
                                  std::isfinite(translated_z) &&
                                  (std::abs(translated_x) > 1.0e-9 ||
                                   std::abs(translated_y) > 1.0e-9 ||
                                   std::abs(translated_z) > 1.0e-9),
                              "Native translation request must contain a finite world-space delta");
                        view.onEntityTranslationRequested = {};
                    }
                    entity.properties["form"]="unsupported-future-form";
                    view.hide();
                    model.apply(sketch::ApplyEntityChanges{model.revision(),
                        {sketch::EntityChange::upsert(entity)},{},"Malformed future form"});
                    view.setSnapshot(model.snapshot(), sketch::visualization::NativeModelView::VisibleEntityIds{});
                    check(!view.isReady() && !view.lastError().isEmpty(),
                          "Invalid hidden building data must still surface a geometry error");
                    check(!view.exportViewImage(temporary.filePath("invalid.png")),
                          "Invalid building data must block successful image export");
                    model.undo(model.revision());
                    view.setSnapshot(model.snapshot());
                    check(view.isReady(),"Undo must restore a valid building presentation");
                    view.show();
                }
            }
            std::cout<<"Native "<<scenario.toStdString()<<" checks passed at DPR "<<ratio<<'\n';
            application.exit(0);
        } catch(const std::exception& error) {
            std::cerr<<error.what()<<'\n';application.exit(1);
        }
    });
    return application.exec();
}
