#pragma once

#include <QByteArray>
#include <QString>

namespace sketch::desktop {
struct CanvasSketchContentRecording;

// Finalized vector PDF bytes. The recorded picture and PDF use the same DPI;
// navigation zoom and selected sheet sizes cannot change this composition.
[[nodiscard]] QByteArray make_sketch_pdf(CanvasSketchContentRecording recording,
                                        QString* diagnostic = nullptr);
}  // namespace sketch::desktop
