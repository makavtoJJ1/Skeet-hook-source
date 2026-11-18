#include "skeet/render/backend.hpp"
#include "skeet/render/menu_background.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <intrin.h>

namespace skeet::ui {
namespace {
std::uint32_t native_color(std::uint32_t value) noexcept {
    return (value & 0xff00ff00U) | ((value & 0xff) << 16) | ((value >> 16) & 0xff);
}
std::int32_t add(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(a) + std::uint32_t(b));
}
std::int32_t subtract(std::int32_t a, std::int32_t b) noexcept {
    return std::bit_cast<std::int32_t>(std::uint32_t(a) - std::uint32_t(b));
}
std::int32_t truncate(float value) noexcept {
    if (!(value >= -2147483648.0f && value < 2147483648.0f))
        return INT32_MIN;
    return static_cast<std::int32_t>(value);
}
template <class T> T read(std::span<const std::byte> bytes, std::size_t offset = 0) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
        throw std::invalid_argument("Truncated render command");
    T result;
    std::memcpy(&result, bytes.data() + offset, sizeof(result));
    return result;
}
}

RendererBackend::RendererBackend(IDirect3DDevice9* device) : device_(device) {
    create_menu_background(*this);
}
RendererBackend::~RendererBackend() {
    wait_for_render();
    release_device_objects();
}
void RendererBackend::wait_for_render() {
    // 4340AF6A waits at most one second for the render-thread critical region.
    for (unsigned i = 0; i < 100 && rendering_.load(std::memory_order_acquire); ++i)
        Sleep(10);
    rendering_.store(false, std::memory_order_release);
}

// 433E9B63: dynamic default-pool buffer, 1024 vertices per growth.
HRESULT RendererBackend::lock_buffer() {
    std::uint32_t bytes = capacity_ * sizeof(RenderVertex);
    if (!buffer_ || grow_) {
        if (grow_) {
            grow_ = false;
            bytes += 0x6000;
        }
        IDirect3DVertexBuffer9* next = nullptr;
        const auto result =
            device_->CreateVertexBuffer(bytes, 0x208, 0x142, D3DPOOL_DEFAULT, &next, nullptr);
        if (FAILED(result))
            return result;
        if (buffer_)
            buffer_->Release();
        buffer_ = next;
        capacity_ = bytes / sizeof(RenderVertex);
    }
    const auto result = buffer_->Lock(
        first_ * sizeof(RenderVertex), bytes - first_ * sizeof(RenderVertex),
        reinterpret_cast<void**>(&mapped_), first_ ? D3DLOCK_NOOVERWRITE : D3DLOCK_DISCARD);
    return FAILED(result) ? result : S_OK;
}

// 4339E532: preserve the native flush order on capacity/texture/type changes.
bool RendererBackend::prepare(D3DPRIMITIVETYPE primitive, IDirect3DBaseTexture9* texture,
                              std::uint32_t count, bool connected) {
    if (first_ + count_ + count >= capacity_) {
        grow_ = true;
        flush(true);
    }
    if (texture_ != texture) {
        flush();
        device_->SetTexture(0, texture);
        texture_ = texture;
    }
    if (primitive_ != primitive) {
        flush();
        primitive_ = primitive;
    }
    if (connected_ != connected) {
        flush();
        connected_ = connected;
    }
    if (!mapped_ && FAILED(lock_buffer()))
        return false;
    return true;
}

// Raw unconnected strips retain their original four-vertex format. Rectangle,
// texture and text commands write quads as triangle lists so they can batch.
void RendererBackend::draw(std::uint32_t count, std::uint32_t first) {
    device_->SetStreamSource(0, buffer_, 0, sizeof(RenderVertex));
    if (primitive_ == D3DPT_TRIANGLESTRIP && !connected_) {
        for (std::uint32_t at = 0; at < count; at += 4)
            device_->DrawPrimitive(D3DPT_TRIANGLESTRIP, first + at, 2);
        return;
    }
    auto primitives = count;
    switch (primitive_) {
    case D3DPT_POINTLIST:
        break;
    case D3DPT_LINELIST:
        primitives >>= 1;
        break;
    case D3DPT_LINESTRIP:
        --primitives;
        break;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:
        primitives -= 2;
        break;
    default:
        primitives /= 3;
        break;
    }
    device_->DrawPrimitive(primitive_, first, primitives);
}

