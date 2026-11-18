#include "skeet/config_storage/presets.hpp"
#include "skeet/config_storage/preserved.hpp"
#include "skeet/menu/config_menu.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace skeet {
namespace {
std::string nul_terminated_name(std::u16string_view name) {
    auto encoded = config_name_to_utf8(name);
    encoded.resize(encoded.find('\0') == std::string::npos ? encoded.size() : encoded.find('\0'));
    return encoded;
}
std::uint32_t stored_name_hash(std::string_view name) noexcept { // 43359C70
    std::uint32_t hash = 0x811C9DC5;
    for (const unsigned char character : name) {
        if (!character)
            break;
        hash = (hash ^ character) * 0x1000193u;
    }
    return hash ^ config_stream_hash_mask;
}
}

std::uint32_t config_preset_hash(std::u16string_view name) noexcept {
    std::uint32_t hash = 0x811C9DC5;
    for (const char16_t character : name) {
        if (!character)
            break;
        hash = (hash ^ character) * 0x1000193u;
    }
    return hash ^ config_stream_hash_mask;
}
std::string config_name_to_utf8(std::u16string_view name) {
    if (name.empty())
        return {};
    if (name.size() > std::size_t(std::numeric_limits<int>::max()))
        throw std::length_error("Native preset name exceeds int");
    static_assert(sizeof(wchar_t) == sizeof(char16_t));
    const auto size =
        WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(name.data()),
                            static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), 0);
    if (size)
        WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(name.data()),
                            static_cast<int>(name.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::u16string config_name_from_utf8(std::string_view name) {
    if (name.empty())
        return {};
    if (name.size() > std::size_t(std::numeric_limits<int>::max()))
        throw std::length_error("Native preset name exceeds int");
    const auto size =
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    std::u16string result(static_cast<std::size_t>(size), 0);
    if (size)
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()),
                            reinterpret_cast<wchar_t*>(result.data()), size);
    return result;
}

ConfigPresetsManager::ConfigPresetsManager(ConfigRegistries& registries, NamedConfigStore& store,
                                           std::span<const std::byte> baseline,
                                           ConfigPresetsServices& services,
                                           SettingsStateBinding settings)
    : registries_(registries), store_(store), baseline_(baseline), services_(services),
      settings_(settings) {}
ConfigPresetsManager::ConfigPresetsManager(ConfigRegistries& registries, NamedConfigStore& store,
                                           PreservedConfigStorage& preserved,
                                           ConfigPresetsServices& services)
    : registries_(registries), store_(store), preserved_(&preserved), services_(services),
      settings_(preserved) {}
std::span<const std::byte> ConfigPresetsManager::baseline_bytes() const {
    return preserved_ ? preserved_->baseline() : baseline_;
}
ConfigStreamView ConfigPresetsManager::baseline_view() const {
    return preserved_ ? ConfigStreamView::native_vector(preserved_->baseline_cells())
                      : ConfigStreamView::snapshot(baseline_);
}
void ConfigPresetsManager::unlink(RegisteredHotkey& hotkey) noexcept {
    hotkey.previous->next = hotkey.next;
    hotkey.next->previous = hotkey.previous;
}
ConfigPresetsManager::~ConfigPresetsManager() {
    for (auto& binding : bindings_)
        unlink(binding->hotkey);
}
RegisteredHotkey* ConfigPresetsManager::selected_hotkey() noexcept {
    if (menu_)
        return menu_->hotkey().value;
    return selected_index < bindings_.size() ? &bindings_[selected_index]->hotkey : nullptr;
}
void ConfigPresetsManager::refresh() {
    const auto records = store_.list();
    while (bindings_.size() > records.size()) {
        unlink(bindings_.back()->hotkey);
        bindings_.pop_back();
    }
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (i < bindings_.size()) {
            auto& packed = bindings_[i]->hotkey.binding.packed;
            packed = (packed & 3u) | (std::uint32_t(records[i].key) << 2);
        } else {
            auto binding = std::make_unique<PresetBinding>();
            binding->manager = this;
            initialize_hotkey(registries_.hotkeys, binding->hotkey, records[i].key,
                              HotkeyMode::hold);
            binding->hotkey.callback_object = binding.get();
            binding->hotkey.callback = preset_hotkey;
            bindings_.push_back(std::move(binding));
        }
    }
    entries_.clear();
    entries_.reserve(records.size());
    for (std::size_t i = 0; i < records.size(); ++i) {
        // 4335C747 hashes the stored UTF-8 bytes before decoding the label;
        // the separate 433971CD update hashes the displayed UTF-16 name.
        const auto marker = static_cast<std::uint32_t>(i) |
                            (stored_name_hash(records[i].name) == active_hash ? 0x80000000u : 0u);
        entries_.push_back({config_name_from_utf8(records[i].name), marker, &bindings_[i]->hotkey});
    }
    if (menu_)
        menu_->synchronize_from_manager();
}
void ConfigPresetsManager::mark_entries() noexcept {
    for (std::size_t i = 0; i < entries_.size(); ++i)
        entries_[i].marker =
            static_cast<std::uint32_t>(i) |
            (config_preset_hash(entries_[i].name) == active_hash ? 0x80000000u : 0u);
}
void ConfigPresetsManager::update_markers() noexcept {
    mark_entries();
    if (menu_)
        menu_->update_markers();
}
void ConfigPresetsManager::select(std::uint32_t index) {
    selected_index = index;
    if (index < entries_.size() && entries_[index].name.size() <= 63)
        name = entries_[index].name;
}
void SKEET_HOTKEY_CALLBACK ConfigPresetsManager::preset_hotkey(void* context,
                                                               RegisteredHotkey& hotkey) {
    auto& binding = *static_cast<PresetBinding*>(context);
    auto& manager = *binding.manager;
    const auto found = std::find_if(manager.entries_.begin(), manager.entries_.end(),
                                    [&](const auto& entry) { return entry.hotkey == &hotkey; });
    if (found == manager.entries_.end())
        return;
    if (!manager.menu_)
        throw std::logic_error("Preset hotkey requires its native configuration menu");
    manager.menu_->load_hotkey(hotkey);
}
void ConfigPresetsManager::apply(std::span<const std::byte> stream, bool defer_loaded_event) {
    services_.before_baseline_load();
    deserialize_config_registry(registries_.descriptors, baseline_bytes(), preserve_hotkey_toggle);
    services_.after_baseline_load();
    deserialize_config_registry(registries_.descriptors, stream, preserve_hotkey_toggle);
    services_.after_values_load();
    if (!defer_loaded_event)
        services_.loaded_event();
}
bool ConfigPresetsManager::load() {
    if (name.empty() || name.size() >= 64)
        return false;
    active_hash = config_preset_hash(name);
    const auto bytes = store_.load(nul_terminated_name(name));
    if (bytes.empty())
        return false;
    apply(bytes);
    update_markers();
    services_.after_named_load();
    return true;
}
bool ConfigPresetsManager::save() {
    if (name.empty() || name.size() >= 64)
        return false;
    const auto requested_name = name; // 433B5919 captures the edit buffer before script callbacks.
    services_.flush_active_weapon_group();
    services_.saving_event(true);
    ConfigBytes bytes;
    if (!serialize_config_registry(registries_.descriptors, bytes, baseline_bytes()))
        throw std::logic_error("Unsupported registered preset descriptor");
    active_hash = config_preset_hash(requested_name);
    const auto* hotkey = selected_hotkey();
    const auto key =
        hotkey ? static_cast<std::uint8_t>(hotkey->binding.packed >> 2) : std::uint8_t(0x1B);
    store_.save(nul_terminated_name(requested_name), bytes, key);
    refresh();
    update_markers();
    services_.saving_event(false);
    services_.persist_settings();
    return true;
}
void ConfigPresetsManager::erase() {
    store_.erase(
        nul_terminated_name(name.size() < 256 ? std::u16string_view(name) : std::u16string_view{}));
    if (auto* settings = settings_.get(); settings && settings->startup_preset_hash &&
        settings->startup_preset_hash == config_preset_hash(name)) {
        settings->startup_preset_hash = 0;
        services_.persist_settings();
    }
    refresh();
}

