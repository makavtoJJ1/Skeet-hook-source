#include "skeet/menu/category_config.hpp"
#include "skeet/config_storage/load_state.hpp"
#include "skeet/color_picker.hpp"
#include "skeet/combobox.hpp"
#include "skeet/hotkey_widget.hpp"
#include "skeet/listbox.hpp"
#include "skeet/multiselect.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace skeet::ui {
namespace {
std::uint32_t lookup_hash(std::u16string_view name) noexcept {
    name = name.substr(0, name.find(u'\0'));
    // 0x433FAB07 decodes into 64 UTF16 units. It clears the destination before
    // rejecting a source whose content plus terminator exceeds that buffer.
    return category_name_hash(name.size() < 64 ? name : std::u16string_view{});
}
const void* widget_value(const Widget& widget) {
    switch (widget.kind) {
    case 3:
        return static_cast<const Checkbox&>(widget).value;
    case 4:
        return static_cast<const ComboBox&>(widget).value;
    case 5:
        return static_cast<const Slider&>(widget).value;
    case 6:
        return static_cast<const Listbox&>(widget).value;
    case 9: {
        auto* hotkey = static_cast<const HotkeyWidget&>(widget).value;
        // The native collector adds 16 even to a zero hotkey pointer. Preserve
        // that nonzero query: nullptr has distinct meaning for tag-12 entries.
        return hotkey ? &hotkey->binding : reinterpret_cast<const void*>(std::uintptr_t(16));
    }
    case 10:
        return nullptr; // Textbox's 0x5C slot is zero from its constructor.
    case 12:
        return static_cast<const ColorPicker&>(widget).value;
    case 13:
        return static_cast<const MultiSelect&>(widget).value;
    default:
        throw std::logic_error("Unsupported widget kind in category configuration");
    }
}
}
std::uint32_t category_name_hash(std::string_view name) noexcept { // 0x4334DEFF
    std::uint32_t hash = 0x811c9dc5;
    for (const unsigned char character : name) {
        if (!character)
            break;
        const auto folded = character >= 'A' && character <= 'Z' ? character + 0x20u : character;
        hash = (hash ^ folded) * 0x1000193u;
    }
    return hash ^ config_stream_hash_mask;
}
std::uint32_t category_name_hash(std::u16string_view name) noexcept { // 0x4341CD7B
    std::uint32_t hash = 0x811c9dc5;
    for (const char16_t character : name) {
        if (!character)
            break;
        const auto folded = character >= u'A' && character <= u'Z' ? character + 0x20u : character;
        hash = (hash ^ folded) * 0x1000193u;
    }
    return hash ^ config_stream_hash_mask;
}
Tab* find_config_tab(MenuWindow& window, std::uint32_t hash) { // 0x4332B5BD
    for (auto* tab : std::span(window.tabs))
        if (lookup_hash(tab->name) == hash)
            return tab;
    return nullptr;
}
Category* find_config_category(Tab& tab, std::uint32_t hash) { // 0x433E9117
    for (auto* category : std::span(tab.categories))
        if (lookup_hash(category->name) == hash)
            return category;
    return nullptr;
}
std::vector<ConfigDescriptor*> collect_category_config(Category& category,
                                                       ConfigLink& registry) { // 0x433E8D40
    std::vector<ConfigDescriptor*> descriptors;
    for (auto* widget : std::span(category.elements)) {
        switch (widget->kind) {
        case 0:
        case 1:
        case 2:
        case 7:
        case 8:
        case 11:
        case 14:
            continue;
        }
        if (auto* descriptor = find_config_by_value(registry, widget_value(*widget)))
            descriptors.push_back(descriptor);
    }
    return descriptors;
}
std::vector<ConfigDescriptor*> collect_tab_config(const Tab& tab, ConfigLink& registry) {
    std::vector<std::uintptr_t> values;
    for (const auto* category : tab.categories) {
        std::shared_lock lock(category->elements_mutex);
        for (const auto* widget : category->elements) {
            switch (widget->kind) {
            case 3:
            case 4:
            case 5:
            case 6:
            case 9:
            case 12:
            case 13:
                if (const auto* value = widget_value(*widget))
                    values.push_back(reinterpret_cast<std::uintptr_t>(value));
                break;
            }
        }
    }
    std::ranges::sort(values);
    std::vector<ConfigDescriptor*> descriptors;
    for (auto* link = registry.next; link && link != &registry; link = link->next) {
        auto& descriptor = static_cast<ConfigDescriptor&>(*link);
        if (descriptor.type == ConfigType::blob)
            continue;
        // Composite controls point into a descriptor (for example, the
        // selected weapon's slot in a LEGIT array).
        const auto bytes = config_value_bytes(descriptor);
        const auto address = reinterpret_cast<std::uintptr_t>(bytes.data());
        const auto value = std::ranges::lower_bound(values, address);
        if (value != values.end() && *value - address < bytes.size())
            descriptors.push_back(&descriptor);
    }
    return descriptors;
}
bool category_config_loading() noexcept {
    return config_loading_flag();
}
void load_category_config_values(std::span<ConfigDescriptor*> descriptors,
                                 std::span<const std::byte> selected,
                                 std::span<const std::byte> baseline_a,
                                 std::span<const std::byte> baseline_b) { // 0x43369B2C
    auto& loading = config_loading_flag();
    const bool previous = std::exchange(loading, true);
    for (const auto stream : {baseline_a, baseline_b, selected})
        if (!stream.empty())
            for (auto* descriptor : descriptors)
                deserialize_config_value(*descriptor, stream, loading);
    loading = previous;
}
bool save_category_config(MenuWindow& window, ConfigLink& registry, std::string_view tab_name,
                          std::string_view category_name, ConfigBytes& output,
                          std::span<const std::byte> baseline_a,
                          std::span<const std::byte> baseline_b) { // 0x43381B62
    auto* tab = find_config_tab(window, category_name_hash(tab_name));
    auto* category = tab ? find_config_category(*tab, category_name_hash(category_name)) : nullptr;
    if (!category)
        return false;
    const auto descriptors = collect_category_config(*category, registry);
    initialize_config_stream(output);
    for (const auto* descriptor : descriptors) {
        const auto key = config_record_key(*descriptor);
        const auto first = find_config_record(baseline_a, key);
        if (first && config_value_equals(*descriptor, first->value))
            continue;
        const auto second = find_config_record(baseline_b, key);
        if (second && config_value_equals(*descriptor, second->value))
            continue;
        if (!serialize_config_value(*descriptor, output))
            throw std::logic_error("Unsupported category configuration descriptor");
    }
    return true;
}
bool load_category_config(MenuWindow& window, ConfigLink& registry, std::string_view tab_name,
                          std::string_view category_name, std::span<const std::byte> selected,
                          std::span<const std::byte> baseline_a,
                          std::span<const std::byte> baseline_b) { // 0x433B0879
    auto* tab = find_config_tab(window, category_name_hash(tab_name));
    auto* category = tab ? find_config_category(*tab, category_name_hash(category_name)) : nullptr;
    if (!category)
        return false;
    auto descriptors = collect_category_config(*category, registry);
    load_category_config_values(descriptors, selected, baseline_a, baseline_b);
    category->config_loaded();
    return true;
}
}
