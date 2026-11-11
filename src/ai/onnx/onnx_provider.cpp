/*
	Untrunc-mejorado (Fase 2.5, prototipo ONNX) - onnx_provider.cpp
	Licencia: GNU GPL-2.0 (ver COPYING en la raíz).
*/

#include "onnx_provider.h"
#include "../../common.h"
#include "../../upath.h"
#include "../../tempfs.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>

#ifndef _WIN32
#include <dlfcn.h>
#include <sys/wait.h>
#else
#include <windows.h>
#endif

namespace fs = std::filesystem;

static const int kVentana = 256;
static const int kFeatures = 264;

// ---------------------------------------------------------------------------
// Rutas
// ---------------------------------------------------------------------------
std::string onnxTrainingDir() {
	std::string home = upath::getenv(
#ifdef _WIN32
	    "USERPROFILE"
#else
	    "HOME"
#endif
	);
	if (home.empty()) {
#ifdef _WIN32
		home = upath::getenv("HOME");
#else
		home = upath::getenv("USERPROFILE");
#endif
	}
	if (home.empty()) return "";
	return upath::fromPath(upath::toPath(home) / ".config" / "untrunc" / "entrenamiento");
}

std::string onnxModelActivoDir() {
	std::string d = onnxTrainingDir();
	return d.empty() ? "" : upath::fromPath(upath::toPath(d) / "modelo_activo");
}

std::string onnxModelAnteriorDir() {
	std::string d = onnxTrainingDir();
	return d.empty() ? "" : upath::fromPath(upath::toPath(d) / "modelo_activo_anterior");
}

std::string onnxDatasetDir() {
	std::string d = onnxTrainingDir();
	return d.empty() ? "" : upath::fromPath(upath::toPath(d) / "dataset");
}

// Ruta del ejecutable guardada al arrancar (ver cabecera).
const char* g_exe_argv0 = nullptr;
void setExeArgv0(const char* argv0) { g_exe_argv0 = argv0; }

// argv0 efectivo: el argumento explícito si lo hay, si no el
// global fijado al arrancar. Sin esto, las llamadas sin argv0
// (OnnxProvider en el orquestador, GUI) no veían el ZIP.
static const char* effectiveArgv0(const char* argv0) {
	if (argv0 && *argv0) return argv0;
	if (g_exe_argv0 && *g_exe_argv0) return g_exe_argv0;
	return nullptr;
}

static std::string exeDir(const char* argv0) {
	argv0 = effectiveArgv0(argv0);
	if (argv0 && *argv0) {
		std::error_code ec;
		fs::path p = upath::toPath(argv0);
		// Si argv0 tiene directorio (p. ej. "./untrunc" o
		// "C:\carpeta\untrunc-gui.exe"), ese es el exe.
		if (p.has_parent_path()) {
			fs::path abs = fs::absolute(p, ec);
			if (!ec) return upath::fromPath(abs.parent_path());
		}
		// Nombre sin directorio (lanzado por PATH): la ruta
		// real del ejecutable, no el directorio de trabajo.
		std::string self = upath::executablePath();
		if (!self.empty())
			return upath::fromPath(upath::toPath(self).parent_path());
	}
	return "";
}

std::string findTrainScript(const char* argv0) {
	const std::string env = upath::getenv("UNTRUNC_TRAIN_SCRIPT");
	if (!env.empty() && fs::exists(upath::toPath(env))) return env;
	std::string dir = exeDir(effectiveArgv0(argv0));
	std::vector<fs::path> candidatos;
	if (!dir.empty()) {
		candidatos.push_back(upath::toPath(dir) / "tools" / "train_onnx.py");
		candidatos.push_back(upath::toPath(dir) / "src" / "ai" / "onnx" / "train_onnx.py");
		candidatos.push_back(upath::toPath(dir) / ".." / "src" / "ai" / "onnx" / "train_onnx.py");
	}
	candidatos.push_back(fs::path("src") / "ai" / "onnx" / "train_onnx.py");
	candidatos.push_back(fs::path("tools") / "train_onnx.py");
	for (auto& c : candidatos) {
		std::error_code ec;
		if (fs::exists(c, ec)) return upath::fromPath(fs::absolute(c, ec));
	}
	return "";
}

