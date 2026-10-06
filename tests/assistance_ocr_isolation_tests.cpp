#include "assistance_ocr.hpp"
#include "support/noninteractive_errors.hpp"

#include <QGuiApplication>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QRawFont>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Sddl.h>
#include <UserEnv.h>
#include <Objbase.h>
#include <winioctl.h>
#endif

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void resourceFailureDiagnostics(const std::filesystem::path& root) {
    using namespace sketch::desktop;
    QTemporaryDir temporary;
    require(temporary.isValid(), "OCR diagnostic fixture requires private storage");
    const auto fixture = std::filesystem::path(temporary.path().toStdWString());
    const auto rejectsAt = [&](AssistanceOcrFailureStage stage, const char* code, bool os_error = false) {
        bool rejected = false;
        try { (void)verifiedAssistanceOcrResources(fixture); }
        catch (const AssistanceOcrFailure& failure) {
            rejected = failure.stage() == stage && std::strcmp(failure.stage_code(), code) == 0 &&
                std::strcmp(failure.what(), "Offline assistance OCR recognition failed.") == 0 &&
                (failure.os_error() != 0) == os_error;
        }
        require(rejected, "OCR resource failures must retain their fixed diagnostic stage");
    };
    rejectsAt(AssistanceOcrFailureStage::resource_metadata, "resource_metadata", true);
    std::filesystem::create_directories(fixture / "assets/assistance/ocr");
    const auto descriptor = fixture / assistanceOcrEnginePath;
    { QFile file(QString::fromStdWString(descriptor.native()));
      require(file.open(QIODevice::WriteOnly), "OCR diagnostic descriptor must open"); }
    rejectsAt(AssistanceOcrFailureStage::resource_read, "resource_read");
    { QFile file(QString::fromStdWString(descriptor.native()));
      require(file.open(QIODevice::WriteOnly) && file.write("{}") == 2,
              "OCR diagnostic descriptor must write"); }
    rejectsAt(AssistanceOcrFailureStage::resource_validation, "resource_validation");
    require(std::filesystem::remove(descriptor), "OCR diagnostic descriptor must replace");
    for (const auto* relative : {assistanceOcrEnginePath, assistanceOcrLicensePath, assistanceOcrModelPath})
        std::filesystem::copy_file(root / relative, fixture / relative);
    const auto verified = loadVerifiedAssistanceOcrBundle(fixture);
    require(verified.model.size() == assistanceOcrModelBytes &&
            verified.resources == verifiedAssistanceOcrResources(root),
            "OCR must return the pinned model snapshot with its resource provenance");
    const auto original_first = verified.model.front();
    { QFile file(QString::fromStdWString((fixture / assistanceOcrModelPath).native()));
      require(file.open(QIODevice::ReadWrite), "OCR diagnostic model must open");
      const auto first = file.read(1);
      require(first.size() == 1 && file.seek(0), "OCR diagnostic model must read");
      const char changed = static_cast<char>(static_cast<unsigned char>(first[0]) ^ 1);
      require(file.write(&changed, 1) == 1, "OCR diagnostic model must change one byte"); }
    rejectsAt(AssistanceOcrFailureStage::resource_hash, "resource_hash");
    require(verified.model.front() == original_first,
            "Loaded OCR model must remain the admitted snapshot after pathname content changes");
}