void RendererBackend::flush(bool reset_start) {
    if (mapped_) {
        if (alpha_ != 1.0f) {
            for (std::uint32_t i = 0; i < count_; ++i) {
                auto& color = mapped_[i].color;
                const auto alpha = static_cast<std::uint8_t>(truncate(float(color >> 24) * alpha_));
                color = (color & 0x00ffffff) | (std::uint32_t(alpha) << 24);
            }
        }
        buffer_->Unlock();
        mapped_ = nullptr;
    }
    if (count_) {
        draw(count_, first_);
        first_ += count_;
        count_ = 0;
    }
    if (reset_start || first_ + 2 >= capacity_)
        first_ = 0;
}

void RendererBackend::vertex(float x, float y, std::uint32_t color, float u, float v) {
    mapped_[count_++] = {x, y, depth_, color, u, v};
}
bool RendererBackend::quad(IDirect3DBaseTexture9* texture, float left, float top,
                           float right, float bottom, const std::array<std::uint32_t, 4>& colors,
                           float u0, float v0, float u1, float v1) {
    if (!prepare(D3DPT_TRIANGLELIST, texture, 6, false))
        return false;
    // Keep the original strip's diagonal, winding, colors and UVs. Adjacent
    // quads with the same state now share one DrawPrimitive call.
    vertex(left, top, colors[0], u0, v0);
    vertex(right, top, colors[1], u1, v0);
    vertex(left, bottom, colors[2], u0, v1);
    vertex(left, bottom, colors[2], u0, v1);
    vertex(right, top, colors[1], u1, v0);
    vertex(right, bottom, colors[3], u1, v1);
    return true;
}
bool RendererBackend::append_vertices(D3DPRIMITIVETYPE primitive, IDirect3DBaseTexture9* texture,
                                      std::span<const RenderVertex> vertices, bool connected) {
    if (!prepare(primitive, texture, static_cast<std::uint32_t>(vertices.size()), connected))
        return false;
    if (!vertices.empty())
        std::memcpy(mapped_ + count_, vertices.data(), vertices.size_bytes());
    count_ += static_cast<std::uint32_t>(vertices.size());
    return true;
}

