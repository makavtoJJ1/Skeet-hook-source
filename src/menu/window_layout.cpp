#include "skeet/menu_window.hpp"
#include "skeet/popup_menu.hpp"
#include <algorithm>
#include <bit>

namespace skeet::ui {
namespace {
std::int32_t add(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(a) + std::uint32_t(b));
}
}
void MenuWindow::update_dpi(float scale) { // 0x433B23FC
    const auto minimum = scale_size(default_size, scale);
    const auto previous = size_for_persistence();
    const auto scaled = scale_size(previous, scale / input.scale);
    preferred_size_ = {std::max(minimum.x, scaled.x), std::max(minimum.y, scaled.y)};
    geometry.size = preferred_size_;
    input.scale = scale;
    input.slider_metrics = {round_layout_coordinate(40.0f * scale),
                            round_layout_coordinate(200.0f * scale),
                            round_layout_coordinate(100.0f * scale)};
    if (tab_context_menu) {
        tab_context_menu->reset_input();
        tab_context_menu->dpi_changed(scale, input.slider_metrics);
    }
    auto y = 20;
    for (auto*& tab : std::span(tabs)) {
        tab->dpi_changed(scale, input.slider_metrics);
        tab->geometry.bounds.y = y;
        tab->geometry.bounds.x = add(tab->geometry.horizontal_offset, 6);
        y = add(y, tab->geometry.bounds.height);
    }
    for (auto*& tab : std::span(tabs)) {
        tab->layout_categories();
        tab->reflow_categories();
        tab->resize(input.slider_metrics);
    }
    const auto viewport = viewport_;
    viewport_ = {};
    update_viewport(viewport);
}
void MenuWindow::resize(Point size) {
    preferred_size_ = size;
    resize_layout(size);
}
void MenuWindow::resize_layout(Point size) {
    if (size.x == geometry.size.x && size.y == geometry.size.y)
        return;
    geometry.size = size;
    for (auto* tab : tabs)
        tab->resize(input.slider_metrics);
}
void MenuWindow::move(Point next) noexcept {
    position = next;
    centered_ = false;
}
void MenuWindow::restore_size(Point saved, Point viewport) {
    const auto minimum = scale_size(default_size, input.scale);
    // Older source builds wrote window coordinates into these size records.
    if (saved.x < minimum.x || saved.y < minimum.y)
        return;
    resize(saved);
    viewport_ = {};
    update_viewport(viewport);
}
Point MenuWindow::size_for_persistence() const noexcept {
    return preferred_size_.x > 0 && preferred_size_.y > 0 ? preferred_size_ : geometry.size;
}
bool MenuWindow::update_viewport(Point viewport) {
    // Minimized/uninitialized windows must not shrink saved dimensions or
    // consume the first-launch centering request.
    if (viewport.x <= 0 || viewport.y <= 0)
        return false;
    if (!startup_placement_pending && viewport.x == viewport_.x && viewport.y == viewport_.y)
        return true;
    viewport_ = viewport;
    const auto minimum = scale_size(default_size, input.scale);
    const auto preferred = size_for_persistence();
    resize_layout({std::clamp(preferred.x, minimum.x, std::max(viewport.x, minimum.x)),
                   std::clamp(preferred.y, minimum.y, std::max(viewport.y, minimum.y))});
    const Point limit{std::max(0, viewport.x - geometry.size.x),
                      std::max(0, viewport.y - geometry.size.y)};
    if (startup_placement_pending)
        centered_ = true;
    if (centered_)
        position = {limit.x / 2, limit.y / 2};
    else
        position = {std::clamp(position.x, 0, limit.x), std::clamp(position.y, 0, limit.y)};
    startup_placement_pending = false;
    return true;
}
void MenuWindow::refresh_viewport() {
    WindowClientBounds bounds{};
    if (event_services.client_bounds(event_services.context, native_window, bounds))
        update_viewport({bounds.right - bounds.left, bounds.bottom - bounds.top});
}
void MenuWindow::finish_resize() {
    const bool changed = (input.mouse_flags & 8u) && (geometry.size.x != resize_start_size.x ||
                                                      geometry.size.y != resize_start_size.y);
    input.mouse_flags &= ~12u;
    button_flags &= ~1u;
    if (changed && resize_finished.save)
        resize_finished.save(resize_finished.context);
}
void GuiDpiState::refresh(bool force) { // 0x4339F0AD
    if (setting_widget)
        setting_widget->config_loaded();
    const auto requested = scales.at(selection);
    if (!force && requested == active_scale)
        return;
    active_scale = requested;
    if (font_resources.context)
        font_resources.refresh(font_resources.context);
    for (auto* window = windows; window; window = window->next)
        window->update_dpi(active_scale);
}
}