#ifdef _WIN32
void createJunction(const std::filesystem::path& link, const std::filesystem::path& target) {
    require(std::filesystem::create_directory(link), "OCR junction fixture must be fresh");
    const auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "OCR junction fixture must open");
    struct Buffer {
        DWORD tag; WORD data_length; WORD reserved;
        WORD substitute_offset; WORD substitute_length; WORD print_offset; WORD print_length;
        wchar_t names[4096];
    } buffer{};
    static_assert(offsetof(Buffer, names) == 16);
    auto print = target.native();
    std::replace(print.begin(), print.end(), L'/', L'\\');
    const auto substitute = L"\\??\\" + print;
    require(substitute.size() + print.size() + 2 <= 4096, "OCR junction fixture names must be bounded");
    buffer.tag = IO_REPARSE_TAG_MOUNT_POINT;
    buffer.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    buffer.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
    buffer.print_length = static_cast<WORD>(print.size() * sizeof(wchar_t));
    buffer.data_length = static_cast<WORD>(8 + (substitute.size() + print.size() + 2) * sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), buffer.names);
    std::copy(print.begin(), print.end(), buffer.names + substitute.size() + 1);
    DWORD returned = 0;
    const auto created = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &buffer,
        8 + buffer.data_length, nullptr, 0, &returned, nullptr);
    CloseHandle(handle);
    require(created, "OCR junction fixture must create its actual reparse point");
}
void enableCaseSensitiveDirectory(const std::filesystem::path& path) {
    const auto handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
        FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "OCR case-sensitive fixture directory must open");
    // Windows 11 FileCaseSensitiveInfo is class 23. The SDK enum spelling is
    // gated by NTDDI_VERSION; this fixture does not alter the target's SDK floor.
    constexpr auto information_class = static_cast<FILE_INFO_BY_HANDLE_CLASS>(23);
    FILE_CASE_SENSITIVE_INFO requested{FILE_CS_FLAG_CASE_SENSITIVE_DIR};
    const auto changed = SetFileInformationByHandle(handle, information_class, &requested, sizeof(requested));
    FILE_CASE_SENSITIVE_INFO observed{};
    const auto queried = GetFileInformationByHandleEx(handle, information_class, &observed, sizeof(observed));
    CloseHandle(handle);
    require(changed && queried && (observed.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR),
            "OCR case-only redirect fixture requires an observed case-sensitive NTFS directory");
}
void resourcePathGuards(const std::filesystem::path& root) {
    using namespace sketch::desktop;
    QTemporaryDir temporary;
    require(temporary.isValid(), "OCR path guard fixture requires private storage");
    const auto holder = std::filesystem::path(temporary.path().toStdWString());
    const auto target = holder / "actual";
    const auto runtime = target / "runtime";
    std::filesystem::create_directories(runtime / "assets/assistance/ocr");
    for (const auto* relative : {assistanceOcrEnginePath, assistanceOcrLicensePath, assistanceOcrModelPath})
        std::filesystem::copy_file(root / relative, runtime / relative);
    const auto pinned = verifiedAssistanceOcrResources(root);
    require(verifiedAssistanceOcrResources(runtime) == pinned &&
        verifiedAssistanceOcrResources(std::filesystem::path(L"\\\\?\\" + runtime.native())) == pinned,
        "OCR must admit ordinary local DOS endpoints with consistent extended prefixes");
    auto lower_drive = runtime.native();
    if (lower_drive.front() >= L'A' && lower_drive.front() <= L'Z')
        lower_drive.front() = static_cast<wchar_t>(lower_drive.front() + (L'a' - L'A'));
    require(verifiedAssistanceOcrResources(std::filesystem::path(lower_drive)) == pinned,
            "OCR endpoint comparison must normalize the inherently case-insensitive drive letter");
    const auto rejectsMetadata = [&](const std::filesystem::path& path) {
        bool rejected = false;
        try { (void)verifiedAssistanceOcrResources(path); }
        catch (const AssistanceOcrFailure& failure) {
            rejected = failure.stage() == AssistanceOcrFailureStage::resource_metadata && failure.os_error() == 0;
        }
        require(rejected, "OCR must refuse ambiguous paths and redirected endpoints before resource admission");
    };
    for (const auto& path : {runtime / ".", runtime / ".." / "runtime",
        std::filesystem::path(runtime.native() + L"."), std::filesystem::path(runtime.native() + L" "),
        std::filesystem::path(runtime.native() + L":stream"), std::filesystem::path(L"C:relative"),
        std::filesystem::path(L"\\\\server\\share"), std::filesystem::path(L"\\\\?\\UNC\\server\\share")})
        rejectsMetadata(path);
    const auto alias = holder / "redirected";
    struct RemoveJunction { std::filesystem::path path; ~RemoveJunction() { RemoveDirectoryW(path.c_str()); } } cleanup{alias};
    createJunction(alias, target);
    rejectsMetadata(alias); // Terminal root is a real reparse point.
    rejectsMetadata(alias / "runtime"); // Ordinary endpoint reached through an ancestor junction.
    const auto redirected_resources = holder / "resource-runtime";
    require(std::filesystem::create_directory(redirected_resources), "OCR resource redirect runtime must be fresh");
    const auto assets_alias = redirected_resources / "assets";
    RemoveJunction resource_cleanup{assets_alias};
    createJunction(assets_alias, runtime / "assets");
    rejectsMetadata(redirected_resources); // Trusted root, but fixed resource traverses a junction.
    const auto sensitive = holder / "case-sensitive";
    require(std::filesystem::create_directory(sensitive), "OCR case-sensitive holder must be fresh");
    enableCaseSensitiveDirectory(sensitive);
    const auto case_target = sensitive / "actual";
    const auto case_runtime = case_target / "runtime";
    std::filesystem::create_directories(case_runtime / "assets/assistance/ocr");
    for (const auto* relative : {assistanceOcrEnginePath, assistanceOcrLicensePath, assistanceOcrModelPath})
        std::filesystem::copy_file(root / relative, case_runtime / relative);
    require(verifiedAssistanceOcrResources(case_runtime) == pinned,
            "OCR must admit the actual stored spelling in a case-sensitive parent");
    const auto case_alias = sensitive / "Actual";
    RemoveJunction case_cleanup{case_alias};
    createJunction(case_alias, case_target);
    rejectsMetadata(case_alias / "runtime"); // Final path differs only in ancestor case.
    require(verifiedAssistanceOcrResources(runtime) == pinned,
            "Rejected junction paths must preserve the actual pinned resource tree");
}

