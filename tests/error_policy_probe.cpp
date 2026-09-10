#include <cstdlib>
#include <cstring>
#include <stdexcept>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
// Exercise assertion handling in Release as well as Debug.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
int main(int argc, char** argv) {
    // Deliberately rely on the CMake-linked process initializer. This proves
    // tests are guarded even when their entry point omits an explicit call.
    if (argc == 2 && std::strcmp(argv[1], "--throw") == 0) throw std::runtime_error("expected test failure");
    if (argc == 2 && std::strcmp(argv[1], "--abort") == 0) std::abort();
    if (argc == 2 && std::strcmp(argv[1], "--assert") == 0) {
        assert(false && "expected assertion failure");
    }
    if (argc == 2 && std::strcmp(argv[1], "--policy") == 0) {
#ifdef _MSC_VER
        return _set_error_mode(_REPORT_ERRMODE) == _OUT_TO_STDERR ? 0 : 3;
#else
        return 0;
#endif
    }
    return 2;
}
