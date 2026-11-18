#include "skeet/hud/composite_runtime.hpp"
#include "skeet/engine/interface_call.hpp"
#include "skeet/menu_window.hpp"
#include "skeet/render/backend_container.hpp"
#include "skeet/script/paint.hpp"
#include <immintrin.h>

namespace skeet::hud {
HudFrameCallback* register_hud_frame(engine::OwnerRegistryBinding registry,
                                     HudFrameCallback* callback) {
    registry.append(callback);
    return callback;
}
void dispatch_hud_frame(const engine::OwnerRegistry& callbacks, ui::RenderQueue& queue,
                        void* player, std::int32_t& cursor) {
    const auto entries = callbacks.entries();
    for (auto* object : entries) {
        auto* callback = static_cast<HudFrameCallback*>(object);
        callback->vtable[0](callback, &queue, player, &cursor);
    }
}
bool draw_composite_frame(const CompositeFrameContext& context) {
    if (context.blocked)
        return false;
    auto* renderer = context.renderer;
    if (!renderer)
        return false;
    auto* queue = renderer->begin();
    if (!queue)
        return false;
    for (auto* window = context.windows; window; window = window->next)
        window->refresh_viewport();
    //433CC0D6 is CVTSS2SI, including the caller's current MXCSR rounding mode.
    const auto scaled = _mm_mul_ss(_mm_set_ss(context.scale), _mm_set_ss(350.0f));
    auto cursor = _mm_cvtss_si32(scaled);
    queue->set_alpha(1.0f);
    if (engine::invoke<bool>(context.engine, 26)) {
        const auto index = engine::invoke<std::int32_t>(context.engine, 12);
        auto* player = engine::invoke<void*>(context.entity_list, 3, index);
        if (player)
            dispatch_hud_frame(context.callbacks, *queue, player, cursor);
    }
    queue->set_alpha(1.0f);
    script::dispatch_paint_ui(context.scripts, *queue, cursor);
    for (auto* window = context.windows; window; window = window->next)
        if (!window->startup_placement_pending &&
            ((window->input.mouse_flags & 1u) || window->transition_state))
            window->draw(*queue, context.accent);
    renderer->publish();
    return true;
}
} // namespace skeet::hud
