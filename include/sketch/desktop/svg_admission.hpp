#pragma once

#include "sketch/svg_admission.hpp"
#include <QBuffer>
#include <QImageReader>
#include <QSvgRenderer>
#include <QXmlStreamReader>

namespace sketch::desktop {
// Full XML admission is separate from the headless structure preflight. It
// never installs an entity resolver, never renders while parsing, and keeps
// ordinary SVG elements (including markers, masks and patterns) intact.
inline void validate_svg_document(const QByteArray& document) {
    validate_svg_structure(std::string_view(document.constData(), static_cast<std::size_t>(document.size())));
    QXmlStreamReader xml(document);
    std::size_t depth = 0;
    bool root_seen = false;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference || token == QXmlStreamReader::ProcessingInstruction)
            throw SvgAdmissionError("SVG cannot contain DTD, entities, or processing instructions");
        if (token == QXmlStreamReader::StartElement) {
            if (++depth > svg_element_depth_limit) throw SvgAdmissionError("SVG exceeds the XML element depth limit");
            if (!root_seen) {
                if (xml.name() != QStringLiteral("svg") || (!xml.namespaceUri().isEmpty() && xml.namespaceUri() != QStringLiteral("http://www.w3.org/2000/svg")))
                    throw SvgAdmissionError("SVG must have an SVG root");
                root_seen = true;
            }
            if (xml.namespaceUri() == QStringLiteral("http://www.w3.org/2001/XInclude"))
                throw SvgAdmissionError("SVG cannot contain external includes");
            const auto element = xml.name().toString().toUtf8();
            svg_admission_detail::resource_text(std::string("<") + element.toStdString() + " ");
            for (const auto& attribute : xml.attributes()) {
                const auto key = attribute.name().toString().toUtf8();
                svg_admission_detail::resource_text(key.toStdString());
                const auto bytes = attribute.value().toString().toUtf8();
                svg_admission_detail::resource_text(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (!depth) throw SvgAdmissionError("Malformed SVG XML");
            --depth;
        }
    }
    if (xml.hasError() || !root_seen || depth) throw SvgAdmissionError("Malformed SVG XML");
}

inline bool load_admitted_svg(QSvgRenderer& renderer, const QByteArray& document) {
    validate_svg_document(document);
    // Constructors that load bytes run before setOptions can override ambient
    // defaults. Always construct empty and make policy explicit before load.
    renderer.setOptions(QtSvg::Option::NoOption);
    return renderer.load(document);
}

// Select only the raster encodings supported by reference import, using bytes
// before constructing a reader. Auto-detection can instantiate QtSvg itself.
inline QByteArray reference_raster_format(const QByteArray& document) {
    if (document.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) return "png";
    if (document.startsWith(QByteArray::fromHex("ffd8ff"))) return "jpeg";
    if (document.startsWith("BM")) return "bmp";
    if (document.startsWith(QByteArray::fromHex("49492a00")) || document.startsWith(QByteArray::fromHex("4d4d002a"))) return "tiff";
    throw std::invalid_argument("Reference render asset must contain supported raster bytes (PNG, JPEG, BMP, or TIFF)");
}

inline QImage decode_reference_raster(const QByteArray& document) {
    const auto format = reference_raster_format(document);
    QBuffer input;
    input.setData(document);
    if (!input.open(QIODevice::ReadOnly)) throw std::invalid_argument("Reference render asset is unavailable");
    QImageReader reader(&input, format);
    reader.setAutoDetectImageFormat(false);
    reader.setDecideFormatFromContent(false);
    reader.setAutoTransform(false);
    return reader.read();
}
} // namespace sketch::desktop
