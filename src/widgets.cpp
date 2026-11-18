#include "skeet/widgets.hpp"
#include <bit>
#include <cwchar>
#include <immintrin.h>
#include <intrin.h>
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
}

Widget::Widget() {
    context = this;
    layout = [](LayoutElement& element) { static_cast<Widget*>(element.context)->on_layout(); };
    reset = [](LayoutElement& element) { static_cast<Widget*>(element.context)->reset_input(); };
}
Widget::Widget(Widget* owner, std::u16string_view label, std::uint8_t type, bool has_value,
               WidgetIdentities& identities)
    : Widget() { // 0x43383D2B
    parent = owner;
    enabled = has_value;
    kind = type;
    name = label;
    geometry.horizontal_offset = 20;
    geometry.label_height = 14;
    identity = identities.allocate();
}
void Widget::reset_input() {
    hovered = false;
} // 0x433E2E4E
bool Widget::hit_test(Point cursor) const {
    return contains(geometry.bounds, cursor);
}
bool Widget::mouse(const InputMessage&, Point) {
    return false;
} // 0x43382057
bool Widget::keyboard(const InputMessage&) {
    return false;
} // 0x43336939
void Widget::config_loaded() {} // 0x433CCE8E
std::int32_t Widget::input_priority() const {
    return 0;
} // 0x43336942
void Widget::resize(const SliderMetrics&) {} // 0x433CCE8E
void Widget::dpi_changed(float scale, const SliderMetrics&) {
    resize_element(geometry, scale);
}
void Widget::draw(RenderQueue&, Point, std::uint32_t) const {} // 0x4334A269
void Widget::notify_callbacks() { // 0x4334E09F
    const auto pending = std::span(callbacks);
    for (const auto& callback : pending)
        callback.function(callback.context, *this);
}
void Widget::set_visible(bool requested) { // 0x433E000B
    if (visible != requested) {
        visible = requested;
        if (!visible)
            reset_input();
        if (parent)
            parent->visibility_changed();
    }
}
void Widget::visibility_changed() { // 0x4332EF98
    if (!visible)
        reset_input();
}
MenuInput* Widget::find_menu() const noexcept { // 0x4334DC50
    for (auto* current = this; current; current = current->parent)
        if (current->kind == 1)
            return current->menu_input;
    return nullptr;
}
void Widget::value_changed() {
    notify_callbacks();
}
void Widget::draw_label(RenderQueue& queue, Point& cursor, i32 maximum_width) const { // 0x43384344
    if (name.empty() || inline_element)
        return;
    auto text = std::u16string_view(name).substr(0, 256);
    if (const auto newline = text.find(u'\n'); newline != text.npos)
        text = text.substr(0, newline);
    if (text.empty())
        return;
    const auto label_color = enabled ? color : (color & 0x00ffffffU) | 0x46000000U;
    queue.text(cursor.x, cursor.y, label_color, text_flags, maximum_width, text);
    cursor.y = add(cursor.y, geometry.label_height);
}

Label::Label() {
    kind = 8;
}
Label::Label(Widget* owner, std::u16string_view text, WidgetIdentities& identities) // 0x43328C61
    : Widget(owner, text, 8, false, identities) {
    enabled = true;
    text_flags = 0;
    geometry.default_size = {200, 12};
    geometry.bounds.width = 200;
    geometry.bounds.height = 12;
}
void Label::draw(RenderQueue& queue, Point origin, std::uint32_t) const { // 0x433F8EB7
    Point cursor{add(origin.x, geometry.bounds.x), add(origin.y, geometry.bounds.y)};
    draw_label(queue, cursor, 0);
}

