#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

inline void check(bool condition, std::string_view contract) {
    if (!condition) {
        throw std::logic_error(std::string(contract));
    }
}
