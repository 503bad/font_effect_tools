#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Resolve a system-font face name (as returned by OBS_PROPERTY_FONT) to an
 * absolute font file path that FreeType can open.
 *
 * out_path must point to a buffer of at least out_path_size bytes.
 * Returns true on success and writes a NUL-terminated UTF-8 path.
 *
 * On Windows, this consults
 *   HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts
 * for "<face> [style] (TrueType|OpenType)" entries and falls back to
 * enumerating %WINDIR%\Fonts and probing each file with FreeType when the
 * registry lookup misses.
 */
bool flametext_resolve_font(const char *face,
			     bool bold,
			     bool italic,
			     char *out_path,
			     size_t out_path_size);

#ifdef __cplusplus
}
#endif
