#include "skeet/menu_window.hpp"
#include "skeet/menu/window_events.hpp"
#include "skeet/popup_menu.hpp"
#include <algorithm>
#include <bit>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace skeet::ui {
namespace {
using i32 = std::int32_t;
i32 add(i32 a, i32 b) noexcept {
    return std::bit_cast<i32>(std::uint32_t(a) + std::uint32_t(b));
}
i32 sub(i32 a, i32 b) noexcept {
    return std::bit_cast<i32>(std::uint32_t(a) - std::uint32_t(b));
}
Point packed_cursor(std::uint32_t value) noexcept {
    return {std::bit_cast<std::int16_t>(std::uint16_t(value)),
            std::bit_cast<std::int16_t>(std::uint16_t(value >> 16))};
}
bool inside(const MenuWindow& window) noexcept {
    return contains(
        {window.position.x, window.position.y, window.geometry.size.x, window.geometry.size.y},
        window.cursor);
}
Tab& active(MenuWindow& window) {
    return *window.tabs.at(window.navigation.active_tab);
}
void clear_text_focus(MenuWindow& window) { // 0x4332A762
    auto& tab = active(window);
    auto* focused = tab.focused_widget();
    if (focused && (focused->kind == 10 || focused->kind == 6))
        tab.focus_widget(nullptr);
}
void modifier(MenuWindow& window, std::uint32_t key, bool down) noexcept {
    std::uint32_t mask = 0;
    if (key == 0x10 || key == 0xa0 || key == 0xa1)
        mask = 4;
    else if (key == 0x11 || key == 0xa2 || key == 0xa3)
        mask = 8;
    if (down)
        window.button_flags |= mask;
    else
        window.button_flags &= ~mask;
}
bool tab_context_input(MenuWindow& window, const InputMessage& message, Point cursor) {
    auto* popup = window.tab_context_menu;
    if (!popup)
        return false;
    if (popup->open) {
        if (popup->mouse(message, cursor))
            return true;
        if (message.message == WM_LBUTTONDOWN || message.message == WM_RBUTTONUP)
            popup->reset_input();
        else if (popup->hit_test(cursor))
            return true;
    }
    if (message.message != WM_RBUTTONUP || !inside(window))
        return false;
    for (auto* tab : window.tabs) {
        if (!tab->visible || !tab->hit_test(cursor))
            continue;
        active(window).reset_input();
        popup->parent = tab;
        auto& rect = popup->geometry.bounds;
        rect.x = std::clamp(cursor.x, 0, std::max(0, window.geometry.size.x - rect.width)) -
                 tab->geometry.bounds.x;
        rect.y = std::clamp(cursor.y, 0, std::max(0, window.geometry.size.y - rect.height)) -
                 tab->geometry.bounds.y;
        popup->open = true;
        return true;
    }
    return false;
}
}
bool window_client_bounds(void*, std::uintptr_t window, WindowClientBounds& bounds) {
    RECT native{};
    if (IsIconic(reinterpret_cast<HWND>(window)) ||
        !GetClientRect(reinterpret_cast<HWND>(window), &native))
        return false;
    bounds = {native.left, native.top, native.right, native.bottom};
    return true;
}
bool window_screen_to_client(void*, std::uintptr_t window, Point& cursor) {
    POINT native{cursor.x, cursor.y};
    if (!ScreenToClient(reinterpret_cast<HWND>(window), &native))
        return false;
    cursor = {native.x, native.y};
    return true;
}
bool route_window_mouse(MenuWindow& window, WindowMessage& message) { // 0x4334EAE8
    const Point relative{sub(window.cursor.x, window.position.x),
                         sub(window.cursor.y, window.position.y)};
    if (tab_context_input(window, message.input, relative))
        return true;
    bool handled = false;
    const auto tabs = std::span(window.tabs);
    for (auto i = tabs.size(); i != 0;) {
        auto* tab = tabs[--i];
        if (tab->visible && tab->mouse(message.input, relative))
            handled = true;
    }
    return handled;
}
bool decode_window_mouse(MenuWindow& window, WindowMessage& message) { // 0x43398693
    window.cursor = packed_cursor(message.input.key_data);
    return route_window_mouse(window, message);
}
void select_window_tab(MenuWindow& window, std::uint32_t requested) { // 0x433B75D3
    if (requested == window.navigation.active_tab || requested >= window.tabs.size())
        return;
    if (window.tab_context_menu)
        window.tab_context_menu->reset_input();
    active(window).reset_input();
    window.navigation.active_tab = requested;
    active(window).activate();
}
void previous_window_tab(MenuWindow& window) { // 0x433FF5EE
    const auto tabs = std::span(window.tabs);
    auto selected = window.navigation.active_tab;
    for (std::size_t tried = 0; tried < tabs.size(); ++tried) {
        selected = selected ? selected - 1 : std::uint32_t(tabs.size() - 1);
        if (tabs[selected]->visible) {
            select_window_tab(window, selected);
            return;
        }
    }
}
void next_window_focus(MenuWindow& window) { // 0x433CBE38
    auto& tab = active(window);
    auto* focused = tab.focused_widget();
    bool after = focused == nullptr;
    for (auto* category : std::span(tab.categories))
        for (auto* child : std::span(category->elements)) {
            if (after && child->focusable && (child->kind == 10 || child->kind == 6)) {
                child->request_focus();
                return;
            }
            if (child == focused)
                after = true;
        }
    if (!focused)
        return;
    for (auto* category : std::span(tab.categories))
        for (auto* child : std::span(category->elements)) {
            if (child == focused)
                return;
            if (child->focusable && (child->kind == 10 || child->kind == 6)) {
                child->request_focus();
                return;
            }
        }
}
bool window_key_down(MenuWindow& window, WindowMessage& message) { // 0x433B6EEC
    const auto key = message.input.parameter;
    if (key == VK_ESCAPE && window.tab_context_menu && window.tab_context_menu->open) {
        window.tab_context_menu->reset_input();
        return true;
    }
    modifier(window, key, true);
    if (key == 9) {
        if (window.button_flags & 8) {
            if (window.button_flags & 4)
                previous_window_tab(window);
            else {
                const auto tabs = std::span(window.tabs);
                auto selected = window.navigation.active_tab;
                for (std::size_t tried = 0; tried < tabs.size(); ++tried) {
                    selected = std::uint32_t(selected + 1) % std::uint32_t(tabs.size());
                    if (tabs[selected]->visible) {
                        select_window_tab(window, selected);
                        break;
                    }
                }
            }
            return true;
        }
        next_window_focus(window);
    }
    return window_character(window, message);
}
bool window_key_up(MenuWindow& window, WindowMessage& message) { // 0x433CF538
    modifier(window, message.input.parameter, false);
    return false;
}
bool window_character(MenuWindow& window, WindowMessage& message) { // 0x433EB7BA
    if (auto* focused = active(window).focused_widget())
        return focused->keyboard(message.input);
    return false;
}
bool window_left_down(MenuWindow& window, WindowMessage& message) { // 0x43327C83
    window.button_flags |= 1;
    if (decode_window_mouse(window, message))
        return true;
    if (!inside(window)) {
        clear_text_focus(window);
        return false;
    }
    for (auto* tab : std::span(window.tabs))
        tab->reset_input();
    clear_text_focus(window);
    const auto grip = round_layout_coordinate(window.input.scale * 15.0f);
    const bool resize =
        !window.event_state->window_size_locked && !(window.input.mouse_flags & 0x10) &&
        sub(window.cursor.x, window.position.x) > sub(window.geometry.size.x, grip) &&
        sub(window.cursor.y, window.position.y) > sub(window.geometry.size.y, grip);
    window.input.mouse_flags |= resize ? 8 : 4;
    window.drag_anchor = window.cursor;
    if (resize)
        window.resize_start_size = window.geometry.size;
    return true;
}
bool window_left_up(MenuWindow& window, WindowMessage& message) { // 0x433EE1F2
    window.finish_resize();
    const auto handled = decode_window_mouse(window, message);
    window.input.mouse_flags &= ~12u;
    window.button_flags &= ~1u;
    if (auto* focused = active(window).focused_widget(); focused && focused->kind == 5)
        static_cast<Slider*>(focused)->dragging = false;
    return handled;
}
bool window_right_down(MenuWindow& window, WindowMessage& message) { // 0x4334C26E
    window.button_flags |= 2;
    return decode_window_mouse(window, message) || inside(window);
}
bool window_right_up(MenuWindow& window, WindowMessage& message) { // 0x433D0B03
    window.button_flags &= ~2u;
    return decode_window_mouse(window, message);
}
bool window_wheel(MenuWindow& window, WindowMessage& message) { // 0x43408192
    window.cursor = packed_cursor(message.input.key_data);
    auto converted = window.cursor;
    if (window.event_services.screen_to_client(window.event_services.context, window.native_window,
                                               converted))
        window.cursor = converted;
    return route_window_mouse(window, message) || inside(window);
}
bool window_mouse_move(MenuWindow& window, WindowMessage& message) { // 0x4339E90B
    const auto handled = decode_window_mouse(window, message);
    if (window.input.mouse_flags & 4) {
        Point next{add(sub(window.position.x, window.drag_anchor.x), window.cursor.x),
                   add(sub(window.position.y, window.drag_anchor.y), window.cursor.y)};
        WindowClientBounds client{};
        if (window.event_services.client_bounds(window.event_services.context, window.native_window,
                                                client)) {
            const Point minimum{window.geometry.size.x / -3, window.geometry.size.y / -3};
            const Point maximum{sub(client.right, window.geometry.size.x / 3),
                                sub(client.bottom, window.geometry.size.y / 3)};
            if (next.x < minimum.x)
                next.x = minimum.x;
            else if (next.x > maximum.x)
                next.x = maximum.x;
            if (next.y < minimum.y)
                next.y = minimum.y;
            else if (next.y > maximum.y)
                next.y = maximum.y;
        }
        if (next.x != window.position.x || next.y != window.position.y) {
            window.move(next);
            window.drag_anchor = window.cursor;
        }
    } else if ((window.input.mouse_flags & 8) && (window.cursor.x != window.drag_anchor.x ||
                                                  window.cursor.y != window.drag_anchor.y)) {
        auto size = window.geometry.size;
        const Point next{add(size.x, sub(window.cursor.x, window.drag_anchor.x)),
                         add(size.y, sub(window.cursor.y, window.drag_anchor.y))};
        const auto minimum = scale_size(window.default_size, window.input.scale);
        if (next.x >= minimum.x) {
            size.x = next.x;
            window.drag_anchor.x = window.cursor.x;
        }
        if (next.y >= minimum.y) {
            size.y = next.y;
            window.drag_anchor.y = window.cursor.y;
        }
        window.resize(size);
    }
    const auto buttons = message.input.parameter & 15;
    const auto changed = window.button_flags ^ buttons;
    if (changed) {
        window.button_flags = buttons;
        if (changed & 1) {
            if (buttons & 1)
                window_left_down(window, message);
            else
                window_left_up(window, message);
        }
        if (changed & 2) {
            if (buttons & 2)
                window_right_down(window, message);
            else
                window_right_up(window, message);
        }
    }
    return handled || inside(window);
}
std::span<const WindowMessageHandler> native_window_handlers() noexcept {
    // Registrations in 0x4348580B..0x43485994; all entries use the native message identity.
    static const std::array<WindowMessageHandler, 9> handlers{{{0x100, window_key_down},
                                                               {0x101, window_key_up},
                                                               {0x102, window_character},
                                                               {0x200, window_mouse_move},
                                                               {0x201, window_left_down},
                                                               {0x202, window_left_up},
                                                               {0x204, window_right_down},
                                                               {0x205, window_right_up},
                                                               {0x20a, window_wheel}}};
    return handlers;
}
}
