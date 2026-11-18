#include "skeet/config_storage/storage.hpp"
#include <cstring>
#include <stdexcept>

namespace skeet {
namespace {
std::uint16_t read16(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return std::to_integer<std::uint16_t>(bytes[offset]) |
           (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8);
}
std::uint32_t read32(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return read16(bytes, offset) | (std::uint32_t(read16(bytes, offset + 2)) << 16);
}
void write16(ConfigBytes& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = std::byte(value & 255);
    bytes[offset + 1] = std::byte(value >> 8);
}
void write32(ConfigBytes& bytes, std::size_t offset, std::uint32_t value) {
    write16(bytes, offset, std::uint16_t(value));
    write16(bytes, offset + 2, std::uint16_t(value >> 16));
}
template <class T> std::span<std::byte> bytes_of(T& value) noexcept {
    return std::as_writable_bytes(std::span<T>(&value, 1));
}
bool supported(const ConfigDescriptor& descriptor) noexcept {
    return config_value_supported(descriptor);
}
constexpr std::string_view alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

// Complete native lookup 43367569, with a bounded read at each record.
std::optional<ConfigRecordView> find_config_record(std::span<const std::byte> stream,
                                                   ConfigRecordKey key) noexcept {
    if (stream.size() <= 12 || read32(stream, 0) != config_stream_magic)
        return {};
    const auto hash = read32(stream, 8) ^ key.hash ^ config_stream_hash_mask;
    std::size_t offset = 12;
    while (offset < stream.size()) {
        if (stream.size() - offset < 8)
            return {};
        const auto size = read16(stream, offset);
        const auto type = std::to_integer<std::uint8_t>(stream[offset + 2]);
        if (stream.size() - offset - 8 < size)
            return {};
        if (read32(stream, offset + 4) == hash && type == key.type &&
            (type == 12 || size == key.size)) {
            return ConfigRecordView{{size, type, read32(stream, offset + 4)},
                                    std::to_integer<std::uint8_t>(stream[offset + 3]),
                                    offset,
                                    stream.subspan(offset + 8, size)};
        }
        offset += std::size_t(size) + 8;
        if (offset >= stream.size())
            break;
        if (stream.size() - offset < 3 || stream[offset + 2] == std::byte{8})
            break;
    }
    return {};
}

bool valid_config_stream(std::span<const std::byte> stream) noexcept {
    if (stream.size() < 12 || read32(stream, 0) != config_stream_magic)
        return false;
    auto remaining = stream.subspan(12);
    while (!remaining.empty()) {
        if (remaining.size() < 8)
            return false;
        const auto size = read16(remaining, 0);
        if (remaining.size() - 8 < size)
            return false;
        remaining = remaining.subspan(8 + size);
    }
    return true;
}

void initialize_config_stream(ConfigBytes& stream) {
    stream.resize(12);
    write32(stream, 0, config_stream_magic);
    write32(stream, 4, config_stream_version);
    write32(stream, 8, config_stream_hash_mask);
}

// 43417939; the resize preserves the prefix and initializes the new padding byte.
void append_config_record(ConfigBytes& stream, ConfigRecordKey key,
                          std::span<const std::byte> value) {
    if (value.size() != key.size)
        throw std::invalid_argument("Native configuration record size mismatch");
    const auto offset = stream.size();
    stream.resize(offset + 8 + value.size());
    write16(stream, offset, key.size);
    stream[offset + 2] = std::byte(key.type);
    write32(stream, offset + 4, key.hash);
    if (!value.empty())
        std::memcpy(stream.data() + offset + 8, value.data(), value.size());
}

std::span<std::byte> config_value_bytes(ConfigDescriptor& descriptor) noexcept {
    auto& header = config_native_header(descriptor);
    std::span<std::byte> bytes;
    switch (header.type) {
    case ConfigType::boolean:
        bytes = bytes_of(static_cast<BooleanConfig&>(header).value);
        break;
    case ConfigType::integer:
        bytes = bytes_of(static_cast<IntegerConfig&>(header).value);
        break;
    case ConfigType::color:
        bytes = bytes_of(static_cast<ColorConfig&>(header).value);
        break;
    case ConfigType::integer_array:
    case ConfigType::fixed_array:
        //433E1D08 copies exactly metadata+C bytes from native descriptor+10.
        // All ConfigValue<std::array<T,N>> retain that same plain header.
        bytes = {reinterpret_cast<std::byte*>(&header) + sizeof(ConfigDescriptor),
                 header.serialized_size};
        break;
    case ConfigType::hotkey:
        bytes = bytes_of(static_cast<HotkeyConfig&>(header).value.binding);
        break;
    case ConfigType::blob: {
        auto* value = config_blob_value(descriptor);
        return value ? std::span<std::byte>(*value) : std::span<std::byte>{};
    }
    default:
        return {};
    }
    return bytes.size() == header.serialized_size ? bytes : std::span<std::byte>{};
}
std::span<const std::byte> config_value_bytes(const ConfigDescriptor& descriptor) noexcept {
    return config_value_bytes(const_cast<ConfigDescriptor&>(descriptor));
}
bool config_value_supported(const ConfigDescriptor& descriptor) noexcept {
    return config_native_header(descriptor).type == ConfigType::blob
               ? config_blob_value(descriptor) != nullptr
               : !config_value_bytes(descriptor).empty();
}
ConfigRecordKey config_record_key(const ConfigDescriptor& descriptor) noexcept {
    const auto& header = config_native_header(descriptor);
    return {header.serialized_size, static_cast<std::uint8_t>(header.type), header.hash};
}
void initialize_blob_config(ConfigLink& registry, ConfigBlob& descriptor, std::uint32_t hash) {
    auto& owner = ConfigRegistries::for_descriptors(registry);
    if (!config_blob_value(descriptor))
        owner.bind_blob(descriptor, descriptor);
    auto& header = config_native_header(descriptor);
    header.hash = hash;
    header.serialized_size = 16;
    header.type = ConfigType::blob;
    append_config_link(registry, header);
    descriptor.value.initialize_empty();
}
bool serialize_config_value(const ConfigDescriptor& descriptor, ConfigBytes& output) {
    if (!supported(descriptor))
        return false;
    auto key = config_record_key(descriptor);
    const auto bytes = config_value_bytes(descriptor);
    if (config_native_header(descriptor).type == ConfigType::blob) {
        if (bytes.size() >= 0xFFFF)
            return true;
        key.size = static_cast<std::uint16_t>(bytes.size());
    }
    append_config_record(output, key, bytes);
    return true;
}

// 433E113E compares the entire serialized value, including both hotkey words.
bool config_value_equals(const ConfigDescriptor& descriptor,
                         std::span<const std::byte> value) noexcept {
    const auto bytes = config_value_bytes(descriptor);
    return supported(descriptor) && bytes.size() == value.size() &&
           (bytes.empty() || std::memcmp(bytes.data(), value.data(), bytes.size()) == 0);
}

// 433CFFB4 and tag-7 specialization 433FF6E4.
bool deserialize_config_value(ConfigDescriptor& descriptor, std::span<const std::byte> stream,
                              bool preserve_hotkey_toggle) noexcept {
    if (!supported(descriptor))
        return false;
    const auto record = find_config_record(stream, config_record_key(descriptor));
    if (!record)
        return false;
    const bool changed = !config_value_equals(descriptor, record->value);
    auto& header = config_native_header(descriptor);
    if (header.type == ConfigType::blob) {
        config_blob_value(descriptor)->assign(record->value);
        return !changed;
    }
    auto bytes = config_value_bytes(descriptor);
    std::uint32_t old_packed = 0;
    if (header.type == ConfigType::hotkey)
        old_packed = static_cast<HotkeyConfig&>(header).value.binding.packed;
    std::memcpy(bytes.data(), record->value.data(), bytes.size());
    if (header.type == ConfigType::hotkey && preserve_hotkey_toggle) {
        auto& packed = static_cast<HotkeyConfig&>(header).value.binding.packed;
        packed ^= (packed ^ old_packed) & 2u;
    }
    return changed;
}

void visit_config_registry(ConfigLink& sentinel, ConfigVisitor visitor, void* context) {
    for (auto* link = sentinel.next; link && link != &sentinel; link = link->next)
        visitor(context, static_cast<ConfigDescriptor&>(*link));
}
ConfigDescriptor* find_config_by_value(ConfigLink& sentinel, const void* value) noexcept {
    for (auto* link = sentinel.next; link && link != &sentinel; link = link->next) {
        auto& descriptor = static_cast<ConfigDescriptor&>(*link);
        if (descriptor.type == ConfigType::blob) {
            if (!value)
                return &descriptor;
            continue;
        }
        const auto bytes = config_value_bytes(descriptor);
        if (!bytes.empty() && bytes.data() == value)
            return &descriptor;
    }
    return nullptr;
}

// Complete registry writer 433D0EB1 and its equality/record dispatch chain.
bool serialize_config_registry(ConfigLink& sentinel, ConfigBytes& stream,
                               std::span<const std::byte> baseline) {
    for (auto* link = sentinel.next; link && link != &sentinel; link = link->next)
        if (!supported(static_cast<const ConfigDescriptor&>(*link)))
            return false;
    initialize_config_stream(stream);
    for (auto* link = sentinel.next; link && link != &sentinel; link = link->next) {
        const auto& descriptor = static_cast<const ConfigDescriptor&>(*link);
        const auto key = config_record_key(descriptor);
        const auto prior = find_config_record(baseline, key);
        if (!prior || !config_value_equals(descriptor, prior->value))
            serialize_config_value(descriptor, stream);
    }
    return true;
}
ConfigLoadResult deserialize_config_registry(ConfigLink& sentinel,
                                             std::span<const std::byte> stream,
                                             bool preserve_hotkey_toggle) {
    ConfigLoadResult result;
    for (auto* link = sentinel.next; link && link != &sentinel; link = link->next) {
        auto& descriptor = static_cast<ConfigDescriptor&>(*link);
        ++result.visited;
        if (!supported(descriptor)) {
            ++result.unsupported;
            continue;
        }
        const auto record = find_config_record(stream, config_record_key(descriptor));
        if (record) {
            ++result.matched;
            if (!config_value_equals(descriptor, record->value))
                ++result.changed;
        }
        deserialize_config_value(descriptor, stream, preserve_hotkey_toggle);
    }
    return result;
}

std::string encode_config_export(std::span<const std::byte> stream) {
    std::string output;
    output.reserve(((stream.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < stream.size(); i += 3) {
        const auto first = std::to_integer<unsigned>(stream[i]);
        const auto second = i + 1 < stream.size() ? std::to_integer<unsigned>(stream[i + 1]) : 0u;
        const auto third = i + 2 < stream.size() ? std::to_integer<unsigned>(stream[i + 2]) : 0u;
        output += alphabet[first >> 2];
        output += alphabet[((first & 3) << 4) | (second >> 4)];
        output += i + 1 < stream.size() ? alphabet[((second & 15) << 2) | (third >> 6)] : '=';
        output += i + 2 < stream.size() ? alphabet[third & 63] : '=';
    }
    return output;
}
ConfigBytes decode_config_export(std::string_view encoded) {
    ConfigBytes output;
    output.reserve(encoded.size() / 4 * 3);
    std::uint32_t accumulator = 0;
    unsigned bits = 0;
    for (const unsigned char character : encoded) {
        if (character == '=')
            break;
        const auto position = alphabet.find(char(character));
        if (position == std::string_view::npos)
            continue;
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(position);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(std::byte((accumulator >> bits) & 255));
        }
    }
    return output;
}
std::optional<std::string> save_config_export(ConfigLink& sentinel,
                                              std::span<const std::byte> baseline) {
    ConfigBytes stream;
    if (!serialize_config_registry(sentinel, stream, baseline))
        return {};
    return encode_config_export(stream);
}
ConfigLoadResult load_config_export(ConfigLink& sentinel, std::string_view encoded,
                                    std::span<const std::byte> baseline,
                                    bool preserve_hotkey_toggle) {
    auto result = deserialize_config_registry(sentinel, baseline, preserve_hotkey_toggle);
    const auto stream = decode_config_export(encoded);
    const auto selected = deserialize_config_registry(sentinel, stream, preserve_hotkey_toggle);
    result.visited += selected.visited;
    result.matched += selected.matched;
    result.changed += selected.changed;
    result.unsupported += selected.unsupported;
    return result;
}

} // namespace skeet
