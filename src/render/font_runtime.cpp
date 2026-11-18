#include "skeet/render/font_runtime.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace skeet::ui {
namespace {
struct GlyphRecord {
    float left, top, right, bottom;
    std::int32_t first, second;
};
struct SharedGlyphLock {
    explicit SharedGlyphLock(SRWLOCK& lock) noexcept : lock_(lock) {
        AcquireSRWLockShared(&lock_);
    }
    ~SharedGlyphLock() { ReleaseSRWLockShared(&lock_); }
    SRWLOCK& lock_;
};
struct ExclusiveGlyphLock {
    explicit ExclusiveGlyphLock(SRWLOCK& lock) noexcept : lock_(lock) {
        AcquireSRWLockExclusive(&lock_);
    }
    ~ExclusiveGlyphLock() { ReleaseSRWLockExclusive(&lock_); }
    SRWLOCK& lock_;
};
std::uint32_t luminance_pixel(std::uint32_t bgra) noexcept {
    if (!bgra)
        return 0;
    const auto blue = bgra & 255;
    const auto green = (bgra >> 8) & 255;
    const auto red = (bgra >> 16) & 255;
    return 0x00ffffffu | (((green * 183 + red * 54 + blue * 19) >> 8) << 24);
}
GlyphCache* metric_cache(FontBackend& backend, std::uint32_t index) {
    auto& fonts = backend.scale == 1.0f ? backend.default_fonts : backend.scaled_fonts;
    if (index >= fonts.size())
        throw std::out_of_range("Font metric index exceeds native font roots");
    return fonts[index].cache;
}
}

struct FontRuntime::Impl {
    HDC dc = nullptr;
    HFONT font = nullptr;
    HBITMAP bitmap = nullptr;
    std::uint32_t* dib = nullptr;
    std::int32_t line_height = 0, text_ascent = 0, inner_leading = 0, outer_leading = 0;
    std::int32_t dib_width = 0, dib_height = 0;
    std::int32_t atlas_width = 0, atlas_height = 0, x = 0, y = 0;
    std::uint32_t flags = 0;
    bool initialized = false;
    std::array<std::uint32_t, 2048> coverage{};
    std::unordered_map<char16_t, GlyphRecord> records;
    std::vector<std::uint32_t> pixels;
    mutable SRWLOCK lock = SRWLOCK_INIT;
    std::atomic_bool dirty = false;

