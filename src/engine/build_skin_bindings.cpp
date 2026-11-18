#include "skeet/engine/build_skin_bindings.hpp"
#include "skeet/engine/interface_call.hpp"
#include "skeet/engine/rtti.hpp"
#include "skeet/skins/native_entity_cosmetics.hpp"
#include "skeet/skins/native_schema.hpp"
#include "skeet/skins/native_agents.hpp"
#include "skeet/skins/refresh.hpp"
#include <array>
#include <cstring>
#include <stdexcept>

namespace skeet::engine {
namespace {
struct CodeRecipe {
    std::uint32_t cell, rva;
    std::array<std::int16_t, 64> pattern;
};
constexpr std::array<CodeRecipe, 7> functions{{
    {0x43467040u, 0x70ADB0u, {{0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x87, 0x98,
                               0x02, 0x00, 0x00, 0x83, 0xF8, 0xFF, 0x74, 0x28, 0x8B, 0x9F, 0x8C,
                               0x02, 0x00, 0x00, 0x8B, 0x75, 0x08, 0x0F, 0x1F, 0x40, 0x00, 0x8D,
                               0x0C, 0x40, 0x8B, 0x54, 0xCB, 0x10, 0x3B, 0xD6, 0x7E, 0x05, 0x8B,
                               0x04, 0xCB, 0xEB, 0x06, 0x7D, 0x09, 0x8B, 0x44, 0xCB, 0x04, 0x83,
                               0xF8, 0xFF, 0x75, 0xE5, 0x85, 0xC0, 0x78, 0x43, 0x3B}}},
    {0x4346A884u, 0x2A78A0u, {{0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08, 0x8B, 0xC3, 0x56, 0x57,
                               0x8B, 0xF9, 0xC1, 0xE0, 0x04, 0x8D, 0x77, 0x04, 0x03, 0xF0, 0x8B,
                               0x0E, 0x85, 0xC9, 0x0F, 0x84, 0x81, 0x00, 0x00, 0x00, 0xC7, 0x45,
                               0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0x8D, 0x55, 0x08, 0x8B, 0x01, 0x52,
                               0xFF, 0x50, 0x04, 0x8B, 0x46, 0x04, 0x51, 0x8B, 0xCC, 0xC1, 0xE0,
                               0x10, 0x0B, 0xC3, 0x89, 0x01, 0x8B, 0xCF, 0x8B, 0x07}}},
    {0x43468E88u, 0x2A77E0u, {{0x55, 0x8B, 0xEC, 0x51, 0x53, 0x8B, 0x99, 0x0C, 0x00, 0x02, 0x00,
                               0x89, 0x4D, 0xFC, 0x56, 0x57, 0x85, 0xDB, 0x75, 0x20, 0x68, -1,
                               -1,   -1,   -1,   0xFF, 0x15, -1,   -1,   -1,   -1,   0x8B, 0x45,
                               0x08, 0x83, 0xC4, 0x04, 0xC7, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x5F,
                               0x5E, 0x5B, 0x8B, 0xE5, 0x5D, 0xC2, 0x08, 0x00, 0x8B, 0x53, 0x08,
                               0x3B, 0xD3, 0x74, 0x2D, 0x8B, 0x43, 0x0C, 0x85, 0xD2}}},
    {0x4346D644u, 0x1E8100u, {{0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x28, 0x56, 0x57,
                               0x8B, 0x7D, 0x08, 0x8B, 0xF1, 0x85, 0xFF, 0x74, 0x0B, 0x8B, 0x07,
                               0x8B, 0xCF, 0xFF, 0x50, 0x08, 0x8B, 0x00, 0xEB, 0x03, 0x83, 0xC8,
                               0xFF, 0x3B, 0x86, 0x08, 0x03, 0x00, 0x00, 0x0F, 0x84, 0x40, 0x01,
                               0x00, 0x00, 0x8B, 0x06, 0x8B, 0xCE, 0xFF, 0x50, 0x28, 0x8B, 0xCE,
                               0xF3, 0x0F, 0x7E, 0x00, 0x8B, 0x40, 0x08, 0x89, 0x44}}},
    {0x434688A4u, 0x215080u, {{0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x2C, 0x53, 0x8B,
                               0xD9, 0x56, 0x57, 0x8B, 0x03, 0xFF, 0x90, 0xF0, 0x03, 0x00, 0x00,
                               0x8B, 0xF8, 0x89, 0x7C, 0x24, 0x0C, 0x85, 0xFF, 0x0F, 0x84, 0xA7,
                               0x03, 0x00, 0x00, 0x8B, 0x17, 0x8B, 0xCF, 0x8B, 0x92, 0x78, 0x02,
                               0x00, 0x00, 0xFF, 0xD2, 0x84, 0xC0, 0x0F, 0x84, 0x93, 0x03, 0x00,
                               0x00, 0x8B, 0x07, 0x8B, 0xCF, 0xFF, 0x90, 0x30, 0x04}}},
    {0x4346D64Cu, 0x4364D0u, {{0x53, 0x8B, 0xD9, 0x56, 0x57, 0x85, 0xDB, 0x74, 0x26, 0x33, 0xFF,
                               0x33, 0xF6, 0x0F, 0x1F, 0x00, 0xFF, 0xB6, -1,   -1,   -1,   -1,
                               0x53, 0xE8, 0xC4, 0x04, 0x56, 0x00, 0x83, 0xC4, 0x08, 0x85, 0xC0,
                               0x75, 0x15, 0x83, 0xC6, 0x14, 0x47, 0x81, 0xFE, 0x04, 0x06, 0x00,
                               0x00, 0x72, 0xE1, 0x5F, 0x5E, 0xB8, -1,   -1,   -1,   -1,   0x5B,
                               0xC3, 0x8D, 0x04, 0xBF, 0x5F, 0x5E, 0x8D, 0x04, 0x85}}},
    {0x4346A374u, 0x2D3A40u, {{0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x08, 0x56, 0x57, 0x8B, 0xF9,
                               0x33, 0xF6, 0x39, 0x77, 0x28, 0x7E, 0x1E, 0x8B, 0x47, 0x1C, 0x8B,
                               0x0C, 0xB0, 0x8B, 0x01, 0xFF, 0x50, 0x30, 0x8B, 0xD3, 0x8B, 0xC8,
                               0xE8, 0x5A, 0x28, 0x6C, 0x00, 0x85, 0xC0, 0x74, 0x23, 0x46, 0x3B,
                               0x77, 0x28, 0x7C, 0xE2, 0x53, 0xFF, 0x77, 0x78, 0x68, -1,   -1,
                               -1,   -1,   0x6A, 0x01, 0xFF, 0x15, -1,   -1,   -1}}},
}};
constexpr std::array<CodeRecipe, 4> operands{{
    {0x43468C94u, 0x195F65u, {{0x8B, 0x3D, -1,   -1,   -1,   -1,   0x85, 0xFF, 0x74, 0x1D, 0x8B,
                               0x07, 0x8B, 0xCF, 0x8B, 0x80, 0x78, 0x02, 0x00, 0x00, 0xFF, 0xD0,
                               0x84, 0xC0, 0x74, 0x0D, 0x80, 0xBF, 0x4D, 0x1A, 0x01, 0x00, 0x00,
                               0x0F, 0x85, 0x9F, 0x02, 0x00, 0x00, 0x53, 0x8B, 0x5D, 0x0C, 0x83,
                               0xFB, 0x01, 0x0F, 0x8D, 0x91, 0x02, 0x00, 0x00, 0x8B, 0x45, 0x08,
                               0x8D, 0x3C, 0xDD, 0x00, 0x00, 0x00, 0x00, 0x2B, 0xFB}}},
    {0x43468994u, 0x20AF16u, {{0xB9, -1,   -1,   -1,   -1,   0xE8, 0x20, 0x8B, 0x0C, 0x00, 0x85,
                               0xC0, 0x0F, 0x84, 0x8C, 0x00, 0x00, 0x00, 0x83, 0xC0, 0xEC, 0x0F,
                               0x84, 0x83, 0x00, 0x00, 0x00, 0x80, 0xB8, 0x48, 0x01, 0x00, 0x00,
                               0x00, 0x74, 0x7A, 0x83, 0x3D, -1,   -1,   -1,   -1,   0x00, 0x74,
                               0x44, 0xA1, -1,   -1,   -1,   -1,   0xA8, 0x01, 0x75, 0x19, 0x6A,
                               0x00, 0x83, 0xC8, 0x01, 0xB9, -1,   -1,   -1,   -1}}},
    {0x43467EF4u, 0x4062E4u, {{0x8B, 0x3C, 0xB5, -1,   -1,   -1,   -1,   0x85, 0xFF, 0x75, 0x04,
                               0x32, 0xC0, 0xEB, 0x16, 0x8B, 0x07, 0x8B, 0xCF, 0xFF, 0x10, 0xB9,
                               0x00, 0x01, 0x00, 0x00, 0x66, 0x3B, 0xC1, 0x73, 0xEC, 0x8B, 0x07,
                               0x8B, 0xCF, 0xFF, 0x10, 0x0F, 0xB6, 0xC0, 0x89, 0x83, 0xFC, 0x04,
                               0x00, 0x00, 0x8B, 0x04, 0xB5, -1,   -1,   -1,   -1,   0x5F, 0x8B,
                               0x84, 0x83, 0x0C, 0x05, 0x00, 0x00, 0x89, 0x83, 0x00}}},
    {0x434670C4u, 0x3CBB2Bu, {{0xBA, -1,   -1,   -1,   -1,   0xB8, -1,   -1,   -1,   -1,   0x8B,
                               0x08, 0x83, 0x3C, 0x8A, 0x00, 0x74, 0x12, 0x83, 0xC0, 0x04, 0x3D,
                               -1,   -1,   -1,   -1,   0x7C, 0xEE, 0x51, 0x8B, 0xCE, 0xE8, 0x41,
                               0x8A, 0xDD, 0xFF, 0x5E, 0x5D, 0xC2, 0x04, 0x00, 0xCC, 0xCC, 0xCC,
                               0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xA1, -1,
                               -1,   -1,   -1,   0x56, 0x68, 0x90, 0x00, 0x00, 0x00}}},
}};
template <class T> T read(const std::byte* address) {
    T value;
    std::memcpy(&value, address, sizeof value);
    return value;
}
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct ClientImage {
    std::byte* base;
    bool contains(const void* pointer, std::size_t size, std::uint32_t rva,
                  std::uint32_t length) const noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(pointer);
        const auto begin = reinterpret_cast<std::uintptr_t>(base) + rva;
        return address >= begin && size <= length && address - begin <= length - size;
    }
    bool text(const void* pointer, std::size_t size) const noexcept {
        return contains(pointer, size, 0x1000u, 0xB38409u);
    }
    bool data(const void* pointer, std::size_t size) const noexcept {
        return contains(pointer, size, 0xDA1000u, 0x460C3E0u);
    }
    bool rdata(const void* pointer, std::size_t size) const noexcept {
        return contains(pointer, size, 0xB3A000u, 0x26629Cu);
    }
};
ClientImage checked(std::byte* module, NativeImageEnvironment images) {
    require(module && images.image_nt_header,
            "Skin binding discovery requires its client module and PE import");
    const auto* nt = images.image_nt_header(module);
    require(nt == module + 0x150 && read<std::uint16_t>(module) == 0x5A4Du &&
                read<std::uint32_t>(nt) == 0x4550u && read<std::uint16_t>(nt + 4) == 0x14Cu &&
                read<std::uint16_t>(nt + 6) == 7u && read<std::uint16_t>(nt + 0x14) == 0xE0u &&
                read<std::uint16_t>(nt + 0x18) == 0x10Bu &&
                read<std::uint32_t>(nt + 8) == 0x65272025u &&
                read<std::uint32_t>(nt + 0x50) == 0x5509000u &&
                read<std::uint32_t>(nt + 0x58) == 0xFA635Fu,
            "Unsupported skin client PE32 build");
    constexpr std::array<const char*, 3> names{".text", ".rdata", ".data"};
    constexpr std::array<std::array<std::uint32_t, 3>, 3> expected{
        {{0x1000u, 0xB38409u, 0x60000020u},
         {0xB3A000u, 0x26629Cu, 0x40000040u},
         {0xDA1000u, 0x460C3E0u, 0xC0000040u}}};
    for (std::size_t wanted = 0; wanted < names.size(); ++wanted) {
        std::array<char, 8> name{};
        std::memcpy(name.data(), names[wanted], std::strlen(names[wanted]));
        const std::byte* match = nullptr;
        for (std::size_t index = 0; index < 7; ++index) {
            const auto* section = nt + 0xF8 + index * 40;
            if (std::memcmp(section, name.data(), 8))
                continue;
            require(!match, "Duplicate skin client section");
            match = section;
        }
        const auto& value = expected[wanted];
        require(match && read<std::uint32_t>(match + 12) == value[0] &&
                    read<std::uint32_t>(match + 8) == value[1] &&
                    read<std::uint32_t>(match + 36) == value[2],
                "Unsupported skin client section fingerprint");
    }
    return {module};
}
const std::byte* unique(const ClientImage& image, const CodeRecipe& recipe) {
    const std::byte* found = nullptr;
    for (std::uint32_t offset = 0x1000u; offset <= 0xB39409u - recipe.pattern.size(); ++offset) {
        const auto* candidate = image.base + offset;
        bool matches = true;
        for (std::size_t i = 0; i < recipe.pattern.size(); ++i)
            if (recipe.pattern[i] >= 0 &&
                static_cast<unsigned char>(candidate[i]) != recipe.pattern[i]) {
                matches = false;
                break;
            }
        if (!matches)
            continue;
        require(!found, "Skin client instruction signature is ambiguous");
        found = candidate;
    }
    require(found == image.base + recipe.rva,
            "Skin client instruction signature is missing or unsupported");
    return found;
}
const void* operand(const ClientImage& image, const std::byte* instruction, std::size_t offset,
                    std::uint32_t expected, std::size_t size) {
    const auto* pointer = reinterpret_cast<const std::byte*>(
        std::uintptr_t(read<std::uint32_t>(instruction + offset)));
    require(pointer == image.base + expected && image.data(pointer, size),
            "Skin static operand has wrong client provenance");
    return pointer;
}
ClientClass* klass(void* client, const ClientImage& image, const char* name) {
    auto* value = find_client_class(client, netvar_name_hash(name));
    require(value && image.data(value, sizeof(ClientClass)) &&
                image.rdata(value->network_name, std::strlen(name) + 1) &&
                std::strcmp(value->network_name, name) == 0,
            "Required skin client class is unavailable");
    return value;
}
std::byte* random_export(std::byte* module, const char* name, NativeImageEnvironment images) {
    require(module != nullptr, "Skin random imports require the loaded vstdlib module");
    auto* value = resolve_pe_export(module, netvar_name_hash(name), 0, true, images);
    const auto* nt = images.image_nt_header(module);
    require(nt && read<std::uint16_t>(nt + 4) == 0x14Cu && read<std::uint16_t>(nt + 0x18) == 0x10Bu,
            "Skin random imports require vstdlib PE32");
    const auto* section = find_pe_section(nt, netvar_name_hash(".text"));
    require(section != nullptr, "Skin random imports require executable vstdlib text");
    const auto first = reinterpret_cast<std::uintptr_t>(module) + read<std::uint32_t>(section + 12);
    const auto address = reinterpret_cast<std::uintptr_t>(value);
    require(value && address >= first && address - first < read<std::uint32_t>(section + 8),
            "Genuine skin vstdlib export is unavailable");
    return value;
}
}

