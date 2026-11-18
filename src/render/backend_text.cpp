#include "skeet/render/backend.hpp"
#include "skeet/render/font_runtime.hpp"
#include "skeet/core.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace skeet::ui {
namespace {
std::int32_t add_text(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(a) + std::uint32_t(b));
}
std::int32_t subtract_text(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(a) - std::uint32_t(b));
}
std::uint32_t text_color(std::uint32_t value) noexcept {
    return (value & 0xff00ff00u) | ((value & 255) << 16) | ((value >> 16) & 255);
}
FontLink* image_font(FontLink& root, char16_t c, GlyphImage& out) {
    for (auto* link = &root; link; link = link->next) {
        if (!link->cache)
            continue;
        if (!link->cache->runtime)
            throw std::logic_error("Text drawing requires a concrete font runtime");
        if (link->cache->runtime->glyph(c, out))
            return link;
    }
    return nullptr;
}
Point text_extent(FontLink& root, std::u16string_view text) { // 4338AA26
    const auto height = root.cache ? root.cache->runtime->height() : 0;
    Point result{0, height};
    std::int32_t width = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == u'\n') {
            width = 0;
            result.y = add_text(result.y, height);
        } else if (text[i] == u'\a' && text.size() - i > 8)
            i += 8;
        else {
            GlyphImage glyph;
            if (image_font(root, text[i], glyph)) {
                width = add_text(width, add_text(glyph.advance_first, glyph.advance_second));
                if (width > result.x)
                    result.x = width;
            }
        }
    }
    return result;
}
}

const RendererBackend::FontTexture* RendererBackend::font_texture(const FontLink& link) const noexcept {
    const auto at = std::find_if(font_textures_.begin(), font_textures_.end(),
                                 [&](const auto& record) { return record.link == &link; });
    return at == font_textures_.end() || !at->texture ? nullptr : &*at;
}
void RendererBackend::upload_fonts() { // 433C96CF..433C973A / 4334A887
    if (!fonts)
        return;
    auto upload = [&](FontSet& set) {
        for (auto& root : set) {
            if (!root.cache)
                continue;
            auto* font = root.cache->runtime;
            if (!font)
                throw std::logic_error("Font uploading requires a concrete font runtime");
            if (!font->consume_dirty())
                continue;
            auto at = std::find_if(font_textures_.begin(), font_textures_.end(),
                                   [&](const auto& record) { return record.link == &root; });
            if (at == font_textures_.end()) {
                font_textures_.push_back({&root, nullptr});
                at = font_textures_.end() - 1;
            }
            const auto atlas_read = font->lock_atlas();
            const auto atlas = atlas_read.view();
            IDirect3DTexture9* uploaded = nullptr;
            if (SUCCEEDED(upload_texture(atlas.width, atlas.height, std::as_bytes(atlas.pixels),
                                         &uploaded))) {
                if (at->texture)
                    at->texture->Release();
                at->texture = uploaded;
                at->width = atlas.width;
                at->height = atlas.height;
            } else {
                font->mark_dirty();
            }
        }
    };
    upload(fonts->default_fonts);
    if (fonts->scale != 1.0f)
        upload(fonts->scaled_fonts);
}

void RendererBackend::text(std::int32_t x, std::int32_t y, std::uint32_t color, std::uint32_t flags,
                           std::int32_t maximum_width,
                           std::u16string_view content) { // 4339D0B8 / 4334173D
    if (!fonts)
        return;
    auto& roots = select_fonts(*fonts, flags);
    const auto index = (flags & 2) ? 2 : (flags & 1) ? 1 : (flags & 4) ? 4 : 0;
    auto& root = roots[index];
    if (!root.cache)
        return;
    if (!root.cache->runtime)
        throw std::logic_error("Text drawing requires a concrete font runtime");
    if ((flags & 0x104) == 4 && fonts->scale != 1.0f)
        flags |= 8;
    if (flags & 0x90) {
        auto size = text_extent(root, content);
        if (maximum_width && size.x > maximum_width)
            size.x = maximum_width;
        if (flags & 0x10) {
            x = subtract_text(x, size.x / 2);
            y = subtract_text(y, size.y / 2);
        } else if (flags & 0x80)
            x = subtract_text(x, size.x);
    }
    auto pass = [&](std::int32_t origin_x, std::int32_t origin_y, std::uint32_t argb,
                    bool shadow) { // 4332667C
        auto pen_x = origin_x, pen_y = origin_y;
        const auto limit = maximum_width ? add_text(origin_x, maximum_width)
                                         : std::numeric_limits<std::int32_t>::max();
        const auto line_height = root.cache->runtime->height();
        for (std::size_t i = 0; i < content.size(); ++i) {
            const auto character = content[i];
            if (character == u'\n') {
                pen_x = origin_x;
                pen_y = add_text(pen_y, line_height);
            } else if (character == u'\a' && content.size() - i > 8) {
                if (!shadow) {
                    char digits[9]{};
                    for (std::size_t j = 0; j < 8; ++j)
                        digits[j] = static_cast<char>(content[i + 1 + j]);
                    const auto rgba = static_cast<std::uint32_t>(std::strtoul(digits, nullptr, 16));
                    argb = (rgba >> 8) | (rgba << 24);
                }
                i += 8;
            } else {
                GlyphImage glyph;
                auto* font = image_font(root, character, glyph);
                if (!font)
                    continue;
                auto* texture = font_texture(*font);
                if (!texture || glyph.right > float(texture->width) ||
                    glyph.bottom > float(texture->height))
                    continue;
                const auto draw_x = add_text(pen_x, glyph.advance_first);
                const auto next = add_text(draw_x, glyph.advance_second);
                if (next >= limit)
                    return;
                const auto left = float(draw_x), top = float(pen_y);
                const auto right = (left + glyph.right) - glyph.left;
                const auto bottom = (top + glyph.bottom) - glyph.top;
                // The producer may have grown the CPU atlas after upload.
                const auto width = float(texture->width), height = float(texture->height);
                if (quad(texture->texture, left, top, right, bottom, {argb, argb, argb, argb},
                         glyph.left / width, glyph.top / height, glyph.right / width,
                         glyph.bottom / height))
                    pen_x = next;
            }
        }
    };
    if (flags & 8) {
        const auto shadow_alpha = std::min(color >> 24, 128u);
        pass(add_text(x, 1), add_text(y, 1), (shadow_alpha << 24) | 0x010101u, true);
    } else if (flags & 0x20) {
        const auto outline_color = (color & 0xff000000u) | 0x0c0c0cu;
        constexpr std::array<Point, 4> offsets{{{1, 1}, {-1, -1}, {-1, 1}, {1, -1}}};
        for (const auto offset : offsets)
            pass(add_text(x, offset.x), add_text(y, offset.y), outline_color, true);
    }
    pass(x, y, text_color(color), false);
}
} // namespace skeet::ui
