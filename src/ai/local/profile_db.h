/*
	Untrunc-mejorado (Fase 2 hibrida) - profile_db.h

	Base local de perfiles de camaras (JSON en profiles/profiles.json).
	Un perfil dice: para esta camara, el video suele ser H.264/H.265,
	esta resolucion, estos fps y este audio. Con la huella del
	archivo danado (ftyp + marcadores + resolucion detectada del SPS)
	se elige el perfil que mas coincide y se usa como plantilla
	sintetica cuando no hay referencia sana.

	Si el JSON no existe o no se puede leer, se usan perfiles
	genericos embebidos en el codigo (degradacion con gracia) y se
	avisa. Nada de esto necesita internet.

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef AI_PROFILE_DB_H
#define AI_PROFILE_DB_H

#include <string>
#include <vector>

struct CameraProfile {
	std::string id;            // "iphone_h264_1080p30"
	std::string nombre_es;     // "iPhone (H.264, 1080p, 30 fps)"
	std::string codec;         // "avc1" o "hvc1"
	int width = 0;
	int height = 0;
	int fps = 0;
	std::string profile;       // "High", "Main", ...
	std::string ftyp;          // marca habitual, p. ej. "isom", "mp42"
	std::string markers;       // "gpmd", "XAVC", "CAEP", "" ...
	int audio_sample_rate = 48000;
	int audio_channels = 2;
};

// Carga profiles/profiles.json (busca junto al ejecutable, en el
// directorio de trabajo y en profiles/). Si no lo encuentra,
// devuelve los perfiles genericos embebidos.
std::vector<CameraProfile> loadProfiles(const std::string& json_path = "");

// Elige el mejor perfil para una huella. Devuelve el perfil y una
// confianza 0..1. Si no hay coincidencia suficiente, confidence < 0.5.
struct ProfileMatch {
	CameraProfile profile;
	double confidence = 0.0;
	std::string reason_es;
};

ProfileMatch matchProfile(const std::vector<CameraProfile>& profiles,
                          const std::string& ftyp,
                          const std::string& codec,
                          int width, int height, int fps,
                          const std::string& markers);

#endif // AI_PROFILE_DB_H
