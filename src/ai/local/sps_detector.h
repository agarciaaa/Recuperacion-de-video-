/*
	Untrunc-mejorado (Fase 2 hibrida) - sps_detector.h

	Detecta los parametros del codec (SPS/PPS/VPS) directamente del
	mdat en bruto, SIN referencia. Es la pieza que desbloquea el
	modo sin referencia: si se conoce el SPS, se conocen resolucion,
	perfil y log2_max_frame_num, y se puede construir una referencia
	sintetica con esos mismos parametros (ver moov_synthesizer).

	El parseo de SPS H.264 es propio (lector Exp-Golomb + eliminacion
	de bytes de prevencion de emulacion 00 00 03). No reutiliza
	SpsInfo::decode() para ancho/alto porque esa clase no los guarda:
	solo guarda log2_max_frame_num, poc_type y frame_mbs_only_flag.
	Aqui se extraen tambien ancho, alto y fps (VUI) cuando existen.
	Para HEVC solo se detecta presencia de VPS/SPS/PPS (el parseo
	completo de SPS HEVC queda como limitacion documentada).

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef AI_SPS_DETECTOR_H
#define AI_SPS_DETECTOR_H

#include <string>
#include <vector>

struct SpsDetected {
	bool found = false;
	std::string codec;      // "avc1" (H.264) o "hvc1" (H.265)
	int profile_idc = 0;
	int level_idc = 0;
	int width = 0;
	int height = 0;
	int fps = 0;            // 0 = sin info de temporizacion (VUI)
	int log2_max_frame_num = 0;
	int poc_type = 0;
	bool frame_mbs_only = true;
	std::string sps_hex;    // RBSP del SPS en hexadecimal
	std::string pps_hex;
	bool valid = false;     // ancho/alto/log2 dentro de rangos sanos
	std::string note_es;
};

// Busca SPS/PPS en un buffer de datos en bruto (normalmente el
// principio del mdat). Prueba primero NAL con prefijo de longitud
// (formato MP4/AVCC, el habitual) y luego Annex-B (00 00 00 01).
// Devuelve el PRIMER SPS valido que encuentra.
SpsDetected detectSpsFromRaw(const unsigned char* data, size_t len);

// Extrae el SPS de un archivo MP4/MOV truncado (localiza el mdat
// y escanea sus primeros max_scan_bytes). No modifica el archivo.
SpsDetected detectSpsFromFile(const std::string& path, size_t max_scan_bytes = 16 << 20);

#endif // AI_SPS_DETECTOR_H
