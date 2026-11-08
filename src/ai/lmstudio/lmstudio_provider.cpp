/*
	Untrunc-mejorado - lmstudio_provider.cpp

	Cliente OpenAI-compatible para LM Studio (Qwen local).
	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "lmstudio_provider.h"
#include "../../upath.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

std::string lmstudioNotRunningEs(const std::string& endpoint) {
	return "LM Studio no responde en " + endpoint +
	       ", abre LM Studio, carga Qwen y pulsa Start Server. "
	       "En LM Studio: pestaña Developer (o Server), elige el modelo "
	       "Qwen descargado y pulsa Start Server en el puerto 1234.";
}

static std::string shellQuote(const std::string& s) {
#ifdef _WIN32
	// Reparaciones (2026-11-07): cmd.exe no entiende las comillas
	// simples POSIX; en Windows se usan comillas dobles (misma
	// convención que onnx_provider.cpp). Sin esto, curl recibía
	// las comillas como parte del argumento y fallaba.
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

// Reparaciones (2026-11-07): `command -v` es un builtin de shell
// Unix que NO existe en cmd.exe, así que en Windows esta función
// siempre decía que no había curl aunque LM Studio corriera con
// Qwen cargado (caso real de Angel, 2026-11-07). En Windows se
// comprueba con `where curl`, que es el equivalente de cmd.exe.
static bool hasCurl() {
#ifdef _WIN32
	return std::system("where curl > NUL 2>&1") == 0;
#else
	return std::system("command -v curl > /dev/null 2>&1") == 0;
#endif
}

// Redirección de stderr a la nada: `2> NUL` en cmd.exe,
// `2>/dev/null` en POSIX. (`2>/dev/null` en cmd.exe es un error.)
static const char* kStderrNull =
#ifdef _WIN32
    "2> NUL";
#else
    "2>/dev/null";
#endif

static std::string toLowerStr(std::string s) {
	for (char& c : s) c = (char)std::tolower((unsigned char)c);
	return s;
}

static std::string stripTrailingSlash(std::string s) {
	while (!s.empty() && s.back() == '/') s.pop_back();
	return s;
}

// Extrae el valor de una clave string de un JSON, con escapes.
// Devuelve false si la clave no existe o el valor no es string.
static bool extractJsonString(const std::string& json, const std::string& key,
                              std::string& out) {
	std::string pat = "\"" + key + "\"";
	size_t pos = json.find(pat);
	if (pos == std::string::npos) return false;
	pos = json.find(':', pos + pat.size());
	if (pos == std::string::npos) return false;
	pos++;
	while (pos < json.size() && std::isspace((unsigned char)json[pos])) pos++;
	if (pos >= json.size() || json[pos] != '"') return false;
	pos++;
	std::string val;
	while (pos < json.size()) {
		char c = json[pos++];
		if (c == '"') { out = val; return true; }
		if (c == '\\' && pos < json.size()) {
			char e = json[pos++];
			switch (e) {
				case 'n': val += '\n'; break;
				case 't': val += '\t'; break;
				case 'r': val += '\r'; break;
				case '"': val += '"'; break;
				case '\\': val += '\\'; break;
				case '/': val += '/'; break;
				case 'b': val += '\b'; break;
				case 'f': val += '\f'; break;
				case 'u': {
					// \uXXXX (BMP) -> UTF-8.
					if (pos + 4 > json.size()) return false;
					unsigned cp = 0;
					for (int i = 0; i < 4; i++) {
						char h = json[pos++];
						cp <<= 4;
						if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
						else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
						else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
						else return false;
					}
					if (cp < 0x80) val += (char)cp;
					else if (cp < 0x800) {
						val += (char)(0xC0 | (cp >> 6));
						val += (char)(0x80 | (cp & 0x3F));
					} else {
						val += (char)(0xE0 | (cp >> 12));
						val += (char)(0x80 | ((cp >> 6) & 0x3F));
						val += (char)(0x80 | (cp & 0x3F));
					}
					break;
				}
				default: val += e; break;
			}
		} else {
			val += c;
		}
	}
	return false;
}

// Todos los valores de "id" de un JSON (para /v1/models).
static std::vector<std::string> extractAllIds(const std::string& json) {
	std::vector<std::string> ids;
	size_t from = 0;
	while (true) {
		size_t pos = json.find("\"id\"", from);
		if (pos == std::string::npos) break;
		std::string v;
		// Reutiliza el extractor sobre el resto del texto.
		if (extractJsonString(json.substr(pos), "id", v) && !v.empty())
			ids.push_back(v);
		from = pos + 4;
	}
	return ids;
}

// Quita cercas ```json ... ``` si Qwen devuelve markdown.
static std::string stripCodeFences(std::string s) {
	size_t a = 0;
	while (a < s.size() && std::isspace((unsigned char)s[a])) a++;
	if (s.compare(a, 3, "```") == 0) {
		size_t nl = s.find('\n', a);
		if (nl != std::string::npos) s = s.substr(nl + 1);
		size_t end = s.rfind("```");
		if (end != std::string::npos) s = s.substr(0, end);
	}
	size_t b = 0;
	while (b < s.size() && std::isspace((unsigned char)s[b])) b++;
	size_t e = s.size();
	while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
	return s.substr(b, e - b);
}

std::string LMStudioProvider::baseUrl() const {
	std::string b = stripTrailingSlash(cfg_.lmstudio_endpoint);
	if (b.empty()) b = "http://localhost:1234";
	return b;
}

std::string LMStudioProvider::chatUrl() const {
	std::string b = baseUrl();
	if (b.find("/v1/chat/completions") != std::string::npos) return b;
	return b + "/v1/chat/completions";
}

std::string LMStudioProvider::modelsUrl() const {
	std::string b = baseUrl();
	if (b.find("/v1/models") != std::string::npos) return b;
	return b + "/v1/models";
}

// GET simple con curl: devuelve true si HTTP 2xx y rellena body.
static bool curlGet(const std::string& url, int timeout_seconds,
                    const std::string& api_key, std::string& body_out,
                    int& http_code_out) {
	body_out.clear();
	http_code_out = 0;
	std::error_code ec;
	fs::path tmpdir = fs::temp_directory_path(ec) / "untrunc-ai-lmstudio";
	if (ec) return false;
	fs::create_directories(tmpdir, ec);
	fs::path resp_file = tmpdir / "get_response.json";
	fs::path curlrc = tmpdir / "curlrc_get";
	{
		std::ofstream f(curlrc, std::ios::trunc);
		if (!f) return false;
		f << "header = \"Accept: application/json\"\n";
		if (!api_key.empty())
			f << "header = \"Authorization: Bearer " << api_key << "\"\n";
		// El código HTTP se pide aquí, en el curlrc, y NO con -w
		// en la línea de comandos: cmd.exe expande %{...} como
		// variable de entorno y lo dejaría vacío. En el archivo
		// de configuración el % es literal en ambas plataformas.
		f << "write-out = \"%{http_code}\"\n";
		f << "max-time = " << timeout_seconds << "\n"
		  << "silent\nshow-error\n";
	}
	fs::permissions(curlrc, fs::perms::owner_read | fs::perms::owner_write, ec);
	std::string cmd = "curl -sS --config " + shellQuote(upath::fromPath(curlrc)) +
	                  " -o " + shellQuote(upath::fromPath(resp_file)) +
	                  " " + shellQuote(url) + " " + kStderrNull;
	FILE* pipe = upath::openPipe(cmd, "r");
	if (!pipe) { fs::remove(curlrc, ec); return false; }
	char buf[64] = {0};
	std::string out;
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf) - 1, pipe)) > 0) out.append(buf, n);
	upath::closePipe(pipe);
	fs::remove(curlrc, ec);
	try { http_code_out = std::stoi(out); } catch (...) { http_code_out = 0; }
	if (http_code_out < 200 || http_code_out >= 300) return false;
	std::ifstream rf(resp_file);
	std::ostringstream ss;
	ss << rf.rdbuf();
	body_out = ss.str();
	fs::remove(resp_file, ec);
	return !body_out.empty();
}

bool LMStudioProvider::listModels(std::vector<std::string>& models_out,
                                  std::string& why_not_es) const {
	models_out.clear();
	std::string why;
	if (!isAvailable(why)) { why_not_es = why; return false; }
	// isAvailable ya comprueba que el servidor responde; aqui se
	// hace el GET real de la lista.
	std::string body;
	int code = 0;
	if (!curlGet(modelsUrl(), 10, cfg_.lmstudio_api_key, body, code)) {
		why_not_es = lmstudioNotRunningEs(baseUrl());
		return false;
	}
	models_out = extractAllIds(body);
	if (models_out.empty()) {
		why_not_es = "LM Studio responde, pero no hay ningun modelo cargado. "
		             "En LM Studio, carga Qwen (3.5 o 3.6) y pulsa Start Server.";
		return false;
	}
	return true;
}

bool LMStudioProvider::resolveModel(std::string& model_out,
                                    std::string& why_not_es) const {
	if (!cfg_.lmstudio_model.empty()) {
		model_out = cfg_.lmstudio_model;
		return true;
	}
	std::vector<std::string> models;
	if (!listModels(models, why_not_es)) return false;
	for (const auto& m : models) {
		if (toLowerStr(m).find("qwen") != std::string::npos) {
			model_out = m;
			return true;
		}
	}
	model_out = models.front();
	return true;
}

bool LMStudioProvider::isAvailable(std::string& why_not_es) const {
	if (!cfg_.lmstudio_enabled) {
		why_not_es = "LM Studio (Qwen local) esta desactivado. Activalo con "
		             "--ia-lmstudio o en Ajustes > IA, y abre LM Studio con "
		             "Qwen cargado y Start Server en el puerto 1234.";
		return false;
	}
	std::string base = baseUrl();
	if (base.find("key=") != std::string::npos ||
	    base.find("api_key") != std::string::npos) {
		why_not_es = "La URL de LM Studio no debe llevar la clave (ni 'key=' ni "
		             "'api_key'). La clave, si la hay, va en el header Authorization.";
		return false;
	}
	// LM Studio es local por definicion: solo localhost / 127.0.0.1.
	// (http, no https: LM Studio sirve en http en localhost.)
	if (base.rfind("http://localhost", 0) != 0 &&
	    base.rfind("http://127.0.0.1", 0) != 0) {
		why_not_es = "El endpoint de LM Studio debe ser http://localhost:1234 "
		             "(o http://127.0.0.1:1234). LM Studio corre en tu PC, no en internet.";
		return false;
	}
	if (!hasCurl()) {
		why_not_es = "No se encontro el programa 'curl' en el sistema. LM Studio "
		             "lo necesita para la llamada. En Windows, curl viene con "
		             "Windows 10+ y con MSYS2.";
		return false;
	}
	// Comprobacion rapida de que el servidor responde (GET /v1/models,
	// timeout corto): asi el fallo es rapido y en espanol, antes de
	// intentar reparar nada.
	std::string body;
	int code = 0;
	if (!curlGet(modelsUrl(), 5, cfg_.lmstudio_api_key, body, code)) {
		why_not_es = lmstudioNotRunningEs(base);
		return false;
	}
	return true;
}

AiSuggestion LMStudioProvider::analyze(const DiagnosticPacket& packet,
                                       const AiDiagnosis& diagnosis) {
	AiSuggestion s;
	s.source = "lmstudio";
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
		s.reason_es = "LM Studio no disponible: " + why;
		return s;
	}
	if (!packet.valid) {
		s.warning_es = "Paquete de diagnóstico no válido: " + packet.error_es;
		s.reason_es = "No se envió nada: " + packet.error_es;
		return s;
	}
	std::string why2;
	if (!validateDiagnosticPacket(packet, why2)) {
		s.warning_es = why2;
		s.reason_es = "No se envió nada: " + why2;
		return s;
	}
	std::string model;
	if (!resolveModel(model, why)) {
		s.warning_es = why;
		s.reason_es = "LM Studio no pudo resolver el modelo: " + why;
		return s;
	}

	// Prompt: Qwen es un LLM de texto. Solo recibe el paquete de
	// diagnostico y debe responder JSON estricto, sin prosa.
	std::string system_prompt =
	    "Eres un asistente que ayuda a reparar videos MP4/MOV truncados. "
	    "Solo recibes un diagnostico en texto (tamanos, atomos, SPS en "
	    "hexadecimal, conteos de NAL). Nunca ves el video ni fotogramas. "
	    "Responde SOLO con un objeto JSON, sin explicaciones ni markdown: "
	    "{\"modo\":\"perfil|rescate|clasico\",\"confianza\":0.0-1.0,"
	    "\"mensaje\":\"...\",\"perfil_sugerido\":\"...\"}. "
	    "modo=perfil si el perfil sugerido encaja y se puede reparar sin "
	    "referencia; rescate si es parcial; clasico si hace falta una "
	    "referencia sana. No inventes resolucion ni fps: si no los sabes, "
	    "usa clasico con confianza baja.";
	std::string user_prompt =
	    "Diagnostico del video danado (solo texto, sin video):\n" +
	    packet.json +
	    "\nResponde SOLO el JSON pedido.";

	std::ostringstream body;
	body << "{"
	     << "\"model\":\"" << jsonEscape(model) << "\","
	     << "\"messages\":["
	     << "{\"role\":\"system\",\"content\":\"" << jsonEscape(system_prompt) << "\"},"
	     << "{\"role\":\"user\",\"content\":\"" << jsonEscape(user_prompt) << "\"}"
	     << "],"
	     << "\"temperature\":0,"
	     << "\"max_tokens\":300,"
	     << "\"stream\":false"
	     << "}";

	// Archivos temporales 0600: cuerpo de la peticion y curlrc.
	// La clave, si la hay, solo va en el header, nunca en argv/URL/logs.
	std::error_code ec;
	fs::path tmpdir = fs::temp_directory_path(ec) / "untrunc-ai-lmstudio";
	if (ec) { s.warning_es = "No se pudo crear el directorio temporal."; return s; }
	fs::create_directories(tmpdir, ec);
	fs::path body_file = tmpdir / "chat_body.json";
	fs::path curlrc = tmpdir / "curlrc";
	fs::path resp_file = tmpdir / "response.json";
	{
		std::ofstream f(body_file, std::ios::binary | std::ios::trunc);
		if (!f) { s.warning_es = "No se pudo escribir la peticion temporal."; return s; }
		f << body.str();
	}
	{
		std::ofstream f(curlrc, std::ios::trunc);
		if (!f) { s.warning_es = "No se pudo escribir la configuración temporal de curl."; return s; }
		if (!cfg_.lmstudio_api_key.empty())
			f << "header = \"Authorization: Bearer " << cfg_.lmstudio_api_key << "\"\n";
		f << "header = \"Content-Type: application/json\"\n"
		  << "header = \"Accept: application/json\"\n"
		  // write-out en el curlrc, no -w en la línea (ver curlGet:
		  // cmd.exe expandiría %{http_code} como variable de entorno).
		  << "write-out = \"%{http_code}\"\n"
		  << "max-time = " << cfg_.lmstudio_timeout_seconds << "\n"
		  << "silent\nshow-error\n";
	}
	fs::permissions(body_file, fs::perms::owner_read | fs::perms::owner_write, ec);
	fs::permissions(curlrc, fs::perms::owner_read | fs::perms::owner_write, ec);

	std::string response;
	int http_code = 0;
	bool sent = false;
	for (int attempt = 0; attempt <= cfg_.max_retries; attempt++) {
		std::string cmd = "curl -sS --config " + shellQuote(upath::fromPath(curlrc)) +
		                  " --data-binary @" + shellQuote(upath::fromPath(body_file)) +
		                  " -o " + shellQuote(upath::fromPath(resp_file)) +
		                  " " + shellQuote(chatUrl()) +
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
	std::error_code ec2;
	fs::remove(curlrc, ec2);
	fs::remove(body_file, ec2);

	if (!sent) {
		s.warning_es = lmstudioNotRunningEs(baseUrl()) +
		               " (HTTP " + std::to_string(http_code) + "). Se sigue solo "
		               "con la IA local; el video nunca salió de tu PC.";
		s.reason_es = "LM Studio sin respuesta; se usa la sugerencia local.";
		return s;
	}
	std::ifstream rf(resp_file);
	std::ostringstream ss;
	ss << rf.rdbuf();
	std::string resp = ss.str();
	fs::remove(resp_file, ec2);
	if (resp.empty() || resp.size() > 64 * 1024) {
		s.warning_es = "LM Studio respondió con un texto vacío o demasiado grande; se ignora.";
		return s;
	}
	// Respuesta OpenAI: {"choices":[{"message":{"content":"{...}"}}]}
	// El contenido es el JSON que pidio el prompt (a veces con cercas).
	std::string content;
	if (!extractJsonString(resp, "content", content)) {
		s.warning_es = "LM Studio respondió sin contenido de chat legible; se ignora "
		               "y se sigue con la IA local.";
		return s;
	}
	std::string inner = stripCodeFences(content);
	std::string mode_str;
	if (jsonFindString(inner, "modo", mode_str) || jsonFindString(inner, "mode", mode_str)) {
		if (mode_str == "perfil") s.mode = AiMode::Perfil;
		else if (mode_str == "rescate") s.mode = AiMode::Rescate;
		else s.mode = AiMode::Clasico;
	} else {
		s.warning_es = "Qwen respondió sin un modo válido; se ignora y se sigue con la IA local.";
		return s;
	}
	double conf = 0;
	if (jsonFindNumber(inner, "confianza", conf) || jsonFindNumber(inner, "confidence", conf)) {
		s.confidence = std::max(0.0, std::min(1.0, conf));
	} else {
		s.confidence = 0.5;
	}
	std::string msg;
	if (jsonFindString(inner, "mensaje", msg) || jsonFindString(inner, "message", msg)) {
		s.reason_es = "LM Studio (Qwen, modelo " + model + "): " + msg;
	} else {
		s.reason_es = "LM Studio (Qwen, modelo " + model + "): modo '" + aiModeName(s.mode) + "'.";
	}
	std::string perfil;
	if (jsonFindString(inner, "perfil_sugerido", perfil) && !perfil.empty())
		s.profile_id = perfil;
	// Las dimensiones SIEMPRE vienen del diagnostico local, nunca del LLM.
	s.width = diagnosis.width;
	s.height = diagnosis.height;
	s.fps = diagnosis.fps_estimado;
	s.codec = diagnosis.codec;
	s.use_step = true;
	s.use_dyn = true;
	return s;
}