std::string findBaseModelDir(const char* argv0) {
	const std::string env = upath::getenv("UNTRUNC_ONNX_BASE");
	if (!env.empty() && fs::exists(upath::toPath(env) / "model_info.json")) return env;
	std::string dir = exeDir(effectiveArgv0(argv0));
	std::vector<fs::path> candidatos;
	if (!dir.empty()) {
		candidatos.push_back(upath::toPath(dir) / "models" / "onnx_base");
		candidatos.push_back(upath::toPath(dir) / "src" / "ai" / "onnx" / "modelo_base");
		candidatos.push_back(upath::toPath(dir) / ".." / "src" / "ai" / "onnx" / "modelo_base");
	}
	candidatos.push_back(fs::path("src") / "ai" / "onnx" / "modelo_base");
	for (auto& c : candidatos) {
		std::error_code ec;
		if (fs::exists(c / "model_info.json", ec)) return upath::fromPath(fs::absolute(c, ec));
	}
	return "";
}

// ---------------------------------------------------------------------------
// Utilidades JSON mínimas (sin librería externa): extraer un string,
// un número o un array de floats por clave.
// ---------------------------------------------------------------------------
static bool jsonString(const std::string& json, const std::string& key, std::string& out) {
	return jsonFindString(json, key, out);
}

static bool jsonNumber(const std::string& json, const std::string& key, double& out) {
	return jsonFindNumber(json, key, out);
}

static bool jsonFloatArray(const std::string& json, const std::string& key,
                           std::vector<float>& out) {
	std::string pat = "\"" + key + "\"";
	size_t pos = json.find(pat);
	if (pos == std::string::npos) return false;
	pos = json.find('[', pos + pat.size());
	if (pos == std::string::npos) return false;
	size_t end = json.find(']', pos);
	if (end == std::string::npos) return false;
	out.clear();
	std::string body = json.substr(pos + 1, end - pos - 1);
	std::stringstream ss(body);
	std::string tok;
	while (std::getline(ss, tok, ',')) {
		try {
			out.push_back(std::stof(tok));
		} catch (...) {
			return false;
		}
	}
	return !out.empty();
}