bool ConfigPresetsManager::selected_for_startup() const {
    const auto* settings = settings_.get();
    return settings && settings->startup_preset_hash && selected_index < entries_.size() &&
           settings->startup_preset_hash == config_preset_hash(entries_[selected_index].name);
}

bool ConfigPresetsManager::set_selected_startup(bool enabled) {
    if (selected_index >= entries_.size())
        return false;
    const auto& selected = entries_[selected_index].name;
    const auto hash = config_preset_hash(selected);
    if (selected.empty() || selected.size() >= 64 || !hash ||
        std::count_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
            return config_preset_hash(entry.name) == hash;
        }) != 1)
        return false;
    auto& saved = settings_.require().startup_preset_hash;
    if (enabled)
        saved = hash;
    else if (saved == hash)
        saved = 0;
    services_.persist_settings();
    return true;
}

bool ConfigPresetsManager::load_startup() {
    auto* settings = settings_.get();
    if (!settings || !settings->startup_preset_hash)
        return false;
    const auto hash = settings->startup_preset_hash;
    refresh();
    const auto matches = std::count_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
        return config_preset_hash(entry.name) == hash;
    });
    if (matches != 1) {
        settings->startup_preset_hash = 0;
        services_.persist_settings();
        return false;
    }
    const auto selected = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
        return config_preset_hash(entry.name) == hash;
    });
    select(static_cast<std::uint32_t>(selected - entries_.begin()));
    if (menu_) {
        menu_->list().set_selection(selected_index);
        menu_->synchronize_selection();
    }
    return load();
}
void ConfigPresetsServices::restore_defaults(ConfigLink& registry,
                                             std::span<const std::byte> baseline,
                                             const bool& preserve_hotkey_toggle) {
    before_baseline_load();
    deserialize_config_registry(registry, baseline, preserve_hotkey_toggle);
    restore_script_defaults();
    after_values_load();
    loaded_event();
}
void ConfigPresetsServices::restore_defaults(ConfigLink& registry,
                                             PreservedConfigStorage& preserved,
                                             const bool& preserve_hotkey_toggle) {
    before_baseline_load();
    deserialize_config_registry(registry, preserved.baseline(), preserve_hotkey_toggle);
    restore_script_defaults();
    after_values_load();
    loaded_event();
}
void ConfigPresetsManager::restore_default_values() {
    if (preserved_)
        services_.restore_defaults(registries_.descriptors, *preserved_, preserve_hotkey_toggle);
    else
        services_.restore_defaults(registries_.descriptors, baseline_, preserve_hotkey_toggle);
}
void ConfigPresetsManager::reset() {
    active_hash = 0;
    restore_default_values();
    update_markers();
}
bool ConfigPresetsManager::import_from_clipboard() {
    const auto bytes = decode_config_export(services_.read_clipboard_export());
    if (bytes.empty())
        return false;
    active_hash = 0;
    apply(bytes, true);
    update_markers();
    services_.loaded_event();
    return true;
}
bool ConfigPresetsManager::export_to_clipboard() {
    const auto text = save_config_export(registries_.descriptors, baseline_bytes());
    if (!text)
        throw std::logic_error("Unsupported registered preset descriptor");
    services_.write_clipboard_export(*text);
    return true;
}

} // namespace skeet
