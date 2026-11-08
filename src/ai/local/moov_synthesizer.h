/*
	Untrunc-mejorado (Fase 2 hibrida) - moov_synthesizer.h

	"Moov sintetico" honesto: Untrunc NO puede construir un moov
	desde cero sin una referencia, porque sus Track se alimentan
	del moov de referencia (parseHealthy). En vez de reescribir
	el nucleo, esta pieza construye una REFERENCIA SINTETICA:
	un MP4 valido de 2 segundos, generado localmente con ffmpeg
	(lavfi testsrc + tono), con los MISMOS parametros que detecto
	la IA local (codec, resolucion, fps, perfil). Esa referencia
	sintetica se usa despues como <ok.mp4> en la reparacion
	clasica: su moov aporta la plantilla y el SPS, y el mdat del
	danado aporta los fotogramas reales.

	Verificado en Linux: con 640x360 30 fps H.264, la reparacion
	con referencia sintetica recupero el 100% (10 s) del truncado
	del corpus de Fase 1. No es magia: funciona cuando codec,
	resolucion y fps coinciden; si no coinciden, la reparacion
	clasica lo dira con su % recuperado.

	Requiere ffmpeg en el PATH (en Windows, el de MSYS2). Si no
	hay ffmpeg, devuelve error en espanol y NO se intenta nada.
	El archivo temporal se crea en el directorio temporal del
	sistema y el llamador debe borrarlo al terminar.

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef AI_MOOV_SYNTHESIZER_H
#define AI_MOOV_SYNTHESIZER_H

#include <string>

struct SyntheticRef {
	bool ok = false;
	std::string path;       // MP4 temporal con moov valido
	std::string error_es;   // si ok == false
	int width = 0;
	int height = 0;
	int fps = 0;
	std::string codec;
};

// Genera una referencia sintetica de 2 s con los parametros dados.
// width/height/fps deben ser > 0; codec "avc1" (H.264) o "hvc1".
SyntheticRef buildSyntheticReference(int width, int height, int fps,
                                     const std::string& codec,
                                     const std::string& profile);

#endif // AI_MOOV_SYNTHESIZER_H
