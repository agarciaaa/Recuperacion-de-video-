/*
	Untrunc-mejorado (Fase 1) - repair_job.cpp

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "repair_job.h"

#include <cstdlib>
#include <filesystem>
#include <sstream>

#include "common.h"
#include "file.h"
#include "memoria.h"
#include "ai/ai_provider.h"
#include "ai/onnx/onnx_provider.h"
#include "resinc_ia.h"
#include "mp4.h"
#include "upath.h"

namespace fs = std::filesystem;

std::string formatDurationEs(int64_t ms) {
	if (ms <= 0) return "desconocida";
	int64_t total_s = ms / 1000;
	int64_t h = total_s / 3600;
	int64_t m = (total_s % 3600) / 60;
	int64_t s = total_s % 60;
	std::ostringstream o;
	if (h > 0) o << h << " h ";
	if (m > 0 || h > 0) o << m << " min ";
	o << s << " s";
	return o.str();
}

double honestRecoveredPercent(bool premature_end, double premature_percentage,
                              uint64_t output_bytes, uint64_t corrupt_bytes,
                              int64_t duration_ms) {
	if (duration_ms < kMinUsefulRecoveredMs || output_bytes == 0) return 0.0;
	double pct = premature_end ? premature_percentage : 100.0;
	if (corrupt_bytes > 0) {
		double kept = 100.0 * (double)output_bytes / (double)corrupt_bytes;
		if (kept < pct) pct = kept;
	}
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	return pct;
}

bool attemptIsBetter(int64_t duration_b, double pct_b,
                     int64_t duration_a, double pct_a) {
	if (duration_b != duration_a) return duration_b > duration_a;
	return pct_b > pct_a;
}

namespace {

// Un intento de reparacion con una configuracion dada.
// Devuelve el resultado sin lanzar excepciones (las captura todas).
struct Attempt {
	bool threw = false;
	std::string error;
	bool premature = false;
	double pct = 0.0;
	std::string output;
	int64_t duration_ms = 0;
	uint64_t out_bytes = 0;
	int resincs = 0;
	int resincs_ia = 0;
	int sincros = 0;
	double sincro_ms = 0;
	double desconocido = 0;  // % del mdat sin reconocer
};

// Si ONNX esta activado y hay un modelo de resincronizacion validado
// ("activo 1", ver resinc_ia.h), mientras dura la reparacion decide los
// candidatos tras una zona danada donde la cadena no decide; al salir se
// desconecta siempre.
struct ResincGuard {
	bool activo = false;
	explicit ResincGuard(bool pedir) {
		if (!pedir) return;
		AiConfig cfg = loadAiConfig();
		if (!(cfg.onnx_enabled || g_onnx_enabled)) return;
		const std::string ruta = rutaModeloResincEnUso();
		std::string why;
		if (!ruta.empty()) activo = activarModeloResinc(ruta, true, why);
	}
	~ResincGuard() {
		if (activo) desactivarModeloResinc();
	}
};

// Un intento que llega al final pero deja mas de esto sin reconocer
// no detiene la busqueda de estrategias.
constexpr double kMaxDesconocidoParaParar = 5.0;

Attempt runAttempt(const std::string& ref, const std::string& corrupt) {
	Attempt a;
	try {
		Mp4 mp4;
		g_mp4 = &mp4;
		mp4.parseOk(ref);
		mp4.repair(corrupt);
		a.premature = mp4.premature_end_;
		a.resincs = g_resync_count;
		a.resincs_ia = g_resync_ia_count;
		a.sincros = g_sincro_count;
		a.sincro_ms = g_sincro_ms;
		a.desconocido = mp4.unknown_percentage_;
		a.output = mp4.getPathRepaired(ref, corrupt);
		a.duration_ms = mp4.getDurationInMs();
		std::error_code ec;
		const fs::path out_path = upath::toPath(a.output);
		if (fs::exists(out_path, ec) && !ec)
			a.out_bytes = fs::file_size(out_path, ec);
		uint64_t corrupt_bytes = fs::file_size(upath::toPath(corrupt), ec);
		if (ec) corrupt_bytes = 0;
		a.pct = honestRecoveredPercent(mp4.premature_end_, mp4.premature_percentage_,
		                               a.out_bytes, corrupt_bytes, a.duration_ms);
	} catch (const DiagnoserError& e) {
		a.threw = true;
		a.pct = 0;
		a.error = e.what();
	} catch (const char* e) {
		a.threw = true;
		a.error = e ? e : "error desconocido";
	} catch (const std::string& e) {
		a.threw = true;
		a.error = e;
	} catch (const std::exception& e) {
		a.threw = true;
		a.error = e.what();
	}
	g_mp4 = nullptr;
	return a;
}

std::string describeMode(bool with_s, bool with_dyn) {
	if (with_s && with_dyn) return "con -s -dyn (omitir desconocidas + estadísticas dinámicas)";
	if (with_s) return "con -s (omitir secuencias desconocidas)";
	if (with_dyn) return "con -dyn (estadísticas dinámicas)";
	return "normal";
}

} // namespace

RepairOutcome repairFileWithAutoRetry(const std::string& ref_path,
                                      const std::string& corrupt_path,
                                      bool allow_retry) {
	return repairFileWithAutoRetry(ref_path, corrupt_path, RepairOptions::fromGlobals(),
	                               allow_retry);
}

RepairOutcome repairFileWithAutoRetry(const std::string& ref_path,
                                      const std::string& corrupt_path,
                                      const RepairOptions& opts,
                                      bool allow_retry) {
	RepairOutcome out;

	// 1.1: pre-vuelo antes de tocar el nucleo.
	DiagReport report = preflightCheck(ref_path, corrupt_path);
	out.code = report.code;
	if (report.code != DiagCode::Ok) {
		out.ok = false;
		out.error_es = diagMessageEs(report);
		out.message_es = out.error_es;
		return out;
	}

	// 1.4: salida a Reparados/, salvo -dst explicito del usuario.
	// Se trabaja sobre una COPIA de las opciones: nada de esto toca
	// las de quien llama ni las de otra operacion.
	RepairOptions run = opts;
	if (run.dst_path.empty()) {
		std::string reparados_dir = ensureReparadosDir(corrupt_path);
		if (!reparados_dir.empty()) run.dst_path = reparados_dir;
	}
	// Desde aqui el nucleo trabaja con `run`; al salir (por cualquier
	// return o excepcion) se restauran las globales.
	RepairSession session(run);
	ResincGuard resinc(run.resinc);

	// Si ya existe la salida y esta -skip, no se repara: se anota y
	// se sigue (en lote no se puede hacer exit(0) como hacia repair()).
	// La ruta esperada se calcula con Mp4::getPathRepaired() para
	// respetar el nombre real de Untrunc ("<nombre>.mp4_fixed.mp4",
	// con sufijo -s/-dyn si el usuario dio esas opciones).
	if (run.skip_existing) {
		Mp4 probe;
		std::string expected = probe.getPathRepaired(ref_path, corrupt_path);
		if (FileRead::alreadyExists(expected)) {
			out.ok = false;
			out.skipped = true;
			out.error_es = "Ya existe el archivo reparado y está activada la opción de omitir "
			               "existentes (-skip): " + expected + "\n"
			               "No se ha tocado el original.";
			out.message_es = out.error_es;
			return out;
		}
	}

	const bool user_s = run.ignore_unknown;
	const bool user_dyn = run.use_chunk_stats;

	// Busqueda de estrategias (antes: un intento + como mucho un
	// reintento con -s -dyn). Orden:
	//   1) lo que pidio el usuario (-s / -dyn), si pidio algo;
	//   2) lo que mejor funciono antes con esta camara (memoria.h);
	//   3) la sugerencia de la IA, si la hay;
	//   4) normal, -s -dyn, -dyn, -s.
	// Se para en cuanto un intento recupera el video completo (>= 99%).
	struct Estrategia {
		bool s, dyn;
		std::string por_que;
	};
	std::vector<Estrategia> plan;
	auto anadir = [&plan](bool s, bool dyn, const std::string& por_que) {
		for (const auto& e : plan)
			if (e.s == s && e.dyn == dyn) return;
		plan.push_back({s, dyn, por_que});
	};
	out.firma_camara = memoria::firmaReferencia(ref_path);
	const memoria::Recuerdo recuerdo = memoria::consultar(out.firma_camara);
	if (user_s || user_dyn) anadir(user_s, user_dyn, "tus opciones");
	if (recuerdo.hay)
		anadir(recuerdo.s, recuerdo.dyn,
		       "fue la mejor con esta cámara " + std::to_string(recuerdo.victorias) +
		           (recuerdo.victorias == 1 ? " vez" : " veces"));
	if (run.hint_set)
		anadir(run.hint_s, run.hint_dyn,
		       "sugerida por la IA" + (run.hint_fuente.empty() ? "" : " (" + run.hint_fuente + ")"));
	anadir(false, false, "orden por defecto");
	anadir(true, true, "orden por defecto");
	anadir(false, true, "orden por defecto");
	anadir(true, false, "orden por defecto");
	// -rsv-ben no es compatible con -s ni -dyn (lo rechaza el CLI).
	if (run.rsv_ben_mode) {
		plan.clear();
		anadir(false, false, "único modo compatible con -rsv-ben");
	}
	if (!allow_retry) plan.resize(1);
	out.desde_memoria = recuerdo.hay && !(user_s || user_dyn);

	auto ejecutar = [&](const Estrategia& e) {
		RepairOptions o = run;
		o.ignore_unknown = e.s;
		o.use_chunk_stats = e.dyn;
		RepairSession sub(o);  // sesion anidada: restaura al salir
		return runAttempt(ref_path, corrupt_path);
	};

	Attempt first = ejecutar(plan[0]);
	if (first.threw) {
		out.ok = false;
		out.error_es = "No se pudo reparar " + corrupt_path + ".\n"
		               "Error: " + first.error + "\n"
		               "El archivo original no se ha modificado.";
		out.message_es = out.error_es;
		return out;
	}

	out.ok = true;
	out.recovered_percent = first.pct;
	out.first_percent = first.pct;
	out.output_path = first.output;
	out.first_output_path = first.output;
	out.duration_ms = first.duration_ms;
	out.output_bytes = first.out_bytes;
	out.mode_es = describeMode(plan[0].s, plan[0].dyn);

	// Resto de estrategias, solo mientras el mejor intento no haya
	// recuperado el video completo.
	auto lineaIntento = [](const Estrategia& e, const Attempt& a) {
		std::ostringstream l;
		l << describeMode(e.s, e.dyn) << " (" << e.por_que << "): ";
		if (a.threw) l << "falló (" << a.error << ")";
		else if (a.duration_ms < kMinUsefulRecoveredMs) l << "nada útil";
		else l << formatDurationEs(a.duration_ms) << " (" << (int)a.pct << "%)";
		return l.str();
	};
	std::vector<std::pair<size_t, Attempt>> hechos = {{0, first}};
	out.intentos_es.push_back(lineaIntento(plan[0], first));
	size_t mejor = 0;
	Attempt best = first;
	for (size_t i = 1; i < plan.size(); i++) {
		// Parar si ya esta completo, o si el mejor intento leyo el
		// archivo hasta el final Y reconocio casi todo. Antes bastaba con
		// no cortarse: con -s nunca se corta, y un intento que dejaba el
		// 70 % sin reconocer paraba la busqueda.
		if (best.duration_ms >= kMinUsefulRecoveredMs &&
		    (best.pct >= 99.0 || (!best.premature && best.desconocido < kMaxDesconocidoParaParar)))
			break;
		Attempt a = ejecutar(plan[i]);
		hechos.push_back({i, a});
		out.intentos_es.push_back(lineaIntento(plan[i], a));
		out.retried = true;
		if (a.threw) {
			if (out.retry_error_es.empty()) out.retry_error_es = a.error;
			continue;
		}
		if (attemptIsBetter(a.duration_ms, a.pct, best.duration_ms, best.pct)) {
			best = a;
			mejor = i;
		}
	}
	out.recovered_percent = best.pct;
	out.output_path = best.output;
	out.duration_ms = best.duration_ms;
	out.output_bytes = best.out_bytes;
	out.resincs = best.resincs;
	out.resincs_ia = best.resincs_ia;
	out.sincros = best.sincros;
	out.sincro_ms = best.sincro_ms;
	if (hechos.size() > 1) {
		out.retry_percent = best.pct;
		std::ostringstream m;
		m << describeMode(plan[mejor].s, plan[mejor].dyn) << " (la mejor de "
		  << hechos.size() << " estrategias probadas";
		if (mejor == 0 && out.desde_memoria) m << "; la memoria acertó";
		m << ")";
		out.mode_es = m.str();
	} else if (out.desde_memoria) {
		out.mode_es += " (elegida por la memoria de reparaciones: " + plan[0].por_que + ")";
	}
	// Solo se conserva la mejor salida; las demas se borran (antes
	// se guardaba tambien "el otro intento", con mas estrategias
	// serian hasta cuatro archivos casi iguales).
	for (const auto& h : hechos) {
		const Attempt& a = h.second;
		if (a.threw || a.output.empty() || a.output == best.output) continue;
		std::error_code ec;
		fs::remove(upath::toPath(a.output), ec);
	}
	if (best.duration_ms < kMinUsefulRecoveredMs && !best.output.empty()) {
		std::error_code ec;
		fs::remove(upath::toPath(best.output), ec);
	}
	if (mejor != 0) out.first_output_path.clear();  // se borro
	if (out.retried && mejor != 0) out.retry_output_path = best.output;

	if (out.duration_ms < kMinUsefulRecoveredMs) {
		out.ok = false;
		out.recovered_percent = 0;
		out.output_path.clear();
		out.output_bytes = 0;
		out.error_es =
		    "No se pudo recuperar ninguna parte útil (al menos 1 segundo) de " + corrupt_path + ".\n"
		    "Lo más probable es que el vídeo de referencia no sea del mismo celular o "
		    "cámara, o que se grabara con otra configuración (resolución, fotogramas por "
		    "segundo, HDR, estabilización...).\n"
		    "Qué hacer: graba un vídeo nuevo de 10-15 segundos con el MISMO dispositivo y "
		    "la MISMA configuración que el vídeo dañado y úsalo como referencia.\n"
		    "El archivo original no se ha modificado.";
		out.message_es = out.error_es;
		return out;
	}
	// Se aprende: la proxima vez, con esta camara, se prueba primero.
	memoria::anotar(out.firma_camara, plan[mejor].s, plan[mejor].dyn);

	// 1.6: vista previa minima en texto: duracion + % reconocido.
	std::ostringstream ss;
	ss << "Reparación terminada: " << corrupt_path << "\n"
	   << "  Recuperado: " << (int)out.recovered_percent << "% del vídeo"
	   << "  |  Duración: " << formatDurationEs(out.duration_ms) << "\n"
	   << "  Modo: " << out.mode_es << "\n"
	   << "  Archivo reparado: " << out.output_path;
	if (out.output_bytes > 0)
		ss << " (" << prettyBytesDiag(out.output_bytes) << ")";
	ss << "\n  El original no se ha modificado. Ábrelo en VLC para ver el primer fotograma.";
	if (out.resincs > 0) {
		ss << "\n  Zonas dañadas: se volvió a encontrar el vídeo " << out.resincs
		   << (out.resincs == 1 ? " vez" : " veces")
		   << " (3 fotogramas seguidos que encajan";
		if (out.resincs_ia > 0)
			ss << "; " << out.resincs_ia << " las decidió el modelo de resincronización";
		ss << ").";
	}
	if (out.sincros > 0) {
		ss << "\n  Sonido e imagen: se corrigió el desfase tras " << out.sincros
		   << (out.sincros == 1 ? " zona dañada" : " zonas dañadas") << " (en total "
		   << (int)std::lround(out.sincro_ms) << " ms) para que sigan a la par.";
	}
	if (out.intentos_es.size() > 1) {
		ss << "\n  Estrategias probadas:";
		for (const auto& l : out.intentos_es) ss << "\n    - " << l;
	}
	// Comprobacion: decodificar la salida y decir si se ve bien y
	// donde no (antes solo se daba la duracion y el %).
	if (run.verificar) {
		out.verificacion = verificarVideo(out.output_path);
		ss << "\n  " << out.verificacion.resumenEs();
	}
	out.message_es = ss.str();

	return out;
}

std::vector<std::pair<std::string, RepairOutcome>> repairBatch(
    const std::string& ref_path,
    const std::vector<std::string>& corrupt_paths,
    bool allow_retry) {
	return repairBatch(ref_path, corrupt_paths, RepairOptions::fromGlobals(), allow_retry);
}

std::vector<std::pair<std::string, RepairOutcome>> repairBatch(
    const std::string& ref_path,
    const std::vector<std::string>& corrupt_paths,
    const RepairOptions& opts,
    bool allow_retry) {
	std::vector<std::pair<std::string, RepairOutcome>> results;
	for (const auto& corrupt : corrupt_paths) {
		// Cada archivo es independiente: si uno falla, el siguiente
		// se intenta igual. repairFileWithAutoRetry no lanza. Todos
		// usan la misma foto de opciones (opts).
		results.emplace_back(corrupt, repairFileWithAutoRetry(ref_path, corrupt, opts, allow_retry));
	}
	return results;
}

std::string repairSummaryEs(const RepairOutcome& o, const std::string& corrupt_path) {
	if (!o.ok) {
		if (o.skipped) return "Omitido (ya existía la salida): " + corrupt_path + "\n" + o.error_es;
		return "FALLO: " + corrupt_path + "\n" + o.error_es;
	}
	return o.message_es;
}

std::string batchSummaryEs(
    const std::vector<std::pair<std::string, RepairOutcome>>& results) {
	int ok = 0, failed = 0, skipped = 0;
	std::ostringstream ss;
	ss << "\n===== INFORME DEL LOTE =====\n";
	for (const auto& [path, o] : results) {
		if (o.ok) ok++;
		else if (o.skipped) skipped++;
		else failed++;
		ss << "- " << path << ": ";
		if (o.ok)
			ss << "OK, " << (int)o.recovered_percent << "% recuperado, duración "
			   << formatDurationEs(o.duration_ms) << ", salida: " << o.output_path << "\n";
		else if (o.skipped)
			ss << "omitido (ya existía la salida)\n";
		else
			ss << "FALLO\n";
	}
	ss << "Total: " << results.size() << " archivos, " << ok << " reparados, "
	   << skipped << " omitidos, " << failed << " con fallo.\n"
	   << "Los originales no se han modificado; las reparaciones están en la carpeta Reparados/.\n";
	return ss.str();
}
