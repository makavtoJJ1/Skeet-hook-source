#include "skeet/rage/runtime_owner.hpp"
#include "skeet/rage/damage_model.hpp"
#include "skeet/rage/post_firing.hpp"
#include "skeet/rage/final_target_context.hpp"
#include "skeet/rage/shot_publication.hpp"
#include "skeet/rage/spread_storage.hpp"
#include "skeet/rage/buffered_shot_repair.hpp"
#include "skeet/client/runtime_storage.hpp"
#include "skeet/client/runtime_composition.hpp"
#include "skeet/client/history_runtime_owner.hpp"
#include "skeet/client/player_runtime_owner.hpp"
#include "skeet/client/command_capture_runtime_owner.hpp"
#include "skeet/client/command_checksum.hpp"
#include "skeet/client/movement_hud_runtime_owner.hpp"
#include "skeet/client/target_refresh_runtime_owner.hpp"
#include "skeet/client/visual_runtime_owner.hpp"
#include "skeet/client/prediction_history_owner.hpp"
#include "skeet/client/native_startup.hpp"
#include "skeet/animation/runtime_owner.hpp"
#include "skeet/engine/decoded_offset.hpp"
#include "skeet/engine/netvar_initialization.hpp"
#include "skeet/engine/scene_runtime_owner.hpp"
#include "skeet/engine/build_hitbox_clip_bindings.hpp"
#include "skeet/esp/runtime_owner.hpp"
#include "skeet/esp/owner_runtime.hpp"
#include "skeet/esp/other_config.hpp"
#include "skeet/effects/runtime_owner.hpp"
#include "skeet/lagcomp/native_history_boundaries.hpp"
#include "skeet/lagcomp/history_runtime.hpp"
#include "skeet/legit/owner.hpp"
#include "skeet/movement/rage_stop.hpp"
#include "skeet/script/source_runtime_owner.hpp"
#include <bit>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace skeet::rage {
namespace {
using client::ClientRuntimeStorage;
template <class T> T read(const void* object, std::ptrdiff_t at) {
    T result;
    std::memcpy(&result, static_cast<const std::byte*>(object) + at, sizeof result);
    return result;
}
struct DerivedField {
    std::uint32_t hash, pointer, key;
};
constexpr std::array derived_fields{DerivedField{0xB8969256u, 0x43467430u, 0x4346A718u},
                                    DerivedField{0x3D52A1A7u, 0x43467F4Cu, 0x4346A8B0u},
                                    DerivedField{0x1CAAF0A8u, 0x43467D78u, 0x43467B80u},
                                    DerivedField{0x17978090u, 0x43469064u, 0x4346A484u},
                                    DerivedField{0x2A671B42u, 0x43468D30u, 0x43468ED4u},
                                    DerivedField{0x3B1F6ECCu, 0x43468368u, 0x43467FACu},
                                    DerivedField{0x54783141u, 0x43468E7Cu, 0x434674A8u}};
std::ptrdiff_t field(ClientRuntimeStorage& s, std::uint32_t hash) {
    const auto& offsets = s.offsets().globals;
    for (const auto& row : derived_fields)
        if (row.hash == hash)
            return engine::DecodedOffset(offsets, row.pointer, row.key, row.hash, 0);
    for (const auto& row : engine::build_receive_offset_recipes())
        if (row.initial_xor_key == hash)
            return engine::DecodedOffset(offsets, row.pointer_identity, row.xor_key_identity,
                                         row.initial_xor_key, 0);
    for (const auto& row : engine::build_data_map_offset_recipes())
        if (row.initial_xor_key == hash)
            return engine::DecodedOffset(offsets, row.pointer_identity, row.xor_key_identity,
                                         row.initial_xor_key, 0);
    throw std::invalid_argument("RAGE field has no recovered receive/datamap/derived recipe");
}
template <class T> T& pointed(ClientRuntimeStorage& s, std::uint32_t identity) {
    auto* pointer = s.data().native_cell<T*>(identity);
    if (!pointer)
        throw std::invalid_argument("RAGE requires its actual published engine alias");
    return *pointer;
}
std::int32_t convar_integer(const void* variable) {
    for (;;) {
        const auto parent = read<const void*>(variable, 0x1C);
        if (parent == variable)
            break;
        variable = parent;
    }
    return std::bit_cast<std::int32_t>(read<std::uint32_t>(variable, 0x30) ^
           std::uint32_t(reinterpret_cast<std::uintptr_t>(variable)));
}
bool convar_boolean(const void* variable) {
    return convar_integer(variable) != 0;
}
engine::OwnerRegistryBinding registry(ClientRuntimeStorage& s, std::uint32_t identity) {
    return {s.owner_registries().at(identity), s.memory()};
}
ClientRuntimeStorage&
checked(ClientRuntimeStorage& s, const client::EngineRuntimeBindings& e,
        client::ConfigurationRuntimeOwner& c, client::HistoryRuntimeOwner& h,
        engine::SceneQueryRuntimeOwner& q, animation::RuntimeOwner& b,
        client::CommandCaptureRuntimeOwner& capture, client::MovementHudRuntimeOwner& m,
        client::TargetRefreshRuntimeOwner& refresh, client::CrtRuntimeOwner& crt,
        esp::EspRuntimeOwner& esp, effects::EffectsRuntimeOwner& effects,
        client::VisualRuntimeOwner& visual, script::SourceScriptRuntimeOwner& scripts) {
    if (&e.variables.data() != &s.data() || &c.storage() != &s || &h.storage() != &s ||
        &q.storage() != &s || &q.players() != &h.players() || &b.storage() != &s ||
        &b.players() != &h.players().cache() || &capture.storage() != &s || &m.storage() != &s ||
        &refresh.storage() != &s || &crt.storage() != &s || &esp.storage() != &s ||
        &effects.storage() != &s || &visual.storage() != &s || &scripts.storage() != &s)
        throw std::invalid_argument(
            "RAGE must share the canonical engine, configuration, history, scene, animation, command, movement, CRT, ESP, effects, HUD and Lua owners");
    for (const auto identity : {0x4346A338u, 0x43467EB8u, 0x4346A334u, 0x43469300u, 0x43475B2Cu,
                                0x434757DCu, 0x43467B04u, 0x4346A894u, 0x43475B90u, 0x43469264u,
                                0x43467118u, 0x434685FCu, 0x4346A614u, 0x43469330u})
        if (!s.data().native_cell<void*>(identity))
            throw std::invalid_argument("RAGE engine/interface publication is incomplete");
    for (const auto identity :
         {0x43469028u, 0x43468EA4u, 0x43468520u, 0x43467A78u, 0x43467C3Cu, 0x43467084u, 0x43468C90u,
          0x434670D0u, 0x43467E4Cu, 0x43467A54u, 0x43467FA0u, 0x43467990u, 0x4346A88Cu, 0x43468FB4u,
          0x43468F4Cu, 0x434674E4u})
        if (!e.variables.at(identity).get())
            throw std::invalid_argument("RAGE requires its genuinely acquired ConVars");
    if (s.data().native_cell<LifecycleOwner*>(0x43475810u))
        throw std::logic_error("RAGE source context must precede its feature factory");
    // These adapters inspect the already loaded supported engine image; they
    // execute no engine function and publish no captured client address.
    engine::initialize_build_hitbox_clip_bindings(s.data());
    engine::initialize_build_pose_range_bindings(s.data());
    client::initialize_source_command_checksum(s.data());
    return s;
}
PreambleOffsets preamble_fields(ClientRuntimeStorage& s) {
    return {field(s, 0xB8969256u), field(s, 0xCA5EC698u), field(s, 0x49C770CAu),
            field(s, 0x5C9F0FBBu), field(s, 0xF71CE331u), field(s, 0xDE530A8Bu),
            field(s, 0xD2563274u), field(s, 0xD720AEA6u), field(s, 0xB538BBA5u),
            field(s, 0x1CAAF0A8u)};
}
FiringPlayerOffsets firing_fields(ClientRuntimeStorage& s) {
    return {field(s, 0x38CA5228u),
            engine::DecodedOffset(s.offsets().globals, 0x4346757Cu, 0x4346707Cu, 0x043FA575u, 0),
            field(s, 0x334B460Cu),
            field(s, 0x3D52A1A7u),
            field(s, 0x32E4DAF1u),
            field(s, 0xC2B6FB10u),
            field(s, 0x1CAAF0A8u),
            field(s, 0xE2FBE695u)};
}
ShotModelOffsets shot_model_fields(ClientRuntimeStorage& s) {
    return {field(s, 0x32E4DAF1u), field(s, 0x47877DEBu), field(s, 0x38CA5228u),
            field(s, 0x8481EA04u), field(s, 0x57B2D535u), field(s, 0xC2B6FB10u),
            field(s, 0x950F7B20u), field(s, 0x5173A531u), field(s, 0xA25D8271u),
            field(s, 0x2A671B42u), field(s, 0x3B1F6ECCu), field(s, 0xFC2723F7u),
            field(s, 0x688E0292u), field(s, 0x4999428Du)};
}
}