Checkbox::Checkbox() {
    kind = 3;
    tied.next = &tied;
    geometry.horizontal_offset = 0;
}
Checkbox::Checkbox(Widget* owner, std::u16string_view label, std::uint8_t* setting,
                   WidgetIdentities& identities) // 0x433ADF35
    : Widget(owner, label, 3, setting != nullptr, identities), value(setting) {
    tied.next = &tied;
    geometry.horizontal_offset = 0;
    geometry.default_size = name.empty() ? Point{17, 0} : Point{150, 13};
    geometry.bounds.width = geometry.default_size.x;
    geometry.bounds.height = geometry.default_size.y;
}
bool Checkbox::hit_test(Point cursor) const { // 0x4339C9E1
    auto bounds = geometry.bounds;
    if (bounds.height == 0)
        bounds.height = 13;
    return contains(bounds, cursor);
}
void Checkbox::set_value(bool checked) { // 0x433AD3C8
    const auto previous = value ? *value : false;
    if (previous != checked) {
        if (value)
            __atomic_exchange_n(value, checked, __ATOMIC_SEQ_CST);
        value_changed();
    }
}
void Checkbox::config_loaded() { // 0x433B927E
    for (auto* link = tied.next; link != &tied; link = link->next)
        link->widget->set_visible(value ? *value : false);
}
void Checkbox::value_changed() { // 0x4334C75F
    notify_callbacks();
    config_loaded();
}
void Checkbox::reset_input() { // 0x4332C42A
    hovered = false;
    held = false;
}
bool Checkbox::mouse(const InputMessage& message, Point cursor) { // 0x4333043A
    if (message.message == 0x200)
        hovered = hit_test(cursor);
    else if (message.message == 0x201) {
        if (hit_test(cursor)) {
            held = true;
            return true;
        }
    } else if (message.message == 0x202 && held) {
        held = false;
        if (hit_test(cursor))
            set_value(!(value && *value));
        return true;
    }
    return false;
}
void Checkbox::dpi_changed(float scale) { // 0x433E085C
    resize_element(geometry, scale);
    box_size = scale_size({8, 8}, scale);
    text_padding = round_layout_coordinate(scale * 20.0f);
}
void Checkbox::draw(RenderQueue& queue, Point origin, std::uint32_t accent) const { // 0x4338902F
    if (!parent)
        throw std::logic_error("Checkbox rendering requires an owning category");
    auto top_color = value && *value ? accent : 0xbb646464U;
    if (!enabled)
        top_color = (top_color & 0x00ffffffU) | 0x46000000U;
    const Point position{add(origin.x, geometry.bounds.x), add(origin.y, geometry.bounds.y)};
    queue.rectangle_outline({position.x, add(position.y, 3), box_size.x, box_size.y, 0xff0c0c0cU},
                            1);
    auto bottom_color = subtract_color(top_color, 0x00282828);
    if (hovered) {
        top_color = add_color(top_color, 0x000a0a0a);
        bottom_color = add_color(bottom_color, 0x000a0a0a);
    }
    queue.vertical_gradient({add(position.x, 1), add(position.y, 4), sub(box_size.x, 2),
                             sub(box_size.y, 2), top_color, bottom_color});
    Point label{add(position.x, text_padding), position.y};
    draw_label(queue, label, sub(sub(parent->geometry.bounds.width, geometry.bounds.x), 28));
}