void RendererBackend::rectangle(RectangleCommand c) { // 433E2C3F -> 433E99C8
    const auto color = native_color(c.color);
    const auto right = float(add(c.x, c.width)), bottom = float(add(c.y, c.height));
    quad(nullptr, float(c.x), float(c.y), right, bottom, {color, color, color, color});
}
void RendererBackend::four_color_gradient(FourColorGradientCommand c) { // 433D0FEC
    const auto right = float(add(c.x, c.width)), bottom = float(add(c.y, c.height));
    quad(nullptr, float(c.x), float(c.y), right, bottom,
         {native_color(c.colors[0]), native_color(c.colors[1]), native_color(c.colors[3]),
          native_color(c.colors[2])});
}
void RendererBackend::vertical_gradient(GradientCommand c) { // 43384795
    four_color_gradient({c.x,
                         c.y,
                         c.width,
                         c.height,
                         {c.first_color, c.first_color, c.second_color, c.second_color}});
}
void RendererBackend::horizontal_gradient(GradientCommand c) { // 4333C85C
    four_color_gradient({c.x,
                         c.y,
                         c.width,
                         c.height,
                         {c.first_color, c.second_color, c.second_color, c.first_color}});
}
void RendererBackend::line(LineCommand c) { // 433E7CEE
    if (!prepare(D3DPT_LINELIST, nullptr, 2, false))
        return;
    const auto color = native_color(c.color);
    vertex(float(c.from_x), float(c.from_y), color);
    vertex(float(c.to_x), float(c.to_y), color);
}
void RendererBackend::polyline(std::span<const LinePoint> points, std::uint32_t color) { // 4333088F
    if (!prepare(D3DPT_LINESTRIP, nullptr, static_cast<std::uint32_t>(points.size()), false))
        return;
    color = native_color(color);
    for (const auto& point : points)
        vertex(float(point.x), float(point.y), color);
}
void RendererBackend::triangle(TriangleCommand c) { // 43387AD9
    if (!prepare(D3DPT_TRIANGLELIST, nullptr, 3, false))
        return;
    const auto color = native_color(c.color);
    vertex(float(c.first_x), float(c.first_y), color);
    vertex(float(c.second_x), float(c.second_y), color);
    vertex(float(c.third_x), float(c.third_y), color);
}
void RendererBackend::colored(std::span<const ColoredVertex> vertices, D3DPRIMITIVETYPE type,
                              bool connected) {
    if (!prepare(type, nullptr, static_cast<std::uint32_t>(vertices.size()), connected))
        return;
    for (const auto& value : vertices)
        vertex(value.x, value.y, native_color(value.color));
}
void RendererBackend::triangle_list(std::span<const ColoredVertex> vertices) { // 4334E741
    colored(vertices, D3DPT_TRIANGLELIST, false);
}
void RendererBackend::triangle_strip(std::span<const ColoredVertex> vertices) { // 434008C4
    if (vertices.size() >= 3)
        colored(vertices, D3DPT_TRIANGLESTRIP, true);
}
void RendererBackend::colored_primitives(std::uint32_t type,
                                         std::span<const ColoredVertex> vertices) { // 43337823
    static constexpr D3DPRIMITIVETYPE types[]{D3DPT_POINTLIST,    D3DPT_LINELIST,
                                              D3DPT_TRIANGLELIST, D3DPT_TRIANGLESTRIP,
                                              D3DPT_LINESTRIP,    static_cast<D3DPRIMITIVETYPE>(0),
                                              D3DPT_TRIANGLESTRIP};
    if (type >= 7 || !types[type])
        return;
    while (!vertices.empty()) {
        const auto count = std::min<std::size_t>(capacity_, vertices.size());
        if (count == 0)
            return;
        colored(vertices.first(count), types[type], type == 3);
        vertices = vertices.subspan(count);
    }
}

void RendererBackend::set_bounds(const std::array<std::int32_t, 4>& bounds) { // 4334F405
    flush();
    const RECT rectangle{bounds[0], bounds[1], add(bounds[0], bounds[2]),
                         add(bounds[1], bounds[3])};
    device_->SetScissorRect(&rectangle);
}
std::array<std::int32_t, 4> RendererBackend::get_bounds() const { // 43368A1E
    RECT rectangle{};
    if (FAILED(device_->GetScissorRect(&rectangle)))
        return {};
    return {rectangle.left, rectangle.top, subtract(rectangle.right, rectangle.left),
            subtract(rectangle.bottom, rectangle.top)};
}
bool RendererBackend::is_bounds_reset() const { // 433EB732
    DWORD enabled = 0;
    device_->GetRenderState(D3DRS_SCISSORTESTENABLE, &enabled);
    return enabled != 0;
}
void RendererBackend::reset_bounds(bool enabled) { // 433ED99E
    if (is_bounds_reset() != enabled) {
        flush();
        device_->SetRenderState(D3DRS_SCISSORTESTENABLE, enabled);
    }
}
void RendererBackend::set_alpha(float value) { // 433AEA69
    // Native minimum_number/maximum_number select the numeric operand for NaN.
    value = std::fmax(std::fmin(value, 1.0f), 0.0f);
    if (alpha_ != value)
        flush();
    alpha_ = value;
}
void RendererBackend::set_depth(float value) noexcept {
    depth_ = value;
} // 433EBBA6

void RendererBackend::texture(TextureCommand c) { // 43368EBB -> 433CCC5C
    if (c.texture == UINT32_MAX || c.texture >= textures_.size())
        return;
    const auto& record = textures_[c.texture];
    if (!record.texture)
        return;
    auto width = c.source_width, height = c.source_height;
    if (c.scale != 1.0f) {
        width = truncate(float(width) * c.scale);
        height = truncate(float(height) * c.scale);
    }
    float u0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    if (c.mode == 2 ||
        (c.mode == 0 && c.source_width > record.width && c.source_height > record.height)) {
        u1 = float(c.source_width) / float(record.width);
        v1 = float(c.source_height) / float(record.height);
    } else if (c.mode != 1 && !(c.mode == 0 && c.source_width == record.width &&
                                c.source_height == record.height)) {
        u0 = float(c.source_x) / float(record.width);
        u1 = float(add(c.source_x, c.source_width)) / float(record.width);
    }
    const auto color = native_color(c.color);
    const auto right = float(add(c.x, width)), bottom = float(add(c.y, height));
    quad(record.texture, float(c.x), float(c.y), right, bottom, {color, color, color, color},
         u0, 0, u1, v1);
}

