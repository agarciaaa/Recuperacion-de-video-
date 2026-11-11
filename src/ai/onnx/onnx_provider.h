/*
	Untrunc-mejorado (Fase 2.5, prototipo ONNX) - onnx_provider.h

	Motor ONNX PEQUEÑO y ligero, entrenado EN LA PC del usuario
	con sus propios vídeos (pestaña "Entrenar" de la GUI, o el
	script src/ai/onnx/train_onnx.py).

	Arquitectura del modelo base (ver train_onnx.py):
	  MLP 264 -> 64 -> 32 -> 4 (ReLU, Softmax), 19.172 parámetros.
	  Entradas: ventana de 256 bytes del mdat + 8 características
	  hechas a mano (= 264). Clases: basura / inicio_video /
	  inicio_audio / inicio_sps. El .onnx base pesa ~77 KB.

	Honestidad sobre ONNX Runtime en este prototipo:
	  - El script de entrenamiento exporta un .onnx REAL (grafo
	    Gemm/Relu/Softmax, opset 13, ir_version 8), verificado con
	    onnx.checker y ejecutado con onnxruntime 1.31.1 en Python.
	  - El proveedor C++ detecta ONNX Runtime por dlopen
	    (libonnxruntime.so.1 / onnxruntime.dll) y lo registra en
	    model_info, pero la inferencia que SE EJECUTA hoy en C++
	    es la del MLP nativo leyendo pesos.json (los MISMOS pesos
	    que el .onnx). Motivo: cablear la API C de ORT (sesión,
	    tensores) sin sus cabeceras instaladas no se puede verificar
	    aquí; hacerlo a ciegas sería fingir. El .onnx queda como
	    artefacto portable y el cableado ORT es el siguiente paso.
	  - Sin modelo, sin pesos.json o sin Python, TODO sigue
	    funcionando como antes: el proveedor dice que no está
	    disponible y el orquestador degrada a heurísticas/Qwen.

	El proveedor NUNCA recibe el vídeo entero: classifyWindow()
	trabaja sobre una ventana de 256 bytes del mdat, y analyzeFile()
	muestrea ventanas del archivo dañado solo para contar inicios
	de frame. No se sube nada, no hay red.

	Licencia: GNU GPL-2.0 (ver COPYING en la raíz).
*/

#ifndef AI_ONNX_PROVIDER_H
#define AI_ONNX_PROVIDER_H

#include <cstdint>
#include <string>
#include <vector>

#include "../ai_provider.h"

// Clases del modelo (mismo orden que en train_onnx.py).
enum class OnnxClase {
	Basura = 0,
	InicioVideo = 1,
	InicioAudio = 2,
	InicioSps = 3,
};

struct OnnxWindowResult {
	int clase = 0;                 // 0..3 (ver OnnxClase)
	double confianza = 0.0;        // probabilidad softmax de la clase ganadora
	double probs[4] = {0, 0, 0, 0};
};

struct OnnxModelInfo {
	bool cargado = false;
	std::string ruta;              // carpeta del modelo (modelo_activo o base)
	std::string nombre = "sin modelo";
	std::string arquitectura;
	size_t tamano_bytes = 0;       // tamaño del .onnx (0 si no se exportó)
	int ejemplos = 0;
	double precision_validacion = 0.0;
	double precision_baseline = 0.0;
	std::string fecha;
	bool onnx_exportado = false;
	bool ort_detectado = false;    // ONNX Runtime presente (dlopen)
	std::string error_es;
};

// Rutas y script de entrenamiento (también usados por la GUI y el CLI).
std::string onnxTrainingDir();                 // ~/.config/untrunc/entrenamiento
std::string onnxModelActivoDir();              // .../entrenamiento/modelo_activo
std::string onnxDatasetDir();                  // .../entrenamiento/dataset
// Busca train_onnx.py: env UNTRUNC_TRAIN_SCRIPT, luego junto al
// ejecutable (tools/train_onnx.py), luego src/ai/onnx/train_onnx.py.
std::string findTrainScript(const char* argv0 = nullptr);
// Busca el modelo base incluido: models/onnx_base junto al exe,
// luego src/ai/onnx/modelo_base (árbol de desarrollo).
std::string findBaseModelDir(const char* argv0 = nullptr);
// Ruta del ejecutable guardada al arrancar (main CLI y Gui::main
// la fijan con setExeArgv0(argv[0])). Es el respaldo cuando una
// llamada llega sin argv0: sin ella, findBaseModelDir() solo veía
// el candidato relativo src/ai/onnx/modelo_base y desde un ZIP
// extraído el modelo base no se encontraba (Fase 3, §6).
extern const char* g_exe_argv0;
void setExeArgv0(const char* argv0);
// Ejecuta el script de entrenamiento (python3/python + argumentos).
// Devuelve el código de salida del script (0 = ok). Si no hay
// Python o no se encuentra el script, devuelve -1 y explica en
// español por qué (why_es), sin fallar en silencio.
// Reparaciones (2026-11-07): la salida del script (stdout+stderr)
// se captura en un archivo temporal y se guarda: antes se usaba
// std::system() y se descartaba, así que la GUI decía "revisa el
// mensaje en la ventana de detalles" pero ahí no había nada y
// encontrar la causa real (p. ej. PyTorch roto) costaba horas.
// lastTrainScriptOutput() devuelve esa salida (recortada); en caso
// de error, why_es ya incluye sus últimas líneas.
int runTrainScript(const std::vector<std::string>& args, std::string& why_es,
                   const char* argv0 = nullptr);