Slider::Slider() {
    kind = 5;
}
Slider::Slider(Widget* owner, std::u16string_view label, i32 lower, i32 upper, i32* setting,
               bool show, std::uint32_t unit, float multiplier,
               WidgetIdentities& identities) // 0x433CF6B2
    : Widget(owner, label, 5, setting != nullptr, identities), value(setting), show_value(show),
      maximum(upper), minimum(lower), display_scale(multiplier), suffix(unit) {
    geometry.default_size = {158, name.empty() ? 10 : 25};
    geometry.bounds.width = geometry.default_size.x;
    geometry.bounds.height = geometry.default_size.y;
    update_value_text();
}
bool Slider::hit_test(Point cursor) const { // 0x433BA3F2
    const auto& bounds = geometry.bounds;
    return sub(bounds.x, 10) <= cursor.x && cursor.x <= add(add(bounds.width, 10), bounds.x) &&
           sub(bounds.y, 2) <= cursor.y && cursor.y <= add(add(bounds.height, 2), bounds.y);
}
i32 Slider::value_at(Point cursor) const noexcept { // 0x43365AF6
    const auto& bounds = geometry.bounds;
    const auto position =
        (float(sub(maximum, minimum)) / float(bounds.width)) * float(sub(cursor.x, bounds.x)) +
        float(minimum);
    auto result = _mm_cvttss_si32(_mm_set_ss(position));
    if (result < minimum)
        result = minimum;
    else if (maximum < result)
        result = maximum;
    return result;
}
void Slider::set_value(i32 requested) { // 0x43398ECF
    const auto previous = value ? *value : 0;
    if (previous != requested) {
        if (value)
            _InterlockedExchange(reinterpret_cast<volatile long*>(value), long(requested));
        value_changed();
    }
}
void Slider::value_changed() { // 0x433A1670
    auto current = value ? *value : 0;
    if (current < minimum)
        current = minimum;
    else if (maximum < current)
        current = maximum;
    if (value)
        _InterlockedExchange(reinterpret_cast<volatile long*>(value), long(current));
    notify_callbacks();
    update_value_text();
}
void Slider::config_loaded() { // 0x4341B3DD
    if (value) {
        if (*value < minimum)
            set_value(minimum);
        else if (*value > maximum)
            set_value(maximum);
    }
    update_value_text();
}
void Slider::update_value_text() { // 0x4334A31C
    static_assert(sizeof(wchar_t) == 2);
    value_text.fill(0);
    if (!show_value || !value)
        return;
    const auto current = *value;
    if (auto label = value_labels.find(std::uint32_t(current)); label != value_labels.end()) {
        if (label->second.size() >= value_text.size())
            throw std::length_error("Slider value label exceeds the native UTF-16 buffer");
        std::copy(label->second.begin(), label->second.end(), value_text.begin());
        return;
    }
    int length;
    if (display_scale == 1.0f) {
        length = swprintf_s(value_text.data(), value_text.size(), L"%d", current);
    } else if (display_scale > 1.0f) {
        const auto scaled = _mm_cvttss_si32(_mm_set_ss(float(current) * display_scale));
        length = swprintf_s(value_text.data(), value_text.size(), L"%d", scaled);
    } else {
        const auto scaled = float(current) * display_scale;
        length = swprintf_s(value_text.data(), value_text.size(),
                            display_scale == 0.001f ? L"%.3f" : L"%.2f", double(scaled));
        while (length > 1 && value_text[length - 2] != L'.' && value_text[length - 1] == L'0')
            value_text[--length] = 0;
    }
    if (length < 0)
        throw std::runtime_error("Slider number formatting failed");
    if (suffix != 0) {
        if (std::size_t(length) + 2 >= value_text.size())
            throw std::length_error("Slider suffix exceeds the native UTF-16 buffer");
        value_text[length] = wchar_t(suffix & 0xffff);
        value_text[length + 1] = wchar_t(suffix >> 16);
        value_text[length + 2] = 0;
    }
}
bool Slider::mouse(const InputMessage& message, Point cursor) { // 0x433E5250
    if (message.message == 0x200) {
        hovered = hit_test(cursor);
        if (!dragging)
            return false;
    } else if (message.message == 0x201) {
        if (!hit_test(cursor))
            return false;
        if (cursor.x < geometry.bounds.x) {
            set_value(sub(value ? *value : 0, 1));
            return true;
        }
        if (add(geometry.bounds.x, geometry.bounds.width) < cursor.x) {
            set_value(add(value ? *value : 0, 1));
            return true;
        }
        dragging = true;
    } else {
        if (message.message == 0x202)
            dragging = false;
        else if (message.message == 0x20a && hit_test(cursor)) {
            auto* menu = find_menu();
            if (!menu)
                throw std::logic_error("Slider wheel input requires an owning menu");
            if (menu->button_flags & 8) {
                const auto wheel =
                    std::bit_cast<std::int16_t>(std::uint16_t(message.parameter >> 16));
                set_value(add(value ? *value : 0, wheel >= 1 ? 1 : -1));
                return true;
            }
        }
        return false;
    }
    set_value(value_at(cursor));
    return true;
}
bool Slider::keyboard(const InputMessage& message) { // 0x4336BC61
    if (message.message != 0x100 || !value)
        return false;
    const auto* menu = find_menu();
    const auto step = menu && (menu->button_flags & 4) ? 5 : 1;
    if (message.parameter == 0x25)
        *value = sub(*value, step);
    else if (message.parameter == 0x27)
        *value = add(*value, step);
    else
        return false;
    value_changed();
    return true;
}
void Slider::reset_input() { // 0x433689A4
    hovered = false;
    dragging = false;
}
void Slider::resize(const SliderMetrics& metrics) { // 0x433F0A8F
    if (!parent)
        throw std::logic_error("Slider resizing requires an owning category");
    auto width = sub(parent->geometry.bounds.width, metrics.category_padding);
    if (width < metrics.minimum_width)
        width = metrics.minimum_width;
    if (maximum_width != 0 && maximum_width < width)
        width = maximum_width;
    geometry.bounds.width = width;
}
void Slider::dpi_changed(float scale, const SliderMetrics& metrics) { // 0x4340BFC4
    resize_element(geometry, scale);
    track_size = scale_size({158, 6}, scale);
    if (maximum_width != 0)
        maximum_width = metrics.preferred_maximum_width;
}
void Slider::draw_value(RenderQueue& queue, i32 x, i32 y, i32 fill_width, i32) const { // 0x43416FBD
    if (!show_value || value_text[0] == 0)
        return;
    const auto text = std::bit_cast<std::array<char16_t, 64>>(value_text);
    const auto length = std::char_traits<char16_t>::length(text.data());
    queue.text(add(x, fill_width), add(y, track_size.y), enabled ? 0xbbffffffU : 0x50ffffffU, 0x31,
               geometry.bounds.width, {text.data(), length});
}
void Slider::draw(RenderQueue& queue, Point origin, std::uint32_t accent) const { // 0x43383A3C
    if (!parent)
        throw std::logic_error("Slider rendering requires an owning category");
    Point position{add(origin.x, geometry.bounds.x), add(origin.y, geometry.bounds.y)};
    const auto current = value ? *value : 0;
    draw_label(queue, position, sub(sub(parent->geometry.bounds.width, geometry.bounds.x), 8));
    const auto top = add(position.y, 2);
    const auto range = sub(maximum, minimum);
    const auto magnitude = range < 0 ? sub(0, range) : range;
    if (geometry.bounds.width < magnitude) {
        if (current != minimum)
            queue.rectangle({sub(position.x, 5), add(top, 3), 3, 1, 0xff646464U});
        if (current != maximum) {
            queue.rectangle(
                {add(add(position.x, geometry.bounds.width), 4), add(top, 3), 3, 1, 0xff646464U});
            queue.rectangle(
                {add(add(position.x, geometry.bounds.width), 5), add(top, 2), 1, 3, 0xff646464U});
        }
    }
    queue.rectangle_outline(
        {position.x, top, geometry.bounds.width, add(track_size.y, 1), 0xff0c0c0cU}, 1);
    const auto upper = !enabled ? 0xff141414U : hovered ? 0xff3c3c3cU : 0xff323232U;
    const auto lower = !enabled ? 0xff282828U : hovered ? 0xff505050U : 0xff464646U;
    const auto inner_x = add(position.x, 1), inner_y = add(top, 1), height = sub(track_size.y, 1);
    queue.vertical_gradient(
        {inner_x, inner_y, sub(geometry.bounds.width, 2), height, upper, lower});
    const auto proportion =
        (1.0f / (float(range) / float(geometry.bounds.width))) * float(sub(current, minimum));
    const auto fill_width = _mm_cvttss_si32(_mm_set_ss(proportion));
    if (!enabled)
        accent = (accent & 0x00ffffffU) | (((accent >> 24) / 3) << 24);
    queue.vertical_gradient(
        {inner_x, inner_y, fill_width, height, accent, subtract_color(accent, 0x003c3c3c)});
    draw_value(queue, position.x, top, fill_width, height);
}

} // namespace skeet::ui
