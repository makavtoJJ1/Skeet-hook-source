#include "skeet/esp/weapon_text.hpp"
#include "detail.hpp"
#include "skeet/text_metrics.hpp"
#include "skeet/engine/interface_call.hpp"
#include <array>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#define NOMINMAX
#include <windows.h>

namespace skeet::esp {
using namespace detail;

WeaponTextProvider weapon_text_provider(WeaponTextBindings& bindings) noexcept {
    return {&bindings,
            [](void* context, std::int32_t definition) -> const char* {
                auto& engine = *static_cast<WeaponTextBindings*>(context);
                if (!engine.item_system && engine.item_system_factory)
                    engine.item_system = engine.item_system_factory();
                if (!engine.item_system)
                    return nullptr;
                const auto schema = skeet::engine::invoke<void*>(engine.item_system, 0);
                if (!schema)
                    return nullptr;
                const auto item = skeet::engine::invoke<void*>(schema, 4, definition);
                if (!item)
                    return nullptr;
                return skeet::engine::invoke<const char*>(item, 2);
            },
            [](void* context, const char* token) -> const char16_t* {
                auto& engine = *static_cast<WeaponTextBindings*>(context);
                return skeet::engine::invoke<const char16_t*>(engine.localization, 11, token);
            }};
}

void draw_weapon_text(ui::RenderQueue& queue, WeaponTextProvider provider, std::int32_t definition,
                      Point position, std::uint32_t color, Point* measured_size) {
    if (definition == 0)
        return;
    const auto* token = provider.schema_name(provider.context, definition);
    if (!token)
        return;
    const auto* localized = provider.localize(provider.context, token);
    std::array<wchar_t, 64> wide{};
    static_assert(sizeof(wchar_t) == sizeof(char16_t));
    if (localized) {
        const auto length = std::char_traits<char16_t>::length(localized);
        if (length >= wide.size())
            throw std::length_error("Weapon localization exceeds native label storage");
        // Unicode casing must not depend on the process's CRT locale.
        if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
                           reinterpret_cast<const wchar_t*>(localized), int(length + 1),
                           wide.data(), int(wide.size()), nullptr, nullptr, 0))
            throw std::runtime_error("Weapon localization uppercase conversion failed");
        queue.text(sub(position.x, 1), position.y, color, 0x14, 0,
                   {reinterpret_cast<const char16_t*>(wide.data()), std::wcslen(wide.data())});
        if (measured_size)
            *measured_size = ui::measure_text(
                queue, {reinterpret_cast<const char16_t*>(wide.data()), std::wcslen(wide.data())},
                4);
    } else {
        const auto length = std::strlen(token);
        if (length < 13 || length - 13 >= 64)
            throw std::length_error("Weapon schema token violates native label storage contract");
        std::array<char, 64> narrow{};
        std::copy_n(token + 13, length - 13, narrow.begin());
        _strupr_s(narrow.data(), narrow.size());
        const auto count = _snwprintf(wide.data(), wide.size(), L"%hs", narrow.data());
        if (count > 0)
            queue.text(sub(position.x, 1), position.y, color, 0x14, 0,
                       {reinterpret_cast<const char16_t*>(wide.data()), std::size_t(count)});
        if (measured_size) { // 0x433B578E -> UTF-8 conversion 0x43396CB3
            wchar_t measured[1024]{};
            const auto converted =
                MultiByteToWideChar(CP_UTF8, 0, narrow.data(), -1, measured, 1024);
            *measured_size = ui::measure_text(queue,
                                              {reinterpret_cast<const char16_t*>(measured),
                                               converted > 0 ? std::size_t(converted - 1) : 0},
                                              4);
        }
    }
}

} // namespace skeet::esp
