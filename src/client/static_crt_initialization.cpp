#include "skeet/client/static_crt_initialization.hpp"
#include "skeet/client/native_startup.hpp"
#include "skeet/client/shutdown.hpp"
#include "skeet/engine/owner_allocation.hpp"
#include "skeet/engine/native_data_image.hpp"
#include "skeet/render/icon_cache.hpp"
#include <array>
#include <stdexcept>
#include <utility>

namespace skeet::client {
namespace {
struct PointerFinalizer {
    std::uint32_t initializer, finalizer, cell;
    bool clear_after_free;
};
constexpr std::array<PointerFinalizer, 11> pointers{
    {{0x43393926u, 0x4339c54fu, 0x43478cc8u, false},
     {0x43320100u, 0x43382567u, 0x43478dd0u, false},
     {0x433b37a8u, 0x433ab14du, 0x43478de0u, false},
     {0x433848d6u, 0x4331e21au, 0x43475944u, false},
     {0x433800e5u, 0x433b32bfu, 0x43479300u, true},
     {0x433e1321u, 0x433e0032u, 0x4347962cu, false},
     {0x433e3de2u, 0x433d1129u, 0x4347961cu, false},
     {0x433eb701u, 0x433e3b6du, 0x43479784u, false},
     {0x43400269u, 0x433ab50cu, 0x43479770u, false},
     {0x433a12a4u, 0x433b76abu, 0x434797a0u, false},
     {0x43349e5au, 0x433b1fe9u, 0x434797b0u, true}}};
StaticCrtInitialization* active_context = nullptr;

template <std::size_t Index> void finalize_pointer() {
    active_context->finalize_pointer_storage(pointers[Index].finalizer);
}
template <std::size_t... Index> constexpr auto pointer_callbacks(std::index_sequence<Index...>) {
    return std::array<void (*)(), sizeof...(Index)>{finalize_pointer<Index>...};
}
constexpr auto finalizers = pointer_callbacks(std::make_index_sequence<pointers.size()>{});

template <std::size_t Index> void register_pointer_finalizer(void* raw) {
    auto& context = *static_cast<StaticCrtInitialization*>(raw);
    (void)context.callbacks.append(context.source_finalizer(pointers[Index].finalizer));
}
template <std::size_t... Index>
void bind_pointers(NativeStartupSources& sources, StaticCrtInitialization& context,
                   std::index_sequence<Index...>) {
    (sources.bind_initializer(
         {pointers[Index].initializer, &context, register_pointer_finalizer<Index>}),
     ...);
}
void initialize_icons(void* raw) {
    static_cast<StaticCrtInitialization*>(raw)->initialize_icon_cache();
}
void initialize_entities(void* raw) {
    static_cast<StaticCrtInitialization*>(raw)->initialize_entity_slots();
}
}

void StaticCrtInitialization::initialize_icon_cache() {
    icons.initialize_native_cache();
}
void StaticCrtInitialization::initialize_entity_slots() {
    engine::require_native_data_image(data)
        .native_cell<std::array<std::uint32_t, 65>>(0x43469094u)
        .fill(UINT32_MAX);
}
void StaticCrtInitialization::finalize_pointer_storage(std::uint32_t native_finalizer) const {
    for (const auto& entry : pointers) {
        if (entry.finalizer != native_finalizer)
            continue;
        auto& image = engine::require_native_data_image(data);
        auto& cell = image.native_cell<void*>(entry.cell);
        if (auto* const allocation = cell) {
            engine::release_owner_storage(memory, allocation);
            // Only 433B32BF/433B1FE9 clear their pointer in the native binary.
            if (entry.clear_after_free)
                cell = nullptr;
        }
        if (entry.cell == 0x434797A0u) {
            // The source application destroys ShotModelBoneStorage after the
            // CRT exit callbacks. Both share this vector, so revoke ownership
            // here to prevent its C++ destructor from freeing the block again.
            image.native_cell<std::array<void*, 3>>(entry.cell).fill(nullptr);
        }
        return;
    }
    throw std::out_of_range("Finalizer is not a recovered static pointer-buffer cleanup entry");
}
void (*StaticCrtInitialization::source_finalizer(std::uint32_t native_finalizer))() {
    for (std::size_t index = 0; index < pointers.size(); ++index) {
        if (pointers[index].finalizer != native_finalizer)
            continue;
        if (active_context && active_context != this)
            throw std::logic_error("Static CRT exit callbacks require one stable source context");
        active_context = this;
        return finalizers[index];
    }
    throw std::out_of_range("Finalizer is not a recovered static pointer-buffer cleanup entry");
}
void bind_static_crt_initializers(NativeStartupSources& sources, StaticCrtInitialization& context) {
    if (context.data.get() != &sources.data())
        throw std::invalid_argument("Static CRT storage must share the startup native data owner");
    if (!context.memory.free || !context.memory.process_heap)
        throw std::invalid_argument("Static CRT finalizers require the canonical process heap");
    bind_pointers(sources, context, std::make_index_sequence<pointers.size()>{});
    sources.bind_initializer({0x433689b6u, &context, initialize_icons});
    sources.bind_initializer({0x433b0bceu, &context, initialize_entities});
}
} //namespace skeet::client
