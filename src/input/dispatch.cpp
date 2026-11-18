#include "skeet/input/runtime.hpp"
#include <bit>
#include <stdexcept>

namespace skeet::input {
bool Runtime::dispatch(ui::WindowMessage& original) { // 0x43405C69
    const auto previous_mouse_buttons = mouse_buttons;
    if (disabled)
        return false;
    auto message = original;
    auto& input = message.input;
    // These messages are filtered out below, but they terminate a resize even
    // when Windows sends no WM_LBUTTONUP to the game window.
    if (input.message == 8 || input.message == 0x1f || input.message == 0x215 ||
        (input.message == 0x1c && !input.parameter))
        for (auto* window = windows; window; window = window->next)
            window->finish_resize();
    switch (input.message) {
    case 0x203:
        input.message = 0x201;
        break;
    case 0x206:
        input.message = 0x204;
        break;
    case 0x209:
        input.message = 0x207;
        break;
    case 0x20d:
        input.message = 0x20b;
        break;
    case 8:
        if (keys[0] & (1u << 18)) {
            input.message = 0x101;
            input.parameter = 18;
            input.key_data = 0x8000;
        }
        break;
    case 0x104:
        input.message = 0x100;
        break;
    case 0x105:
        input.message = 0x101;
        break;
    }
    if (input.message == 0x100) {
        if (!menu_hotkey)
            throw std::logic_error("Input menu hotkey is not bound to its registered setting");
        if (input.parameter == menu_hotkey->binding.packed >> 2)
            toggle_menu();
    }
    if (input.message - 0x200 > 14 &&
        (input.message < 0x100 || (input.message > 0x102 && input.message != 0xa090)))
        return false;
    const auto previous_keys = keys;
    bool consumed = false;
    for (auto* window = windows; window; window = window->next) {
        if (window->dispatch(message)) {
            consumed = true;
            break;
        }
    }
    update_keys(input, consumed);
    const auto newly_pressed_mouse =
        static_cast<std::uint16_t>((mouse_buttons ^ previous_mouse_buttons) & mouse_buttons);
    if (consumed) {
        captured_mouse_buttons = static_cast<std::uint16_t>(
            (newly_pressed_mouse | captured_mouse_buttons) & mouse_buttons);
        return true;
    }
    captured_mouse_buttons =
        static_cast<std::uint16_t>(~newly_pressed_mouse & captured_mouse_buttons & mouse_buttons);
    if (captured_mouse_buttons &&
        (input.message == 0x200 || input.message == 0x201 || input.message == 0x202 ||
         input.message == 0x204 || input.message == 0x205 || input.message == 0x207 ||
         input.message == 0x208 || input.message == 0x20b || input.message == 0x20c))
        original.input.parameter &= ~std::uint32_t(captured_mouse_buttons);
    if (input.message == 0x100 && is_typing_key(input.parameter) && interfaces.in_game() &&
        (interfaces.chat_active() || interfaces.console_visible()))
        return false;
    if (input.message != 0x100 && input.message != 0x201 && input.message != 0x204 &&
        input.message != 0x207 && input.message != 0x20b)
        return false;
    auto current_keys = keys;
    constexpr std::uint8_t mouse_keys[]{1, 2, 16, 17, 4, 5, 6};
    for (std::uint32_t i = 0; i < std::size(mouse_keys); ++i)
        if (captured_mouse_buttons & (1u << i))
            current_keys[mouse_keys[i] >> 5] &= ~(1u << (mouse_keys[i] & 31));
    for (std::uint32_t word = 0; word < current_keys.size(); ++word) {
        auto pressed = (previous_keys[word] ^ current_keys[word]) & current_keys[word];
        while (pressed) {
            const auto bit = static_cast<std::uint32_t>(std::countr_zero(pressed));
            const auto key = word * 32 + bit;
            if (key != 0 && key != 27 &&
                ((key != 1 && key != 2) || !(main_menu->input.mouse_flags & 1))) {
                if (hotkey_registry) {
                    for (auto* link = hotkey_registry->next; link && link != hotkey_registry;
                         link = link->next) {
                        auto* hotkey = static_cast<RegisteredHotkey*>(link);
                        if (hotkey->binding.packed >> 2 != key)
                            continue;
                        hotkey->binding.packed ^= 2;
                        if (hotkey->callback_object)
                            hotkey->callback(hotkey->callback_object, *hotkey);
                    }
                }
            }
            pressed &= ~(1u << bit);
        }
    }
    return false;
}
}
