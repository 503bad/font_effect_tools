#include "flametext-ftload.h"

#include <stdio.h>

#include <util/bmem.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

bool flametext_ft_new_face_w(FT_Library lib, const wchar_t *path,
			     long face_index, FT_Face *out_face,
			     void **out_data)
{
	*out_face = NULL;
	*out_data = NULL;

	FILE *f = _wfopen(path, L"rb");
	if (!f)
		return false;

	long size = 0;
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return false;
	}

	unsigned char *buf = bmalloc((size_t)size);
	size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	if (got != (size_t)size) {
		bfree(buf);
		return false;
	}

	FT_Face face;
	if (FT_New_Memory_Face(lib, buf, (FT_Long)size, (FT_Long)face_index,
			       &face) != 0) {
		bfree(buf);
		return false;
	}
	*out_face = face;
	*out_data = buf;
	return true;
}

bool flametext_ft_new_face_utf8(FT_Library lib, const char *utf8_path,
				long face_index, FT_Face *out_face,
				void **out_data)
{
	wchar_t wide[1024];
	if (MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, wide,
				(int)(sizeof(wide) / sizeof(wide[0]))) <= 0) {
		*out_face = NULL;
		*out_data = NULL;
		return false;
	}
	return flametext_ft_new_face_w(lib, wide, face_index, out_face,
				       out_data);
}

#else /* !_WIN32 */

bool flametext_ft_new_face_utf8(FT_Library lib, const char *utf8_path,
				long face_index, FT_Face *out_face,
				void **out_data)
{
	*out_data = NULL;
	return FT_New_Face(lib, utf8_path, (FT_Long)face_index, out_face) == 0;
}

#endif
