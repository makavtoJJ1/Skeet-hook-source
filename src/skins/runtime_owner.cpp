#include "skeet/skins/runtime_owner.hpp"
#include "skeet/client/runtime_storage.hpp"
#include "skeet/client/runtime_composition.hpp"
#include "skeet/engine/interface_call.hpp"
#include "skeet/engine/build_skin_receive_patches.hpp"
#include <map>
#include <stdexcept>
#include <string>

namespace skeet::skins {
namespace {
template <class T> T& cell(client::ClientRuntimeStorage& storage, std::uint32_t address) {
    return storage.data().native_cell<T>(address);
}
std::ptrdiff_t offset(const engine::NativeOffsetGlobals& globals, std::uint32_t pointer,
                      std::uint32_t key, std::uint32_t mask) {
    return std::ptrdiff_t(*globals.pointers.at(pointer) ^ globals.keys.at(key) ^ mask);
}
Settings& require_inputs(client::ClientRuntimeStorage& storage,
                         client::ConfigurationRuntimeOwner& configuration,
                         const engine::BuildSkinBindings& imports) {
    if (&configuration.storage() != &storage || &imports.data != &storage.data())
        throw std::invalid_argument(
            "Skin runtime requires its canonical configuration and data owners");
    if (!imports.seed_random || !imports.random_integer || !imports.weapon_world_model_class ||
        !imports.knife_class)
        throw std::invalid_argument(
            "Skin runtime requires genuine engine random imports and client class objects");
    if (storage.event_registry().engine_interface != cell<void*>(storage, 0x4346A514u) ||
        !storage.event_registry().engine_interface ||
        storage.event_registry().event_manager != cell<void*>(storage, 0x43468364u) ||
        !storage.event_registry().event_manager)
        throw std::invalid_argument(
            "Skin death listeners require the canonically bound engine event registry");
    for (const auto address : {0x4346A514u, 0x4346A890u, 0x434683E0u, 0x4346A614u, 0x43469330u,
                               0x43467198u, 0x4346A31Cu, 0x43468C94u, 0x4346841Cu})
        if (!cell<void*>(storage, address))
            throw std::invalid_argument(
                "Skin runtime requires the initialized canonical engine interfaces and data operands");
    if (!cell<NativeSchemaBindings::AcquireItemSystem>(storage, 0x43468B9Cu) ||
        !cell<NativeSchemaBindings::PaintkitDefinition>(storage, 0x43467040u) ||
        !cell<NativeAgentBindings::SupportModels>(storage, 0x4346D64Cu) ||
        !cell<const float*>(storage, 0x4346A334u))
        throw std::invalid_argument(
            "Skin runtime requires its real item, schema, support-model and clock bindings");
    return configuration.skins_settings();
}
class NativeModels final : public ModelInfo {
public:
    explicit NativeModels(void* const& source) : source_(source) {}
    std::int32_t model_index(std::string_view path) override {
        const std::string text(path);
        return engine::invoke<std::int32_t>(source_, 2, text.c_str());
    }
    bool load_model(std::string_view path) override {
        const std::string text(path);
        return engine::invoke<void*>(source_, 43, text.c_str()) != nullptr;
    }

private:
    void* const& source_;
};
class NativeTable final : public StringTable {
public:
    explicit NativeTable(void* source) : source_(source) {}
    const char* name() override {
        return engine::invoke<const char*>(source_, 1);
    }
    std::int32_t add_string(std::int32_t flags, std::string_view path, std::int32_t length,
                            const void* data) override {
        const std::string text(path);
        return engine::invoke<std::int32_t>(source_, 8, flags, text.c_str(), length, data);
    }

private:
    void* source_;
};
class NativeTables final : public StringTables {
public:
    explicit NativeTables(void* const& source) : source_(source) {}
    std::int32_t count() override {
        return engine::invoke<std::int32_t>(source_, 5);
    }
    StringTable* at(std::int32_t index) override {
        auto* table = engine::invoke<void*>(source_, 4, index);
        if (!table)
            return nullptr;
        auto& view = views_[table];
        if (!view)
            view = std::make_unique<NativeTable>(table);
        return view.get();
    }

private:
    void* const& source_;
    std::map<void*, std::unique_ptr<NativeTable>> views_;
};
class NativeCounterRandom final : public CounterRandom {
public:
    explicit NativeCounterRandom(engine::BuildSkinBindings::RandomInteger function)
        : function_(function) {}
    std::int32_t random_integer(std::int32_t minimum, std::int32_t maximum) override {
        return function_(minimum, maximum);
    }

private:
    engine::BuildSkinBindings::RandomInteger function_;
};
NativeCosmeticBindings cosmetic_bindings(client::ClientRuntimeStorage& storage) {
    return {cell<void*>(storage, 0x434683E0u),
            cell<void*>(storage, 0x4346A514u),
            cell<void*>(storage, 0x4346A890u),
            cell<void*>(storage, 0x4346A614u),
            cell<void*>(storage, 0x43469330u),
            cell<void*>(storage, 0x43467198u),
            cell<NativeCosmeticBindings::AcquireItemSystem>(storage, 0x43468B9Cu),
            cell<NativeCosmeticBindings::RegisterEntity>(storage, 0x4346A884u),
            cell<NativeCosmeticBindings::UnregisterEntity>(storage, 0x43468E88u),
            cell<NativeCosmeticBindings::SetParent>(storage, 0x4346D644u),
            cell<NativeCosmeticBindings::InvalidateTransform>(storage, 0x43468C10u),
            cell<NativeCosmeticBindings::InvalidateBones>(storage, 0x434688A4u)};
}
events::WeaponDeathOffsets death_offsets(const engine::NativeOffsetGlobals& globals) {
    return {offset(globals, 0x434688B0u, 0x4346A518u, 0x8B5586F6u),
            offset(globals, 0x43467A74u, 0x434678C0u, 0xCD812654u),
            offset(globals, 0x4346908Cu, 0x43468BA0u, 0x5C9F0FBBu),
            offset(globals, 0x43469248u, 0x4346798Cu, 0x3C1D8AA6u),
            offset(globals, 0x43468390u, 0x434683E4u, 0xA1493792u),
            offset(globals, 0x434682E8u, 0x4346A6C8u, 0xF2DC2C8Au),
            offset(globals, 0x43469080u, 0x434682B4u, 0xD172D9BAu)};
}
}

struct SkinsRuntimeOwner::Data {
    client::ClientRuntimeStorage& storage;
    engine::BuildSkinBindings imports;
    Settings& settings;
    NativeModels models;
    NativeTables tables;
    NativeCounterRandom random;
    WeaponRefresh refresh;
    NativeSchemaCatalog schema;
    NativeEntityCosmetics cosmetics;
    AgentModelReceiver agent;
    SequenceRuntime sequence;
    SequenceReceiveBindings receive;
    SequenceReceivers receivers;
    engine::RecvVarProxy viewmodel_model = nullptr;
    NativeSequenceRuntime sequence_update;
    events::DeathBindings death;
    events::WeaponDeathOffsets death_fields;
    events::DeathWeaponIcons death_icons;
    events::DeathWeaponIconsListener death_listener;
    // The original networkable method returns ClientClass*, represented by a
    // dword in the source runtime. These are actual class-object identities.
    const std::uint32_t weapon_world_model_class, knife_class;
    client::SkinRuntimeFactoryBindings factories;
    SequenceOwnerBindings sequence_factory;

