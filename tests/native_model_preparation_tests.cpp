#include "sketch/visualization/native_model_view.hpp"
#include "sketch/document.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QByteArray>
#include <QLabel>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
using namespace sketch;
using sketch::visualization::NativeModelView;

void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

Document wall_document(double height = 2.8) {
    return Document::create({Entity::create("wall", {
        {"baseline", {{"start", {0.0, 0.0}}, {"end", {4.0, 0.0}},
                      {"sweep_radians", 0.0}}},
        {"thickness_m", 0.2}, {"height_m", height}, {"elevation_m", 0.0}})});
}

void await_preparation(NativeModelView& view) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    // Deliberately never process Qt events: callers without an event loop must
    // be able to collect the real worker result on the widget's owner thread.
    do {
        view.pollGeometryPreparation();
        if (!view.isGeometryPending()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("native model preparation timed out");
}
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    try {
        NativeModelView view;
        view.setAttribute(Qt::WA_DontShowOnScreen, true);
        check(!view.isGeometryPrepared(), "an unrequested view is not prepared");
        check(!view.lastPublicationMetrics(), "an unrequested view has no native publication measurements");
        int requests = 0;
        view.setErrorCallback([&](const QString& message) {
            if (message.startsWith(QStringLiteral("Preparing 3D geometry"))) ++requests;
        });
        auto document = wall_document();
        const auto first = document.snapshot();
        {
            NativeModelView throwing_observer;
            throwing_observer.setAttribute(Qt::WA_DontShowOnScreen, true);
            throwing_observer.setErrorCallback([](const QString&) {
                throw std::runtime_error("observer failure must not own geometry state");
            });
            throwing_observer.setSnapshot(first);
            auto* timer = throwing_observer.findChild<QTimer*>();
            check(timer && timer->isActive() && throwing_observer.isGeometryPending(),
                  "a throwing status observer must not interrupt preparation scheduling");
            await_preparation(throwing_observer);
            check(throwing_observer.isGeometryPrepared() &&
                  throwing_observer.lastError().isEmpty(),
                  "real geometry must complete despite a throwing status observer");
            throwing_observer.setSnapshot(wall_document(-1.0).snapshot());
            await_preparation(throwing_observer);
            check(!throwing_observer.isGeometryPrepared() &&
                  throwing_observer.lastError().startsWith(
                      QStringLiteral("3D geometry is incomplete:")) &&
                  !throwing_observer.lastError().contains(QStringLiteral("observer failure")),
                  "a throwing observer must preserve the actual geometry diagnostic");
            throwing_observer.setSnapshot(first);
            await_preparation(throwing_observer);
            check(throwing_observer.isGeometryPrepared(),
                  "preparation must recover after an invalid snapshot with a throwing observer");
            check(!throwing_observer.exportViewImage(QString()) &&
                  throwing_observer.lastError().contains(QStringLiteral("not ready")),
                  "a throwing observer must preserve an export readiness diagnostic");
            QShowEvent unavailable_event;
            QApplication::sendEvent(&throwing_observer, &unavailable_event);
            check(throwing_observer.lastError().contains(QStringLiteral("platform 'offscreen'")) &&
                  !throwing_observer.isVisible(),
                  "a throwing observer must preserve the native initialization diagnostic");
        }
        view.setSnapshot(first);
        check(view.isGeometryPending() && !view.isGeometryPrepared(),
              "a new wall request must await real preparation");
        auto* preparation_label = view.findChild<QLabel*>();
        check(preparation_label && !preparation_label->isHidden(),
              "pending preparation must expose its compact status banner");
        const auto previous_size = view.size();
        view.resize(800, 600);
        QResizeEvent pending_resize(view.size(), previous_size);
        QApplication::sendEvent(&view, &pending_resize);
        check(preparation_label->height() <= preparation_label->sizeHint().height(),
              "resizing during preparation must keep the retained scene uncovered");
        for (int i = 0; i < 10; ++i) view.setSnapshot(document.snapshot());
        check(requests == 1, "identical pending snapshots must not restart preparation");
        await_preparation(view);
        check(view.isGeometryPrepared(), "polling must prepare actual wall geometry");
        check(!view.isVisible() && !view.isReady(),
              "semantic preparation must not require or create a visible native view");
        check(view.lastError().isEmpty(), "completed hidden geometry must clear pending status");
        view.setSnapshot(document.snapshot());
        check(requests == 1 && view.isGeometryPrepared() && !view.isGeometryPending(),
              "an unchanged completed snapshot must retain preparation");
        check(!view.exportViewImage(QString()), "a prepared hidden view cannot export a framebuffer");
        check(view.isGeometryPrepared(), "an export error must not invalidate semantic geometry");
        const auto export_error = view.lastError();
        view.pollGeometryPreparation();
        check(view.lastError() == export_error, "polling must preserve an existing operation error");

        auto wall = first.entities().begin()->second;
        wall.properties["height_m"] = 4.0;
        document.apply(ApplyEntityChanges{document.revision(),
            {EntityChange::upsert(wall)}, {}, "native preparation revision test"});
        view.setSnapshot(document.snapshot());
        check(!view.isGeometryPrepared() && view.isGeometryPending(),
              "changed revision must invalidate previous preparation immediately");
        await_preparation(view);
        check(view.isGeometryPrepared(), "changed wall revision must prepare successfully");

        view.setSnapshot(document.snapshot(), NativeModelView::VisibleEntityIds{});
        check(!view.isGeometryPrepared() && view.isGeometryPending(),
              "a changed visibility mask must request new preparation");
        await_preparation(view);
        check(view.isGeometryPrepared(), "hidden valid wall must still be validated");
        const auto mask_requests = requests;
        view.setSnapshot(document.snapshot(), NativeModelView::VisibleEntityIds{});
        check(requests == mask_requests && !view.isGeometryPending(),
              "identical visibility masks must not restart preparation");

        auto fork_valid = Document::fork(first);
        auto fork_invalid = Document::fork(first);
        auto fork_taller = Document::fork(first);
        for (auto [fork, height] : {std::pair{&fork_valid, 4.0},
                                   std::pair{&fork_invalid, -1.0},
                                   std::pair{&fork_taller, 6.0}}) {
            auto fork_wall = first.entities().begin()->second;
            fork_wall.properties["height_m"] = height;
            fork->apply(ApplyEntityChanges{fork->revision(),
                {EntityChange::upsert(fork_wall)}, {}, "divergent native geometry fork"});
        }
        const auto valid_fork_snapshot = fork_valid.snapshot();
        const auto invalid_fork_snapshot = fork_invalid.snapshot();
        const auto taller_fork_snapshot = fork_taller.snapshot();
        check(valid_fork_snapshot.document_id() == invalid_fork_snapshot.document_id() &&
              valid_fork_snapshot.document_id() == taller_fork_snapshot.document_id() &&
              valid_fork_snapshot.revision() == invalid_fork_snapshot.revision() &&
              valid_fork_snapshot.revision() == taller_fork_snapshot.revision(),
              "divergent forks must share document identity and revision");
        check(valid_fork_snapshot.entities().begin()->second.properties.at("height_m") == 4.0 &&
              invalid_fork_snapshot.entities().begin()->second.properties.at("height_m") == -1.0 &&
              taller_fork_snapshot.entities().begin()->second.properties.at("height_m") == 6.0,
              "equal-revision fork fixtures must contain distinct wall heights");
        view.setSnapshot(valid_fork_snapshot);
        await_preparation(view);
        check(view.isGeometryPrepared(), "valid fork must prepare before invalid replacement");
        view.setSnapshot(invalid_fork_snapshot);
        check(!view.isGeometryPrepared() && view.isGeometryPending(),
              "invalid fork at equal identity and revision must invalidate prepared geometry");
        await_preparation(view);
        check(!view.isGeometryPrepared() && !view.isReady() && !view.lastError().isEmpty(),
              "invalid fork must fail preparation instead of retaining its valid sibling");
        const auto fork_failure = view.lastError();
        const auto fork_failed_requests = requests;
        view.setSnapshot(fork_invalid.snapshot());
        check(requests == fork_failed_requests && !view.isGeometryPending() &&
              !view.isGeometryPrepared() && view.lastError() == fork_failure,
              "unchanged invalid fork must retain failure without new work");

        view.setSnapshot(valid_fork_snapshot);
        await_preparation(view);
        check(view.isGeometryPrepared(), "valid sibling must recover from invalid fork");
        const auto before_taller_requests = requests;
        view.setSnapshot(taller_fork_snapshot);
        check(requests == before_taller_requests + 1 &&
              !view.isGeometryPrepared() && view.isGeometryPending(),
              "different valid fork content must request fresh preparation at equal revision");
        view.setSnapshot(fork_taller.snapshot());
        check(requests == before_taller_requests + 1,
              "unchanged pending fork must not restart preparation");
        await_preparation(view);
        check(view.isGeometryPrepared() && view.lastError().isEmpty(),
              "different valid fork must complete its own preparation");
        view.setSnapshot(fork_taller.snapshot());
        check(requests == before_taller_requests + 1 && view.isGeometryPrepared() &&
              !view.isGeometryPending(),
              "unchanged completed fork must retain preparation");

        // Ordinary JSON equality conflates these representations, but the
        // authoritative document state and prepared-solid content do not.
        for (bool integer_height : {true, false}) {
            auto representation_fork = Document::fork(first);
            auto representation_wall = first.entities().begin()->second;
            representation_wall.properties["height_m"] = integer_height
                ? nlohmann::json(4) : nlohmann::json(4.0);
            representation_wall.properties["elevation_m"] = integer_height ? 0.0 : -0.0;
            representation_fork.apply(ApplyEntityChanges{representation_fork.revision(),
                {EntityChange::upsert(representation_wall)}, {}, "exact numeric content"});
            const auto representation_snapshot = representation_fork.snapshot();
            check(representation_snapshot.document_id() == valid_fork_snapshot.document_id() &&
                  representation_snapshot.revision() == valid_fork_snapshot.revision() &&
                  representation_snapshot.entities() == valid_fork_snapshot.entities() &&
                  representation_wall.properties.dump() !=
                      valid_fork_snapshot.entities().begin()->second.properties.dump(),
                  "numeric fixture must differ in exact content despite equal JSON values");
            view.setSnapshot(valid_fork_snapshot);
            await_preparation(view);
            const auto representation_requests = requests;
            view.setSnapshot(representation_snapshot);
            check(requests == representation_requests + 1 && view.isGeometryPending() &&
                  !view.isGeometryPrepared(),
                  "integer/float and signed-zero changes must invalidate content identity");
            await_preparation(view);
            check(view.isGeometryPrepared(), "valid numeric representation must prepare");
            view.setSnapshot(representation_fork.snapshot());
            check(requests == representation_requests + 1 && !view.isGeometryPending(),
                  "unchanged exact numeric representation must retain preparation");
        }

        auto valid_replacement = wall_document();
        auto invalid_replacement = wall_document(-1.0);
        check(valid_replacement.revision() == invalid_replacement.revision() &&
              valid_replacement.snapshot().document_id() != invalid_replacement.snapshot().document_id(),
              "replacement fixture must use different identities at equal revisions");
        view.setSnapshot(valid_replacement.snapshot());
        await_preparation(view);
        check(view.isGeometryPrepared(), "replacement valid wall must prepare");
        view.setSnapshot(invalid_replacement.snapshot());
        check(!view.isGeometryPrepared() && view.isGeometryPending(),
              "a different document at equal revision must invalidate preparation");
        await_preparation(view);
        check(!view.isGeometryPrepared() && !view.isReady() && !view.lastError().isEmpty(),
              "invalid wall geometry must fail readiness with a diagnostic");
        const auto invalid_error = view.lastError();
        const auto failed_requests = requests;
        view.setSnapshot(invalid_replacement.snapshot());
        view.pollGeometryPreparation();
        check(requests == failed_requests && !view.isGeometryPending() &&
              !view.isGeometryPrepared() && view.lastError() == invalid_error,
              "unchanged invalid geometry must retain its failure without restarting");
        view.setSnapshot(invalid_replacement.snapshot(), NativeModelView::VisibleEntityIds{});
        await_preparation(view);
        check(!view.isGeometryPrepared(), "visibility cannot bypass invalid geometry");

        // Supersede an uncollected invalid request with a valid document at the
        // same revision: only the newest request may determine readiness.
        view.setSnapshot(invalid_replacement.snapshot());
        view.setSnapshot(valid_replacement.snapshot());
        await_preparation(view);
        check(view.isGeometryPrepared() && view.lastError().isEmpty(),
              "superseded failure must not poison latest valid geometry");

        // Exercise Qt's offscreen native-initialization failure synchronously,
        // without showing the widget or creating a visible native window.
        QShowEvent show_event;
        QApplication::sendEvent(&view, &show_event);
        const auto native_error = view.lastError();
        check(native_error.contains(QStringLiteral("Native OCCT 3D view unavailable")),
              "offscreen initialization must report a native-view error");
        view.setSnapshot(first);
        await_preparation(view);
        check(view.isGeometryPrepared() && !view.isReady() && view.lastError() == native_error,
              "semantic completion must preserve native errors and reject GUI readiness");
        check(!view.lastPublicationMetrics(),
              "offscreen geometry completion must not fabricate native publication measurements");
        check(!view.isVisible(), "preparation tests must never show a native window");
        std::cout << "native model preparation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
