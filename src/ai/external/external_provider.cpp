/*
	Untrunc-mejorado (Fase 2 hibrida) - external_provider.cpp
	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "external_provider.h"
#include "../../upath.h"
#include "../../tempfs.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../../diagnoser.h"
#include "../local/local_provider.h"

namespace fs = std::filesystem;

static std::string shellQuote(const std::string& s) {
#ifdef _WIN32
	// Reparaciones (2026-11-07): cmd.exe no entiende las comillas
	// simples POSIX; en Windows se usan comillas dobles (misma
	// convención que onnx_provider.cpp).
	std::string out = "\"";
	for (char c : s) {
		if (c == '"') out += "\\\"";
		else out += c;
	}
	out += "\"";
	return out;
#else
	std::string out = "'";
	for (char c : s) {
		if (c == '\'') out += "'\\''";
		else out += c;
	}
	out += "'";
	return out;
#endif
}

// Reparaciones (2026-11-07): mismo problema que en LM Studio —
// `command -v` no existe en cmd.exe, así que en Windows la
// comprobación de la IA externa siempre fallaba aunque curl
// estuviera instalado. En Windows se usa `where curl`.
static bool hasCurl() {
#ifdef _WIN32
	return std::system("where curl > NUL 2>&1") == 0;
#else
	// `command -v` no escribe la ruta en stdout aqui; solo el codigo.
	return std::system("command -v curl > /dev/null 2>&1") == 0;
#endif
}

// Redirección de stderr a la nada (ver lmstudio_provider.cpp).
static const char* kStderrNull =
#ifdef _WIN32
    "2> NUL";
#else
    "2>/dev/null";
#endif

bool ExternalProvider::isAvailable(std::string& why_not_es) const {
	if (!cfg_.external_enabled) {
		why_not_es = "La IA externa está desactivada (por defecto). Actívala con --ia-externa "
		             "o en Ajustes > IA, y pon tu API key.";
		return false;
	}
	if (cfg_.api_key.empty()) {
		why_not_es = "Falta la API key de la IA externa. Ponla en la variable "
		             "UNTRUNC_AI_API_KEY o en ~/.config/untrunc/ai.conf (api_key=...). "
		             "Sin clave no se envía nada.";
		return false;
	}
	if (cfg_.endpoint.empty() || cfg_.endpoint == defaultEndpoint()) {
		why_not_es = "Falta el endpoint de la IA externa. Pon una URL real en "
		             "UNTRUNC_AI_ENDPOINT o en ai.conf. El endpoint por defecto es "
		             "intencionadamente inválido para no llamar a ningún servidor por accidente.";
		return false;
	}
	if (cfg_.endpoint.find("key=") != std::string::npos ||
	    cfg_.endpoint.find("api_key") != std::string::npos) {
		why_not_es = "La URL del endpoint no debe llevar la clave (ni 'key=' ni 'api_key'). "
		             "La clave va en el header Authorization, no en la URL.";
		return false;
	}
	if (cfg_.endpoint.rfind("https://", 0) != 0 && cfg_.endpoint.rfind("http://127.0.0.1", 0) != 0 &&
	    cfg_.endpoint.rfind("http://localhost", 0) != 0) {
		why_not_es = "El endpoint debe ser https:// (o http:// solo en localhost para pruebas).";
		return false;
	}
	if (!hasCurl()) {
		why_not_es = "No se encontró el programa 'curl' en el sistema. La IA externa lo "
		             "necesita para hacer la llamada. En Windows, curl viene con Windows 10+ "
		             "y con MSYS2.";
		return false;
	}
	return true;
}

DiagnosticPacket ExternalProvider::buildPacketForFile(const std::string& damaged_path,
                                                     const std::string& ref_path) {
	AiDiagnosis d = analyzeLocally(damaged_path);
	std::string code = "local";
	if (!ref_path.empty()) {
		DiagReport r = preflightCheck(ref_path, damaged_path);
		code = diagCodeName(r.code);
	}
	return buildDiagnosticPacket(d, code);
}

AiSuggestion ExternalProvider::analyze(const DiagnosticPacket& packet,
                                       const AiDiagnosis& diagnosis) {
	AiSuggestion s;
	s.source = "externa";
	s.codec = diagnosis.codec;
	s.width = diagnosis.width;
	s.height = diagnosis.height;
	s.fps = diagnosis.fps_estimado;
	s.profile_id = diagnosis.profile_id;
	s.mode = AiMode::Rescate;
	s.confidence = 0.0;

	std::string why;
	if (!isAvailable(why)) {
		s.warning_es = why;
		s.reason_es = "IA externa no disponible: " + why;
		return s;
	}
	if (!packet.valid) {
		s.warning_es = "Paquete de diagnóstico no válido: " + packet.error_es;
		s.reason_es = "No se envió nada: " + packet.error_es;
		return s;
	}
	// Defensa final: validar de nuevo justo antes de enviar.
	std::string why2;
	if (!validateDiagnosticPacket(packet, why2)) {
		s.warning_es = why2;
		s.reason_es = "No se envió nada: " + why2;
		return s;
	}

	// Archivos temporales con permisos 0600: el paquete y la
	// configuracion de curl con el header Authorization. La clave
	// NUNCA va en la linea de comandos ni en los logs.
	// Carpeta temporal unica (tempfs.h): se borra sola al salir,
	// tambien en los return anticipados, con el curlrc (la clave) dentro.
	std::error_code ec;
	tempfs::ScopedDir tmpdir("ai-ext");
	if (!tmpdir.ok()) {
		s.warning_es = "No se pudo crear el directorio temporal.";
		return s;
	}
	fs::path packet_file = tmpdir / "packet.json";
	fs::path curlrc = tmpdir / "curlrc";
	fs::path resp_file = tmpdir / "response.json";
	{
		std::ofstream f(packet_file, std::ios::binary | std::ios::trunc);
		if (!f) { s.warning_es = "No se pudo escribir el paquete temporal."; return s; }
		f << packet.json;
	}
	{
		std::ofstream f(curlrc, std::ios::trunc);
		if (!f) { s.warning_es = "No se pudo escribir la configuración temporal de curl."; return s; }
		// La clave va aqui, en un archivo 0600, no en argv.
		f << "header = \"Authorization: Bearer " << cfg_.api_key << "\"\n"
		  << "header = \"Content-Type: application/json\"\n"
		  << "header = \"Accept: application/json\"\n"
		  // write-out en el curlrc, no -w en la línea (ver
		  // lmstudio_provider.cpp: cmd.exe expandiría %{http_code}).
		  << "write-out = \"%{http_code}\"\n"
		  << "max-time = " << cfg_.timeout_seconds << "\n"
		  << "silent\nshow-error\n";
	}
	fs::permissions(packet_file, fs::perms::owner_read | fs::perms::owner_write, ec);
	fs::permissions(curlrc, fs::perms::owner_read | fs::perms::owner_write, ec);

	// Peticion: POST del paquete (solo texto). Se espera una
	// respuesta JSON con campos "modo", "confianza" y "mensaje".
	// Reintentos limitados: max_retries (por defecto 1 reintento).
	std::string response;
	int http_code = 0;
	bool sent = false;
	for (int attempt = 0; attempt <= cfg_.max_retries; attempt++) {
		std::string cmd = "curl -sS --config " + shellQuote(upath::fromPath(curlrc)) +
		                  " --data-binary @" + shellQuote(upath::fromPath(packet_file)) +
		                  " -o " + shellQuote(upath::fromPath(resp_file)) +
		                  " " + shellQuote(cfg_.endpoint) +
		                  " " + kStderrNull;
		FILE* pipe = upath::openPipe(cmd, "r");
		if (!pipe) break;
		char buf[64] = {0};
		std::string out;
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf) - 1, pipe)) > 0) out.append(buf, n);
		int rc = upath::closePipe(pipe);
		(void)rc;
		try { http_code = std::stoi(out); } catch (...) { http_code = 0; }
		if (http_code >= 200 && http_code < 300) { sent = true; break; }
		if (attempt < cfg_.max_retries) continue;
		break;
	}
	// Borrar la clave del disco inmediatamente, se haya enviado o no.
	std::error_code ec2;
	fs::remove(curlrc, ec2);
	fs::remove(packet_file, ec2);

	if (!sent) {
		s.warning_es = "No se pudo contactar con la IA externa (HTTP " +
		               std::to_string(http_code) + "). Se sigue solo con la IA local; "
		               "el video nunca salió de tu PC.";
		s.reason_es = "IA externa sin respuesta; se usa la sugerencia local.";
		return s;
	}
	std::ifstream rf(resp_file);
	std::ostringstream ss;
	ss << rf.rdbuf();
	std::string resp = ss.str();
	fs::remove(resp_file, ec2);
	if (resp.empty() || resp.size() > 64 * 1024) {
		s.warning_es = "La IA externa respondió con un texto vacío o demasiado grande; se ignora.";
		return s;
	}
	// Parseo minimo de la respuesta: modo, confianza, mensaje.
	std::string mode_str;
	if (jsonFindString(resp, "modo", mode_str) || jsonFindString(resp, "mode", mode_str)) {
		if (mode_str == "perfil") s.mode = AiMode::Perfil;
		else if (mode_str == "rescate") s.mode = AiMode::Rescate;
		else s.mode = AiMode::Clasico;
	}
	double conf = 0;
	if (jsonFindNumber(resp, "confianza", conf) || jsonFindNumber(resp, "confidence", conf)) {
		s.confidence = std::max(0.0, std::min(1.0, conf));
	} else {
		s.confidence = 0.5;  // respondio, pero sin confianza explicita
	}
	std::string msg;
	if (jsonFindString(resp, "mensaje", msg) || jsonFindString(resp, "sugerencia", msg) ||
	    jsonFindString(resp, "message", msg)) {
		s.reason_es = "IA externa: " + msg;
	} else {
		s.reason_es = "IA externa: respondió con modo '" + aiModeName(s.mode) + "'.";
	}
	s.use_step = true;
	s.use_dyn = true;
	return s;
}