struct RuntimeOwner::Data final : CommandSource {
    ClientRuntimeStorage& storage;
    const client::EngineRuntimeBindings& engine;
    client::ConfigurationRuntimeOwner& configuration;
    client::HistoryRuntimeOwner& history;
    engine::SceneQueryRuntimeOwner& scene;
    animation::RuntimeOwner& animation;
    client::CommandCaptureRuntimeOwner& capture_owner;
    client::MovementHudRuntimeOwner& movement;
    client::TargetRefreshRuntimeOwner& refresh;
    client::CrtRuntimeOwner& crt;
    esp::EspRuntimeOwner& esp;
    effects::EffectsRuntimeOwner& effects;
    client::VisualRuntimeOwner& visual;
    script::SourceScriptRuntimeOwner& scripts;
    const CommandFrameInputs* explicit_inputs;
    InputBitmap input;
    ui::SettingsMenuValues menu;
    PreambleState state_{};
    DamageRulesCache damage_rules;
    AcquisitionStorage acquisition_{};
    std::int32_t shot_index_ = 0;
    esp::HitboxHighlightState highlight_{};
    FiringSharedState shared;
    engine::JobDispatcher jobs;
    engine::EyePoseParameterRuntime eye_pose;
    engine::PoseRangeRuntime pose_range;
    RageBoxClipper clipper;
    lagcomp::RotationMath rotation;
    esp::HitboxHighlightEngineContext highlight_context;
    OwnerBindings factory;

