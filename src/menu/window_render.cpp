#include "skeet/menu/window_render.hpp"
#include "skeet/menu_window.hpp"
#include "skeet/popup_menu.hpp"
#include <bit>

namespace skeet::ui {
namespace {
using i32 = std::int32_t;
i32 add(i32 a, i32 b) noexcept {
    return std::bit_cast<i32>(std::uint32_t(a) + std::uint32_t(b));
}
i32 sub(i32 a, i32 b) noexcept {
    return std::bit_cast<i32>(std::uint32_t(a) - std::uint32_t(b));
}
}
void draw_window_frame(RenderQueue& queue, const WindowRenderState& window) { // 0x433AD3F2
    const auto& b = window.bounds;
    queue.rectangle_outline({b.x, b.y, b.width, b.height, 0xff0c0c0c}, 1);
    queue.rectangle_outline(
        {add(b.x, 1), add(b.y, 1), sub(b.width, 2), sub(b.height, 2), 0xff3c3c3c}, 1);
    queue.rectangle({add(b.x, 2), add(b.y, 2), sub(b.width, 4), sub(b.height, 4), 0xff282828});
    queue.rectangle_outline(
        {add(b.x, 5), add(b.y, 5), sub(b.width, 10), sub(b.height, 10), 0xff3c3c3c}, 1);
    queue.rectangle({add(b.x, 6), add(b.y, 6), sub(b.width, 12), sub(b.height, 12), 0xff0c0c0c});
    if (window.background_texture != UINT32_MAX)
        queue.texture({add(b.x, 6), add(b.y, 10), window.background_texture, 0xffffffff, 0, 0,
                       sub(b.width, 12), sub(b.height, 16), 0, 1.0f});
}
void draw_window_accent(RenderQueue& queue, const WindowRenderState& window) { // 0x4338490E
    const auto x = add(window.bounds.x, 7), y = add(window.bounds.y, 7);
    const auto width = sub(window.bounds.width, 14);
    const auto first_width = width / 2;
    queue.horizontal_gradient({x, y, first_width, 2, 0xffdab137, 0xffcd46cc});
    queue.horizontal_gradient(
        {add(x, first_width), y, sub(width, first_width), 2, 0xffcd46cc, 0xff35e3cc});
    queue.rectangle({x, add(y, 1), width, 2, 0x77000000});
}
void draw_window_decorations(RenderQueue& queue, const WindowRenderState& window,
                             const Rectangle& active_tab) { // 0x433D2AB3
    draw_window_frame(queue, window);
    const auto& b = window.bounds;
    const auto x = add(b.x, 6), y = add(b.y, 10);
    const auto width = active_tab.width, top_height = sub(active_tab.y, 10);
    const auto black_x = add(x, sub(width, 2)), border_x = add(x, sub(width, 1));
    if (add(top_height, add(active_tab.height, 20)) < b.height) {
        queue.rectangle({x, y, width, top_height, 0xff0c0c0c});
        queue.rectangle({black_x, y, 1, sub(top_height, 1), 0xff000000});
        queue.rectangle({border_x, y, 1, top_height, 0xff282828});
        queue.rectangle({x, add(y, sub(top_height, 2)), sub(width, 1), 1, 0xff000000});
        queue.rectangle({x, add(y, sub(top_height, 1)), width, 1, 0xff282828});
        const auto bottom_y = add(add(y, top_height), active_tab.height);
        const auto bottom_height = add(sub(b.height, bottom_y), sub(b.y, 6));
        queue.rectangle({x, bottom_y, width, bottom_height, 0xff0c0c0c});
        queue.rectangle({black_x, add(bottom_y, 1), 1, sub(bottom_height, 1), 0xff000000});
        queue.rectangle({border_x, bottom_y, 1, bottom_height, 0xff282828});
        queue.rectangle({x, bottom_y, width, 1, 0xff282828});
        queue.rectangle({x, add(bottom_y, 1), sub(width, 1), 1, 0xff000000});
    } else {
        const auto height = add(sub(b.height, y), sub(b.y, 6));
        queue.rectangle({x, y, width, height, 0xff0c0c0c});
        queue.rectangle({black_x, add(y, 1), 1, sub(height, 1), 0xff000000});
        queue.rectangle({border_x, y, 1, height, 0xff282828});
    }
    draw_window_accent(queue, window);
}
void MenuWindow::update_transition(RenderQueue& queue) { // 0x43384045
    if (transition_state == 2) {
        const auto elapsed = read_ticks(clock_context) - transition_started;
        opacity = static_cast<float>(elapsed) / 150.0f;
        if (opacity > 1.0f) {
            transition_state = 0;
            opacity = 1.0f;
        }
    } else if (transition_state == 1) {
        if (tab_context_menu)
            tab_context_menu->reset_input();
        const auto elapsed = read_ticks(clock_context) - transition_started;
        auto progress = static_cast<float>(elapsed) / 150.0f;
        if (progress > 1.0f) {
            finish_resize();
            transition_state = 0;
            opacity = 1.0f;
            auto flags = input.mouse_flags;
            if (!tabs.empty()) {
                for (auto* tab : std::span(tabs))
                    tab->reset_input();
                flags = input.mouse_flags;
                progress = opacity;
            } else
                progress = 1.0f;
            input.mouse_flags = flags & ~std::uint32_t(1);
        }
        opacity = 1.0f - progress;
    } else
        opacity = 1.0f;
    queue.set_alpha(opacity);
}
void MenuWindow::draw(RenderQueue& queue, std::uint32_t accent) { // 0x433AC5E2
    update_transition(queue);
    if (!(input.mouse_flags & 1))
        return;
    const WindowRenderState state{{position.x, position.y, geometry.size.x, geometry.size.y},
                                  background_texture};
    draw_window_decorations(queue, state, tabs.at(navigation.active_tab)->geometry.bounds);
    for (auto* tab : std::span(tabs))
        if (tab->visible)
            tab->draw(queue, position, accent);
    if (tab_context_menu && tab_context_menu->open && tab_context_menu->parent->visible) {
        const auto& bounds = tab_context_menu->parent->geometry.bounds;
        tab_context_menu->draw(queue, {add(position.x, bounds.x), add(position.y, bounds.y)}, accent);
    }
}
}
