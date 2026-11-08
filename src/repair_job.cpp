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

namespace {

// Un intento de reparacion con una configuracion dada.
// Devuelve el resultado sin lanzar excepciones (las captura todas).
struct Attempt {
	bool threw = false;
	std::string error;
	bool premature = false;
	double pct = 100.0;
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
		a.pct = mp4.premature_end_ ? mp4.premature_percentage_ : 100.0;
		a.output = mp4.getPathRepaired(ref, corrupt);
		a.duration_ms = mp4.getDurationInMs();
		std::error_code ec;
		if (fs::exists(a.output, ec) && !ec)
			a.out_bytes = fs::file_size(a.output, ec);
	} catch (const DiagnoserError& e) {
		a.threw = true;
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
	// g_dst_path solo tiene valor si el usuario dio -dst en el CLI.
	const std::string prev_dst = g_dst_path;
	const bool user_dst = !prev_dst.empty();
	std::string reparados_dir;
	if (!user_dst) {
		reparados_dir = ensureReparadosDir(corrupt_path);
		if (!reparados_dir.empty()) g_dst_path = reparados_dir;
	}

	// Si ya existe la salida y esta -skip, no se repara: se anota y
	// se sigue (en lote no se puede hacer exit(0) como hacia repair()).
	// La ruta esperada se calcula con Mp4::getPathRepaired() para
	// respetar el nombre real de Untrunc ("<nombre>.mp4_fixed.mp4",
	// con sufijo -s/-dyn si el usuario dio esas opciones).
	if (g_skip_existing) {
		Mp4 probe;
		std::string expected = probe.getPathRepaired(ref_path, corrupt_path);
		if (FileRead::alreadyExists(expected)) {
			out.ok = false;
			out.skipped = true;
			out.error_es = "Ya existe el archivo reparado y está activada la opción de omitir "
			               "existentes (-skip): " + expected + "\n"
			               "No se ha tocado el original.";
			out.message_es = out.error_es;
			g_dst_path = prev_dst;
			return out;
		}
	}

	const bool user_s = g_ignore_unknown;
	const bool user_dyn = g_use_chunk_stats;

	Attempt first = runAttempt(ref_path, corrupt_path);
	if (first.threw) {
		out.ok = false;
		out.error_es = "No se pudo reparar " + corrupt_path + ".\n"
		               "Error: " + first.error + "\n"
		               "El archivo original no se ha modificado.";
		out.message_es = out.error_es;
		g_dst_path = prev_dst;
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
	// con 90, que es lo que se queria decir.
	if (allow_retry && !user_s && first.premature && first.pct < 90.0) {
		out.retried = true;
		const bool saved_s = g_ignore_unknown;
		const bool saved_dyn = g_use_chunk_stats;
		g_ignore_unknown = true;
		g_use_chunk_stats = true;
		Attempt retry = runAttempt(ref_path, corrupt_path);
		g_ignore_unknown = saved_s;
		g_use_chunk_stats = saved_dyn;
		if (!retry.threw) {
			out.retry_percent = retry.pct;
			out.retry_output_path = retry.output;
			if (retry.pct > first.pct) {
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
	if (out.retried && !out.retry_output_path.empty() && out.retry_output_path != out.output_path)
		ss << "\n  (También se guardó el otro intento en: " << out.retry_output_path << ")";
	out.message_es = ss.str();

	g_dst_path = prev_dst;
	return out;
}

std::vector<std::pair<std::string, RepairOutcome>> repairBatch(
    const std::string& ref_path,
    const std::vector<std::string>& corrupt_paths,
    bool allow_retry) {
	std::vector<std::pair<std::string, RepairOutcome>> results;
	for (const auto& corrupt : corrupt_paths) {
		// Cada archivo es independiente: si uno falla, el siguiente
		// se intenta igual. repairFileWithAutoRetry no lanza.
		results.emplace_back(corrupt, repairFileWithAutoRetry(ref_path, corrupt, allow_retry));
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
