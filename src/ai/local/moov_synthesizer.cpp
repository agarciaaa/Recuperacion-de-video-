/*
	Untrunc-mejorado (Fase 2 hibrida) - moov_synthesizer.cpp
	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "moov_synthesizer.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "../../diagnoser.h"

namespace fs = std::filesystem;

// Entrecomilla un argumento para la shell (POSIX y MSYS2).
// Sin esto, una ruta con espacios o caracteres raros romperia
// el comando de ffmpeg o, peor, permitiria inyeccion.
static std::string shellQuote(const std::string& s) {
	std::string out = "'";
	for (char c : s) {
		if (c == '\'') out += "'\\''";
		else out += c;
	}
	out += "'";
	return out;
}

SyntheticRef buildSyntheticReference(int width, int height, int fps,
                                     const std::string& codec,
                                     const std::string& profile) {
	SyntheticRef r;
	r.width = width;
	r.height = height;
	r.fps = fps;
	r.codec = codec;
	if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
		r.error_es = "No se puede construir una referencia sintética: resolución no detectada "
		             "o fuera de rango (" + std::to_string(width) + "x" + std::to_string(height) + ").";
		return r;
	}
	if (fps <= 0 || fps > 240) fps = 30;  // valor seguro por defecto
	r.fps = fps;
	if (codec != "avc1" && codec != "hvc1") {
		r.error_es = "No se puede construir una referencia sintética: códec no soportado (" +
		             codec + "). Solo H.264 (avc1) y H.265 (hvc1).";
		return r;
	}
	// ffmpeg en el PATH (Linux, MSYS2 en Windows).
	int has_ffmpeg = std::system("ffmpeg -version > /dev/null 2>&1");
	if (has_ffmpeg != 0) {
		r.error_es = "No se encontró ffmpeg en el sistema. El modo sin referencia necesita "
		             "ffmpeg para generar la referencia sintética. En Windows, instálalo "
		             "con MSYS2 (pacman -S mingw-w64-x86_64-ffmpeg) o usa una referencia sana.";
		return r;
	}
	std::error_code ec;
	fs::path tmpdir = fs::temp_directory_path(ec) / "untrunc-ai-ref";
	if (ec) {
		r.error_es = "No se pudo crear el directorio temporal para la referencia sintética.";
		return r;
	}
	fs::create_directories(tmpdir, ec);
	if (ec) {
		r.error_es = "No se pudo crear el directorio temporal para la referencia sintética.";
		return r;
	}
	fs::path out = tmpdir / "ref_sintetica.mp4";
	std::string encoder = (codec == "hvc1") ? "libx265" : "libx264";
	std::string prof = profile.empty() ? "high" : profile;
	if (prof != "high" && prof != "main" && prof != "baseline") prof = "high";
	std::string cmd = "ffmpeg -y -v error -f lavfi -i testsrc=size=" +
	                  std::to_string(width) + "x" + std::to_string(height) +
	                  ":rate=" + std::to_string(fps) + ":duration=2"
	                  " -f lavfi -i sine=frequency=440:duration=2"
	                  " -c:v " + encoder + " -profile:v " + prof +
	                  " -pix_fmt yuv420p -g " + std::to_string(fps) +
	                  " -c:a aac -b:a 96k -ar 48000 -ac 2 -shortest " +
	                  shellQuote(out.string());
	int rc = std::system(cmd.c_str());
	if (rc != 0) {
		r.error_es = "ffmpeg no pudo generar la referencia sintética (código de salida " +
		             std::to_string(rc) + ").";
		return r;
	}
	// Verificar que la referencia generada tiene moov (si no, no sirve).
	FileProbe probe = probeFile(out.string());
	if (!probe.has_moov) {
		r.error_es = "La referencia sintética generada no tiene índice (moov); no se puede usar.";
		return r;
	}
	r.ok = true;
	r.path = out.string();
	return r;
}
