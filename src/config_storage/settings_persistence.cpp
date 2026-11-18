#include "skeet/config_storage/settings_persistence.hpp"
#include "skeet/config_storage/preserved.hpp"
#include <cstring>
#include <stdexcept>

namespace skeet {
SettingsPersistenceState* SettingsStateBinding::get() const {
    if (!storage_)
        return standalone_;
    if (!storage_->image().native_cell<std::uint32_t>(0x43478C68u))
        return nullptr;
    return &storage_->settings();
}
SettingsPersistenceState& SettingsStateBinding::require() const {
    auto* state = get();
    if (!state)
        throw std::logic_error(
            "CONFIG settings owner has not reached its startup construction phase");
    return *state;
}
namespace {
constexpr std::array<std::size_t, 10> descriptor_offsets{0,    0x14, 0x3C,  0x50,  0x64,
                                                         0x78, 0x8C, 0x29C, 0x2B0, 0x314};
std::byte* checked_owner(std::byte* owner, std::size_t size) {
    if (!owner || size < 0x325 || reinterpret_cast<std::uintptr_t>(owner) % alignof(std::uint32_t))
        throw std::invalid_argument(
            "Constructed native settings owner has an invalid extent or alignment");
    return owner;
}
template <class T> T read(const std::byte* bytes, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, bytes + offset, sizeof value);
    return value;
}
template <class T> void write(std::byte* bytes, std::size_t offset, T value) noexcept {
    std::memcpy(bytes + offset, &value, sizeof value);
}
template <class T> T& native_value(std::byte* bytes, std::size_t size, std::size_t offset) {
    if (!bytes || offset > size || size - offset < sizeof(T))
        throw std::invalid_argument("Constructed native settings value is outside its owner");
    return *reinterpret_cast<T*>(bytes + offset);
}
template <class T> std::span<const std::byte> value_bytes(const T& value) {
    return std::as_bytes(std::span<const T>(&value, 1));
}
}

ConfigRecordKey SettingsRecordKeys::operator[](std::size_t index) const noexcept {
    if (!native_owner_)
        return owned_[index];
    const auto descriptor = native_owner_ + descriptor_offsets[index];
    return {read<std::uint16_t>(descriptor, 0xC), read<std::uint8_t>(descriptor, 0xE),
            read<std::uint32_t>(descriptor, 8)};
}
void SettingsRecordKeys::set(std::size_t index, ConfigRecordKey key) noexcept {
    if (!native_owner_) {
        owned_[index] = key;
        return;
    }
    auto* descriptor = native_owner_ + descriptor_offsets[index];
    write(descriptor, 8, key.hash);
    write(descriptor, 0xC, key.size);
    write(descriptor, 0xE, key.type);
}
SettingsPersistenceState::SettingsPersistenceState()
    : owned_(std::make_unique<SettingsPersistenceValues>()), global_value(owned_->global_value),
      menu_binding(owned_->menu_binding), input_mode(owned_->input_mode),
      dpi_scale(owned_->dpi_scale), menu_width(owned_->menu_width), menu_height(owned_->menu_height),
      script_hashes(owned_->script_hashes), optional_value(owned_->optional_value),
      menu_controls(owned_->menu_controls), flag(owned_->flag) {}
SettingsPersistenceState::SettingsPersistenceState(std::byte* owner, std::size_t size)
    : native_owner_(checked_owner(owner, size)), record_keys(native_owner_),
      global_value(native_value<std::int32_t>(native_owner_, size, 0x10)),
      menu_binding(native_value<HotkeyBinding>(native_owner_, size, 0x34)),
      input_mode(native_value<std::int32_t>(native_owner_, size, 0x4C)),
      dpi_scale(native_value<std::int32_t>(native_owner_, size, 0x60)),
      menu_width(native_value<std::int32_t>(native_owner_, size, 0x74)),
      menu_height(native_value<std::int32_t>(native_owner_, size, 0x88)),
      script_hashes(native_value<std::array<std::uint32_t, 128>>(native_owner_, size, 0x9C)),
      optional_value(native_value<std::uint32_t>(native_owner_, size, 0x2AC)),
      menu_controls(native_value<std::array<std::int32_t, 21>>(native_owner_, size, 0x2C0)),
      flag(native_value<std::uint8_t>(native_owner_, size, 0x324)) {}
namespace {
std::uint32_t script_hash(std::u16string_view name) noexcept {
    std::uint32_t hash = 0x811C9DC5;
    for (char16_t character : name) {
        if (!character)
            break;
        hash = (hash ^ character) * 0x1000193u;
    }
    return hash;
}
bool module_gate(void* context, std::u16string_view module) {
    const auto hash = settings_module_hash(module);
    if (hash != 0x0F3D9476 && hash != 0xE2280AF1)
        return true;
    *static_cast<bool*>(context) = true;
    return false;
}
}