    Data(client::ClientRuntimeStorage& runtime, client::ConfigurationRuntimeOwner& configuration,
         engine::BuildSkinBindings bindings)
        : storage(runtime), imports(bindings),
          settings(require_inputs(storage, configuration, imports)),
          models(cell<void*>(storage, 0x4346A614u)), tables(cell<void*>(storage, 0x43467198u)),
          random(imports.random_integer),
          refresh({cell<std::uint8_t>(storage, 0x43475BD2u),
                   cell<const float*>(storage, 0x4346A334u), cell<float>(storage, 0x4346A8A0u),
                   cell<std::uint8_t>(storage, 0x4346838Bu)},
                  {cell<void*>(storage, 0x4346A514u), cell<void*>(storage, 0x4346A890u),
                   offset(storage.offsets().globals, 0x43468E78u, 0x434684E4u, 0x9D920E35u),
                   offset(storage.offsets().globals, 0x434688B0u, 0x4346A518u, 0x8B5586F6u),
                   cell<void*>(storage, 0x43475808u), cell<void*>(storage, 0x43468994u),
                   cell<RefreshBindings::FindHudElement>(storage, 0x4346A374u),
                   cell<void*>(storage, 0x43468364u)}),
          schema({cell<void*>(storage, 0x43476424u),
                  cell<NativeSchemaBindings::AcquireItemSystem>(storage, 0x43468B9Cu),
                  cell<NativeSchemaBindings::PaintkitDefinition>(storage, 0x43467040u),
                  cell<void*>(storage, 0x4346A31Cu)}),
          cosmetics(settings, native_cosmetic_offsets(storage.offsets().globals),
                    {cell<void*>(storage, 0x43476424u), *cell<void**>(storage, 0x43468C94u),
                     cell<engine::ClientClass*>(storage, 0x4346841Cu)},
                    cosmetic_bindings(storage)),
          agent(
              {cell<std::uint32_t>(storage, 0x4346D648u), cell<std::int32_t>(storage, 0x4346D650u)},
              {cell<void*>(storage, 0x4346A514u),
               cell<engine::RecvVarProxy>(storage, 0x43476A94u)}),
          receive{settings,
                  cell<void*>(storage, 0x43475808u),
                  cell<void*>(storage, 0x4346A514u),
                  storage.offsets(),
                  nullptr,
                  nullptr,
                  nullptr,
                  nullptr},
          receivers{sequence, receive},
          sequence_update(settings, sequence,
                          {storage.offsets(), cell<void*>(storage, 0x4346A514u),
                           cell<void*>(storage, 0x4346A890u), cell<void*>(storage, 0x4346A614u),
                           cell<void*>(storage, 0x43469330u), cell<void*>(storage, 0x4346A460u),
                           cell<misc::ViewmodelLagOwner*>(storage, 0x43475860u),
                           cell<std::int32_t*>(storage, 0x43468328u),
                           cell<void**>(storage, 0x434693A4u), cell<float*>(storage, 0x4346A338u),
                           receive, viewmodel_model, imports.seed_random, imports.random_integer},
                          refresh),
          death{cell<void*>(storage, 0x4346A514u), cell<void*>(storage, 0x4346A890u), nullptr},
          death_fields(death_offsets(storage.offsets().globals)), death_icons(settings, sequence),
          death_listener{death_icons, death, death_fields},
          weapon_world_model_class(static_cast<std::uint32_t>(
              reinterpret_cast<std::uintptr_t>(imports.weapon_world_model_class))),
          knife_class(
              static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(imports.knife_class))),
          factories{storage.configuration(),
                    {storage.owner_registries().at(0x43475B78u), storage.memory()},
                    storage.command_phases(),
                    storage.entity_observers(),
                    settings,
                    {settings.weapons, random, refresh, cell<ui::RootMenu*>(storage, 0x43476924u),
                     cell<std::array<std::uint32_t, 3>>(storage, 0x43474580u),
                     cell<void*>(storage, 0x4346A514u), cell<void*>(storage, 0x4346A890u),
                     cell<const std::byte*>(storage, 0x43467EF4u),
                     cell<const std::int32_t*>(storage, 0x434670C4u), storage.offsets().globals,
                     death, death_fields, storage.event_registry()},
                    {settings, cosmetics, cell<void*>(storage, 0x4346A514u),
                     cell<void*>(storage, 0x43475808u)},
                    {settings, schema.catalog(), agent, cell<void*>(storage, 0x4346A890u),
                     cell<NativeAgentBindings::SupportModels>(storage, 0x4346D64Cu),
                     storage.offsets().globals, models, tables, storage.data_update_observers()}},
          sequence_factory{settings,
                           sequence,
                           receivers,
                           sequence_update,
                           cell<void*>(storage, 0x434683E0u),
                           cell<void*>(storage, 0x4346A890u),
                           cell<void*>(storage, 0x43467198u),
                           weapon_world_model_class,
                           knife_class,
                           viewmodel_model,
                           death_listener,
                           storage.event_registry()} {}
};

