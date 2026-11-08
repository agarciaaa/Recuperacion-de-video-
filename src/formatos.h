/*
	Untrunc-mejorado - formatos.h

	Deteccion de formato de contenedor por CONTENIDO (magic bytes),
	no solo por extension, y conversion honesta a MP4 con FFmpeg.

	Contexto: Untrunc solo repara contenedores ISO BMFF (MP4, MOV,
	M4V, 3GP) porque su nucleo reconstruye el indice moov. Angel
	pidio (2026-11-07) que el programa soporte varios formatos, no
	solo MP4. Este modulo NO finge que un MKV se repara directo:
	detecta el formato real, dice si es BMFF directo o si hay que
	convertir primero a MP4 (remux sin re-codificar cuando los
	codecs lo permiten, re-codificacion solo como ultimo recurso y
	avisando que pierde calidad y tarda).

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef FORMATOS_H
#define FORMATOS_H

#include <string>

// Contenedores que sabemos detectar por magic bytes.
enum class FormatKind {
	Bmff,      // MP4 / MOV / M4V / 3GP (ftyp): reparacion DIRECTA
	Mkv,       // Matroska (.mkv): convertir a MP4
	Webm,      // WebM (EBML + "webm"): convertir, casi siempre RE-codificando
	Avi,       // AVI (RIFF....AVI ): convertir a MP4
	Wmv,       // ASF/WMV (GUID 30 26 B2 75): convertir, casi siempre RE-codificando
	Flv,       // FLV: convertir a MP4
	Mpg,       // MPEG-PS (.mpg/.mpeg): convertir a MP4
	Mts,       // MPEG-TS (.mts/.m2ts/.ts): convertir a MP4
	Unknown    // no se reconoce: ni extension ni contenido
};

struct FormatInfo {
	FormatKind kind = FormatKind::Unknown;
	std::string name;        // "MP4", "MOV", "MKV", "AVI", ...
	std::string ftyp;        // marca BMFF (isom, qt, M4V , 3gp6...), vacia si no es BMFF
	bool is_bmff = false;    // reparacion directa con Untrunc
	// Conversion posible a MP4 (remux o re-codificacion). False
	// solo para Unknown: no sabemos ni que es.
	bool convertible = false;
};

// Detecta el formato de un archivo por su contenido (primeros
// 4 KB) y, si el contenido no dice nada, por su extension.
// Nunca modifica el archivo. Si no existe o esta vacio, kind
// es Unknown y name es "desconocido".
FormatInfo detectFormat(const std::string& path);

// Nombre legible del formato para mensajes en espanol, p. ej.
// "MP4", "MOV (QuickTime)", "MKV (Matroska)", "desconocido".
std::string formatNameEs(const FormatInfo& info);

// true si el archivo es un contenedor BMFF reparable directo
// (MP4, MOV, M4V, 3GP, segun su contenido, no su extension).
bool isDirectlyRepairable(const std::string& path);

// Resultado de una conversion a MP4 con FFmpeg.
struct ConvertResult {
	bool ok = false;
	std::string output;      // ruta del MP4 generado (vacia si fallo)
	bool recoded = false;    // true si hubo que RE-codificar (pierde calidad)
	std::string method_es;   // "sin re-codificar (remux)" o "re-codificando"
	std::string error_es;    // en espanol si ok == false
};

// Convierte un video a MP4 con FFmpeg, sin tocar el original.
// 1) Intenta remux sin re-codificar (-c copy): rapido y sin
//    perdida. Solo funciona si los codecs ya son compatibles
//    con MP4 (H.264/H.265/MPEG-4 + AAC/MP3/AC3).
// 2) Si el remux produce un MP4 con codecs incompatibles (p. ej.
//    VP9/Opus de WebM, WMV2 de WMV) o falla, y recode_ok es true,
//    re-codifica a H.264 + AAC (lento, pierde calidad) y lo dice
//    en method_es. Si recode_ok es false, falla con error claro.
// Si output esta vacio, se genera junto al original como
// <nombre>_convertido.mp4. Nunca sobrescribe la entrada.
ConvertResult convertToMp4(const std::string& input,
                           const std::string& output = "",
                           bool recode_ok = true);

// Nombre de salida por defecto para una conversion.
std::string convertedPathDefault(const std::string& input);

// Codecs de video/audio de un archivo (ffprobe). Vacio si no se
// pudo leer. Se usa para decidir si el remux sirve o hay que
// re-codificar: un MP4 con VP9 no lo repara Untrunc aunque tenga moov.
bool probeCodecs(const std::string& path, std::string& video_codec,
                 std::string& audio_codec);

#endif // FORMATOS_H
