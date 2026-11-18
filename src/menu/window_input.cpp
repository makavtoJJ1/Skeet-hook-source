#include "skeet/menu_window.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdexcept>

namespace skeet::ui {
namespace {
template <class Handler>
Handler* find_handler(std::span<Handler*> buckets, std::uint32_t key, std::size_t& bucket) {
    if (buckets.size() < 2 || !buckets.back())
        throw std::logic_error("Window callback table requires buckets and its native end marker");
    bucket = key % (buckets.size() - 1);
    for (auto* handler = buckets[bucket]; handler; handler = handler->next)
        if (handler->key == key)
            return handler;
    bucket = buckets.size() - 1;
    return buckets.back();
}
}
std::uint32_t window_ticks(void*) {
    return GetTickCount();
}

bool MenuWindow::dispatch(WindowMessage& message) { // 0x433BA305
    if (message.input.message == 0xa090) {
        std::size_t bucket = 0;
        auto* handler = find_handler(notifications, message.input.parameter, bucket);
        if (handler == notifications.back())
            return true;
        auto* end = handler->next;
        while (end && end->key == message.input.parameter)
            end = end->next;
        auto end_bucket = bucket;
        while (!end)
            end = notifications[++end_bucket]; // 0x43341DC7
        while (handler != end) {
            handler->callback(*this, message.input.key_data);
            handler = handler->next;
            while (!handler)
                handler = notifications[++bucket];
        }
        return true;
    }
    if (message.input.message == 0x202 && transition_state != 0)
        finish_resize();
    if (startup_placement_pending || !(input.mouse_flags & 1) || transition_state != 0)
        return false;
    auto* focused = tabs.at(navigation.active_tab)->focused_widget();
    if (focused &&
        (focused->kind == 9 || focused->kind == 5 || focused->kind == 10 || focused->kind == 6))
        if (focused->keyboard(message.input))
            return true;
    std::size_t bucket = 0;
    auto* handler = find_handler(message_handlers, message.input.message, bucket);
    if (handler == message_handlers.back())
        return false;
    return handler->callback(*this, message);
}
void MenuWindow::open() { // 0x433AE63F
    if (input.mouse_flags & 1)
        return;
    input.mouse_flags |= 1;
    if (!tabs.empty())
        tabs.at(navigation.active_tab)->activate();
    transition_started = read_ticks(clock_context);
    transition_state = 2;
}
}
