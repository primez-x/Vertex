#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <array>
#include <cwchar>
#include <string_view>

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--sleep-ms") {
        wchar_t* end = nullptr;
        const auto milliseconds = wcstoul(argv[2], &end, 10);
        if (!end || *end != L'\0' || milliseconds > 30'000) return 5;
        Sleep(milliseconds);
        return 0;
    }
    if (argc > 1 && std::wstring_view(argv[1]) != L"--echo") return 6;
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!input || input == INVALID_HANDLE_VALUE || !output || output == INVALID_HANDLE_VALUE) return 2;
    std::array<unsigned char, 64 * 1024> buffer{};
    for (;;) {
        DWORD received = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &received, nullptr)) return 3;
        if (received == 0) break;
        DWORD offset = 0;
        while (offset < received) {
            DWORD written = 0;
            if (!WriteFile(output, buffer.data() + offset, received - offset, &written, nullptr) || written == 0)
                return 4;
            offset += written;
        }
    }
    FlushFileBuffers(output);
    return 0;
}