    Data(ClientRuntimeStorage& s, const client::EngineRuntimeBindings& e,
         client::ConfigurationRuntimeOwner& c, client::HistoryRuntimeOwner& h,
         engine::SceneQueryRuntimeOwner& q, animation::RuntimeOwner& b,
         client::CommandCaptureRuntimeOwner& capture, client::MovementHudRuntimeOwner& m,
         client::TargetRefreshRuntimeOwner& target, client::CrtRuntimeOwner& process,
         esp::EspRuntimeOwner& esp_owner, effects::EffectsRuntimeOwner& effects_owner,
         client::VisualRuntimeOwner& visual_owner, script::SourceScriptRuntimeOwner& lua,
         const CommandFrameInputs* inputs, RuntimeImports imports)
        : storage(checked(s, e, c, h, q, b, capture, m, target, process, esp_owner, effects_owner,
                          visual_owner, lua)),
          engine(e), configuration(c), history(h), scene(q), animation(b), capture_owner(capture),
          movement(m), refresh(target), crt(process), esp(esp_owner), effects(effects_owner),
          visual(visual_owner), scripts(lua), explicit_inputs(inputs),
          input(s.input_runtime().bitmap()), menu(c.settings_menu().values()),
          shared(FiringSharedState::Live{}, s.data().native_cell<std::int32_t>(0x434759CCu),
                 s.data().native_cell<RageVector>(0x434759D0u),
                 s.data().native_cell<std::int32_t>(0x434759C8u)),
          jobs(imports.thread_pool, s.memory()), eye_pose(s.data()), pose_range(s.data()),
          clipper(engine::native_hitbox_clipper(s.data())),
          rotation{b.trigonometry().sine, b.trigonometry().cosine,
                   lagcomp::native_history_numeric_bindings().arccosine,
                   animation::native_sse_atan2},
          highlight_context{esp_owner.active(),
                            s.offsets(),
                            s.data().native_cell<std::uint8_t>(0x4346A65Cu),
                            visual_owner.projection(),
                            s.data().native_cell<RageVector>(0x43479CCCu),
                            b.trigonometry(),
                            h.players().adjustments()},
          factory{registry(s, 0x43475B78u),
                  s.command_phases(),
                  registry(s, 0x43475A9Cu),
                  s.entity_observers(),
                  s.configuration(),
                  c.rage_settings().aimbot,
                  c.player_esp_settings().aimbot_visualization,
                  *this,
                  {lua.bridge(), c.rage_settings().global, c.rage_settings().aimbot,
                   c.other_esp_settings().feature_indicators, input, visual_owner.indicators(),
                   c.player_esp_settings().aimbot_visualization, highlight_, highlight_context},
                  {s.data().native_cell<void*>(0x4346A514u),
                   s.data().native_cell<void*>(0x4346A890u),
                   s.data().native_cell<void*>(0x4346A614u),
                   s.data().native_cell<std::int32_t*>(0x43469300u),
                   s.data().native_cell<void*>(0x43467478u),
                   {field(s, 0x7501CB91u), field(s, 0x8D1BD8D0u), field(s, 0x47877DEBu),
                    field(s, 0x9BB80163u)}}} {}

    PreambleState& state() noexcept override {
        return state_;
    }
    AcquisitionStorage& acquisition() noexcept override {
        return acquisition_;
    }
    std::int32_t& shot_index() noexcept override {
        return shot_index_;
    }
    esp::HitboxHighlightState& action_state() noexcept override {
        return highlight_;
    }
    std::array<animation::AnimationAngleSample, 2>& pending_angles() noexcept override {
        return storage.data().native_cell<std::array<animation::AnimationAngleSample, 2>>(
            0x434759DCu);
    }
    PredictionMaskOwner* const& prediction_masks() const noexcept override {
        return storage.data().native_cell<PredictionMaskOwner*>(0x43475A58u);
    }
    void require_factory(const AimbotSettings& a, const GlobalSettings& g,
                         const esp::AimbotVisualizationConfig& v,
                         const esp::HitboxHighlightState& h) const override {
        if (&a != &configuration.rage_settings().aimbot ||
            &g != &configuration.rage_settings().global ||
            &v != &configuration.player_esp_settings().aimbot_visualization || &h != &highlight_)
            throw std::invalid_argument(
                "RAGE factory must retain this source owner's actual configuration and highlight state");
    }
    CommandPhase2Bindings phase2_bindings() const {
        auto& s = storage;
        auto& data = s.data();
        return {configuration.rage_settings().global,
                input,
                data.native_cell<std::int32_t>(0x4347C9ECu),
                engine.variables.at(0x43468EA4u),
                data.native_cell<void**>(0x4346A3A4u),
                pointed<float>(s, 0x4346A338u),
                {capture_owner.firing().offsets, field(s, 0x3D10B94Cu), field(s, 0x49C770CAu)}};
    }
    CommandPhase6Bindings phase6_bindings() {
        return {storage.data().native_cell<const void**>(0x4346A3A4u), field(storage, 0xD720AEA6u),
                field(storage, 0x5C9F0FBBu), pending_angles()};
    }
    void phase2(void* player, void* weapon, client::Command& command, std::array<float, 3>& angles,
                std::uint8_t& send) override {
        process_rage_command_phase2(state_, player, weapon, command, angles, send,
                                    phase2_bindings());
    }
    void phase6(const client::CommandFrame& frame, const client::Command& command) override {
        process_rage_phase6(frame, command, phase6_bindings());
    }
    void capture(const client::CommandFrame& frame) noexcept override {
        capture_rage_command_frame(state_, frame);
    }
    struct Frame;
    void process(client::CommandFrame&, void*, client::Command&, std::uint8_t&) override;
};

