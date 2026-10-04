#include "sketch/desktop/area_class_palette.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>

namespace sketch::desktop {
QByteArray encode_area_class_drag(const QString& classification) {
    return QJsonDocument(QJsonObject{{"version",1},{"class",classification}}).toJson(QJsonDocument::Compact);
}
std::optional<QString> decode_area_class_drag(const QMimeData* mime) {
    if(!mime || !mime->hasFormat(area_class_mime_type))return {};
    const auto bytes=mime->data(area_class_mime_type);if(bytes.size()>4096)return {};
    const auto document=QJsonDocument::fromJson(bytes);if(!document.isObject())return {};
    const auto object=document.object();
    if(object.size()!=2 || object.value("version").toDouble()!=1.0 || !object.value("class").isString())return {};
    const auto value=object.value("class").toString();if(value.size()>256 || value!=value.trimmed() || value.contains(QChar::Null))return {};
    return value;
}
} // namespace sketch::desktop
