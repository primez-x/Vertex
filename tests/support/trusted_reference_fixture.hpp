#pragma once

#include "sketch/desktop/main_window.hpp"
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QUuid>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace sketch::testing {

// Only for locally generated fixture files and caller-owned fixture pixels.
// Exercise the real import first. If this source-build host cannot meet the
// sandbox installation gate, assert fail-closed behavior and seed Document
// commands so downstream editing/rendering coverage is not skipped.
inline QString importOrSeedTrustedReferenceFixture(desktop::MainWindow& window,
                                                   const QString& path, const QImage& pixels) {
    const auto revision = window.document().revision();
    const auto imported = window.importReferenceImage(path);
    if (!imported.isEmpty()) return imported;
    if (!window.lastError().contains("Isolated reference import is unavailable") ||
        window.document().revision() != revision || pixels.isNull())
        throw std::runtime_error("reference fixture import must succeed or fail closed at the sandbox gate");
    std::cout << "Sandbox unavailable: asserted rejection; seeding trusted reference editing fixture\n";
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("trusted reference fixture must open");
    const auto source = file.readAll();
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !pixels.save(&buffer, "PNG"))
        throw std::runtime_error("trusted reference preview fixture must encode");
    const auto bytes = [](const QByteArray& value) {
        std::vector<std::byte> result(static_cast<std::size_t>(value.size()));
        std::memcpy(result.data(), value.constData(), result.size());
        return result;
    };
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const auto source_id = id + "-source";
    const auto preview_id = id + "-preview";
    const auto mime = QFileInfo(path).suffix() == "pdf" ? "application/pdf" : "image/png";
    auto original = Asset::create(source_id, mime, bytes(source),
        {{"width_px", pixels.width()}, {"height_px", pixels.height()}, {"content", "raster-reference"}});
    auto preview = Asset::create(preview_id, "image/png", bytes(png), {{"content", "trusted-test-preview"}});
    auto entity = Entity::create("reference_asset", {
        {"asset_id", source_id}, {"render_asset_id", preview_id}, {"mime_type", mime},
        {"source_path", QFileInfo(path).fileName().toStdString()}, {"position_m", {0.0, 0.0}},
        {"metres_per_source_unit", 0.01}, {"scale", 1.0}, {"rotation_degrees", 0.0},
        {"flip_horizontal", false}, {"flip_vertical", false}, {"intensity", 0.72}, {"visible", true}});
    entity.id = id;
    window.document().apply(ApplyEntityChanges{revision, {EntityChange::upsert(std::move(entity))},
        {AssetChange::upsert(std::move(original)), AssetChange::upsert(std::move(preview))},
        "Seed trusted reference test fixture"});
    const auto selected = QString::fromStdString(id);
    if (!window.selectEntity(selected)) throw std::runtime_error("trusted reference fixture must select");
    return selected;
}
} // namespace sketch::testing
