#pragma once

#include "sketch/desktop/main_window.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"
#include <QTemporaryDir>
#include <filesystem>
#include <stdexcept>

namespace sketch::testing {
// Ordinary Open starts a new UI session but retains the complete saved model
// history. Replay that history independently, without changing the open window.
inline Document fresh_open_history_document(desktop::MainWindow& window) {
    const auto opened = window.document().snapshot();
    const auto digest = document_snapshot_digest(opened);
    if (window.undoCommand() || window.redoCommand() ||
        document_snapshot_digest(window.document().snapshot()) != digest)
        throw std::runtime_error("ordinary Open must refuse inherited Undo/Redo without mutation");
    // Document restoration is intentionally private to storage. Use the public
    // storage path for this independent copy of the exact reopened history.
    QTemporaryDir directory;
    if (!directory.isValid()) throw std::runtime_error("history replay directory unavailable");
    const auto path = std::filesystem::path(directory.filePath("history.bldproj").toStdWString());
    (void)ProjectStore::save(path, opened);
    auto loaded = ProjectStore::load(path);
    if (loaded.document.snapshot().entities() != opened.entities() ||
        loaded.document.snapshot().assets() != opened.assets() ||
        document_authoring_source_digest_v2(loaded.document.snapshot()) !=
            document_authoring_source_digest_v2(opened))
        throw std::runtime_error("independent saved history must retain the exact reopened authoring source");
    return std::move(loaded.document);
}

inline bool saved_undo_redo(desktop::MainWindow& window,
                            const DocumentSnapshot& before,
                            const DocumentSnapshot& after) {
    auto history = fresh_open_history_document(window);
    history.undo(history.revision());
    if (history.snapshot().entities() != before.entities() ||
        history.snapshot().assets() != before.assets()) return false;
    history.redo(history.revision());
    return history.snapshot().entities() == after.entities() &&
        history.snapshot().assets() == after.assets();
}
} // namespace sketch::testing
