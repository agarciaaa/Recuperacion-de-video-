/*
	Untrunc-mejorado - tempfs.cpp (ver tempfs.h)

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "tempfs.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>

namespace fs = std::filesystem;

namespace tempfs {

static std::string randomSuffix() {
	static std::atomic<uint64_t> counter{0};
	static thread_local std::mt19937_64 rng([] {
		std::random_device rd;
		uint64_t seed = ((uint64_t)rd() << 32) ^ rd();
		seed ^= (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
		return seed;
	}());
	uint64_t v = rng() ^ (counter.fetch_add(1) * 0x9E3779B97F4A7C15ull);
	char buf[17];
	std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
	return buf;
}

fs::path createUniqueDir(const std::string& prefix, std::error_code& ec) {
	fs::path base = fs::temp_directory_path(ec);
	if (ec) return fs::path();
	for (int attempt = 0; attempt < 100; attempt++) {
		fs::path dir = base / ("untrunc-" + prefix + "-" + randomSuffix());
		ec.clear();
		// true solo si la creo AHORA: nunca se reutiliza una existente.
		if (fs::create_directory(dir, ec) && !ec) {
			std::error_code perm_ec;
			fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, perm_ec);
			return dir;
		}
		if (ec) return fs::path();  // error real (sin permisos, disco lleno...)
	}
	ec = std::make_error_code(std::errc::file_exists);
	return fs::path();
}

void removeDir(const fs::path& dir) {
	if (dir.empty()) return;
	std::error_code ec;
	fs::remove_all(dir, ec);
}

ScopedDir::ScopedDir(const std::string& prefix) {
	std::error_code ec;
	path_ = createUniqueDir(prefix, ec);
}

ScopedDir::~ScopedDir() { removeDir(path_); }

fs::path ScopedDir::release() {
	fs::path p = path_;
	path_.clear();
	return p;
}

} // namespace tempfs