    ~Impl() {
        if (dc)
            DeleteDC(dc);
        if (bitmap)
            DeleteObject(bitmap);
        if (font)
            DeleteObject(font);
    }
    bool covered(char16_t character) const noexcept {
        const auto c = static_cast<std::uint16_t>(character);
        return (coverage[c >> 5] & (std::uint32_t(1) << (c & 31))) != 0;
    }
    bool failed(char16_t character) noexcept {
        const auto c = static_cast<std::uint16_t>(character);
        coverage[c >> 5] &= ~(std::uint32_t(1) << (c & 31));
        return false;
    }
    void reserve_glyph(std::int32_t width) {
        if (x + width >= atlas_width) {
            x = 0;
            y += line_height;
        }
        if (y + line_height >= atlas_height) {
            const auto height = atlas_height ? atlas_height * 2 : atlas_width;
            pixels.resize(static_cast<std::size_t>(atlas_width) * height);
            atlas_height = height;
        }
    }
    void outline(std::int32_t width) {
        // 4332F89F..4332FA69. Black outline pixels never seed another outline.
        for (std::int32_t py = y; py < y + line_height; ++py) {
            for (std::int32_t px = x; px < x + width; ++px) {
                auto& destination = pixels[static_cast<std::size_t>(py) * atlas_width + px];
                if (destination >> 24)
                    continue;
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    for (std::int32_t dy = -1; dy <= 1; ++dy) {
                        if ((!dx && !dy) || px + dx < x || px + dx >= x + width || py + dy < y ||
                            py + dy >= y + line_height)
                            continue;
                        const auto neighbour =
                            pixels[static_cast<std::size_t>(py + dy) * atlas_width + px + dx];
                        if ((neighbour & 255) && (neighbour & 0xff00) && (neighbour & 0xff0000) &&
                            (neighbour & 0xff000000))
                            destination = 0xdc000000;
                    }
                }
            }
        }
    }
    bool create_glyph(char16_t character) { // 4332F22F; batch 433CA690
        if (records.contains(character))
            return true;
        if (!initialized || !covered(character))
            return false;
        ABC abc{};
        const auto c = static_cast<UINT>(character);
        if (flags & 2) {
            if (!GetCharABCWidthsW(dc, c, c, &abc))
                return failed(character);
        } else {
            int width = 0;
            if (!GetCharWidth32W(dc, c, c, &width))
                return failed(character);
            abc.abcB = static_cast<UINT>(width);
        }
        if ((flags & 2) && abc.abcB == 2 && !(flags & 4)) {
            MAT2 matrix{};
            matrix.eM11.value = matrix.eM22.value = 1;
            GLYPHMETRICS gm{};
            const auto length = GetGlyphOutlineW(dc, c, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &matrix);
            if (!length || length == GDI_ERROR || !gm.gmBlackBoxY)
                return failed(character);
            std::vector<std::uint8_t> gray(length);
            const auto written = GetGlyphOutlineW(dc, c, GGO_GRAY8_BITMAP, &gm, length,
                                                  gray.data(), &matrix);
            if (written == GDI_ERROR || written > length || !gm.gmBlackBoxY ||
                !gm.gmBlackBoxX || gm.gmBlackBoxX >= static_cast<UINT>(atlas_width))
                return failed(character);
            const auto pitch = written / gm.gmBlackBoxY;
            if (pitch < gm.gmBlackBoxX)
                return failed(character);
            const auto width = static_cast<std::int32_t>(gm.gmBlackBoxX);
            const auto rows = static_cast<std::int32_t>(
                std::min(gm.gmBlackBoxY, static_cast<UINT>(line_height)));
            const auto offset = std::clamp<std::int32_t>(text_ascent - gm.gmptGlyphOrigin.y,
                                                         0, line_height - rows);
            reserve_glyph(width);
            for (std::int32_t row = 0; row < rows; ++row)
                for (UINT column = 0; column < gm.gmBlackBoxX; ++column) {
                    const auto value = gray[static_cast<std::size_t>(row) * pitch + column];
                    pixels[static_cast<std::size_t>(y + row + offset) *
                               atlas_width +
                           x + column] =
                        value ? 0x00ffffffu | (std::min(std::uint32_t(value) * 4, 255u) << 24) : 0;
                }
            records.emplace(character, GlyphRecord{float(x), float(y), float(x + width),
                                                   float(y + line_height), gm.gmptGlyphOrigin.x,
                                                   gm.gmCellIncX - gm.gmptGlyphOrigin.x});
            x += width;
        } else {
            if (!abc.abcA && !abc.abcB && !abc.abcC)
                return failed(character);
            const std::int32_t border = (flags >> 2) & 1;
            if (abc.abcB >= static_cast<UINT>(atlas_width - border * 2))
                return failed(character);
            const auto width = static_cast<std::int32_t>(abc.abcB) + border * 2;
            reserve_glyph(width);
            std::memset(dib, 0, static_cast<std::size_t>(dib_width) * dib_height * 4);
            if (character != u'\t' && character != u' ') {
                SetBkColor(dc, 0);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, 0xffffff);
                SetTextAlign(dc, 0);
                const wchar_t text = static_cast<wchar_t>(character);
                const auto success =
                    ExtTextOutW(dc, border - abc.abcA, border, 0, nullptr, &text, 1, nullptr);
                GdiFlush();
                if (!success)
                    return failed(character);
                const auto end_column = std::min(width - border, dib_width);
                for (std::int32_t row = border; row < line_height - border; ++row)
                    for (std::int32_t column = border; column < end_column; ++column)
                        pixels[static_cast<std::size_t>(y + row) * atlas_width + x + column] =
                            luminance_pixel(
                                dib[static_cast<std::size_t>(row) * dib_width + column]);
            }
            if (flags & 4)
                outline(width);
            records.emplace(character, GlyphRecord{float(x), float(y), float(x + width),
                                                   float(y + line_height), abc.abcA,
                                                   static_cast<std::int32_t>(abc.abcB) + abc.abcC});
            x += width;
        }
        dirty.store(true, std::memory_order_seq_cst);
        return true;
    }
};

