/*
	Untrunc-mejorado (Fase 2 hibrida) - hybrid.h

	Orquestador hibrido: decide que sugerencia usar y ejecuta el
	rescate sin referencia.

	Orden de decision (siempre):
	  1. IA LOCAL primero (offline, sin red, sin clave).
	  2. Si la confianza local es baja (< 0.75) Y la IA externa
	     esta activada por el usuario (opt-in), se consulta a la
	     externa SOLO con el paquete de diagnostico en texto.
	  3. Se elige la sugerencia de mayor confianza.
	  4. Si ambas fallan, se cae al modo clasico con referencia
	     (o se dice claro que hace falta una referencia sana).

	La IA externa nunca recibe el video: solo el DiagnosticPacket
	(validado, <= 50 KB). Si la externa esta desactivada, el paso 2
	no existe: no hay ninguna llamada de red.

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef AI_HYBRID_H
#define AI_HYBRID_H

#include <string>

#include "ai_provider.h"
#include "../repair_job.h"

struct HybridResult {
	AiSuggestion suggestion;   // la elegida
	AiDiagnosis diagnosis;     // diagnostico local (base de todo)
	DiagnosticPacket packet;   // paquete que se habria enviado
	bool external_consulted = false;
	bool external_used = false;
	bool lmstudio_consulted = false;
	bool lmstudio_used = false;
	bool onnx_consulted = false;
	bool onnx_used = false;
	std::string summary_es;    // resumen en espanol para el usuario
};

struct RescueOutcome {
	HybridResult hybrid;
	bool attempted = false;        // se intento reparar
	bool ok = false;               // la reparacion clasica termino
	bool synthetic_ref_used = false;
	std::string synthetic_ref_path;  // temporal; borrar al terminar
	RepairOutcome repair;          // resultado de Fase 1 (si se intento)
	std::string error_es;          // si no se pudo intentar
	std::string summary_es;
};

// Analiza un video danado con el orquestador hibrido.
// ref_path vacio = sin referencia (se intentara perfil/rescate).
HybridResult hybridAnalyze(const std::string& damaged_path,
                           const std::string& ref_path,
                           const AiConfig& cfg,
                           const std::string& profile_override = "",
                           const char* argv0 = nullptr);

// Rescate sin referencia: IA local -> referencia sintetica con
// los parametros detectados -> reparacion clasica con esa
// referencia. Devuelve el resultado real (duracion, % recuperado).
// Si la IA no detecta parametros, NO se inventa nada: se devuelve
// error_es explicando que hace falta una referencia sana.
RescueOutcome rescueWithoutReference(const std::string& damaged_path,
                                     const AiConfig& cfg,
                                     const std::string& profile_override = "",
                                     const char* argv0 = nullptr);
// Igual, con una foto de opciones explicita (GUI): ver repair_options.h.
RescueOutcome rescueWithoutReference(const std::string& damaged_path,
                                     const AiConfig& cfg,
                                     const RepairOptions& opts,
                                     const std::string& profile_override,
                                     const char* argv0);

// Intento forzado ("Intentar de todos modos", reparaciones
// 2026-11-07, punto 4): para archivos que el preflight rechaza
// con BadNoFtyp/BadNoMdat (sin ftyp/moov/mdat, como el caso real
// f18931679.mp4, 37 MB de Recoverit). El preflight NO se elimina:
// esta vía solo se usa cuando el usuario lo pide explícitamente
// (--forzar en CLI o la casilla del Asistente), sabiendo que
// probablemente no haya nada que recuperar.
// Lanza el rescate sin referencia con el perfil elegido; si el
// rescate tampoco encuentra vídeo recuperable, lo dice con
// honestidad y NO deja un archivo vacío engañoso: si la
// reparación "terminó" con 0% recuperado o 0 bytes de salida, se
// borra esa salida y el resultado es un fallo claro.
RescueOutcome forcedRescueAttempt(const std::string& damaged_path,
                                  const AiConfig& cfg,
                                  const std::string& profile_override = "",
                                  const char* argv0 = nullptr);
RescueOutcome forcedRescueAttempt(const std::string& damaged_path,
                                  const AiConfig& cfg,
                                  const RepairOptions& opts,
                                  const std::string& profile_override,
                                  const char* argv0);

#endif // AI_HYBRID_H
