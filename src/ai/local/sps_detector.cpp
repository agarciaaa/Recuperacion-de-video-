/*
	Untrunc-mejorado (Fase 2 hibrida) - sps_detector.cpp
	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "sps_detector.h"
#include "../../upath.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "../ai_provider.h"

namespace {

// Lector de bits sobre un RBSP (sin bytes 00 00 03).
struct BitReader {
	const unsigned char* d = nullptr;
	size_t len = 0;
	size_t bitpos = 0;
	bool error = false;
	bool bit() {
		if (bitpos / 8 >= len) { error = true; return false; }
		bool b = (d[bitpos / 8] >> (7 - (bitpos % 8))) & 1;
		bitpos++;
		return b;
	}
	uint32_t bits(int n) {
		uint32_t v = 0;
		for (int i = 0; i < n; i++) v = (v << 1) | (bit() ? 1 : 0);
		return v;
	}
	uint32_t ue() {  // Exp-Golomb sin signo
		int zeros = 0;
		while (!bit()) {
			zeros++;
			if (zeros > 31 || error) { error = true; return 0; }
		}
		if (zeros == 0) return 0;
		return (uint32_t)((1u << zeros) - 1 + bits(zeros));
	}
	int32_t se() {  // Exp-Golomb con signo
		uint32_t v = ue();
		if (error) return 0;
		return (v & 1) ? (int32_t)((v + 1) / 2) : -(int32_t)(v / 2);
	}
};

static std::vector<unsigned char> removeEmulationPrevention(const unsigned char* d, size_t len) {
	std::vector<unsigned char> out;
	out.reserve(len);
	for (size_t i = 0; i < len; i++) {
		if (i + 2 < len && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 3) {
			out.push_back(0);
			out.push_back(0);
			i += 2;  // se salta el 03
		} else {
			out.push_back(d[i]);
		}
	}
	return out;
}

static void skipScalingList(BitReader& br, int size) {
	int last = 8, next = 8;
	for (int i = 0; i < size; i++) {
		if (next != 0) {
			int delta = br.se();
			next = (last + delta + 256) % 256;
		}
		last = (next == 0) ? last : next;
		if (br.error) return;
	}
}

// Parsea un SPS H.264 completo. `rbsp` es el NAL sin cabecera
// (ya sin el byte de tipo) y sin prevencion de emulacion.
static bool parseH264Sps(const std::vector<unsigned char>& rbsp, SpsDetected& out) {
	BitReader br;
	br.d = rbsp.data();
	br.len = rbsp.size();
	if (br.len < 4) return false;
	out.profile_idc = (int)br.bits(8);
	br.bits(8);  // constraint flags + reserved
	out.level_idc = (int)br.bits(8);
	br.ue();     // seq_parameter_set_id
	int chroma = 1;
	if (out.profile_idc == 100 || out.profile_idc == 110 || out.profile_idc == 122 ||
	    out.profile_idc == 244 || out.profile_idc == 44 || out.profile_idc == 83 ||
	    out.profile_idc == 86 || out.profile_idc == 118 || out.profile_idc == 128 ||
	    out.profile_idc == 138 || out.profile_idc == 139 || out.profile_idc == 134 ||
	    out.profile_idc == 135) {
		chroma = (int)br.ue();
		if (chroma == 3) br.bit();  // separate_colour_plane_flag
		br.ue();  // bit_depth_luma_minus8
		br.ue();  // bit_depth_chroma_minus8
		br.bit(); // qpprime_y_zero_transform_bypass_flag
		if (br.bit()) {  // seq_scaling_matrix_present_flag
			int n = (chroma != 3) ? 8 : 12;
			for (int i = 0; i < n; i++) {
				if (br.bit()) skipScalingList(br, i < 6 ? 16 : 64);
				if (br.error) return false;
			}
		}
	}
	int log2_minus4 = (int)br.ue();
	out.log2_max_frame_num = log2_minus4 + 4;
	out.poc_type = (int)br.ue();
	if (out.poc_type == 0) {
		br.ue();  // log2_max_pic_order_cnt_lsb_minus4
	} else if (out.poc_type == 1) {
		br.bit();
		br.se();
		br.se();
		int n = (int)br.ue();
		for (int i = 0; i < n; i++) br.se();
	} else if (out.poc_type != 2) {
		return false;
	}
	br.ue();  // max_num_ref_frames
	br.bit(); // gaps_in_frame_num_value_allowed_flag
	int mb_w = (int)br.ue() + 1;
	int mb_h = (int)br.ue() + 1;
	out.frame_mbs_only = br.bit();
	if (!out.frame_mbs_only) br.bit();  // mb_adaptive_frame_field_flag
	br.bit();  // direct_8x8_inference_flag
	int crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
	if (br.bit()) {  // frame_cropping_flag
		crop_l = (int)br.ue();
		crop_r = (int)br.ue();
		crop_t = (int)br.ue();
		crop_b = (int)br.ue();
	}
	if (br.error) return false;
	int sub_w = (chroma == 0) ? 1 : 2;
	int sub_h = (chroma == 0) ? 1 : (out.frame_mbs_only ? 2 : 1);
	out.width = mb_w * 16 - (crop_l + crop_r) * sub_w;
	out.height = mb_h * 16 * (out.frame_mbs_only ? 1 : 2) - (crop_t + crop_b) * sub_h;
	// VUI: solo interesa la temporizacion (fps).
	if (br.bit()) {  // vui_parameters_present_flag
		if (br.bit()) {  // aspect_ratio_info_present_flag
			int aspect = (int)br.bits(8);
			if (aspect == 255) { br.bits(16); br.bits(16); }
		}
		if (br.bit()) br.bit();  // overscan_info_present + flag
		if (br.bit()) {          // video_signal_type_present_flag
			br.bits(3);
			br.bit();
			if (br.bit()) { br.bits(8); br.bits(8); br.bits(8); }
		}
		if (br.bit()) {          // chroma_loc_info_present_flag
			br.ue(); br.ue();
		}
		if (br.bit()) {          // timing_info_present_flag
			uint32_t num_units = br.bits(32);
			uint32_t time_scale = br.bits(32);
			bool fixed = br.bit();
			if (num_units > 0 && time_scale > 0 && fixed) {
				double fps = (double)time_scale / (2.0 * (double)num_units);
				if (fps > 1 && fps < 240) out.fps = (int)std::lround(fps);
			}
		}
	}
	if (br.error) return false;
	out.valid = (out.width >= 16 && out.width <= 8192 &&
	             out.height >= 16 && out.height <= 8192 &&
	             out.log2_max_frame_num >= 0 && out.log2_max_frame_num <= 16 &&
	             out.poc_type >= 0 && out.poc_type <= 2 &&
	             // Perfil H.264 conocido: 66 Baseline, 77 Main, 88 Extended,
	             // 100 High, 110 High 10, 122 High 4:2:2, 244 High 4:4:4, etc.
	             // Sin esto, cualquier secuencia de bytes al azar "parsea"
	             // como un SPS con resolucion absurda (p. ej. 256x32).
	             (out.profile_idc == 66 || out.profile_idc == 77 || out.profile_idc == 88 ||
	              out.profile_idc == 100 || out.profile_idc == 110 || out.profile_idc == 122 ||
	              out.profile_idc == 244 || out.profile_idc == 44 || out.profile_idc == 83 ||
	              out.profile_idc == 86 || out.profile_idc == 118 || out.profile_idc == 128 ||
	              out.profile_idc == 138 || out.profile_idc == 139 || out.profile_idc == 134 ||
	              out.profile_idc == 135) &&
	             out.level_idc >= 10 && out.level_idc <= 62);
	return out.valid;
}

} // namespace

SpsDetected detectSpsFromRaw(const unsigned char* data, size_t len) {
	SpsDetected out;
	out.codec = "desconocido";
	if (!data || len < 8) return out;

	// 1) NAL con prefijo de longitud (formato MP4/AVCC, el habitual
	//    en el mdat de un MP4: u32 longitud + byte de tipo + datos).
	//    Se recorre secuencialmente; un SPS valido tiene que parsear.
	size_t off = 0;
	int guard = 0;
	while (off + 5 <= len && guard++ < 100000) {
		uint32_t nalu_len = ((uint32_t)data[off] << 24) | ((uint32_t)data[off + 1] << 16) |
		                    ((uint32_t)data[off + 2] << 8) | (uint32_t)data[off + 3];
		if (nalu_len == 0 || nalu_len > 16 * 1024 * 1024 || off + 4 + nalu_len > len) break;
		unsigned char hdr = data[off + 4];
		int h264_type = hdr & 0x1F;
		int hevc_type = (hdr >> 1) & 0x3F;
		if (h264_type == 7) {  // SPS H.264
			auto rbsp = removeEmulationPrevention(data + off + 5, nalu_len - 1);
			SpsDetected cand;
			cand.codec = "avc1";
			cand.sps_hex = toHexLower(data + off + 5, std::min<size_t>(nalu_len - 1, 64));
			if (parseH264Sps(rbsp, cand)) {
				cand.found = true;
				cand.note_es = "SPS H.264 detectado en el mdat (prefijo de longitud).";
				return cand;
			}
		}
		if (hevc_type == 33) {  // SPS HEVC (solo presencia; parseo completo pendiente)
			out.found = true;
			out.codec = "hvc1";
			out.sps_hex = toHexLower(data + off + 5, std::min<size_t>(nalu_len - 1, 64));
			out.note_es = "SPS HEVC detectado, pero su parseo completo no esta implementado: "
			              "resolucion no deducible del SPS en esta version.";
			return out;
		}
		off += 4 + nalu_len;
	}

	// 2) Fallback Annex-B (00 00 00 01): para flujos en bruto
	//    (OBS, camaras) donde no hay prefijo de longitud.
	for (size_t i = 0; i + 4 < len; i++) {
		size_t nal = 0;
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) nal = i + 4;
		else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) nal = i + 3;
		else continue;
		if (nal >= len) break;
		int h264_type = data[nal] & 0x1F;
		if (h264_type != 7) { i = nal; continue; }
		// Fin del NAL: siguiente codigo de inicio o fin del buffer.
		size_t end = len;
		for (size_t j = nal + 1; j + 3 < len; j++) {
			if (data[j] == 0 && data[j + 1] == 0 &&
			    (data[j + 2] == 1 || (data[j + 2] == 0 && data[j + 3] == 1))) { end = j; break; }
		}
		if (end - (nal + 1) < 4) { i = nal; continue; }
		auto rbsp = removeEmulationPrevention(data + nal + 1, end - (nal + 1));
		SpsDetected cand;
		cand.codec = "avc1";
		cand.sps_hex = toHexLower(data + nal + 1, std::min<size_t>(end - (nal + 1), 64));
		if (parseH264Sps(rbsp, cand)) {
			cand.found = true;
			cand.note_es = "SPS H.264 detectado en el mdat (Annex-B).";
			return cand;
		}
		i = nal;
	}
	out.note_es = "No se encontro un SPS valido en los datos escaneados. "
	              "En un MP4 normal esto es lo esperado: el SPS vive en el avcC del moov "
	              "(que es justo lo que se perdio), no en el mdat. Solo algunos "
	              "grabadores (ciertas camaras, OBS) repiten el SPS dentro del mdat.";
	return out;
}

SpsDetected detectSpsFromFile(const std::string& path, size_t max_scan_bytes) {
	SpsDetected out;
	std::ifstream f(upath::toPath(path), std::ios::binary);
	if (!f) return out;
	// Localizar el mdat (atomos de nivel superior) y escanear solo
	// su principio: ahi estan los primeros SPS/PPS de la grabacion.
	auto read32 = [](const unsigned char* p) -> uint32_t {
		return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
		       (uint32_t(p[2]) << 8) | uint32_t(p[3]);
	};
	std::vector<unsigned char> head(64);
	f.read(reinterpret_cast<char*>(head.data()), 64);
	size_t head_n = (size_t)f.gcount();
	// Recorrido simple de atomos para encontrar el mdat.
	std::ifstream g(upath::toPath(path), std::ios::binary);
	uint64_t offset = 0, mdat_off = 0, mdat_len = 0;
	std::error_code ec;
	uint64_t file_size = (uint64_t)std::filesystem::file_size(upath::toPath(path), ec);
	if (ec) return out;
	int guard = 0;
	while (offset + 8 <= file_size && guard++ < 1000) {
		unsigned char hdr[16] = {0};
		g.seekg((std::streamoff)offset);
		g.read(reinterpret_cast<char*>(hdr), 8);
		if (g.gcount() < 8) break;
		uint32_t sz = read32(hdr);
		std::string type(reinterpret_cast<const char*>(hdr + 4), 4);
		uint64_t atom_size = sz, header = 8;
		if (sz == 1) {
			g.read(reinterpret_cast<char*>(hdr), 8);
			if (g.gcount() < 8) break;
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
	(void)head; (void)head_n;
	if (mdat_len == 0) return out;
	size_t to_read = (size_t)std::min<uint64_t>(mdat_len, max_scan_bytes);
	std::vector<unsigned char> buf(to_read);
	g.seekg((std::streamoff)mdat_off);
	g.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)to_read);
	buf.resize((size_t)g.gcount());
	return detectSpsFromRaw(buf.data(), buf.size());
}