SkinsRuntimeOwner::SkinsRuntimeOwner(client::ClientRuntimeStorage& storage,
                                     client::ConfigurationRuntimeOwner& configuration,
                                     engine::BuildSkinBindings imports)
    : data_(std::make_unique<Data>(storage, configuration, imports)) {}
SkinsRuntimeOwner::~SkinsRuntimeOwner() = default;
NativeCatalog& SkinsRuntimeOwner::catalog() noexcept {
    return data_->schema.catalog();
}
WeaponRefresh& SkinsRuntimeOwner::refresh() noexcept {
    return data_->refresh;
}
Settings& SkinsRuntimeOwner::settings() noexcept {
    return data_->settings;
}
NativeEntityCosmetics& SkinsRuntimeOwner::cosmetics() noexcept {
    return data_->cosmetics;
}
ModelInfo& SkinsRuntimeOwner::models() noexcept {
    return data_->models;
}
StringTables& SkinsRuntimeOwner::string_tables() noexcept {
    return data_->tables;
}
client::SkinRuntimeFactoryBindings& SkinsRuntimeOwner::factory_bindings() noexcept {
    return data_->factories;
}
SequenceOwnerBindings& SkinsRuntimeOwner::sequence_bindings() noexcept {
    return data_->sequence_factory;
}
SequenceRuntime& SkinsRuntimeOwner::sequence_state() noexcept {
    return data_->sequence;
}
SequenceReceivers& SkinsRuntimeOwner::sequence_receivers() noexcept {
    return data_->receivers;
}
NativeSequenceRuntime& SkinsRuntimeOwner::sequence_runtime() noexcept {
    return data_->sequence_update;
}
AgentModelReceiver& SkinsRuntimeOwner::agent_receiver() noexcept {
    return data_->agent;
}
void SkinsRuntimeOwner::bind_receive_patches(
    std::span<client::EntityUpdatePatch* const, 4> sequence,
    client::EntityUpdatePatch& agent) noexcept {
    bind_sequence_receive_runtime(data_->receivers, sequence);
    bind_agent_model_receiver(data_->agent, agent);
}
void SkinsRuntimeOwner::bind_receive_patches(const engine::BuildSkinReceivePatches& patches) {
    if (&patches.data() != &data_->storage.data())
        throw std::invalid_argument(
            "Skin receive patches require the same canonical runtime data owner");
    bind_receive_patches(patches.sequence(), patches.agent());
}
} //namespace skeet::skins
