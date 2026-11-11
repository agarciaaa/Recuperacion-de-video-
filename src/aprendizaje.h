/*
	Untrunc-mejorado - aprendizaje.h

	Aprendizaje continuo del motor: cada reparacion con una referencia
	real (no una plantilla) ensena algo al programa, sin preguntar.

	Que se aprende y de donde:
	  1) De la REFERENCIA (video sano de la misma camara): se dana en
	     memoria (los mismos 5 tipos de dano de resinc_ia.h) y se apunta
	     cada candidato de resincronizacion con su verdad exacta. Nada se
	     escribe en disco salvo numeros. Cada camara es un grupo, asi el
	     modelo se valida con camaras que no vio.
	  2) Del video DANADO: solo lo que el motor reconocio con seguridad
	     (fotogramas, tamano medio, claves, audio, resincronizaciones,
	     correcciones de sincro): contadores por camara.
	  3) Una plantilla automatica (auto_<camara>.plantilla, solo
	     cabecera, datos a cero) si esa camara aun no tenia, para poder
	     reparar la proxima vez sin referencia.
	  4) Cuando hay bastantes datos nuevos (o una camara nueva) se
	     reentrena el modelo de resincronizacion; el nuevo solo sustituye
	     al anterior si en camaras no vistas lo mejora sin aceptar
	     ningun falso (train_onnx.py --solo-si-mejora).

	Privacidad: nunca se copia ni se recorta un video; no se guardan
	rutas ni nombres de archivo. Solo numeros, la firma de la camara
	(codec, resolucion, marca) y un resumen del contenido de la
	referencia (para no aprender dos veces del mismo video).

	Carpeta: onnxTrainingDir()/aprendizaje/ (base.tsv, camaras.tsv,
	referencias.tsv, estado.txt).

	UNTRUNC_NO_APRENDER=1 lo desactiva (solo para pruebas y CI).

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef UNTRUNC_APRENDIZAJE_H
#define UNTRUNC_APRENDIZAJE_H

#include <cstdint>
#include <string>

namespace aprendizaje {

// Lo que el motor reconocio con seguridad en el video danado.
struct DatosSeguros {
	long fotogramas = 0;       // fotogramas de video recuperados
	double bytes_video = 0;    // suma de sus tamanos
	long claves = 0;           // fotogramas clave
	long audio = 0;            // muestras de audio recuperadas
	int resincs = 0;
	int sincros = 0;
	int64_t duracion_ms = 0;
};

struct Resultado {
	bool aprendido = false;    // se anadio algo a la base
	bool camara_nueva = false;
	bool referencia_repetida = false;
	long filas = 0;            // candidatos anadidos
	bool plantilla_creada = false;
	bool reentrenado = false;  // se lanzo el reentrenamiento
	bool modelo_cambiado = false;
	int camaras = 0;           // camaras en la base
	std::string resumen_es;    // una linea para el resumen de la reparacion
};

// Se llama tras una reparacion correcta con una referencia real.
Resultado aprenderDeReparacion(const std::string& ref_path, const std::string& firma,
                               const DatosSeguros& datos);

// Codigo de camara (31 bits) a partir de la firma.
int codigoCamara(const std::string& firma);

// Resumen en espanol de todo lo aprendido (--resumen-aprendizaje).
std::string resumenEs();

std::string carpeta();

// Solo para pruebas: otra carpeta ("" = la normal) y umbral de filas
// nuevas para reentrenar (<= 0 = el normal).
void usarCarpeta(const std::string& ruta);
void umbralReentreno(long filas);

} // namespace aprendizaje

#endif
