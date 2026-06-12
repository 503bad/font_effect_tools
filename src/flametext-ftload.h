#pragma once

#include <stdbool.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#ifdef _WIN32
#include <wchar.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Open a FreeType face from a UTF-8 file path.
 *
 * FT_New_Face() opens the file with fopen(), which on Windows interprets the
 * path in the ANSI code page — a UTF-8 path containing non-ASCII characters
 * (e.g. a Japanese font filename) fails to open. These helpers read the file
 * through the wide-character API instead and hand the bytes to
 * FT_New_Memory_Face().
 *
 * On success *out_face is the new face and *out_data is the file buffer
 * backing it; the buffer must stay alive until FT_Done_Face(), then be
 * released with bfree(). On non-Windows platforms *out_data is NULL
 * (bfree(NULL) is a no-op, so callers need no special casing).
 */
bool flametext_ft_new_face_utf8(FT_Library lib, const char *utf8_path,
				long face_index, FT_Face *out_face,
				void **out_data);

#ifdef _WIN32
/* Same, from a wide-character path. */
bool flametext_ft_new_face_w(FT_Library lib, const wchar_t *path,
			     long face_index, FT_Face *out_face,
			     void **out_data);
#endif

#ifdef __cplusplus
}
#endif
