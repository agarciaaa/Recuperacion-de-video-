/*
	Untrunc-mejorado (Fase 2 hibrida) - ai_provider.h

	Interfaz comun de los DOS modulos de conexion a IA:
	  A) IA local  (src/ai/local/)   - offline, el video nunca sale de la PC.
	  B) IA externa (src/ai/external/) - por API key, opt-in, desactivada
	      por defecto, SOLO texto de diagnostico, NUNCA el video.

	Regla de privacidad por diseno (se aplica en TODOS los proveedores):
	  - Un proveedor de IA solo recibe un AiDiagnosis (estructura) o un
	    DiagnosticPacket (texto JSON de pocos KB). Jamas recibe bytes
	    del mdat, fotogramas, audio, ni rutas reales de archivo.
	  - El paquete de diagnostico usa nombres genericos
	    ("video_danado.mp4", "referencia.mp4"), nunca la ruta real,
	    que puede contener el nombre del usuario.
	  - La IA no genera video: solo infiere ESTRUCTURA (codec, SPS,
	    limites de frame, perfil de camara) y explica en espanol.
	    No puede inventar fotogramas borrados ni reparar un 0 bytes.

	Licencia: GNU GPL-2.0 (ver COPYING en la raiz).
	Basado en anthwlock/untrunc (fork de ponchio/untrunc).
*/

#ifndef AI_PROVIDER_H
#define AI_PROVIDER_H

#include <cstdint>
#include <string>
#include <vector>

// Limite duro de privacidad: la IA externa nunca recibe mas de esto.
// Un paquete de diagnostico real tiene unos 1-3 KB; 50 KB es ya
// un margen amplio y permite rechazar cualquier intento de enviar
// datos de video por accidente.
static const size_t kMaxDiagnosticPacketBytes = 50 * 1024;

// ---------------------------------------------------------------------------
// Configuracion (se lee de variables de entorno y de un archivo local;
// nunca de argumentos en la linea de comandos, para que la clave no
// quede en el historial ni en `ps`).
//
//   UNTRUNC_AI_API_KEY    clave de la IA externa (opcional)
//   UNTRUNC_AI_ENDPOINT   URL del servicio (opcional)
//   UNTRUNC_AI_MODEL      nombre del modelo (opcional)
//   UNTRUNC_AI_TIMEOUT    segundos de espera, por defecto 20
//
//   LM Studio (Qwen local, 100% en localhost, sin clave externa):
//   UNTRUNC_LMSTUDIO_ENDPOINT   por defecto http://localhost:1234
//   UNTRUNC_LMSTUDIO_MODEL      identificador del modelo cargado en
//                               LM Studio (p. ej. "qwen2.5-7b-instruct"),
//                               verlo con --lmstudio-listar
//   UNTRUNC_LMSTUDIO_API_KEY    casi nunca necesaria en localhost;
//                               solo si LM Studio la pide
//   UNTRUNC_LMSTUDIO_TIMEOUT    segundos, por defecto 45 (los LLM
//                               locales son mas lentos que una API)
//
// Archivo opcional: ~/.config/untrunc/ai.conf  (lineas CLAVE=valor)
// La clave solo se guarda ahi si el usuario lo decide; el archivo
// debe tener permisos 0600 (el programa avisa si no los tiene).
// ---------------------------------------------------------------------------
struct AiConfig {
	bool local_enabled = true;    // IA local: siempre disponible, sin red
	std::string local_profile;    // perfil por defecto ("" = automático)
	bool external_enabled = false; // IA externa: APAGADA por defecto
	std::string endpoint;         // "" = sin configurar (placeholder invalido)
	std::string model = "untrunc-ai-texto";
	std::string api_key;          // nunca se escribe en logs
	int timeout_seconds = 20;
	int max_retries = 1;          // reintentos de red (total: 2 intentos)
	std::string config_file;      // ruta del ai.conf leido, o ""
	// LM Studio (Qwen local). Apagado por defecto: solo se usa con
	// --ia-lmstudio o activandolo en Ajustes > IA.
	bool lmstudio_enabled = false;
	std::string lmstudio_endpoint = "http://localhost:1234";
	std::string lmstudio_model;   // "" = autodetectar (primer qwen cargado)
	std::string lmstudio_api_key; // casi nunca necesaria en localhost
	int lmstudio_timeout_seconds = 45;
	// ONNX pequeño (Fase 2.5, pestaña "Entrenar"). Apagado por
	// defecto: solo se usa si hay un modelo válido (entrenado por
	// el usuario o el modelo base incluido) y el usuario lo activa.
	bool onnx_enabled = false;
	std::string onnx_model_path;  // "" = buscar modelo_activo, luego base
	double onnx_threshold = 0.75; // confianza mínima para fiarse de una ventana
};

// Globales de la GUI (pestaña IA). La GUI los lee/escribe; el CLI
// usa AiConfig. Por defecto: local SI, externa NO, LM Studio NO.
extern bool g_ai_local_enabled;
extern bool g_ai_external_enabled;
extern std::string g_ai_endpoint;
extern std::string g_ai_api_key; // solo en memoria, nunca en logs
extern std::string g_ai_model;   // modelo de la IA externa ("" = el de AiConfig)
extern int g_ai_timeout_seconds; // segundos de espera, IA externa
extern bool g_ai_lmstudio_enabled;
extern std::string g_lmstudio_endpoint;
extern std::string g_lmstudio_model;
extern int g_lmstudio_timeout_seconds; // segundos, LM Studio (LLM local, lento)
// Perfil por defecto de la IA local ("" = automático: lo detecta la
// local o lo pide el usuario con --perfil). Valores: los "id" de
// profiles/profiles.json, p. ej. "generico_h264_1080p30".
extern std::string g_ai_local_profile;
// Modelo ONNX pequeño (Fase 2.5, pestaña "Entrenar"): apagado por
// defecto hasta que haya un modelo válido y el usuario lo active.
extern bool g_onnx_enabled;
extern std::string g_onnx_model_path; // "" = modelo_activo, luego base
extern double g_onnx_threshold;       // 0.75 por defecto

