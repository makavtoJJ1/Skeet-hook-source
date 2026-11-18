#include "skeet/client/config_initializers.hpp"
#include "skeet/client/native_startup.hpp"
#include "skeet/anti_aim/config.hpp"
#include "skeet/esp/config.hpp"
#include "skeet/esp/effects_config.hpp"
#include "skeet/esp/other_config.hpp"
#include "skeet/misc/config.hpp"
#include "skeet/rage/settings.hpp"

namespace skeet::client {
namespace {
ConfigInitializerBindings& bindings(void* context) noexcept {
    return *static_cast<ConfigInitializerBindings*>(context);
}
void append(ConfigInitializerBindings& source, ConfigDescriptor& descriptor) noexcept {
    //43385AE4 ->433AFD92 changes only the descriptor/registry links.
    append_config_link(source.configuration.descriptors, descriptor);
}
void feature_indicators(void* context) { //434909F8, descriptor43468B84
    auto& source = bindings(context);
    append(source, source.other_esp.feature_indicators);
    source.other_esp.feature_indicators.value = 0;
}
void anti_aim_correction(void* context) { //43490A1A, descriptor4346753C
    auto& source = bindings(context);
    append(source, source.rage.anti_aim_correction);
    append(source, source.rage.experimental_rage);
    for (const auto option : rage::active_experimental_rage_options)
        append(source, source.rage.experimental_options[std::size_t(option)]);
}
void visual_recoil(void* context) { //43490A34, descriptor4346744C
    auto& source = bindings(context);
    append(source, source.effects.visual_recoil);
}
void reduce_aim_step(void* context) { //43490A4E, descriptor434670B0
    auto& source = bindings(context);
    append(source, source.rage.reduce_aim_step);
}
void aim_step_limit(void* context) { //43490A68, descriptor4346708C
    auto& source = bindings(context);
    append(source, source.rage.aim_step_limit);
    source.rage.aim_step_limit.value = 15;
}
void ping_enabled(void* context) { //43490AAF, descriptor434691EC
    auto& source = bindings(context);
    append(source, source.ping_spike.enabled);
}
void ping_key(void* context) { //43490AC9, descriptor43469378
    auto& source = bindings(context);
    auto& key = source.ping_spike.key.value;
    append(source, source.ping_spike.key);
    append_hotkey(source.configuration.hotkeys, key); //4333CA03 ->433AFD92
    const auto packed = key.binding.packed;
    key.callback_object = nullptr;
    key.callback = nullptr;
    key.binding.mode = HotkeyMode::hold;
    key.binding.packed = (packed & 1u) | 0x6cu;
}
void ping_milliseconds(void* context) { //43490B14, descriptor43467ECC
    auto& source = bindings(context);
    append(source, source.ping_spike.milliseconds);
    source.ping_spike.milliseconds.value = 100;
}
void crosshair(void* context) { //43490B5B, descriptor4346A754
    auto& source = bindings(context);
    append(source, source.other_esp.overlays.crosshair);
}
void money(void* context) { //43490B75, descriptor4346906C
    auto& source = bindings(context);
    append(source, source.player_esp.money);
}
void grenade_entities(void* context) { //43490B8F, descriptor43467CC4
    auto& source = bindings(context);
    append(source, source.other_esp.grenades.entities);
}
void grenade_color(void* context) { //43490BA9, descriptor43468F50
    auto& source = bindings(context);
    append(source, source.other_esp.grenades.entity_color);
    source.other_esp.grenades.entity_color.value = 0xff0047ffu;
}
void anti_aim_enabled(void* context) { //43490BCE, descriptor4346A730
    auto& source = bindings(context);
    append(source, source.anti_aim.enabled);
}
void anti_aim_pitch(void* context) { //43490BE8, descriptor43467F50
    auto& source = bindings(context);
    append(source, source.anti_aim.pitch);
    source.anti_aim.pitch.value = 0;
}
void anti_aim_yaw(void* context) { //43490C0A, descriptor434682EC
    auto& source = bindings(context);
    append(source, source.anti_aim.yaw);
    source.anti_aim.yaw.value = 0;
}
void anti_aim_body_yaw(void* context) { //43490C2C, descriptor434685A8
    auto& source = bindings(context);
    append(source, source.anti_aim.body_yaw);
    source.anti_aim.body_yaw.value = 0;
}
void double_tap_quick_stop(void* context) { //43490C4E, descriptor4346A664
    auto& source = bindings(context);
    append(source, source.rage.double_tap_quick_stop);
}
void target_mask(void* context) { //43490C68, descriptor43467974
    auto& source = bindings(context);
    append(source, source.target_mask);
}
}

void bind_native_config_initializers(NativeStartupSources& sources,
                                     ConfigInitializerBindings& source) {
    sources.bind_initializer({0x434909f8u, &source, feature_indicators});
    sources.bind_initializer({0x43490a1au, &source, anti_aim_correction});
    sources.bind_initializer({0x43490a34u, &source, visual_recoil});
    sources.bind_initializer({0x43490a4eu, &source, reduce_aim_step});
    sources.bind_initializer({0x43490a68u, &source, aim_step_limit});
    sources.bind_initializer({0x43490aafu, &source, ping_enabled});
    sources.bind_initializer({0x43490ac9u, &source, ping_key});
    sources.bind_initializer({0x43490b14u, &source, ping_milliseconds});
    sources.bind_initializer({0x43490b5bu, &source, crosshair});
    sources.bind_initializer({0x43490b75u, &source, money});
    sources.bind_initializer({0x43490b8fu, &source, grenade_entities});
    sources.bind_initializer({0x43490ba9u, &source, grenade_color});
    sources.bind_initializer({0x43490bceu, &source, anti_aim_enabled});
    sources.bind_initializer({0x43490be8u, &source, anti_aim_pitch});
    sources.bind_initializer({0x43490c0au, &source, anti_aim_yaw});
    sources.bind_initializer({0x43490c2cu, &source, anti_aim_body_yaw});
    sources.bind_initializer({0x43490c4eu, &source, double_tap_quick_stop});
    sources.bind_initializer({0x43490c68u, &source, target_mask});
}

} // namespace skeet::client
