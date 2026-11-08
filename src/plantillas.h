/*
	Untrunc-mejorado - plantillas.h

	Plantillas de cabecera por modelo de camara para el modo sin
	referencia.

	Por que: el nucleo de Untrunc solo usa de la referencia su
	estructura (ftyp + moov: tablas stbl, SPS/PPS del avcC/hvcC,
	escalas de tiempo, marcas). No decodifica los datos del mdat de la
	referencia (comprobado: con el mdat de la referencia puesto a cero
	la recuperacion es identica). Una referencia generada con otro
	codificador (libx264) no sirve porque sus SPS/PPS y su estructura
	no son los de la camara; una plantilla con la cabecera REAL de la
	camara si.

	Una plantilla (*.plantilla) es un MP4 "vacio":
	  - ftyp tal cual;
	  - moov completo, con udta/meta/uuid/XMP convertidos en 'free'
	    (mismo tamano, contenido a cero: sin datos personales, GPS,
	    numero de serie...) y las fechas de mvhd/tkhd/mdhd a cero;
	  - del mdat (y de free/skip/wide y cualquier otro atomo de nivel
	    superior) solo la cabecera con su tamano original, para que los
	    desplazamientos del moov sigan siendo validos.
	No contiene ni imagen ni sonido: pesa unos KB y se puede compartir.

	Para usarla se "materializa": se escribe un MP4 temporal con el
	mdat relleno de ceros y se repara con el como referencia.

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef UNTRUNC_PLANTILLAS_H
#define UNTRUNC_PLANTILLAS_H

#include <cstdint>
#include <string>
#include <vector>

#include "repair_job.h"
#include "repair_options.h"

namespace plantillas {

extern const char* const kExtension;  // ".plantilla"

struct InfoPlantilla {
	bool ok = false;
	std::string error_es;
	std::string path;
	std::string id;            // nombre del archivo sin extension
	std::string brand;         // marca principal del ftyp ("isom", "qt  "...)
	std::string video_codec;   // fourcc del stsd de video ("avc1", "hvc1"...)
	std::vector<std::string> codecs;  // fourcc de todas las pistas
	int width = 0;
	int height = 0;
	uint64_t mdat_bytes = 0;   // tamano del mdat original (se rellena a cero)
	uint64_t file_bytes = 0;   // tamano de la plantilla

	std::string resumenEs() const;  // "avc1 1920x1080, ftyp isom, ..."
};

struct ResultadoCrear {
	bool ok = false;
	std::string error_es;
	std::string path;
	InfoPlantilla info;
	int atomos_limpiados = 0;  // udta/meta/uuid... vaciados
};

// Limite del mdat del video sano: la plantilla se materializa con el
// mdat a cero, asi que un video de horas obligaria a escribir GB
// temporales en cada intento. 10-15 s de video bastan.
static const uint64_t kMaxMdatBytes = 1ull * 1024 * 1024 * 1024;

// Crea <dest_dir>/<id>.plantilla a partir de un video SANO grabado con
// la misma camara y la misma configuracion (resolucion, fps, codec).
// dest_dir vacio = carpetaUsuario(). id vacio = nombre del video.
// No modifica el video sano.
ResultadoCrear crearPlantilla(const std::string& video_sano,
                              const std::string& dest_dir,
                              const std::string& id);

// Solo escribe la plantilla en `destino` (sin comprobarla con el
// nucleo ni aplicar el limite de tamano). Para pruebas y herramientas.
bool escribirPlantilla(const std::string& video_sano, const std::string& destino,
                       int& limpiados, std::string& err);

// Lee la informacion de una plantilla (o de un MP4 cualquiera).
InfoPlantilla leerPlantilla(const std::string& path);

// Escribe un MP4 utilizable como referencia a partir de la plantilla
// (mdat relleno de ceros). Devuelve false y err en espanol si falla.
bool materializarPlantilla(const std::string& plantilla,
                           const std::string& salida_mp4,
                           std::string& err);

// Extension que debe llevar el MP4 materializado (".mov" para
// QuickTime, ".mp4" para el resto): el nucleo la usa para la salida.
std::string extensionMaterializada(const InfoPlantilla& info);

// Carpeta de plantillas del usuario: %APPDATA%\untrunc\plantillas
// (Windows) o ~/.config/untrunc/plantillas.
std::string carpetaUsuario();

// Carpetas donde se buscan plantillas, en orden: la del usuario,
// <exe>/plantillas, <exe>/profiles/plantillas, profiles/plantillas.
std::vector<std::string> carpetasBusqueda(const char* argv0);

// Todas las plantillas validas encontradas (sin duplicar ids: gana la
// primera carpeta, la del usuario).
std::vector<InfoPlantilla> listarPlantillas(const char* argv0);

// "ruta/a/archivo.plantilla" o un id de listarPlantillas().
// Devuelve la ruta o "" si no existe.
std::string resolverPlantilla(const std::string& ruta_o_id, const char* argv0);

// Ordena candidatas para un video danado: primero la preferida (id
// exacto), luego las que coinciden en codec y resolucion detectados.
// w/h/codec pueden ser 0/"" si no se detectaron.
std::vector<InfoPlantilla> ordenarCandidatas(std::vector<InfoPlantilla> todas,
                                             const std::string& preferida,
                                             int width, int height,
                                             const std::string& codec);

struct ResultadoPlantillas {
	bool attempted = false;    // habia al menos una plantilla que probar
	bool ok = false;           // alguna recupero >= 1 s
	InfoPlantilla usada;       // la que dio el mejor resultado
	RepairOutcome repair;      // resultado de la mejor
	std::vector<std::string> probadas_es;  // una linea por plantilla probada
};

// Repara el danado probando las plantillas dadas (en orden, como
// mucho max_intentos) y se queda con la que recupere mas. La salida
// final va donde iria una reparacion normal (Reparados/ o -dst).
ResultadoPlantillas repararConPlantillas(const std::vector<InfoPlantilla>& candidatas,
                                         const std::string& danado,
                                         const RepairOptions& opts,
                                         size_t max_intentos = 3);

} // namespace plantillas

#endif
