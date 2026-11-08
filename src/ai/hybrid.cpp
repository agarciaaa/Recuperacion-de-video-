/*
	Untrunc-mejorado (Fase 2 hibrida) - hybrid.cpp
	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "hybrid.h"

#include <filesystem>
#include <sstream>

#include "../common.h"
#include "../diagnoser.h"
#include "../tempfs.h"
#include "../upath.h"
#include "external/external_provider.h"
#include "lmstudio/lmstudio_provider.h"
#include "local/local_provider.h"
#include "local/moov_synthesizer.h"
#include "onnx/onnx_provider.h"

HybridResult hybridAnalyze(const std::string& damaged_path,
                           const std::string& ref_path,
                           const AiConfig& cfg,
                           const std::string& profile_override,
                           const char* argv0) {
	HybridResult r;
	r.diagnosis = analyzeLocally(damaged_path, profile_override);

	// Paquete de diagnostico (solo texto). Se construye siempre,
	// se envie o no: es lo que muestra --solo-diagnostico.
	std::string code = "local";
	if (!ref_path.empty()) {
		DiagReport rep = preflightCheck(ref_path, damaged_path);
		code = diagCodeName(rep.code);
	}
	r.packet = buildDiagnosticPacket(r.diagnosis, code);

	LocalProvider local;
	std::string why;
	if (!local.isAvailable(why)) {
		r.suggestion.source = "local";
		r.suggestion.confidence = 0.0;
		r.suggestion.reason_es = "IA local no disponible: " + why;
		r.suggestion.mode = AiMode::Clasico;
	} else {
		r.suggestion = local.analyze(r.packet, r.diagnosis);
	}

	// Si hay referencia sana, el modo clasico manda: la IA solo
	// asesora (parametros, -s/-dyn). No se consulta a la externa
	// para un caso que el clasico ya resuelve.
	bool has_ref = !ref_path.empty() && r.diagnosis.has_mdat;
	if (has_ref) {
		r.suggestion.mode = AiMode::Clasico;
		r.suggestion.source = "local";
		r.summary_es = "IA local: hay referencia sana; se usa el modo clasico "
		               "(el mas fiable). La IA solo asesora: se activan -s y -dyn "
		               "si hace falta (reintento automatico de Fase 1).";
		return r;
	}

	// Sin referencia: si la confianza local es baja, consultar
	// primero al modelo ONNX entrenado en esta PC (si está activado
	// en la pestaña «Entrenar» y hay un modelo válido). ONNX
	// muestrea ventanas reales del mdat; si encuentra bastantes
	// inicios de vídeo con confianza, su sugerencia puede superar
	// a la local. Si no hay modelo o no se fía, se sigue como antes.
	if (r.suggestion.confidence < 0.75 && cfg.onnx_enabled) {
		OnnxProvider onnx(cfg, argv0);
		if (onnx.isAvailable(why)) {
			r.onnx_consulted = true;
			AiSuggestion onnx_s = onnx.analyzeFile(damaged_path, profile_override);
			if (onnx_s.confidence > r.suggestion.confidence) {
				onnx_s.source = "onnx";
				r.suggestion = onnx_s;
				r.onnx_used = true;
			} else {
				r.suggestion.warning_es +=
				    " El modelo ONNX fue consultado pero su confianza fue menor; se usa la local.";
			}
		} else {
			r.suggestion.warning_es += " ONNX no disponible: " + why +
			                           " Se sigue solo con la IA local.";
		}
	}

	// Sin referencia: si la confianza local es baja, consultar
	// primero a LM Studio (Qwen local, localhost, sin internet)
	// y despues a la IA externa, siempre SOLO con el paquete de
	// texto. Se elige la de mayor confianza en cada paso.
	if (r.suggestion.confidence < 0.75 && cfg.lmstudio_enabled) {
		LMStudioProvider lm(cfg);
		if (lm.isAvailable(why)) {
			r.lmstudio_consulted = true;
			AiSuggestion lm_s = lm.analyze(r.packet, r.diagnosis);
			if (lm_s.confidence > r.suggestion.confidence) {
				lm_s.source = "lmstudio";
				r.suggestion = lm_s;
				r.lmstudio_used = true;
			} else {
				r.suggestion.warning_es +=
				    " LM Studio (Qwen local) fue consultado pero su confianza fue menor; se usa la local.";
			}
		} else {
			r.suggestion.warning_es += " LM Studio no disponible: " + why +
			                           " Se sigue solo con la IA local.";
		}
	}

	// Sin referencia: si la confianza sigue baja y la externa
	// esta activada, consultarla SOLO con el paquete de texto.
	if (r.suggestion.confidence < 0.75 && cfg.external_enabled) {
		ExternalProvider ext(cfg);
		if (ext.isAvailable(why)) {
			r.external_consulted = true;
			AiSuggestion ext_s = ext.analyze(r.packet, r.diagnosis);
			if (ext_s.confidence > r.suggestion.confidence) {
				ext_s.source = "hibrida";
				r.suggestion = ext_s;
				r.external_used = true;
			} else {
				r.suggestion.source = "hibrida";
				r.suggestion.warning_es +=
				    " La IA externa fue consultada pero su confianza fue menor; se usa la local.";
			}
		} else {
			r.suggestion.warning_es += " IA externa no disponible: " + why +
			                           " Se sigue solo con la IA local.";
		}
	}

	std::ostringstream ss;
	ss << "Sugerencia IA (" << r.suggestion.source << ", modo " << aiModeNameEs(r.suggestion.mode)
	   << ", confianza " << (int)(r.suggestion.confidence * 100) << "%): "
	   << r.suggestion.reason_es;
	if (!r.suggestion.warning_es.empty()) ss << "\nAdvertencia: " << r.suggestion.warning_es;
	r.summary_es = ss.str();
	return r;
}

RescueOutcome rescueWithoutReference(const std::string& damaged_path,
                                     const AiConfig& cfg,
                                     const std::string& profile_override,
                                     const char* argv0) {
	return rescueWithoutReference(damaged_path, cfg, RepairOptions::fromGlobals(),
	                              profile_override, argv0);
}

RescueOutcome rescueWithoutReference(const std::string& damaged_path,
                                     const AiConfig& cfg,
                                     const RepairOptions& opts,
                                     const std::string& profile_override,
                                     const char* argv0) {
	RescueOutcome out;
	out.hybrid = hybridAnalyze(damaged_path, "", cfg, profile_override, argv0);
	const AiSuggestion& s = out.hybrid.suggestion;

	// Sin parametros detectados no se inventa nada.
	if (s.width <= 0 || s.height <= 0) {
		out.error_es = "La IA local no pudo detectar la resolucion del video danado "
		               "(no se encontro un SPS valido en el mdat; en MP4 el SPS vive en "
		               "el moov perdido). Sin resolucion no se puede construir una "
		               "referencia sintetica. Opciones: un video de referencia sano del "
		               "mismo dispositivo, o indicar el perfil con --perfil "
		               "(p. ej. --perfil generico_h264_1080p30).";
		// Se incluye el resumen hibrido para que el usuario vea que
		// IA se consulto (local y, si estaba activada, externa) y
		// con que confianza; si no, la consulta externa quedaba
		// invisible cuando el rescate fallaba.
		out.summary_es = out.error_es + "\n" + out.hybrid.summary_es;
		return out;
	}
	int fps = (s.fps > 0) ? s.fps : 30;
	std::string profile = s.profile_id;
	// "avc1?" significa: se vieron slices H.264 (NAL tipo 1/5) pero
	// sin SPS que lo confirmara. Para la referencia sintetica eso
	// basta: los slices H.264 implican H.264.
	std::string codec = (s.codec == "avc1?" ? "avc1" : s.codec);
	// El perfil de la referencia sintetica es el de video (High/Main).
	// Si el perfil detectado es de camara, se usa "high" por defecto.
	SyntheticRef synth = buildSyntheticReference(s.width, s.height, fps, codec, "high");
	(void)profile;
	if (!synth.ok) {
		out.error_es = synth.error_es;
		out.summary_es = synth.error_es + "\n" + out.hybrid.summary_es;
		return out;
	}
	out.synthetic_ref_used = true;
	out.synthetic_ref_path = synth.path;
	out.attempted = true;

	// La reparacion clasica con la referencia sintetica necesita
	// -s (los datos del danado no son exactamente los del video
	// sintetico). Se activa solo para este intento y se restaura.
	// Sobre una copia de las opciones de quien llama: las globales
	// las aplica y restaura la RepairSession de repairFileWithAutoRetry.
	RepairOptions rescue_opts = opts;
	rescue_opts.ignore_unknown = true;
	rescue_opts.use_chunk_stats = true;
	out.repair = repairFileWithAutoRetry(synth.path, damaged_path, rescue_opts, true);
	// La referencia sintetica ya no hace falta: se borra su carpeta
	// temporal unica (antes quedaba en %TEMP%/untrunc-ai-ref para siempre).
	tempfs::removeDir(upath::toPath(synth.path).parent_path());
	out.synthetic_ref_path.clear();
	out.ok = out.repair.ok;

	std::ostringstream ss;
	ss << "Rescate sin referencia (IA " << s.source << ", modo " << aiModeNameEs(s.mode)
	   << ", confianza " << (int)(s.confidence * 100) << "%).\n"
	   << "Referencia sintética: " << s.width << "x" << s.height << " " << fps
	   << " fps, " << codec << " (generada localmente con ffmpeg, 2 s).\n";
	if (out.ok) {
		ss << "Recuperado: " << (int)out.repair.recovered_percent << "% del vídeo"
		   << " | Duración: " << formatDurationEs(out.repair.duration_ms)
		   << " | Salida: " << out.repair.output_path << "\n"
		   << "El original no se ha modificado.";
		if (s.mode == AiMode::Rescate)
			ss << "\nNota: es un rescate parcial; revisa el vídeo reparado en VLC "
			      "de principio a fin antes de borrar el original.";
	} else {
		ss << "La reparación con referencia sintética no terminó: " << out.repair.error_es
		   << "\nPrueba con una referencia sana real del mismo dispositivo.";
	}
	out.summary_es = ss.str();
	return out;
}

RescueOutcome forcedRescueAttempt(const std::string& damaged_path,
                                  const AiConfig& cfg,
                                  const std::string& profile_override,
                                  const char* argv0) {
	return forcedRescueAttempt(damaged_path, cfg, RepairOptions::fromGlobals(),
	                           profile_override, argv0);
}

RescueOutcome forcedRescueAttempt(const std::string& damaged_path,
                                  const AiConfig& cfg,
                                  const RepairOptions& opts,
                                  const std::string& profile_override,
                                  const char* argv0) {
	RescueOutcome out = rescueWithoutReference(damaged_path, cfg, opts, profile_override, argv0);
	const std::string prefijo =
	    "Intento forzado (el archivo no parecía un MP4/MOV: sin ftyp/moov/mdat; "
	    "se intentó el rescate sin referencia con perfil/ONNX):\n";
	if (!out.ok) {
		// El rescate ya explica por qué no se pudo; solo se añade
		// el contexto de que fue un intento forzado, para que el
		// usuario no lo confunda con un fallo del modo normal.
		out.error_es = prefijo + out.error_es +
		               "\nNo se creó ningún archivo de salida: no se encontró "
		               "vídeo recuperable en este archivo.";
		out.summary_es = prefijo + out.summary_es;
		return out;
	}
	// La reparación "terminó": comprobar que hay algo real dentro.
	// Un 0% recuperado o una salida de 0 bytes es el MP4 vacío del
	// Untrunc original (válido por fuera, sin imagen ni sonido):
	// no se deja como si fuera una reparación.
	bool vacio = out.repair.recovered_percent < 1.0 ||
	             out.repair.output_bytes == 0;
	if (vacio) {
		std::error_code ec;
		if (!out.repair.output_path.empty())
			std::filesystem::remove(upath::toPath(out.repair.output_path), ec);
		out.ok = false;
		out.attempted = true;
		out.error_es = prefijo +
		               "El rescate terminó pero no se encontró vídeo recuperable "
		               "(0% recuperado): el archivo no contiene datos de vídeo "
		               "H.264/H.265 que se puedan reconstruir. No se dejó ningún "
		               "archivo vacío: el original no se ha modificado.";
		out.summary_es = out.error_es;
		out.repair = RepairOutcome();
		return out;
	}
	out.summary_es = prefijo + out.summary_es;
	return out;
}
