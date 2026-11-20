#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace skeet::engine {
inline const std::byte* studio_hitbox_set(const std::byte* header, std::int32_t index) noexcept {
    if (!header || index < 0)
        return nullptr;
    std::int32_t count, offset;
    std::memcpy(&count, header + 0xac, sizeof count);
    std::memcpy(&offset, header + 0xb0, sizeof offset);
    if (index >= count || offset <= 0 ||
        index > (std::numeric_limits<std::ptrdiff_t>::max() - offset) / 12)
        return nullptr;
    return header + offset + std::ptrdiff_t(index) * 12;
}
}