// Salida completa (recortada a 64 KB) de la última ejecución de
// runTrainScript, para mostrarla en la ventana de detalles de la
// pestaña Entrenar. Solo contiene lo que imprime el script (nombres
// base de vídeos, conteos, errores): nunca vídeos ni rutas completas.
const std::string& lastTrainScriptOutput();
// Lee model_info.json + pesos.json de una carpeta y dice si el
// modelo es válido. No lanza excepciones.
bool readOnnxModelInfo(const std::string& dir, OnnxModelInfo& info);

// Copia de seguridad del modelo activo anterior (la deja el script
// al aceptar un modelo nuevo): .../entrenamiento/modelo_activo_anterior
std::string onnxModelAnteriorDir();

// true si la carpeta tiene un modelo aceptado Y sus pesos (pesos.json)
// se pueden cargar de verdad. why_es explica el fallo en espanol.
bool onnxModelUsable(const std::string& dir, std::string& why_es);

// Modelo que se usara realmente, en orden: ruta configurada, modelo
// activo, copia de seguridad, modelo base incluido. Se salta los que
// esten incompletos (p. ej. sin pesos.json) y lo explica en aviso_es.
// Devuelve la carpeta ("" si no hay ninguno) y rellena info.
std::string onnxModeloEnUso(const std::string& ruta_configurada, const char* argv0,
                            OnnxModelInfo& info, std::string& aviso_es);

// Conecta el modelo ONNX en uso al nucleo de reparacion
// (g_resync_scorer, ver Mp4::tryResync): tras una zona danada, el
// nucleo solo vuelve a aceptar fotogramas que el modelo reconoce como
// inicio. Se carga una vez y se reutiliza mientras la ruta del modelo
// no cambie. Devuelve false (y why_es) si no hay modelo usable; en ese
// caso el nucleo trabaja como siempre.
bool activarResincronizacionOnnx(const AiConfig& cfg, std::string& modelo_es,
                                 std::string& why_es);
void desactivarResincronizacionOnnx();

class OnnxProvider : public IAiProvider {
public:
	explicit OnnxProvider(const AiConfig& cfg, const char* argv0 = nullptr);
	std::string name() const override { return "onnx"; }
	bool isAvailable(std::string& why_not_es) const override;
	// La interfaz común no tiene el archivo: sin él no se puede
	// muestrear ventanas, así que devuelve confianza 0 y explica
	// que hay que usar analyzeFile(). El orquestador llama a
	// analyzeFile() cuando ONNX está activado.
	AiSuggestion analyze(const DiagnosticPacket& packet,
	                     const AiDiagnosis& diagnosis) override;
	// Análisis real: muestrea ventanas del mdat del archivo dañado,
	// las clasifica y, si hay bastantes inicios de vídeo con alta
	// confianza, devuelve una sugerencia (modo Perfil/Rescate) con
	// confianza medida, no inventada.
	AiSuggestion analyzeFile(const std::string& path,
	                         const std::string& profile_override = "");
	// Clasifica UNA ventana de 256 bytes. Es la pieza que en el
	// futuro usará el escáner de frames (frame_scanner) en vez de
	// solo las reglas, cuando el modelo esté validado.
	OnnxWindowResult classifyWindow(const unsigned char* data, size_t len) const;
	const OnnxModelInfo& modelInfo() const { return info_; }
	// Modelos saltados al cargar (incompletos o danados), o "".
	const std::string& avisoCarga() const { return aviso_carga_; }

	// Extracción de características: IDÉNTICA a train_onnx.py
	// (extraer_features). Si se cambia aquí, hay que cambiarla allí.
	static void extractFeatures(const unsigned char* data, size_t len, float* out264);

private:
	AiConfig cfg_;
	OnnxModelInfo info_;
	bool weights_loaded_ = false;
	std::string aviso_carga_;
	// Pesos del MLP (mismos que el .onnx): W1[264*64], b1[64],
	// W2[64*32], b2[32], W3[32*4], b3[4].
	std::vector<float> w1_, b1_, w2_, b2_, w3_, b3_;
	bool loadWeights(const std::string& dir);
};

#endif // AI_ONNX_PROVIDER_H
