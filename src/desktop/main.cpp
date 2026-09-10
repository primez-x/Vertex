#include "sketch/desktop/main_window.hpp"
#include "../../tests/support/noninteractive_errors.hpp"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QRegularExpression>
#include <QSize>
#include <QTimer>
#include <QUuid>
#include <cstring>

namespace {

QString smoke_output_path(const QStringList& arguments) {
    const auto index = arguments.indexOf(QStringLiteral("--smoke-output"));
    if (index >= 0 && index + 1 < arguments.size() && !arguments.at(index + 1).isEmpty()) {
        return arguments.at(index + 1);
    }
    return QDir::tempPath() + QStringLiteral("/property-studio-desktop-smoke-%1.png")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

QString native_smoke_output_path(const QStringList& arguments) {
    const auto index = arguments.indexOf(QStringLiteral("--smoke-3d-output"));
    if (index >= 0 && index + 1 < arguments.size() && !arguments.at(index + 1).isEmpty()) {
        return arguments.at(index + 1);
    }
    return QDir::tempPath() + QStringLiteral("/property-studio-desktop-smoke-3d-%1.png")
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
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

bool seed_smoke_document(sketch::desktop::MainWindow& window, bool architectural) {
    if (architectural) {
        const auto wall_id = window.createStraightWall(
            {0.0, 0.0}, {12.0, 0.0}, QStringLiteral("exterior"));
        if (wall_id.isEmpty() || !window.selectEntity(wall_id) ||
            window.createHostedOpening(QStringLiteral("door"), QStringLiteral("3 ft"),
                                       QStringLiteral("3 ft"), QStringLiteral("0 in"),
                                       QStringLiteral("7 ft"))
                .isEmpty()) {
            return false;
        }
        if (window.createStraightWall({0.0, 0.0}, {0.0, 8.0}, QStringLiteral("exterior"))
                .isEmpty()) {
            return false;
        }
        return !window.createSlabFromBoundary(
                         sketch::Boundary{
                             {{0.0, 0.0}, {12.0, 0.0}, 0.0},
                             {{12.0, 0.0}, {12.0, 8.0}, 0.0},
                             {{12.0, 8.0}, {0.0, 8.0}, 0.0},
                             {{0.0, 8.0}, {0.0, 0.0}, 0.0},
                         },
                         QStringLiteral("6 in"), QStringLiteral("0 in"))
                         .isEmpty();
    }
    const auto boundary_id = window.createBoundary(sketch::Boundary{
        {{0.0, 0.0}, {12.0, 0.0}, 0.0},
        {{12.0, 0.0}, {12.0, 8.0}, 0.0},
        {{12.0, 8.0}, {0.0, 8.0}, 0.0},
        {{0.0, 8.0}, {0.0, 0.0}, 0.0},
    });
    if (boundary_id.isEmpty()) {
        return false;
    }
    if (window.createStraightWall({4.0, 0.0}, {4.0, 8.0}, QStringLiteral("interior")).isEmpty()) {
        return false;
    }
    return !window.createStraightWall({8.0, 0.0}, {8.0, 8.0}, QStringLiteral("interior")).isEmpty() &&
           window.selectEntity(boundary_id);
}

}  // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--smoke") == 0) {
            sketch::testing::noninteractive_errors();
            break;
        }
    }
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("Property Studio"));
    application.setApplicationDisplayName(QStringLiteral("Property Studio"));
    application.setOrganizationName(QStringLiteral("Property Studio"));
    const auto font_loaded = loadBundledFont(application);
    const auto smoke = application.arguments().contains(QStringLiteral("--smoke"));
    const auto architectural = architectural_smoke(application.arguments());
    if (smoke && !smoke_workspace_is_valid(application.arguments())) {
        qCritical() << "Property Studio: --smoke-workspace must be measurement or architectural";
        return 2;
    }
    if (!font_loaded) {
        qWarning() << "Property Studio: bundled Inter font was not loaded";
        if (smoke) {
            qCritical() << "Property Studio: refusing visual smoke capture without bundled font";
            return 3;
        }
    }

    sketch::desktop::MainWindow window;
    if (smoke) {
        window.setAttribute(Qt::WA_DontShowOnScreen, true);
        window.setAttribute(Qt::WA_ShowWithoutActivating, true);
        window.resize(smoke_size(application.arguments()));
        if (!seed_smoke_document(window, architectural)) {
            return 2;
        }
        window.fitView();
    }
    window.show();

    if (smoke) {
        const auto output = smoke_output_path(application.arguments());
        const auto native_output = native_smoke_output_path(application.arguments());
        QTimer::singleShot(500, &window,
                           [&application, &window, output, native_output, architectural] {
            window.fitView();
            if (architectural && !window.exportNativeViewImage(native_output)) {
                qCritical() << "Property Studio: native 3D smoke export failed:"
                            << window.lastError();
                application.exit(4);
                return;
            }
            const auto image = window.grab();
            if (image.isNull() || !image.save(output)) {
                qCritical() << "Property Studio: visual smoke capture failed:" << output;
                application.exit(2);
                return;
            }
            application.exit(0);
        });
    }
    return application.exec();
}
