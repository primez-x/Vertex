#include "assistance_ocr_recognizer.hpp"
#include "assistance_ocr.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <tesseract/baseapi.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace sketch::desktop {
namespace {
[[noreturn]] void failed() { throw std::runtime_error("Offline assistance OCR recognition failed."); }
bool denyFileRead(const char*, std::vector<char>*) { return false; }
std::string boundedText(const char* text) {
    if (!text) failed();
    std::size_t length = 0;
    while (length <= assistanceOcrTextLimit && text[length]) ++length;
    if (length > assistanceOcrTextLimit) failed();
    return {text, length};
}
} // namespace

std::vector<std::byte> recognizeAssistanceOcrFrame(std::span<const std::byte> frame,
                                                 const std::filesystem::path& root) {
    const auto raster = decodeAssistanceOcrFrame(frame);
    const auto resources = verifiedAssistanceOcrResources(root);
    const auto path = root / assistanceOcrModelPath;
#ifdef _WIN32
    QFile file(QString::fromStdWString(path.native()));
#else
    QFile file(QString::fromStdString(path.native()));
#endif
    if (!file.open(QIODevice::ReadOnly) || file.size() != assistanceOcrModelBytes) failed();
    const auto model = file.read(assistanceOcrModelBytes + 1);
    if (file.error() != QFileDevice::NoError || !file.atEnd() || model.size() != assistanceOcrModelBytes ||
        QCryptographicHash::hash(model, QCryptographicHash::Sha256).toHex() != assistanceOcrModelSha256) failed();

    // Reject a link-time engine substitution before interpreting traineddata.
    if (std::string_view(tesseract::TessBaseAPI::Version()) != "5.5.2") failed();
    tesseract::TessBaseAPI api;
    // The in-memory overload plus an always-refusing FileReader prevents model
    // fallback. No caller/config parameters or external config files are passed.
    if (api.Init(model.constData(), static_cast<int>(model.size()), "eng", tesseract::OEM_LSTM_ONLY,
        nullptr, 0, nullptr, nullptr, false, denyFileRead) != 0) failed();
    std::vector<std::string> languages;
    api.GetLoadedLanguagesAsVector(&languages);
    if (languages != std::vector<std::string>{"eng"}) failed();
    api.SetPageSegMode(tesseract::PSM_AUTO); // Explicitly excludes OSD and osd.traineddata.
    api.SetImage(raster.luminance.data(), static_cast<int>(raster.width), static_cast<int>(raster.height),
                 1, static_cast<int>(raster.width));
    api.SetSourceResolution(300);
    if (api.Recognize(nullptr) != 0) failed();
    const std::unique_ptr<char[]> original(api.GetUTF8Text());
    AssistanceOcrResult result;
    result.text = boundedText(original.get());
    result.resources = resources; result.producer = assistanceOcrProducer;
    const std::unique_ptr<tesseract::ResultIterator> iterator(api.GetIterator());
    std::size_t search = 0;
    if (iterator) do {
        const std::unique_ptr<char[]> raw(iterator->GetUTF8Text(tesseract::RIL_TEXTLINE));
        if (!raw) continue; // An observed empty/nontext block has no text range.
        auto line = boundedText(raw.get());
        // Tesseract supplies line separators in these strings. Keep every byte
        // in original text, while the rectangle selects the visible line text.
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        if (result.runs.size() == assistanceOcrRunLimit) failed();
        const auto offset = result.text.find(line, search);
        if (offset == std::string::npos) failed();
        int left, top, right, bottom;
        if (!iterator->BoundingBox(tesseract::RIL_TEXTLINE, &left, &top, &right, &bottom) ||
            left < 0 || top < 0 || right <= left || bottom <= top ||
            right > raster.width || bottom > raster.height) failed();
        const auto confidence = static_cast<double>(iterator->Confidence(tesseract::RIL_TEXTLINE)) / 100.0;
        if (!std::isfinite(confidence) || confidence < 0 || confidence > 1) failed();
        const auto x = double(left) / raster.width, y = double(top) / raster.height;
        result.runs.push_back({offset, line.size(), x, y,
            double(right) / raster.width - x, double(bottom) / raster.height - y, confidence});
        search = offset + line.size();
    } while (iterator->Next(tesseract::RIL_TEXTLINE));
    return encodeAssistanceOcrReply(result);
}
} // namespace sketch::desktop
