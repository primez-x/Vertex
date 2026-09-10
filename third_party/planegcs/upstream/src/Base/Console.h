// Compatibility shim for the extracted PlaneGCS target.
#pragma once

#include "Tools.h"

namespace Base {

class ConsoleSingleton {
public:
    template <typename... Arguments>
    void log(const char*, Arguments&&...) const noexcept {}

    template <typename... Arguments>
    void warning(const char*, Arguments&&...) const noexcept {}
};

inline ConsoleSingleton& Console() noexcept {
    static ConsoleSingleton console;
    return console;
}

}  // namespace Base
