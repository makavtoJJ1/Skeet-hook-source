#include "skeet/listbox.hpp"
#include "skeet/font_metrics.hpp"
#include "skeet/menu_tree.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <mutex>
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
i32 mul(i32 a, i32 b) noexcept {
    return std::bit_cast<i32>(std::uint32_t(a) * std::uint32_t(b));
}
i32 divide(i32 a, i32 b) {
    if (!b || (a == std::numeric_limits<i32>::min() && b == -1))
        throw std::overflow_error("Invalid listbox integer division");
    return a / b;
}
char16_t lower_ascii(char16_t c) noexcept {
    return c >= u'A' && c <= u'Z' ? c + 32 : c;
}
bool contains_search(std::u16string_view name, std::u16string_view query) {
    name = name.substr(0, name.find(u'\0'));
    query = query.substr(0, query.find(u'\0'));
    if (query.empty())
        return false; // 0x433D27C7 returns null for an empty needle.
    return std::search(name.begin(), name.end(), query.begin(), query.end(),
                       [](char16_t a, char16_t b) { return lower_ascii(a) == lower_ascii(b); }) !=
           name.end();
}
void arrow(RenderQueue& q, Point p, bool down) {
    q.rectangle({p.x, add(p.y, down ? -1 : 1), 5, 1, 0xff000000U});
    q.rectangle({p.x, p.y, 5, 1, 0xc8ffffffU});
    q.rectangle({add(p.x, 1), add(p.y, down ? 1 : -1), 3, 1, 0xc8ffffffU});
    q.rectangle({add(p.x, 2), add(p.y, down ? 2 : -2), 1, 1, 0xc8ffffffU});
}
}
Listbox::Listbox() {
    kind = 6;
}
Listbox::Listbox(Widget* owner, std::u16string_view label, Point size, i32* setting,
                 bool show_search, WidgetIdentities& identities) // 0x433EDA39
    : Widget(owner, label, 6, setting != nullptr, identities), value(setting),
      original_height(size.y), search_visible(show_search) {
    geometry.default_size = size;
    geometry.bounds.width = size.x;
    geometry.bounds.height = size.y;
    visible_rows = sub(divide(size.y, row_height), search_visible ? 1 : 0);
    if (size.x != 158)
        geometry.horizontal_offset = 0;
}
void Listbox::set_selection(std::uint32_t index) noexcept { // 0x433B0844
    if (index < options.size())
        selected_item = std::bit_cast<i32>(index);
}
void Listbox::set_options(std::vector<ListboxOption>* replacement) { // 0x433C9FBF
    auto* category = owning_category(this);
    if (!category)
        return;
    const auto selection = replacement ? std::uint32_t(selected_item) : 0U;
    const auto position = replacement && scroll.enabled ? scroll.position : 0;
    std::unique_lock lock(category->elements_mutex);
    set_selection(0);
    options.clear();
    filtered_indices.clear();
    if (replacement && replacement != &options)
        options.swap(*replacement);
    filter_options();
    set_selection(
        options.empty() ? 0 : std::min(selection, static_cast<std::uint32_t>(options.size() - 1)));
    scroll.position = position;
    visible_rows = sub(divide(geometry.bounds.height, row_height), search_visible ? 1 : 0);
    update_scroll();
}
void Listbox::filter_options() { // 0x433F27E3
    scroll.position = 0;
    if (search.empty()) {
        if (filtered_indices.size() != options.size()) {
            filtered_indices.resize(options.size());
            for (std::size_t i = 0; i < options.size(); ++i)
                filtered_indices[i] = static_cast<std::uint16_t>(i);
        }
    } else {
        filtered_indices.clear();
        filtered_indices.reserve(options.size());
        for (std::size_t i = 0; i < options.size(); ++i)
            if (contains_search(options[i].name, search))
                filtered_indices.push_back(static_cast<std::uint16_t>(i));
    }
    update_scroll();
}
void Listbox::update_scroll() noexcept { // 0x4333FF11
    ListboxLayout layout{geometry,
                         row_height,
                         search_visible,
                         visible_rows,
                         static_cast<i32>(filtered_indices.size()),
                         scroll};
    update_listbox_scroll(layout);
    scroll = layout.scroll;
}
i32 Listbox::option_at(Point cursor) const { // 0x4336B2E0
    if (filtered_indices.empty() || cursor.y < 0)
        return -1;
    const auto offset = scroll.enabled ? mul(divide(scroll.position, row_height), row_height) : 0;
    const auto index = divide(add(offset, cursor.y), row_height);
    return index >= 0 && std::uint32_t(index) < filtered_indices.size() ? filtered_indices[index]
                                                                        : -1;
}
bool Listbox::mouse(const InputMessage& message, Point cursor) { // 0x4336C227
    if (options.empty())
        return false;
    Point relative{sub(cursor.x, geometry.bounds.x), sub(cursor.y, geometry.bounds.y)};
    if (search_visible)
        relative.y = sub(relative.y, row_height);
    if (scroll.enabled && scrollbar_mouse(scroll, message.message, relative, scale))
        return true;
    if (message.message == 0x201 && hit_test(cursor)) {
        if (relative.y < 0)
            search_active = true;
        else {
            const auto index = option_at(relative);
            if (index == -1)
                return false;
            set_selection(std::uint32_t(index));
            if (value)
                *value = options[selected_item].value;
            notify_selection();
        }
        return true;
    }
    if (message.message == 0x202)
        scroll.dragging = false;
    else if (message.message == 0x20a && scroll.enabled && hit_test(cursor)) {
        const auto wheel =
            std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(message.parameter >> 16));
        move_scroll_position(scroll.position, scroll.viewport.y, scroll.content_height,
                             mul(row_height, wheel > 0 ? -4 : 4));
        return true;
    }
    return false;
}
bool Listbox::keyboard(const InputMessage& message) { // 0x433F2387
    if (options.empty())
        return false;
    const auto key = message.parameter;
    if (message.message == 0x100) {
        if (key == 0x26) {
            if (selected_item > 0) {
                set_selection(std::uint32_t(sub(selected_item, 1)));
                if (value)
                    *value = options[selected_item].value;
                notify_selection();
                const auto position = mul(selected_item, row_height);
                if (position < mul(divide(scroll.position, row_height), row_height)) {
                    scroll.position = position;
                    update_scroll();
                }
            }
            return true;
        }
        if (key == 0x28) {
            if (add(selected_item, 1) < static_cast<i32>(options.size())) {
                set_selection(std::uint32_t(add(selected_item, 1)));
                if (value)
                    *value = options[selected_item].value;
                notify_selection();
                if (mul(selected_item, row_height) >=
                    mul(add(divide(scroll.position, row_height), visible_rows), row_height)) {
                    scroll.position = mul(add(sub(selected_item, visible_rows), 1), row_height);
                    update_scroll();
                }
            }
            return true;
        }
        if (key == 0xc0) {
            reset_input();
            return false;
        }
        if (scroll.enabled && static_cast<i32>(options.size()) >= visible_rows &&
            (key == 0x21 || key == 0x22)) {
            move_scroll_position(
                scroll.position, scroll.viewport.y, scroll.content_height,
                mul(key == 0x21 ? sub(1, visible_rows) : sub(visible_rows, 1), row_height));
            return true;
        }
        if (!search_active || !search_visible)
            return false;
        if (key == 8) {
            if (search.empty())
                return false;
            search.pop_back();
            filter_options();
            return true;
        }
        return key >= 0x20 && key <= 0x7e;
    }
    if (message.message != 0x102 || !search_active || !search_visible || key < 0x20 || key > 0x7e ||
        search.size() >= 16)
        return false;
    search.push_back(static_cast<char16_t>(key));
    filter_options();
    return true;
}
void Listbox::config_loaded() { // 0x433293EA
    if (!value)
        return;
    for (std::size_t i = 0; i < options.size(); ++i)
        if (options[i].value == *value) {
            set_selection(static_cast<std::uint32_t>(i));
            return;
        }
}
void Listbox::value_changed() {
    config_loaded();
    notify_callbacks();
} // 0x433EE5AD
void Listbox::notify_selection() {
    notify_callbacks();
    for (const auto& callback : selection_callbacks)
        callback.function(callback.context, *this);
}
void Listbox::reset_input() {
    hovered = search_active = scroll.dragging = false;
} // 0x433BA433
void Listbox::resize(const SliderMetrics& metrics) { // 0x4333C9A7
    if (!parent)
        throw std::logic_error("Listbox resizing requires an owning category");
    const auto width = sub(parent->geometry.bounds.width, metrics.category_padding);
    geometry.bounds.width = width < metrics.minimum_width
                                ? metrics.minimum_width
                                : std::min(width, metrics.preferred_maximum_width);
    visible_rows = sub(divide(geometry.bounds.height, row_height), search_visible ? 1 : 0);
    update_scroll();
}
void Listbox::dpi_changed(float factor, const SliderMetrics&) { // 0x4339E4DF
    scale = factor;
    ListboxLayout layout{geometry,
                         row_height,
                         search_visible,
                         visible_rows,
                         static_cast<i32>(filtered_indices.size()),
                         scroll};
    reflow_listbox(layout, factor);
    geometry = layout.element;
    row_height = layout.row_height;
    visible_rows = layout.visible_rows;
    scroll = layout.scroll;
}
void Listbox::draw(RenderQueue& queue, Point origin, std::uint32_t accent) const { // 0x434179DE
    if (!queue.fonts)
        throw std::logic_error("Listbox rendering requires its font backend");
    const auto text_offset = divide(sub(row_height, font_height(*queue.fonts, 0)), 2);
    Point position{add(origin.x, geometry.bounds.x), add(origin.y, geometry.bounds.y)};
    const auto alpha = enabled ? 0xff000000U : 0x50000000U;
    queue.outlined_rectangle(
        {position.x, position.y, geometry.bounds.width, geometry.bounds.height, alpha | 0x232323},
        0xff0c0c0c, 1);
    auto text_color = [&](std::uint32_t color) {
        return enabled ? color : (color & 0xffffffU) | 0x50000000U;
    };
    if (search_visible) {
        queue.outlined_rectangle({add(position.x, 1), add(position.y, 1),
                                  sub(geometry.bounds.width, 2), row_height, alpha | 0x191919},
                                 alpha | 0x323232, 1);
        queue.rectangle_outline({add(position.x, 2), add(position.y, 2),
                                 sub(geometry.bounds.width, 4), sub(row_height, 2),
                                 alpha | 0x101010},
                                1);
        auto display = search.substr(0, search.find(u'\0'));
        display.push_back(u'_');
        queue.text(add(position.x, 5), add(position.y, text_offset),
                   text_color(search_active ? accent : 0xc8ffffffU), 0, geometry.bounds.width,
                   display);
        position.y = add(position.y, row_height);
    }
    if (options.empty())
        return;
    const auto rows =
        std::min(static_cast<std::uint32_t>(options.size()),
                 static_cast<std::uint32_t>(
                     sub(divide(geometry.bounds.height, row_height), search_visible ? 1 : 0)));
    const auto offset = scroll.enabled ? divide(scroll.position, row_height) : 0;
    auto y = position.y;
    for (std::uint32_t row = 0; row < rows; ++row) {
        const auto filtered = row + std::uint32_t(offset);
        if (filtered >= filtered_indices.size())
            break;
        const auto index = filtered_indices[filtered];
        const auto& option = options.at(index);
        const auto selected = std::uint32_t(selected_item) == index;
        if (selected)
            queue.rectangle({add(position.x, 1), add(y, 1), sub(geometry.bounds.width, 2),
                             sub(row_height, 2), 0xff1a1a1a});
        const auto color =
            option.value < 0 || (value && option.value == *value) ? accent : 0xc8ffffffU;
        queue.text(add(position.x, 10), add(y, text_offset), text_color(color), selected ? 1 : 0,
                   sub(geometry.bounds.width, 20), option.name);
        y = add(y, row_height);
    }
    if (!scroll.enabled)
        return;
    const auto pixels = mul(divide(scroll.position, row_height), row_height);
    if (pixels > 0)
        arrow(queue, {sub(add(position.x, geometry.bounds.width), 20), add(position.y, 8)}, false);
    if (add(visible_rows, pixels) < static_cast<i32>(filtered_indices.size())) {
        auto arrow_y = add(sub(position.y, 9), geometry.bounds.height);
        if (search_visible)
            arrow_y = sub(arrow_y, row_height);
        arrow(queue, {sub(add(position.x, geometry.bounds.width), 20), arrow_y}, true);
    }
    const auto width = round_layout_coordinate(scale * 7.0f);
    queue.rectangle({sub(add(position.x, scroll.viewport.x), add(width, 1)), add(position.y, 1),
                     width, sub(scroll.viewport.y, 2), alpha | 0x282828});
    const auto thumb = scrollbar_thumb(scroll, 7, scale);
    queue.rectangle({add(position.x, thumb.x), add(position.y, thumb.y), thumb.width, thumb.height,
                     alpha | 0x414141});
}
} // namespace skeet::ui
