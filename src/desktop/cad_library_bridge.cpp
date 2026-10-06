#include "cad_library_bridge.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#if defined(VERTEX_CAD_CONTROLLED_MANIFEST_SHA256) || \
    defined(VERTEX_CAD_CONTROLLED_IFC_EXTENSION_SHA256) || \
    defined(VERTEX_CAD_CONTROLLED_IFC_WRAPPER_SHA256)
#if !defined(VERTEX_CAD_CONTROLLED_MANIFEST_SHA256) || \
    !defined(VERTEX_CAD_CONTROLLED_IFC_EXTENSION_SHA256) || \
    !defined(VERTEX_CAD_CONTROLLED_IFC_WRAPPER_SHA256)
#error Controlled CAD runtime selection requires all three compiled SHA-256 identities.
#endif
#define VERTEX_CAD_CONTROLLED_RUNTIME 1
#endif

#ifdef _WIN32
#include <QByteArrayView>
#include <QCryptographicHash>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>

// The bundled interpreter is a release build, including in a Debug worker.
// Python's Windows SDK otherwise auto-links python313_d.lib and changes its ABI.
#pragma push_macro("_DEBUG")
#pragma push_macro("slots")
#undef _DEBUG
#undef slots
#include <Python.h>
#pragma pop_macro("slots")
#pragma pop_macro("_DEBUG")

#if PY_MAJOR_VERSION != 3 || PY_MINOR_VERSION != 13 || PY_MICRO_VERSION != 15
#error The CAD worker requires the pinned CPython 3.13.15 SDK.
#endif
#endif

