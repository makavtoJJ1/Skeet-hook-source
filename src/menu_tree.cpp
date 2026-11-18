#include "skeet/menu_tree.hpp"
#include <algorithm>
#include <bit>
#include <immintrin.h>
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
i32 magnitude(i32 a) noexcept {
    return a < 0 ? sub(0, a) : a;
}
std::int8_t narrow(i32 a) noexcept {
    return std::bit_cast<std::int8_t>(std::uint8_t(a));
}
void merge_input(std::span<Widget*> left, std::span<Widget*> right, Widget** output) { // 0x433E8B57
    std::size_t l = 0, r = 0;
    while (l < left.size() && r < right.size()) {
        const auto right_priority = right[r]->input_priority();
        const auto left_priority = left[l]->input_priority(); // 0x4341F30E
        *output++ = right_priority > left_priority ? right[r++] : left[l++];
    }
    while (l < left.size())
        *output++ = left[l++];
    while (r < right.size())
        *output++ = right[r++];
}
void sort_input(std::span<Widget*> input, std::span<Widget*> scratch) { // 0x433ED024
    if (input.size() <= 1)
        return;
    const auto middle = input.size() / 2;
    auto prepare = [&](std::span<Widget*> part, std::span<Widget*> temporary) {
        if (part.size() == 1) {
            temporary[0] = part[0];
            return;
        }
        const auto half = part.size() / 2;
        sort_input(part.first(half), temporary.first(half));
        sort_input(part.subspan(half), temporary.subspan(half));
        merge_input(part.first(half), part.subspan(half), temporary.data());
    };
    prepare(input.first(middle), scratch.first(middle));
    prepare(input.subspan(middle), scratch.subspan(middle));
    merge_input(scratch.first(middle), scratch.subspan(middle), input.data());
}
MenuInput& menu_state(Widget& widget) {
    auto* state = widget.find_menu();
    if (!state)
        throw std::logic_error("Category input requires its menu context");
    return *state;
}
}
Category::Category(Widget* owner, std::u16string_view label, GridPosition position,
                   std::size_t reserve_elements, bool allow_resize,
                   WidgetIdentities& identities) // 0x4348210C / 0x4348E543
    : Widget(owner, label, 7, false, identities), column(position.column),
      column_span(position.column_span), row(position.row), row_span(position.row_span),
      resizable(allow_resize), movable(position.row_span != -1) {
    geometry.horizontal_offset = 0;
    text_flags = 9;
    if (reserve_elements)
        elements.reserve(reserve_elements);
}
void Category::reset_children() { // 0x433E4282
    drag_state = 0;
    for (auto* child : std::span(elements))
        child->reset_input();
}
void Category::reset_input() { // 0x433CDFD7
    background = 0xff171717;
    reset_children();
}
bool Category::keyboard(const InputMessage& message) { // 0x433FBE32
    for (auto* child : std::span(elements))
        if (child->enabled && child->visible && child->keyboard(message))
            return true;
    return false;
}
void Category::config_loaded() { // 0x43402B5C
    for (auto*& child : std::span(elements)) {
        const auto original_kind = child->kind;
        if (original_kind == 2)
            continue;
        child->config_loaded();
        if (original_kind != 9)
            child->value_changed();
    }
}
std::vector<Widget*> Category::input_order() const { // 0x433C8238
    const auto count = std::min(elements.size(), std::size_t(1024));
    std::vector<Widget*> ordered(elements.begin(), elements.begin() + count);
    std::vector<Widget*> temporary(count);
    sort_input(ordered, temporary);
    return ordered;
}
void Category::reflow() { // 0x433AD23F, typed owning-container entry
    const auto initial_height = geometry.bounds.height;
    auto next_row = padding, inline_row = padding;
    for (auto* child : std::span(elements)) {
        if (!child->visible)
            continue;
        auto& bounds = child->geometry.bounds;
        if (!child->inline_element) {
            bounds.x = add(child->geometry.horizontal_offset, padding);
            bounds.y = next_row;
            if (bounds.height > 0) {
                inline_row = next_row;
                next_row = add(add(next_row, 5), bounds.height);
            }
        } else if (child->kind == 12 || child->kind == 9) {
            bounds.x = sub(
                add(sub(child->geometry.horizontal_offset, bounds.width), geometry.bounds.width),
                add(padding, padding));
            bounds.y = add(inline_row, 1);
            inline_row = add(add(inline_row, 5), bounds.height);
        } else {
            bounds.x = add(child->geometry.horizontal_offset, padding);
            bounds.y = inline_row;
        }
        child->on_layout();
    }
    if (elements.size() < 2 || next_row <= sub(initial_height, 5)) {
        scroll.position = 0;
        scroll.enabled = false;
    } else {
        scroll.enabled = true;
        scroll.viewport = {geometry.bounds.width, geometry.bounds.height};
        scroll.content_height = add(padding, next_row);
        move_scroll_position(scroll.position, scroll.viewport.y, scroll.content_height, 0);
    }
    // Native dependencies and Lua can change visibility several times before
    // a frame is drawn. Cancel clipped input against the presented layout in
    // draw(), not against these temporary positions.
}
void Category::visibility_changed() { // 0x433D08F1
    if (!visible)
        reset_input();
    reflow();
}
void Category::resize(const SliderMetrics& metrics) { // 0x4341B3A1
    for (auto* child : std::span(elements))
        child->resize(metrics);
}
void Category::dpi_changed(float scale, const SliderMetrics& metrics) { // 0x43388E3F
    resize_element(geometry, scale);
    padding = round_layout_coordinate(scale * 20.0f);
    margin = round_layout_coordinate(scale * 10.0f);
    fixed_height = round_layout_coordinate(float(default_fixed_height) * scale);
    for (auto* child : std::span(elements))
        child->dpi_changed(scale, metrics);
    reflow();
}
bool Category::move(Point cursor) { // 0x4334C2C3
    if (!movable)
        return false;
    auto& tab = *static_cast<Tab*>(parent);
    if (tab.focused_widget() != this)
        return false;
    const auto dx = sub(cursor.x, drag_origin.x), dy = sub(cursor.y, drag_origin.y);
    auto next_column = column, next_row = row;
    if (magnitude(dx) >= sub(tab.menu_geometry.size.x, 105) / 24) {
        next_column = narrow(column + (dx > 0 ? 1 : -1));
        if (next_column < 0)
            next_column = 0;
        else if (next_column > narrow(24 - column_span))
            next_column = narrow(24 - column_span);
    }
    if (magnitude(dy) >= sub(tab.menu_geometry.size.y, 35) / 24) {
        next_row = narrow(row + (dy > 0 ? 1 : -1));
        if (next_row < 0)
            next_row = 0;
        else if (next_row > narrow(24 - row_span))
            next_row = narrow(24 - row_span);
    }
    if (next_column != column || next_row != row) {
        Category* collided = nullptr;
        for (auto* other : std::span(tab.categories)) {
            if (other == this)
                continue;
            if (std::max(i32(next_column), i32(other->column)) <
                    std::min(next_column + column_span, other->column + other->column_span) &&
                std::max(i32(next_row), i32(other->row)) <
                    std::min(next_row + row_span, other->row + other->row_span)) {
                collided = other;
                break;
            }
        }
        if (collided) {
            std::swap(column, collided->column);
            std::swap(column_span, collided->column_span);
            std::swap(row, collided->row);
            std::swap(row_span, collided->row_span);
        } else {
            column = next_column;
            row = next_row;
        }
        drag_origin = cursor;
        tab.layout_categories();
        tab.reflow_categories();
    }
    return true;
}
bool Category::change_size(Point cursor) { // 0x4341D248
    if (!resizable)
        return false;
    auto& tab = *static_cast<Tab*>(parent);
    if (tab.focused_widget() != this)
        return false;
    const auto dx = sub(cursor.x, drag_origin.x), dy = sub(cursor.y, drag_origin.y);
    bool changed = false;
    if (magnitude(dx) >= sub(tab.menu_geometry.size.x, 105) / 24) {
        auto next = narrow(column_span + (dx > 0 ? 1 : -1));
        if (next < 3)
            next = 3;
        else if (next > narrow(24 - column))
            next = narrow(24 - column);
        if (next != column_span) {
            column_span = next;
            drag_origin.x = cursor.x;
            changed = true;
        }
    }
    if (magnitude(dy) >= sub(tab.menu_geometry.size.y, 35) / 24) {
        auto next = narrow(row_span + (dy > 0 ? 1 : -1));
        if (next < 2)
            next = 2;
        else if (next > narrow(24 - row))
            next = narrow(24 - row);
        if (next != row_span) {
            row_span = next;
            drag_origin.y = cursor.y;
            changed = true;
        }
    }
    if (changed)
        tab.resize(menu_state(*this).slider_metrics);
    return true;
}
bool Category::mouse(const InputMessage& message, Point cursor) { // 0x4333F951
    if (elements.empty())
        return false;
    const Point local{sub(cursor.x, geometry.bounds.x), sub(cursor.y, geometry.bounds.y)};
    Point content = local;
    bool outside = false;
    if (scroll.enabled) {
        outside = local.x < 5 || local.x > sub(geometry.bounds.width, 10) || local.y < 20 ||
                  local.y > sub(geometry.bounds.height, 10);
        if (scroll.position)
            content.y = add(content.y, scroll.position);
    }
    if (message.message == 0x202)
        drag_state = 0;
    for (auto* child : input_order()) {
        if (child->enabled && child->visible && (!outside || child->input_priority() > 0) &&
            child->mouse(message, content)) {
            child->request_focus();
            return true;
        }
    }
    auto& state = menu_state(*this);
    if (message.message == 0x201 && hit_test(cursor)) {
        drag_origin = cursor;
        if (!state.lock_category_layout && resizable) {
            const auto threshold = state.scale * 15.0f;
            const auto pixels = _mm_cvttss_si32(_mm_set_ss(threshold));
            if (local.x > sub(geometry.bounds.width, pixels) &&
                local.y > sub(geometry.bounds.height, pixels))
                drag_state = 2;
            else if (threshold > float(local.y))
                drag_state = 1;
            else
                goto scroll_input;
            request_focus();
            return true;
        }
    } else if (message.message == 0x200 && !state.lock_category_layout && resizable) {
        if (drag_state == 1 && move(cursor))
            return true;
        if (drag_state == 2 && change_size(cursor))
            return true;
    }
scroll_input:
    if (!scroll.enabled)
        return false;
    if (!scrollbar_mouse(scroll, message.message, local, state.scale)) {
        if (message.message != 0x20a || !hit_test(cursor))
            return false;
        auto delta = round_layout_coordinate(state.scale * 20.0f);
        if (std::bit_cast<std::int16_t>(std::uint16_t(message.parameter >> 16)) > 0)
            delta = sub(0, delta);
        move_scroll_position(scroll.position, scroll.viewport.y, scroll.content_height, delta);
    }
    for (auto* child : std::span(elements))
        child->reset_input();
    return true;
}
Widget* Tab::focused_widget() noexcept { // 0x4336EAD1
    if (focus.identity == UINT32_MAX)
        return nullptr;
    if (focus.category == UINT32_MAX)
        return identity == focus.identity ? this : nullptr;
    if (focus.category >= categories.size())
        return nullptr;
    auto* category = categories[focus.category];
    Widget* candidate = category;
    if (focus.element != UINT32_MAX) {
        if (focus.element >= category->elements.size())
            return nullptr;
        candidate = category->elements[focus.element];
    }
    return candidate->identity == focus.identity ? candidate : nullptr;
}
Tab::Tab(Menu& menu, MenuInput& input, MenuGeometry& layout, std::u16string_view label,
         Point& next_position, WidgetIdentities& identities) // 0x4348E41D
    : Widget(nullptr, label, 1, false, identities), navigation(&menu), menu_geometry(layout) {
    menu_input = &input;
    geometry.horizontal_offset = 0;
    geometry.bounds = {next_position.x, next_position.y, 75, 64};
    geometry.default_size = {75, 64};
    next_position.y = add(next_position.y, 64);
}
FocusPath Tab::path_to(const Widget* target) const noexcept { // 0x4332AA08
    FocusPath path;
    if (!target)
        return path;
    if (target == this) {
        path.identity = target->identity;
        return path;
    }
    for (std::uint32_t category_index = 0; category_index < categories.size(); ++category_index) {
        const auto* category = categories[category_index];
        if (target == category)
            return {category_index, UINT32_MAX, target->identity};
        for (std::uint32_t index = 0; index < category->elements.size(); ++index)
            if (category->elements[index] == target)
                return {category_index, index, target->identity};
    }
    return path;
}
void Tab::focus_widget(Widget* target) { // 0x433B1444
    auto* previous = focused_widget();
    if (!target || previous != target) {
        if (previous)
            previous->reset_input();
        focus = path_to(target);
    }
}
void Tab::reset_input() { // 0x43340044
    if (auto* previous = focused_widget(); previous && previous->kind == 10)
        focus_widget(nullptr);
    hovered = false;
    for (auto* category : std::span(categories))
        category->reset_input();
}
bool Tab::keyboard(const InputMessage& message) { // 0x433E9E96
    for (auto* category : std::span(categories))
        if (category->keyboard(message))
            return true;
    return false;
}
void Tab::config_loaded() { // 0x4334C8AC
    for (auto* category : std::span(categories))
        category->config_loaded();
}
bool Tab::mouse(const InputMessage& message, Point cursor) { // 0x433ECEA1
    if (message.message == 0x201 || message.message == 0x200) {
        if (hit_test(cursor)) {
            if (message.message == 0x201) {
                if (!navigation)
                    throw std::logic_error("Tab selection requires its menu");
                select_tab(*navigation, identity);
                return true;
            }
            hovered = true;
            return false;
        }
        hovered = false;
    }
    if (!navigation)
        throw std::logic_error("Tab input requires its menu");
    if (navigation->active_tab != identity)
        return false;
    auto* focused_category = owning_category(focused_widget());
    if (focused_category && focused_category->visible && focused_category->mouse(message, cursor))
        return true;
    for (auto* category : std::span(categories))
        if (category != focused_category && category->visible && category->mouse(message, cursor))
            return true;
    return false;
}
void Tab::layout_categories() { // 0x43384477, owning-container entry
    std::vector<CategoryGeometry> placement(categories.size());
    std::vector<CategoryGeometry*> pointers(categories.size());
    for (std::size_t i = 0; i < categories.size(); ++i) {
        const auto& c = *categories[i];
        placement[i] = {c.geometry,    c.fixed_height, c.margin,  c.column,
                        c.column_span, c.row,          c.row_span};
        pointers[i] = &placement[i];
    }
    layout_category_grid(menu_geometry, {geometry.bounds.width, padding, pointers});
    for (std::size_t i = 0; i < categories.size(); ++i)
        categories[i]->geometry = placement[i].element;
}
void Tab::reflow_categories() { // 0x433CF58B
    for (auto* category : std::span(categories))
        category->reflow();
}
void Tab::resize(const SliderMetrics& metrics) { // 0x433CCAE7
    layout_categories();
    reflow_categories();
    for (auto* category : std::span(categories))
        category->resize(metrics);
}
void Tab::dpi_changed(float scale, const SliderMetrics& metrics) { // 0x433817D6
    padding = round_layout_coordinate(scale * 10.0f);
    resize_element(geometry, scale);
    for (auto* category : std::span(categories))
        category->dpi_changed(scale, metrics);
}
void Tab::draw(RenderQueue& queue, Point origin, std::uint32_t accent) const { // 0x433AC635
    if (!navigation)
        throw std::logic_error("Tab rendering requires its menu");
    const bool selected = navigation->active_tab == identity;
    const auto tint = selected ? 0xffffffffU : hovered ? 0xc8ffffffU : 0x64ffffffU;
    const auto& bounds = geometry.bounds;
    const Point position{add(origin.x, bounds.x), add(origin.y, bounds.y)};
    if (icon == UINT32_MAX) {
        if (!name.empty() && add(bounds.y, bounds.height) < menu_geometry.size.y)
            queue.text(add(position.x, bounds.width / 2), add(position.y, bounds.height / 2), tint,
                       0x12, 0, std::u16string_view(name).substr(0, 64));
    } else {
        queue.texture({add(position.x, sub(bounds.width, icon_source.width) / 2),
                       add(position.y, sub(bounds.height, icon_source.height) / 2), icon, tint,
                       icon_source.x, icon_source.y, icon_source.width, icon_source.height, 0,
                       1.0f});
    }
    if (selected) {
        auto current = categories.end();
        while (current != categories.begin()) {
            auto* category = *--current;
            if (category->visible)
                category->draw(queue, origin, accent);
        }
    }
}
Category* owning_category(Widget* widget) noexcept { // 0x43364799
    while (widget && widget->kind != 7)
        widget = widget->parent;
    return static_cast<Category*>(widget);
}
Tab* owning_tab(Widget* widget) noexcept { // 0x43330233
    while (widget && widget->kind != 1)
        widget = widget->parent;
    return static_cast<Tab*>(widget);
}
void Widget::request_focus() { // 0x4337ED94
    if (focusable && visible)
        if (auto* tab = owning_tab(this))
            tab->focus_widget(this);
}
} // namespace skeet::ui