std::uint32_t settings_module_hash(std::u16string_view module) noexcept {
    std::uint32_t hash = 0x811C9DC5;
    for (char16_t character : module) {
        if (character >= u'A' && character <= u'Z')
            character += u'a' - u'A';
        hash = (hash ^ character) * 0x1000193u;
    }
    return hash ^ config_stream_hash_mask;
}

bool serialize_settings_snapshot(SettingsPersistenceState& state,
                                 const SettingsPersistenceInputs& inputs,
                                 std::uint32_t optional_value, std::uint32_t excluded_script,
                                 ConfigBytes& output) {
    constexpr std::array<std::uint16_t, 10> sizes{4, 8, 4, 4, 4, 4, 512, 4, 84, 1};
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const auto key = state.record_keys[i];
        if (key.size != sizes[i])
            return false;
        const auto type = key.type;
        if (i == 1 ? type != 7 : (type != 1 && type != 3 && type != 4 && type != 6 && type != 11))
            return false;
    }
    state.global_value = inputs.global_value;
    state.menu_binding.packed =
        inputs.menu_key ^ ((state.menu_binding.packed ^ inputs.menu_key) & 3u);
    state.input_mode = inputs.input_mode;
    state.dpi_scale = inputs.dpi_scale;
    state.menu_width = inputs.menu_width;
    state.menu_height = inputs.menu_height;
    state.menu_controls[0] = 9;
    for (std::size_t i = 0; i < 15; ++i)
        state.menu_controls[i + 1] = inputs.menu_control_values[i];
    state.menu_controls[19] = inputs.menu_control_values[15];
    state.menu_controls[20] = inputs.menu_control_values[16];
    if (inputs.capture_scripts) {
        state.script_hashes.fill(0);
        std::size_t count = 0;
        for (const auto& script : inputs.scripts) {
            const bool starred = !script.name.empty() && script.name.front() == u'*';
            if (script.selected == starred)
                continue;
            const auto hash = script_hash(script.name);
            if (hash == excluded_script)
                continue;
            state.script_hashes[count++] = hash;
            if (count == state.script_hashes.size())
                break;
        }
    }
    state.flag = inputs.flag;
    initialize_config_stream(output);
    append_config_record(output, state.record_keys[0], value_bytes(state.global_value));
    append_config_record(output, state.record_keys[1], value_bytes(state.menu_binding));
    append_config_record(output, state.record_keys[2], value_bytes(state.input_mode));
    append_config_record(output, state.record_keys[3], value_bytes(state.dpi_scale));
    append_config_record(output, state.record_keys[4], value_bytes(state.menu_width));
    append_config_record(output, state.record_keys[5], value_bytes(state.menu_height));
    append_config_record(output, state.record_keys[6], value_bytes(state.script_hashes));
    if (optional_value) {
        state.optional_value = optional_value;
        append_config_record(output, state.record_keys[7], value_bytes(state.optional_value));
    }
    if (state.startup_preset_hash)
        append_config_record(output, startup_preset_record, value_bytes(state.startup_preset_hash));
    append_config_record(output, state.record_keys[8], value_bytes(state.menu_controls));
    append_config_record(output, state.record_keys[9], value_bytes(state.flag));
    return true;
}

SettingsPersistence::SettingsPersistence(SettingsPersistenceBackend& backend,
                                         SettingsPersistenceState& state,
                                         std::u16string_view value_name, Capture capture)
    : backend_(backend), state_(state), value_name_(value_name), capture_(std::move(capture)) {}
SettingsPersistence::SettingsPersistence(SettingsPersistenceBackend& backend,
                                         PreservedConfigStorage& preserved, Capture capture)
    : backend_(backend), state_(preserved), preserved_(&preserved), capture_(std::move(capture)) {}

bool SettingsPersistence::persist(std::uint32_t optional_value, std::uint32_t excluded_script) {
    bool blocked = false;
    backend_.enumerate_loaded_modules(module_gate, &blocked);
    if (blocked)
        return false;
    ConfigBytes output;
    output.reserve(0x1000);
    const auto window =
        preserved_ ? preserved_->capture_registry_window() : SettingsRegistryWindow{};
    auto& state = state_.require();
    if (!serialize_settings_snapshot(state, capture_(), optional_value, excluded_script, output))
        return false;
    const auto value_name =
        preserved_ ? preserved_->registry_value_name(window) : std::u16string_view(value_name_);
    backend_.write_registry_value(5, u"Software", value_name, 3, output);
    return true;
}

} // namespace skeet
