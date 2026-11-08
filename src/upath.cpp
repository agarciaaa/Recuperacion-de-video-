/*
	Untrunc-mejorado - upath.cpp (ver upath.h)

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "upath.h"

#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace upath {

#ifdef _WIN32

static std::wstring widen(const std::string& s) {
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	if (n <= 0) return std::wstring(s.begin(), s.end());
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
	return w;
}

static std::string narrow(const std::wstring& w) {
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	if (n <= 0) return std::string();
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

fs::path toPath(const std::string& utf8) { return fs::path(widen(utf8)); }

std::string fromPath(const fs::path& p) { return narrow(p.wstring()); }

int system(const std::string& utf8_cmd) { return _wsystem(widen(utf8_cmd).c_str()); }

FILE* openPipe(const std::string& utf8_cmd, const char* mode) {
	return _wpopen(widen(utf8_cmd).c_str(), widen(mode).c_str());
}

int closePipe(FILE* f) { return ::_pclose(f); }

std::string getenv(const char* name) {
	const wchar_t* v = _wgetenv(widen(name).c_str());
	return v ? narrow(v) : std::string();
}

std::string executablePath() {
	std::wstring buf(32768, L'\0');
	DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
	if (n == 0 || n >= buf.size()) return std::string();
	buf.resize(n);
	return narrow(buf);
}

#else

fs::path toPath(const std::string& utf8) { return fs::path(utf8); }

std::string fromPath(const fs::path& p) { return p.string(); }

int system(const std::string& utf8_cmd) { return std::system(utf8_cmd.c_str()); }

FILE* openPipe(const std::string& utf8_cmd, const char* mode) {
	return ::popen(utf8_cmd.c_str(), mode);
}

int closePipe(FILE* f) { return ::pclose(f); }

std::string getenv(const char* name) {
	const char* v = std::getenv(name);
	return v ? std::string(v) : std::string();
}

std::string executablePath() {
	std::error_code ec;
	fs::path self = fs::read_symlink("/proc/self/exe", ec);
	return ec ? std::string() : self.string();
}

#endif

} // namespace upath
