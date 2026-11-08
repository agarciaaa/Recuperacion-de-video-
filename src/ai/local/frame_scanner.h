/*
	Untrunc-mejorado (Fase 2 hibrida) - frame_scanner.h

	Escanea el mdat en bruto y cuenta unidades NAL sin decodificar
	video. Sirve para estimar cuantos frames hay y para el
	histograma de tamanos que viaja (como numeros) en el paquete
	de diagnostico.

	IMPORTANTE: en un MP4 el mdat guarda los NAL con PREFIJO DE
	LONGITUD (u32 big-endian + NAL), no con codigos Annex-B
	(00 00 00 01). Buscar solo Annex-B da falsos positivos dentro
	de los datos de audio y de los propios frames. Por eso el
	recorrido principal es por longitud, y Annex-B solo es un
	fallback con validacion (el NAL tiene que ser de un tipo
	conocido y su longitud, plausible).

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef AI_FRAME_SCANNER_H
#define AI_FRAME_SCANNER_H

#include <cstdint>
#include <string>

struct FrameScan {
	bool mdat_found = false;
	uint64_t mdat_bytes = 0;
	uint64_t scanned_bytes = 0;
	int nal_sps = 0;
	int nal_pps = 0;
	int nal_idr = 0;
	int nal_slice = 0;   // slices H.264 (tipo 1 y 5) + HEVC (0,1,19,20)
	int nal_aud = 0;
	int nal_sei = 0;
	int nal_other = 0;
	int length_prefixed_units = 0;  // NAL recorridos por longitud
	int annexb_units = 0;           // NAL recorridos por Annex-B
	int estimated_frames = 0;
	// Histograma de tamanos de slice en KB: [<1, 1-4, 4-16, 16-64, >64]
	int hist_slice_kb[5] = {0, 0, 0, 0, 0};
	std::string note_es;
};

// Escanea el mdat de un archivo (max_scan_bytes por defecto 64 MB).
FrameScan scanFramesFromFile(const std::string& path, size_t max_scan_bytes = 64 << 20);

#endif // AI_FRAME_SCANNER_H
