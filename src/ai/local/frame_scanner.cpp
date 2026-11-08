/*
	Untrunc-mejorado (Fase 2 hibrida) - frame_scanner.cpp
	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "frame_scanner.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

static uint32_t readBe32(const unsigned char* p) {
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
	       (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static void countSliceSize(FrameScan& s, uint32_t len) {
	int kb = (int)(len / 1024);
	int bucket = (kb < 1) ? 0 : (kb < 4) ? 1 : (kb < 16) ? 2 : (kb < 64) ? 3 : 4;
	s.hist_slice_kb[bucket]++;
}

FrameScan scanFramesFromFile(const std::string& path, size_t max_scan_bytes) {
	FrameScan s;
	std::error_code ec;
	uint64_t file_size = (uint64_t)fs::file_size(path, ec);
	if (ec || file_size == 0) {
		s.note_es = "Archivo vacio o inexistente.";
		return s;
	}
	std::ifstream f(path, std::ios::binary);
	if (!f) {
		s.note_es = "No se pudo abrir el archivo.";
		return s;
	}
	// Localizar el mdat.
	uint64_t offset = 0, mdat_off = 0, mdat_len = 0;
	int guard = 0;
	while (offset + 8 <= file_size && guard++ < 1000) {
		unsigned char hdr[16] = {0};
		f.seekg((std::streamoff)offset);
		f.read(reinterpret_cast<char*>(hdr), 8);
		if (f.gcount() < 8) break;
		uint32_t sz = readBe32(hdr);
		std::string type(reinterpret_cast<const char*>(hdr + 4), 4);
		uint64_t atom_size = sz, header = 8;
		if (sz == 1) {
			f.read(reinterpret_cast<char*>(hdr), 8);
			if (f.gcount() < 8) break;
			uint64_t v = 0;
			for (int i = 0; i < 8; i++) v = (v << 8) | hdr[i];
			atom_size = v; header = 16;
		} else if (sz == 0) {
			atom_size = file_size - offset;
		}
		if (atom_size < header) break;
		if (type == "mdat") { mdat_off = offset + header; mdat_len = atom_size - header; break; }
		offset += atom_size;
	}
	if (mdat_len == 0) {
		s.note_es = "No se encontro bloque mdat en el archivo.";
		return s;
	}
	s.mdat_found = true;
	s.mdat_bytes = mdat_len;
	size_t to_read = (size_t)std::min<uint64_t>(mdat_len, max_scan_bytes);
	std::vector<unsigned char> buf(to_read);
	f.seekg((std::streamoff)mdat_off);
	f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)to_read);
	buf.resize((size_t)f.gcount());
	s.scanned_bytes = buf.size();

	// Recorrido principal: NAL con prefijo de longitud (MP4/AVCC).
	size_t off = 0;
	int units = 0;
	bool length_walk_ok = false;
	while (off + 5 <= buf.size() && units < 2000000) {
		uint32_t len = readBe32(buf.data() + off);
		if (len == 0 || len > 16 * 1024 * 1024 || off + 4 + len > buf.size()) break;
		unsigned char hdr = buf[off + 4];
		// En AVCC el byte de cabecera de un NAL valido tiene
		// forbidden_zero_bit = 0 (bit 7 a 0). Si no, no es un NAL.
		if (hdr & 0x80) break;
		int h264 = hdr & 0x1F;
		int hevc = (hdr >> 1) & 0x3F;
		// Tipo conocido en H.264 o en HEVC. En un MP4 el mdat
		// mezcla muestras de video (NAL con prefijo de longitud)
		// con muestras de audio (AAC crudo, SIN prefijo): el
		// recorrido se rompe al llegar a la primera muestra de
		// audio y eso es normal, no un error.
		bool known = (h264 >= 1 && h264 <= 12) ||
		             (hevc == 0 || hevc == 1 || hevc == 19 || hevc == 20 ||
		              hevc == 32 || hevc == 33 || hevc == 34 || hevc == 35 || hevc == 39);
		if (!known) break;
		length_walk_ok = true;
		units++;
		s.length_prefixed_units++;
		if (h264 == 7) s.nal_sps++;
		else if (h264 == 8) s.nal_pps++;
		else if (h264 == 5) { s.nal_idr++; s.nal_slice++; countSliceSize(s, len); }
		else if (h264 == 1) { s.nal_slice++; countSliceSize(s, len); }
		else if (h264 == 9) s.nal_aud++;
		else if (h264 == 6) s.nal_sei++;
		else if (hevc == 32 || hevc == 33 || hevc == 34) { s.nal_sps++; }
		else if (hevc == 19 || hevc == 20) { s.nal_idr++; s.nal_slice++; countSliceSize(s, len); }
		else if (hevc == 0 || hevc == 1) { s.nal_slice++; countSliceSize(s, len); }
		else s.nal_other++;
		off += 4 + len;
	}
	// SIN fallback Annex-B para contar: buscar "00 00 01" dentro
	// de datos comprimidos da falsos positivos (los bytes de un
	// slice o de un frame AAC forman ese patron por azar) y los
	// conteos salian inflados. El recorrido por longitud es lo
	// unico fiable aqui: se detiene en la primera muestra de
	// audio, asi que los conteos son una COTA INFERIOR (lo que
	// habia antes del primer audio), no el total del archivo.
	// Estimacion de frames: slices contados (suponiendo 1 slice
	// por frame, lo habitual en x264 con un solo slice). Es una
	// cota inferior: el total real es mayor. Se dice asi en la
	// nota para no inventar un total.
	if (s.nal_aud > 0) {
		s.estimated_frames = s.nal_aud;
		s.note_es = "Frames estimados por delimitadores AUD (cota inferior: "
		            "el escaneo se detiene en la primera muestra de audio).";
	} else if (s.nal_slice > 0) {
		s.estimated_frames = s.nal_slice;
		s.note_es = "Frames estimados por slices, suponiendo 1 slice por frame "
		            "(COTA INFERIOR: el escaneo se detiene en la primera muestra "
		            "de audio; el total real es mayor).";
	} else {
		s.note_es = "No se encontraron slices de video en los datos escaneados.";
	}
	return s;
}
