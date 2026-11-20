#pragma once
#include <array>
#include <cstddef>
#include <stdexcept>

namespace skeet {
class BulletFilterMethods {
public:
    static constexpr std::size_t source_slot = 2, last_hit_slot = 4;

    BulletFilterMethods(const BulletFilterMethods&) = delete;
    BulletFilterMethods& operator=(const BulletFilterMethods&) = delete;
    BulletFilterMethods(void* const* engine, void* should_hit, void* trace_type = nullptr) {
        if (!engine)
            throw std::logic_error("Missing engine bullet filter table");
        entries_ = {should_hit, trace_type ? trace_type : engine[1], engine[source_slot],
                    engine[3], engine[last_hit_slot]};
    }
    void** data() noexcept { return entries_.data(); }

private:
    std::array<void*, last_hit_slot + 1> entries_;
};
} // namespace skeet