namespace sketch::desktop {
namespace {

#ifdef VERTEX_CAD_CONTROLLED_RUNTIME
template<std::size_t Size>
constexpr bool valid_compiled_sha256(const char (&value)[Size]) {
    if constexpr (Size != 65) return false;
    else {
        for (std::size_t i = 0; i < 64; ++i)
            if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
                return false;
        return value[64] == '\0';
    }
}
static_assert(valid_compiled_sha256(VERTEX_CAD_CONTROLLED_MANIFEST_SHA256),
              "Controlled CAD manifest SHA-256 must be 64 lowercase hexadecimal characters");
static_assert(valid_compiled_sha256(VERTEX_CAD_CONTROLLED_IFC_EXTENSION_SHA256),
              "Controlled IFC extension SHA-256 must be 64 lowercase hexadecimal characters");
static_assert(valid_compiled_sha256(VERTEX_CAD_CONTROLLED_IFC_WRAPPER_SHA256),
              "Controlled IFC wrapper SHA-256 must be 64 lowercase hexadecimal characters");
#endif

#ifdef _WIN32
constexpr std::size_t input_limit = 64U * 1024U * 1024U;
constexpr std::size_t json_limit = 32U * 1024U * 1024U;

std::wstring windows_path_text(const std::filesystem::path& path) {
    auto value = path.lexically_normal().wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.starts_with(L"\\\\?\\UNC\\")) value = L"\\\\" + value.substr(8);
    else if (value.starts_with(L"\\\\?\\")) value.erase(0, 4);
    return value;
}

bool same_windows_path(const std::filesystem::path& left, const std::filesystem::path& right) {
    const auto a = windows_path_text(left), b = windows_path_text(right);
    return !a.empty() && !b.empty() && a.size() < 32768 && b.size() < 32768 &&
        CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                             b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

#ifdef VERTEX_CAD_CONTROLLED_RUNTIME
[[noreturn]] void controlled_identity_error() {
    throw std::runtime_error("Bundled controlled CAD runtime identity mismatch");
}

bool reserved_windows_component(std::wstring_view component) {
    const auto stem = component.substr(0, component.find(L'.'));
    const auto equal = [stem](std::wstring_view name) {
        return CompareStringOrdinal(stem.data(), static_cast<int>(stem.size()),
            name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
    };
    for (const auto name : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"})
        if (equal(name)) return true;
    if (stem.size() != 4) return false;
    const auto last = stem.back();
    return ((last >= L'1' && last <= L'9') || last == L'\u00b9' || last == L'\u00b2' || last == L'\u00b3') &&
        (equal(std::wstring(L"COM") + last) || equal(std::wstring(L"LPT") + last));
}

// The broker already holds the immutable module-root lease. Query only the
// ordinary granted endpoints, so AppContainer never needs ancestor enumeration.
std::wstring controlled_local_path(const std::filesystem::path& path) {
    auto value = path.wstring();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    if (value.starts_with(L"\\\\?\\")) value.erase(0, 4);
    if (value.size() < 3 || value.size() >= 32764 || value.find(L'\0') != std::wstring::npos ||
        !((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z')) ||
        value[1] != L':' || value[2] != L'\\') controlled_identity_error();
    if (value[0] >= L'a' && value[0] <= L'z') value[0] -= L'a' - L'A';
    for (std::size_t start = 3; start < value.size();) {
        const auto separator = value.find(L'\\', start);
        const auto end = separator == std::wstring::npos ? value.size() : separator;
        const auto component = std::wstring_view(value).substr(start, end - start);
        if (component.empty() || component == L"." || component == L".." ||
            component.back() == L'.' || component.back() == L' ' ||
            reserved_windows_component(component) ||
            component.find_first_of(L":<>\"|?*") != std::wstring_view::npos ||
            std::any_of(component.begin(), component.end(), [](wchar_t c) { return c < 32 || (c >= 127 && c <= 159); }) ||
            (separator != std::wstring::npos && separator + 1 == value.size()))
            controlled_identity_error();
        start = end + 1;
    }
    return value;
}

bool same_controlled_path(const std::filesystem::path& left, const std::filesystem::path& right) {
    const auto a = controlled_local_path(left), b = controlled_local_path(right);
    // Case-sensitive Windows directories can contain case-only junctions.
    // Only the drive letter has an inherently case-insensitive identity.
    return a == b;
}

class ControlledFile final {
public:
    ControlledFile(const std::filesystem::path& path, std::uint64_t limit, const char* expected)
        : path_(controlled_local_path(path)) {
        const auto extended = L"\\\\?\\" + path_.wstring();
        // Deny write and delete sharing through import and interpreter teardown.
        handle_ = CreateFileW(extended.c_str(), FILE_READ_DATA | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) controlled_identity_error();
        try {
            BY_HANDLE_FILE_INFORMATION information{};
            if (!GetFileInformationByHandle(handle_, &information) ||
                GetFileType(handle_) != FILE_TYPE_DISK || information.nNumberOfLinks != 1 ||
                (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
                controlled_identity_error();
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(handle_, &size) || size.QuadPart <= 0 ||
                static_cast<std::uint64_t>(size.QuadPart) > limit) controlled_identity_error();
            verify_path();
            QCryptographicHash hash(QCryptographicHash::Sha256);
            std::array<char, 64U * 1024U> buffer{};
            std::uint64_t total = 0;
            for (;;) {
                DWORD received = 0;
                if (!ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &received, nullptr))
                    controlled_identity_error();
                if (received == 0) break;
                total += received;
                if (total > limit || total > static_cast<std::uint64_t>(size.QuadPart))
                    controlled_identity_error();
                hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(received)));
            }
            if (total != static_cast<std::uint64_t>(size.QuadPart) || hash.result().toHex() != expected)
                controlled_identity_error();
            verify_path();
        } catch (...) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw;
        }
    }
    ~ControlledFile() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    ControlledFile(const ControlledFile&) = delete;
    ControlledFile& operator=(const ControlledFile&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    void verify_path() const {
        std::wstring final(32768, L'\0');
        const auto length = GetFinalPathNameByHandleW(handle_, final.data(), static_cast<DWORD>(final.size()),
            FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!length || length >= final.size()) controlled_identity_error();
        final.resize(length);
        if (!same_controlled_path(path_, std::filesystem::path(final)))
            controlled_identity_error();
    }
private:
    std::filesystem::path path_;
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

class ControlledRuntimeIdentity final {
public:
    explicit ControlledRuntimeIdentity(const std::filesystem::path& root)
        : manifest_(root / L"controlled-runtime-manifest.json", 16ULL * 1024 * 1024,
                    VERTEX_CAD_CONTROLLED_MANIFEST_SHA256),
          extension_(root / L"Lib" / L"site-packages" / L"ifcopenshell" /
                     L"_ifcopenshell_wrapper.cp313-win_amd64.pyd", 512ULL * 1024 * 1024,
                     VERTEX_CAD_CONTROLLED_IFC_EXTENSION_SHA256),
          wrapper_(root / L"Lib" / L"site-packages" / L"ifcopenshell" / L"ifcopenshell_wrapper.py",
                   512ULL * 1024 * 1024, VERTEX_CAD_CONTROLLED_IFC_WRAPPER_SHA256) {}
    void verify_paths() const {
        manifest_.verify_path(); extension_.verify_path(); wrapper_.verify_path();
    }
    [[nodiscard]] const std::filesystem::path& extension_path() const noexcept { return extension_.path(); }
    [[nodiscard]] const std::filesystem::path& wrapper_path() const noexcept { return wrapper_.path(); }
private:
    ControlledFile manifest_, extension_, wrapper_;
};
#endif

// CMake delay-loads python313.dll. Load its verified absolute location before
// any C API reference can invoke the delay loader's basename-based lookup.
class PythonRuntimeDll final {
public:
    explicit PythonRuntimeDll(const std::filesystem::path& root) {
        auto expected_path = (root / L"python313.dll").lexically_normal();
        expected_path.make_preferred();
        const auto expected = expected_path.wstring();
        if (expected.size() >= 32768)
            throw std::runtime_error("Bundled CAD interpreter path exceeds Windows limit");
        module_ = LoadLibraryExW(expected.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) throw std::runtime_error("Cannot load bundled CAD interpreter");
        try {
            std::wstring loaded(32768, L'\0');
            const auto length = GetModuleFileNameW(module_, loaded.data(),
                                                   static_cast<DWORD>(loaded.size()));
            if (length == 0 || length >= loaded.size())
                throw std::runtime_error("Cannot identify bundled CAD interpreter");
            loaded.resize(length);
            auto loaded_path = std::filesystem::path(loaded).lexically_normal();
            loaded_path.make_preferred();
            loaded = loaded_path.wstring();
            if (!same_windows_path(loaded_path, expected_path) ||
                GetModuleHandleW(L"python313.dll") != module_)
                throw std::runtime_error("Bundled CAD interpreter location mismatch");
        } catch (...) {
            FreeLibrary(module_);
            module_ = nullptr;
            throw;
        }
    }
    ~PythonRuntimeDll() { if (module_) FreeLibrary(module_); }
    PythonRuntimeDll(const PythonRuntimeDll&) = delete;
    PythonRuntimeDll& operator=(const PythonRuntimeDll&) = delete;
private:
    HMODULE module_{};
};

// SetStdHandle alone does not redirect printf/write, and _dup2 alone does not
// redirect native libraries that obtain their output handles from Windows.
class SilencedOutput final {
public:
    SilencedOutput() {
        if (std::fflush(stdout) != 0 || std::fflush(stderr) != 0)
            throw std::runtime_error("Cannot flush CAD worker output");
        original_handles_[0] = GetStdHandle(STD_OUTPUT_HANDLE);
        original_handles_[1] = GetStdHandle(STD_ERROR_HANDLE);
        original_crt_handles_[0] = reinterpret_cast<HANDLE>(_get_osfhandle(1));
        original_crt_handles_[1] = reinterpret_cast<HANDLE>(_get_osfhandle(2));
        try {
            saved_fds_[0] = _dup(1);
            saved_fds_[1] = _dup(2);
            if (saved_fds_[0] < 0 || saved_fds_[1] < 0)
                throw std::runtime_error("Cannot preserve CAD worker output");
            null_fd_ = _wopen(L"NUL", _O_WRONLY | _O_BINARY | _O_NOINHERIT);
            if (null_fd_ < 0)
                throw std::runtime_error("Cannot suppress CAD worker output");
            for (int index = 0; index < 2; ++index) {
                if (_dup2(null_fd_, index + 1) != 0)
                    throw std::runtime_error("Cannot suppress CAD worker output");
                redirected_[index] = true;
            }
            if (!SetStdHandle(STD_OUTPUT_HANDLE,
                              reinterpret_cast<HANDLE>(_get_osfhandle(1))) ||
                !SetStdHandle(STD_ERROR_HANDLE,
                              reinterpret_cast<HANDLE>(_get_osfhandle(2))))
                throw std::runtime_error("Cannot suppress CAD worker native output");
        } catch (...) {
            restore_noexcept();
            throw;
        }
    }

    SilencedOutput(const SilencedOutput&) = delete;
    SilencedOutput& operator=(const SilencedOutput&) = delete;
    ~SilencedOutput() { restore_noexcept(); }

    void restore() {
        if (!restore_noexcept())
            throw std::runtime_error("Cannot restore CAD worker output");
    }

private:
    bool restore_noexcept() noexcept {
        bool restored = true;
        // Discard pending library output while both destinations still are NUL.
        if (redirected_[0]) std::fflush(stdout);
        if (redirected_[1]) std::fflush(stderr);
        for (int index = 0; index < 2; ++index) {
            if (redirected_[index]) {
                if (_dup2(saved_fds_[index], index + 1) != 0) restored = false;
                redirected_[index] = false;
            }
        }
        // _dup2 closes the old CRT OS handle. If that was also a Windows
        // standard handle, restore Windows to the new CRT handle, not the
        // closed numeric handle. Distinct inherited Windows handles stay intact.
        for (int index = 0; index < 2; ++index) {
            auto handle = original_handles_[index];
            for (int crt = 0; crt < 2; ++crt) {
                if (handle == original_crt_handles_[crt]) {
                    handle = reinterpret_cast<HANDLE>(_get_osfhandle(crt + 1));
                    break;
                }
            }
            if (!SetStdHandle(index == 0 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE,
                              handle)) restored = false;
        }
        for (auto& fd : saved_fds_) {
            if (fd >= 0) _close(fd);
            fd = -1;
        }
        if (null_fd_ >= 0) _close(null_fd_);
        null_fd_ = -1;
        return restored;
    }

    HANDLE original_handles_[2]{};
    HANDLE original_crt_handles_[2]{};
    int saved_fds_[2]{-1, -1};
    int null_fd_{-1};
    bool redirected_[2]{};
};

class PythonObject final {
public:
    explicit PythonObject(PyObject* value = nullptr) noexcept : value_(value) {}
    ~PythonObject() { Py_XDECREF(value_); }
    PythonObject(const PythonObject&) = delete;
    PythonObject& operator=(const PythonObject&) = delete;
    [[nodiscard]] PyObject* get() const noexcept { return value_; }
private:
    PyObject* value_;
};

[[noreturn]] void python_error(const char* message) {
    // Never print a traceback: stdout and stderr are the candidate IPC pipe.
    // The fixed error text also avoids exposing document or filesystem content.
    PyErr_Clear();
    throw std::runtime_error(message);
}

void require_status(PyStatus status) {
    if (PyStatus_Exception(status))
        throw std::runtime_error("Cannot configure isolated CAD interpreter");
}

class PythonConfig final {
public:
    PythonConfig() { PyConfig_InitIsolatedConfig(&value); }
    ~PythonConfig() { PyConfig_Clear(&value); }
    PythonConfig(const PythonConfig&) = delete;
    PythonConfig& operator=(const PythonConfig&) = delete;
    PyConfig value{};
};

void check_library_versions() {
    PythonObject metadata(PyImport_ImportModule("importlib.metadata"));
    if (!metadata.get()) python_error("Cannot inspect bundled CAD library versions");
    PythonObject version(PyObject_GetAttrString(metadata.get(), "version"));
    if (!version.get() || !PyCallable_Check(version.get()))
        python_error("Cannot inspect bundled CAD library versions");
    const struct { const char* package; const char* version; } libraries[]{
        {"ezdxf", "1.4.3"}
#ifndef VERTEX_CAD_CONTROLLED_RUNTIME
        , {"ifcopenshell", "0.8.3.post2"}
#endif
    };
    for (const auto& library : libraries) {
        PythonObject name(PyUnicode_FromString(library.package));
        if (!name.get()) python_error("Cannot inspect bundled CAD library versions");
        PythonObject actual(PyObject_CallOneArg(version.get(), name.get()));
        if (!actual.get() || !PyUnicode_Check(actual.get()))
            python_error("Cannot inspect bundled CAD library versions");
        const auto equal = PyUnicode_CompareWithASCIIString(actual.get(), library.version);
        if (equal != 0) python_error("Bundled CAD library version mismatch");
    }
#ifdef VERTEX_CAD_CONTROLLED_RUNTIME
    // The controlled composer replaces the wheel package and its dist-info.
    // A surviving wheel identity is a mixed runtime, not source admission.
    PythonObject missing(PyObject_GetAttrString(metadata.get(), "PackageNotFoundError"));
    PythonObject ifc_name(PyUnicode_FromString("ifcopenshell"));
    if (!missing.get() || !PyExceptionClass_Check(missing.get()) || !ifc_name.get())
        python_error("Cannot inspect controlled IFC distribution identity");
    PythonObject wheel_version(PyObject_CallOneArg(version.get(), ifc_name.get()));
    if (wheel_version.get() || !PyErr_ExceptionMatches(missing.get()))
        python_error("Controlled IFC runtime contains a distribution identity");
    PyErr_Clear();
#endif
}

#ifdef VERTEX_CAD_CONTROLLED_RUNTIME
void check_python_module_path(PyObject* module, const std::filesystem::path& expected) {
    // PyModule_Check imports the PyModule_Type DLL data symbol, which cannot be
    // delay-loaded by MSVC. The C API validates the module without data imports.
    if (!module || !PyModule_GetDict(module)) python_error("Cannot inspect controlled IFC module identity");
    PythonObject file(PyObject_GetAttrString(module, "__file__"));
    if (!file.get() || !PyUnicode_Check(file.get()))
        python_error("Cannot inspect controlled IFC module identity");
    const auto length = PyUnicode_GetLength(file.get());
    if (length <= 0 || length >= 32768)
        python_error("Controlled IFC module location mismatch");
    Py_ssize_t written = 0;
    const std::unique_ptr<wchar_t, decltype(&PyMem_Free)> text(
        PyUnicode_AsWideCharString(file.get(), &written), &PyMem_Free);
    if (!text) python_error("Cannot inspect controlled IFC module identity");
    const std::wstring value(text.get(), static_cast<std::size_t>(written));
    // Do not normalize an unsafe origin into an accepted path.
    if (!same_controlled_path(std::filesystem::path(value), expected))
        python_error("Controlled IFC module location mismatch");
}

void check_controlled_ifc_modules(const ControlledRuntimeIdentity& identity) {
    // The exact compiled manifest is checked before Python loads. Verified
    // manifest-bound staging supplies package source, and the broker retains
    // the immutable module root. Informative versions confer no admission.
    PythonObject package(PyImport_ImportModule("ifcopenshell"));
    check_python_module_path(package.get(), identity.wrapper_path().parent_path() / L"__init__.py");
    PythonObject wrapper(PyImport_ImportModule("ifcopenshell.ifcopenshell_wrapper"));
    check_python_module_path(wrapper.get(), identity.wrapper_path());
    PythonObject native(PyImport_ImportModule("ifcopenshell._ifcopenshell_wrapper"));
    check_python_module_path(native.get(), identity.extension_path());

    // Identify the native module by the address of its module definition,
    // rather than a basename that could select a different loaded DLL.
    auto* definition = PyModule_GetDef(native.get());
    HMODULE module = nullptr;
    if (!definition || !GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(definition), &module))
        python_error("Cannot identify controlled IFC native module");
    std::wstring loaded(32768, L'\0');
    const auto length = GetModuleFileNameW(module, loaded.data(), static_cast<DWORD>(loaded.size()));
    if (!length || length >= loaded.size())
        python_error("Cannot identify controlled IFC native module");
    loaded.resize(length);
    if (!same_controlled_path(std::filesystem::path(loaded), identity.extension_path()))
        python_error("Controlled IFC native module location mismatch");
    identity.verify_paths();
}
#endif

class PythonInterpreter final {
public:
    explicit PythonInterpreter(const std::filesystem::path& root) {
        if (Py_IsInitialized())
            throw std::runtime_error("CAD interpreter already initialized");
        // The worker is single-use. Keep initialization, references and shutdown
        // on this thread with the GIL held throughout the synchronous call.
        try {
            PythonConfig config;
            auto& value = config.value;
            value.isolated = 1;
            value.use_environment = 0;
            value.site_import = 0;
            value.user_site_directory = 0;
            value.write_bytecode = 0;
            value.safe_path = 1;
            value.parse_argv = 0;
            value.install_signal_handlers = 0;
            value.configure_c_stdio = 0;
            value.buffered_stdio = 0;
            value.pathconfig_warnings = 0;
            value.module_search_paths_set = 1;
            // Explicit extended paths also work when a restricted token cannot
            // query the machine's long-path policy. Extension modules otherwise
            // silently disappear from Python's finder beyond MAX_PATH.
            auto extended_text = root.wstring();
            if (!extended_text.starts_with(L"\\\\?\\")) {
                extended_text = extended_text.starts_with(L"\\\\")
                    ? L"\\\\?\\UNC\\" + extended_text.substr(2)
                    : L"\\\\?\\" + extended_text;
            }
            const std::filesystem::path extended_root(extended_text);
            const auto root_text = extended_root.wstring();
            const auto executable = (extended_root / L"python.exe").wstring();
            const auto zip = (extended_root / L"python313.zip").wstring();
            const auto packages = (extended_root / L"Lib" / L"site-packages").wstring();
            const auto stdlib = (extended_root / L"Lib").wstring();
            for (auto* field : {&value.home, &value.prefix, &value.base_prefix,
                                &value.exec_prefix, &value.base_exec_prefix})
                require_status(PyConfig_SetString(&value, field, root_text.c_str()));
            for (auto* field : {&value.program_name, &value.executable,
                                &value.base_executable})
                require_status(PyConfig_SetString(&value, field, executable.c_str()));
            require_status(PyConfig_SetString(&value, &value.stdlib_dir, stdlib.c_str()));
            require_status(PyWideStringList_Append(&value.argv, L"cad_library_worker"));
            for (const auto& path : {zip, root_text, packages})
                require_status(PyWideStringList_Append(&value.module_search_paths, path.c_str()));
            require_status(Py_InitializeFromConfig(&value));
            const std::string_view version(Py_GetVersion());
            if (!version.starts_with("3.13.15 "))
                throw std::runtime_error("Bundled CAD interpreter version mismatch");
            check_library_versions();
        } catch (...) {
            // Failed initialization may have reached an initialized interpreter.
            if (Py_IsInitialized()) Py_FinalizeEx();
            throw;
        }
    }
    ~PythonInterpreter() { if (Py_IsInitialized()) Py_FinalizeEx(); }
    PythonInterpreter(const PythonInterpreter&) = delete;
    PythonInterpreter& operator=(const PythonInterpreter&) = delete;
};

nlohmann::json invoke_adapter(const char* function, std::string_view bytes,
                              std::span<const std::uint64_t> native_admitted_ids) {
    PythonObject adapter(PyImport_ImportModule("cad_library_adapter"));
    if (!adapter.get()) python_error("Cannot load bundled CAD adapter");
    PythonObject callable(PyObject_GetAttrString(adapter.get(), function));
    if (!callable.get() || !PyCallable_Check(callable.get()))
        python_error("Cannot resolve bundled CAD adapter function");
    PythonObject input(PyBytes_FromStringAndSize(bytes.data(), static_cast<Py_ssize_t>(bytes.size())));
    if (!input.get()) python_error("Cannot allocate CAD adapter input");
    PythonObject admitted(PyTuple_New(static_cast<Py_ssize_t>(native_admitted_ids.size())));
    if (!admitted.get()) python_error("Cannot allocate native IFC admission identities");
    for (std::size_t i = 0; i < native_admitted_ids.size(); ++i) {
        auto* id = PyLong_FromUnsignedLongLong(native_admitted_ids[i]);
        if (!id) python_error("Cannot encode native IFC admission identity");
        if (PyTuple_SetItem(admitted.get(), static_cast<Py_ssize_t>(i), id) != 0)
            python_error("Cannot construct native IFC admission identities");
    }
    PythonObject args(std::strcmp(function, "project_ifc") == 0
        ? PyTuple_Pack(2, input.get(), admitted.get()) : PyTuple_Pack(1, input.get()));
    if (!args.get()) python_error("Cannot allocate CAD adapter arguments");
    PythonObject result(PyObject_CallObject(callable.get(), args.get()));
    if (!result.get()) python_error("CAD library rejected document");

    PythonObject json(PyImport_ImportModule("json"));
    if (!json.get()) python_error("Cannot load bundled JSON serializer");
    PythonObject dumps(PyObject_GetAttrString(json.get(), "dumps"));
    PythonObject serialization_args(PyTuple_Pack(1, result.get()));
    PythonObject kwargs(PyDict_New());
    // Use functions rather than Py_True/Py_False DLL data imports, which MSVC
    // cannot delay-load. These still return Python's immutable bool singletons.
    PythonObject false_value(PyBool_FromLong(0));
    PythonObject true_value(PyBool_FromLong(1));
    if (!dumps.get() || !PyCallable_Check(dumps.get()) || !serialization_args.get() || !kwargs.get() ||
        !false_value.get() || !true_value.get() ||
        PyDict_SetItemString(kwargs.get(), "allow_nan", false_value.get()) != 0 ||
        PyDict_SetItemString(kwargs.get(), "ensure_ascii", true_value.get()) != 0)
        python_error("Cannot configure bundled JSON serializer");
    PythonObject serialized(PyObject_Call(dumps.get(), serialization_args.get(), kwargs.get()));
    if (!serialized.get() || !PyUnicode_Check(serialized.get()))
        python_error("Cannot serialize CAD adapter result");
    const auto characters = PyUnicode_GetLength(serialized.get());
    if (characters < 0) python_error("Cannot inspect CAD adapter result");
    if (static_cast<std::size_t>(characters) > json_limit)
        throw std::runtime_error("CAD adapter result exceeds transport limit");
    Py_ssize_t length = 0;
    const auto* text = PyUnicode_AsUTF8AndSize(serialized.get(), &length);
    if (!text) python_error("Cannot encode CAD adapter result");
    if (length < 0 || static_cast<std::size_t>(length) > json_limit)
        throw std::runtime_error("CAD adapter result exceeds transport limit");
    // The iterator overload requires the whole input to be valid JSON, rejects
    // comments/trailing content, and does not accept NaN or Infinity.
    return nlohmann::json::parse(text, text + length, nullptr, true, false);
}
#endif

} // namespace

nlohmann::json call_cad_library(const std::filesystem::path& runtime_root,
                              const char* function, std::string_view bytes,
                              std::span<const std::uint64_t> native_admitted_ids) {
#ifdef _WIN32
    if (!function || (std::strcmp(function, "normalize_dxf") != 0 &&
                      std::strcmp(function, "project_ifc") != 0))
        throw std::invalid_argument("Unsupported CAD adapter function");
    if (native_admitted_ids.size() > 100'000 ||
        (std::strcmp(function, "project_ifc") != 0 && !native_admitted_ids.empty()) ||
        std::any_of(native_admitted_ids.begin(), native_admitted_ids.end(), [](const auto id) { return id == 0; }))
        throw std::invalid_argument("Invalid native IFC admission identities");
    if (!runtime_root.is_absolute() || runtime_root.native().find(L'\0') != std::wstring::npos)
        throw std::invalid_argument("CAD runtime directory must be an absolute path");
    if (bytes.size() > input_limit ||
        bytes.size() > static_cast<std::size_t>((std::numeric_limits<Py_ssize_t>::max)()))
        throw std::invalid_argument("CAD adapter input exceeds transport limit");
    SilencedOutput output;
    nlohmann::json result;
    {
        const auto root = runtime_root.lexically_normal();
#ifdef VERTEX_CAD_CONTROLLED_RUNTIME
        // Keep all verified endpoints open before any delay-loaded Python call,
        // through imports, adapter execution, and interpreter finalization.
        const ControlledRuntimeIdentity identity(runtime_root);
#endif
        PythonRuntimeDll dll(root);
        PythonInterpreter interpreter(root);
#ifdef VERTEX_CAD_CONTROLLED_RUNTIME
        check_controlled_ifc_modules(identity);
#endif
        result = invoke_adapter(function, bytes, native_admitted_ids);
    }
    output.restore();
    return result;
#else
    static_cast<void>(runtime_root);
    static_cast<void>(function);
    static_cast<void>(bytes);
    static_cast<void>(native_admitted_ids);
    throw std::runtime_error("CAD library embedding is only supported on Windows");
#endif
}

} // namespace sketch::desktop
