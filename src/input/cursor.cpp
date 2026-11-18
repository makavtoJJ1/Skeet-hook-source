#include "skeet/input/runtime.hpp"

namespace skeet::input {
void Runtime::enable_cursor() { // 0x433C0428
    if (disabled)
        return;
    if (!cursor_context)
        cursor_context = engine::invoke<std::uint32_t>(interfaces.cursor_contexts, 9);
    if (cursor_context)
        engine::invoke<void>(interfaces.cursor_contexts, 11, cursor_context, true);
}
void Runtime::release_cursor() { // 0x4339F7FB
    Point position{};
    if (cursor_context) {
        if (interfaces.in_game()) {
            engine::invoke<void>(interfaces.engine_client, 5, &position.x, &position.y);
            position.x >>= 1;
            position.y >>= 1;
            if (engine::invoke<bool>(interfaces.cursor_contexts, 16, cursor_context))
                engine::invoke<void>(interfaces.cursor_contexts, 15, cursor_context, position.x,
                                     position.y);
        }
        engine::invoke<void>(interfaces.cursor_contexts, 11, cursor_context, false);
    }
    if (interfaces.in_game())
        engine::invoke<void>(interfaces.input_system, 61, &position.x, &position.y);
}
void Runtime::destroy_cursor() { // 0x433675D9
    if (!interfaces.cursor_contexts || !cursor_context)
        return;
    engine::invoke<void>(interfaces.cursor_contexts, 11, cursor_context, true);
    if (engine::invoke<bool>(interfaces.cursor_contexts, 16, cursor_context))
        engine::invoke<void>(interfaces.cursor_contexts, 10);
    else
        engine::invoke<void>(interfaces.cursor_contexts, 11, cursor_context, false);
    cursor_context = 0;
}
void Runtime::toggle_menu() { // 0x4338579D
    if (main_menu->input.mouse_flags & 1)
        release_cursor();
    else
        enable_cursor();
    if (!(main_menu->input.mouse_flags & 1)) {
        main_menu->open();
        return;
    }
    if (main_menu->transition_state == 1)
        return;
    for (auto* window = windows; window; window = window->next) {
        if (!(window->input.mouse_flags & 1))
            continue;
        window->finish_resize();
        window->transition_started = window->read_ticks(window->clock_context);
        window->transition_state = 1;
    }
}
}
