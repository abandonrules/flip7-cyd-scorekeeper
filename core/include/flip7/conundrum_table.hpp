#pragma once

#include <cstddef>

namespace flip7::countdown {

struct ConundrumEntry {
    const char* solution;
    const char* scramble;
    const char* hint;
};

extern const ConundrumEntry kConundrumTable[];
extern const std::size_t kConundrumTableSize;

} // namespace flip7::countdown