// All physical views below live on this invocation's stack. The acquisition
// owner synchronously drains its jobs before Frame is destroyed.
struct RuntimeOwner::Data::Frame {
    Data& owner;
    CommandFrameInputs inputs;
    lagcomp::EntityPreparationLiveGlobals preparation_globals;
    lagcomp::HistoryTickLiveGlobals selection_globals;
    lagcomp::HistoryTickTiming timing;
    lagcomp::EntityPreparationContext preparation;
    TargetBindings targeting;
    anti_aim::RawTargetTraceEntities trace_entities;
    lagcomp::RecordAttachmentContext attachments;
    lagcomp::RecordSelectionContext selected_records;
    MovementExtrapolationBindings movement_extrapolation;
    lagcomp::ExtrapolatedRecordBindings extrapolation;
    lagcomp::HistoryAlternativeBindings alternative;
    lagcomp::HistorySelectionBindings history;
    PreambleBindings preamble;
    TargetPreparationBindings target_preparation;
    RecordCandidateBindings candidates;
    std::array<RageSpreadSample, 32> samples;
    ExperimentalDamageContext damage_context;
    ScanContext scanner;
    BodyAimProbeContext body_probe;
    TargetPredictionBindings prediction;
    AcquisitionRecordBindings records;
    SingleTargetBindings single;
    MultipleTargetBindings multiple;
    FinalTargetContextBindings final_target;
    FiringReadinessBindings readiness;
    FiringPostAngleBindings post_angles;
    PostFiringMovementBindings post_movement;
    ShotDiagnosticBindings diagnostics;
    resolver_runtime::RawAttemptEngine attempts;
    PendingShotPublicationBindings pending;
    ShotModelPublicationBindings shot_models;
    RageStopBindings stop;
    PostFiringBindings post_firing;
    ActionStateBindings action;
    client::CommandButtonPolicy button_policy;
    PreambleTailBindings tail;
    CommandPhase2Bindings phase2;
    CommandPhase6Bindings phase6;
    CommandOwner command;

    Frame(Data&, client::CommandFrame&, void*, client::Command&);
    RuntimeFrameBindings views() const noexcept {
        return {targeting, candidates, selected_records, extrapolation};
    }
};

namespace {
void* shot_registration_receiver(void* entity_list) {
    const auto* table = read<const std::byte*>(entity_list, 0);
    const auto* locator = read<const std::byte*>(table, -4);
    return static_cast<std::byte*>(entity_list) - read<std::uint32_t>(locator, 4);
}
PreambleAnimationHistory animation_history(ClientRuntimeStorage& s) {
    auto* active = s.data().native_cell<client::PredictionHistoryOwner*>(0x434759B4u);
    return PreambleAnimationHistory(
        reinterpret_cast<const std::byte*>(client::prediction_history_snapshots(active)));
}
}

