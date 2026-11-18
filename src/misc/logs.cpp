#include "skeet/misc/logs.hpp"
#include "skeet/misc/config.hpp"
#include "skeet/engine/interface_call.hpp"
#include "skeet/script/bridge.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <windows.h>

namespace skeet::misc {
void ColoredLogRing::append_segment(ColoredLogLine& line, std::u16string_view text,
                                    std::uint32_t color) {
    if (line.text.size() + text.size() >= 512)
        return; // native checks before adding its color prefix
    if (text.size() != 1 || text.front() != u'\n') {
        line.text += u'\x07';
        for (unsigned byte = 0; byte < 4; ++byte) {
            const auto component = std::uint8_t(color >> (byte * 8));
            line.text += hex[component >> 4];
            line.text += hex[component & 15];
        }
    }
    line.text.append(text);
}
void ColoredLogRing::append(const char* utf8, std::uint32_t color) {
    if (!*enabled)
        return;
    if (count == 0) {
        if (count != 8)
            ++count;
        head = (head - 1) & 7;
        lines[head].text.clear();
        lines[head].lifetime = 8.0f;
    }
    auto* line = &lines[head & 7];
    std::array<wchar_t, 1024> wide{};
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), static_cast<int>(wide.size()));
    wide.back() = 0;
    auto* current = reinterpret_cast<const char16_t*>(wide.data());
    while (*current) {
        std::u16string_view remaining(current);
        const auto newline = remaining.find(u'\n');
        if (newline == remaining.npos) {
            append_segment(*line, remaining, color);
            line->lifetime = 8.0f;
            return;
        }
        const auto length = newline + 1;
        append_segment(*line, remaining.substr(0, length), color);
        if (!line->text.empty() && line->text.front() != u'\n') {
            if (count != 8)
                ++count;
            head = (head - 1) & 7;
            line = &lines[head];
        }
        line->text.clear();
        line->lifetime = 8.0f;
        current += length;
    }
}
void ErrorLog::write(std::string_view text) {
    format("%.*s", static_cast<int>(text.size()), text.data());
}
void ErrorLog::format(const char* format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    vformat(format, arguments);
    va_end(arguments);
}
void ErrorLog::vformat(const char* format, std::va_list arguments) {
    if (*disabled)
        return;
    std::array<char, 8192> message{};
    const auto length = vsprintf_s(message.data(), message.size(), format, arguments);
    if (length <= 0 || length >= 0x1FFF)
        return;
    message[length] = '\n';
    message[length + 1] = '\0';
    using Print = void (*)(void*, const std::uint32_t*, const char*, ...);
    const std::uint32_t prefix_color = 0xFF1DE6B5, text_color = 0xFF2828FF;
    reinterpret_cast<Print>((*static_cast<void***>(cvar))[25])(cvar, &prefix_color, "%s",
                                                               "[gamesense] ");
    reinterpret_cast<Print>((*static_cast<void***>(cvar))[25])(cvar, &text_color, "%s",
                                                               message.data());
    if (auto* hud = *hud_slot)
        hud->append(message.data(), text_color);
    const auto delta = *current_time - last_sound;
    const auto absolute = std::bit_cast<float>(std::bit_cast<std::uint32_t>(delta) & 0x7FFFFFFFu);
    if (absolute > 5.0f) {
        engine::invoke<void>(surface, 82, "ui/weapon_cant_buy.wav");
        last_sound = *current_time;
    }
}
void NormalLog::write(std::string_view text) {
    format("%.*s", static_cast<int>(text.size()), text.data());
}
namespace {
void print_console(const NormalLog& log, const char* status, std::uint32_t color,
                   const char* format, std::va_list args) {
    if (*log.disabled)
        return;
    std::array<char, 1024> message{};
    const auto count = vsnprintf(message.data(), message.size() - 2, format, args);
    if (count <= 0 || count >= std::int32_t(message.size() - 2))
        return;
    message[std::size_t(count)] = '\n';
    auto* const* table = *static_cast<void***>(log.cvar);
    if (status) {
        using ColorPrint = void (*)(void*, const std::uint32_t*, const char*, ...);
        reinterpret_cast<ColorPrint>(table[25])(log.cvar, &color, "%s", status);
    }
    using Print = void (*)(void*, const char*, ...);
    reinterpret_cast<Print>(table[26])(log.cvar, "[gamesense] %s", message.data());
}
}
void NormalLog::console_format(const char* format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    print_console(*this, nullptr, 0, format, arguments);
    va_end(arguments);
}
void NormalLog::console_status(bool success, const char* format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    print_console(*this, success ? "YES " : "NO ", success ? 0xFF60DC55u : 0xFF5050E6u,
                  format, arguments);
    va_end(arguments);
}
void NormalLog::format(const char* format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    vformat(format, arguments);
    va_end(arguments);
}
void NormalLog::vformat(const char* format, std::va_list arguments) {
    if (*disabled)
        return;
    std::array<char, 8192> message{};
    const auto length = vsprintf_s(message.data(), message.size(), format, arguments);
    if (length <= 0 || length >= 0x1FFF)
        return;
    message[length] = '\n';
    message[length + 1] = '\0';
    using ColorPrint = void (*)(void*, const std::uint32_t*, const char*, ...);
    using Print = void (*)(void*, const char*, ...);
    const std::uint32_t prefix_color = 0xFF1DE6B5;
    reinterpret_cast<ColorPrint>((*static_cast<void***>(cvar))[25])(cvar, &prefix_color, "%s",
                                                                    "[gamesense] ");
    reinterpret_cast<Print>((*static_cast<void***>(cvar))[26])(cvar, "%s", message.data());
    if (script && script::has_output(*script)) {
        message[length] = '\0';
        script::dispatch_output(*script, std::string_view(message.data(), length), 0xFFFFFFFFu);
    } else if (auto* hud = *hud_slot) {
        hud->append(message.data(), 0xC8FFFFFFu);
    }
}
void bind_log_configuration(const HideFromObsConfig& config, ErrorLog& error,
                            NormalLog& normal) noexcept {
    error.disabled = &config.enabled.value;
    normal.disabled = &config.enabled.value;
}
void bind_log_hud(ColoredLogRing* const& slot, ErrorLog& error, NormalLog& normal) noexcept {
    error.hud_slot = &slot;
    normal.hud_slot = &slot;
}
} // namespace skeet::misc