AiConfig loadAiConfig();
// Rellena endpoint/modelo por defecto si no hay nada configurado.
// El endpoint por defecto es INTENCIONADAMENTE invalido
// (https://api.example.invalid/...) para que nunca se haga una
// llamada real por accidente: el usuario debe poner uno real.
std::string defaultEndpoint();

// ---------------------------------------------------------------------------
// Diagnostico: lo unico que la IA puede "ver" de un video.
// Todo son numeros y textos cortos. No hay bytes de video.
// ---------------------------------------------------------------------------
struct AiDiagnosis {
	bool file_exists = false;
	uint64_t file_size = 0;
	std::string ftyp;            // marca, p. ej. "isom"
	bool has_moov = false;
	bool has_mdat = false;
	uint64_t mdat_bytes = 0;
	// SPS/PPS detectados en el mdat en bruto (hex, max ~64 bytes c/u).
	std::string codec = "desconocido"; // "avc1" (H.264), "hvc1" (H.265), ...
	int width = 0;               // 0 = no detectado
	int height = 0;
	int fps_estimado = 0;        // 0 = no detectado
	bool dimensions_from_profile = false; // true si ancho/alto/fps vienen
	                                       // de un perfil elegido por el
	                                       // usuario (--perfil), no de un SPS
	std::string sps_hex;         // vacio si no se encontro SPS
	std::string pps_hex;
	bool sps_valido = false;
	int nal_sps = 0;             // nº de NAL SPS vistos
	int nal_pps = 0;
	int nal_idr = 0;             // frames clave (IDR)
	int nal_slice = 0;           // slices de video
	int nal_aud = 0;             // delimitadores de unidad de acceso
	int estimated_frames = 0;    // estimacion (ver frame_scanner)
	std::string frame_estimate_note_es;
	// Histograma de tamanos de slice: 5 cubos, solo conteos.
	// [ <1KB, 1-4KB, 4-16KB, 16-64KB, >64KB ]
	int hist_slice_kb[5] = {0, 0, 0, 0, 0};
	std::string profile_id;      // perfil de camara que mas coincide
	double profile_confidence = 0.0;
	std::string markers;         // "gpmd", "XAVC", "CAEP", ... o ""
};

enum class AiMode {
	Clasico,  // con referencia sana (lo de siempre, lo mas fiable)
	Perfil,   // sin referencia, con perfil de camara detectado
	Rescate   // sin referencia ni perfil fiable: rescate parcial honesto
};

struct AiSuggestion {
	AiMode mode = AiMode::Clasico;
	std::string source;          // "local", "externa" o "hibrida"
	double confidence = 0.0;     // 0..1
	std::string profile_id;
	std::string codec;
	int width = 0;
	int height = 0;
	int fps = 0;
	bool use_step = false;       // usar -s
	bool use_dyn = false;        // usar -dyn
	std::string reason_es;       // por que se recomienda esto, en espanol
	std::string warning_es;      // advertencia, si la hay
};

std::string aiModeName(AiMode m);
std::string aiModeNameEs(AiMode m);

// ---------------------------------------------------------------------------
// Paquete de diagnostico: el UNICO texto que puede salir de la PC hacia
// una IA externa. Se construye desde AiDiagnosis + el codigo de
// pre-vuelo de Fase 1. No contiene rutas reales ni bytes de video.
// ---------------------------------------------------------------------------
struct DiagnosticPacket {
	std::string json;            // texto JSON, solo metadatos
	size_t bytes = 0;            // json.size()
	bool valid = false;
	std::string error_es;        // si no es valido, por que
};

DiagnosticPacket buildDiagnosticPacket(const AiDiagnosis& d,
                                       const std::string& diag_code_name);

// Valida el paquete antes de enviarlo: tamano <= 50 KB, solo texto
// imprimible (sin bytes binarios), sin campos de video.
// Devuelve false y un motivo en espanol si algo no cumple.
bool validateDiagnosticPacket(const DiagnosticPacket& p, std::string& why_es);

// Utilidades (tambien usadas por los tests).
std::string jsonEscape(const std::string& s);
std::string toHexLower(const unsigned char* data, size_t len);

// Extrae un numero / string simple de una respuesta JSON sin
// libreria externa. Devuelve false si no se encuentra.
bool jsonFindNumber(const std::string& json, const std::string& key, double& out);
bool jsonFindString(const std::string& json, const std::string& key, std::string& out);

// ---------------------------------------------------------------------------
// Interfaz abstracta: TODO proveedor de IA la implementa.
// analyze() recibe SOLO el paquete de diagnostico (texto) y
// devuelve una sugerencia. Ningun proveedor recibe jamas la ruta
// real del video ni sus bytes.
// ---------------------------------------------------------------------------
class IAiProvider {
public:
	virtual ~IAiProvider() = default;
	virtual std::string name() const = 0;      // "local" / "externa"
	virtual bool isAvailable(std::string& why_not_es) const = 0;
	// Si no esta disponible, devuelve confidence 0 y warning_es.
	virtual AiSuggestion analyze(const DiagnosticPacket& packet,
	                             const AiDiagnosis& diagnosis) = 0;
};

#endif // AI_PROVIDER_H
