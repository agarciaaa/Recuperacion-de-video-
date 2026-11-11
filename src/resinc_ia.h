#ifndef RESINC_IA_H
#define RESINC_IA_H

// Modelo de resincronizacion (ver Mp4::tryResync y common.h).
//
// Tras una zona danada, un candidato se acepta si los 2 fotogramas
// siguientes encajan en cadena. Si encajan 0 o 1 (otra zona danada justo
// despues), la cadena no decide y antes se perdia ese fotograma. Este
// modelo decide SOLO esos casos, a partir de kResincNFeat caracteristicas
// numericas del candidato (nunca bytes del video).
//
// Datos de entrenamiento sin copiar videos: generarDatasetResinc lee cada
// video sano, lo dana EN MEMORIA mientras lo lee (file.h,
// g_dano_simulado), ejecuta la reparacion real sin escribir nada y anota
// cada candidato con sus caracteristicas y su etiqueta verdadera (si
// empieza donde empezaba un fotograma en el video sano). El TSV solo
// tiene numeros: indice del video, tipo de dano, semilla, etiqueta,
// cadena, codec y caracteristicas. Ni rutas ni bytes.
//
// El modelo (resinc_modelo.txt, lo escribe train_onnx.py entrenar-resinc)
// solo se usa si su validacion en videos no vistos dio 0 falsos aceptados
// ("activo 1"); si no, el nucleo trabaja como sin modelo.

#include <string>
#include <vector>

#include "common.h"

extern const char* const kResincNombres[kResincNFeat];

struct ResultadoResinc {
	int videos = 0, escenarios = 0, fallos = 0;
	long filas = 0, positivos = 0, negativos = 0;
	long filas_sin_cadena = 0, positivos_sin_cadena = 0;
	// Evaluacion: fotogramas de video recuperados que empiezan donde un
	// fotograma real (verdaderos) o no (falsos), sin y con modelo.
	long verdaderos_sin = 0, falsos_sin = 0, verdaderos_con = 0, falsos_con = 0;
	long decisiones_modelo = 0;
	std::string resumen_es;
};

// Dana en memoria cada video sano con varios escenarios y escribe los
// candidatos en salida_tsv. Devuelve 0 si escribio al menos una fila.
int generarDatasetResinc(const std::vector<std::string>& sanos, const std::string& salida_tsv,
                         int semillas, ResultadoResinc& r);

// Mismos escenarios (con otras semillas), reparando sin y con el modelo
// (ruta_modelo; "" = solo sin modelo). Cuenta fotogramas verdaderos y
// falsos recuperados. Ignora la marca "activo" del modelo.
int evaluarResinc(const std::vector<std::string>& sanos, const std::string& ruta_modelo,
                  int semillas, ResultadoResinc& r);

// Audio AAC: con los mismos escenarios (y uno sin dano), cuantos bloques
// de audio recuperables (no destruidos por el dano) se recuperan bien, y
// cuantos se recuperan en un sitio que no era un bloque de audio.
struct ResultadoAudio {
	struct PorTipo {
		long reparaciones = 0;
		long audio_posibles = 0, audio_bien = 0, audio_falsos = 0;
		long video_posibles = 0, video_bien = 0;
		// Perdidos (recuperables) entre el final de una zona danada y lo
		// primero que se volvio a reconocer despues; y bytes saltados ahi.
		long audio_antes_reenganche = 0, video_antes_reenganche = 0;
		long zonas = 0;
		double bytes_hasta_reenganche = 0;
		// Desfase audio-video al final del video reparado (ms): cuanto se
		// adelanto el audio respecto a la imagen por lo que se perdio.
		double desfase_abs_ms = 0, desfase_max_ms = 0;
	};
	int videos = 0, escenarios = 0, fallos = 0;
	PorTipo por_tipo[6];  // 0 = sin dano, 1.. = tipos de dano
	std::string resumen_es;
};
int evaluarAudio(const std::vector<std::string>& sanos, int semillas, ResultadoAudio& r);

// Carga un modelo y lo conecta al nucleo (g_resync_scorer/umbral). Con
// exigir_activo, rechaza un modelo sin "activo 1".
bool activarModeloResinc(const std::string& ruta, bool exigir_activo, std::string& why_es);
void desactivarModeloResinc();
// Donde se busca: env UNTRUNC_MODELO_RESINC, el entrenado
// (~/.config/untrunc/entrenamiento/resinc_modelo.txt) y el incluido
// (models/onnx_base/resinc_modelo.txt). "" si no hay.
std::string rutaModeloResincEnUso(const char* argv0 = nullptr);

#endif