BuildSkinBindings initialize_build_skin_bindings(NativeDataImage& data,
                                                 NativeImageEnvironment images) {
    const auto image = checked(data.native_cell<std::byte*>(0x4346A55Cu), images);
    std::array<const std::byte*, functions.size()> entries;
    for (std::size_t i = 0; i < entries.size(); ++i)
        entries[i] = unique(image, functions[i]);
    std::array<const std::byte*, operands.size()> sources;
    for (std::size_t i = 0; i < sources.size(); ++i)
        sources[i] = unique(image, operands[i]);
    auto* override_slot =
        const_cast<void*>(operand(image, sources[0], 2, 0x52BF6D8u, sizeof(void*)));
    auto* hud = const_cast<void*>(operand(image, sources[1], 1, 0x523BC98u, 0x80u));
    const auto* rows =
        static_cast<const std::byte*>(operand(image, sources[2], 3, 0xE047DCu, 0x1Cu));
    const auto* counters =
        static_cast<const std::int32_t*>(operand(image, sources[3], 1, 0x525EBC4u, 0x134u * 4u));
    // The HUD data anchor actually calls this same discovered FindHudElement.
    const auto hud_call = sources[1] + 5;
    require(hud_call[0] == std::byte{0xE8} &&
                hud_call + 5 + read<std::int32_t>(hud_call + 1) == entries[6],
            "Skin HUD data and lookup function do not share their actual call site");
    auto* base_client = data.native_cell<void*>(0x434683E0u);
    require(base_client != nullptr, "Skin classes require the canonical VClient018 interface");
    auto* glove = klass(base_client, image, "CEconWearable");
    auto* weapon_world_model = klass(base_client, image, "CBaseWeaponWorldModel");
    auto* knife = klass(base_client, image, "CKnife");
    auto* vstdlib = data.native_cell<std::byte*>(0x43468A7Cu);
    const auto seed = reinterpret_cast<BuildSkinBindings::RandomSeed>(
        random_export(vstdlib, "RandomSeed", images));
    const auto random = reinterpret_cast<BuildSkinBindings::RandomInteger>(
        random_export(vstdlib, "RandomInt", images));
    using Cosmetic = skins::NativeCosmeticBindings;
    data.native_cell<skins::NativeSchemaBindings::PaintkitDefinition>(0x43467040u) =
        reinterpret_cast<skins::NativeSchemaBindings::PaintkitDefinition>(
            const_cast<std::byte*>(entries[0]));
    data.native_cell<Cosmetic::RegisterEntity>(0x4346A884u) =
        reinterpret_cast<Cosmetic::RegisterEntity>(const_cast<std::byte*>(entries[1]));
    data.native_cell<Cosmetic::UnregisterEntity>(0x43468E88u) =
        reinterpret_cast<Cosmetic::UnregisterEntity>(const_cast<std::byte*>(entries[2]));
    data.native_cell<Cosmetic::SetParent>(0x4346D644u) =
        reinterpret_cast<Cosmetic::SetParent>(const_cast<std::byte*>(entries[3]));
    data.native_cell<Cosmetic::InvalidateBones>(0x434688A4u) =
        reinterpret_cast<Cosmetic::InvalidateBones>(const_cast<std::byte*>(entries[4]));
    data.native_cell<skins::NativeAgentBindings::SupportModels>(0x4346D64Cu) =
        reinterpret_cast<skins::NativeAgentBindings::SupportModels>(
            const_cast<std::byte*>(entries[5]));
    data.native_cell<skins::RefreshBindings::FindHudElement>(0x4346A374u) =
        reinterpret_cast<skins::RefreshBindings::FindHudElement>(
            const_cast<std::byte*>(entries[6]));
    data.native_cell<void**>(0x43468C94u) = static_cast<void**>(override_slot);
    data.native_cell<void*>(0x43468994u) = hud;
    data.native_cell<const std::byte*>(0x43467EF4u) = rows;
    data.native_cell<const std::int32_t*>(0x434670C4u) = counters;
    data.native_cell<ClientClass*>(0x4346841Cu) = glove;
    return {data, seed, random, weapon_world_model, knife};
}
BuildSkinBindings initialize_build_skin_bindings(NativeDataImage& data) {
    return initialize_build_skin_bindings(data, native_image_environment());
}
} //namespace skeet::engine
