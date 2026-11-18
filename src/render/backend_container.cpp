#include "skeet/render/backend_container.hpp"
#include <stdexcept>

namespace skeet::ui {
namespace {
std::atomic_flag publication_lock = ATOMIC_FLAG_INIT; // 43475BC0, 433B32F6/433CCB51
struct PublicationGuard {
    PublicationGuard() {
        while (publication_lock.test_and_set(std::memory_order_acquire)) {
        }
    }
    ~PublicationGuard() {
        publication_lock.clear(std::memory_order_release);
    }
};
struct FontExclusiveGuard {
    explicit FontExclusiveGuard(SRWLOCK& value) : lock(&value) {
        AcquireSRWLockExclusive(lock);
    }
    ~FontExclusiveGuard() {
        ReleaseSRWLockExclusive(lock);
    }
    SRWLOCK* lock;
};
void link_font_fallbacks(FontSet& fonts) noexcept {
    // See startup_font_recipes for the seven font slots.
    for (auto& font : fonts)
        font.next = nullptr;
    fonts[0].next = fonts[1].next = fonts[4].next = &fonts[3];
    fonts[3].next = fonts[5].next = &fonts[6];
    fonts[2].next = &fonts[5];
}
std::int32_t font_height(std::int32_t height, float scale) {
    const auto value = float(height) * scale + 0.5f;
    if (!(value >= -2147483648.0f && value < 2147483648.0f))
        return INT32_MIN;
    return static_cast<std::int32_t>(value);
}
}
RendererContainer::RendererContainer(IDirect3DDevice9* device)
    : backend_(std::make_unique<RendererBackend>(device)) {
    backend_->fonts = &fonts;
    for (auto& frame : frames_) {
        frame.queue.fonts = &fonts;
        frame.queue.renderer_ = this;
    }
}
RendererContainer::~RendererContainer() {
    destroy();
}
void RendererContainer::configure_fonts(std::span<const FontDefinition, 7> definitions) {
    FontExclusiveGuard lock(font_lock_);
    for (std::size_t i = 0; i < 7; ++i) {
        font_definitions[i] = definitions[i];
        auto runtime = std::make_unique<FontRuntime>(definitions[i].face, definitions[i].height,
                                                     definitions[i].flags);
        if (!runtime->valid())
            throw std::runtime_error("Native font creation failed");
        default_fonts[i] = std::move(runtime);
        fonts.default_fonts[i].cache = &default_fonts[i]->cache();
    }
    link_font_fallbacks(fonts.default_fonts);
    fonts_configured_ = true;
}
void RendererContainer::set_font_scale(float scale) noexcept {
    requested_scale_.store(scale, std::memory_order_relaxed);
} // 433EA298
float RendererContainer::requested_font_scale() const noexcept {
    return requested_scale_.load(std::memory_order_relaxed);
}
void RendererContainer::refresh_fonts() { // 433C95EA..433C96CB
    const auto requested = requested_font_scale();
    if (fonts.scale == requested)
        return;
    FontExclusiveGuard lock(font_lock_);
    if (requested != 1.0f) {
        if (!fonts_configured_)
            throw std::logic_error("Seven native font definitions are required");
        for (std::size_t i = 0; i < 7; ++i) {
            const auto& definition = font_definitions[i];
            auto runtime = std::make_unique<FontRuntime>(
                definition.face, font_height(definition.height, requested), definition.flags);
            if (!runtime->valid())
                throw std::runtime_error("Native scaled font creation failed");
            scaled_fonts[i] = std::move(runtime);
            auto& link = fonts.scaled_fonts[i];
            link.cache = &scaled_fonts[i]->cache();
            for (auto& gpu : backend_->font_textures_)
                if (gpu.link == &link && gpu.texture) {
                    gpu.texture->Release();
                    gpu.texture = nullptr;
                }
        }
        link_font_fallbacks(fonts.scaled_fonts);
    }
    fonts.scale = requested;
}
RenderQueue* RendererContainer::begin() {
    active_ = nullptr;
    for (auto& frame : frames_) {
        std::uint32_t expected = 0;
        if (frame.state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            frame.queue.renderer_ = this;
            frame.queue.commands.clear();
            frame.queue.desaturate = false;
            frame.queue.bounds_reset = false;
            frame.queue.depth = 0.5f;
            active_ = &frame;
            AcquireSRWLockShared(&font_lock_);
            return &frame.queue;
        }
    }
    return nullptr;
}
void RendererContainer::publish() {
    if (!active_)
        return;
    ReleaseSRWLockShared(&font_lock_);
    auto* frame = active_;
    active_ = nullptr;
    auto next = std::make_shared<PublishedFrame>(*frame);
    frame->state.exchange(2, std::memory_order_acq_rel);
    {
        PublicationGuard lock;
        published_.swap(next);
    }
}
bool RendererContainer::draw() {
    std::shared_ptr<PublishedFrame> frame;
    {
        PublicationGuard lock;
        frame = published_;
    }
    if (!frame || !backend_ || frame->frame->queue.commands.empty())
        return false;
    refresh_fonts();
    return backend_->render(frame->frame->queue);
}
void RendererContainer::release_context() { // 433F529D / 4340AF6A
    if (!backend_)
        return;
    backend_->wait_for_render();
    backend_->release_device_objects();
    for (std::size_t i = 7; i != 0; --i)
        scaled_fonts[i - 1].reset();
    for (std::size_t i = 7; i != 0; --i)
        default_fonts[i - 1].reset();
    backend_.reset();
    for (std::size_t i = 7; i != 0; --i)
        std::u16string().swap(font_definitions[i - 1].face);
}
void RendererContainer::destroy() {
    // Native publication guard is released first, then queue storage in the
    // order +4C,+38,+24,+10. Context cleanup follows those four vector frees.
    published_.reset();
    for (std::size_t i = frames_.size(); i != 0; --i)
        std::vector<std::byte>().swap(frames_[i - 1].queue.commands);
    release_context();
    for (auto& frame : frames_)
        frame.queue.renderer_ = nullptr;
    active_ = nullptr;
}
} // namespace skeet::ui
