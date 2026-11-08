/*
	Untrunc-mejorado (Fase 1) - diagnoser.cpp

	Implementacion del pre-vuelo. Lee solo cabeceras de atomos MP4
	(tamano de 32 bits big-endian + tipo de 4 letras) y busca inicios
	de NAL en los primeros megabytes. No usa FFmpeg: asi el
	diagnostico funciona incluso cuando el archivo no tiene moov
	(justo el caso que FFmpeg no puede leer).

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "diagnoser.h"
#include "formatos.h"
#include "upath.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

static uint32_t readBe32(const unsigned char* p) {
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
	       (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static uint64_t readBe64(const unsigned char* p) {
	uint64_t v = 0;
	for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
	return v;
}

static std::string readFourCc(const unsigned char* p) {
	return std::string(reinterpret_cast<const char*>(p), 4);
}

std::string prettyBytesDiag(uint64_t bytes) {
	const char* units[] = {"B", "KB", "MB", "GB"};
	double v = (double)bytes;
	int u = 0;
	while (v >= 1024.0 && u < 3) { v /= 1024.0; u++; }
	std::ostringstream ss;
	ss.precision(2);
	ss << std::fixed << v << " " << units[u];
	return ss.str();
}

static bool sameFilePaths(const std::string& a, const std::string& b) {
	std::error_code ec;
	const fs::path pa = upath::toPath(a), pb = upath::toPath(b);
	if (fs::exists(pa, ec) && fs::exists(pb, ec)) {
		std::error_code ec2;
		if (fs::equivalent(pa, pb, ec2) && !ec2) return true;
	}
	// Comparacion por ruta normalizada (por si uno no existe).
	std::error_code ec3, ec4;
	auto ca = fs::weakly_canonical(pa, ec3);
	auto cb = fs::weakly_canonical(pb, ec4);
	return !ec3 && !ec4 && ca == cb;
}

// Busca inicios de NAL Annex-B (00 00 00 01 o 00 00 01) en un buffer.
static void scanNals(const std::vector<unsigned char>& buf, FileProbe& out) {
	for (size_t i = 0; i + 4 < buf.size(); i++) {
		size_t nal = 0;
		if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1) {
			nal = i + 4;
		} else if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
			nal = i + 3;
		} else {
			continue;
		}
		if (nal >= buf.size()) break;
		unsigned char b = buf[nal];
		int h264_type = b & 0x1F;            // H.264: tipo en los 5 bits bajos
		int hevc_type = (b >> 1) & 0x3F;     // H.265: tipo en los bits 1..6
		if (h264_type == 1 || h264_type == 5 || h264_type == 7 || h264_type == 8)
			out.has_nal = true;
		if (hevc_type == 0 || hevc_type == 1 || hevc_type == 19 ||
		    hevc_type == 20 || hevc_type == 32 || hevc_type == 33 || hevc_type == 34)
			out.has_nal = true;
		if (h264_type == 7) out.has_avc_sps = true;
		if (out.has_nal && out.has_avc_sps) return;
		i = nal;  // saltar al NAL encontrado
	}
}

// Punto 8 (2026-11-07): busca atomos ftyp/moov/mdat en CUALQUIER
// posicion del buffer, no solo al principio. Solo se usa cuando el
// paseo secuencial no encontro nada: un archivo recuperado puede
// tener la cabecera inicial danada (basura en los primeros bytes,
// primer tamano incoherente) y la estructura MP4 valida unos KB
// despues; VLC lo reproduce porque escanea todo el archivo, pero
// el paseo secuencial se detiene en el primer tamano roto y no la
// ve. Para evitar falsos positivos dentro de los datos de video,
// se exige un tamano previo plausible (8..lo que queda de archivo,
// o 1 = 64 bits, o 0 = hasta el final) y, para ftyp, una marca
// legible (4 bytes ASCII imprimibles justo despues).
static void scanDeepAtoms(const std::vector<unsigned char>& buf,
                          uint64_t file_size, FileProbe& out) {
	if (buf.size() < 16) return;
	auto plausible_size = [&](size_t pos, uint32_t s32) -> bool {
		if (s32 == 0) return true;  // hasta el final del archivo
		if (s32 == 1) {             // tamano de 64 bits: leerlo
			if (pos + 16 > buf.size()) return false;
			uint64_t s64 = readBe64(buf.data() + pos + 8);
			return s64 >= 16 && s64 <= file_size - (uint64_t)pos;
		}
		return s32 >= 8 && (uint64_t)s32 <= file_size - (uint64_t)pos;
	};
	for (size_t i = 0; i + 16 <= buf.size(); i++) {
		std::string type = readFourCc(buf.data() + i + 4);
		if (type != "ftyp" && type != "moov" && type != "mdat") continue;
		uint32_t s32 = readBe32(buf.data() + i);
		if (!plausible_size(i, s32)) continue;
		if (type == "ftyp") {
			bool brand_ok = true;
			for (int k = 0; k < 4; k++) {
				unsigned char c = buf[i + 8 + (size_t)k];
				if (c < 0x20 || c > 0x7E) { brand_ok = false; break; }
			}
			if (!brand_ok) continue;
			if (!out.has_ftyp_deep) out.deep_ftyp_offset = (uint64_t)i;
			out.has_ftyp_deep = true;
		} else if (type == "moov") {
			out.has_moov_deep = true;
		} else {
			out.has_mdat_deep = true;
		}
		if (out.has_ftyp_deep && out.has_moov_deep && out.has_mdat_deep) return;
	}
}

FileProbe probeFile(const std::string& path) {
	FileProbe out;
	std::error_code ec;
	const fs::path fpath = upath::toPath(path);
	if (!fs::exists(fpath, ec) || ec) return out;
	out.exists = true;
	// Formato por contenido (formatos.h): un .mp4 renombrado a
	// .mov sigue siendo MP4, y un MKV renombrado a .mp4 sigue
	// siendo MKV. Esto se calcula siempre, aunque el archivo
	// este truncado y no tenga atomos legibles.
	FormatInfo fi = detectFormat(path);
	out.format_es = fi.name;
	out.is_bmff = fi.is_bmff;
	out.size = fs::file_size(fpath, ec);
	if (ec) out.size = 0;
	if (out.size == 0) return out;

	std::ifstream f(fpath, std::ios::binary);
	if (!f) return out;

	// 1) Atomos de nivel superior.
	uint64_t offset = 0;
	int guard = 0;
	while (offset + 8 <= out.size && guard++ < 1000) {
		unsigned char hdr[16] = {0};
		f.seekg((std::streamoff)offset);
		f.read(reinterpret_cast<char*>(hdr), 8);
		if (f.gcount() < 8) break;
		uint32_t size32 = readBe32(hdr);
		std::string type = readFourCc(hdr + 4);
		uint64_t atom_size = size32;
		uint64_t header = 8;
		if (size32 == 1) {  // tamano de 64 bits
			f.read(reinterpret_cast<char*>(hdr), 8);
			if (f.gcount() < 8) break;
			atom_size = readBe64(hdr);
			header = 16;
		} else if (size32 == 0) {
			atom_size = out.size - offset;  // hasta el final del archivo
		}
		if (atom_size < header) break;
		if (type == "ftyp") {
			out.has_ftyp = true;
			unsigned char brand[4] = {0};
			f.seekg((std::streamoff)(offset + header));
			f.read(reinterpret_cast<char*>(brand), 4);
			if (f.gcount() == 4) out.ftyp = readFourCc(brand);
		} else if (type == "moov") {
			out.has_moov = true;
		} else if (type == "mdat") {
			out.has_mdat = true;
			out.mdat_bytes += (atom_size > header) ? (atom_size - header) : 0;
		}
		if (atom_size == 0) break;
		offset += atom_size;
	}

	// 2) Inicios de NAL en los primeros 16 MB (ahi suele estar el
	// principio del mdat; basta para saber si hay video H.264/H.265).
	const uint64_t kScanMax = 16ull << 20;
	uint64_t to_read = std::min<uint64_t>(out.size, kScanMax);
	std::vector<unsigned char> buf((size_t)to_read);
	f.seekg(0);
	f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)to_read);
	buf.resize((size_t)f.gcount());
	scanNals(buf, out);
	// Punto 8: si el paseo secuencial no vio ningun atomo, buscar
	// en profundidad antes de declarar que no es un MP4.
	if (!out.has_ftyp && !out.has_moov && !out.has_mdat)
		scanDeepAtoms(buf, out.size, out);
	return out;
}

DiagReport preflightCheck(const std::string& ref_path, const std::string& bad_path) {
	DiagReport r;
	r.ref_path = ref_path;
	r.bad_path = bad_path;

	if (sameFilePaths(ref_path, bad_path)) {
		r.code = DiagCode::SameFile;
		r.ref = probeFile(ref_path);
		r.bad = r.ref;
		return r;
	}
	r.ref = probeFile(ref_path);
	r.bad = probeFile(bad_path);

	if (!r.ref.exists) { r.code = DiagCode::RefMissing; return r; }
	if (r.ref.size == 0) { r.code = DiagCode::RefZeroLength; return r; }
	if (!r.bad.exists) { r.code = DiagCode::BadMissing; return r; }
	if (r.bad.size == 0) { r.code = DiagCode::BadZeroLength; return r; }
	if (!r.ref.has_ftyp && !r.ref.has_moov && !r.ref.has_mdat) {
		// Formato conocido pero no BMFF (MKV, AVI, WebM...):
		// no es un archivo desconocido, es un formato que hay
		// que convertir a MP4 primero. Se distingue del caso
		// "no parece un video" para dar la accion correcta.
		if (!r.ref.is_bmff && r.ref.format_es != "desconocido" &&
		    r.ref.format_es != "vacío") {
			r.code = DiagCode::RefFormatNoBmff; return r;
		}
		r.code = DiagCode::RefNoFtyp; return r;
	}
	if (!r.bad.has_ftyp && !r.bad.has_moov && !r.bad.has_mdat) {
		if (!r.bad.is_bmff && r.bad.format_es != "desconocido" &&
		    r.bad.format_es != "vacío") {
			r.code = DiagCode::BadFormatNoBmff; return r;
		}
		// Punto 8: estructura MP4 desplazada (cabecera inicial
		// danada pero ftyp/moov/mdat presentes mas adelante):
		// no es "no parece un video", es "parece un video con
		// la cabecera rota". Se distingue para dar la accion
		// correcta (convertir primero, o intentar el rescate).
		if (r.bad.has_ftyp_deep || r.bad.has_moov_deep || r.bad.has_mdat_deep) {
			r.code = DiagCode::BadCabeceraDaniada; return r;
		}
		r.code = DiagCode::BadNoFtyp; return r;
	}
	if (!r.ref.has_moov) {
		// 1.2: si el "danado" SI tiene indice y datos, lo mas
		// probable es que los archivos esten intercambiados.
		if (r.bad.has_moov && r.bad.has_mdat && r.bad.mdat_bytes > 65536)
			r.code = DiagCode::RefNoMoovSwappedLikely;
		else
			r.code = DiagCode::RefNoMoov;
		return r;
	}
	if (!r.bad.has_mdat) { r.code = DiagCode::BadNoMdat; return r; }

	r.code = DiagCode::Ok;
	return r;
}

std::string diagCodeName(DiagCode code) {
	switch (code) {
		case DiagCode::Ok: return "Ok";
		case DiagCode::RefMissing: return "RefMissing";
		case DiagCode::RefZeroLength: return "RefZeroLength";
		case DiagCode::BadMissing: return "BadMissing";
		case DiagCode::BadZeroLength: return "BadZeroLength";
		case DiagCode::SameFile: return "SameFile";
		case DiagCode::RefNoMoov: return "RefNoMoov";
		case DiagCode::RefNoMoovSwappedLikely: return "RefNoMoovSwappedLikely";
		case DiagCode::BadNoMdat: return "BadNoMdat";
		case DiagCode::RefNoFtyp: return "RefNoFtyp";
		case DiagCode::BadNoFtyp: return "BadNoFtyp";
		case DiagCode::BadCabeceraDaniada: return "BadCabeceraDaniada";
		case DiagCode::RefFormatNoBmff: return "RefFormatNoBmff";
		case DiagCode::BadFormatNoBmff: return "BadFormatNoBmff";
	}
	return "Unknown";
}

std::string diagActionEs(DiagCode code) {
	switch (code) {
		case DiagCode::Ok: return "Se puede intentar la reparación.";
		case DiagCode::RefMissing: return "Elige un vídeo de referencia que exista.";
		case DiagCode::RefZeroLength: return "Ese archivo de referencia está vacío; elige otro.";
		case DiagCode::BadMissing: return "Elige un vídeo dañado que exista.";
		case DiagCode::BadZeroLength: return "Ese vídeo dañado está vacío (0 bytes); no hay nada que reparar.";
		case DiagCode::SameFile: return "Elige dos archivos distintos: uno sano y uno dañado.";
		case DiagCode::RefNoMoov: return "Graba un vídeo nuevo de 10-15 segundos con el mismo celular o cámara y úsalo como referencia.";
		case DiagCode::RefNoMoovSwappedLikely: return "Intercambia los dos archivos e inténtalo de nuevo.";
		case DiagCode::BadNoMdat: return "Ese archivo dañado no tiene datos de vídeo; busca otra copia o vuelve a recuperarlo.";
		case DiagCode::RefNoFtyp: return "Ese archivo no parece un vídeo MP4/MOV; elige otra referencia.";
		case DiagCode::BadNoFtyp: return "Ese archivo no parece un vídeo MP4/MOV; comprueba si se reproduce en VLC: si sí, pulsa «Convertir a MP4» o marca «Intentar de todos modos». Si no se reproduce en ningún reproductor, probablemente no contiene vídeo recuperable; busca otra copia.";
		case DiagCode::BadCabeceraDaniada: return "El archivo sí tiene estructura MP4 pero la cabecera inicial está dañada. Pulsa «Convertir a MP4» primero (FFmpeg reconstruye la cabecera) y repara el MP4 resultante; o marca «Intentar de todos modos» para probar el rescate sin referencia.";
		case DiagCode::RefFormatNoBmff: return "Convierte la referencia a MP4 primero (botón «Convertir a MP4» o untrunc --convertir-a-mp4).";
		case DiagCode::BadFormatNoBmff: return "Convierte el vídeo dañado a MP4 primero (botón «Convertir a MP4» o untrunc --convertir-a-mp4) y repara el MP4 resultante.";
	}
	return "";
}

std::string diagMessageEs(const DiagReport& r) {
	std::ostringstream ss;
	// Punto 8: el nombre base, no solo la ruta larga con espacios:
	// en el mensaje de error lo primero que se lee debe ser QUÉ
	// archivo falló, sin tener que descifrar una ruta larga.
	auto baseNameDiag = [](const std::string& p) {
		std::string f = upath::fromPath(upath::toPath(p).filename());
		return f.empty() ? p : f;
	};
	auto fileLine = [&](const char* label, const std::string& p, const FileProbe& f) {
		ss << label << ": " << p << " (" << prettyBytesDiag(f.size) << ")";
		if (f.has_ftyp && !f.ftyp.empty()) ss << " [ftyp=" << f.ftyp << "]";
		ss << (f.has_moov ? " [con índice moov]" : " [SIN índice moov]");
		ss << (f.has_mdat ? " [con datos de vídeo]" : " [sin datos de vídeo]");
		ss << "\n";
	};
	switch (r.code) {
		case DiagCode::Ok:
			ss << "Revisión previa correcta: la referencia tiene índice y el vídeo dañado tiene datos.\n"
			   << "Se puede intentar la reparación.\n";
			break;
		case DiagCode::RefMissing:
			ss << "No se encontró el vídeo de referencia.\n"
			   << "Archivo: " << r.ref_path << "\n"
			   << "Revisa la ruta o elige otro archivo.\n";
			break;
		case DiagCode::RefZeroLength:
			ss << "El vídeo de referencia está vacío (0 bytes).\n"
			   << "Archivo: " << r.ref_path << "\n"
			   << "Un archivo vacío no tiene índice que copiar. Elige otro vídeo sano.\n";
			break;
		case DiagCode::BadMissing:
			ss << "No se encontró el vídeo dañado.\n"
			   << "Archivo: " << r.bad_path << "\n";
			break;
		case DiagCode::BadZeroLength:
			ss << "El vídeo dañado está vacío (0 bytes).\n"
			   << "Archivo: " << r.bad_path << "\n"
			   << "No hay fotogramas que recuperar. Ningún programa puede reparar un archivo vacío.\n";
			break;
		case DiagCode::SameFile:
			ss << "La referencia y el vídeo dañado son el mismo archivo.\n"
			   << "Archivo: " << r.ref_path << "\n"
			   << "Tienes que elegir dos archivos distintos: uno sano y uno dañado.\n";
			break;
		case DiagCode::RefNoMoov:
			// Caso de Angel (src/mp4.cpp:167-168): la referencia
			// tampoco tiene moov, asi que no hay plantilla que copiar.
			ss << "El vídeo de referencia NO tiene índice (moov).\n";
			fileLine("Referencia", r.ref_path, r.ref);
			fileLine("Dañado    ", r.bad_path, r.bad);
			ss << "Sin índice en la referencia no hay plantilla que copiar, así que la reparación "
			      "no puede ni empezar. Esto NO significa que tu vídeo dañado esté perdido: "
			      "se esperaba que el dañado no tuviera índice.\n"
			   << "Lo más probable es que tu vídeo \"sano\" también esté dañado o venga de una "
			      "recuperación incompleta.\n"
			   << "Qué hacer: graba un vídeo nuevo de 10-15 segundos con el mismo celular o cámara, "
			      "con la misma resolución, deja que termine de guardar y úsalo como referencia. "
			      "Antes de reparar, comprueba que ese vídeo nuevo se reproduce completo en VLC.\n";
			break;
		case DiagCode::RefNoMoovSwappedLikely:
			ss << "Parece que los archivos están AL REVÉS.\n";
			fileLine("Como referencia", r.ref_path, r.ref);
			fileLine("Como dañado    ", r.bad_path, r.bad);
			ss << "El archivo que pusiste como referencia no tiene índice, pero el que pusiste "
			      "como dañado SÍ tiene índice y datos de vídeo. Eso es justo lo contrario de "
			      "lo que necesita Untrunc.\n"
			   << "Qué hacer: intercambia los dos archivos (el sano como referencia, el dañado "
			      "como vídeo a reparar) e inténtalo de nuevo.\n";
			break;
		case DiagCode::BadNoMdat:
			ss << "El vídeo dañado no tiene datos de vídeo (sin bloque mdat).\n";
			fileLine("Dañado", r.bad_path, r.bad);
			ss << "Sin datos no hay fotogramas que rescatar. Busca otra copia del archivo o "
			      "vuelve a recuperarlo de la tarjeta o el disco.\n";
			break;
		case DiagCode::RefNoFtyp:
			ss << "El archivo de referencia no parece un vídeo MP4/MOV (sin cabecera ftyp).\n"
			   << "Archivo: " << r.ref_path << "\n";
			if (r.ref.has_ftyp_deep)
				ss << "Ojo: sí se encontró una cabecera ftyp en el byte "
				   << r.ref.deep_ftyp_offset << ": la cabecera inicial de la referencia está "
				      "dañada, no es que no sea un vídeo. Prueba a convertir la referencia a "
				      "MP4 primero («Convertir a MP4») y usar el MP4 resultante.\n";
			ss << "Elige una referencia en formato MP4, MOV, M4V o 3GP.\n";
			break;
		case DiagCode::BadNoFtyp:
			// Punto 8: el mensaje dice CLARAMENTE qué archivo
			// falló (nombre base), qué se comprobó (ftyp/moov/
			// mdat al principio y en los primeros 16 MB, y si
			// hay datos H.264/H.265 sueltos) y qué hacer según
			// se reproduzca o no en VLC. Antes solo decía
			// "elige otro archivo dañado", como si el archivo
			// fuera inútil por definición.
			ss << "El archivo dañado «" << baseNameDiag(r.bad_path) << "» no parece un vídeo "
			      "MP4/MOV (sin cabecera ftyp).\n";
			fileLine("Dañado", r.bad_path, r.bad);
			ss << "Se buscaron las marcas ftyp, moov y mdat al principio del archivo y "
			      "también en los primeros 16 MB, y no se encontró ninguna";
			if (r.bad.has_nal)
				ss << ". Sí se vieron datos de vídeo H.264/H.265 sueltos, sin contenedor MP4";
			ss << ".\n"
			   << "Untrunc solo repara MP4, MOV, M4V y 3GP.\n"
			   << "Si este vídeo se reproduce en VLC, puede ser que VLC lo lea porque busca "
			      "la estructura en todo el archivo: en ese caso pulsa «Convertir a MP4» "
			      "primero (reconstruye la cabecera) o marca «Intentar de todos modos».\n"
			   << "Si no se reproduce en ningún reproductor, el archivo probablemente no "
			      "contiene vídeo recuperable (la recuperación devolvió datos equivocados): "
			      "busca otra copia del archivo o vuelve a recuperarlo.\n";
			break;
		case DiagCode::BadCabeceraDaniada:
			// Punto 8: el archivo SÍ es un MP4 (o lo fue), pero
			// la cabecera inicial está dañada o desplazada. Es
			// el caso de un vídeo "bueno" que el pre-vuelo
			// rechazaba: VLC lo reproduce porque escanea todo
			// el archivo, mientras el paseo secuencial de
			// átomos se detenía en el primer tamaño roto.
			ss << "El vídeo dañado «" << baseNameDiag(r.bad_path) << "» SÍ tiene estructura "
			      "MP4, pero la cabecera inicial está dañada.\n";
			fileLine("Dañado", r.bad_path, r.bad);
			ss << "No se encontró ftyp/moov/mdat al principio del archivo, pero sí más adelante";
			if (r.bad.has_ftyp_deep)
				ss << " (ftyp en el byte " << r.bad.deep_ftyp_offset << ")";
			ss << ". Por eso un reproductor como VLC puede reproducirlo (VLC busca la "
			      "estructura en todo el archivo) y aun así el pre-vuelo lo rechazaba.\n"
			   << "Qué hacer: pulsa «Convertir a MP4» primero — FFmpeg reconstruye la cabecera "
			      "y el MP4 resultante ya se puede reparar con la referencia de forma normal. "
			      "O marca «Intentar de todos modos» para probar el rescate sin referencia.\n";
			break;
		case DiagCode::RefFormatNoBmff:
			ss << "La referencia es " << r.ref.format_es << ", no MP4/MOV/M4V/3GP.\n"
			   << "Archivo: " << r.ref_path << "\n"
			   << "Untrunc solo repara contenedores MP4/MOV directamente. Este formato "
			      "sí se puede convertir a MP4 con FFmpeg, normalmente sin re-codificar "
			      "y sin perder calidad.\n"
			   << "Qué hacer: pulsa «Convertir a MP4» en la pestaña Reparar, o en la "
			      "terminal: untrunc --convertir-a-mp4 \"" << r.ref_path << "\". "
			      "Luego usa el MP4 resultante como referencia.\n";
			break;
		case DiagCode::BadFormatNoBmff:
			ss << "El vídeo dañado es " << r.bad.format_es << ", no MP4/MOV/M4V/3GP.\n"
			   << "Archivo: " << r.bad_path << "\n"
			   << "Untrunc NO repara este formato directamente: su núcleo reconstruye "
			      "el índice moov, que solo existe en MP4/MOV/M4V/3GP. Pero el vídeo "
			      "no está perdido: se puede convertir a MP4 y reparar el MP4 resultante.\n"
			   << "Qué hacer: pulsa «Convertir a MP4» en la pestaña Reparar, o en la "
			      "terminal: untrunc --convertir-a-mp4 \"" << r.bad_path << "\". "
			      "La conversión intenta primero no re-codificar (rápido, sin pérdida); "
			      "si los códecs no caben en MP4 (p. ej. VP9 de WebM o WMV2), avisa y "
			      "re-codifica a H.264 + AAC. Después repara el MP4 convertido con la "
			      "misma referencia.\n";
			break;
	}
	ss << "Acción sugerida: " << diagActionEs(r.code) << "\n";
	return ss.str();
}

std::string repairedPathInReparados(const std::string& corrupt_path) {
	// OJO: Untrunc original nombra la salida como
	// <nombre-completo>_fixed<ext>, es decir "truncado.mp4_fixed.mp4"
	// (getPathRepaired(): corrupt + "_fixed" + sufijo + extension).
	// Aqui se respeta ese nombre para no romper alreadyRepaired().
	fs::path p = upath::toPath(corrupt_path);
	fs::path parent = p.parent_path();
	if (parent.empty()) parent = ".";
	std::string filename = upath::fromPath(p.filename());
	std::string ext = upath::fromPath(p.extension());
	if (ext.empty()) ext = ".mp4";
	return upath::fromPath(parent / "Reparados" / upath::toPath(filename + "_fixed" + ext));
}

std::string ensureReparadosDir(const std::string& corrupt_path) {
	fs::path p = upath::toPath(corrupt_path);
	fs::path parent = p.parent_path();
	if (parent.empty()) parent = ".";
	fs::path dir = parent / "Reparados";
	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec) return "";
	return upath::fromPath(dir);
}
