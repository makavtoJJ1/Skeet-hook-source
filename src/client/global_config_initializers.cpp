#include "skeet/client/global_config_initializers.hpp"
#include "skeet/client/native_startup.hpp"
#include "skeet/esp/effects_config.hpp"
#include "skeet/esp/model_color_config.hpp"
#include "skeet/esp/other_config.hpp"
#include "skeet/menu/settings_menu_storage.hpp"
#include "skeet/misc/config.hpp"
#include "skeet/rage/settings.hpp"

namespace skeet::client {
namespace {
GlobalConfigInitializerBindings& bindings(void* context) noexcept {
    return *static_cast<GlobalConfigInitializerBindings*>(context);
}
void append(GlobalConfigInitializerBindings& source, ConfigDescriptor& descriptor) noexcept {
    append_config_link(source.configuration.descriptors, descriptor); //43385AE4
}
void hold_key(GlobalConfigInitializerBindings& source, HotkeyConfig& descriptor,
              std::uint32_t packed_key) noexcept {
    append(source, descriptor);
    auto& key = descriptor.value;
    append_hotkey(source.configuration.hotkeys, key); //4333CA03
    const auto packed = key.binding.packed;
    key.callback_object = nullptr;
    key.callback = nullptr;
    key.binding.mode = HotkeyMode::hold;
    key.binding.packed = (packed & 1u) | packed_key;
}
void always_key(GlobalConfigInitializerBindings& source, HotkeyConfig& descriptor) noexcept {
    append(source, descriptor);
    auto& key = descriptor.value;
    append_hotkey(source.configuration.hotkeys, key);
    const auto packed = key.binding.packed;
    key.callback_object = nullptr;
    key.callback = nullptr;
    key.binding.packed = (packed & 1u) | 0x6cu;
    key.binding.mode = HotkeyMode::always;
}

void anti_untrusted(void* context) { //434928E5,434693DC
    auto& source = bindings(context);
    append(source, source.anti_untrusted.enabled);
    source.anti_untrusted.enabled.value = 1;
}
void hide_from_obs(void* context) { //43492907,43468E48
    auto& source = bindings(context);
    append(source, source.hide_from_obs.enabled);
}
void multithreading(void* context) { //43492921,434683EC
    auto& source = bindings(context);
    append(source, source.settings_menu.multithreading_config);
    source.settings_menu.multithreading_config.value = 1;
}
void custom_events(void* context) { //43492943,434691A8
    auto& source = bindings(context);
    append(source, source.custom_events.enabled);
    source.custom_events.enabled.value = 1;
}
void free_look(void* context) { //43493241,43468264
    auto& source = bindings(context);
    hold_key(source, source.free_look.key, 0x6cu);
}
void duck_peek(void* context) { //434933C0,43467C7C
    auto& source = bindings(context);
    hold_key(source, source.rage.duck_peek_assist, 0x6cu);
}
void shared_esp(void* context) { //434934A1,43467F88
    auto& source = bindings(context);
    append(source, source.other_esp.shared.enabled);
}
void restrict_shared_esp(void* context) { //434934BB,43468588
    auto& source = bindings(context);
    append(source, source.other_esp.shared.restrict_updates);
}
void remove_flash(void* context) { //434934D5,43468FF4
    auto& source = bindings(context);
    append(source, source.effects.remove_flash);
}
void third_person_dead(void* context) { //434934EF,4346DAE0
    auto& source = bindings(context);
    append(source, source.effects.third_person_dead);
}
void third_person_key(void* context) { //43493509,43468438
    auto& source = bindings(context);
    always_key(source, source.effects.third_person_key);
}
void third_person_alive(void* context) { //43493552,43468A00
    auto& source = bindings(context);
    append(source, source.effects.third_person_alive);
}
void disable_ragdolls(void* context) { //4349356C,43469010
    auto& source = bindings(context);
    append(source, source.effects.disable_ragdolls);
}
void disable_model_occlusion(void* context) { //43493586,43467ADC
    auto& source = bindings(context);
    append(source, source.colored_models.disable_model_occlusion);
}
void disable_teammates(void* context) { //434935A0,43468544
    auto& source = bindings(context);
    append(source, source.effects.disable_teammates);
    source.effects.disable_teammates.value = 0;
}
void colored_ragdolls(void* context) { //434935C2,4346A680
    auto& source = bindings(context);
    append(source, source.colored_models.ragdolls);
}
void pattern_recoil(void* context) { //434935DC,43467CA4
    auto& source = bindings(context);
    append(source, source.rage.pattern_recoil_correction);
}
void automatic_penetration(void* context) { //434935F6,4346A3D0
    auto& source = bindings(context);
    append(source, source.rage.automatic_penetration);
}
void automatic_fire(void* context) { //43493610,43468400
    auto& source = bindings(context);
    append(source, source.rage.automatic_fire);
}
void target_hitboxes(void* context) { //4349362A,434691BC
    auto& source = bindings(context);
    append(source, source.rage.target_hitboxes);
    source.rage.target_hitboxes.value = 2;
}
void avoid_unsafe_hitboxes(void* context) { //4349364F,43467020
    auto& source = bindings(context);
    append(source, source.rage.avoid_unsafe_hitboxes);
    source.rage.avoid_unsafe_hitboxes.value = 0;
}
void prefer_safe_point(void* context) { //43493671,43468F68
    auto& source = bindings(context);
    append(source, source.rage.prefer_safe_point);
}
void rage_activation(void* context) { //4349368B,43467F24
    auto& source = bindings(context);
    hold_key(source, source.rage.activation, 0x18u);
}
void quick_stop_key(void* context) { //434936D6,43469308
    auto& source = bindings(context);
    always_key(source, source.rage.quick_stop_key);
}
void force_body_aim(void* context) { //4349371F,4346A2F0
    auto& source = bindings(context);
    hold_key(source, source.rage.force_body_aim, 0x6cu);
}
void multipoint_key(void* context) { //4349376A,43469400
    auto& source = bindings(context);
    always_key(source, source.rage.multipoint_key);
}
void rage_enabled(void* context) { //434937B3,43467214
    auto& source = bindings(context);
    append(source, source.rage.enabled);
}
void silent_aim(void* context) { //434937CD,434684D0
    auto& source = bindings(context);
    append(source, source.rage.silent_aim);
}
void remove_recoil(void* context) { //434937E7,43467134
    auto& source = bindings(context);
    append(source, source.rage.remove_recoil);
}
void quick_stop(void* context) { //43493801,434671D0
    auto& source = bindings(context);
    append(source, source.rage.quick_stop);
}
void quick_stop_options(void* context) { //4349381B,43467E30
    auto& source = bindings(context);
    append(source, source.rage.quick_stop_options);
    source.rage.quick_stop_options.value = 0;
}
void prefer_body_aim(void* context) { //4349383D,43467D60
    auto& source = bindings(context);
    append(source, source.rage.prefer_body_aim);
}
void body_aim_disablers(void* context) { //43493857,4346A8C0
    auto& source = bindings(context);
    append(source, source.rage.body_aim_disablers);
}
void force_safe_point(void* context) { //43493871,434678D0
    auto& source = bindings(context);
    hold_key(source, source.rage.force_safe_point, 0x6cu);
}
void double_tap_hit_chance(void* context) { //434938BC,4346A4A0
    auto& source = bindings(context);
    append(source, source.rage.double_tap_hit_chance);
    source.rage.double_tap_hit_chance.value = 50;
}
void automatic_scope(void* context) { //434938E1,434693C4
    auto& source = bindings(context);
    append(source, source.rage.automatic_scope);
}
}

void bind_global_config_initializers(NativeStartupSources& sources,
                                     GlobalConfigInitializerBindings& source) {
    sources.bind_initializer({0x434928e5u, &source, anti_untrusted});
    sources.bind_initializer({0x43492907u, &source, hide_from_obs});
    sources.bind_initializer({0x43492921u, &source, multithreading});
    sources.bind_initializer({0x43492943u, &source, custom_events});
    sources.bind_initializer({0x43493241u, &source, free_look});
    sources.bind_initializer({0x434933c0u, &source, duck_peek});
    sources.bind_initializer({0x434934a1u, &source, shared_esp});
    sources.bind_initializer({0x434934bbu, &source, restrict_shared_esp});
    sources.bind_initializer({0x434934d5u, &source, remove_flash});
    sources.bind_initializer({0x434934efu, &source, third_person_dead});
    sources.bind_initializer({0x43493509u, &source, third_person_key});
    sources.bind_initializer({0x43493552u, &source, third_person_alive});
    sources.bind_initializer({0x4349356cu, &source, disable_ragdolls});
    sources.bind_initializer({0x43493586u, &source, disable_model_occlusion});
    sources.bind_initializer({0x434935a0u, &source, disable_teammates});
    sources.bind_initializer({0x434935c2u, &source, colored_ragdolls});
    sources.bind_initializer({0x434935dcu, &source, pattern_recoil});
    sources.bind_initializer({0x434935f6u, &source, automatic_penetration});
    sources.bind_initializer({0x43493610u, &source, automatic_fire});
    sources.bind_initializer({0x4349362au, &source, target_hitboxes});
    sources.bind_initializer({0x4349364fu, &source, avoid_unsafe_hitboxes});
    sources.bind_initializer({0x43493671u, &source, prefer_safe_point});
    sources.bind_initializer({0x4349368bu, &source, rage_activation});
    sources.bind_initializer({0x434936d6u, &source, quick_stop_key});
    sources.bind_initializer({0x4349371fu, &source, force_body_aim});
    sources.bind_initializer({0x4349376au, &source, multipoint_key});
    sources.bind_initializer({0x434937b3u, &source, rage_enabled});
    sources.bind_initializer({0x434937cdu, &source, silent_aim});
    sources.bind_initializer({0x434937e7u, &source, remove_recoil});
    sources.bind_initializer({0x43493801u, &source, quick_stop});
    sources.bind_initializer({0x4349381bu, &source, quick_stop_options});
    sources.bind_initializer({0x4349383du, &source, prefer_body_aim});
    sources.bind_initializer({0x43493857u, &source, body_aim_disablers});
    sources.bind_initializer({0x43493871u, &source, force_safe_point});
    sources.bind_initializer({0x434938bcu, &source, double_tap_hit_chance});
    sources.bind_initializer({0x434938e1u, &source, automatic_scope});
}

} //namespace skeet::client
