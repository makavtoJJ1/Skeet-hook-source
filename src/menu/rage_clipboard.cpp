#include "skeet/menu/rage_clipboard.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstring>

namespace skeet::ui {
namespace {
bool open(void*, std::uintptr_t owner) {
    return OpenClipboard(reinterpret_cast<HWND>(owner)) != FALSE;
}
bool empty(void*) {
    return EmptyClipboard() != FALSE;
}
std::uintptr_t allocate(void*, std::uint32_t flags, std::size_t size) {
    return reinterpret_cast<std::uintptr_t>(GlobalAlloc(flags, size));
}
std::uintptr_t set_data(void*, std::uint32_t format, std::uintptr_t memory) {
    return reinterpret_cast<std::uintptr_t>(
        SetClipboardData(format, reinterpret_cast<HANDLE>(memory)));
}
void free_memory(void*, std::uintptr_t memory) {
    GlobalFree(reinterpret_cast<HGLOBAL>(memory));
}
void close(void*) {
    CloseClipboard();
}
bool available(void*, std::uint32_t format) {
    return IsClipboardFormatAvailable(format) != FALSE;
}
std::uintptr_t get_data(void*, std::uint32_t format) {
    return reinterpret_cast<std::uintptr_t>(GetClipboardData(format));
}
const char* wire(void*, std::uintptr_t memory) {
    return static_cast<const char*>(GlobalWire(reinterpret_cast<HGLOBAL>(memory)));
}
std::size_t size(void*, std::uintptr_t memory) {
    return GlobalSize(reinterpret_cast<HGLOBAL>(memory));
}
void unlock(void*, std::uintptr_t memory) {
    GlobalUnlock(reinterpret_cast<HGLOBAL>(memory));
}

void switch_group(void* raw, bool force) {
    auto& context = *static_cast<RageWeaponClipboardContext*>(raw);
    if (force)
        context.state.last_group = UINT32_MAX;
    switch_rage_weapon_group(context.state, context.selected_group);
}
void copy_group(void* raw, std::uint32_t group) {
    auto& context = *static_cast<RageWeaponClipboardContext*>(raw);
    write_rage_clipboard(context.state.groups[group]->value, *context.clipboard);
}
bool paste_group(void* raw, std::uint32_t group) {
    auto& context = *static_cast<RageWeaponClipboardContext*>(raw);
    ConfigBytes value;
    read_rage_clipboard(value, *context.clipboard);
    if (value.empty())
        return false;
    context.state.groups[group]->value = std::move(value);
    return true;
}
}
RageClipboardApi& system_rage_clipboard() noexcept {
    static RageClipboardApi api{nullptr, open,      empty,    allocate, set_data, free_memory,
                                close,   available, get_data, wire,     size,     unlock};
    return api;
}
void write_rage_clipboard(std::span<const std::byte> value, RageClipboardApi& api) { // 0x4335C6C2
    if (value.empty())
        return;
    const auto encoded = encode_config_export(value);
    if (encoded.empty() || !api.open(api.context, 0))
        return;
    if (api.empty(api.context)) {
        const auto memory = api.allocate(api.context, 0, encoded.size() + 1);
        if (memory) {
            std::memcpy(reinterpret_cast<void*>(memory), encoded.c_str(), encoded.size() + 1);
            if (!api.set_data(api.context, 1, memory))
                api.free(api.context, memory);
        }
    }
    api.close(api.context);
}
namespace {
void read_clipboard(ConfigBytes& destination, RageClipboardApi& api, bool native_pointer) {
    if (!api.available(api.context, 1) || !api.open(api.context, 0))
        return;
    if (const auto memory = api.get_data(api.context, 1)) {
        if (const auto text = api.wire(api.context, memory)) {
            const auto handle = native_pointer ? reinterpret_cast<std::uintptr_t>(text) : memory;
            const auto capacity = api.size(api.context, handle);
            std::size_t length = 0;
            while (length < capacity && text[length])
                ++length;
            destination = decode_config_export(std::string_view(text, length));
            api.unlock(api.context, handle);
        }
    }
    api.close(api.context);
}
}
void read_rage_clipboard(ConfigBytes& destination, RageClipboardApi& api) { // 0x43394985
    read_clipboard(destination, api, true);
}
void read_config_clipboard(ConfigBytes& destination, RageClipboardApi& api) {
    read_clipboard(destination, api, false);
}
void write_config_clipboard(std::span<const std::byte> value, RageClipboardApi& api,
                            std::uintptr_t window) {
    if (value.empty())
        return;
    const auto encoded = encode_config_export(value);
    const auto memory = api.allocate(api.context, GMEM_MOVEABLE, encoded.size() + 1);
    if (!memory)
        return;
    const auto text = api.wire(api.context, memory);
    if (text) {
        std::memcpy(const_cast<char*>(text), encoded.c_str(), encoded.size() + 1);
        api.unlock(api.context, memory);
        if (api.open(api.context, window)) {
            const bool transferred = api.empty(api.context) && api.set_data(api.context, CF_TEXT, memory);
            api.close(api.context);
            if (transferred)
                return;
        }
    }
    api.free(api.context, memory);
}
RageWeaponSelectorActions
rage_weapon_clipboard_actions(RageWeaponClipboardContext& context) noexcept {
    return {&context, switch_group, copy_group, paste_group};
}
void RageWeaponEnabledSettings::register_config(ConfigLink& registry) noexcept {
    for (std::size_t i = 0; i < values.size(); ++i)
        initialize_boolean_config(registry, values[i], hashes[i]);
}
std::array<BooleanConfig*, 12> RageWeaponEnabledSettings::descriptors() noexcept {
    std::array<BooleanConfig*, 12> result;
    for (std::size_t i = 0; i < values.size(); ++i)
        result[i] = &values[i];
    return result;
}
void RageWeaponEnabledSettings::bind(RageWeaponSelector& selector) noexcept {
    auto pointers = descriptors();
    selector.bind_enabled_groups(pointers);
}
}
