#include "skeet/rage/feedback_runtime_owner.hpp"
#include "skeet/rage/pending_shot_publication.hpp"
#include "skeet/rage/spread_storage.hpp"
#include "skeet/client/runtime_storage.hpp"
#include "skeet/client/runtime_composition.hpp"
#include "skeet/client/history_runtime_owner.hpp"
#include "skeet/client/player_runtime_owner.hpp"
#include "skeet/animation/runtime_owner.hpp"
#include "skeet/engine/scene_runtime_owner.hpp"
#include "skeet/engine/decoded_offset.hpp"
#include "skeet/engine/netvar_initialization.hpp"
#include "skeet/engine/build_hitbox_clip_bindings.hpp"
#include "skeet/esp/runtime_owner.hpp"
#include "skeet/script/api_runtime_owner.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace skeet::rage {
namespace {
using client::ClientRuntimeStorage;
template <class T> T read(const void* value, std::ptrdiff_t at) {
    T result;
    std::memcpy(&result, static_cast<const std::byte*>(value) + at, sizeof result);
    return result;
}
std::ptrdiff_t field(ClientRuntimeStorage& s, std::uint32_t hash) {
    const auto& globals = s.offsets().globals;
    for (const auto& recipe : engine::build_receive_offset_recipes())
        if (recipe.initial_xor_key == hash)
            return engine::DecodedOffset(globals, recipe.pointer_identity, recipe.xor_key_identity,
                                         recipe.initial_xor_key, 0);
    throw std::invalid_argument("Feedback field has no recovered receive-offset recipe");
}
engine::OwnerRegistryBinding registry(ClientRuntimeStorage& s, std::uint32_t identity) {
    return {s.owner_registries().at(identity), s.memory()};
}
ClientRuntimeStorage& checked(ClientRuntimeStorage& s, const client::EngineRuntimeBindings& e,
                              client::ConfigurationRuntimeOwner& c, client::HistoryRuntimeOwner& h,
                              engine::SceneQueryRuntimeOwner& q, animation::RuntimeOwner& b,
                              client::CrtRuntimeOwner& crt, esp::EspRuntimeOwner& esp,
                              script::ScriptApiRuntimeOwner& scripts) {
    if (&e.variables.data() != &s.data() || &c.storage() != &s || &h.storage() != &s ||
        &q.storage() != &s || &q.players() != &h.players() || &b.storage() != &s ||
        &b.players() != &h.players().cache() || &crt.storage() != &s || &esp.storage() != &s ||
        &scripts.storage() != &s)
        throw std::invalid_argument(
            "Feedback requires the canonical engine, configuration, history, scene, animation, CRT, ESP and Lua API owners");
    for (const auto identity : {0x4346A514u, 0x4346A890u, 0x4346A614u, 0x4346A334u, 0x4346A338u,
                                0x43467B04u, 0x43475B2Cu, 0x4346A894u})
        if (!s.data().native_cell<void*>(identity))
            throw std::invalid_argument("Feedback engine publication is incomplete");
    for (const auto identity :
         {0x43468520u, 0x434670D0u, 0x4346747Cu, 0x4346A748u, 0x4346A330u, 0x43468EA4u})
        if (!e.variables.at(identity).get())
            throw std::invalid_argument("Feedback requires its acquired live ConVars");
    if (s.data().native_cell<FeedbackOwner*>(0x43475A7Cu))
        throw std::logic_error("Feedback runtime must precede its actual feature factory");
    engine::initialize_build_hitbox_clip_bindings(s.data());
    engine::initialize_build_pose_range_bindings(s.data());
    return s;
}
class LiveFeedbackEngine final : public ShotFeedbackEngine {
    ClientRuntimeStorage& storage_;
    RageBoxClipper clipper_;
    std::ptrdiff_t hidden_, weapon_handle_, ammunition_, weapon_shot_, player_injury_, player_time_;
    std::vector<RageLocalHitbox> boxes_;
    template <class T> static T read_field(void* object, std::ptrdiff_t at) {
        return read<T>(object, at);
    }
    static bool read_hidden(void* object, std::ptrdiff_t at) {
        return read<std::uint8_t>(object, at) != 0;
    }
    static std::int32_t read_health(void* object, std::ptrdiff_t) {
        return engine::invoke<std::int32_t>(object, 122);
    }

public:
    LiveFeedbackEngine(ClientRuntimeStorage& s, RageBoxClipper clipper)
        : storage_(s), clipper_(clipper), hidden_(field(s, 0x9D920E35u)),
          weapon_handle_(field(s, 0xCD812654u)), ammunition_(field(s, 0xCA5EC698u)),
          weapon_shot_(field(s, 0x0176A381u)), player_injury_(field(s, 0xAE838771u)),
          player_time_(field(s, 0xD5FD2E2Cu)) {}
    std::int32_t current_command() override {
        return engine::invoke<std::int32_t>(storage_.data().native_cell<void*>(0x4346A514u), 205);
    }
    std::int32_t local_player_index() override {
        auto* local = storage_.data().native_cell<std::byte*>(0x43475808u);
        return engine::invoke<std::int32_t>(local + 8, 10);
    }
    float engine_time() override {
        return engine::invoke<float>(storage_.data().native_cell<void*>(0x4346A514u), 14);
    }
    ShotFeedbackEntity local_player() override {
        ShotFeedbackEntity result;
        auto* local = storage_.data().native_cell<void*>(0x43475808u);
        result.exists = local != nullptr;
        if (local) {
            result.hidden = ShotFeedbackEntityField<bool>::live(local, hidden_, read_hidden);
            result.weapon_handle = ShotFeedbackEntityField<std::uint32_t>::live(
                local, weapon_handle_, read_field<std::uint32_t>);
        }
        return result;
    }
    ShotFeedbackEntity entity_by_index(std::int32_t index) override {
        ShotFeedbackEntity result;
        auto* player =
            engine::invoke<void*>(storage_.data().native_cell<void*>(0x4346A890u), 3, index);
        result.exists = player != nullptr;
        if (player) {
            result.field_d5fd2e2c =
                ShotFeedbackEntityField<float>::live(player, player_time_, read_field<float>);
            result.health = ShotFeedbackEntityField<std::int32_t>::live(player, 0, read_health);
            result.last_shot_time =
                ShotFeedbackEntityField<float>::live(player, player_injury_, read_field<float>);
        }
        return result;
    }
    ShotFeedbackEntity entity_by_handle(std::uint32_t handle) override {
        ShotFeedbackEntity result;
        auto* weapon =
            engine::invoke<void*>(storage_.data().native_cell<void*>(0x4346A890u), 4, handle);
        result.exists = weapon != nullptr;
        if (weapon) {
            result.life_id = ShotFeedbackEntityField<std::int32_t>::live(weapon, ammunition_,
                                                                         read_field<std::int32_t>);
            result.simulation_time =
                ShotFeedbackEntityField<float>::live(weapon, weapon_shot_, read_field<float>);
        }
        return result;
    }
    std::span<const RageLocalHitbox> hitboxes(std::int32_t index, std::int32_t set_index) override {
        boxes_.clear();
        auto* info = storage_.data().native_cell<void*>(0x4346A614u);
        const auto* model = engine::invoke<const void*>(info, 1, index);
        if (!model)
            return boxes_;
        const auto* header = engine::invoke<const std::byte*>(info, 32, model);
        if (!header)
            return boxes_;
        const auto* set =
            header + read<std::int32_t>(header, 0xB0) + std::ptrdiff_t(set_index) * 12;
        const auto count = (std::min)(read<std::uint32_t>(set, 4), 19u);
        const auto* first = set + read<std::int32_t>(set, 8);
        boxes_.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto* descriptor = first + i * 0x44;
            boxes_.push_back({read<std::int32_t>(descriptor, 4), read<RageVector>(descriptor, 8),
                              read<RageVector>(descriptor, 0x14),
                              read<RageVector>(descriptor, 0x24), read<float>(descriptor, 0x30),
                              descriptor});
        }
        return boxes_;
    }
    int clip_hitbox(const RageRay& ray, const RageLocalHitbox& hitbox, const Matrix3x4& matrix,
                    RageTrace& trace) override {
        RageScanHitbox selected{hitbox.hitgroup, {}, hitbox.engine_hitbox, matrix};
        return clipper_.clip(clipper_.context, ray, selected, trace);
    }
};
}
struct FeedbackRuntimeOwner::Data {
    ClientRuntimeStorage& storage;
    engine::PoseRangeRuntime pose;
    RageBoxClipper clipper;
    resolver_runtime::RawAttemptEngine attempts;
    LiveFeedbackEngine engine;
    ShotFeedbackRuntime runtime;
    FeedbackEntityBindings live;
    FeedbackPacketBindings packet;
    PendingShotPublicationBindings pending;
    FeedbackOwnerBindings factory;
    Data(ClientRuntimeStorage& s, const client::EngineRuntimeBindings& e,
         client::ConfigurationRuntimeOwner& c, client::HistoryRuntimeOwner& h,
         engine::SceneQueryRuntimeOwner& q, animation::RuntimeOwner& b,
         client::CrtRuntimeOwner& crt, esp::EspRuntimeOwner& esp,
         script::ScriptApiRuntimeOwner& scripts)
        : storage(checked(s, e, c, h, q, b, crt, esp, scripts)), pose(s.data()),
          clipper(engine::native_hitbox_clipper(s.data())),
          attempts{s.data().native_cell<void*>(0x4346A890u),
                   s.data().native_cell<float*>(0x4346A334u),
                   h.target_offsets().rejection_bytes[0],
                   std::ptrdiff_t(h.entity_runtime().offsets.animation_state),
                   b.bones().fields.studio_wrapper_32e4daf1,
                   b.bones().fields.pose_parameters,
                   b.bones().fields.hitbox_set_c2b6fb10,
                   pose,
                   clipper},
          engine(s, clipper), runtime{q.queries().ballistics(),
                                      engine,
                                      attempts,
                                      b.trigonometry(),
                                      h.players().cache(),
                                      h.players().adjustments(),
                                      c.rage_settings().shot_log,
                                      scripts.normal_log()},
          live{c.rage_settings().global.anti_aim_correction.value,
               s.data().native_cell<const float*>(0x4346A334u),
               s.data().native_cell<const float*>(0x4346A338u),
               s.data().native_cell<const void*>(0x43467B04u),
               s.data().native_cell<std::uint32_t>(0x4346747Cu),
               s.data().native_cell<std::uint32_t>(0x4346A748u),
               s.data().native_cell<std::uint32_t>(0x4346A330u),
               s.data().native_cell<std::uint32_t>(0x43468EA4u),
               s.data().native_cell<float>(0x43469254u),
               s.data().native_cell<float>(0x43476928u),
               c.settings_menu().maximum_unlag,
               c.rage_settings().global.experimental(ExperimentalRageOption::resolver)},
          packet{c.rage_settings().global.enabled.value,
                 c.rage_settings().global.anti_aim_correction.value,
                 s.data().native_cell<void*>(0x4346A894u),
                 e.variables.at(0x43468520u),
                 feedback_packet_offsets(s.offsets()),
                 crt.spread().tables(),
                 crt.random(),
                 esp.shotgun_pattern(),
                 b.trigonometry()},
          pending{
              s.data().native_cell<FeedbackOwner*>(0x43475A7Cu),
              c.rage_settings().global.anti_aim_correction.value,
              h.players().cache(),
              s.data().native_cell<void*>(0x4346A514u),
              s.data().native_cell<void*>(0x4346A890u),
              s.data().native_cell<void*>(0x43467B54u),
              e.variables.at(0x434670D0u),
              {field(s, 0xCA5EC698u), field(s, 0x47877DEBu), b.bones().fields.hitbox_set_c2b6fb10},
              s.data().native_cell<float*>(0x4346A334u),
              s.data().native_cell<float*>(0x43467EB8u),
              s.data().native_cell<float*>(0x4346A338u),
              &s.data().native_cell<std::uint32_t>(0x434759C0u),
              attempts,
              c.rage_settings().global.experimental(ExperimentalRageOption::resolver),
              {&c.rage_settings().global.jitter_fix.value,
               &c.rage_settings().global.experimental_rage.value}},
          factory{registry(s, 0x43475B78u),
                  s.command_phases(),
                  s.entity_observers(),
                  registry(s, 0x4347584Cu),
                  registry(s, 0x43475AD8u),
                  registry(s, 0x43475930u),
                  s.recharge_observers(),
                  s.configuration(),
                  s.event_registry(),
                  c.rage_settings().shot_log,
                  h.feedback_state(),
                  runtime,
                  live,
                  packet,
                  *s.data().native_cell<std::int32_t*>(0x43475B2Cu),
                  s.data().native_cell<void*>(0x4346A514u),
                  s.data().native_cell<FeedbackOwner*>(0x43475A7Cu)} {
        runtime.resolver_rework = c.rage_settings().global.experimental(ExperimentalRageOption::resolver);
    }
};
FeedbackRuntimeOwner::FeedbackRuntimeOwner(
    ClientRuntimeStorage& s, const client::EngineRuntimeBindings& e,
    client::ConfigurationRuntimeOwner& c, client::HistoryRuntimeOwner& h,
    engine::SceneQueryRuntimeOwner& q, animation::RuntimeOwner& b, client::CrtRuntimeOwner& crt,
    esp::EspRuntimeOwner& esp, script::ScriptApiRuntimeOwner& scripts)
    : data_(std::make_unique<Data>(s, e, c, h, q, b, crt, esp, scripts)) {}
FeedbackRuntimeOwner::~FeedbackRuntimeOwner() = default;
ClientRuntimeStorage& FeedbackRuntimeOwner::storage() const noexcept {
    return data_->storage;
}
ShotFeedbackRuntime& FeedbackRuntimeOwner::runtime() noexcept {
    return data_->runtime;
}
FeedbackOwnerBindings& FeedbackRuntimeOwner::factory_bindings() noexcept {
    return data_->factory;
}
const PendingShotPublicationBindings& FeedbackRuntimeOwner::pending_shots() const noexcept {
    return data_->pending;
}
void FeedbackRuntimeOwner::bind_initializer(client::NativeStartupSources& sources) {
    bind_feedback_owner_factory(sources, data_->factory);
}
} //namespace skeet::rage
