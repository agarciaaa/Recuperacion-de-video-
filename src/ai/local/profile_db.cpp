/*
	Untrunc-mejorado (Fase 2 hibrida) - profile_db.cpp
	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "profile_db.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

static std::vector<CameraProfile> embeddedProfiles() {
	// Los mismos perfiles genericos que en profiles/profiles.json,
	// por si el JSON no esta junto al ejecutable.
	return {
		{"generico_h264_1080p30", "Genérico H.264 1080p 30 fps", "avc1", 1920, 1080, 30, "High", "isom", "", 48000, 2},
		{"generico_h264_720p30", "Genérico H.264 720p 30 fps", "avc1", 1280, 720, 30, "High", "isom", "", 48000, 2},
		{"generico_h264_360p30", "Genérico H.264 360p 30 fps (pruebas)", "avc1", 640, 360, 30, "High", "isom", "", 48000, 2},
		{"iphone_h264_1080p30", "iPhone (H.264, 1080p, 30 fps)", "avc1", 1920, 1080, 30, "High", "mp42", "", 48000, 2},
		{"samsung_h264_1080p30", "Samsung Galaxy (H.264, 1080p, 30 fps)", "avc1", 1920, 1080, 30, "High", "mp42", "", 48000, 2},
		{"gopro_h264_1080p60", "GoPro (H.264, 1080p, 60 fps)", "avc1", 1920, 1080, 60, "High", "mp42", "gpmd", 48000, 2},
		{"dji_h264_4k30", "DJI (H.264, 4K, 30 fps)", "avc1", 3840, 2160, 30, "High", "isom", "", 48000, 2},
		{"sony_xavc_1080p30", "Sony XAVC-S (H.264, 1080p, 30 fps)", "avc1", 1920, 1080, 30, "High", "XAVC", "XAVC", 48000, 2},
		{"obs_x264_1080p30", "OBS Studio (x264, 1080p, 30 fps)", "avc1", 1920, 1080, 30, "High", "isom", "", 48000, 2},
	};
}

// Busca "clave": "valor" o "clave": numero en un objeto JSON plano.
// Suficiente para nuestro profiles.json (sin libreria externa).
static bool jsonStr(const std::string& obj, const std::string& key, std::string& out) {
	std::string pat = "\"" + key + "\"";
	size_t pos = obj.find(pat);
	if (pos == std::string::npos) return false;
	pos = obj.find(':', pos + pat.size());
	if (pos == std::string::npos) return false;
	pos = obj.find('"', pos + 1);
	if (pos == std::string::npos) return false;
	size_t end = obj.find('"', pos + 1);
	if (end == std::string::npos) return false;
	out = obj.substr(pos + 1, end - pos - 1);
	return true;
}

static bool jsonInt(const std::string& obj, const std::string& key, int& out) {
	std::string pat = "\"" + key + "\"";
	size_t pos = obj.find(pat);
	if (pos == std::string::npos) return false;
	pos = obj.find(':', pos + pat.size());
	if (pos == std::string::npos) return false;
	pos++;
	while (pos < obj.size() && (obj[pos] == ' ' || obj[pos] == '\t')) pos++;
	size_t end = pos;
	if (end < obj.size() && obj[end] == '-') end++;
	while (end < obj.size() && obj[end] >= '0' && obj[end] <= '9') end++;
	if (end == pos) return false;
	try { out = std::stoi(obj.substr(pos, end - pos)); } catch (...) { return false; }
	return true;
}

std::vector<CameraProfile> loadProfiles(const std::string& json_path) {
	std::vector<std::string> candidates;
	if (!json_path.empty()) candidates.push_back(json_path);
	candidates.push_back("profiles/profiles.json");
	candidates.push_back("../profiles/profiles.json");
	candidates.push_back("source/profiles/profiles.json");
	// Junto al ejecutable: en Windows el ZIP lleva profiles/ al lado.
	// (En Linux, el binario se ejecuta desde source/, asi que
	// "profiles/profiles.json" ya cubre ese caso.)
	std::string text;
	for (const auto& c : candidates) {
		std::ifstream f(c);
		if (!f) continue;
		std::ostringstream ss;
		ss << f.rdbuf();
		text = ss.str();
		if (!text.empty()) break;
	}
	if (text.empty()) return embeddedProfiles();
	// Cada perfil es un objeto {...} dentro del array "perfiles".
	std::vector<CameraProfile> out;
	size_t pos = 0;
	while (true) {
		size_t open = text.find('{', pos);
		if (open == std::string::npos) break;
		// Saltar el objeto raiz (el que contiene "perfiles": [ ...).
		size_t close = text.find('}', open);
		if (close == std::string::npos) break;
		std::string obj = text.substr(open, close - open + 1);
		CameraProfile p;
		if (jsonStr(obj, "id", p.id)) {
			jsonStr(obj, "nombre_es", p.nombre_es);
			jsonStr(obj, "codec", p.codec);
			jsonStr(obj, "profile", p.profile);
			jsonStr(obj, "ftyp", p.ftyp);
			jsonStr(obj, "markers", p.markers);
			jsonInt(obj, "width", p.width);
			jsonInt(obj, "height", p.height);
			jsonInt(obj, "fps", p.fps);
			jsonInt(obj, "audio_sample_rate", p.audio_sample_rate);
			jsonInt(obj, "audio_channels", p.audio_channels);
			out.push_back(p);
		}
		pos = close + 1;
	}
	if (out.empty()) return embeddedProfiles();
	return out;
}

ProfileMatch matchProfile(const std::vector<CameraProfile>& profiles,
                          const std::string& ftyp,
                          const std::string& codec,
                          int width, int height, int fps,
                          const std::string& markers) {
	ProfileMatch best;
	double best_score = -1;
	for (const auto& p : profiles) {
		double score = 0;
		if (!p.codec.empty() && p.codec == codec) score += 25;
		else if (!codec.empty() && codec != "desconocido") score -= 10;
		if (width > 0 && p.width == width) score += 25;
		if (height > 0 && p.height == height) score += 15;
		if (fps > 0 && p.fps == fps) score += 15;
		else if (fps > 0 && p.fps > 0) score -= 5;
		if (!ftyp.empty() && p.ftyp == ftyp) score += 10;
		if (!markers.empty() && !p.markers.empty() && markers.find(p.markers) != std::string::npos)
			score += 20;
		if (score > best_score) { best_score = score; best.profile = p; }
	}
	// Puntuacion maxima posible ~110. Confianza = score/100, tope 0.95.
	// Menos de 60 puntos = coincidencia debil: no fiarse.
	best.confidence = std::max(0.0, std::min(0.95, best_score / 100.0));
	if (best_score >= 60) {
		best.reason_es = "El perfil '" + best.profile.nombre_es + "' coincide con la huella "
		                 "del video (codec, resolucion y marca).";
	} else if (best_score >= 35) {
		best.reason_es = "Coincidencia debil con '" + best.profile.nombre_es + "': se usara "
		                 "como plantilla aproximada y el resultado se marcara como rescate parcial.";
	} else {
		best.confidence = std::min(best.confidence, 0.4);
		best.reason_es = "Ningun perfil coincide bien; se usara el perfil generico mas cercano.";
	}
	return best;
}
