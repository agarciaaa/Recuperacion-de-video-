/*
	Untrunc-mejorado (Fase 2 hibrida) - ai_provider.cpp

	Implementacion de la interfaz comun: paquete de diagnostico
	(solo texto), validacion de privacidad (<= 50 KB, sin binarios),
	configuracion por entorno/archivo y utilidades JSON minimas.

	PRIVACIDAD: la clave API nunca se escribe en logs. El paquete
	usa nombres genericos, nunca rutas reales.

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "ai_provider.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

bool g_ai_local_enabled = true;
bool g_ai_external_enabled = false;
std::string g_ai_endpoint;
std::string g_ai_api_key;
std::string g_ai_model;
int g_ai_timeout_seconds = 20;
bool g_ai_lmstudio_enabled = false;
std::string g_lmstudio_endpoint;
std::string g_lmstudio_model;
int g_lmstudio_timeout_seconds = 45;
std::string g_ai_local_profile;
bool g_onnx_enabled = false;
std::string g_onnx_model_path;
double g_onnx_threshold = 0.75;

std::string defaultEndpoint() {
	// Dominio .invalid (RFC 2606): no se resuelve nunca. Asi, si
	// alguien activa la IA externa sin configurar nada, la llamada
	// falla en local sin salir a ningun servidor real.
	return "https://api.example.invalid/v1/ai-repair";
}

std::string jsonEscape(const std::string& s) {
	std::string out;
	out.reserve(s.size() + 2);
	for (unsigned char c : s) {
		switch (c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (c < 0x20) {
					char buf[8];
					snprintf(buf, sizeof(buf), "\\u%04x", c);
					out += buf;
				} else {
					out += (char)c;
				}
		}
	}
	return out;
}

std::string toHexLower(const unsigned char* data, size_t len) {
	static const char* hex = "0123456789abcdef";
	std::string out;
	out.reserve(len * 2);
	for (size_t i = 0; i < len; i++) {
		out += hex[(data[i] >> 4) & 0xF];
		out += hex[data[i] & 0xF];
	}
	return out;
}

std::string aiModeName(AiMode m) {
	switch (m) {
		case AiMode::Clasico: return "clasico";
		case AiMode::Perfil: return "perfil";
		case AiMode::Rescate: return "rescate";
	}
	return "clasico";
}

std::string aiModeNameEs(AiMode m) {
	switch (m) {
		case AiMode::Clasico: return "clásico (con referencia sana)";
		case AiMode::Perfil: return "perfil (sin referencia, con perfil de cámara)";
		case AiMode::Rescate: return "rescate (sin referencia, parcial)";
	}
	return "clásico";
}

static std::string trim(const std::string& s) {
	size_t a = 0;
	while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
	size_t b = s.size();
	while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
	return s.substr(a, b - a);
}

AiConfig loadAiConfig() {
	AiConfig cfg;
	cfg.endpoint = defaultEndpoint();

	// 1) Archivo opcional ~/.config/untrunc/ai.conf (CLAVE=valor).
	//    No es obligatorio; si no existe, no pasa nada.
	const char* home = std::getenv("HOME");
	if (home && *home) {
		fs::path p = fs::path(home) / ".config" / "untrunc" / "ai.conf";
		cfg.config_file = p.string();
		std::ifstream f(p);
		if (f) {
			std::string line;
			while (std::getline(f, line)) {
				line = trim(line);
				if (line.empty() || line[0] == '#') continue;
				auto eq = line.find('=');
				if (eq == std::string::npos) continue;
				std::string k = trim(line.substr(0, eq));
				std::string v = trim(line.substr(eq + 1));
				if (k == "endpoint") cfg.endpoint = v;
				else if (k == "model") cfg.model = v;
				else if (k == "api_key") cfg.api_key = v;
				else if (k == "timeout") { try { cfg.timeout_seconds = std::stoi(v); } catch (...) {} }
				else if (k == "local_profile") cfg.local_profile = v;
				else if (k == "lmstudio_endpoint") cfg.lmstudio_endpoint = v;
				else if (k == "lmstudio_model") cfg.lmstudio_model = v;
				else if (k == "lmstudio_api_key") cfg.lmstudio_api_key = v;
				else if (k == "lmstudio_timeout") { try { cfg.lmstudio_timeout_seconds = std::stoi(v); } catch (...) {} }
				else if (k == "onnx_enabled") cfg.onnx_enabled = (v == "1" || v == "true" || v == "si");
				else if (k == "onnx_model") cfg.onnx_model_path = v;
				else if (k == "onnx_threshold") { try { cfg.onnx_threshold = std::stod(v); } catch (...) {} }
			}
		}
	}

	// 2) Variables de entorno: mandan sobre el archivo.
	//    (Asi la clave puede vivir solo en el entorno, sin archivo.)
	const char* e = nullptr;
	if ((e = std::getenv("UNTRUNC_AI_ENDPOINT")) && *e) cfg.endpoint = e;
	if ((e = std::getenv("UNTRUNC_AI_MODEL")) && *e) cfg.model = e;
	if ((e = std::getenv("UNTRUNC_AI_API_KEY")) && *e) cfg.api_key = e;
	if ((e = std::getenv("UNTRUNC_AI_TIMEOUT")) && *e) {
		try { cfg.timeout_seconds = std::stoi(e); } catch (...) {}
	}
	if ((e = std::getenv("UNTRUNC_AI_LOCAL_PROFILE")) && *e) cfg.local_profile = e;
	if ((e = std::getenv("UNTRUNC_LMSTUDIO_ENDPOINT")) && *e) cfg.lmstudio_endpoint = e;
	if ((e = std::getenv("UNTRUNC_LMSTUDIO_MODEL")) && *e) cfg.lmstudio_model = e;
	if ((e = std::getenv("UNTRUNC_LMSTUDIO_API_KEY")) && *e) cfg.lmstudio_api_key = e;
	if ((e = std::getenv("UNTRUNC_LMSTUDIO_TIMEOUT")) && *e) {
		try { cfg.lmstudio_timeout_seconds = std::stoi(e); } catch (...) {}
	}
	if ((e = std::getenv("UNTRUNC_ONNX_ENABLED")) && *e)
		cfg.onnx_enabled = (std::string(e) == "1" || std::string(e) == "true");
	if ((e = std::getenv("UNTRUNC_ONNX_MODEL")) && *e) cfg.onnx_model_path = e;
	if ((e = std::getenv("UNTRUNC_ONNX_THRESHOLD")) && *e) {
		try { cfg.onnx_threshold = std::stod(e); } catch (...) {}
	}

	// 3) Globales de la GUI (si la GUI los cambio en esta sesion).
	cfg.local_enabled = g_ai_local_enabled;
	if (!g_ai_local_profile.empty()) cfg.local_profile = g_ai_local_profile;
	cfg.external_enabled = g_ai_external_enabled;
	if (!g_ai_endpoint.empty()) cfg.endpoint = g_ai_endpoint;
	if (!g_ai_api_key.empty()) cfg.api_key = g_ai_api_key;
	if (!g_ai_model.empty()) cfg.model = g_ai_model;
	cfg.timeout_seconds = g_ai_timeout_seconds;
	cfg.lmstudio_enabled = g_ai_lmstudio_enabled;
	if (!g_lmstudio_endpoint.empty()) cfg.lmstudio_endpoint = g_lmstudio_endpoint;
	if (!g_lmstudio_model.empty()) cfg.lmstudio_model = g_lmstudio_model;
	cfg.lmstudio_timeout_seconds = g_lmstudio_timeout_seconds;
	cfg.onnx_enabled = g_onnx_enabled;
	if (!g_onnx_model_path.empty()) cfg.onnx_model_path = g_onnx_model_path;
	cfg.onnx_threshold = g_onnx_threshold;
	// Limites (una sola vez, despues de archivo, entorno y GUI).
	if (cfg.timeout_seconds < 5) cfg.timeout_seconds = 5;
	if (cfg.timeout_seconds > 120) cfg.timeout_seconds = 120;
	if (cfg.lmstudio_timeout_seconds < 10) cfg.lmstudio_timeout_seconds = 10;
	if (cfg.lmstudio_timeout_seconds > 180) cfg.lmstudio_timeout_seconds = 180;
	if (cfg.onnx_threshold < 0.5) cfg.onnx_threshold = 0.5;
	if (cfg.onnx_threshold > 0.95) cfg.onnx_threshold = 0.95;
	return cfg;
}

DiagnosticPacket buildDiagnosticPacket(const AiDiagnosis& d,
                                       const std::string& diag_code_name) {
	DiagnosticPacket p;
	std::ostringstream ss;
	// Nombres genericos a proposito: la ruta real puede contener
	// el nombre del usuario o de sus carpetas personales.
	ss << "{"
	   << "\"version\":1,"
	   << "\"archivo\":\"video_danado.mp4\","
	   << "\"referencia\":\"referencia.mp4\","
	   << "\"codigo_preflight\":\"" << jsonEscape(diag_code_name) << "\","
	   << "\"ftyp\":\"" << jsonEscape(d.ftyp) << "\","
	   << "\"tamano_bytes\":" << d.file_size << ","
	   << "\"tiene_moov\":" << (d.has_moov ? "true" : "false") << ","
	   << "\"tiene_mdat\":" << (d.has_mdat ? "true" : "false") << ","
	   << "\"mdat_bytes\":" << d.mdat_bytes << ","
	   << "\"codec\":\"" << jsonEscape(d.codec) << "\","
	   << "\"ancho\":" << d.width << ","
	   << "\"alto\":" << d.height << ","
	   << "\"fps_estimado\":" << d.fps_estimado << ","
	   << "\"sps_hex\":\"" << jsonEscape(d.sps_hex) << "\","
	   << "\"pps_hex\":\"" << jsonEscape(d.pps_hex) << "\","
	   << "\"sps_valido\":" << (d.sps_valido ? "true" : "false") << ","
	   << "\"nal_sps\":" << d.nal_sps << ","
	   << "\"nal_pps\":" << d.nal_pps << ","
	   << "\"nal_idr\":" << d.nal_idr << ","
	   << "\"nal_slice\":" << d.nal_slice << ","
	   << "\"nal_aud\":" << d.nal_aud << ","
	   << "\"frames_estimados\":" << d.estimated_frames << ","
	   << "\"histograma_tamano_slice_kb\":["
	   << d.hist_slice_kb[0] << "," << d.hist_slice_kb[1] << ","
	   << d.hist_slice_kb[2] << "," << d.hist_slice_kb[3] << ","
	   << d.hist_slice_kb[4] << "],"
	   << "\"perfil_sugerido\":\"" << jsonEscape(d.profile_id) << "\","
	   << "\"confianza_perfil\":" << d.profile_confidence << ","
	   << "\"marcadores\":\"" << jsonEscape(d.markers) << "\""
	   << "}";
	p.json = ss.str();
	p.bytes = p.json.size();
	std::string why;
	p.valid = validateDiagnosticPacket(p, why);
	if (!p.valid) p.error_es = why;
	return p;
}

bool validateDiagnosticPacket(const DiagnosticPacket& p, std::string& why_es) {
	if (p.bytes > kMaxDiagnosticPacketBytes) {
		why_es = "El paquete de diagnóstico supera los 50 KB; se rechaza sin enviar nada.";
		return false;
	}
	if (p.json.empty() || p.json.front() != '{' || p.json.back() != '}') {
		why_es = "El paquete de diagnóstico no es un JSON válido.";
		return false;
	}
	// Solo texto imprimible: ningun byte binario ni NUL.
	for (unsigned char c : p.json) {
		if (c == 0 || (c < 0x20 && c != '\n' && c != '\t' && c != '\r')) {
			why_es = "El paquete contiene bytes no imprimibles; se rechaza sin enviar nada.";
			return false;
		}
	}
	// Defensa en profundidad: si alguien metio por error un campo
	// con datos de video, se rechaza. El paquete legitimo no tiene
	// ninguno de estos campos.
	static const char* prohibidos[] = {"\"mdat\"", "\"video_base64\"", "\"frame\"", "\"audio\""};
	for (const char* f : prohibidos) {
		if (p.json.find(f) != std::string::npos) {
			why_es = std::string("El paquete contiene un campo prohibido (") + f +
			         "); se rechaza sin enviar nada.";
			return false;
		}
	}
	return true;
}

bool jsonFindNumber(const std::string& json, const std::string& key, double& out) {
	std::string pat = "\"" + key + "\"";
	size_t pos = json.find(pat);
	if (pos == std::string::npos) return false;
	pos = json.find(':', pos + pat.size());
	if (pos == std::string::npos) return false;
	pos++;
	while (pos < json.size() && std::isspace((unsigned char)json[pos])) pos++;
	size_t end = pos;
	if (end < json.size() && (json[end] == '-' || json[end] == '+')) end++;
	while (end < json.size() && (std::isdigit((unsigned char)json[end]) || json[end] == '.' ||
	       json[end] == 'e' || json[end] == 'E' || json[end] == '-' || json[end] == '+')) end++;
	if (end == pos) return false;
	try {
		out = std::stod(json.substr(pos, end - pos));
	} catch (...) {
		return false;
	}
	return true;
}

bool jsonFindString(const std::string& json, const std::string& key, std::string& out) {
	std::string pat = "\"" + key + "\"";
	size_t pos = json.find(pat);
	if (pos == std::string::npos) return false;
	pos = json.find(':', pos + pat.size());
	if (pos == std::string::npos) return false;
	pos = json.find('"', pos + 1);
	if (pos == std::string::npos) return false;
	size_t end = json.find('"', pos + 1);
	if (end == std::string::npos) return false;
	out = json.substr(pos + 1, end - pos - 1);
	return true;
}