RuntimeOwner::Data::Frame::Frame(Data& d, client::CommandFrame& frame, void* player,
                                 client::Command&)
    : owner(d),
      preparation_globals{d.storage.data().native_cell<void*>(0x43467B04u),
                          d.storage.data().native_cell<float*>(0x43467EB8u),
                          d.storage.data().native_cell<float*>(0x4346A334u),
                          d.storage.data().native_cell<lagcomp::TickTimingSamples>(0x43478C70u)},
      selection_globals{d.storage.data().native_cell<float*>(0x43467EB8u),
                        d.storage.data().native_cell<float>(0x43476928u),
                        d.storage.data().native_cell<float*>(0x4346A338u), d.menu.maximum_unlag},
      timing{pointed<float>(d.storage, 0x43467EB8u),
             read<float>(&frame, 0x198),
             selection_globals.interpolation,
             pointed<float>(d.storage, 0x4346A338u),
             d.menu.maximum_unlag,
             std::bit_cast<std::int32_t>(frame.field_d4),
             std::bit_cast<std::int32_t>(frame.field_18c),
             &selection_globals,
             &frame.field_d4,
             &frame.field_18c},
      preparation{d.history.entity_runtime(),
                  std::bit_cast<std::uint32_t>(pointed<float>(d.storage, 0x43467EB8u)),
                  read<float>(d.storage.data().native_cell<void*>(0x43467B04u), 0),
                  pointed<float>(d.storage, 0x4346A334u),
                  d.storage.data().native_cell<lagcomp::TickTimingSamples>(0x43478C70u),
                  nullptr,
                  &preparation_globals},
      targeting{player,
                d.storage.data().native_cell<void*>(0x4346A890u),
                d.history.game_type(),
                d.history.teammates_are_enemies(),
                d.history.players().adjustments(),
                d.history.target_offsets()},
      trace_entities{targeting.entity_list, std::uint32_t(field(d.storage, 0xE4E2D1A7u)),
                     std::uint32_t(field(d.storage, 0x49D93E62u) - 0x18)},
      attachments{d.animation.bones(),
                  inputs.attachment_matrices,
                  d.animation.bones().update_attachments,
                  field(d.storage, 0x6E9852D6u),
                  field(d.storage, 0x2A671B42u),
                  field(d.storage, 0x4E66BB34u),
                  field(d.storage, 0x83A6CF2Bu)},
      selected_records{d.history.players().cache(),
                       preparation,
                       timing,
                       d.rotation,
                       inputs.record_interpolation,
                       attachments,
                       d.storage.data().native_cell<std::uint32_t*>(0x43469060u)},
      movement_extrapolation{d.history.entity_runtime().offsets,
                             std::uint32_t(field(d.storage, 0x7A2D60C1u)),
                             std::uint32_t(field(d.storage, 0x4E66BB34u)),
                             std::uint32_t(field(d.storage, 0x2A671B42u)),
                             std::uint32_t(field(d.storage, 0x7090D8DDu)),
                             std::uint32_t(field(d.storage, 0x52899DF3u)),
                             std::uint32_t(field(d.storage, 0xD3D4230Du)),
                             std::nullopt,
                             &d.history.players().cache(),
                             std::nullopt,
                             std::nullopt,
                             std::nullopt,
                             &d.scene.queries().movement(),
                             lagcomp::native_history_animation_engine(
                                 d.storage.data().native_cell<void*>(0x434693F0u)),
                             d.storage.data().native_cell<std::int32_t*>(0x434678F8u),
                             d.storage.data().native_cell<float*>(0x43467EB8u),
                             d.storage.data().native_cell<float*>(0x4346A338u),
                             d.storage.data().native_cell<std::uint8_t*>(0x43467B24u),
                             d.history.entity_runtime().set_absolute_angles,
                             d.storage.data().native_cell<void*>(0x43467204u)},
      extrapolation{d.history.players().cache(), preparation, attachments, d.animation.bones(),
                    movement_extrapolation, std::nullopt,
                    d.configuration.rage_settings().global.experimental(ExperimentalRageOption::records),
                    d.configuration.rage_settings().global.experimental(ExperimentalRageOption::extrapolated_pose)},
      alternative{d.history.entity_runtime().offsets,
                  {std::uint32_t(field(d.storage, 0x36375C23u)),
                   std::uint32_t(field(d.storage, 0x4E66BB34u)),
                   std::uint32_t(field(d.storage, 0x2A671B42u)),
                   std::uint32_t(field(d.storage, 0x3D10B94Cu)),
                   std::uint32_t(field(d.storage, 0xE2FBE695u)),
                   std::uint32_t(field(d.storage, 0xE23A9BE1u)),
                   std::uint32_t(field(d.storage, 0x3EB71843u)),
                   std::uint32_t(field(d.storage, 0x0176A381u))},
                  {d.storage.data().native_cell<void**>(0x4346A3A4u),
                   std::uint32_t(field(d.storage, 0xBF567619u)),
                   std::uint32_t(field(d.storage, 0xDDA99AB5u)),
                   std::uint32_t(field(d.storage, 0xB7EA8FDAu)),
                   &d.storage.data().native_cell<std::int32_t>(0x43469274u)},
                  {d.engine.variables.at(0x43467E4Cu), d.engine.variables.at(0x43467A54u),
                   d.engine.variables.at(0x43467FA0u), d.engine.variables.at(0x43467990u),
                   d.engine.variables.at(0x4346A88Cu), d.engine.variables.at(0x43468FB4u),
                   d.engine.variables.at(0x43468F4Cu), d.engine.variables.at(0x434674E4u)},
                  d.storage.data().native_cell<void*>(0x4346A514u),
                  targeting.entity_list,
                  d.storage.data().native_cell<float*>(0x4346A338u),
                  d.storage.data().native_cell<float*>(0x4346A338u),
                  d.storage.data().native_cell<float*>(0x43467EB8u),
                  d.storage.data().native_cell<std::int32_t*>(0x434678F8u),
                  d.storage.data().native_cell<std::uint8_t*>(0x43467B24u),
                  d.storage.data().native_cell<std::uint8_t>(0x4347CA58u),
                  d.scene.queries().movement(),
                  d.storage.data().native_cell<std::uint8_t*>(0x434683D0u),
                  movement_extrapolation.animation_engine,
                  d.animation.trigonometry(),
                  animation::native_sse_atan2,
                  d.history.entity_runtime().set_absolute_angles,
                  d.storage.data().native_cell<void*>(0x43467204u),
                  inputs.alternative_animation},
      history{extrapolation,
              alternative,
              d.rotation,
              inputs.history_interpolation,
              pointed<float>(d.storage, 0x43467EB8u),
              selection_globals.interpolation,
              d.menu.maximum_unlag,
              {&d.configuration.rage_settings().global.jitter_fix.value,
               &d.configuration.rage_settings().global.experimental_rage.value},
              d.configuration.rage_settings().global.experimental(ExperimentalRageOption::animation),
              d.configuration.rage_settings().global.experimental(ExperimentalRageOption::resolver)},
      preamble{
          d.configuration.rage_settings().global,
          d.configuration.rage_settings().aimbot,
          d.input,
          d.storage.data().native_cell<std::uint32_t>(0x434759C0u),
          d.engine.variables.at(0x43469028u),
          d.storage.data().native_cell<const void**>(0x4346A3A4u),
          preamble_fields(d.storage),
          std::span<client::Command, 150>(
              read<client::Command*>(d.storage.data().native_cell<void*>(0x4346A894u), 0xF0), 150),
          animation_history(d.storage),
          pointed<float>(d.storage, 0x43467EB8u),
          pointed<float>(d.storage, 0x4346A338u),
          d.configuration.rage_settings().global.silent_aim.value != 0,
          d.menu.hold_aim != 0,
          d.menu.hold_aim != 0,
          pointed<std::int32_t>(d.storage, 0x43475B2Cu),
          d.storage.data().native_cell<std::int32_t>(0x434759C4u),
          d.configuration.rage_settings().global.double_tap_hit_chance.value,
          convar_boolean(d.engine.variables.at(0x43468520u)),
          d.crt.spread().tables(),
          d.crt.random(),
          d.esp.shotgun_pattern(),
          d.animation.trigonometry(),
          d.storage.data().native_cell<anti_aim::TargetState>(0x4346DB14u),
          std::cref(d.refresh),
          d.history.entity_runtime().set_absolute_angles},
      target_preparation{targeting,
                         preamble.settings,
                         preamble.weapon_settings,
                         d.input,
                         d.state_,
                         preamble.yaw_target.handle,
                         d.engine.variables.at(0x43468EA4u),
                         {field(d.storage, 0x1CAAF0A8u), field(d.storage, 0x3D52A1A7u),
                          field(d.storage, 0x32E4DAF1u), field(d.storage, 0xC2B6FB10u),
                          field(d.storage, 0x17978090u), field(d.storage, 0x36375C23u),
                          field(d.storage, 0xB538BBA5u)}},
      candidates{d.engine.variables.at(0x43467A78u),
                 d.storage.data().native_cell<void*>(0x4346A514u),
                 preamble.prediction_time,
                 preamble.interval,
                 d.history.entity_runtime().receive_interval,
                 selection_globals.interpolation,
                 d.menu.maximum_unlag,
                 std::nullopt,
                 &d.capture_owner.budget(),
                 extrapolation.validate_record,
                 extrapolation.rebuild_pose},
      samples(hitchance_spread_samples(d.state_.spread)),
      damage_context(d.damage_rules.capture(
          bool(preamble.settings.experimental(ExperimentalRageOption::damage)),
          d.storage.data().native_cell<void*>(0x4346A87Cu),
          reinterpret_cast<const void*>(std::uintptr_t(frame.weapon_data_address)),
          field(d.storage, 0xEAA57C2Cu),
          preamble.settings.experimental(ExperimentalRageOption::damage)
              ? convar_integer(d.history.game_type()) : 0)),
      scanner{d.scene.queries().ballistics(),
              targeting,
              BallisticEntity(reinterpret_cast<std::uintptr_t>(player)),
              reinterpret_cast<void*>(std::uintptr_t(frame.weapon_address)),
              field(d.storage, 0x5C9F0FBBu),
              preamble.settings.automatic_penetration.value,
              frame.weapon_id_d8,
              frame.origin_dc,
              std::bit_cast<float>(frame.field_160),
              frame.pellet_count_19c,
              std::bit_cast<std::int32_t>(frame.field_1d0),
              preamble.weapon_settings.minimum_hit_chance.value,
              d.state_.flags,
              d.state_.previous_targets,
              samples,
              {field(d.storage, 0x32E4DAF1u), field(d.storage, 0xC2B6FB10u),
               field(d.storage, 0x17978090u), field(d.storage, 0xA9EE6062u)},
              d.animation.trigonometry(),
              d.clipper,
              d.history.players().cache(),
              preparation,
              {field(d.storage, 0x6E9852D6u), field(d.storage, 0x2A671B42u),
               field(d.storage, 0x4E66BB34u), field(d.storage, 0x83A6CF2Bu)},
              d.animation.bones().update_attachments,
              {d.engine.variables.at(0x43467C3Cu), d.engine.variables.at(0x43467084u),
               d.engine.variables.at(0x43468C90u), d.history.teammates_are_enemies()},
              d.storage.data().native_cell<void**>(0x43475B90u),
              trace_entities, &damage_context},
      body_probe{d.scene.queries().movement(),
                 scanner.ballistics,
                 scanner.source,
                 scanner.weapon,
                 {field(d.storage, 0xA9EE6062u), field(d.storage, 0x36375C23u),
                  field(d.storage, 0x32E4DAF1u), field(d.storage, 0xC2B6FB10u),
                  field(d.storage, 0x17978090u), field(d.storage, 0x5C9F0FBBu)},
                 d.state_.flags,
                 d.state_.weapon_flags,
                 preamble.settings.automatic_penetration.value,
                 d.storage.data().native_cell<std::uint8_t*>(0x434683D0u),
                 scanner.trace_filter_vtable,
                 trace_entities},
      prediction{body_probe,
                 selected_records,
                 d.storage.data().native_cell<void*>(0x43468304u),
                 field(d.storage, 0x3EB71843u),
                 preamble.scheduler_flags,
                 nullptr},
      records{d.history.players().cache(),
              candidates.lag_compensation,
              preamble.interval,
              preamble.yaw_target.handle,
              preamble.settings.body_aim_disablers.value,
              d.storage.data().native_cell<FeedbackOwner*>(0x43475A7Cu),
              history,
              extrapolation,
              scanner,
              d.menu,
              d.animation.bones(),
              d.movement.live_movement(),
              d.capture_owner.budget()},
      single{target_preparation,
             prediction,
             candidates,
             records,
             d.history.players().adjustments(),
             pointed<std::int32_t>(d.storage, 0x43469300u),
             d.jobs},
      multiple{target_preparation,
               body_probe,
               prediction,
               candidates,
               records,
               d.history.players().adjustments(),
               pointed<std::int32_t>(d.storage, 0x43469300u),
               d.jobs},
      final_target{scanner, d.storage.data().native_cell<std::uint32_t*>(0x43469060u), d.menu,
                   &d.jobs, &history, &preamble},
      readiness{
          d.history.players().cache(),
          d.storage.data().native_cell<void*>(0x4346A514u),
          candidates.lag_compensation,
          preamble.interval,
          selection_globals.interpolation,
          preamble.prediction_time,
          d.menu.maximum_unlag,
          0.5f,
          d.history.entity_runtime().receive_interval,
          preamble.settings.automatic_fire.value != 0,
          preamble.settings.silent_aim.value != 0,
          preamble.send_mode,
          d.configuration.anti_aim_global().body_yaw.value,
          preamble.scheduler_flags,
          preamble.selected_marker,
          preamble.current_sequence,
          frame.weapon_id_d8 == 64
              ? read<float>(reinterpret_cast<const void*>(std::uintptr_t(frame.weapon_address)),
                            d.capture_owner.firing().offsets.next_secondary)
              : frame.postpone_fire_1e4},
      post_angles{player,
                  firing_fields(d.storage),
                  d.storage.data().native_cell<void*>(0x4346A894u),
                  d.animation.bones(),
                  d.animation.bones(),
                  d.animation.bones(),
                  d.animation.trigonometry(),
                  {animation::native_sse_atan2},
                  d.eye_pose,
                  legit::recoil_compensation_enabled(
                      legit::legit_owner_settings(
                          d.storage.data().native_cell<legit::LifecycleOwner*>(0x4347588Cu)),
                      std::uint32_t(std::int32_t(std::int8_t(frame.weapon_kind_124))), d.input) ||
                      !preamble.settings.remove_recoil.value,
                  preamble.settings.automatic_fire.value != 0,
                  preamble.send_mode,
                  preamble.settings.remove_recoil.value != 0,
                  preamble.scheduler_flags,
                  preamble.prediction_time},
      post_movement{preamble.settings,
                    d.input,
                    d.state_.flags,
                    preamble.scheduler_flags,
                    d.state_.weapon_flags,
                    d.state_.target_mask,
                    targeting,
                    {field(d.storage, 0xA9EE6062u), field(d.storage, 0xDE530A8Bu),
                     field(d.storage, 0xCACAEECCu), field(d.storage, 0xC07D1150u)},
                    scanner.ballistics,
                    scanner.source,
                    scanner.weapon_item_offset,
                    scanner.trace_filter_vtable,
                    trace_entities,
                    {d.prediction_masks()},
                    preamble.prediction_time,
                    preamble.yaw_target.handle},
      diagnostics{d.storage.data().native_cell<client::BufferedShotLifecycleOwner*>(0x43475968u),
                  d.history.players().cache(),
                  {field(d.storage, 0xD5FD2E2Cu), field(d.storage, 0xE2FBE695u),
                   field(d.storage, 0x3309C0CFu), field(d.storage, 0x7F5ED2D0u)},
                  d.storage.data().native_cell<std::uint32_t*>(0x4346A334u),
                  preamble.interval,
                  d.storage.data().native_cell<void*>(0x43467B54u)},
      attempts{d.storage.data().native_cell<void*>(0x4346A890u),
               d.storage.data().native_cell<float*>(0x4346A334u),
               field(d.storage, 0x9D920E35u),
               field(d.storage, 0x3D52A1A7u),
               field(d.storage, 0x32E4DAF1u),
               field(d.storage, 0x1CAAF0A8u),
               field(d.storage, 0xC2B6FB10u),
               d.pose_range,
               d.clipper},
      pending{d.storage.data().native_cell<FeedbackOwner*>(0x43475A7Cu),
              d.configuration.rage_settings().global.anti_aim_correction.value,
              d.history.players().cache(),
              d.storage.data().native_cell<void*>(0x4346A514u),
              targeting.entity_list,
              d.storage.data().native_cell<void*>(0x43467B54u),
              d.engine.variables.at(0x434670D0u),
              {field(d.storage, 0xCA5EC698u), field(d.storage, 0x47877DEBu),
               field(d.storage, 0xC2B6FB10u)},
              d.storage.data().native_cell<float*>(0x4346A334u),
              d.storage.data().native_cell<float*>(0x43467EB8u),
              d.storage.data().native_cell<float*>(0x4346A338u),
              &preamble.scheduler_flags,
              attempts,
              nullptr,
              {&d.configuration.rage_settings().global.jitter_fix.value,
               &d.configuration.rage_settings().global.experimental_rage.value}},
      shot_models{d.configuration.colored_model_settings(),
                  targeting.entity_list,
                  d.storage.data().native_cell<void*>(0x4346A614u),
                  d.storage.data().native_cell<void*>(0x43469330u),
                  read<ShotEntityCreate>(d.storage.data().native_cell<void*>(0x434685FCu), 0),
                  d.storage.data().native_cell<RegisterShotEntity>(0x4346A884u),
                  d.storage.data().native_cell<UnregisterShotEntity>(0x43468E88u),
                  shot_registration_receiver(targeting.entity_list),
                  shot_model_fields(d.storage),
                  d.storage.data().native_cell<InvalidateShotTransform>(0x43468C10u),
                  &d.storage.data().native_cell<engine::RecvVarProxy>(0x43476B44u),
                  d.storage.data().native_cell<std::uint32_t>(0x4346D674u),
                  d.effects.shot_state(),
                  d.effects.shot_bones(),
                  d.history.players().cache(),
                  preparation,
                  timing,
                  d.storage.data().native_cell<std::uint32_t*>(0x43469060u),
                  d.animation.bones(),
                  d.storage.data().native_cell<void*>(0x43467118u)},
      stop{d.movement.movement_environment()},
      post_firing{preamble,
                  post_movement,
                  d.history.players().cache(),
                  d.capture_owner.view_angles(),
                  final_target,
                  d.scripts.bridge(),
                  diagnostics,
                  pending,
                  shot_models,
                  d.shot_index_,
                  pointed<std::int32_t>(d.storage, 0x43469300u),
                  d.storage.data().native_cell<float>(0x43469254u),
                  nullptr,
                  stop,
                  d.movement.movement_capture(),
                  d.scene.queries().movement(),
                  prediction,
                  scanner,
                  d.animation.bones()},
      action{d.storage.data().native_cell<std::uint8_t>(0x4346A65Cu) != 0,
             d.configuration.player_esp_settings().aimbot_visualization,
             d.state_,
             preamble,
             target_preparation,
             d.history.players().adjustments(),
             pending.active,
             history,
             scanner,
             d.scene.queries().movement(),
             body_probe.trace_flag,
             d.animation.bones(),
             d.movement.live_movement(),
             d.capture_owner.budget()},
      button_policy{d.storage.data().native_cell<std::array<std::uint32_t, 150>>(0x43476090u),
                    d.configuration.anti_aim_other().leg_movement.value,
                    pointed<std::int32_t>(d.storage, 0x434757DCu)},
      tail{preamble,
           d.highlight_,
           action,
           legit::legit_owner_settings(
               d.storage.data().native_cell<legit::LifecycleOwner*>(0x4347588Cu)),
           {animation::native_sse_atan2},
           animation::native_sse_arctangent,
           d.storage.data().native_cell<void*>(0x4346A894u),
           post_angles.offsets,
           button_policy,
           {d.animation.trigonometry().sine, d.animation.trigonometry().cosine},
           d.capture_owner.angle_environment(),
           std::span<client::VerifiedCommand, 150>(
               read<client::VerifiedCommand*>(d.storage.data().native_cell<void*>(0x4346A894u),
                                              0xF4),
               150),
           d.storage.data().native_cell<client::CommandChecksum>(0x43467C48u),
           d.storage.data().native_cell<void*>(0x43469264u)},
      phase2(d.phase2_bindings()), phase6(d.phase6_bindings()),
      command{d.state_,  d.acquisition_, preamble,    single, multiple, final_target, d.shared,
              readiness, post_angles,    post_firing, tail,   phase2,   phase6,       d.menu} {
    // Native43344C0C/4336C572/4333DB01 own their automatic workspaces.
    // Production retains actual unwritten local bytes where native does;
    // only the explicit differential profile supplies a prior byte image.
    if (d.explicit_inputs)
        std::memcpy(&inputs, d.explicit_inputs, sizeof inputs);
}

