#include "sketch/desktop/symbol_svg_palette.hpp"
#include "sketch/desktop/svg_admission.hpp"

#include <QSet>
#include <QString>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <cmath>
#include <stdexcept>
#include <vector>

namespace sketch::desktop {
namespace {
[[noreturn]] void unsupported(const char* reason) {
    throw std::invalid_argument(reason);
}

QString color(const std::string& value, bool shade = false) {
    if (value.size() != 7 || value.front() != '#')
        unsupported("SVG palette colors must use #RRGGBB");
    QString result = QStringLiteral("#");
    const auto digit = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        unsupported("SVG palette colors must use #RRGGBB");
    };
    for (std::size_t i = 1; i < value.size(); i += 2) {
        const auto component = digit(value[i]) * 16 + digit(value[i + 1]);
        const auto channel = shade ? static_cast<int>(std::lround(component * 242.0 / 255.0)) : component;
        result += QStringLiteral("%1").arg(channel, 2, 16, QLatin1Char('0'));
    }
    return result;
}

bool safe_id(const QString& id) {
    if (id.isEmpty()) return false;
    for (const auto c : id)
        if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
              (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
              (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
              c == QLatin1Char('_') || c == QLatin1Char('-') || c == QLatin1Char('.')))
            return false;
    return true;
}
} // namespace

