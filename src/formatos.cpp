/*
	Untrunc-mejorado - formatos.cpp

	Implementacion: deteccion por magic bytes + conversion a MP4.
	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "formatos.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "diagnoser.h"

namespace fs = std::filesystem;

// Entrecomilla un argumento para la shell (POSIX y MSYS2), igual
// que en moov_synthesizer.cpp: sin esto, una ruta con espacios
// romperia el comando de ffmpeg o permitiria inyeccion.
static std::string shellQuote(const std::string& s) {
	std::string out = "'";
	for (char c : s) {
		if (c == '\'') out += "'\\''";
		else out += c;
	}
	out += "'";
	return out;
}

static std::string lowerExt(const std::string& path) {
	fs::path p(path);
	std::string e = p.extension().string();
	for (char& c : e) c = (char)std::tolower((unsigned char)c);
	return e;
}

static bool startsWith(const unsigned char* buf, size_t n, const char* magic, size_t m) {
	if (n < m) return false;
	for (size_t i = 0; i < m; i++)
		if (buf[i] != (unsigned char)magic[i]) return false;
	return true;
}

FormatInfo detectFormat(const std::string& path) {
	FormatInfo info;
	std::error_code ec;
	if (!fs::exists(path, ec) || ec) { info.name = "desconocido"; return info; }
	if (fs::file_size(path, ec) == 0 || ec) { info.name = "vacío"; return info; }

	std::ifstream f(path, std::ios::binary);
	if (!f) { info.name = "desconocido"; return info; }
	unsigned char buf[4096] = {0};
	f.read(reinterpret_cast<char*>(buf), sizeof(buf));
	size_t n = (size_t)f.gcount();
	std::string ext = lowerExt(path);

	// 1) BMFF por contenido: tamano (4 bytes) + "ftyp" en el offset 4.
	if (n >= 12 && std::string(reinterpret_cast<const char*>(buf + 4), 4) == "ftyp") {
		info.is_bmff = true;
		info.convertible = true;  // ya es MP4: no hay que convertir
		info.kind = FormatKind::Bmff;
		info.ftyp = std::string(reinterpret_cast<const char*>(buf + 8), 4);
		// Nombre por marca, no por extension (un .mp4 renombrado
		// a .mov sigue siendo MP4, y viceversa).
		if (info.ftyp == "qt  ") info.name = "MOV (QuickTime)";
		else if (info.ftyp == "M4V " || info.ftyp == "M4VP") info.name = "M4V";
		else if (info.ftyp.rfind("3gp", 0) == 0 || info.ftyp.rfind("3g2", 0) == 0) info.name = "3GP";
		else if (info.ftyp == "isom" || info.ftyp == "iso2" || info.ftyp == "mp41" ||
		         info.ftyp == "mp42" || info.ftyp == "avc1" || info.ftyp == "dash")
			info.name = "MP4";
		else info.name = "MP4 (ftyp=" + info.ftyp + ")";
		return info;
	}
	// 2) EBML (Matroska / WebM): 1A 45 DF A3. WebM se distingue
	// porque la palabra "webm" aparece en los primeros 4 KB.
	if (n >= 4 && buf[0] == 0x1A && buf[1] == 0x45 && buf[2] == 0xDF && buf[3] == 0xA3) {
		std::string head(reinterpret_cast<const char*>(buf), n);
		if (head.find("webm") != std::string::npos) {
			info.kind = FormatKind::Webm; info.name = "WebM";
		} else {
			info.kind = FormatKind::Mkv; info.name = "MKV (Matroska)";
		}
		info.convertible = true;
		return info;
	}
	// 3) AVI: "RIFF" + tamano + "AVI ".
	if (n >= 12 && startsWith(buf, n, "RIFF", 4) &&
	    std::string(reinterpret_cast<const char*>(buf + 8), 4) == "AVI ") {
		info.kind = FormatKind::Avi; info.name = "AVI"; info.convertible = true;
		return info;
	}
	// 4) ASF / WMV: GUID 30 26 B2 75 8E 66 CF 11 A6 D9 00 AA 00 62 CE 6C.
	static const unsigned char kAsfGuid[16] = {
		0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
		0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
	if (n >= 16 && startsWith(buf, n, reinterpret_cast<const char*>(kAsfGuid), 16)) {
		info.kind = FormatKind::Wmv; info.name = "WMV (ASF)"; info.convertible = true;
		return info;
	}
	// 5) FLV: "FLV" + version.
	if (n >= 3 && startsWith(buf, n, "FLV", 3)) {
		info.kind = FormatKind::Flv; info.name = "FLV"; info.convertible = true;
		return info;
	}
	// 6) MPEG-PS: 00 00 01 BA (pack start code).
	if (n >= 4 && buf[0] == 0 && buf[1] == 0 && buf[2] == 1 && buf[3] == 0xBA) {
		info.kind = FormatKind::Mpg; info.name = "MPEG-PS (.mpg)"; info.convertible = true;
		return info;
	}
	// 7) MPEG-TS: byte de sincronia 0x47 en 0, 188 y 376.
	if (n >= 377 && buf[0] == 0x47 && buf[188] == 0x47 && buf[376] == 0x47) {
		info.kind = FormatKind::Mts; info.name = "MPEG-TS (.mts/.m2ts)"; info.convertible = true;
		return info;
	}
	// 8) El contenido no dice nada: solo queda la extension.
	// Esto cubre archivos truncados a los que les falta la
	// cabecera, o contenedores raros. Es una pista, no una certeza.
	if (ext == ".mp4" || ext == ".mov" || ext == ".m4v" || ext == ".3gp") {
		info.kind = FormatKind::Bmff; info.is_bmff = true; info.convertible = true;
		info.name = (ext == ".mov") ? "MOV (por extensión)" :
		            (ext == ".m4v") ? "M4V (por extensión)" :
		            (ext == ".3gp") ? "3GP (por extensión)" : "MP4 (por extensión)";
	} else if (ext == ".mkv") { info.kind = FormatKind::Mkv; info.name = "MKV (por extensión)"; info.convertible = true; }
	else if (ext == ".webm") { info.kind = FormatKind::Webm; info.name = "WebM (por extensión)"; info.convertible = true; }
	else if (ext == ".avi") { info.kind = FormatKind::Avi; info.name = "AVI (por extensión)"; info.convertible = true; }
	else if (ext == ".wmv" || ext == ".asf") { info.kind = FormatKind::Wmv; info.name = "WMV (por extensión)"; info.convertible = true; }
	else if (ext == ".flv") { info.kind = FormatKind::Flv; info.name = "FLV (por extensión)"; info.convertible = true; }
	else if (ext == ".mpg" || ext == ".mpeg") { info.kind = FormatKind::Mpg; info.name = "MPEG-PS (por extensión)"; info.convertible = true; }
	else if (ext == ".mts" || ext == ".m2ts" || ext == ".ts") { info.kind = FormatKind::Mts; info.name = "MPEG-TS (por extensión)"; info.convertible = true; }
	else { info.kind = FormatKind::Unknown; info.name = "desconocido"; info.convertible = false; }
	return info;
}

std::string formatNameEs(const FormatInfo& info) {
	return info.name.empty() ? "desconocido" : info.name;
}

bool isDirectlyRepairable(const std::string& path) {
	return detectFormat(path).is_bmff;
}

std::string convertedPathDefault(const std::string& input) {
	fs::path p(input);
	fs::path parent = p.parent_path();
	if (parent.empty()) parent = ".";
	std::string stem = p.stem().string();
	return (parent / (stem + "_convertido.mp4")).string();
}

bool probeCodecs(const std::string& path, std::string& video_codec,
                 std::string& audio_codec) {
	video_codec.clear();
	audio_codec.clear();
	std::error_code ec;
	fs::path tmp = fs::temp_directory_path(ec) / "untrunc-codecs.txt";
	if (ec) return false;
	std::string cmd = "ffprobe -v error -show_entries stream=codec_type,codec_name "
	                  "-of csv=p=0 " + shellQuote(path) + " > " + shellQuote(tmp.string()) + " 2>/dev/null";
	if (std::system(cmd.c_str()) != 0) return false;
	std::ifstream in(tmp);
	if (!in) return false;
	std::string line;
	bool any = false;
	while (std::getline(in, line)) {
		// Formato csv de ffprobe: "h264,video" / "aac,audio"
		// (codec_name primero, codec_type despues). Se aceptan
		// los dos ordenes, por si cambia la version de ffprobe.
		auto comma = line.find(',');
		if (comma == std::string::npos) continue;
		std::string f1 = line.substr(0, comma);
		std::string f2 = line.substr(comma + 1);
		auto clean = [](std::string s) {
			s.erase(std::remove_if(s.begin(), s.end(),
			                       [](unsigned char c){ return c == '\r' || c == '\n' || c == ' '; }),
			        s.end());
			return s;
		};
		f1 = clean(f1); f2 = clean(f2);
		std::string type, codec;
		if (f1 == "video" || f1 == "audio") { type = f1; codec = f2; }
		else if (f2 == "video" || f2 == "audio") { type = f2; codec = f1; }
		else continue;
		if (type == "video" && video_codec.empty()) { video_codec = codec; any = true; }
		if (type == "audio" && audio_codec.empty()) { audio_codec = codec; any = true; }
	}
	fs::remove(tmp, ec);
	return any;
}

// Codecs que un MP4 admite y que Untrunc sabe reparar. Si el
// remux deja un codec fuera de esta lista, el MP4 tendra moov
// pero Untrunc no podra reparar su video (caso real medido:
// WebM VP9+Opus remuxeado a MP4: el MP4 se crea, pero la
// reparacion solo recupera 0,02 s).
static bool mp4CompatibleCodec(const std::string& video_codec,
                               const std::string& audio_codec) {
	static const char* kVideo[] = {"h264", "hevc", "mpeg4", "mjpeg"};
	bool v_ok = video_codec.empty();
	for (auto v : kVideo) if (video_codec == v) v_ok = true;
	static const char* kAudio[] = {"aac", "mp3", "ac3", "eac3", "alac", "mp4a"};
	bool a_ok = audio_codec.empty();
	for (auto a : kAudio) if (audio_codec == a) a_ok = true;
	return v_ok && a_ok;
}

ConvertResult convertToMp4(const std::string& input,
                           const std::string& output,
                           bool recode_ok) {
	ConvertResult r;
	std::error_code ec;
	if (!fs::exists(input, ec) || ec) {
		r.error_es = "No se encontró el archivo a convertir: " + input;
		return r;
	}
	if (std::system("ffmpeg -version > /dev/null 2>&1") != 0) {
		r.error_es = "No se encontró ffmpeg en el sistema. En Windows, instálalo con "
		             "MSYS2 (pacman -S mingw-w64-ucrt-x86_64-ffmpeg). Sin ffmpeg no se "
		             "puede convertir este formato a MP4.";
		return r;
	}
	r.output = output.empty() ? convertedPathDefault(input) : output;
	if (fs::equivalent(fs::path(input), fs::path(r.output), ec) && !ec) {
		r.error_es = "La salida no puede ser el mismo archivo que la entrada.";
		return r;
	}
	// 1) Remux sin re-codificar: rapido y sin perdida.
	// OJO: con archivos TRUNCADOS ffmpeg suele salir con codigo
	// distinto de 0 ("File ended prematurely") aunque el MP4
	// salio usable. Por eso el exito se juzga por la salida
	// (existe y pesa >1 KB), no por el codigo de salida.
	std::string cmd = "ffmpeg -y -v error -i " + shellQuote(input) +
	                  " -c copy -map 0 " + shellQuote(r.output) + " 2>/dev/null";
	std::system(cmd.c_str());
	if (fs::exists(r.output, ec) && !ec &&
	    fs::file_size(r.output, ec) > 1024 && !ec) {
		std::string vc, ac;
		if (probeCodecs(r.output, vc, ac) && mp4CompatibleCodec(vc, ac)) {
			r.ok = true;
			r.recoded = false;
			r.method_es = "sin re-codificar (remux, sin pérdida de calidad)";
			return r;
		}
		// El remux "funciono" pero dejo codecs que Untrunc no
		// repara (VP9, Opus, WMV2...): hay que re-codificar.
		fs::remove(r.output, ec);
		if (!recode_ok) {
			r.error_es = "El archivo se convirtió a MP4, pero sus códecs (" +
			             (vc.empty() ? std::string("desconocido") : vc) + " / " +
			             (ac.empty() ? std::string("sin audio") : ac) +
			             ") no son compatibles con MP4/Untrunc. Habría que re-codificar "
			             "a H.264 + AAC (pierde algo de calidad y tarda más).";
			return r;
		}
	} else {
		// El remux fallo del todo (p. ej. WMV2 no cabe en MP4).
		fs::remove(r.output, ec);
		if (!recode_ok) {
			r.error_es = "No se pudo convertir a MP4 sin re-codificar (los códecs de "
			             "este formato no caben en un MP4). Re-codificar a H.264 + AAC "
			             "podría funcionar, pero pierde algo de calidad y tarda más.";
			return r;
		}
	}
	// 2) Re-codificacion a H.264 + AAC: ultimo recurso, avisado.
	// Igual que el remux: el exito se juzga por la salida, no
	// por el codigo de ffmpeg (un archivo truncado casi siempre
	// hace que ffmpeg se queje al final, aunque el MP4 sirva).
	cmd = "ffmpeg -y -v error -i " + shellQuote(input) +
	      " -map 0:v:0 -map 0:a? -c:v libx264 -pix_fmt yuv420p -preset veryfast -crf 20 "
	      "-c:a aac -b:a 128k " + shellQuote(r.output) + " 2>/dev/null";
	std::system(cmd.c_str());
	if (!fs::exists(r.output, ec) || ec ||
	    fs::file_size(r.output, ec) <= 1024) {
		r.error_es = "FFmpeg no pudo convertir este archivo a MP4 ni siquiera "
		             "re-codificando. Puede que el archivo esté demasiado dañado "
		             "o use un códec que FFmpeg no entiende.";
		return r;
	}
	r.ok = true;
	r.recoded = true;
	r.method_es = "re-codificando a H.264 + AAC (pierde algo de calidad y tarda más, "
	              "pero el MP4 resultante sí se puede reparar)";
	return r;
}
