#include "skeet/menu/startup_registry.hpp"
#include "skeet/engine/owner_allocation.hpp"
#include <array>
#include <cstring>
#include <stdexcept>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace skeet::ui {
namespace {
#if defined(_WIN32)
template <class Function> Function imported(const char* name) {
    const auto module = GetModuleHandleA("ntdll.dll");
    const auto address = module ? GetProcAddress(module, name) : nullptr;
    if (!address)
        throw std::runtime_error("A required startup registry export is missing");
    return reinterpret_cast<Function>(address);
}
#endif
template <class T> void unlink(T& link) {
    link.previous->next = link.next;
    link.next->previous = link.previous;
}
}
StartupRegistryImports native_startup_registry_imports() {
#if defined(_WIN32)
    StartupRegistryImports imports{};
#define REGISTRY_IMPORT(member, name) imports.member = imported<decltype(imports.member)>(name)
    REGISTRY_IMPORT(current_user_path, "RtlFormatCurrentUserKeyPath");
    REGISTRY_IMPORT(append_string, "RtlAppendUnicodeStringToString");
    REGISTRY_IMPORT(free_string, "RtlFreeUnicodeString");
    REGISTRY_IMPORT(append_text, "RtlAppendUnicodeToString");
    REGISTRY_IMPORT(open_key, "NtOpenKey");
    REGISTRY_IMPORT(initialize_string, "RtlInitUnicodeString");
    REGISTRY_IMPORT(query_value, "NtQueryValueKey");
    REGISTRY_IMPORT(close, "NtClose");
#undef REGISTRY_IMPORT
    return imports;
#else
    throw std::runtime_error("Startup registry input requires Windows");
#endif
}
std::optional<ConfigBytes> read_startup_registry_value(const char16_t* name,
                                                       StartupRegistryImports imports,
                                                       engine::JobMemory memory) {
    if (!name || !imports.current_user_path || !imports.append_string || !imports.free_string ||
        !imports.append_text || !imports.open_key || !imports.initialize_string ||
        !imports.query_value || !imports.close || !memory.allocate || !memory.free ||
        !memory.process_heap)
        throw std::invalid_argument(
            "Startup registry input requires complete native imports and process heap");
    NativeConfigPath current{};
    (void)imports.current_user_path(&current);
    std::array<char16_t, 0x106> text;
    NativeConfigPath path{0, 0x20C, text.data()};
    // The supplied binary replaces its first status tests with unconditional
    // branches. Preserve the observed supported path and ignored statuses.
    (void)imports.append_string(&path, &current);
    imports.free_string(&current);
    (void)imports.append_text(&path, u"\\Software");
    StartupRegistryObjectAttributes attributes{
        sizeof(StartupRegistryObjectAttributes), nullptr, &path, 0x40, nullptr, nullptr};
    void* key = nullptr;
    (void)imports.open_key(&key, 0x20019, &attributes);
    imports.initialize_string(&path, name);
    std::uint32_t length = 0;
    (void)imports.query_value(key, &path, 2, nullptr, 0, &length);
    if (!length) {
        (void)imports.close(key);
        return std::nullopt;
    }
    auto* allocation = static_cast<std::byte*>(engine::allocate_owner_storage(memory, length));
    // Native43490D97 skips its close-key block on allocation failure.
    if (!allocation)
        return std::nullopt;
    const auto size = length;
    const auto status = imports.query_value(key, &path, 2, allocation, length, &length);
    if (status) {
        engine::release_owner_storage(memory, allocation);
        (void)imports.close(key);
        return std::nullopt;
    }
    ConfigBytes result(allocation, allocation + size);
    engine::release_owner_storage(memory, allocation);
    (void)imports.close(key);
    return result;
}
void unlink_startup_settings_records(SettingsPersistenceState& state) {
    auto* const owner = const_cast<std::byte*>(state.native_owner());
    if (!owner)
        throw std::invalid_argument(
            "Startup settings unlink requires its constructed native owner");
    constexpr std::array<std::size_t, 10> offsets{0,    0x14, 0x3C,  0x50,  0x64,
                                                  0x78, 0x8C, 0x29C, 0x2B0, 0x314};
    unlink(*reinterpret_cast<ConfigLink*>(owner));
    unlink(*reinterpret_cast<ConfigLink*>(owner + 0x14));
    unlink(*reinterpret_cast<HotkeyLink*>(owner + 0x24));
    for (std::size_t index = 2; index < offsets.size(); ++index)
        unlink(*reinterpret_cast<ConfigLink*>(owner + offsets[index]));
}
std::array<bool, 10> restore_startup_settings_records(SettingsPersistenceState& state,
                                                      std::span<const std::byte> information) {
    const auto word = [](std::span<const std::byte> bytes, std::size_t offset) {
        std::uint32_t value;
        std::memcpy(&value, bytes.data() + offset, sizeof value);
        return value;
    };
    if (information.size() < 12 || word(information, 4) != 3)
        throw std::invalid_argument("Startup settings require a native REG_BINARY value");
    const auto length = word(information, 8);
    if (length > information.size() - 12 || length < 12)
        throw std::invalid_argument("Startup settings registry payload has an invalid extent");
    const auto stream = information.subspan(12, length);
    if (word(stream, 0) != config_stream_magic)
        throw std::invalid_argument("Startup settings stream has an invalid signature");
    state.startup_preset_hash = 0;
    if (const auto preset = find_config_record(stream, startup_preset_record))
        state.startup_preset_hash = word(preset->value, 0);
    auto* const owner = const_cast<std::byte*>(state.native_owner());
    if (!owner)
        throw std::invalid_argument(
            "Startup settings restore requires its constructed native owner");
    constexpr std::array<std::size_t, 10> offsets{0,    0x14, 0x3C,  0x50,  0x64,
                                                  0x78, 0x8C, 0x29C, 0x2B0, 0x314};
    std::array<bool, 10> records{};
    for (std::size_t index = 0; index < offsets.size(); ++index) {
        records[index] = find_config_record(stream, state.record_keys[index]).has_value();
        if (records[index])
            (void)deserialize_config_value(
                *reinterpret_cast<ConfigDescriptor*>(owner + offsets[index]), stream, false);
    }
    return records;
}
#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(StartupRegistryObjectAttributes) == 24);
#endif
} //namespace skeet::ui
