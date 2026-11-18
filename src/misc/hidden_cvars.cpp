#include "skeet/misc/hidden_cvars.hpp"
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace skeet::misc {
namespace {
constexpr std::uint32_t hidden_flags = (1u << 1) | (1u << 4);
}
HiddenCvars::~HiddenCvars() {
    if (cvar && enabled_)
        set_enabled(false);
}
void HiddenCvars::set_enabled(bool enabled) {
    if (enabled == enabled_)
        return;
    if (!cvar)
        throw std::logic_error("Hidden cvars require the engine's VEngineCvar007 interface");
    ConsoleCommandBase* first;
    // vstdlib!CCvar::UnregisterConCommands: head at +30, next at +4.
    std::memcpy(&first, static_cast<const std::byte*>(cvar) + 0x30, sizeof(first));
    update(first, enabled);
    enabled_ = enabled;
}
void HiddenCvars::update(ConsoleCommandBase* first, bool enabled) {
    for (auto* command = first; command; command = command->next) {
        if (!command->name)
            continue;
        if (enabled) {
            const auto flags = command->flags & hidden_flags;
            if (!flags)
                continue;
            auto [entry, inserted] = removed_.try_emplace(
                command, RemovedFlags{command->name, flags});
            if (!inserted) {
                if (entry->second.name != command->name)
                    entry->second = {command->name, flags};
                else
                    entry->second.flags |= flags;
            }
            command->flags &= ~hidden_flags;
        } else {
            const auto found = removed_.find(command);
            // Restore only still-registered objects, preserving unrelated flag changes.
            if (found != removed_.end() && found->second.name == command->name)
                command->flags |= found->second.flags;
        }
    }
    if (!enabled)
        removed_.clear();
}
#if UINTPTR_MAX == UINT32_MAX
static_assert(offsetof(ConsoleCommandBase, next) == 4);
static_assert(offsetof(ConsoleCommandBase, name) == 0xC);
static_assert(offsetof(ConsoleCommandBase, flags) == 0x14);
#endif
}