std::filesystem::path profileTempRoot() {
    PSID sid = nullptr;
    auto result = CreateAppContainerProfile(L"Vertex.ImportWorker", L"Vertex import worker",
        L"Local offline import isolation", nullptr, 0, &sid);
    if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        result = DeriveAppContainerSidFromAppContainerName(L"Vertex.ImportWorker", &sid);
    require(SUCCEEDED(result) && sid, "OCR fixture must resolve the real worker profile");
    LPWSTR sid_text = nullptr;
    const auto converted = ConvertSidToStringSidW(sid, &sid_text);
    FreeSid(sid);
    require(converted, "OCR fixture profile SID must stringify");
    PWSTR folder = nullptr;
    result = GetAppContainerFolderPath(sid_text, &folder);
    LocalFree(sid_text);
    require(SUCCEEDED(result) && folder, "OCR fixture must resolve profile storage");
    auto path = std::filesystem::path(folder) / "Temp";
    CoTaskMemFree(folder);
    std::filesystem::create_directories(path);
    path /= "ocr-isolation-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64());
    require(std::filesystem::create_directory(path), "OCR fixture private holder must be fresh");
    return path;
}

// Only this fixture's import slot changes. The actual broker and worker run;
// cancellation is asserted immediately after the real suspended child resumes.
class CancelAtResume final {
public:
    explicit CancelAtResume(std::shared_ptr<std::atomic_bool> flag) {
        flag_ = std::move(flag);
        const auto image = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
        const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
        const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
        const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        auto descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image + directory.VirtualAddress);
        for (; descriptor->Name && !slot_; ++descriptor) {
            if (!descriptor->OriginalFirstThunk) continue;
            auto name = reinterpret_cast<IMAGE_THUNK_DATA*>(image + descriptor->OriginalFirstThunk);
            auto address = reinterpret_cast<IMAGE_THUNK_DATA*>(image + descriptor->FirstThunk);
            for (; name->u1.AddressOfData; ++name, ++address) {
                if (IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal)) continue;
                const auto imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(image + name->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(imported->Name), "ResumeThread") == 0) {
                    slot_ = reinterpret_cast<void**>(&address->u1.Function);
                    break;
                }
            }
        }
        require(slot_, "OCR cancellation fixture must observe native resume");
        DWORD protection = 0;
        require(VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &protection), "OCR fixture import slot must be writable");
        original_ = InterlockedExchangePointer(slot_, reinterpret_cast<void*>(&resume));
        DWORD ignored = 0;
        VirtualProtect(slot_, sizeof(*slot_), protection, &ignored);
    }
    ~CancelAtResume() {
        DWORD protection = 0;
        if (VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &protection)) {
            InterlockedExchangePointer(slot_, original_);
            DWORD ignored = 0;
            VirtualProtect(slot_, sizeof(*slot_), protection, &ignored);
        }
        flag_.reset();
    }