void RendererBackend::consume(std::span<const std::byte> commands) { // 4338AD28..4338AD51
    reset_bounds(false);
    set_depth(0.5f);
    set_alpha(1.0f);
    for (std::size_t offset = 0; offset < commands.size();) {
        const auto header = read<std::uint32_t>(commands, offset);
        const auto end = header & 0x07ffffffU;
        if (end <= offset + 3 || end > commands.size())
            throw std::invalid_argument("Invalid render command end");
        const auto payload = commands.subspan(offset + 4, end - offset - 4);
        switch (header >> 27) {
        case 0:
            rectangle(read<RectangleCommand>(payload));
            break;
        case 1:
            four_color_gradient(read<FourColorGradientCommand>(payload));
            break;
        case 2:
            vertical_gradient(read<GradientCommand>(payload));
            break;
        case 3:
            horizontal_gradient(read<GradientCommand>(payload));
            break;
        case 4:
            line(read<LineCommand>(payload));
            break;
        case 5: {
            const auto count = read<std::uint32_t>(payload, 4);
            if (std::uint64_t(count) * 8 + 8 > payload.size())
                throw std::invalid_argument("Truncated polyline");
            std::vector<LinePoint> points(count);
            if (count)
                std::memcpy(points.data(), payload.data() + 8, count * 8);
            polyline(points, read<std::uint32_t>(payload));
            break;
        }
        case 6:
            triangle(read<TriangleCommand>(payload));
            break;
        case 7:
        case 8: {
            const auto count = read<std::uint32_t>(payload);
            if (std::uint64_t(count) * 12 + 4 > payload.size())
                throw std::invalid_argument("Truncated colored vertices");
            std::vector<ColoredVertex> values(count);
            if (count)
                std::memcpy(values.data(), payload.data() + 4, count * 12);
            if (header >> 27 == 7)
                triangle_list(values);
            else
                triangle_strip(values);
            break;
        }
        case 9:
            circle(read<CircleCommand>(payload));
            break;
        case 10: {
            const auto count = read<std::uint16_t>(payload, 20);
            if (std::size_t(count) * 2 + 22 > payload.size())
                throw std::invalid_argument("Truncated text");
            std::u16string value(count, u'\0');
            if (count)
                std::memcpy(value.data(), payload.data() + 22, count * 2);
            text(read<std::int32_t>(payload), read<std::int32_t>(payload, 4),
                 read<std::uint32_t>(payload, 8), read<std::uint32_t>(payload, 12),
                 read<std::int32_t>(payload, 16), value);
            break;
        }
        case 11:
            texture(read<TextureCommand>(payload));
            break;
        case 12:
            set_bounds(read<std::array<std::int32_t, 4>>(payload));
            break;
        case 13:
            reset_bounds(read<std::uint8_t>(payload) != 0);
            break;
        case 14:
            set_alpha(read<float>(payload));
            break;
        case 15:
            set_depth(read<float>(payload));
            break;
        case 16:
            circle_outline(read<CircleOutlineCommand>(payload));
            break;
        case 17: {
            const auto count = read<std::uint32_t>(payload, 4);
            if (std::uint64_t(count) * 12 + 8 > payload.size())
                throw std::invalid_argument("Truncated primitive vertices");
            std::vector<ColoredVertex> values(count);
            if (count)
                std::memcpy(values.data(), payload.data() + 8, count * 12);
            colored_primitives(read<std::uint32_t>(payload), values);
            break;
        }
        default:
            throw std::invalid_argument("Unknown render opcode");
        }
        offset = end;
    }
}
bool RendererBackend::render(const RenderQueue& queue) {
    if (queue.commands.empty() || !begin_frame())
        return false;
    consume(queue.commands);
    end_frame();
    return true;
}

} // namespace skeet::ui