QByteArray colored_symbol_svg(const QByteArray& document, const SymbolSvgPalette& palette) {
    validate_svg_document(document);
    if (palette.profile != "white-outline-2") unsupported("Unsupported SVG palette profile");
    const auto outline = color(palette.outline_color);
    const auto surface = color(palette.surface_color);
    const auto shaded_surface = color(palette.surface_color, true);
    // This profile deliberately supports the supplied catalog vocabulary only.
    // No entity expansion, CSS, active content, or resource resolution belongs
    // to a paint copy. XML streaming never enables a resolver.
    if (document.isEmpty() || document.size() > 8 * 1024 * 1024 || document.contains('&'))
        unsupported("SVG palette artwork is empty, oversized, or contains entity references");
    static const QSet<QString> elements{
        "svg", "title", "desc", "defs", "g", "rect", "path", "ellipse", "circle",
        "line", "polygon", "linearGradient", "radialGradient", "stop"};
    static const QSet<QString> attributes{
        "id", "width", "height", "viewBox", "role", "aria-labelledby", "data-vertex-style",
        "x", "y", "x1", "y1", "x2", "y2", "cx", "cy", "r", "rx", "ry", "d", "points",
        "fill", "stroke", "stroke-width", "stroke-linejoin", "stroke-linecap", "stroke-dasharray",
        "opacity", "transform", "stop-color", "stop-opacity", "offset",
        "gradientUnits", "gradientTransform", "spreadMethod", "fx", "fy"};
    static const QSet<QString> primary{
        "ceramic", "rim", "steel", "upholstery", "cushion", "linen", "wood",
        "counter", "foliage", "leaf", "tread"};
    static const QSet<QString> protected_roles{"well", "steelwell", "dark", "glass"};
    const auto namespace_uri = QStringLiteral("http://www.w3.org/2000/svg");
    QXmlStreamReader xml(document);
    QByteArray derivative;
    QXmlStreamWriter writer(&derivative);
    std::vector<QString> gradient_stack;
    std::vector<QString> element_stack;
    QSet<QString> ids;
    QSet<QString> gradient_ids;
    QSet<QString> paint_references;
    bool root_seen = false;
    std::size_t element_count = 0;
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference ||
            token == QXmlStreamReader::ProcessingInstruction)
            unsupported("SVG palette artwork cannot contain DTD, entity, or processing instructions");
        if (token == QXmlStreamReader::StartElement) {
            const auto name = xml.name().toString();
            if (!elements.contains(name) || xml.namespaceUri() != namespace_uri || !xml.prefix().isEmpty())
                unsupported("SVG palette artwork contains an unsupported element or namespace");
            if (++element_count > 200000 || element_stack.size() >= 64)
                unsupported("SVG palette artwork exceeds structural limits");
            if (element_stack.empty()) {
                if (root_seen || name != QStringLiteral("svg") ||
                    xml.attributes().value(QStringLiteral("data-vertex-style")) != QStringLiteral("white-outline-2"))
                    unsupported("SVG palette artwork does not declare the white-outline-2 root profile");
                root_seen = true;
            } else if (name == QStringLiteral("svg")) {
                unsupported("SVG palette artwork cannot contain nested SVG roots");
            }
            auto gradient = gradient_stack.empty() ? QString{} : gradient_stack.back();
            const bool is_gradient = name == QStringLiteral("linearGradient") || name == QStringLiteral("radialGradient");
            if (is_gradient) {
                if (!gradient.isEmpty()) unsupported("SVG palette artwork contains nested gradients");
                const auto id = xml.attributes().value(QStringLiteral("id")).toString();
                const auto separator = id.lastIndexOf(QStringLiteral("--"));
                if (separator <= 0 || !safe_id(id)) unsupported("SVG palette gradient has no supported paint role");
                gradient = id.mid(separator + 2);
                if (!primary.contains(gradient) && !protected_roles.contains(gradient))
                    unsupported("SVG palette gradient has an unsupported paint role");
                gradient_ids.insert(id);
            }
            if (name == QStringLiteral("stop") &&
                (element_stack.empty() || !(element_stack.back() == QStringLiteral("linearGradient") ||
                                            element_stack.back() == QStringLiteral("radialGradient"))))
                unsupported("SVG palette stop must belong directly to a named gradient");
            writer.writeStartElement(name);
            for (const auto& declaration : xml.namespaceDeclarations()) {
                if (!declaration.prefix().isEmpty() || declaration.namespaceUri() != namespace_uri)
                    unsupported("SVG palette artwork contains an unsupported namespace declaration");
                writer.writeDefaultNamespace(namespace_uri);
            }
            for (const auto& attribute : xml.attributes()) {
                const auto key = attribute.name().toString();
                auto value = attribute.value().toString();
                if (!attribute.namespaceUri().isEmpty() || !attributes.contains(key))
                    unsupported("SVG palette artwork contains an unsupported paint or resource attribute");
                if (key == QStringLiteral("id")) {
                    if (!safe_id(value) || ids.contains(value)) unsupported("SVG palette artwork has an invalid or duplicate ID");
                    ids.insert(value);
                }
                if ((key == QStringLiteral("fill") || key == QStringLiteral("stroke")) &&
                    value.contains(QStringLiteral("url"), Qt::CaseInsensitive)) {
                    if (!value.startsWith(QStringLiteral("url(#")) || !value.endsWith(QLatin1Char(')')) ||
                        !safe_id(value.mid(5, value.size() - 6)))
                        unsupported("SVG palette artwork contains an external or unsupported paint reference");
                    paint_references.insert(value.mid(5, value.size() - 6));
                }
                if (key == QStringLiteral("stroke") && value == QStringLiteral("#111111")) value = outline;
                if (key == QStringLiteral("fill") ||
                    (key == QStringLiteral("stop-color") && name == QStringLiteral("stop") && primary.contains(gradient))) {
                    if (value == QStringLiteral("#ffffff")) value = surface;
                    else if (value == QStringLiteral("#f2f2f2")) value = shaded_surface;
                }
                writer.writeAttribute(key, value);
            }
            element_stack.push_back(name);
            gradient_stack.push_back(gradient);
        } else if (token == QXmlStreamReader::EndElement) {
            if (element_stack.empty()) unsupported("Malformed SVG palette artwork");
            writer.writeEndElement();
            element_stack.pop_back();
            gradient_stack.pop_back();
        } else if (token == QXmlStreamReader::Characters) {
            writer.writeCharacters(xml.text().toString());
        } else if (token == QXmlStreamReader::Comment) {
            writer.writeComment(xml.text().toString());
        }
    }
    if (xml.hasError() || writer.hasError() || !root_seen || !element_stack.empty())
        unsupported("Malformed SVG palette artwork");
    for (const auto& reference : paint_references)
        if (!gradient_ids.contains(reference)) unsupported("SVG palette paint references an unsupported gradient");
    return derivative;
}
} // namespace sketch::desktop
