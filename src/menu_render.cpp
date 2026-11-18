#include "skeet/menu_tree.hpp"
#include "skeet/scrollbar_render.hpp"
#include "skeet/text_metrics.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace skeet::ui {
namespace {
using i32 = std::int32_t;
i32 add(i32 a, i32 b) noexcept {
    return std::bit_cast<i32>(std::uint32_t(a) + std::uint32_t(b));
}
i32 sub(i32 a, i32 b) noexcept {
    return std::bit_cast<i32>(std::uint32_t(a) - std::uint32_t(b));
}
void merge_draw(std::span<Widget*> left, std::span<Widget*> right,
                Widget** output) { // 0x433E8B57 / 0x4333C986
    std::size_t l = 0, r = 0;
    while (l < left.size() && r < right.size()) {
        const auto right_priority = right[r]->input_priority();
        const auto left_priority = left[l]->input_priority();
        *output++ = right_priority < left_priority ? right[r++] : left[l++];
    }
    while (l < left.size())
        *output++ = left[l++];
    while (r < right.size())
        *output++ = right[r++];
}
void sort_draw(std::span<Widget*> input, std::span<Widget*> scratch) { // 0x433ED024
    if (input.size() <= 1)
        return;
    const auto middle = input.size() / 2;
    auto prepare = [&](std::span<Widget*> part, std::span<Widget*> temporary) {
        if (part.size() == 1) {
            temporary[0] = part[0];
            return;
        }
        const auto half = part.size() / 2;
        sort_draw(part.first(half), temporary.first(half));
        sort_draw(part.subspan(half), temporary.subspan(half));
        merge_draw(part.first(half), part.subspan(half), temporary.data());
    };
    prepare(input.first(middle), scratch.first(middle));
    prepare(input.subspan(middle), scratch.subspan(middle));
    merge_draw(scratch.first(middle), scratch.subspan(middle), input.data());
}
void arrow(RenderQueue& queue, Point point, std::uint32_t color, bool down) {
    const auto step = down ? 1 : -1;
    queue.rectangle({point.x, sub(point.y, step), 5, 1, 0xff000000});
    queue.rectangle({point.x, point.y, 5, 1, color});
    queue.rectangle({add(point.x, 1), add(point.y, step), 3, 1, color});
    queue.rectangle({add(point.x, 2), add(point.y, step * 2), 1, 1, color});
}
}
void draw_up_arrow(RenderQueue& queue, Point point, std::uint32_t color) {
    arrow(queue, point, color, false);
} // 0x43326909
void draw_down_arrow(RenderQueue& queue, Point point, std::uint32_t color) {
    arrow(queue, point, color, true);
} // 0x433C9F33
void draw_scrollbar(RenderQueue& queue, const Scrollbar& scroll, Point origin, float scale,
                    std::uint32_t alpha) { // 0x433E7AFE
    const auto width = round_layout_coordinate(scale * 7.0f);
    const auto track_color = ((alpha & 255) << 24) | 0x00282828U;
    const auto thumb_color = ((alpha & 255) << 24) | 0x00414141U;
    queue.rectangle({sub(add(origin.x, sub(scroll.viewport.x, width)), 1), add(origin.y, 1), width,
                     sub(scroll.viewport.y, 2), track_color});
    const auto thumb = scrollbar_thumb(scroll, 7, scale);
    queue.rectangle(
        {add(origin.x, thumb.x), add(origin.y, thumb.y), thumb.width, thumb.height, thumb_color});
}
std::vector<Widget*> Category::draw_order() const { // 0x433FC71A
    std::vector<Widget*> ordered;
    ordered.reserve(std::min(elements.size(), std::size_t(1024)));
    for (auto* child : elements) {
        if (child->visible) {
            ordered.push_back(child);
            if (ordered.size() == 1024)
                break;
        }
    }
    std::vector<Widget*> temporary(ordered.size());
    sort_draw(ordered, temporary);
    return ordered;
}
void Category::draw(RenderQueue& queue, Point origin, std::uint32_t accent) const { // 0x433AE90F
    const auto& b = geometry.bounds;
    const Point position{add(origin.x, b.x), add(origin.y, b.y)};
    const auto border = drag_state == 2 ? accent : inner_border;
    const bool titled = show_title && !name.empty();
    std::u16string_view title(name);
    if (titled && title.size() >= 256)
        throw std::length_error("Category title exceeds its native UTF-16 buffer");
    auto metric_title = title;
    if (const auto zero = metric_title.find(char16_t(0)); zero != metric_title.npos)
        metric_title = metric_title.substr(0, zero);
    if (titled) { // 0x4334AF01
        queue.rectangle({add(position.x, 2), add(position.y, 2), sub(b.width, 3), sub(b.height, 3),
                         background});
        queue.rectangle({position.x, position.y, 10, 1, outer_border});
        queue.rectangle({add(position.x, 1), add(position.y, 1), 9, 1, border});
        const auto size = measure_text(queue, metric_title, text_flags);
        const auto right_x = add(add(position.x, 17), size.x),
                   right_width = sub(sub(b.width, size.x), 16);
        queue.rectangle({right_x, position.y, right_width, 1, outer_border});
        queue.rectangle({right_x, add(position.y, 1), right_width, 1, border});
        queue.rectangle({position.x, position.y, 1, b.height, outer_border});
        queue.rectangle({add(position.x, 1), add(position.y, 1), 1, b.height, border});
        queue.rectangle({add(position.x, b.width), position.y, 1, b.height, outer_border});
        queue.rectangle(
            {add(sub(position.x, 1), b.width), add(position.y, 1), 1, b.height, border});
        queue.rectangle({position.x, add(position.y, b.height), add(b.width, 1), 1, outer_border});
        queue.rectangle(
            {add(position.x, 1), add(sub(position.y, 1), b.height), sub(b.width, 1), 1, border});
    } else {
        queue.rectangle_outline({position.x, position.y, b.width, b.height, outer_border}, 1);
        queue.rectangle_outline(
            {add(position.x, 1), add(position.y, 1), sub(b.width, 2), sub(b.height, 2), border}, 1);
        queue.rectangle({add(position.x, 2), add(position.y, 2), sub(b.width, 4), sub(b.height, 4),
                         background});
    }
    if (titled) {
        const auto size = measure_text(queue, metric_title, text_flags);
        queue.text(add(position.x, 14), sub(position.y, size.y / 2),
                   drag_state == 1 ? accent : title_color, text_flags, 0, title);
        if (!scroll.enabled && resizable) {
            const auto x = add(sub(position.x, 1), b.width), y = add(sub(position.y, 1), b.height);
            queue.triangle({sub(x, 5), y, x, sub(y, 5), x, y, border});
        }
    }
    if (!visible || elements.empty())
        return; // 0x433D0330
    const bool had_bounds = queue.is_bounds_reset();
    const auto saved_bounds = had_bounds ? queue.get_bounds() : std::array<i32, 4>{};
    if (!had_bounds)
        queue.reset_bounds(true);
    queue.set_bounds({position.x, scroll.enabled ? add(position.y, 7) : position.y, b.width,
                      scroll.enabled ? sub(b.height, 9) : b.height});
    const auto offset = scroll.enabled ? scroll.position : 0;
    const Point content{position.x, sub(position.y, offset)};
    {
        std::shared_lock lock(elements_mutex); // RtlAcquireSRWLockShared / RtlReleaseSRWLockShared
        const auto bottom = add(b.height, offset);
        std::array<Widget*, 1024> ordered, scratch;
        std::size_t visible_count = 0, draw_count = 0;
        for (auto* child : std::span(elements)) {
            const auto& bounds = child->geometry.bounds;
            const auto child_bottom = add(bounds.y, bounds.height);
            if (child_bottom < offset || bottom < child_bottom)
                child->reset_input();
            if (!child->visible || visible_count == ordered.size())
                continue;
            ++visible_count;
            if (bounds.y <= bottom && child_bottom >= offset)
                ordered[draw_count++] = child;
        }
        const auto on_screen = std::span(ordered).first(draw_count);
        sort_draw(on_screen, std::span(scratch).first(draw_count));
        for (auto* child : on_screen)
            child->draw(queue, content, accent);
    }
    if (had_bounds)
        queue.set_bounds(saved_bounds);
    else
        queue.reset_bounds(false);
    if (!scroll.enabled)
        return;
    const auto* menu = find_menu();
    if (!menu)
        throw std::logic_error("Category scrollbar rendering requires its menu context");
    const auto scale = menu->scale;
    const auto saved_depth = queue.get_depth();
    queue.set_depth(0.25f);
    if (scroll.position > 0) {
        queue.vertical_gradient(
            {add(position.x, 2), add(position.y, 7), sub(b.width, 9), 20, 0xff171717, 0x00171717});
        draw_up_arrow(queue,
                      {sub(add(position.x, b.width), round_layout_coordinate(scale * 17.0f)),
                       add(position.y, round_layout_coordinate(scale * 9.0f))},
                      0xc8ffffff);
    }
    if (scroll.position < sub(scroll.content_height, scroll.viewport.y)) {
        queue.vertical_gradient({add(position.x, 2), add(add(position.y, b.height), -21),
                                 sub(b.width, 9), 20, 0x00171717, 0xff171717});
        draw_down_arrow(queue,
                        {sub(add(position.x, b.width), round_layout_coordinate(scale * 17.0f)),
                         add(sub(position.y, round_layout_coordinate(scale * 9.0f)), b.height)},
                        0xc8ffffff);
    }
    draw_scrollbar(queue, scroll, {add(position.x, 1), position.y}, scale);
    queue.set_depth(saved_depth);
}
}