void RuntimeOwner::Data::process(client::CommandFrame& frame, void* player,
                                 client::Command& command, std::uint8_t& send) {
    if (!player)
        throw std::invalid_argument("RAGE requires the actual current command player");
    Frame physical(*this, frame, player, command);
    process_rage_command(physical.command, frame, player, command, send);
}

RuntimeOwner::RuntimeOwner(ClientRuntimeStorage& s, const client::EngineRuntimeBindings& e,
                           client::ConfigurationRuntimeOwner& c, client::HistoryRuntimeOwner& h,
                           engine::SceneQueryRuntimeOwner& q, animation::RuntimeOwner& b,
                           client::CommandCaptureRuntimeOwner& capture,
                           client::MovementHudRuntimeOwner& m,
                           client::TargetRefreshRuntimeOwner& refresh, client::CrtRuntimeOwner& crt,
                           esp::EspRuntimeOwner& esp, effects::EffectsRuntimeOwner& effects,
                           client::VisualRuntimeOwner& visual,
                           script::SourceScriptRuntimeOwner& scripts, CommandFrameInputs& incoming)
    : data_(std::make_unique<Data>(s, e, c, h, q, b, capture, m, refresh, crt, esp, effects, visual,
                                   scripts, &incoming,
                                   RuntimeImports{engine::native_thread_pool()})) {}
