#include "skeet/menu/window_lifetime.hpp"
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace skeet::ui {
WindowLifetimeImports native_window_lifetime_imports() {
    const auto native = GetModuleHandleA("ntdll.dll");
    const auto release = native ? GetProcAddress(native, "RtlFreeUnicodeString") : nullptr;
    if (!release)
        throw std::runtime_error("Required RtlFreeUnicodeString export is missing");
    return {reinterpret_cast<WindowLifetimeImports::FreeUnicodeString>(release)};
}
void WindowCallbackStorage::initialize(MenuWindow& window) {
    const auto registrations = native_window_handlers();
    handlers_.assign(registrations.begin(), registrations.end());
    handler_buckets_.assign(12, nullptr);
    handler_buckets_.back() = &handler_end_;
    for (auto& handler : handlers_) {
        const auto bucket = handler.key % 11;
        handler.next = handler_buckets_[bucket];
        handler_buckets_[bucket] = &handler;
    }
    notification_buckets_ = {nullptr, &notification_end_};
    window.message_handlers = handler_buckets_;
    window.notifications = notification_buckets_;
}

void WindowCallbackStorage::destroy(MenuWindow& window) noexcept {
    window.message_handlers = {};
    std::vector<WindowMessageHandler*>{}.swap(handler_buckets_);
    std::vector<WindowMessageHandler>{}.swap(handlers_);
    window.notifications = {};
    notification_buckets_[0] = nullptr;
}

void initialize_menu_window(MenuWindow& window, WindowCallbackStorage& callbacks,
                            std::uintptr_t native_window, Point size) { // 0x43485772
    window.native_window = native_window;
    window.next = nullptr;
    window.position = {};
    window.default_size = size;
    window.geometry.size = size;
    window.geometry.start_padding = {7, 10};
    window.geometry.end_padding = {7, 6};
    window.input.mouse_flags = 1;
    window.button_flags = 0;
    window.cursor = {};
    window.tabs.clear();
    // Initialize the shared widths before any category constructs controls.
    // GuiDpiState starts at1.0, so refresh(false) at100% does not run the DPI
    // callback and cannot initialize an otherwise-zero SliderMetrics object.
    window.update_dpi(1.0f);
    window.navigation.active_tab = 0;
    window.transition_started = window.read_ticks(window.clock_context);
    window.transition_state = 2;
    window.opacity = 1.0f;
    callbacks.initialize(window);
}

void destroy_menu_window(MenuWindow& window,
                         WindowCallbackStorage& callbacks) noexcept { // 0x4333F611
    callbacks.destroy(window);
    window.tab_context_menu = nullptr;
    for (auto*& tab : window.tabs) {
        delete tab;
        tab = nullptr;
    }
    std::vector<Tab*>{}.swap(window.tabs);
}
MenuWindow* delete_menu_window(MenuWindow* window, WindowCallbackStorage& callbacks,
                               std::uint8_t flags) noexcept { // 0x43328FEB
    destroy_menu_window(*window, callbacks);
    if (flags & 1)
        delete window;
    return window;
}
void destroy_main_menu_window(MenuWindow& window, WindowCallbackStorage& callbacks,
                              WindowTitleString& title,
                              const WindowLifetimeImports& imports) noexcept { // 0x433E71FA
    if (title.buffer) {
        imports.free_unicode_string(&title);
        title = {};
    }
    destroy_menu_window(window, callbacks);
}
MenuWindow* delete_main_menu_window(MenuWindow* window, WindowCallbackStorage& callbacks,
                                    WindowTitleString& title, const WindowLifetimeImports& imports,
                                    std::uint8_t flags) noexcept { // 0x4334C6CB
    destroy_main_menu_window(*window, callbacks, title, imports);
    if (flags & 1)
        delete window;
    return window;
}
}
