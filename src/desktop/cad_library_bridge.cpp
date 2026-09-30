#include "cad_library_bridge.hpp"

#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>

// The bundled interpreter is a release build, including in a Debug worker.
// Python's Windows SDK otherwise auto-links python313_d.lib and changes its ABI.
#pragma push_macro("_DEBUG")
#undef _DEBUG
#include <Python.h>
#pragma pop_macro("_DEBUG")

#if PY_MAJOR_VERSION != 3 || PY_MINOR_VERSION != 13 || PY_MICRO_VERSION != 15
#error The CAD worker requires the pinned CPython 3.13.15 SDK.
#endif
#endif

namespace sketch::desktop {
namespace {

#ifdef _WIN32
constexpr std::size_t input_limit = 64U * 1024U * 1024U;
constexpr std::size_t json_limit = 32U * 1024U * 1024U;

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
            if (CompareStringOrdinal(loaded.data(), static_cast<int>(loaded.size()),
                                     expected.data(), static_cast<int>(expected.size()), TRUE) != CSTR_EQUAL ||
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
        {"ezdxf", "1.4.3"}, {"ifcopenshell", "0.8.3.post2"}};
    for (const auto& library : libraries) {
        PythonObject name(PyUnicode_FromString(library.package));
        if (!name.get()) python_error("Cannot inspect bundled CAD library versions");
        PythonObject actual(PyObject_CallOneArg(version.get(), name.get()));
        if (!actual.get() || !PyUnicode_Check(actual.get()))
            python_error("Cannot inspect bundled CAD library versions");
        const auto equal = PyUnicode_CompareWithASCIIString(actual.get(), library.version);
        if (equal != 0) python_error("Bundled CAD library version mismatch");
    }
}

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

nlohmann::json invoke_adapter(const char* function, std::string_view bytes) {
    PythonObject adapter(PyImport_ImportModule("cad_library_adapter"));
    if (!adapter.get()) python_error("Cannot load bundled CAD adapter");
    PythonObject callable(PyObject_GetAttrString(adapter.get(), function));
    if (!callable.get() || !PyCallable_Check(callable.get()))
        python_error("Cannot resolve bundled CAD adapter function");
    PythonObject input(PyBytes_FromStringAndSize(bytes.data(), static_cast<Py_ssize_t>(bytes.size())));
    if (!input.get()) python_error("Cannot allocate CAD adapter input");
    PythonObject result(PyObject_CallOneArg(callable.get(), input.get()));
    if (!result.get()) python_error("CAD library rejected document");

    PythonObject json(PyImport_ImportModule("json"));
    if (!json.get()) python_error("Cannot load bundled JSON serializer");
    PythonObject dumps(PyObject_GetAttrString(json.get(), "dumps"));
    PythonObject args(PyTuple_Pack(1, result.get()));
    PythonObject kwargs(PyDict_New());
    // Use functions rather than Py_True/Py_False DLL data imports, which MSVC
    // cannot delay-load. These still return Python's immutable bool singletons.
    PythonObject false_value(PyBool_FromLong(0));
    PythonObject true_value(PyBool_FromLong(1));
    if (!dumps.get() || !PyCallable_Check(dumps.get()) || !args.get() || !kwargs.get() ||
        !false_value.get() || !true_value.get() ||
        PyDict_SetItemString(kwargs.get(), "allow_nan", false_value.get()) != 0 ||
        PyDict_SetItemString(kwargs.get(), "ensure_ascii", true_value.get()) != 0)
        python_error("Cannot configure bundled JSON serializer");
    PythonObject serialized(PyObject_Call(dumps.get(), args.get(), kwargs.get()));
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
                              const char* function, std::string_view bytes) {
#ifdef _WIN32
    if (!function || (std::strcmp(function, "normalize_dxf") != 0 &&
                      std::strcmp(function, "project_ifc") != 0))
        throw std::invalid_argument("Unsupported CAD adapter function");
    if (!runtime_root.is_absolute() || runtime_root.native().find(L'\0') != std::wstring::npos)
        throw std::invalid_argument("CAD runtime directory must be an absolute path");
    if (bytes.size() > input_limit ||
        bytes.size() > static_cast<std::size_t>((std::numeric_limits<Py_ssize_t>::max)()))
        throw std::invalid_argument("CAD adapter input exceeds transport limit");
    SilencedOutput output;
    nlohmann::json result;
    {
        const auto root = runtime_root.lexically_normal();
        PythonRuntimeDll dll(root);
        PythonInterpreter interpreter(root);
        result = invoke_adapter(function, bytes);
    }
    output.restore();
    return result;
#else
    static_cast<void>(runtime_root);
    static_cast<void>(function);
    static_cast<void>(bytes);
    throw std::runtime_error("CAD library embedding is only supported on Windows");
#endif
}

} // namespace sketch::desktop