FontRuntime::FontRuntime(std::u16string_view face, std::int32_t height, std::uint32_t flags)
    : impl_(std::make_unique<Impl>()) { // 4334E194 / 4332302B
    auto& state = *impl_;
    cache_.context = this;
    cache_.prepare = [](void* self, char16_t c) {
        return static_cast<FontRuntime*>(self)->prepare(c);
    };
    cache_.height = [](void* self) { return static_cast<FontRuntime*>(self)->height(); };
    cache_.metrics = [](void* self, char16_t c, GlyphMetrics& out) {
        GlyphImage image;
        if (!static_cast<FontRuntime*>(self)->glyph(c, image))
            return false;
        out = {image.advance_first, image.advance_second};
        return true;
    };
    cache_.internal_leading = [](void* self) {
        return static_cast<FontRuntime*>(self)->internal_leading();
    };
    cache_.ascent = [](void* self) { return static_cast<FontRuntime*>(self)->ascent(); };
    cache_.runtime = this;
    state.dc = CreateCompatibleDC(nullptr);
    if (!state.dc)
        return;
    SetMapMode(state.dc, MM_TEXT);
    SetTextAlign(state.dc, 0);
    LOGFONTW description{};
    description.lfHeight = std::bit_cast<std::int32_t>(0u - static_cast<std::uint32_t>(height));
    description.lfWeight = flags & 1 ? 700 : 400;
    description.lfQuality = flags & 2 ? CLEARTYPE_NATURAL_QUALITY : NONANTIALIASED_QUALITY;
    auto count = face.find(u'\0');
    if (count == std::u16string_view::npos)
        count = face.size();
    // wcscpy_s clears its destination when the complete name does not fit.
    if (count < LF_FACESIZE)
        for (std::size_t i = 0; i < count; ++i)
            description.lfFaceName[i] = static_cast<wchar_t>(face[i]);
    state.font = CreateFontIndirectW(&description);
    if (!state.font)
        return;
    SelectObject(state.dc, state.font);
    const auto coverage_bytes = GetFontUnicodeRanges(state.dc, nullptr);
    if (!coverage_bytes)
        return;
    std::vector<std::uint8_t> coverage(coverage_bytes);
    auto* glyphs = reinterpret_cast<GLYPHSET*>(coverage.data());
    glyphs->cbThis = coverage_bytes;
    if (!GetFontUnicodeRanges(state.dc, glyphs))
        return;
    for (DWORD i = 0; i < glyphs->cRanges; ++i) {
        const auto begin = std::uint32_t(glyphs->ranges[i].wcLow);
        const auto end = begin + glyphs->ranges[i].cGlyphs;
        for (auto c = begin; c < end && c < 65536; ++c)
            state.coverage[c >> 5] |= std::uint32_t(1) << (c & 31);
    }
    TEXTMETRICW metrics{};
    if (!GetTextMetricsW(state.dc, &metrics))
        return;
    state.text_ascent = metrics.tmAscent;
    state.inner_leading = metrics.tmInternalLeading;
    state.outer_leading = metrics.tmExternalLeading;
    state.dib_width = metrics.tmMaxCharWidth + (flags & 4 ? 2 : 0);
    state.line_height = state.dib_height = metrics.tmHeight + (flags & 4 ? 2 : 0);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = state.dib_width;
    info.bmiHeader.biHeight = -state.dib_height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    state.bitmap = CreateDIBSection(state.dc, &info, DIB_RGB_COLORS,
                                    reinterpret_cast<void**>(&state.dib), nullptr, 0);
    if (!state.bitmap)
        return;
    std::memset(state.dib, 0, static_cast<std::size_t>(state.dib_width) * state.dib_height * 4);
    SelectObject(state.dc, state.bitmap);
    state.atlas_width = height > 60 ? 2048 : height > 20 ? 1024 : height > 8 ? 512 : 256;
    state.flags = flags;
    state.initialized = true;
}
FontRuntime::~FontRuntime() = default;
bool FontRuntime::valid() const noexcept {
    return impl_->initialized;
}
bool FontRuntime::supports(char16_t c) const noexcept {
    SharedGlyphLock lock(impl_->lock);
    return impl_->covered(c);
}
bool FontRuntime::prepare(char16_t c) {
    auto& state = *impl_;
    {
        SharedGlyphLock lock(state.lock);
        if (state.records.contains(c))
            return true;
        if (!state.covered(c))
            return false;
    }
    ExclusiveGlyphLock lock(state.lock);
    return state.create_glyph(c);
}
void FontRuntime::prepare(std::u16string_view text) {
    for (auto c : text)
        prepare(c);
}
bool FontRuntime::glyph(char16_t c, GlyphImage& out) const noexcept { // 43386484
    const auto& state = *impl_;
    SharedGlyphLock lock(state.lock);
    const auto found = state.records.find(c);
    if (found == state.records.end())
        return false;
    const auto& glyph = found->second;
    out = {glyph.left,
           glyph.top,
           glyph.right,
           glyph.bottom,
           glyph.left / float(state.atlas_width),
           glyph.top / float(state.atlas_height),
           glyph.right / float(state.atlas_width),
           glyph.bottom / float(state.atlas_height),
           glyph.first,
           glyph.second};
    return true;
}
FontRuntime::AtlasRead::AtlasRead(const Impl& state) noexcept : state_(state) {
    AcquireSRWLockShared(&state_.lock);
}
FontRuntime::AtlasRead::~AtlasRead() {
    ReleaseSRWLockShared(&state_.lock);
}
GlyphAtlas FontRuntime::AtlasRead::view() const noexcept {
    return {state_.atlas_width, state_.atlas_height, state_.pixels};
}
FontRuntime::AtlasRead FontRuntime::lock_atlas() const noexcept {
    return AtlasRead(*impl_);
}
void FontRuntime::mark_dirty() noexcept {
    impl_->dirty.store(true, std::memory_order_seq_cst);
} // 433E9F4D
bool FontRuntime::consume_dirty() noexcept {
    return impl_->dirty.exchange(false);
} // 43394E61
GlyphCache& FontRuntime::cache() noexcept {
    return cache_;
}
std::int32_t FontRuntime::height() const noexcept {
    return impl_->line_height;
} // 43416FAF
std::int32_t FontRuntime::ascent() const noexcept {
    return impl_->text_ascent;
} // 4340C42F
std::int32_t FontRuntime::internal_leading() const noexcept {
    return impl_->inner_leading;
} // 43329ACD
std::int32_t FontRuntime::external_leading() const noexcept {
    return impl_->outer_leading;
} // 43327E79
std::int32_t font_internal_leading(FontBackend& backend, std::uint32_t index) { // 433CF7C4
    const auto* cache = metric_cache(backend, index);
    if (!cache)
        return 0;
    if (!cache->internal_leading)
        throw std::logic_error("Font provider has no internal-leading accessor");
    return cache->internal_leading(cache->context);
}
std::int32_t font_ascent(FontBackend& backend, std::uint32_t index) { // 4332917E
    const auto* cache = metric_cache(backend, index);
    if (!cache)
        return 0;
    if (!cache->ascent)
        throw std::logic_error("Font provider has no ascent accessor");
    return cache->ascent(cache->context);
}
} // namespace skeet::ui
