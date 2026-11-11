/*
	Untrunc-mejorado (Fase 2 hibrida) - moov_synthesizer.cpp
	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "moov_synthesizer.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "../../diagnoser.h"
#include "../../shell_util.h"
#include "../../upath.h"
#include "../../tempfs.h"

namespace fs = std::filesystem;

// Entrecomilla un argumento para la shell (POSIX y MSYS2).
// Sin esto, una ruta con espacios o caracteres raros romperia
// el comando de ffmpeg o, peor, permitiria inyeccion.
static std::string shellQuote(const std::string& s) { return shell::quote(s); }

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
	if (!shell::commandAvailable("ffmpeg")) {
		r.error_es = "No se encontró ffmpeg. El modo sin referencia lo necesita para "
		             "generar la referencia sintética. En Windows viene incluido junto "
		             "al programa (ffmpeg.exe): si falta, vuelve a descomprimir el ZIP "
		             "completo. En Linux, instálalo con tu gestor de paquetes (por "
		             "ejemplo: sudo apt install ffmpeg). También puedes usar una "
		             "referencia sana.";
		return r;
	}
	// Carpeta unica (tempfs.h). La referencia se usa DESPUES de esta
	// funcion, asi que no se borra aqui: la borra quien la usa
	// (rescueWithoutReference) con tempfs::removeDir(parent_path).
	std::error_code ec;
	fs::path tmpdir = tempfs::createUniqueDir("ai-ref", ec);
	if (ec || tmpdir.empty()) {
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
	                  shellQuote(upath::fromPath(out));
	int rc = upath::system(cmd);
	if (rc != 0) {
		r.error_es = "ffmpeg no pudo generar la referencia sintética (código de salida " +
		             std::to_string(rc) + ").";
		return r;
	}
	// Verificar que la referencia generada tiene moov (si no, no sirve).
	FileProbe probe = probeFile(upath::fromPath(out));
	if (!probe.has_moov) {
		r.error_es = "La referencia sintética generada no tiene índice (moov); no se puede usar.";
		return r;
	}
	r.ok = true;
	r.path = upath::fromPath(out);
	return r;
}
