/*
	Untrunc-mejorado (Fase 1) - repair_job.cpp

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "repair_job.h"

#include <filesystem>
#include <sstream>

#include "common.h"
#include "file.h"
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
};

Attempt runAttempt(const std::string& ref, const std::string& corrupt) {
	Attempt a;
	try {
		Mp4 mp4;
		g_mp4 = &mp4;
		mp4.parseOk(ref);
		mp4.repair(corrupt);
		a.premature = mp4.premature_end_;
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
	if (with_s && with_dyn) return "reintento automático con -s -dyn";
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

	Attempt first = runAttempt(ref_path, corrupt_path);
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
	out.mode_es = describeMode(user_s, user_dyn);

	// 1.3: auto -s + auto -dyn. Solo un reintento, solo si el
	// primer intento se corto antes del 90% y el usuario no dio -s.
	// Nota: premature_percentage_ es un porcentaje 0-100 (no una
	// fraccion 0-1); el codigo original de la GUI comparaba con
	// 0.9, lo que practicamente nunca se cumplia. Aqui se compara
	// con 90, que es lo que se queria decir. Se usa el % honesto
	// (honestRecoveredPercent), asi que tambien se reintenta si el
	// primer intento "termino" pero no recupero nada.
	int64_t retry_duration_ms = 0;
	if (allow_retry && !user_s && first.pct < 90.0) {
		out.retried = true;
		RepairOptions retry_opts = run;
		retry_opts.ignore_unknown = true;
		retry_opts.use_chunk_stats = true;
		Attempt retry;
		{
			RepairSession retry_session(retry_opts);  // sesion anidada
			retry = runAttempt(ref_path, corrupt_path);
		}
		if (!retry.threw) {
			retry_duration_ms = retry.duration_ms;
			out.retry_percent = retry.pct;
			out.retry_output_path = retry.output;
			if (attemptIsBetter(retry.duration_ms, retry.pct, first.duration_ms, first.pct)) {
				// Nos quedamos con el resultado mas largo.
				out.recovered_percent = retry.pct;
				out.output_path = retry.output;
				out.duration_ms = retry.duration_ms;
				out.output_bytes = retry.out_bytes;
				out.mode_es = "reintento automático con -s -dyn (mejor que el primer intento)";
			} else {
				out.mode_es = "normal (el reintento automático con -s -dyn no mejoró el resultado; "
				              "se conserva el primer intento)";
			}
		} else {
			out.retry_error_es = retry.error;
			out.mode_es = "normal (el reintento automático falló; se conserva el primer intento)";
		}
	}

	// Un intento que no llego a kMinUsefulRecoveredMs no recupero
	// nada util: su salida (un MP4 de pocos KB, 0-1 fotogramas) no
	// sirve y confunde. Se borra.
	auto removeEmptyOutput = [](const std::string& path, int64_t duration_ms) {
		if (path.empty() || duration_ms >= kMinUsefulRecoveredMs) return;
		std::error_code ec;
		fs::remove(upath::toPath(path), ec);
	};
	removeEmptyOutput(out.first_output_path, first.duration_ms);
	if (out.retried) removeEmptyOutput(out.retry_output_path, retry_duration_ms);

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

	// El otro intento solo se menciona si recupero algo (si no, se borro).
	std::string other_output;
	if (out.retried) {
		const bool kept_first = (out.output_path == out.first_output_path);
		const std::string other = kept_first ? out.retry_output_path : out.first_output_path;
		const int64_t other_ms = kept_first ? retry_duration_ms : first.duration_ms;
		if (!other.empty() && other != out.output_path && other_ms >= kMinUsefulRecoveredMs)
			other_output = other;
	}

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
	if (!other_output.empty())
		ss << "\n  (También se guardó el otro intento en: " << other_output << ")";
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
