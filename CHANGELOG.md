# Changelog

## 2026-11-18

### Added

- An option to load a saved configuration on startup.
- Copy and paste for menu categories.
- A **Reset** option in the color picker context menu to restore the default color.
- **Open directory** and **Delete** in the Lua script list context menu. **Delete** moves the clicked script to the Recycle Bin without opening File Explorer.
- The **Experimental RAGE** checkbox. Enabling **Extrapolated pose rebuild** is highly recommended.

### Fixed

- Menu and FPS issues.
- Shared ESP decryptor and skin issues.
- Beam traces for grenades in flight.
- Lua compatibility for case-insensitive `ui.set` selections and numeric `entity.set_prop` values.
- Lua stack overflow handling through LuaJIT's internal stack unwinder.
- Native access violations in protected Lua callbacks unwind the active Lua frames before returning to the host.
- Missing symbols and Korean glyphs, concurrent font atlas access, and out-of-bounds glyph writes.

### Performance

- Reduced redundant Windows memory queries during Lua FFI string conversion and access to Lua-owned values.

## Note

Every experimental RAGE change is gated by a checkbox. Turn it off to restore the original RAGE behavior.
