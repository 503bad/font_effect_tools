#include "flametext-font-resolve.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_IDS_H

#include <obs.h>
#include <plugin-support.h>

static const wchar_t *FONTS_REGISTRY_KEY = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";

static bool utf8_to_wide(const char *in, wchar_t *out, int out_chars)
{
	return MultiByteToWideChar(CP_UTF8, 0, in, -1, out, out_chars) > 0;
}

static bool wide_to_utf8(const wchar_t *in, char *out, int out_bytes)
{
	return WideCharToMultiByte(CP_UTF8, 0, in, -1, out, out_bytes, NULL, NULL) > 0;
}

/* %WINDIR%\Fonts — used to resolve bare filenames in HKLM. */
static void get_system_fonts_dir(wchar_t *out, size_t out_chars)
{
	out[0] = 0;
	wchar_t buf[MAX_PATH];
	if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_FONTS, NULL, SHGFP_TYPE_CURRENT, buf)))
		wcsncpy_s(out, out_chars, buf, _TRUNCATE);
}

/* %LOCALAPPDATA%\Microsoft\Windows\Fonts — per-user "Install for me only"
 * fonts since Windows 10 1809. Used to resolve bare filenames in HKCU. */
static void get_user_fonts_dir(wchar_t *out, size_t out_chars)
{
	out[0] = 0;
	wchar_t local[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return;
	_snwprintf_s(out, out_chars, _TRUNCATE,
		     L"%s\\Microsoft\\Windows\\Fonts", local);
}

/* Build the candidate registry value names for the requested face+style. */
static void build_candidate_names(const wchar_t *face, bool bold, bool italic,
				  wchar_t candidates[][192], int *count)
{
	const wchar_t *style;
	if (bold && italic)
		style = L"Bold Italic";
	else if (bold)
		style = L"Bold";
	else if (italic)
		style = L"Italic";
	else
		style = L"";

	int n = 0;
	const wchar_t *suffixes[] = {L"(TrueType)", L"(OpenType)"};
	for (int s = 0; s < 2; ++s) {
		if (style[0])
			_snwprintf_s(candidates[n++], 192, _TRUNCATE,
				     L"%s %s %s", face, style, suffixes[s]);
		_snwprintf_s(candidates[n++], 192, _TRUNCATE,
			     L"%s %s", face, suffixes[s]);
	}
	*count = n;
}

/* Look the face up in one specific registry hive. Bare filenames in the
 * value are resolved relative to fallback_dir (HKLM → %WINDIR%\Fonts,
 * HKCU → %LOCALAPPDATA%\Microsoft\Windows\Fonts). */
static bool registry_lookup_in_hive(HKEY hive, const wchar_t *fallback_dir,
				    const wchar_t *face, bool bold, bool italic,
				    wchar_t *out_path, size_t out_path_chars)
{
	HKEY key;
	if (RegOpenKeyExW(hive, FONTS_REGISTRY_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
		return false;

	wchar_t candidates[8][192];
	int candidate_count = 0;
	build_candidate_names(face, bold, italic, candidates, &candidate_count);

	bool found = false;
	for (int i = 0; i < candidate_count && !found; ++i) {
		wchar_t value[MAX_PATH];
		DWORD size = sizeof(value);
		DWORD type = 0;
		LSTATUS r = RegQueryValueExW(key, candidates[i], NULL, &type,
					     (LPBYTE)value, &size);
		if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
			continue;
		value[MAX_PATH - 1] = 0;
		if (value[0] && value[1] == L':') {
			/* Absolute path */
			wcsncpy_s(out_path, out_path_chars, value, _TRUNCATE);
			found = true;
		} else if (fallback_dir[0]) {
			_snwprintf_s(out_path, out_path_chars, _TRUNCATE,
				     L"%s\\%s", fallback_dir, value);
			found = true;
		}
	}
	RegCloseKey(key);
	return found;
}

static bool registry_lookup(const wchar_t *face, bool bold, bool italic,
			    wchar_t *out_path, size_t out_path_chars)
{
	wchar_t sysfonts[MAX_PATH];
	wchar_t usrfonts[MAX_PATH];
	get_system_fonts_dir(sysfonts, MAX_PATH);
	get_user_fonts_dir(usrfonts, MAX_PATH);

	if (sysfonts[0] && registry_lookup_in_hive(HKEY_LOCAL_MACHINE, sysfonts,
						   face, bold, italic,
						   out_path, out_path_chars))
		return true;
	if (usrfonts[0] && registry_lookup_in_hive(HKEY_CURRENT_USER, usrfonts,
						   face, bold, italic,
						   out_path, out_path_chars))
		return true;
	return false;
}

/* Decode a UTF-16BE sfnt name entry (Microsoft / Apple Unicode platforms)
 * into UTF-8. Returns true on success. */
static bool decode_name_entry_utf8(const FT_SfntName *name,
				   char *out, int out_bytes)
{
	if (name->platform_id != TT_PLATFORM_MICROSOFT &&
	    name->platform_id != TT_PLATFORM_APPLE_UNICODE)
		return false;
	if (!name->string || name->string_len < 2 || (name->string_len & 1))
		return false;

	int chars = (int)(name->string_len / 2);
	wchar_t stackbuf[256];
	wchar_t *swapped = chars + 1 <= (int)(sizeof(stackbuf) / sizeof(stackbuf[0]))
				 ? stackbuf
				 : (wchar_t *)malloc((size_t)(chars + 1) * sizeof(wchar_t));
	if (!swapped)
		return false;
	for (int i = 0; i < chars; ++i) {
		unsigned char hi = name->string[2 * i + 0];
		unsigned char lo = name->string[2 * i + 1];
		swapped[i] = (wchar_t)((hi << 8) | lo);
	}
	swapped[chars] = 0;
	int n = WideCharToMultiByte(CP_UTF8, 0, swapped, -1, out, out_bytes, NULL, NULL);
	if (swapped != stackbuf)
		free(swapped);
	return n > 0;
}

/* True if any of the face's name table entries (in any language) matches
 * the requested name. Tries name_id 1 (Family), 4 (Full name), 16
 * (Typographic family). */
static bool face_name_matches(FT_Face face, const char *want_utf8)
{
	if (face->family_name && _stricmp(face->family_name, want_utf8) == 0)
		return true;

	FT_UInt count = FT_Get_Sfnt_Name_Count(face);
	for (FT_UInt i = 0; i < count; ++i) {
		FT_SfntName name;
		if (FT_Get_Sfnt_Name(face, i, &name) != 0)
			continue;
		if (name.name_id != TT_NAME_ID_FONT_FAMILY &&
		    name.name_id != TT_NAME_ID_FULL_NAME &&
		    name.name_id != TT_NAME_ID_TYPOGRAPHIC_FAMILY)
			continue;
		char buf[256];
		if (!decode_name_entry_utf8(&name, buf, sizeof(buf)))
			continue;
		if (_stricmp(buf, want_utf8) == 0)
			return true;
	}
	return false;
}

/* Enumerate one font directory looking for a face that matches want_utf8. */
static bool enumerate_dir(FT_Library lib, const wchar_t *dir,
			  const char *want_utf8,
			  wchar_t *out_path, size_t out_path_chars)
{
	if (!dir[0])
		return false;

	wchar_t pattern[MAX_PATH];
	_snwprintf_s(pattern, MAX_PATH, _TRUNCATE, L"%s\\*", dir);

	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(pattern, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	bool matched = false;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		const wchar_t *ext = wcsrchr(fd.cFileName, L'.');
		if (!ext)
			continue;
		if (_wcsicmp(ext, L".ttf") != 0 &&
		    _wcsicmp(ext, L".otf") != 0 &&
		    _wcsicmp(ext, L".ttc") != 0)
			continue;

		wchar_t fullw[MAX_PATH];
		_snwprintf_s(fullw, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);

		char fullu[MAX_PATH * 3];
		if (!wide_to_utf8(fullw, fullu, sizeof(fullu)))
			continue;

		FT_Long n_faces = 1;
		for (FT_Long fi = 0; fi < n_faces && !matched; ++fi) {
			FT_Face face;
			if (FT_New_Face(lib, fullu, fi, &face) != 0)
				continue;
			n_faces = face->num_faces;
			if (face_name_matches(face, want_utf8)) {
				wcsncpy_s(out_path, out_path_chars, fullw, _TRUNCATE);
				matched = true;
			}
			FT_Done_Face(face);
		}
	} while (!matched && FindNextFileW(h, &fd));

	FindClose(h);
	return matched;
}

static bool enumerate_fallback(const wchar_t *face_wide,
			       wchar_t *out_path, size_t out_path_chars)
{
	char want[256];
	if (!wide_to_utf8(face_wide, want, sizeof(want)))
		return false;

	FT_Library lib;
	if (FT_Init_FreeType(&lib) != 0)
		return false;

	bool matched = false;

	wchar_t dir[MAX_PATH];
	get_system_fonts_dir(dir, MAX_PATH);
	matched = enumerate_dir(lib, dir, want, out_path, out_path_chars);
	if (!matched) {
		get_user_fonts_dir(dir, MAX_PATH);
		matched = enumerate_dir(lib, dir, want, out_path, out_path_chars);
	}

	FT_Done_FreeType(lib);
	return matched;
}

bool flametext_resolve_font(const char *face, bool bold, bool italic,
			     char *out_path, size_t out_path_size)
{
	if (!face || !face[0] || !out_path || out_path_size == 0)
		return false;

	wchar_t face_wide[256];
	if (!utf8_to_wide(face, face_wide, 256))
		return false;

	wchar_t pathw[MAX_PATH];
	bool ok = registry_lookup(face_wide, bold, italic, pathw, MAX_PATH);
	if (!ok)
		ok = enumerate_fallback(face_wide, pathw, MAX_PATH);
	if (!ok) {
		obs_log(LOG_WARNING, "could not resolve font face '%s'", face);
		return false;
	}
	return wide_to_utf8(pathw, out_path, (int)out_path_size);
}

#endif /* _WIN32 */
