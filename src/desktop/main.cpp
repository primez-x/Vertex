#include "sketch/desktop/main_window.hpp"
#include "sketch/noninteractive_errors.hpp"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSize>
#include <QTimer>
#include <QUuid>
#include <array>
#include <cstring>
#include <utility>

namespace {

QString smoke_output_path(const QStringList& arguments) {
    const auto index = arguments.indexOf(QStringLiteral("--smoke-output"));
    if (index >= 0 && index + 1 < arguments.size() && !arguments.at(index + 1).isEmpty()) {
        return arguments.at(index + 1);
    }
    return QDir::tempPath() + QStringLiteral("/vertex-desktop-smoke-%1.png")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

QString native_smoke_output_path(const QStringList& arguments) {
    const auto index = arguments.indexOf(QStringLiteral("--smoke-3d-output"));
    if (index >= 0 && index + 1 < arguments.size() && !arguments.at(index + 1).isEmpty()) {
        return arguments.at(index + 1);
    }
    return QDir::tempPath() + QStringLiteral("/vertex-desktop-smoke-3d-%1.png")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

QString smoke_project_output_path(const QStringList& arguments) {
    const auto index = arguments.indexOf(QStringLiteral("--smoke-project-output"));
    if (index >= 0 && index + 1 < arguments.size() && !arguments.at(index + 1).isEmpty()) {
        return arguments.at(index + 1);
    }
    return {};
}

QString smoke_project_input_path(const QStringList& arguments) {
    const auto index = arguments.indexOf(QStringLiteral("--smoke-project-input"));
    if (index >= 0 && index + 1 < arguments.size() && !arguments.at(index + 1).isEmpty()) {
        return arguments.at(index + 1);
    }
    return {};
}

QString smoke_performance_output_path(const QStringList& arguments) {
    const auto index = arguments.indexOf(QStringLiteral("--smoke-performance-output"));
    if (index >= 0 && index + 1 < arguments.size() && !arguments.at(index + 1).isEmpty()) {
        return arguments.at(index + 1);
    }
    return {};
}

QString smoke_market(const QStringList& arguments) {
    const auto index = arguments.indexOf(QStringLiteral("--smoke-market"));
    if (index >= 0 && index + 1 < arguments.size() && !arguments.at(index + 1).isEmpty()) {
        return arguments.at(index + 1).trimmed().toLower();
    }
    return QStringLiteral("residential");
}

QString smoke_theme(const QStringList& arguments) {
    const auto index = arguments.indexOf(QStringLiteral("--smoke-theme"));
    if (index >= 0 && index + 1 < arguments.size() && !arguments.at(index + 1).isEmpty()) {
        return arguments.at(index + 1).trimmed().toLower();
    }
    return QStringLiteral("light");
}

bool architectural_smoke(const QStringList& arguments) {
    const auto index = arguments.indexOf(QStringLiteral("--smoke-workspace"));
    if (index < 0 || index + 1 >= arguments.size()) {
        return false;
    }
    const auto workspace = arguments.at(index + 1).trimmed().toLower();
    return workspace == QStringLiteral("architectural");
}

bool smoke_workspace_is_valid(const QStringList& arguments) {
    const auto index = arguments.indexOf(QStringLiteral("--smoke-workspace"));
    if (index < 0) {
        return true;
    }
    if (index + 1 >= arguments.size()) {
        return false;
    }
    const auto workspace = arguments.at(index + 1).trimmed().toLower();
    return workspace == QStringLiteral("measurement") ||
           workspace == QStringLiteral("architectural");
}

QSize smoke_size(const QStringList& arguments) {
    const auto index = arguments.indexOf(QStringLiteral("--smoke-size"));
    if (index >= 0 && index + 1 < arguments.size()) {
        const auto match = QRegularExpression(QStringLiteral(R"(^([0-9]+)x([0-9]+)$)"))
                               .match(arguments.at(index + 1));
        if (match.hasMatch()) {
            const auto width = match.captured(1).toInt();
            const auto height = match.captured(2).toInt();
            if (width >= 640 && height >= 480) {
                return {width, height};
            }
        }
    }
    return {1366, 768};
}

bool loadBundledFont(const QApplication& application) {
    const QStringList candidates{
        QStringLiteral(":/fonts/Inter.ttf"),
        QStringLiteral(":/fonts/InterVariable.ttf"),
        QDir(application.applicationDirPath()).filePath(QStringLiteral("fonts/Inter.ttf")),
        QDir(application.applicationDirPath()).filePath(QStringLiteral("fonts/InterVariable.ttf")),
    };
    for (const auto& candidate : candidates) {
        const auto font_id = QFontDatabase::addApplicationFont(candidate);
        if (font_id >= 0) {
            const auto families = QFontDatabase::applicationFontFamilies(font_id);
            if (!families.isEmpty()) {
                QFont font(families.front());
                font.setPointSize(9);
                application.setFont(font);
                return true;
            }
        }
    }
    return false;
}

bool seed_smoke_document(sketch::desktop::MainWindow& window, bool architectural,
                         const QString& market) {
    const auto commercial = market == QStringLiteral("light-commercial");
    if (!window.selectEntity(QStringLiteral("property-1")) ||
        !window.editProjectSubject(
            commercial ? QStringLiteral("Light-commercial smoke")
                       : QStringLiteral("Residential smoke"),
            QStringLiteral("1 Vertex Way"),
            QStringLiteral("installed-runtime-%1").arg(market),
            QStringLiteral("{\"market\":\"%1\",\"fixture\":\"installed-runtime-v1\"}")
                .arg(market))) {
        return false;
    }
    if (architectural) {
        const auto wall_id = window.createStraightWall(
            {0.0, 0.0}, {12.0, 0.0}, QStringLiteral("exterior"));
        if (wall_id.isEmpty() || !window.selectEntity(wall_id) ||
            window.createHostedOpening(QStringLiteral("door"), QStringLiteral("3 ft"),
                                       QStringLiteral("3 ft"), QStringLiteral("0 in"),
                                       QStringLiteral("7 ft"), std::nullopt,
                                       sketch::DoorOperation{false, true, 90.0})
                .isEmpty()) {
            return false;
        }
        if (window.createStraightWall({0.0, 0.0}, {0.0, 8.0}, QStringLiteral("exterior"))
                .isEmpty()) {
            return false;
        }
        const auto slab_id = window.createSlabFromBoundary(
                         sketch::Boundary{
                             {{0.0, 0.0}, {12.0, 0.0}, 0.0},
                             {{12.0, 0.0}, {12.0, 8.0}, 0.0},
                             {{12.0, 8.0}, {0.0, 8.0}, 0.0},
                             {{0.0, 8.0}, {0.0, 0.0}, 0.0},
                         },
                         QStringLiteral("6 in"), QStringLiteral("0 in"));
        if (slab_id.isEmpty()) return false;
        if (commercial &&
            window.createAnnotationSymbol(QStringLiteral("checkout-counter"), {6.0, 4.0})
                .isEmpty()) {
            return false;
        }
        return true;
    }
    const auto boundary_id = window.createBoundary(sketch::Boundary{
        {{0.0, 0.0}, {12.0, 0.0}, 0.0},
        {{12.0, 0.0}, {12.0, 8.0}, 0.0},
        {{12.0, 8.0}, {0.0, 8.0}, 0.0},
        {{0.0, 8.0}, {0.0, 0.0}, 0.0},
    }, QStringLiteral("living_area"));
    if (boundary_id.isEmpty()) {
        return false;
    }
    const auto boundary_snapshot = window.document().snapshot();
    const auto boundary_found = boundary_snapshot.entities().find(boundary_id.toStdString());
    if (boundary_found == boundary_snapshot.entities().end() ||
        !boundary_found->second.properties.contains("segments") ||
        boundary_found->second.properties.at("segments").size() != 4) {
        return false;
    }
    const std::array dimension_positions{
        sketch::Vec2{6.0, -0.55}, sketch::Vec2{12.55, 4.0},
        sketch::Vec2{6.0, 8.55}, sketch::Vec2{-0.55, 4.0},
    };
    for (std::size_t index = 0; index < dimension_positions.size(); ++index) {
        const auto& encoded = boundary_found->second.properties.at("segments").at(index);
        if (!encoded.contains("segment_id") || !encoded.at("segment_id").is_string() ||
            window.createLengthDimension(boundary_id,
                QString::fromStdString(encoded.at("segment_id").get<std::string>()),
                dimension_positions[index]).isEmpty()) {
            return false;
        }
    }
    const auto rectangle = [](double left, double bottom, double right, double top) {
        return sketch::Boundary{
            {{left, bottom}, {right, bottom}, 0.0},
            {{right, bottom}, {right, top}, 0.0},
            {{right, top}, {left, top}, 0.0},
            {{left, top}, {left, bottom}, 0.0},
        };
    };
    const std::array rooms{
        std::pair{QStringLiteral("Bedroom"), rectangle(0.0, 4.0, 4.0, 8.0)},
        std::pair{QStringLiteral("Dining"), rectangle(4.0, 4.0, 8.0, 8.0)},
        std::pair{QStringLiteral("Kitchen"), rectangle(8.0, 4.0, 12.0, 8.0)},
        std::pair{QStringLiteral("Bedroom"), rectangle(0.0, 0.0, 4.0, 4.0)},
        std::pair{QStringLiteral("Living Room"), rectangle(4.0, 0.0, 8.0, 4.0)},
        std::pair{QStringLiteral("Bath"), rectangle(8.0, 0.0, 12.0, 4.0)},
    };
    for (const auto& [name, room] : rooms) {
        if (window.createRoomBoundary(room, name).isEmpty()) return false;
    }
    if (window.createBoundary(rectangle(8.0, -6.0, 16.0, 0.0),
                              QStringLiteral("Garage")).isEmpty() ||
        window.createBoundary(rectangle(0.0, -2.0, 5.0, 0.0),
                              QStringLiteral("Porch")).isEmpty() ||
        window.createBoundary(rectangle(5.0, 8.0, 8.0, 10.0),
                              QStringLiteral("Patio")).isEmpty()) {
        return false;
    }
    const auto exterior_wall = window.createStraightWall(
        {0.0, 8.0}, {12.0, 8.0}, QStringLiteral("exterior"));
    if (exterior_wall.isEmpty() || !window.selectEntity(exterior_wall) ||
        window.createHostedOpening(QStringLiteral("window"), QStringLiteral("2 m"),
                                   QStringLiteral("1.6 m"), QStringLiteral("0.9 m"),
                                   QStringLiteral("1.2 m")).isEmpty()) {
        return false;
    }
    const auto interior_wall = window.createStraightWall(
        {8.0, 0.0}, {8.0, 8.0}, QStringLiteral("interior"));
    if (interior_wall.isEmpty() || !window.selectEntity(interior_wall) ||
        window.createHostedOpening(QStringLiteral("door"), QStringLiteral("1.2 m"),
                                   QStringLiteral("0.9 m"), QStringLiteral("0 m"),
                                   QStringLiteral("2.1 m"), std::nullopt,
                                   sketch::DoorOperation{false, true, 90.0}).isEmpty()) {
        return false;
    }
    const std::array symbols{
        std::pair{QStringLiteral("double-bed"), sketch::Vec2{2.0, 6.0}},
        std::pair{QStringLiteral("double-bed"), sketch::Vec2{2.0, 2.0}},
        std::pair{QStringLiteral("dining-table"), sketch::Vec2{6.0, 6.0}},
        std::pair{QStringLiteral("sofa"), sketch::Vec2{6.0, 2.0}},
        std::pair{QStringLiteral("refrigerator"), sketch::Vec2{9.0, 7.0}},
        std::pair{QStringLiteral("range"), sketch::Vec2{10.2, 7.0}},
        std::pair{QStringLiteral("kitchen-island-sink"), sketch::Vec2{10.0, 5.5}},
        std::pair{QStringLiteral("toilet"), sketch::Vec2{9.0, 2.7}},
        std::pair{QStringLiteral("bathtub"), sketch::Vec2{10.7, 1.2}},
        std::pair{QStringLiteral("sink"), sketch::Vec2{11.0, 3.0}},
    };
    for (const auto& [symbol, position] : symbols) {
        if (window.createAnnotationSymbol(symbol, position).isEmpty()) return false;
    }
    // End with a non-canvas container selected so the visual smoke output is
    // a clean production plan rather than a selection-state demonstration.
    return window.selectEntity(QStringLiteral("property-1"));
}

bool seed_smoke_reference(sketch::desktop::MainWindow& window) {
    const auto path = QDir::temp().filePath(
        QStringLiteral("vertex-reference-smoke-%1.png")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QImage image(600, 400, QImage::Format_ARGB32);
    image.fill(QColor(217, 232, 242));
    {
        QPainter painter(&image);
        painter.setPen(QPen(QColor(120, 150, 170), 3));
        for (int x = 0; x <= image.width(); x += 50) painter.drawLine(x, 0, x, image.height());
        for (int y = 0; y <= image.height(); y += 50) painter.drawLine(0, y, image.width(), y);
        painter.setPen(QPen(QColor(72, 104, 125), 8));
        painter.drawRect(24, 24, image.width() - 48, image.height() - 48);
    }
    if (!image.save(path, "PNG")) return false;
    const auto id = window.importReferenceImage(path);
    QFile::remove(path);
    if (id.isEmpty()) return false;
    return window.editReferenceTransform(id, QStringLiteral("6"), QStringLiteral("4"),
                                         QStringLiteral("0.02"), QStringLiteral("1"),
                                         QStringLiteral("0"), QStringLiteral("0.18"),
                                         false, false, true);
}

}  // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--smoke") == 0) {
            sketch::runtime::configure_noninteractive_errors();
            break;
        }
    }
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("Vertex"));
    application.setApplicationDisplayName(QStringLiteral("Vertex"));
    application.setOrganizationName(QStringLiteral("Vertex"));
    const auto font_loaded = loadBundledFont(application);
    const auto smoke = application.arguments().contains(QStringLiteral("--smoke"));
    const auto architectural = architectural_smoke(application.arguments());
    const auto market = smoke_market(application.arguments());
    const auto theme = smoke_theme(application.arguments());
    if (smoke && !smoke_workspace_is_valid(application.arguments())) {
        qCritical() << "Vertex: --smoke-workspace must be measurement or architectural";
        return 2;
    }
    if (smoke && market != QStringLiteral("residential") &&
        market != QStringLiteral("light-commercial")) {
        qCritical() << "Vertex: --smoke-market must be residential or light-commercial";
        return 2;
    }
    if (smoke && theme != QStringLiteral("light") && theme != QStringLiteral("dark") &&
        theme != QStringLiteral("high-contrast")) {
        qCritical() << "Vertex: --smoke-theme must be light, dark, or high-contrast";
        return 2;
    }
    if (!font_loaded) {
        qWarning() << "Vertex: bundled Inter font was not loaded";
        if (smoke) {
            qCritical() << "Vertex: refusing visual smoke capture without bundled font";
            return 3;
        }
    }

    sketch::desktop::MainWindow window;
    if (smoke) {
        window.setAttribute(Qt::WA_DontShowOnScreen, true);
        window.setAttribute(Qt::WA_ShowWithoutActivating, true);
        window.resize(smoke_size(application.arguments()));
        if (application.arguments().contains(QStringLiteral("--smoke-assistance-disabled"))) {
            window.setAssistanceEnabled(false);
            if (window.assistanceEnabled()) {
                qCritical() << "Vertex: smoke assistance could not be disabled";
                return 2;
            }
        }
        const auto project_input = smoke_project_input_path(application.arguments());
        if (project_input.isEmpty()) {
            if (!seed_smoke_document(window, architectural, market)) {
                return 2;
            }
        } else if (!window.openProject(project_input)) {
            qCritical() << "Vertex: smoke project reopen failed:" << window.lastError();
            return 2;
        }
        if (application.arguments().contains(QStringLiteral("--smoke-reference")) &&
            !seed_smoke_reference(window)) {
            qCritical() << "Vertex: reference underlay smoke fixture failed";
            return 2;
        }
        window.setWorkspaceTheme(theme == QStringLiteral("dark")
            ? sketch::WorkspaceTheme::dark
            : theme == QStringLiteral("high-contrast")
                ? sketch::WorkspaceTheme::high_contrast
                : sketch::WorkspaceTheme::light);
        window.fitView();
    }
    window.show();

    if (!smoke) {
        QTimer::singleShot(0, &window, [&window] { (void)window.offerStartupRecovery(); });
    }

    if (smoke) {
        const auto output = smoke_output_path(application.arguments());
        const auto native_output = native_smoke_output_path(application.arguments());
        const auto project_output = smoke_project_output_path(application.arguments());
        const auto performance_output = smoke_performance_output_path(application.arguments());
        QTimer::singleShot(500, &window,
                           [&application, &window, output, native_output, project_output,
                            performance_output,
                            architectural] {
            window.fitView();
            // Selection is local presentation state and is not persisted in a
            // project. Clear it before both source and reopened captures so
            // their screenshots compare the document rather than an
            // inspector highlight left by the seed fixture.
            (void)window.selectEntity(QString{});
            if (!project_output.isEmpty() && !window.saveProjectAs(project_output)) {
                qCritical() << "Vertex: smoke project save failed:" << window.lastError();
                application.exit(5);
                return;
            }
            // Saving and clearing selection refreshes the inspector and
            // navigator asynchronously. Wait for that presentation state to
            // settle before exporting the native view and grabbing the shell,
            // otherwise source and reopened captures can differ only because
            // one process was caught mid-refresh.
            QTimer::singleShot(150, &window,
                               [&application, &window, output, native_output,
                                performance_output,
                                architectural] {
                window.fitView();
                if (architectural && !window.exportNativeViewImage(native_output)) {
                    qCritical() << "Vertex: native 3D smoke export failed:"
                                << window.lastError();
                    application.exit(4);
                    return;
                }
                // Qt window grabs cannot include the native OCCT child surface.
                // Keep the dedicated 3D export above, then collapse that child so
                // the architectural UI capture contains no misleading black pane.
                if (architectural) {
                    window.setNativeModelViewVisible(false);
                    application.processEvents();
                }
                const auto image = window.grab();
                if (image.isNull() || !image.save(output)) {
                    qCritical() << "Vertex: visual smoke capture failed:" << output;
                    application.exit(2);
                    return;
                }
                if (!performance_output.isEmpty()) {
                    QSaveFile report(performance_output);
                    if (!report.open(QIODevice::WriteOnly | QIODevice::Text) ||
                        report.write(window.performanceReportJson().toUtf8()) < 0 ||
                        !report.commit()) {
                        qCritical() << "Vertex: performance report write failed:" << performance_output;
                        application.exit(6);
                        return;
                    }
                }
                application.exit(0);
            });
        });
    }
    return application.exec();
}