private:
    static DWORD WINAPI resume(HANDLE thread) {
        const auto result = reinterpret_cast<DWORD (WINAPI*)(HANDLE)>(original_)(thread);
        if (result != static_cast<DWORD>(-1)) flag_->store(true, std::memory_order_release);
        return result;
    }
    inline static std::shared_ptr<std::atomic_bool> flag_;
    inline static void* original_{};
    void** slot_{};
};
#endif
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QGuiApplication application(argc, argv);
    try {
#ifndef _WIN32
        throw std::runtime_error("OCR isolation qualification requires Windows 11.");
#else
        using namespace sketch;
        using namespace sketch::desktop;
        require(argc == 1, "OCR isolation fixture must run beside its frozen worker and resources");
        const auto root = assistanceOcrApplicationRoot();
        const auto pinned_resources = verifiedAssistanceOcrResources(root);
        resourceFailureDiagnostics(root);
        resourcePathGuards(root);
        const auto font_id = QFontDatabase::addApplicationFont(QString::fromStdWString((root / "assets/fonts/Inter.ttf").wstring()));
        require(font_id >= 0, "OCR fixture requires its frozen bundled font");
        const auto families = QFontDatabase::applicationFontFamilies(font_id);
        require(!families.isEmpty(), "OCR fixture font must resolve");
        QFont font(families.front()); font.setPixelSize(72);
        const auto raw = QRawFont::fromFont(font);
        for (const auto character : QStringLiteral("Wall: 31 ft 6 in Width: 4.25 m Room: Kitchen"))
            require(raw.isValid() && raw.supportsCharacter(character), "OCR fixture requires real rendered glyphs");
        QImage image(1600, 650, QImage::Format_Grayscale8); image.fill(Qt::white);
        {
            QPainter painter(&image); painter.setFont(font); painter.setPen(Qt::black);
            painter.drawText(80, 150, "Wall: 31 ft 6 in");
            painter.drawText(80, 330, "Width: 4.25 m");
            painter.drawText(80, 510, "Room: Kitchen");
        }
        AssistanceRaster raster; raster.reference_id = "isolated-real-ocr-fixture";
        raster.width = image.width(); raster.height = image.height();
        for (int y = 0; y < image.height(); ++y)
            raster.luminance.insert(raster.luminance.end(), image.constScanLine(y), image.constScanLine(y) + image.width());
        WindowsImportWorkerOptions options;
        options.executable = std::filesystem::path(QCoreApplication::applicationDirPath().toStdWString()) / "vertex-import-worker.exe";
        options.temporary_root = profileTempRoot(); options.immutable_module_roots = {root};
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{options.temporary_root};
        WindowsImportWorkerReport report;
        const AssistanceOcrBroker observed = [&](const WindowsImportWorkerOptions& request) {
            report = run_windows_import_worker(request);
            std::cout << "actual_ocr_broker: " << report.to_json().dump() << '\n';
            return report;
        };
        const auto result = recognizeAssistanceRaster(raster, options, observed);
        require(report.controls_attested() && report.job_membership_verified && report.launched && report.exit_code == 0 &&
                result.isolation_controls_attested && result.producer == assistanceOcrProducer && result.resources == pinned_resources,
                "actual OCR must preserve pinned provenance and every observed broker control");
        require(result.text.find("31 ft 6 in") != std::string::npos && result.text.find("4.25 m") != std::string::npos &&
                result.text.find("Kitchen") != std::string::npos && result.runs.size() == 3,
                "isolated OCR worker must recognize actual imperial, metric and room pixels");
        for (const auto& run : result.runs)
            require(run.confidence && *run.confidence > 0 && *run.confidence <= 1 && run.width > 0 && run.height > 0,
                    "isolated OCR must return actual confidence and text regions");
        require(std::filesystem::is_empty(options.temporary_root), "completed OCR must clean private invocation storage");
        const auto capture_path = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!capture_path.isEmpty()) {
            QDir directory(capture_path); require(directory.mkpath("."), "OCR evidence directory must exist");
            require(image.save(directory.filePath("assistance-isolated-ocr.png"), "PNG"), "OCR raster evidence must save");
            const auto save = [&](const QString& name, const QByteArray& data) {
                QFile file(directory.filePath(name));
                require(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(), "OCR evidence must save completely");
            };
            save("assistance-isolated-ocr-report.json", QByteArray::fromStdString(report.to_json().dump(2)));
            save("assistance-isolated-ocr-reply.json", QByteArray(reinterpret_cast<const char*>(report.output.data()), static_cast<qsizetype>(report.output.size())));
        }
        auto flag = std::make_shared<std::atomic_bool>(false); options.cancellation_requested = flag;
        bool cancelled = false;
        {
            CancelAtResume cancel(flag);
            try { (void)recognizeAssistanceRaster(raster, options, observed); }
            catch (const std::runtime_error& error) { cancelled = std::string(error.what()) == "Assistance OCR cancelled."; }
        }
        require(cancelled && report.launched && report.status == WindowsImportWorkerStatus::cancelled &&
                !report.completed && report.output.empty() && !report.controls_attested() &&
                std::filesystem::is_empty(options.temporary_root), "live broker cancellation must refuse OCR and clean confirmed worker storage");
        flag->store(false); options.executable = root / "missing-worker.exe";
        bool refused = false;
        try { (void)recognizeAssistanceRaster(raster, options, observed); }
        catch (const std::runtime_error&) { refused = true; }
        require(refused && !report.launched && !report.controls_attested() && report.output.empty(),
                "actual broker launch refusal must never become a successful OCR result");
        std::cout << "actual OCR broker isolation tests passed\n";
        return 0;
#endif
    } catch (const std::exception& error) {
        std::cerr << "assistance_ocr_isolation_tests: " << error.what() << '\n';
        return 1;
    }
}
