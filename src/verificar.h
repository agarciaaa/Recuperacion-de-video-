/*
	Untrunc-mejorado - verificar.h

	Comprobacion del video reparado: lo decodifica entero (video y
	audio) con las mismas bibliotecas de FFmpeg que usa el programa y
	apunta en que segundos hay errores. Asi el resumen ya no dice solo
	"14 s, 99 %", sino tambien si esos segundos se ven bien:

	  "Se ve bien de principio a fin"  o
	  "Imagen con errores en 0:09-0:11 (3 fotogramas); el resto bien"

	No modifica el archivo. No necesita ffmpeg.exe (usa libavcodec).

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef UNTRUNC_VERIFICAR_H
#define UNTRUNC_VERIFICAR_H

#include <cstdint>
#include <string>
#include <vector>

struct TramoConErrores {
	double inicio_s = 0;
	double fin_s = 0;
	int errores = 0;
};

struct InformeVerificacion {
	bool hecho = false;          // se pudo abrir y decodificar
	std::string error_es;        // por que no se pudo (si !hecho)
	double duracion_s = 0;
	bool tiene_video = false;
	bool tiene_audio = false;
	int fotogramas = 0;          // fotogramas de video decodificados
	int errores_video = 0;       // paquetes/fotogramas de video con errores
	int muestras_audio = 0;      // paquetes de audio decodificados
	int errores_audio = 0;
	std::vector<TramoConErrores> tramos_video;  // zonas con errores de imagen
	std::vector<TramoConErrores> tramos_audio;  // zonas con errores de sonido
	bool truncado_en_limite = false;  // se paro en el limite de tiempo

	bool limpio() const { return hecho && errores_video == 0 && errores_audio == 0; }
	// Porcentaje de la duracion con errores (aprox., 0-100).
	double porcentajeConErrores() const;
	// Resumen en espanol, varias lineas, sin rutas.
	std::string resumenEs() const;
};

// Decodifica `path` y devuelve el informe. limite_s > 0 limita los
// segundos de video que se revisan (0 = todo).
InformeVerificacion verificarVideo(const std::string& path, double limite_s = 0);

// "1:05", "0:09", "1:02:03" (para los tramos).
std::string formatoTiempo(double segundos);

// Une instantes con errores en tramos: dos errores a menos de
// `separacion_s` quedan en el mismo tramo. Cada error ocupa al
// menos `duracion_min_s`. Expuesto para las pruebas.
std::vector<TramoConErrores> agruparErrores(std::vector<double> instantes,
                                            double separacion_s = 1.0,
                                            double duracion_min_s = 0.04);

#endif
