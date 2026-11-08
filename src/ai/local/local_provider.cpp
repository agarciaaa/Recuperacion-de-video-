/*
	Untrunc-mejorado (Fase 2 hibrida) - local_provider.cpp
	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "local_provider.h"

#include <algorithm>

#include "../../diagnoser.h"
#include "frame_scanner.h"
#include "profile_db.h"
#include "sps_detector.h"

bool LocalProvider::isAvailable(std::string& why_not_es) const {
	// La IA local siempre esta disponible: son heuristicas y
	// parseo de bytes, no un modelo descargado. Si en el futuro
	// se anade un modelo ONNX opcional, aqui se comprobara su
	// presencia y se degradara a heuristicas si falta.
	(void)why_not_es;
	return true;
}

AiDiagnosis analyzeLocally(const std::string& path, const std::string& profile_override) {
	AiDiagnosis d;
	FileProbe probe = probeFile(path);
	d.file_exists = probe.exists;
	d.file_size = probe.size;
	d.ftyp = probe.ftyp;
	d.has_moov = probe.has_moov;
	d.has_mdat = probe.has_mdat;
	d.mdat_bytes = probe.mdat_bytes;

	FrameScan scan = scanFramesFromFile(path);
	d.nal_sps = scan.nal_sps;
	d.nal_pps = scan.nal_pps;
	d.nal_idr = scan.nal_idr;
	d.nal_slice = scan.nal_slice;
	d.nal_aud = scan.nal_aud;
	d.estimated_frames = scan.estimated_frames;
	d.frame_estimate_note_es = scan.note_es;
	for (int i = 0; i < 5; i++) d.hist_slice_kb[i] = scan.hist_slice_kb[i];

	SpsDetected sps = detectSpsFromFile(path);
	if (sps.found) {
		d.codec = sps.codec;
		d.width = sps.width;
		d.height = sps.height;
		d.fps_estimado = sps.fps;
		d.sps_hex = sps.sps_hex;
		d.pps_hex = sps.pps_hex;
		d.sps_valido = sps.valid;
	} else {
		// Sin SPS, el codec se deduce por los NAL vistos.
		d.codec = (scan.nal_slice > 0) ? "avc1?" : "desconocido";
	}
	// Marcadores de camara: se buscan en la cabecera ftyp y en
	// los primeros bytes del archivo (gpmd, XAVC, CAEP).
	if (d.ftyp == "XAVC") d.markers = "XAVC";
	else if (d.ftyp == "CAEP") d.markers = "CAEP";

	auto profiles = loadProfiles();
	ProfileMatch m;
	if (!profile_override.empty()) {
		for (const auto& p : profiles) {
			if (p.id == profile_override) {
				m.profile = p;
				m.confidence = 0.85;
				m.reason_es = "Perfil elegido por el usuario: '" + p.nombre_es + "'.";
				break;
			}
		}
		if (m.profile.id.empty()) {
			m.reason_es = "El perfil '" + profile_override + "' no existe; se ignora y se busca el mejor perfil.";
		}
	}
	if (m.profile.id.empty()) {
		m = matchProfile(profiles, d.ftyp, d.codec == "avc1?" ? "avc1" : d.codec,
		                 d.width, d.height, d.fps_estimado, d.markers);
	}
	d.profile_id = m.profile.id;
	d.profile_confidence = m.confidence;
	// Si el usuario eligio un perfil y no hay SPS (lo normal en MP4,
	// donde el SPS vive en el moov perdido), las dimensiones del
	// perfil son la unica fuente de ancho/alto/fps. Se marcan como
	// "de perfil" para que la sugerencia lo diga y no finja que se
	// detectaron en el video.
	if (!m.profile.id.empty() && !profile_override.empty() &&
	    d.width == 0 && m.profile.width > 0) {
		d.width = m.profile.width;
		d.height = m.profile.height;
		d.fps_estimado = m.profile.fps;
		d.dimensions_from_profile = true;
	}
	return d;
}

AiSuggestion LocalProvider::analyze(const DiagnosticPacket& packet,
                                    const AiDiagnosis& diagnosis) {
	(void)packet;  // la local trabaja con el diagnostico, no con el JSON
	AiSuggestion s;
	s.source = "local";
	s.profile_id = diagnosis.profile_id;
	s.codec = diagnosis.codec;
	s.width = diagnosis.width;
	s.height = diagnosis.height;
	s.fps = diagnosis.fps_estimado;
	s.use_step = true;
	s.use_dyn = true;
	if (diagnosis.dimensions_from_profile && diagnosis.width > 0) {
		s.mode = AiMode::Perfil;
		s.confidence = 0.8;
		s.reason_es = "IA local: sin SPS en el mdat (normal en MP4), se usan las "
		              "dimensiones del perfil elegido por el usuario '" +
		              diagnosis.profile_id + "' (" +
		              std::to_string(diagnosis.width) + "x" +
		              std::to_string(diagnosis.height) + " a " +
		              std::to_string(diagnosis.fps_estimado) + " fps). Si la "
		              "resolucion real era otra, el rescate puede fallar: elige "
		              "el perfil que corresponda a tu camara.";
	} else if (diagnosis.sps_valido && diagnosis.profile_confidence >= 0.6) {
		s.mode = AiMode::Perfil;
		s.confidence = std::min(0.9, 0.6 + diagnosis.profile_confidence * 0.3);
		s.reason_es = "IA local: SPS valido detectado (" +
		              std::to_string(diagnosis.width) + "x" +
		              std::to_string(diagnosis.height) + ") y perfil '" +
		              diagnosis.profile_id + "' con confianza " +
		              std::to_string((int)(diagnosis.profile_confidence * 100)) +
		              "%. Se puede intentar sin referencia, con una referencia sintetica "
		              "de esos mismos parametros.";
	} else if (diagnosis.sps_valido) {
		s.mode = AiMode::Rescate;
		s.confidence = 0.65;
		s.reason_es = "IA local: SPS valido pero sin perfil de camara que coincida bien. "
		              "Se intentara un rescate con los parametros detectados; el resultado "
		              "se marcara como parcial.";
	} else if (diagnosis.nal_slice > 0) {
		s.mode = AiMode::Rescate;
		s.confidence = 0.5;
		s.reason_es = "IA local: no se detecto un SPS valido, pero si hay " +
		              std::to_string(diagnosis.nal_slice) + " slices de video. "
		              "Sin SPS ni perfil no se conoce la resolucion: elige un perfil "
		              "con --perfil (p. ej. generico_h264_1080p30) o usa una referencia sana.";
		s.warning_es = "Sin SPS valido ni perfil, la resolucion es desconocida. "
		               "El rescate sin referencia necesita --perfil o una referencia sana "
		               "del mismo dispositivo.";
	} else {
		s.mode = AiMode::Clasico;
		s.confidence = 0.2;
		s.reason_es = "IA local: no se detectaron ni SPS ni slices de video en los datos "
		              "escaneados. Sin referencia sana no se puede reparar con seguridad.";
		s.warning_es = "Hace falta un video de referencia sano del mismo dispositivo, "
		               "o el archivo puede estar vacio o fragmentado.";
	}
	return s;
}

AiSuggestion LocalProvider::analyzeFile(const std::string& path,
                                        const std::string& profile_override) {
	AiDiagnosis d = analyzeLocally(path, profile_override);
	DiagnosticPacket p = buildDiagnosticPacket(d, "local");
	return analyze(p, d);
}
