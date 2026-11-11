/*
	Untrunc-mejorado - tarjeta.h

	Rescate de videos directamente de una tarjeta SD, un pendrive o una
	imagen de disco (.img/.dd/.bin), sin pasar por un programa de
	"recuperar archivos borrados".

	Que hace:
	  1) Lee el origen SOLO EN LECTURA, en bloques grandes (nunca escribe
	     en la tarjeta ni en la imagen).
	  2) Busca el principio de cada video (caja 'ftyp' al principio de un
	     sector) y sigue su estructura (ftyp, moov, mdat...).
	  3) Video completo y seguido (moov + mdat dentro): se copia tal cual
	     al destino.
	  4) Video incompleto (grabacion cortada: sin moov o mdat abierto) o
	     partido en trozos alrededor de otros videos completos: se juntan
	     sus datos saltando los otros videos y se repara con la referencia,
	     la plantilla elegida o las plantillas guardadas (que el
	     aprendizaje crea solas por camara).

	El destino nunca puede estar en la tarjeta que se rescata.

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef UNTRUNC_TARJETA_H
#define UNTRUNC_TARJETA_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "repair_options.h"

namespace tarjeta {

// Origen de solo lectura: archivo de imagen o unidad (Windows "E:",
// "\\.\E:", "\\.\PhysicalDrive1"; Linux "/dev/sdb", "/dev/mmcblk0").
class Fuente {
public:
	Fuente();
	~Fuente();
	Fuente(const Fuente&) = delete;
	Fuente& operator=(const Fuente&) = delete;

	bool abrir(const std::string& ruta, std::string& err_es);
	uint64_t tam() const { return tam_; }
	bool esUnidad() const { return unidad_; }
	// Lee hasta n bytes desde off; devuelve los leidos (0 al final o si
	// falla la lectura: un sector ilegible se lee como ceros).
	size_t leer(uint64_t off, void* buf, size_t n);
	int sectoresIlegibles() const { return ilegibles_; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
	uint64_t tam_ = 0;
	bool unidad_ = false;
	int ilegibles_ = 0;
	uint64_t bloque_off_ = UINT64_MAX;
	size_t bloque_len_ = 0;
	bool cargar(uint64_t off_alineado);
};

// true si la ruta es una unidad (no un archivo de imagen).
bool esRutaUnidad(const std::string& ruta);

struct Tramo {
	uint64_t ini = 0, fin = 0;  // [ini, fin) en el origen
};

struct VideoEncontrado {
	uint64_t inicio = 0;      // offset del 'ftyp'
	uint64_t fin = 0;         // fin de lo que se rescata
	bool completo = false;    // moov + mdat enteros y seguidos
	bool tiene_moov = false;
	bool tiene_mdat = false;
	bool mdat_abierto = false;  // tamano 0 o se sale del origen
	std::string marca;        // marca del ftyp ("isom", "qt  "...)
	std::vector<Tramo> tramos;  // datos a juntar (sin otros videos)
	uint64_t bytes() const;
};

struct OpcionesRescate {
	std::string referencia;   // video sano de la misma camara (opcional)
	std::string plantilla;    // id o ruta de plantilla (opcional)
	std::string destino;      // carpeta de salida (obligatoria con unidades)
	bool solo_buscar = false; // listar sin escribir nada
	bool verificar = true;    // decodificar lo rescatado
	bool conservar_crudos = false;  // dejar los trozos juntados sin reparar
	const char* argv0 = nullptr;
	// Opciones para las reparaciones (por defecto, las del programa: un
	// RepairOptions vacio deja el rango -range a 0 y el nucleo aborta).
	RepairOptions reparar = RepairOptions::fromGlobals();
	// Progreso (0-100) y texto; puede ser nulo.
	std::function<void(int, const std::string&)> progreso;
};

struct VideoRescatado {
	VideoEncontrado v;
	bool ok = false;
	std::string salida;       // archivo final
	std::string estado_es;    // una linea
	int64_t duracion_ms = 0;
};

struct ResultadoRescate {
	bool ok = false;          // se pudo leer el origen
	std::string error_es;
	uint64_t bytes_origen = 0;
	int sectores_ilegibles = 0;
	std::vector<VideoEncontrado> encontrados;
	std::vector<VideoRescatado> rescatados;
	std::string resumen_es;
};

// Busca los videos del origen (no escribe nada).
std::vector<VideoEncontrado> buscarVideos(Fuente& f,
                                          const std::function<void(int)>& progreso = nullptr);

// Todo el rescate: buscar, copiar los completos y reparar el resto.
ResultadoRescate rescatar(const std::string& origen, const OpcionesRescate& o);

// Comprueba que el destino no esta en la unidad de origen.
bool destinoSeguro(const std::string& origen, const std::string& destino, std::string& err_es);

} // namespace tarjeta

#endif