RuntimeOwner::RuntimeOwner(ClientRuntimeStorage& s, const client::EngineRuntimeBindings& e,
                           client::ConfigurationRuntimeOwner& c, client::HistoryRuntimeOwner& h,
                           engine::SceneQueryRuntimeOwner& q, animation::RuntimeOwner& b,
                           client::CommandCaptureRuntimeOwner& capture,
                           client::MovementHudRuntimeOwner& m,
                           client::TargetRefreshRuntimeOwner& refresh, client::CrtRuntimeOwner& crt,
                           esp::EspRuntimeOwner& esp, effects::EffectsRuntimeOwner& effects,
                           client::VisualRuntimeOwner& visual,
                           script::SourceScriptRuntimeOwner& scripts)
    : RuntimeOwner(s, e, c, h, q, b, capture, m, refresh, crt, esp, effects, visual, scripts,
                   RuntimeImports{engine::native_thread_pool()}) {}
RuntimeOwner::RuntimeOwner(ClientRuntimeStorage& s, const client::EngineRuntimeBindings& e,
                           client::ConfigurationRuntimeOwner& c, client::HistoryRuntimeOwner& h,
                           engine::SceneQueryRuntimeOwner& q, animation::RuntimeOwner& b,
                           client::CommandCaptureRuntimeOwner& capture,
                           client::MovementHudRuntimeOwner& m,
                           client::TargetRefreshRuntimeOwner& refresh, client::CrtRuntimeOwner& crt,
                           esp::EspRuntimeOwner& esp, effects::EffectsRuntimeOwner& effects,
                           client::VisualRuntimeOwner& visual,
                           script::SourceScriptRuntimeOwner& scripts, RuntimeImports imports)
    : data_(std::make_unique<Data>(s, e, c, h, q, b, capture, m, refresh, crt, esp, effects, visual,
                                   scripts, nullptr, imports)) {}