static std::string readFileText(const std::string& path) {
	std::ifstream f(upath::toPath(path));
	if (!f) return "";
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static bool ortDetectado() {
#ifdef _WIN32
	// En Windows la detección se hace intentando cargar la DLL por
	// nombre; si no está en el PATH, no está disponible y se usa la
	// inferencia nativa (ver cabecera).
	HMODULE h = LoadLibraryA("onnxruntime.dll");
	if (h) { FreeLibrary(h); return true; }
	return false;
#else
	void* h = dlopen("libonnxruntime.so.1", RTLD_LAZY | RTLD_LOCAL);
	if (!h) h = dlopen("libonnxruntime.so", RTLD_LAZY | RTLD_LOCAL);
	if (!h) return false;
	// Solo se comprueba que existe el símbolo de entrada de la API C.
	void* sym = dlsym(h, "OrtGetApiBase");
	dlclose(h);
	return sym != nullptr;
#endif
}

bool readOnnxModelInfo(const std::string& dir, OnnxModelInfo& info) {
	info = OnnxModelInfo();
	if (dir.empty()) {
		info.error_es = "No hay carpeta de modelo configurada.";
		return false;
	}
	std::string txt = readFileText(upath::fromPath(upath::toPath(dir) / "model_info.json"));
	if (txt.empty()) {
		info.error_es = "No hay model_info.json en " + dir + " (modelo no entrenado).";
		return false;
	}
	info.ruta = dir;
	double d = 0;
	std::string s;
	if (jsonString(txt, "nombre", s)) info.nombre = s;
	if (jsonString(txt, "arquitectura", s)) info.arquitectura = s;
	if (jsonNumber(txt, "tamano_bytes", d)) info.tamano_bytes = (size_t)d;
	if (jsonNumber(txt, "ejemplos", d)) info.ejemplos = (int)d;
	if (jsonNumber(txt, "precision_validacion", d)) info.precision_validacion = d;
	if (jsonNumber(txt, "precision_baseline", d)) info.precision_baseline = d;
	if (jsonString(txt, "fecha", s)) info.fecha = s;
	if (jsonNumber(txt, "onnx_exportado", d)) info.onnx_exportado = (d != 0);
	// onnx_exportado es booleano en el JSON (true/false), no número:
	// si no se leyó como número, buscar el literal.
	if (txt.find("\"onnx_exportado\": true") != std::string::npos)
		info.onnx_exportado = true;
	if (txt.find("\"aceptado\": false") != std::string::npos) {
		info.error_es = "El modelo de esa carpeta fue rechazado en validación "
		                "(no superó a la heurística). No se usa.";
		return false;
	}
	info.ort_detectado = ortDetectado();
	info.cargado = true;
	return true;
}

// ---------------------------------------------------------------------------
// Ejecución del script de entrenamiento
// ---------------------------------------------------------------------------
static std::string shellQuote(const std::string& a) {
#ifdef _WIN32
	// Puntos 2/3 (2026-11-07, corrección del fallo en Windows):
	// cmd.exe, al recibir el comando tras /C, aplica su regla
	// documentada de comillas: si la línea tiene más de dos
	// comillas, quita el primer carácter si es `"` y elimina la
	// ÚLTIMA comilla de la línea. runTrainScript entrecomillaba
	// TODOS los tokens, así que el comando empezaba con `"python"`
	// y cmd.exe lo dejaba en `python" ...` (programa inexistente,
	// rc=1), y la comilla de cierre del archivo temporal también
	// desaparecía, así que la redirección no creaba el archivo y
	// la GUI decía "Sin salida del script". El script nunca
	// llegaba a ejecutarse: por eso fallaban las tres vías
	// (entrenar, anexar, probar) aunque el script y el fallback a
	// NumPy estuvieran bien (a mano, en cmd.exe interactivo, esa
	// regla no se aplica y todo funcionaba; lo confirmó Angel el
	// 2026-11-07 18:44 corriendo el script nuevo a mano: entrenó
	// con NumPy, 96.6% vs 62.6%). Los comandos de curl no tenían
	// este problema porque empiezan con `curl` sin comillas.
	// Ahora solo se entrecomilla el token si lo necesita (espacio
	// o carácter especial de cmd.exe); `python` queda sin comillas
	// y el comando ya no empieza con `"`.
	if (a.empty()) return "\"\"";
	bool need = false;
	for (char c : a) {
		if (c == ' ' || c == '\t' || c == '"' || c == '&' || c == '<' ||
		    c == '>' || c == '(' || c == ')' || c == '@' || c == '^' ||
		    c == '|') {
			need = true;
			break;
		}
	}
	if (!need) return a;
	std::string out = "\"";
	for (char c : a) {
		if (c == '"') out += "\\\"";
		else out += c;
	}
	out += "\"";
	return out;
#else
	std::string out = "'";
	for (char c : a) {
		if (c == '\'') out += "'\\''";
		else out += c;
	}
	out += "'";
	return out;
#endif
}

// Salida de la última ejecución del script (punto 3, 2026-11-07):
// se guarda para mostrarla en la ventana de detalles de la pestaña
// Entrenar. Recortada a 64 KB; el script solo imprime nombres base,
// conteos y errores, nunca vídeos ni rutas completas.
static std::string g_last_train_script_output;
const std::string& lastTrainScriptOutput() { return g_last_train_script_output; }

// Devuelve las últimas líneas de un texto (máx. `max_lineas`),
// para que el diálogo de error enseñe lo esencial sin inundar.
static std::string lastLines(const std::string& text, int max_lineas) {
	if (text.empty()) return "";
	std::vector<std::string> lines;
	std::string cur;
	for (char c : text) {
		if (c == '\n') { lines.push_back(cur); cur.clear(); }
		else if (c != '\r') cur += c;
	}
	if (!cur.empty()) lines.push_back(cur);
	while (!lines.empty() && lines.back().empty()) lines.pop_back();
	if ((int)lines.size() <= max_lineas) {
		std::string out;
		for (auto& l : lines) out += l + "\n";
		return out;
	}
	std::string out = "...\n";
	for (size_t i = lines.size() - (size_t)max_lineas; i < lines.size(); i++)
		out += lines[i] + "\n";
	return out;
}

int runTrainScript(const std::vector<std::string>& args, std::string& why_es,
                   const char* argv0) {
	std::string script = findTrainScript(argv0);
	if (script.empty()) {
		why_es = "No se encontró train_onnx.py. Busca el script en la carpeta "
		         "del programa (tools/train_onnx.py) o indica su ruta con la "
		         "variable UNTRUNC_TRAIN_SCRIPT. Sin el script no se puede "
		         "entrenar desde la GUI; el entrenamiento manual es:\n"
		         "  python3 train_onnx.py entrenar --listas "
		         "~/.config/untrunc/entrenamiento";
		return -1;
	}
#ifdef _WIN32
	std::string python = "python";
#else
	std::string python = "python3";
#endif
	const std::string env_py = upath::getenv("UNTRUNC_PYTHON");
	if (!env_py.empty()) python = env_py;
#ifdef _WIN32
	// Si el intérprete tiene espacios en su ruta (p. ej.
	// UNTRUNC_PYTHON=C:\Program Files\...), entrecomillarlo haría
	// que el comando empezara con `"` y cmd.exe lo rompería igual
	// que antes. Se intenta su ruta corta 8.3 (sin espacios); si no
	// se puede, se usa entrecomillado y el diagnóstico de abajo lo
	// dirá. El `python` de PATH (caso normal) no tiene espacios y
	// no pasa por aquí.
	if (python.find(' ') != std::string::npos) {
		// Versión W: la ruta puede tener acentos (C:\Users\José\...).
		const std::wstring wpy = upath::toPath(python).wstring();
		wchar_t buf[MAX_PATH];
		DWORD n = GetShortPathNameW(wpy.c_str(), buf, MAX_PATH);
		if (n > 0 && n < MAX_PATH) python = upath::fromPath(fs::path(std::wstring(buf, n)));
	}
#endif
	// Punto 3 (2026-11-07): capturar stdout+stderr en un archivo
	// temporal y leerlo al terminar. Antes se usaba std::system()
	// tal cual y la salida se perdía: la GUI decía "revisa el
	// mensaje en la ventana de detalles" y ahí no había nada.
	// El archivo temporal se borra siempre al terminar; solo
	// queda su contenido en memoria (g_last_train_script_output).
	std::error_code ec;
	tempfs::ScopedDir tmpdir("train");  // unica; se borra al salir
	fs::path out_file;
	if (tmpdir.ok()) out_file = tmpdir / "salida.txt";
	std::string cmd = shellQuote(python) + " " + shellQuote(script);
	for (auto& a : args) cmd += " " + shellQuote(a);
	if (!out_file.empty())
		cmd += " > " + shellQuote(upath::fromPath(out_file)) + " 2>&1";
	int raw = upath::system(cmd);
	// std::system devuelve en POSIX el estado de espera (código de
	// salida << 8), no el código en sí: hay que decodificarlo, o el
	// llamador no puede distinguir "sin base" (2) de "modelo
	// rechazado por la puerta de calidad" (4) ni de un error (1/5).
	// En Windows, system() ya devuelve el código directamente.
	// (Antes se devolvía `raw` tal cual: la GUI comparaba rc == 4
	// y ese caso nunca se cumplía, porque llegaba 1024.)
#ifdef _WIN32
	int rc = raw;
#else
	int rc = -1;
	if (raw == -1) rc = -1;
	else if (WIFEXITED(raw)) rc = WEXITSTATUS(raw);
	else if (WIFSIGNALED(raw)) rc = 128 + WTERMSIG(raw);
#endif
	// Punto 2/3 (2026-11-07): si la captura falla, no dejar la
	// ventana de detalles en "Sin salida del script": guardar el
	// MOTIVO (qué archivo temporal, si se creó, y el comando) para
	// que la GUI lo muestre en vez de un genérico. El comando se
	// recorta: lo diagnóstico está al principio (intérprete,
	// script, subcomando); las rutas de vídeo van al final.
	bool captured = false;
	if (!out_file.empty()) {
		std::error_code ec2;
		bool out_exists = fs::exists(out_file, ec2);
		std::ifstream f(out_file);
		if (f) {
			std::stringstream ss;
			ss << f.rdbuf();
			g_last_train_script_output = ss.str();
			// Recortar a 64 KB (desde el final: lo útil está abajo).
			const size_t kMax = 64 * 1024;
			if (g_last_train_script_output.size() > kMax)
				g_last_train_script_output =
				    g_last_train_script_output.substr(
				        g_last_train_script_output.size() - kMax);
			captured = true;
		} else {
			g_last_train_script_output.clear();
			std::string cmd_corto = cmd;
			if (cmd_corto.size() > 500)
				cmd_corto = cmd_corto.substr(0, 500) + " ...";
			g_last_train_script_output =
			    "No se pudo leer la salida del script de entrenamiento.\n"
			    "Archivo temporal: " + upath::fromPath(out_file) +
			    (out_exists ? " (existe pero no se pudo abrir)." :
			                  " (no se creó: el comando no llegó a ejecutarse o la redirección falló).") +
			    "\nComando ejecutado: " + cmd_corto +
			    "\nCódigo de salida: " + std::to_string(rc) + ".";
		}
		fs::remove(out_file, ec);
	} else {
		// No se pudo determinar la carpeta temporal del sistema:
		// no se intentó capturar nada.
		g_last_train_script_output =
		    "No se pudo determinar la carpeta temporal del sistema; "
		    "no se intentó capturar la salida del script.";
	}
	// Si el script terminó bien (rc=0) o en un estado informativo
	// (rc=2: p. ej. "Sin base todavía" en resumen), su salida
	// vuelve a stdout como antes: el CLI (resumen, probar,
	// entrenar...) depende de ella y las pruebas la leen de ahí.
	// Sin esto, la captura se tragaba la salida y `--resumen-onnx`
	// y `--probar-onnx` salían vacíos (regresión detectada por las
	// baterías ONNX 17/18 y base persistente 25/32, 2026-11-07).
	// En error real (rc 1/3/4/5) NO se vuelca: why_es ya lleva
	// las últimas líneas al diálogo/stderr y volcarlo todo
	// duplicaría el texto. En la GUI, cout va al registro; la
	// ventana de detalles usa lastTrainScriptOutput().
	if (captured && (rc == 0 || rc == 2) && !g_last_train_script_output.empty())
		std::cout << g_last_train_script_output << std::flush;
	if (rc != 0) {
		why_es = "El script de entrenamiento terminó con error (código " +
		         std::to_string(rc) + ").";
		std::string tail = lastLines(g_last_train_script_output, 12);
		if (!tail.empty())
			why_es += "\nSalida del script (últimas líneas):\n" + tail;
		else
			why_es += "\nNo se pudo leer la salida del script; revisa la "
			          "ventana de detalles de la pestaña Entrenar.";
		// La pista de "falta PyTorch o numpy" solo si la salida lo
		// dice: antes aparecía siempre y confundía (el caso real de
		// Angel era torch ROTO, no ausente, y la GUI ni siquiera
		// llegaba a ejecutar el script).
		if (g_last_train_script_output.find("falta numpy") != std::string::npos ||
		    g_last_train_script_output.find("no está instalado") != std::string::npos ||
		    g_last_train_script_output.find("No module named") != std::string::npos)
			why_es += "\nSi falta PyTorch o numpy, el propio script dice el "
			          "comando exacto para instalarlo.";
	}
	return rc;
}

// ---------------------------------------------------------------------------
// OnnxProvider
// ---------------------------------------------------------------------------
namespace {
std::unique_ptr<OnnxProvider> g_resinc_modelo;
std::string g_resinc_ruta;

double puntuarInicioVideo(const unsigned char* data, size_t len) {
	if (!g_resinc_modelo) return 0.0;
	OnnxWindowResult r = g_resinc_modelo->classifyWindow(data, len);
	// Inicio de video o de SPS (tambien abre un fotograma de video).
	return r.probs[(int)OnnxClase::InicioVideo] + r.probs[(int)OnnxClase::InicioSps];
}
} // namespace

bool activarResincronizacionOnnx(const AiConfig& cfg, std::string& modelo_es,
                                 std::string& why_es) {
	auto p = std::make_unique<OnnxProvider>(cfg);
	if (!p->isAvailable(why_es)) {
		desactivarResincronizacionOnnx();
		return false;
	}
	const OnnxModelInfo& info = p->modelInfo();
	modelo_es = info.nombre;
	if (!g_resinc_modelo || g_resinc_ruta != info.ruta) {
		g_resinc_modelo = std::move(p);
		g_resinc_ruta = info.ruta;
	}
	g_resync_scorer = puntuarInicioVideo;
	return true;
}

void desactivarResincronizacionOnnx() {
	g_resync_scorer = nullptr;
}

OnnxProvider::OnnxProvider(const AiConfig& cfg, const char* argv0) : cfg_(cfg) {
	// argv0 explícito si lo hay; si no, el global fijado al
	// arrancar (setExeArgv0) o el respaldo relativo en desarrollo.
	// Antes se llamaba a findBaseModelDir() sin argumento y desde
	// un ZIP extraído el modelo base no se encontraba (Fase 3 §6).
	// Orden: ruta configurada, modelo activo, copia de seguridad del
	// anterior, modelo base. Antes, si el modelo activo tenia
	// model_info.json pero le faltaba pesos.json, se elegia igual,
	// no cargaba y ONNX quedaba muerto (sin pasar al siguiente).
	std::string dir = onnxModeloEnUso(cfg_.onnx_model_path, argv0, info_, aviso_carga_);
	if (!dir.empty()) weights_loaded_ = loadWeights(dir);
	if (!weights_loaded_ && info_.cargado) {
		info_.cargado = false;
		info_.error_es = "El modelo tiene model_info.json pero no se pudieron "
		                 "leer sus pesos (pesos.json). Se degrada a heurísticas.";
	}
}

namespace {
bool pesosValidos(const std::string& dir) {
	std::string txt = readFileText(upath::fromPath(upath::toPath(dir) / "pesos.json"));
	if (txt.empty()) return false;
	std::vector<float> v;
	const struct { const char* k; size_t n; } formas[] = {
	    {"W1", 264 * 64}, {"b1", 64}, {"W2", 64 * 32}, {"b2", 32}, {"W3", 32 * 4}, {"b3", 4}};
	for (const auto& f : formas)
		if (!jsonFloatArray(txt, f.k, v) || v.size() != f.n) return false;
	return true;
}
} // namespace

bool onnxModelUsable(const std::string& dir, std::string& why_es) {
	OnnxModelInfo info;
	if (!readOnnxModelInfo(dir, info)) {
		why_es = info.error_es;
		return false;
	}
	if (!pesosValidos(dir)) {
		why_es = "Al modelo de " + dir + " le falta pesos.json o está dañado; "
		         "sin él no se puede usar.";
		return false;
	}
	return true;
}

std::string onnxModeloEnUso(const std::string& ruta_configurada, const char* argv0,
                            OnnxModelInfo& info, std::string& aviso_es) {
	aviso_es.clear();
	const char* eff = effectiveArgv0(argv0);
	const std::pair<std::string, const char*> candidatos[] = {
	    {ruta_configurada, "la carpeta configurada"},
	    {onnxModelActivoDir(), "tu modelo entrenado"},
	    {onnxModelAnteriorDir(), "la copia de seguridad de tu modelo anterior"},
	    {findBaseModelDir(eff), "el modelo base incluido"}};
	for (const auto& c : candidatos) {
		if (c.first.empty()) continue;
		std::string why;
		if (onnxModelUsable(c.first, why)) {
			readOnnxModelInfo(c.first, info);
			return c.first;
		}
		// Solo se avisa de modelos que existen pero estan rotos
		// (no de carpetas vacias o modelos rechazados a proposito).
		OnnxModelInfo tmp;
		if (readOnnxModelInfo(c.first, tmp))
			aviso_es += std::string("No se pudo usar ") + c.second + ": " + why + " ";
	}
	info = OnnxModelInfo();
	info.error_es = aviso_es.empty() ? "No hay ningún modelo ONNX disponible." : aviso_es;
	return "";
}

bool OnnxProvider::loadWeights(const std::string& dir) {
	std::string txt = readFileText(upath::fromPath(upath::toPath(dir) / "pesos.json"));
	if (txt.empty()) return false;
	if (!jsonFloatArray(txt, "W1", w1_) || w1_.size() != 264 * 64) return false;
	if (!jsonFloatArray(txt, "b1", b1_) || b1_.size() != 64) return false;
	if (!jsonFloatArray(txt, "W2", w2_) || w2_.size() != 64 * 32) return false;
	if (!jsonFloatArray(txt, "b2", b2_) || b2_.size() != 32) return false;
	if (!jsonFloatArray(txt, "W3", w3_) || w3_.size() != 32 * 4) return false;
	if (!jsonFloatArray(txt, "b3", b3_) || b3_.size() != 4) return false;
	return true;
}

bool OnnxProvider::isAvailable(std::string& why_not_es) const {
	if (!cfg_.onnx_enabled) {
		why_not_es = "El modelo ONNX entrenado está desactivado. Actívalo con "
		             "--usar-onnx o en la pestaña «Entrenar» (casilla "
		             "«Usar modelo entrenado en la reparación»).";
		return false;
	}
	if (!info_.cargado || !weights_loaded_) {
		why_not_es = info_.error_es.empty()
		             ? "No hay ningún modelo ONNX cargado (ni entrenado por ti "
		               "ni el modelo base incluido). Ve a la pestaña «Entrenar» "
		               "y crea un dataset con tus vídeos, o restablece el modelo base."
		             : info_.error_es;
		return false;
	}
	return true;
}

void OnnxProvider::extractFeatures(const unsigned char* data, size_t len, float* out) {
	// IDÉNTICA a train_onnx.py extraer_features (ver su cabecera).
	unsigned char w[kVentana] = {0};
	size_t n = std::min(len, (size_t)kVentana);
	for (size_t i = 0; i < n; i++) w[i] = data[i];
	for (int i = 0; i < kVentana; i++) out[i] = w[i] / 255.0f;
	uint32_t be32 = (uint32_t(w[0]) << 24) | (uint32_t(w[1]) << 16) |
	                (uint32_t(w[2]) << 8) | uint32_t(w[3]);
	bool plausible = be32 >= 5 && be32 <= 262144;
	out[256] = plausible ? 1.0f : 0.0f;
	bool annexb = (w[0] == 0 && w[1] == 0 && w[2] == 0 && w[3] == 1) ||
	              (w[0] == 0 && w[1] == 0 && w[2] == 1);
	out[257] = annexb ? 1.0f : 0.0f;
	int nal_tipo = 0;
	if (plausible) nal_tipo = w[4] & 0x1F;
	else if (annexb) {
		int off = (w[0] == 0 && w[1] == 0 && w[2] == 0 && w[3] == 1) ? 4 : 3;
		nal_tipo = w[off] & 0x1F;
	}
	out[258] = nal_tipo / 31.0f;
	int zeros = 0, ffs = 0;
	int counts[256] = {0};
	for (int i = 0; i < kVentana; i++) {
		if (w[i] == 0) zeros++;
		if (w[i] == 0xFF) ffs++;
		counts[w[i]]++;
	}
	out[259] = zeros / 256.0f;
	out[260] = ffs / 256.0f;
	double ent = 0;
	for (int c = 0; c < 256; c++) {
		if (counts[c]) {
			double p = counts[c] / 256.0;
			ent -= p * (std::log(p) / std::log(2.0));
		}
	}
	out[261] = (float)(ent / 8.0);
	out[262] = (w[0] == 0 && w[1] == 0) ? 1.0f : 0.0f;
	out[263] = plausible ? std::min(be32 / 262144.0f, 1.0f) : 0.0f;
}

OnnxWindowResult OnnxProvider::classifyWindow(const unsigned char* data, size_t len) const {
	OnnxWindowResult r;
	if (!weights_loaded_ || !data || len == 0) return r;
	float f[kFeatures];
	extractFeatures(data, len, f);
	// MLP 264 -> 64 -> 32 -> 4, ReLU, Softmax (mismos pesos que el .onnx).
	float h1[64], h2[32], logits[4];
	for (int j = 0; j < 64; j++) {
		double s = b1_[j];
		for (int i = 0; i < 264; i++) s += f[i] * w1_[i * 64 + j];
		h1[j] = (float)std::max(s, 0.0);
	}
	for (int j = 0; j < 32; j++) {
		double s = b2_[j];
		for (int i = 0; i < 64; i++) s += h1[i] * w2_[i * 32 + j];
		h2[j] = (float)std::max(s, 0.0);
	}
	double mx = -1e30;
	for (int j = 0; j < 4; j++) {
		double s = b3_[j];
		for (int i = 0; i < 32; i++) s += h2[i] * w3_[i * 4 + j];
		logits[j] = (float)s;
		if (s > mx) mx = s;
	}
	double sum = 0;
	for (int j = 0; j < 4; j++) {
		r.probs[j] = std::exp(logits[j] - mx);
		sum += r.probs[j];
	}
	for (int j = 0; j < 4; j++) r.probs[j] /= sum;
	r.clase = 0;
	for (int j = 1; j < 4; j++)
		if (r.probs[j] > r.probs[r.clase]) r.clase = j;
	r.confianza = r.probs[r.clase];
	return r;
}

AiSuggestion OnnxProvider::analyze(const DiagnosticPacket& packet,
                                   const AiDiagnosis& diagnosis) {
	(void)packet;
	(void)diagnosis;
	AiSuggestion s;
	s.source = "onnx";
	s.confidence = 0.0;
	s.reason_es = "El modelo ONNX necesita el archivo dañado para muestrear "
	              "ventanas de su mdat; con solo el paquete de diagnóstico no "
	              "puede opinar. El orquestador usa analyzeFile() cuando ONNX "
	              "está activado.";
	s.warning_es = "ONNX no evaluado (falta el archivo). Se sigue con heurísticas.";
	return s;
}

AiSuggestion OnnxProvider::analyzeFile(const std::string& path,
                                       const std::string& profile_override) {
	AiSuggestion s;
	s.source = "onnx";
	s.profile_id = profile_override;
	std::string why;
	if (!isAvailable(why)) {
		s.confidence = 0.0;
		s.reason_es = "ONNX no disponible: " + why;
		return s;
	}
	// Localizar el mdat y muestrear ventanas (máx. 64 MB, como el
	// frame_scanner; una ventana cada 4 KB + en los inicios que el
	// recorrido por longitud encuentra).
	std::ifstream f(upath::toPath(path), std::ios::binary);
	if (!f) {
		s.confidence = 0.0;
		s.reason_es = "ONNX: no se pudo abrir el archivo dañado.";
		return s;
	}
	std::error_code ec;
	uint64_t file_size = (uint64_t)fs::file_size(upath::toPath(path), ec);
	if (ec || file_size == 0) {
		s.confidence = 0.0;
		s.reason_es = "ONNX: archivo vacío o inexistente.";
		return s;
	}
	auto readBe32 = [](const unsigned char* p) {
		return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
		       (uint32_t(p[2]) << 8) | uint32_t(p[3]);
	};
	uint64_t offset = 0, mdat_off = 0, mdat_len = 0;
	int guard = 0;
	while (offset + 8 <= file_size && guard++ < 1000) {
		unsigned char hdr[16] = {0};
		f.seekg((std::streamoff)offset);
		f.read(reinterpret_cast<char*>(hdr), 8);
		if (f.gcount() < 8) break;
		uint32_t sz = readBe32(hdr);
		std::string type(reinterpret_cast<const char*>(hdr + 4), 4);
		uint64_t atom_size = sz, header = 8;
		if (sz == 1) {
			f.read(reinterpret_cast<char*>(hdr), 8);
			if (f.gcount() < 8) break;
			uint64_t v = 0;
			for (int i = 0; i < 8; i++) v = (v << 8) | hdr[i];
			atom_size = v; header = 16;
		} else if (sz == 0) {
			atom_size = file_size - offset;
		}
		if (atom_size < header) break;
		if (type == "mdat") { mdat_off = offset + header; mdat_len = atom_size - header; break; }
		offset += atom_size;
	}
	if (mdat_len == 0) {
		s.confidence = 0.0;
		s.reason_es = "ONNX: no se encontró bloque mdat en el archivo dañado.";
		return s;
	}
	size_t to_read = (size_t)std::min<uint64_t>(mdat_len, 64 << 20);
	std::vector<unsigned char> buf(to_read);
	f.seekg((std::streamoff)mdat_off);
	f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)to_read);
	buf.resize((size_t)f.gcount());
	if (buf.size() < (size_t)kVentana) {
		s.confidence = 0.0;
		s.reason_es = "ONNX: el mdat es demasiado pequeño para muestrear.";
		return s;
	}
	int ventanas = 0, inicios_video = 0, inicios_sps = 0;
	double conf_media_video = 0;
	// Muestreo 1: una ventana cada 4 KB.
	for (size_t off = 0; off + kVentana <= buf.size(); off += 4096) {
		OnnxWindowResult r = classifyWindow(buf.data() + off, kVentana);
		ventanas++;
		if (r.clase == (int)OnnxClase::InicioVideo && r.confianza >= cfg_.onnx_threshold) {
			inicios_video++;
			conf_media_video += r.confianza;
		} else if (r.clase == (int)OnnxClase::InicioSps && r.confianza >= cfg_.onnx_threshold) {
			inicios_sps++;
		}
	}
	// Muestreo 2: inicios que encuentra el recorrido por longitud.
	size_t off = 0;
	while (off + 5 <= buf.size()) {
		uint32_t ln = readBe32(buf.data() + off);
		if (ln >= 5 && ln <= 1048576) {
			int tipo = buf[off + 4] & 0x1F;
			if (tipo == 1 || tipo == 5 || tipo == 7) {
				OnnxWindowResult r = classifyWindow(buf.data() + off, kVentana);
				ventanas++;
				if (r.clase == (int)OnnxClase::InicioVideo && r.confianza >= cfg_.onnx_threshold) {
					inicios_video++;
					conf_media_video += r.confianza;
				} else if (r.clase == (int)OnnxClase::InicioSps) {
					inicios_sps++;
				}
			}
			off += 4 + ln;
		} else {
			off += 1;
		}
	}
	if (ventanas < 20 || inicios_video < 5) {
		s.confidence = 0.30;
		s.mode = AiMode::Rescate;
		s.reason_es = "ONNX (" + info_.nombre + "): solo encontró " +
		              std::to_string(inicios_video) + " inicios de vídeo con "
		              "confianza en " + std::to_string(ventanas) +
		              " ventanas muestreadas; no basta para fiarse. Se sigue "
		              "con heurísticas.";
		return s;
	}
	double frac = (double)inicios_video / (double)ventanas;
	double conf = 0.55 + 0.35 * frac;
	if (conf > 0.90) conf = 0.90;
	s.confidence = conf;
	s.mode = profile_override.empty() ? AiMode::Rescate : AiMode::Perfil;
	s.codec = "avc1?";
	s.use_step = true;
	s.use_dyn = true;
	std::ostringstream ss;
	ss << "ONNX (" << info_.nombre << ", precisión en validación "
	   << (int)(info_.precision_validacion * 100) << "%): encontró "
	   << inicios_video << " inicios de vídeo" << (inicios_sps ? " y " +
	   std::to_string(inicios_sps) + " SPS" : "") << " con confianza en "
	   << ventanas << " ventanas del mdat. El vídeo tiene estructura "
	   "recuperable aunque no haya referencia.";
	s.reason_es = ss.str();
	return s;
}
