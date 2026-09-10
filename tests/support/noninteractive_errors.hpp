#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#endif

namespace sketch::testing {
// Process-local test behavior only. No registry, system-wide error policy or
// user's normal application behavior is changed. Failures keep nonzero exits.
inline void noninteractive_errors() {
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#ifdef _MSC_VER
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
    const int report_types[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
    for (int type : report_types) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
#endif
#endif
    std::set_terminate([] {
        std::fputs("Test terminated: ", stderr);
        if (const auto error = std::current_exception()) {
            try { std::rethrow_exception(error); }
            catch (const std::exception& exception) { std::fputs(exception.what(), stderr); }
            catch (...) { std::fputs("unknown exception", stderr); }
        } else {
            std::fputs("terminate called without an active exception", stderr);
        }
        std::fputc('\n', stderr);
        std::fflush(stderr);
        std::_Exit(86);
    });
}
} // namespace sketch::testing
