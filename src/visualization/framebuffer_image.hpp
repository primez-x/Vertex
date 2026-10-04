#pragma once

#include <Image_PixMap.hxx>
#include <QImage>

#include <cstring>
#include <limits>

namespace sketch::visualization::detail {

// Copy logical top-to-bottom rows into Qt-owned storage. The native pixmap may
// be bottom-up or padded; image encoding must not borrow its native storage.
inline QImage framebufferImage(const Image_PixMap& pixels) {
    if (pixels.IsEmpty() || pixels.Depth() != 1 ||
        pixels.Width() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        pixels.Height() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
    int bytes_per_pixel = 0, red = 0, blue = 2;
    switch (pixels.Format()) {
    case Image_Format_RGB: bytes_per_pixel = 3; break;
    case Image_Format_RGB32: case Image_Format_RGBA: bytes_per_pixel = 4; break;
    case Image_Format_BGR: bytes_per_pixel = 3; red = 2; blue = 0; break;
    case Image_Format_BGR32: case Image_Format_BGRA: bytes_per_pixel = 4; red = 2; blue = 0; break;
    default: return {}; // Only the requested eight-bit RGB framebuffer is valid.
    }
    QImage image(static_cast<int>(pixels.Width()), static_cast<int>(pixels.Height()), QImage::Format_RGB888);
    if (image.isNull()) return {};
    for (int y = 0; y < image.height(); ++y) {
        auto* output = image.scanLine(y);
        if (pixels.Format() == Image_Format_RGB) {
            std::memcpy(output, pixels.Row(y), pixels.Width() * 3);
        } else {
            const auto* input = pixels.Row(y);
            for (int x = 0; x < image.width(); ++x) {
                output[0] = input[red]; output[1] = input[1]; output[2] = input[blue];
                input += bytes_per_pixel; output += 3;
            }
        }
    }
    return image;
}

} // namespace sketch::visualization::detail
