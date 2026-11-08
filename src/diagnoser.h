/*
	Untrunc-mejorado (Fase 1) - diagnoser.h

	Pre-vuelo en espanol para Untrunc: inspecciona la referencia y el
	archivo danado ANTES de reparar, sin FFmpeg y sin tocar los
	archivos, y devuelve un codigo de error estructurado en vez de
	un string en ingles.

	Basado en anthwlock/untrunc (fork de ponchio/untrunc).
	Licencia: GNU GPL-2.0 (ver COPYING en la raiz del repositorio).
	Autores originales: Federico Ponchio, anthwlock y colaboradores.
	Mejoras Fase 1: diagnostico en espanol, deteccion de archivos
	intercambiados, salida a carpeta Reparados.
*/

#ifndef DIAGNOSER_H
#define DIAGNOSER_H

#include <cstdint>
#include <stdexcept>
#include <string>

// Codigos de error estructurados (Fase 1, 1.1).
// Ok = se puede intentar la reparacion. Cualquier otro codigo
// significa que hay que avisar al usuario en espanol y NO llamar
// a Mp4::parseOk() / Mp4::repair() con esos archivos.
enum class DiagCode {
	Ok,
	RefMissing,              // no existe el archivo de referencia
	RefZeroLength,           // referencia de 0 bytes
	BadMissing,              // no existe el archivo danado
	BadZeroLength,           // danado de 0 bytes
	SameFile,                // referencia y danado son el mismo archivo
	RefNoMoov,               // referencia sin indice moov (caso de Angel)
	RefNoMoovSwappedLikely, // referencia sin moov, pero el "danado" SI tiene moov: probablemente estan al reves
	BadNoMdat,               // danado sin datos de video (sin mdat)
	RefNoFtyp,               // referencia sin cabecera ftyp: no parece un MP4/MOV
	BadNoFtyp,               // danado sin cabecera ftyp: no parece un MP4/MOV
	BadCabeceraDaniada,      // danado CON estructura MP4, pero la cabecera inicial esta danada/desplazada (ftyp/moov/mdat no al principio)
	RefFormatNoBmff,         // referencia en formato conocido pero NO BMFF (MKV, AVI, WebM...): hay que convertir a MP4 primero
	BadFormatNoBmff          // danado en formato conocido pero NO BMFF: hay que convertir a MP4 primero
};

// Excepcion estructurada: lleva el codigo ademas del mensaje.
// En la GUI, logg(ET) lanza std::runtime_error; esta clase permite
// que la GUI distinga el caso y muestre el mensaje en espanol.
class DiagnoserError : public std::runtime_error {
public:
	DiagCode code;
	DiagnoserError(DiagCode c, const std::string& msg)
		: std::runtime_error(msg), code(c) {}
};

// Lo que se sabe de un archivo tras inspeccionar solo sus atomos
// de nivel superior y los primeros megabytes (sin decodificar).
struct FileProbe {
	bool exists = false;
	uint64_t size = 0;
	bool has_ftyp = false;
	std::string ftyp;          // marca, p. ej. "isom"
	bool has_moov = false;     // tiene indice
	bool has_mdat = false;     // tiene datos de video/audio
	uint64_t mdat_bytes = 0;   // tamano del contenido mdat (sin cabecera)
	bool has_nal = false;      // se vieron inicios de NAL H.264/H.265
	bool has_avc_sps = false;  // se vio un SPS (NAL tipo 7) de H.264
	// Busqueda profunda (punto 8, 2026-11-07): si el paseo
	// secuencial de atomos no encontro nada (p. ej. la cabecera
	// inicial esta danada y el primer tamano es basura), se busca
	// ftyp/moov/mdat en los primeros 16 MB aunque no esten al
	// principio. Un reproductor como VLC hace lo mismo: escanea
	// todo el archivo, por eso un video puede reproducirse en VLC
	// y aun asi fallar el pre-vuelo. Solo se rellena cuando el
	// paseo secuencial no encontro nada.
	bool has_ftyp_deep = false;
	bool has_moov_deep = false;
	bool has_mdat_deep = false;
	uint64_t deep_ftyp_offset = 0;  // donde se encontro el ftyp (0 = no se encontro)
	// Formato detectado por CONTENIDO (formatos.h), no solo por
	// extension: "MP4", "MOV (QuickTime)", "MKV (Matroska)", ...
	std::string format_es;
	bool is_bmff = false;      // true si es MP4/MOV/M4V/3GP por contenido
};

struct DiagReport {
	DiagCode code = DiagCode::Ok;
	FileProbe ref;
	FileProbe bad;
	std::string ref_path;
	std::string bad_path;
};

// Inspeccion ligera de un solo archivo (no lo modifica).
FileProbe probeFile(const std::string& path);

// Pre-vuelo: comprueba referencia y danado en orden y devuelve el
// primer problema encontrado (o Ok). No repara nada.
DiagReport preflightCheck(const std::string& ref_path, const std::string& bad_path);

// Nombre tecnico del codigo (para el registro/log en ingles).
std::string diagCodeName(DiagCode code);

// Mensaje completo en espanol: que paso, en que archivo y que hacer.
// Es lo que debe ver el usuario; el log tecnico en ingles se queda
// en el panel "Detalles" de la GUI / en la salida -v del CLI.
std::string diagMessageEs(const DiagReport& report);

// Accion corta sugerida (una linea), para botones o etiquetas.
std::string diagActionEs(DiagCode code);

// Ruta de salida Fase 1 (1.4): <carpeta del danado>/Reparados/
// <nombre-completo>_fixed.<ext>, p. ej. "truncado.mp4_fixed.mp4"
// (es el nombre que usa Untrunc original: corrupt + "_fixed" + ext).
// No toca el original.
std::string repairedPathInReparados(const std::string& corrupt_path);

// Crea la carpeta Reparados/ junto al archivo danado. Devuelve
// la ruta de la carpeta, o "" si no se pudo crear.
std::string ensureReparadosDir(const std::string& corrupt_path);

std::string prettyBytesDiag(uint64_t bytes);

#endif // DIAGNOSER_H
