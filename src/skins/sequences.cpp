#include "skeet/skins/sequences.hpp"
#include <cstring>
#include <xmmintrin.h>

namespace skeet::skins {
namespace {
std::int32_t selection(const Settings& settings, const SequenceEntity& player) {
    return player.team == 3 ? settings.knife_model_ct.value : settings.knife_model_t.value;
}
bool enabled(const Settings& settings, const SequenceEntity& player) {
    return (player.team == 3 ? settings.knife_ct.value : settings.knife_t.value) != 0;
}
bool same_owner(const SequenceEntity& entity, SequenceEngine& engine) {
    PlayerIdentity identity;
    return engine.local_identity(identity) && (identity.low || identity.high) &&
           entity.original_owner.low == identity.low && entity.original_owner.high == identity.high;
}
std::uint32_t sequence_hash(std::string_view value) {
    std::uint32_t hash = 0x811C9DC5;
    for (const auto ch : value)
        hash = (hash ^ std::uint8_t(ch)) * 0x1000193;
    return hash ^ 0x92CA5DF1;
}
std::int32_t weighted_sequence(SequenceEntity& entity, std::uint32_t seed, SequenceEngine& engine) {
    if (!engine.animation_interface_available())
        return -1;
    auto* random_seed = engine.prediction_random_seed();
    std::int32_t previous_seed = 0;
    if (random_seed) {
        previous_seed = *random_seed;
        *random_seed = std::int32_t(seed);
    }
    auto** prediction_player = engine.prediction_player();
    auto* local = engine.local_player();
    SequenceEntity* previous_player = nullptr;
    if (prediction_player && local) {
        previous_player = *prediction_player;
        *prediction_player = local;
    }
    const auto result = engine.select_weighted_sequence(entity, 213);
    if (prediction_player && local)
        *prediction_player = previous_player;
    if (random_seed)
        *random_seed = previous_seed;
    return result;
}
void remap(const Settings& settings, const SequenceRuntime& state, SequenceEntity& viewmodel,
           std::int32_t& model, std::int32_t& sequence, SequenceEngine& engine) {
    auto* local = engine.local_player();
    if (!local || engine.demo_playback() || engine.hltv() || !enabled(settings, *local))
        return;
    if (!state.knife_class || viewmodel.weapon_handle == 0xFFFFFFFF)
        return;
    auto* weapon = engine.entity_for_handle(viewmodel.weapon_handle);
    if (!weapon || !same_owner(*weapon, engine))
        return;
    const bool knife = weapon->class_id == state.knife_class;
    std::int32_t selected_model = -1;
    if (knife && engine.local_player()) {
        const auto index = selection(settings, *engine.local_player());
        if (std::uint32_t(index) >= state.models.size())
            return;
        selected_model = state.models[std::size_t(index)].entity_model;
        if (selected_model == -1)
            return;
        const auto original = model == -1 ? viewmodel.model_index : model;
        if (original != selected_model)
            model = selected_model;
    }
    const auto command = engine.acknowledged_command();
    const auto& history = state.history[command % 150];
    if (history.command != command || history.weapon_handle != viewmodel.weapon_handle ||
        history.animation_token_08 != viewmodel.animation_token_08)
        return;
    if (history.animation_token_0c == viewmodel.animation_token_0c &&
        (!knife || history.selected_model == selected_model)) {
        sequence = history.sequence;
        return;
    }
    local = engine.local_player();
    if (!local || local->dead)
        return;
    if (local->weighted_activity) {
        auto mapped = weighted_sequence(viewmodel, history.random_seed, engine);
        if (mapped == -1 && engine.animation_interface_available())
            mapped = engine.lookup_sequence(viewmodel, state.inspection_sequence);
        if (mapped != -1)
            sequence = mapped;
        return;
    }
    if (!knife || sequence < 0)
        return;
    const auto index = selection(settings, *local);
    const auto& target = state.models[std::size_t(index)];
    if (target.entity_model == state.original_model || state.original_model == -1)
        return;
    const auto* source = engine.model_sequences(state.original_model);
    if (!source || sequence >= std::int32_t(source->descriptions.size()))
        return;
    const auto& description = source->descriptions[std::size_t(sequence)];
    const auto* destination = engine.model_sequences(target.entity_model);
    if (!destination)
        return;
    const auto count = destination->descriptions.size();
    if (!description.name.empty()) {
        std::array<char, 64> name{};
        ::strcpy_s(name.data(), name.size(), description.name.data());
        const auto variants = knife_models()[std::size_t(index)].native_word_08;
        if (variants > 1 && sequence_hash(description.name) == 0xD4A1BEA9) {
            const auto ticks = weapon->sequence_time / engine.interval_per_tick() + 0.5f;
            engine.random_seed(_mm_cvttss_si32(_mm_set_ss(ticks)));
            name[7] = char(std::uint8_t(engine.random_integer(1, std::int32_t(variants))) + 0x30);
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (::_stricmp(destination->descriptions[i].name.data(), name.data()) == 0) {
                sequence = std::int32_t(i);
                return;
            }
        }
    }
    if (description.activity != -1) {
        for (std::size_t i = 0; i < count; ++i) {
            if (destination->descriptions[i].activity == description.activity) {
                sequence = std::int32_t(i);
                return;
            }
        }
    }
}
}
void update_sequences(const Settings& settings, SequenceRuntime& state,
                      std::span<const InventoryEntry> inventory, SequenceEngine& engine) {
    for (const auto& pending : state.viewmodels) {
        if (pending.handle == 0xFFFFFFFF)
            continue;
        auto* entity = engine.entity_for_handle(pending.handle);
        if (!entity)
            continue;
        std::int32_t model = pending.model, sequence = pending.sequence;
        remap(settings, state, *entity, model, sequence, engine);
        if (model != -1)
            engine.receive_model_index(*entity, model);
        if (sequence != -1)
            engine.receive_sequence(*entity, sequence);
    }
    state.viewmodels.clear();
    auto* player = engine.inventory_player();
    auto* local = engine.local_player();
    if (player && !player->dead && local && !engine.demo_playback() && !engine.hltv() &&
        enabled(settings, *local)) {
        for (const auto& entry : inventory) {
            auto* entity = engine.inventory_entity(entry.handle);
            if (!entity || !entity->class_id)
                continue;
            if (entity->class_id == state.knife_class) {
                if (!same_owner(*entity, engine))
                    continue;
                const auto index = selection(settings, *player);
                if (std::uint32_t(index) >= state.models.size())
                    continue;
                const auto& model = state.models[std::size_t(index)];
                if (!entry.tag) {
                    state.original_model = entity->model_index;
                    state.original_definition = entity->definition;
                }
                entity->definition = knife_models()[std::size_t(index)].definition;
                entity->entity_model = model.entity_model;
                entity->view_model = model.view_model;
                entity->world_model = model.world_model;
                entity->model_kind = 3;
                if (model.entity_model == -1 || model.entity_model == entity->model_index)
                    continue;
                engine.begin_model_cache_lock();
                engine.begin_model_cache_lock();
                engine.set_model_index(*entity, model.entity_model);
                engine.end_model_cache_lock();
                engine.end_model_cache_lock();
                if (entry.tag)
                    engine.schedule_full_update(engine.current_time() + 0.25f);
            } else if (entity->class_id == state.weapon_world_model_class) {
                local = engine.local_player();
                if (!local)
                    continue;
                const auto index = selection(settings, *local);
                if (std::uint32_t(index) >= state.models.size() ||
                    entity->owner_handle == 0xFFFFFFFF ||
                    std::uint16_t(entity->owner_handle) !=
                        std::uint32_t(engine.local_player_index()) ||
                    entity->weapon_handle == 0xFFFFFFFF)
                    continue;
                auto* weapon = engine.entity_for_handle(entity->weapon_handle);
                if (!weapon || weapon->class_id != state.knife_class ||
                    !same_owner(*weapon, engine))
                    continue;
                const auto model = state.models[std::size_t(index)].view_model;
                if (model == -1 || model == entity->model_index ||
                    !engine.has_viewmodel_model_proxy())
                    continue;
                engine.receive_viewmodel_model(*entity, model);
                entity->model_index = std::int16_t(model);
            }
        }
    }
    for (const auto& pending : state.weapon_handles) {
        if (pending.handle == 0xFFFFFFFF)
            continue;
        auto* entity = engine.entity_for_handle(pending.handle);
        if (entity)
            engine.receive_weapon_handle(*entity, pending.value);
    }
    state.weapon_handles.clear();
}
} // namespace skeet::skins