RuntimeOwner::~RuntimeOwner() = default;
ClientRuntimeStorage& RuntimeOwner::storage() const noexcept {
    return data_->storage;
}
PreambleState& RuntimeOwner::state() noexcept {
    return data_->state_;
}
AcquisitionStorage& RuntimeOwner::acquisition() noexcept {
    return data_->acquisition_;
}
CommandEndpoint& RuntimeOwner::commands() noexcept {
    return data_->factory.commands;
}
OwnerBindings& RuntimeOwner::factory_bindings() noexcept {
    return data_->factory;
}
void RuntimeOwner::with_frame(client::CommandFrame& frame, void* player, client::Command& command,
                              const std::function<void(const RuntimeFrameBindings&)>& consume) {
    if (!player || !consume)
        throw std::invalid_argument(
            "RAGE frame composition requires an actual player and synchronous consumer");
    Data::Frame physical(*data_, frame, player, command);
    consume(physical.views());
}
const lagcomp::EvaluationState* RuntimeOwner::repair_buffered(client::CommandFrame& frame,
                                                              void* player,
                                                              client::Command& command,
                                                              const ShotDiagnosticEntry& diagnostic,
                                                              BufferedShotRepairScratch& scratch) {
    if (!storage().data().native_cell<LifecycleOwner*>(0x43475810u) || (frame.flags & 0x20040u))
        return nullptr;
    if (!player)
        throw std::invalid_argument(
            "Buffered RAGE reconstruction requires this invocation's actual local player");
    Data::Frame physical(*data_, frame, player, command);
    return repair_buffered_shot(frame, player, command, diagnostic, scratch,
                                BufferedShotRepairBindings(&physical.command));
}
void RuntimeOwner::bind_initializer(client::NativeStartupSources& sources) {
    if (&sources.data() != &storage().data())
        throw std::invalid_argument("RAGE initializer requires its canonical source data");
    sources.bind_initializer({0x4348DA25u, data_.get(), [](void* raw) {
                                  auto& d = *static_cast<Data*>(raw);
                                  (void)construct_rage_owner(
                                      d.factory,
                                      d.storage.data().native_cell<LifecycleOwner*>(0x43475810u));
                              }});
}

} //namespace skeet::rage
