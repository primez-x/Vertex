#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace sketch {

// Convert only at the filesystem/API boundary. Retain the caller's original path for
// receipts, diagnostics and callbacks, and retain existing qualified namespace semantics.
// Short ordinary paths keep Win32's alias handling (including existing trailing-dot names).
inline std::filesystem::path windows_project_path(const std::filesystem::path& path) {
    auto preferred = path;
    preferred.make_preferred();
    const auto& name = preferred.native();
    if (name.starts_with(L"\\\\?\\") || name.starts_with(L"\\\\.\\")) {
        return preferred;
    }
    const auto required = GetFullPathNameW(name.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        throw std::filesystem::filesystem_error("cannot resolve Windows project path", path,
            std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(required), L'\0');
    const auto written = GetFullPathNameW(name.c_str(), required, buffer.data(), nullptr);
    if (written == 0 || written >= required) {
        const auto error = written == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        throw std::filesystem::filesystem_error("cannot resolve Windows project path", path,
            std::error_code(static_cast<int>(error), std::system_category()));
    }
    std::wstring absolute(buffer.data(), written);
    // Reserve room for SQLite's longest exact sidecar suffix and a terminating NUL.
    if (absolute.size() < MAX_PATH - 12) {
        return std::filesystem::path(std::move(absolute));
    }
    if (absolute.starts_with(L"\\\\")) {
        return std::filesystem::path(L"\\\\?\\UNC\\" + absolute.substr(2));
    }
    return std::filesystem::path(L"\\\\?\\" + absolute);
}

}  // namespace sketch
#endif
