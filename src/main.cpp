/*
	Untrunc - main.cpp

	Untrunc is GPL software; you can freely distribute,
	redistribute, modify & use under the terms of the GNU General
	Public License; either version 2 or its successor.

	Untrunc is distributed under the GPL "AS IS", without
	any warranty; without the implied warranty of merchantability
	or fitness for either an expressed or implied particular purpose.

	Please see the included GNU General Public License (GPL) for
	your rights and further details; see the file COPYING. If you
	cannot, write to the Free Software Foundation, 59 Temple Place
	Suite 330, Boston, MA 02111-1307, USA.  Or www.fsf.org

	Copyright 2010 Federico Ponchio

							*/

#include <iostream>
#include <string>
#include <vector>

#include "libavutil/ffversion.h"

#include "mp4.h"
#include "atom.h"
#include "common.h"
#include "diagnoser.h"
#include "plantillas.h"
#include "repair_options.h"
#include "formatos.h"
#include "repair_job.h"
#include "ai/ai_provider.h"
#include "ai/hybrid.h"
#include "ai/local/local_provider.h"
#include "ai/external/external_provider.h"
#include "ai/lmstudio/lmstudio_provider.h"
#include "ai/onnx/onnx_provider.h"
#include "resinc_ia.h"
#include "aprendizaje.h"
#include "tarjeta.h"

// Modos ONNX de entrenamiento (Fase 2.5): delegan en el script
// Python train_onnx.py, que es quien sabe crear el dataset y
// entrenar. Devuelve -1 si argv no trae ningún modo ONNX, o el
// código de salida del script en caso contrario.
static int runOnnxCliMode(int argc, char* argv[]) {
	std::string mode;
	std::vector<std::string> rest;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--crear-dataset-onnx" || a == "--entrenar-onnx" ||
		    a == "--probar-onnx" || a == "--restablecer-onnx" ||
		    a == "--verificar-privacidad-onnx" || a == "--anexar-dataset-onnx" ||
		    a == "--resumen-onnx" || a == "--borrar-base-onnx") {
			mode = a;
			for (int j = i + 1; j < argc; j++) rest.push_back(argv[j]);
			break;
		}
	}
	if (mode.empty()) return -1;
	std::string why;
	if (mode == "--verificar-privacidad-onnx") {
		std::string carpeta = onnxTrainingDir();
		for (size_t i = 0; i < rest.size(); i++) {
			if (rest[i] == "--carpeta" && i + 1 < rest.size()) carpeta = rest[++i];
		}
		int rc = runTrainScript({"verificar-privacidad", "--carpeta", carpeta},
		                        why, argv[0]);
		if (rc != 0) std::cerr << why << "\n";
		return rc == 0 ? 0 : 1;
	}
	if (mode == "--resumen-onnx") {
		// §12: muestra la base guardada sin cargar ningún vídeo.
		std::vector<std::string> args = {"resumen"};
		for (size_t i = 0; i < rest.size(); i++) {
			if ((rest[i] == "--carpeta" || rest[i] == "--dataset" ||
			     rest[i] == "--modelo") && i + 1 < rest.size()) {
				args.push_back(rest[i]);
				args.push_back(rest[++i]);
			}
		}
		int rc = runTrainScript(args, why, argv[0]);
		if (rc != 0 && rc != 2) std::cerr << why << "\n";
		return rc == 0 ? 0 : (rc == 2 ? 2 : 1);
	}
	if (mode == "--borrar-base-onnx") {
		// §12: exige confirmación explícita (--confirmar); sin
		// ella el script rechaza (rc=2) y no borra nada.
		std::string base = findBaseModelDir(argv[0]);
		std::string destino = onnxModelActivoDir();
		std::string dataset = onnxDatasetDir();
		bool confirmar = false;
		for (size_t i = 0; i < rest.size(); i++) {
			if (rest[i] == "--confirmar") confirmar = true;
			if (rest[i] == "--dataset" && i + 1 < rest.size()) dataset = rest[++i];
			if (rest[i] == "--destino" && i + 1 < rest.size()) destino = rest[++i];
			if (rest[i] == "--base" && i + 1 < rest.size()) base = rest[++i];
		}
		if (!confirmar) {
			std::cerr << "Borrar la base es destructivo: vuelve a llamar con "
			             "--confirmar. Borra el dataset guardado y el modelo "
			             "entrenado (los vídeos originales NO se tocan).\n";
			return 2;
		}
		std::vector<std::string> args = {"borrar-base", "--confirmar",
		                                 "--dataset", dataset,
		                                 "--destino", destino};
		if (!base.empty()) { args.push_back("--base"); args.push_back(base); }
		int rc = runTrainScript(args, why, argv[0]);
		if (rc != 0) std::cerr << why << "\n";
		return rc == 0 ? 0 : 1;
	}
	if (mode == "--anexar-dataset-onnx") {
		// §12: añade vídeos nuevos a la base guardada sin borrarla.
		// Uso: --anexar-dataset-onnx <salida> <bueno1> [bueno2...]
		//      [--corruptos <c1> ...]
		if (rest.empty()) {
			std::cerr << "Uso: untrunc --anexar-dataset-onnx <salida> <bueno1> "
			             "[bueno2...] [--corruptos <c1> ...]\n";
			return 2;
		}
		std::vector<std::string> args = {"anexar-dataset", "--salida", rest[0]};
		std::vector<std::string> buenos, corruptos;
		bool en_corruptos = false;
		for (size_t i = 1; i < rest.size(); i++) {
			if (rest[i] == "--corruptos") { en_corruptos = true; continue; }
			(en_corruptos ? corruptos : buenos).push_back(rest[i]);
		}
		if (!buenos.empty()) {
			args.push_back("--buenos");
			for (auto& b : buenos) args.push_back(b);
		}
		if (!corruptos.empty()) {
			args.push_back("--corruptos");
			for (auto& c : corruptos) args.push_back(c);
		}
		int rc = runTrainScript(args, why, argv[0]);
		if (rc != 0) std::cerr << why << "\n";
		return rc == 0 ? 0 : 1;
	}
	if (mode == "--restablecer-onnx") {
		std::string base = findBaseModelDir(argv[0]);
		std::string destino = onnxModelActivoDir();
		if (base.empty() || destino.empty()) {
			std::cerr << "No se encontró el modelo base incluido ni la carpeta de "
			        "entrenamiento. El modelo base viene en el ZIP (models/onnx_base).\n";
			return 1;
		}
		return runTrainScript({"restablecer", "--base", base, "--destino", destino},
		                      why, argv[0]) == 0 ? 0 : 1;
	}
	if (mode == "--crear-dataset-onnx") {
		if (rest.empty()) {
			std::cerr << "Uso: untrunc --crear-dataset-onnx <salida> <bueno1> [bueno2...] "
			        "[--corruptos <c1> ...]\n";
			return 2;
		}
		std::vector<std::string> args = {"crear-dataset", "--salida", rest[0]};
		std::vector<std::string> buenos, corruptos;
		bool en_corruptos = false;
		for (size_t i = 1; i < rest.size(); i++) {
			if (rest[i] == "--corruptos") { en_corruptos = true; continue; }
			(en_corruptos ? corruptos : buenos).push_back(rest[i]);
		}
		if (!buenos.empty()) {
			args.push_back("--buenos");
			for (auto& b : buenos) args.push_back(b);
		}
		if (!corruptos.empty()) {
			args.push_back("--corruptos");
			for (auto& c : corruptos) args.push_back(c);
		}
		int rc = runTrainScript(args, why, argv[0]);
		if (rc != 0) std::cerr << why << "\n";
		return rc == 0 ? 0 : 1;
	}
	if (mode == "--entrenar-onnx") {
		std::vector<std::string> args = {"entrenar"};
		for (size_t i = 0; i < rest.size(); i++) {
			if (rest[i] == "--anexar") {
				// §12: anexa los vídeos nuevos a la base guardada
				// en vez de crear el dataset desde cero.
				args.push_back("--anexar");
			} else if ((rest[i] == "--listas" || rest[i] == "--dataset" ||
			     rest[i] == "--salida" || rest[i] == "--motor") && i + 1 < rest.size()) {
				args.push_back(rest[i]);
				args.push_back(rest[++i]);
			} else if (rest[i] == "--buenos" || rest[i] == "--corruptos") {
				// Rutas directas (camino de la GUI): se pasan tal cual,
				// solo como argumentos en memoria, nunca se escriben.
				args.push_back(rest[i]);
				while (i + 1 < rest.size() &&
				       rest[i + 1].rfind("--", 0) != 0) {
					args.push_back(rest[++i]);
				}
			}
		}
		int rc = runTrainScript(args, why, argv[0]);
		if (rc != 0) std::cerr << why << "\n";
		if (rc == 0) {
			// Comprobacion final: que el programa pueda CARGAR el modelo
			// (no basta con que el script diga que lo guardo).
			std::string salida = onnxModelActivoDir();
			for (size_t i = 0; i + 1 < rest.size(); i++)
				if (rest[i] == "--salida") salida = rest[i + 1];
			std::string why_uso;
			if (onnxModelUsable(salida, why_uso)) {
				std::cout << "Comprobado: el programa carga el modelo nuevo y lo usará en "
				             "las próximas reparaciones (se guarda en " << salida << ").\n";
				if (salida == onnxModelActivoDir() && !loadAiConfig().onnx_enabled)
					std::cout << "Para usarlo siempre sin poner --usar-onnx, añade "
					             "onnx_enabled=1 a " << aiConfPath()
					          << " (la casilla de la pestaña Entrenar lo hace sola).\n";
			} else {
				std::cerr << "ATENCIÓN: el modelo no se puede cargar: " << why_uso << "\n";
				return 1;
			}
		}
		return rc == 0 ? 0 : 1;
	}
	// --probar-onnx
	std::vector<std::string> args = {"probar"};
	for (size_t i = 0; i < rest.size(); i++) {
		if ((rest[i] == "--modelo" || rest[i] == "--dataset") && i + 1 < rest.size()) {
			args.push_back(rest[i]);
			args.push_back(rest[++i]);
		}
	}
	int rc = runTrainScript(args, why, argv[0]);
	if (rc != 0) std::cerr << why << "\n";
	return rc == 0 ? 0 : 1;
}

using namespace std;

// Punto 4 (reparaciones 2026-11-07): si la reparación clásica
// falló SOLO porque el pre-vuelo dijo que el dañado no parece
// MP4/MOV (BadNoFtyp/BadNoMdat) y el usuario pidió --forzar, se
// intenta el rescate sin referencia en vez de rendirse. Devuelve
// el resultado clásico tal cual en cualquier otro caso. El resumen
// del rescate (si lo hubo) sale por rescue_summary_out.
static RepairOutcome maybeForcedRescue(const std::string& corrupt,
                                       const RepairOutcome& classic,
                                       bool forzar,
                                       const std::string& profile_override,
                                       bool ai_external, bool ai_lmstudio, bool ai_onnx,
                                       const std::string& onnx_model_override,
                                       const std::string& lmstudio_endpoint_override,
                                       const std::string& lmstudio_model_override,
                                       char* argv0,
                                       std::string& rescue_summary_out) {
	rescue_summary_out.clear();
	if (!forzar || classic.ok || classic.skipped) return classic;
	if (classic.code != DiagCode::BadNoFtyp && classic.code != DiagCode::BadNoMdat &&
	    classic.code != DiagCode::BadCabeceraDaniada)
		return classic;
	AiConfig cfg = loadAiConfig();
	cfg.external_enabled = ai_external;
	cfg.lmstudio_enabled = ai_lmstudio;
	cfg.onnx_enabled = cfg.onnx_enabled || ai_onnx;
	if (!onnx_model_override.empty()) cfg.onnx_model_path = onnx_model_override;
	if (!lmstudio_endpoint_override.empty()) cfg.lmstudio_endpoint = lmstudio_endpoint_override;
	if (!lmstudio_model_override.empty()) cfg.lmstudio_model = lmstudio_model_override;
	RescueOutcome r = forcedRescueAttempt(corrupt, cfg, profile_override, argv0);
	rescue_summary_out = r.summary_es;
	RepairOutcome o;
	if (r.ok) {
		o = r.repair;
	} else {
		o.ok = false;
		o.error_es = r.error_es;
		o.message_es = r.error_es;
	}
	o.code = classic.code;
	return o;
}

void usage() {
	cerr << "Usage: untrunc [options] <ok.mp4> [corrupt.mp4]\n"
	     << "\ngeneral options:\n"
	     << "-V  - version\n"
	     << "-n  - no interactive\n" // in Mp4::analyze()
	     << "\n"
	     << "repair options:\n"
	     << "-s  - step through unknown sequences\n"
	     << "-st <step_size> - used with '-s'\n"
	     << "-sv - stretches video to match audio duration (beta)\n"
	     << "-rsv-ben - RSV file recovery (Sony recording-in-progress files)\n"
	     << "-dw - don't write _fixed.mp4\n"
	     << "-dr - dump repaired tracks, implies '-dw'\n"
	     << "-k  - keep unknown sequences\n"
	     << "-sm  - search mdat, even if no mp4-structure found\n"
	     << "-dcc  - dont check if chunks are inside mdat\n"
	     << "-dyn  - use dynamic stats\n"
	     << "-range <A:B>  - raw data range\n"
	     << "-dst <dir|file>  - set destination\n"
	     << "-skip  - skip existing\n"
	     << "-noctts  - dont restore ctts\n"
	     << "-mp <bytes>  - set max partsize\n"
	     << "\n"
	     << "Fase 1 (mejoras en espanol, sin IA):\n"
	     << "--preflight - solo revisa la referencia y el danado y explica en espanol si se puede reparar\n"
	     << "--sin-verificar - no comprobar el video reparado al terminar (por defecto se\n"
	     << "              decodifica entero y se indica en que segundos hay errores)\n"
	     << "--dataset-resinc <salida.tsv> [--semillas N] <sano.mp4>... - datos para\n"
	     << "    entrenar la resincronizacion: dana los videos solo en memoria (no copia\n"
	     << "    ni escribe ningun video) y guarda solo numeros\n"
	     << "--evaluar-resinc <modelo.txt|-> [--semillas N] <sano.mp4>... - mide cuantos\n"
	     << "    fotogramas reales y falsos se recuperan sin y con el modelo\n"
	     << "--evaluar-audio [--semillas N] <sano.mp4>... - mide cuanto audio AAC se\n"
	     << "    recupera y el desfase audio-video tras danos simulados en memoria\n"
	     << "--rescatar-tarjeta <unidad|imagen> [referencia.mp4] [--plantilla nombre]\n"
	     << "    [-dst carpeta] [--solo-buscar] [--conservar-crudos] - busca los videos\n"
	     << "    directamente en una tarjeta SD (E:, /dev/sdb) o en una imagen de disco\n"
	     << "    (.img/.dd), SOLO EN LECTURA. Copia los completos y junta y repara los\n"
	     << "    incompletos o partidos en trozos (con la referencia, la plantilla o las\n"
	     << "    plantillas guardadas). El destino debe estar en otro disco.\n"
	     << "--resumen-aprendizaje - que ha aprendido el motor de cada camara (aprende\n"
	     << "    solo en cada reparacion con referencia: dana la referencia en memoria y\n"
	     << "    guarda solo numeros; ni videos ni rutas)\n"
	     << "--sin-sincro - no corregir el desfase entre sonido e imagen tras una zona\n"
	     << "    danada\n"
	     << "--sin-resinc - no intentar volver a encontrar el video tras una zona danada\n"
	     << "              (por defecto se hace; si hay un modelo validado, lo confirma)\n"
	     << "--verificar <video> - solo comprueba un video (reparado o no) y dice si hay\n"
	     << "              errores de imagen o sonido y en que segundos\n"
	     << "--convertir-a-mp4 <entrada> [salida] - convierte un video MKV, AVI, WMV,\n"
	     << "              WebM, FLV, MPG o MTS a MP4 con FFmpeg, para poder repararlo.\n"
	     << "              Primero intenta NO re-codificar (remux, rapido y sin perdida);\n"
	     << "              si los codecs no caben en MP4 (p. ej. VP9 de WebM o WMV2),\n"
	     << "              re-codifica a H.264 + AAC y lo avisa. Con --sin-recodificar\n"
	     << "              no re-codifica: falla con un mensaje claro si no se puede.\n"
	     << "              El original NUNCA se toca; la salida por defecto es\n"
	     << "              <nombre>_convertido.mp4 junto al original. Untrunc solo\n"
	     << "              repara MP4, MOV, M4V y 3GP directamente; los demas formatos\n"
	     << "              se reparan DESPUES de convertirlos, y eso se dice siempre.\n"
	     << "--lote <ref> <danado1> [danado2...] - repara varios videos con la misma referencia\n"
	     << "              (alias: -lote, --batch, -batch). La salida va a la carpeta Reparados/\n"
	     << "--sin-reintento - no reintentar automaticamente con -s cuando la reparacion se corta\n"
	     << "              Por defecto, si la reparacion se corta antes del 90%, se reintenta una\n"
	     << "              sola vez con -s -dyn y se queda con el resultado mas largo.\n"
	     << "--forzar - si el pre-vuelo rechaza el danado por no parecer MP4/MOV\n"
	     << "              (sin ftyp/moov/mdat), intentarlo de todos modos con el rescate\n"
	     << "              sin referencia (perfil/ONNX). Alias: --intentar-de-todos-modos.\n"
	     << "              El pre-vuelo NO se elimina: solo se intenta cuando tu lo pides,\n"
	     << "              sabiendo que probablemente no haya nada que recuperar. Si el\n"
	     << "              rescate tampoco encuentra video, lo dice claro y NO deja un\n"
	     << "              archivo vacio enganoso (como hacia el Untrunc original).\n"
	     << "\n"
	     << "Fase 2 (IA hibrida: local + externa, solo texto):\n"
	     << "--ia-local - usa la IA local (offline, sin red) para sugerir modo y perfil\n"
	     << "--ia-externa - consulta ADEMAS a la IA externa (opt-in, apagada por defecto).\n"
	     << "              Necesita UNTRUNC_AI_API_KEY y UNTRUNC_AI_ENDPOINT. Solo envia el\n"
	     << "              paquete de diagnostico en texto (<= 50 KB). NUNCA envia el video.\n"
	     << "--solo-diagnostico <danado> [referencia] - imprime el paquete de diagnostico\n"
	     << "              en texto y sale SIN enviar nada por red. Revisalo antes de activar\n"
	     << "              la IA externa.\n"
	     << "--perfil <nombre> - fuerza un perfil de camara (ver profiles/profiles.json),\n"
	     << "              p. ej. --perfil iphone_h264_1080p30\n"
	     << "--sin-referencia <danado> - rescate sin referencia. Primero prueba las\n"
	     << "              plantillas de camara guardadas (cabeceras reales, ver abajo); si\n"
	     << "              ninguna sirve, la IA local detecta los parametros (SPS,\n"
	     << "              resolucion, fps), genera una referencia sintetica con ffmpeg y\n"
	     << "              repara con ella. El video nunca sale de tu PC.\n"
	     << "\n"
	     << "Plantillas de camara (cabecera real de cada modelo, sin imagen ni sonido):\n"
	     << "--crear-plantilla <video_sano> [nombre] - guarda la cabecera de un video SANO\n"
	     << "              corto (10-15 s) de la misma camara y configuracion. Se quitan\n"
	     << "              fechas, GPS y metadatos personales. Pesa unos KB.\n"
	     << "--listar-plantillas - muestra las plantillas encontradas y donde buscarlas.\n"
	     << "--plantilla <nombre|archivo> <danado> - repara usando esa plantilla como\n"
	     << "              referencia (equivale a tener el video sano a mano).\n"
	     << "\n"
	     << "LM Studio (Qwen local, 100% en localhost, sin internet):\n"
	     << "--ia-lmstudio - consulta ADEMAS a Qwen en LM Studio (localhost:1234).\n"
	     << "              Qwen es un LLM de texto: solo recibe el paquete de diagnostico\n"
	     << "              en texto, NUNCA el video ni fotogramas. Sin API key necesaria.\n"
	     << "--lmstudio-endpoint <url> - base de LM Studio, por defecto http://localhost:1234\n"
	     << "--lmstudio-model <nombre> - modelo cargado en LM Studio (p. ej. qwen3-...).\n"
	     << "              Si no se pone, se autodetecta el primer Qwen con /v1/models.\n"
	     << "              Ver el nombre exacto en LM Studio, pestana Developer / Server.\n"
	     << "--lmstudio-listar - lista los modelos cargados en LM Studio y sale.\n"
	     << "              Si LM Studio no responde: abre LM Studio, carga Qwen y pulsa\n"
	     << "              Start Server en el puerto 1234.\n"
	     << "\n"
	     << "ONNX pequeno (Fase 2.5, pestana «Entrenar»): modelo entrenado en tu PC\n"
	     << "              con tus propios videos buenos y corruptos. Todo local, sin red.\n"
	     << "--usar-onnx - consulta ADEMAS al modelo ONNX entrenado (si esta activado\n"
	     << "              y hay un modelo valido). Apagado por defecto.\n"
	     << "--onnx-modelo <carpeta> - carpeta del modelo (modelo_activo o modelo_base).\n"
	     << "--crear-dataset-onnx <salida> <bueno1> [bueno2...] [--corruptos <c1> ...]\n"
	     << "              - crea el dataset (ventanas de 256 bytes + etiquetas) desde\n"
	     << "              tus videos. Los videos enteros NO se copian.\n"
	     << "--entrenar-onnx [--buenos <b1> ...] [--corruptos <c1> ...] [--dataset <dir>]\n"
	     << "              [--salida <dir>] [--motor auto|torch|numpy] - entrena el modelo\n"
	     << "              y exporta modelo.onnx. Sin PyTorch usa NumPy y lo dice. Si el modelo\n"
	     << "              no supera a la heuristica, NO reemplaza el modelo activo.\n"
	     << "              Ningun video se guarda: solo se extraen ventanas de 256 bytes\n"
	     << "              con su etiqueta, leyendo por streaming desde el original.\n"
	     << "--probar-onnx [--modelo <dir>] [--dataset <dir>] - evalua el modelo.\n"
	     << "--restablecer-onnx - vuelve al modelo base minimo incluido.\n"
	     << "--verificar-privacidad-onnx [--carpeta <dir>] - verifica que en la carpeta\n"
	     << "              de entrenamiento no haya ningun video ni rutas completas.\n"
	     << "              Base persistente e incremental (Fase 2.5, sec. 12): la base\n"
	     << "              que SE QUEDA es el dataset (ventanas de 256 bytes) + el modelo;\n"
	     << "              las listas de videos NO se guardan y se pierden al cerrar.\n"
	     << "--resumen-onnx [--carpeta <dir>] - muestra la base guardada (ejemplos,\n"
	     << "              por clase, videos que aportaron por nombre + hash, modelo\n"
	     << "              activo) SIN cargar ningun video. Al reabrir, la pestana\n"
	     << "              «Entrenar» muestra este mismo resumen.\n"
	     << "--anexar-dataset-onnx <salida> <bueno1> [bueno2...] [--corruptos <c1> ...]\n"
	     << "              - anade videos NUEVOS a la base guardada sin borrarla ni\n"
	     << "              recargar los anteriores; deduplica por hash de video y de\n"
	     << "              ventana. Luego entrena con --entrenar-onnx --anexar.\n"
	     << "--entrenar-onnx --anexar [--buenos ...] - anexa y re-entrena desde cero\n"
	     << "              sobre la base ampliada (mas estable que el fine-tuning).\n"
	     << "--borrar-base-onnx --confirmar - borra el dataset y el modelo entrenado\n"
	     << "              y vuelve al modelo base minimo. Sin --confirmar no borra\n"
	     << "              nada. Los videos originales NUNCA se tocan.\n"
	     << "              El entrenamiento necesita Python 3 + numpy; PyTorch CPU es\n"
	     << "              opcional: pip install torch --index-url https://download.pytorch.org/whl/cpu\n"
	     << "\n"
	     << "analyze options:\n"
	     << "-a  - analyze\n"
	     << "-i[t|a|s] - info [tracks|atoms|stats]\n"
	     << "-d  - dump samples\n"
	     << "-f  - find all atoms and check their lenghts\n"
	     << "-lsm - find all mdat,moov atoms\n"
	     << "-m <offset> - match/analyze file offset\n"
	     << "untrunc <ok.mp4> <ok.mp4> - report wrong values\n"
	     << "\n"
	     << "other options:\n"
	     << "-ms  - make streamable\n"
	     << "-sh  - shorten\n"
	     << "-u <mdat-file> <moov-file> - unite fragments\n"
	     << "\n"
	     << "logging options:\n"
	     << "-q  - quiet, only errors\n"
	     << "-w  - show hidden warnings\n"
	     << "-v  - verbose\n"
	     << "-vv - more verbose\n"
	     << "-do - don't omit potential noise\n";
	exit(-1);
}

void printVersion() {
	cout << g_version_str << '\n';
	exit(0);
}

void parseRange(const string& s) {
	auto pos = s.find(":");
	if (pos == string::npos)
		logg(ET, "use python slice notation\n");
	auto s1 = s.substr(0, pos);
	auto s2 = s.substr(pos+1);
	g_range_start = s1.size() ? stoll(s1) : 0;
	g_range_end = s2.size() ? stoll(s2) : numeric_limits<int64_t>::max();
}

int main(int argc, char *argv[]) {
	const int kExpectArg = -22;
	bool show_info = false;
	bool show_tracks = false;
	bool show_atoms = false;
	bool show_stats = false;
	bool analyze = false;
	bool find_atoms = false;
	bool dump_samples = false;
	bool analyze_offset = false;
	bool make_streamable = false;
	bool unite = false;
	bool listm = false;
	bool shorten = false;
	bool force_shorten = false;
	// Fase 1
	bool preflight = false;
	bool batch = false;
	bool allow_retry = true;
	// Reparaciones (2026-11-07, punto 4): --forzar / --intentar-de-todos-modos.
	bool forzar = false;
	// Fase 2 (IA hibrida)
	bool ai_local = false;
	bool ai_external = false;
	bool solo_diag = false;
	bool no_ref = false;
	// LM Studio (Qwen local)
	bool ai_lmstudio = false;
	bool lmstudio_listar = false;
	std::string lmstudio_endpoint_override;
	std::string lmstudio_model_override;
	// ONNX pequeño (Fase 2.5)
	bool ai_onnx = false;
	std::string onnx_model_override;
	int arg_onnx_modelo = -1;
	std::string profile_override;
	off_t arg_offset = -1;
	int arg_step = -1;
	int arg_range = -1;
	int arg_dst = -1;
	int arg_mp = -1;
	int arg_perfil = -1;
	int arg_plantilla = -1;
	std::string plantilla_arg;
	int arg_lmstudio_endpoint = -1;
	int arg_lmstudio_model = -1;

	argv_as_utf8(argc, argv);

	// Fase 3 §6 (defecto ONNX en el ZIP): guardar argv[0] para que
	// el proveedor ONNX encuentre models/onnx_base/ junto al
	// ejecutable también cuando se le llama sin argv0.
	setExeArgv0(argc > 0 ? argv[0] : nullptr);

	// Modos ONNX de entrenamiento: se resuelven antes que nada
	// (no necesitan vídeos de referencia/dañado como tal).
	{
		int rc_onnx = runOnnxCliMode(argc, argv);
		if (rc_onnx >= 0) return rc_onnx;
	}

	// Formatos (Fase 2.5): --convertir-a-mp4 se resuelve antes
	// que la reparacion, porque no necesita referencia.
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--convertir-a-mp4" || a == "--convertir") {
			if (i + 1 >= argc) {
				cerr << "Error: --convertir-a-mp4 necesita el archivo de entrada.\n"
				     << "Uso: untrunc --convertir-a-mp4 <entrada.mkv|avi|...> [salida.mp4]\n";
				return 1;
			}
			std::string entrada = argv[i + 1];
			std::string salida = (i + 2 < argc && argv[i + 2][0] != '-') ? argv[i + 2] : "";
			bool sin_recodificar = false;
			for (int j = i + 1; j < argc; j++)
				if (std::string(argv[j]) == "--sin-recodificar") sin_recodificar = true;
			FormatInfo fi = detectFormat(entrada);
			cout << "Formato detectado: " << formatNameEs(fi) << "\n";
			if (fi.is_bmff) {
				cout << "Este archivo ya es " << formatNameEs(fi) << ": no necesita "
				        "conversión, se puede reparar directamente.\n";
				return 0;
			}
			if (!fi.convertible) {
				cerr << "No se reconoce el formato de este archivo; no se puede convertir.\n";
				return 1;
			}
			ConvertResult r = convertToMp4(entrada, salida, !sin_recodificar);
			if (!r.ok) {
				cerr << "No se pudo convertir: " << r.error_es << "\n";
				return 1;
			}
			cout << "Convertido a MP4 " << r.method_es << ".\n"
			     << "Salida: " << r.output << "\n"
			     << "Ahora repara ese MP4 con tu referencia, por ejemplo:\n"
			     << "  untrunc <referencia.mp4> " << r.output << "\n";
			return 0;
		}
	}

	// Plantillas de camara: crear y listar no necesitan nada mas.
	for (int j = 1; j < argc; j++) {
		std::string a = argv[j];
		if (a == "--crear-plantilla") {
			if (j + 1 >= argc) {
				cerr << "Uso: untrunc --crear-plantilla <video_sano.mp4> [nombre]\n";
				return 1;
			}
			std::string nombre = (j + 2 < argc && argv[j + 2][0] != '-') ? argv[j + 2] : "";
			plantillas::ResultadoCrear r = plantillas::crearPlantilla(argv[j + 1], "", nombre);
			if (!r.ok) {
				cerr << r.error_es << "\n";
				return 1;
			}
			cout << "Plantilla creada: " << r.path << "\n"
			     << "  " << r.info.resumenEs() << "\n"
			     << "  Tamaño: " << r.info.file_bytes << " bytes (sin imagen ni sonido; "
			     << r.atomos_limpiados << " bloques de metadatos vaciados).\n"
			     << "Úsala con:\n"
			     << "  untrunc --plantilla " << r.info.id << " <dañado.mp4>\n"
			     << "o deja que --sin-referencia la elija sola.\n";
			return 0;
		}
		if (a == "--rescatar-tarjeta") {
			tarjeta::OpcionesRescate o;
			o.argv0 = argv[0];
			std::string origen;
			for (int k = j + 1; k < argc; k++) {
				std::string x = argv[k];
				if ((x == "-dst" || x == "--dst") && k + 1 < argc) o.destino = argv[++k];
				else if (x == "--plantilla" && k + 1 < argc) o.plantilla = argv[++k];
				else if (x == "--solo-buscar") o.solo_buscar = true;
				else if (x == "--sin-verificar") o.verificar = false;
				else if (x == "--conservar-crudos") o.conservar_crudos = true;
				else if (origen.empty()) origen = x;
				else if (o.referencia.empty()) o.referencia = x;
			}
			if (origen.empty()) {
				cerr << "Uso: untrunc --rescatar-tarjeta <unidad|imagen> [referencia.mp4] "
				        "[--plantilla nombre] [-dst carpeta] [--solo-buscar]\n";
				return 1;
			}
			int ultimo = -1;
			o.progreso = [&ultimo](int pct, const std::string& txt) {
				if (!txt.empty()) cerr << "\r" << txt << "            \n";
				else if (pct != ultimo) cerr << "\r  " << pct << " %   " << std::flush;
				ultimo = pct;
			};
			tarjeta::ResultadoRescate r = tarjeta::rescatar(origen, o);
			cerr << "\n";
			if (!r.ok) {
				cerr << r.error_es << "\n";
				return 1;
			}
			cout << r.resumen_es << "\n";
			return 0;
		}
		if (a == "--resumen-aprendizaje") {
			cout << aprendizaje::resumenEs();
			return 0;
		}
		if (a == "--evaluar-audio") {
			int semillas = 3;
			std::vector<std::string> sanos;
			for (int k = j + 1; k < argc; k++) {
				std::string x = argv[k];
				if (x == "--semillas" && k + 1 < argc) semillas = std::max(1, atoi(argv[++k]));
				else sanos.push_back(x);
			}
			if (sanos.empty()) {
				cerr << "Uso: untrunc --evaluar-audio [--semillas N] <sano.mp4>...\n";
				return 1;
			}
			ResultadoAudio r;
			int rc = evaluarAudio(sanos, semillas, r);
			cout << r.resumen_es;
			return rc;
		}
		if (a == "--dataset-resinc" || a == "--evaluar-resinc") {
			const bool dataset = a == "--dataset-resinc";
			int semillas = 3;
			std::vector<std::string> sanos;
			std::string destino;
			for (int k = j + 1; k < argc; k++) {
				std::string x = argv[k];
				if (x == "--semillas" && k + 1 < argc) semillas = std::max(1, atoi(argv[++k]));
				else if (destino.empty()) destino = x;
				else sanos.push_back(x);
			}
			if (destino.empty() || sanos.empty()) {
				cerr << "Uso: untrunc --dataset-resinc <salida.tsv> [--semillas N] <sano.mp4>...\n"
				     << "     untrunc --evaluar-resinc <modelo.txt|-> [--semillas N] <sano.mp4>...\n";
				return 1;
			}
			ResultadoResinc r;
			int rc = dataset ? generarDatasetResinc(sanos, destino, semillas, r)
			                 : evaluarResinc(sanos, destino == "-" ? "" : destino, semillas, r);
			cout << r.resumen_es << "\n";
			return rc;
		}
		if (a == "--verificar") {
			if (j + 1 >= argc) {
				cerr << "Uso: untrunc --verificar <video.mp4>\n";
				return 1;
			}
			cout << "Comprobando " << argv[j + 1] << " (se decodifica entero)...\n";
			InformeVerificacion v = verificarVideo(argv[j + 1]);
			cout << "Duración: " << formatoTiempo(v.duracion_s) << "\n" << v.resumenEs() << "\n";
			return v.limpio() ? 0 : (v.hecho ? 3 : 1);
		}
		if (a == "--listar-plantillas") {
			auto lista = plantillas::listarPlantillas(argv[0]);
			cout << "Carpetas donde se buscan plantillas (" << plantillas::kExtension << "):\n";
			for (const auto& d : plantillas::carpetasBusqueda(argv[0])) cout << "  " << d << "\n";
			if (lista.empty()) {
				cout << "No hay plantillas. Crea una con:\n"
				     << "  untrunc --crear-plantilla <video_sano.mp4> [nombre]\n";
				return 0;
			}
			cout << "Plantillas:\n";
			for (const auto& p : lista)
				cout << "  " << p.id << " - " << p.resumenEs() << "\n    " << p.path << "\n";
			return 0;
		}
	}

	int i = 1;
	for (; i < argc; i++) {
		string arg = argv[i];
		if (arg_offset == kExpectArg) {arg_offset = stoll(arg); continue;}
		if (arg_step == kExpectArg) {arg_step = stoi(arg); continue;}
		if (arg_range == kExpectArg) {parseRange(arg); arg_range = -1; continue;}
		if (arg_dst == kExpectArg) {g_dst_path = arg; arg_dst = -1; continue;}
		if (arg_mp == kExpectArg) {parseMaxPartsize(arg); arg_mp = -1; continue;}
		if (arg_perfil == kExpectArg) {profile_override = arg; arg_perfil = -1; continue;}
		if (arg_plantilla == kExpectArg) {plantilla_arg = arg; arg_plantilla = -1; continue;}
		if (arg_onnx_modelo == kExpectArg) {onnx_model_override = arg; arg_onnx_modelo = -1; continue;}
		if (arg_lmstudio_endpoint == kExpectArg) {lmstudio_endpoint_override = arg; arg_lmstudio_endpoint = -1; continue;}
		if (arg_lmstudio_model == kExpectArg) {lmstudio_model_override = arg; arg_lmstudio_model = -1; continue;}
		if (arg == "--version") printVersion();
		if (arg[0] == '-') {
			auto a = arg.substr(1);
			if      (a == "-preflight") preflight = true;
			else if (a == "lote" || a == "-lote" || a == "batch" || a == "-batch") batch = true;
			else if (a == "-sin-reintento") allow_retry = false;
			else if (a == "-forzar" || a == "-intentar-de-todos-modos") forzar = true;
			else if (a == "-ia-local") ai_local = true;
			else if (a == "-ia-externa") ai_external = true;
			else if (a == "-ia-lmstudio") ai_lmstudio = true;
			else if (a == "-usar-onnx") { ai_onnx = true; g_onnx_enabled = true; }
			else if (a == "-onnx-modelo") arg_onnx_modelo = kExpectArg;
			else if (a == "-lmstudio-listar") lmstudio_listar = true;
			else if (a == "-lmstudio-endpoint") arg_lmstudio_endpoint = kExpectArg;
			else if (a == "-lmstudio-model") arg_lmstudio_model = kExpectArg;
			else if (a == "-solo-diagnostico") solo_diag = true;
			else if (a == "-sin-referencia") no_ref = true;
			else if (a == "-perfil") arg_perfil = kExpectArg;
			else if (a == "-plantilla") arg_plantilla = kExpectArg;
			else if (a == "i") show_info = true;
			else if (a == "it") show_tracks = true;
			else if (a == "ia") show_atoms = true;
			else if (a == "is") show_stats = true;
			else if (a == "sm") g_search_mdat = true;
			else if (a == "sv") g_stretch_video = true;
			else if (a == "st") arg_step = kExpectArg;
			else if (a == "sh") shorten = true;
			else if (a == "fsh") force_shorten = shorten = true;
			else if (a == "lsm") listm = true;
			else if (a == "s") g_ignore_unknown = true;
			else if (a == "k") g_dont_exclude = true;
			else if (a == "a") analyze = true;
			else if (a == "V") printVersion();
			else if (a == "vv") g_log_mode = LogMode::VV;
			else if (a == "v") g_log_mode = LogMode::V;
			else if (a == "w") g_log_mode = LogMode::W2;
			else if (a == "q") g_log_mode = LogMode::E;
			else if (a == "noctts") g_no_ctts = true;
			else if (a == "n") g_interactive = false;
			else if (a == "f") find_atoms = true;
			else if (a == "do") g_dont_omit = true;
			else if (a == "dw") g_dont_write = true;
			else if (a == "dr") g_dump_repaired = true;
			else if (a == "d") {dump_samples = true; g_log_mode = LogMode::E;}
			else if (a == "m") {analyze_offset = true; arg_offset = kExpectArg; g_log_mode = LogMode::E;}
			else if (a == "ms") make_streamable = true;
			else if (a == "u") unite = true;
			else if (a == "dcc") g_ignore_out_of_bound_chunks = true;
			else if (a == "dyn") g_use_chunk_stats = true;
			else if (a == "rsv-ben") g_rsv_ben_mode = true;
			else if (a == "range") arg_range = kExpectArg;
			else if (a == "dst") arg_dst = kExpectArg;
			else if (a == "skip") g_skip_existing = true;
			else if (a == "-sin-verificar") g_verificar_salida = false;
			else if (a == "-sin-resinc") g_resync_enabled = false;
			else if (a == "-sin-sincro") g_sincro_enabled = false;
			else if (a == "mp") arg_mp = kExpectArg;
			else if (a == "dec") g_off_as_hex = false;
			else if (a == "fa") g_fast_assert = true;
			else if (arg.size() > 2) {cerr << "Error: seperate multiple options with space! See '-h'\n";  return -1;}
			else usage();
		}
		else if (argc > i+2 && !batch) usage();  // too many arguments (salvo --lote)
		else break;
	}
	// LM Studio: listar modelos cargados y salir (no necesita archivos).
	if (lmstudio_listar) {
		AiConfig cfg = loadAiConfig();
		cfg.lmstudio_enabled = true;
		if (!lmstudio_endpoint_override.empty()) cfg.lmstudio_endpoint = lmstudio_endpoint_override;
		if (!lmstudio_model_override.empty()) cfg.lmstudio_model = lmstudio_model_override;
		LMStudioProvider lm(cfg);
		std::vector<std::string> models;
		std::string why;
		if (!lm.listModels(models, why)) {
			cerr << "LM Studio: " << why << "\n";
			return 1;
		}
		cout << "Modelos cargados en LM Studio (" << lm.baseUrl() << "):\n";
		for (auto& m : models) cout << "  " << m << "\n";
		cout << "Usa uno con --lmstudio-model <nombre>.\n";
		return 0;
	}
	if (argc == i) usage();  // no filename given

	string ok = argv[i++], corrupt;
	if (i < argc) corrupt = argv[i++];

	// Fase 1 (1.4): lote con la misma referencia.
	if (batch) {
		std::vector<std::string> files;
		if (!corrupt.empty()) files.push_back(corrupt);
		while (i < argc) files.push_back(argv[i++]);
		if (files.empty()) {
			cerr << "Error: --lote necesita al menos un vídeo dañado.\n"
			     << "Uso: untrunc --lote <referencia.mp4> <dañado1.mp4> [dañado2.mp4 ...]\n";
			return 1;
		}
		auto results = repairBatch(ok, files, allow_retry);
		if (forzar) {
			for (auto& [path, outcome] : results) {
				std::string rs;
				outcome = maybeForcedRescue(path, outcome, forzar, profile_override,
				                            ai_external, ai_lmstudio, ai_onnx,
				                            onnx_model_override, lmstudio_endpoint_override,
				                            lmstudio_model_override, argv[0], rs);
				if (!rs.empty()) cout << rs << "\n";
			}
		}
		cout << batchSummaryEs(results);
		chkHiddenWarnings();
		int failed = 0;
		for (auto& [path, outcome] : results)
			if (!outcome.ok && !outcome.skipped) failed++;
		return failed == (int)results.size() ? 1 : 0;
	}

	// Fase 1 (1.1): solo pre-vuelo, sin reparar.
	if (preflight) {
		if (corrupt.empty()) {
			cerr << "Error: --preflight necesita referencia y vídeo dañado.\n"
			     << "Uso: untrunc --preflight <referencia.mp4> <dañado.mp4>\n";
			return 1;
		}
		auto report = preflightCheck(ok, corrupt);
		cout << diagMessageEs(report);
		cout << "Código técnico: " << diagCodeName(report.code) << "\n";
		return report.code == DiagCode::Ok ? 0 : 1;
	}

	// Fase 2: --solo-diagnostico: imprime el paquete de texto y
	// sale SIN enviar nada por red. Es la forma de revisar antes
	// de activar la IA externa que se enviaria exactamente.
	if (solo_diag) {
		AiConfig cfg0 = loadAiConfig();
		ExternalProvider ext0(cfg0);
		DiagnosticPacket pkt = ext0.buildPacketForFile(ok, corrupt);
		cout << pkt.json << "\n"
		     << "---\n"
		     << "Tamaño del paquete: " << pkt.bytes << " bytes (límite: 50 KB).\n"
		     << "NO se ha enviado nada por red. Esto es exactamente lo que se enviaría "
		        "a la IA externa si la activaras con --ia-externa.\n"
		     << "Nunca incluye el vídeo, fotogramas, audio ni rutas reales.\n";
		return pkt.valid ? 0 : 1;
	}

	// Reparar con una plantilla de camara concreta.
	if (!plantilla_arg.empty()) {
		if (!corrupt.empty()) {
			cerr << "Error: --plantilla solo necesita el vídeo dañado.\n"
			     << "Uso: untrunc --plantilla <nombre|archivo.plantilla> <dañado.mp4>\n";
			return 1;
		}
		std::string ruta = plantillas::resolverPlantilla(plantilla_arg, argv[0]);
		if (ruta.empty()) {
			cerr << "No se encontró la plantilla '" << plantilla_arg << "'. "
			     << "Mira las disponibles con --listar-plantillas.\n";
			return 1;
		}
		plantillas::InfoPlantilla info = plantillas::leerPlantilla(ruta);
		if (!info.ok) {
			cerr << "La plantilla " << ruta << " no es válida: " << info.error_es << "\n";
			return 1;
		}
		auto r = plantillas::repararConPlantillas({info}, ok, RepairOptions::fromGlobals(), 1);
		if (!r.ok) {
			cerr << "No se pudo recuperar nada útil con la plantilla '" << info.id << "' ("
			     << info.resumenEs() << ")."
			     << (r.repair.error_es.empty() ? "" : "\n" + r.repair.error_es) << "\n"
			     << "¿Es de la misma cámara y con la misma configuración (resolución, fps)?\n";
			chkHiddenWarnings();
			return 1;
		}
		cout << "Reparado con la plantilla '" << info.id << "' (" << info.resumenEs() << ").\n"
		     << "Recuperado: " << (int)r.repair.recovered_percent << "% del vídeo | Duración: "
		     << formatDurationEs(r.repair.duration_ms) << "\n"
		     << "Salida: " << r.repair.output_path << "\n"
		     << "El original no se ha modificado.\n";
		if (r.repair.verificacion.hecho) cout << r.repair.verificacion.resumenEs() << "\n";
		chkHiddenWarnings();
		return 0;
	}

	// Fase 2: rescate sin referencia (plantillas de camara -> IA
	// local + referencia sintetica -> reparacion clasica).
	if (no_ref) {
		if (!corrupt.empty()) {
			cerr << "Error: --sin-referencia solo necesita el vídeo dañado.\n"
			     << "Uso: untrunc --sin-referencia <dañado.mp4>\n";
			return 1;
		}
		AiConfig cfg = loadAiConfig();
		cfg.external_enabled = ai_external;  // solo si el usuario lo pidio
		cfg.lmstudio_enabled = ai_lmstudio;  // solo si el usuario lo pidio
		cfg.onnx_enabled = cfg.onnx_enabled || ai_onnx;  // --usar-onnx o activado en ai.conf
		if (!onnx_model_override.empty()) cfg.onnx_model_path = onnx_model_override;
		if (!lmstudio_endpoint_override.empty()) cfg.lmstudio_endpoint = lmstudio_endpoint_override;
		if (!lmstudio_model_override.empty()) cfg.lmstudio_model = lmstudio_model_override;
		// Igual que en el camino clasico: si se pidio IA externa sin
		// clave/endpoint, se falla rapido y claro SIN red ni reparacion.
		if (ai_external) {
			std::string why;
			ExternalProvider ext(cfg);
			if (!ext.isAvailable(why)) {
				cerr << "IA externa no disponible: " << why << "\n"
				     << "No se ha enviado nada por red y no se ha reparado nada.\n";
				return 1;
			}
		}
		// Lo mismo para LM Studio: si se pidio y no responde, se
		// falla rapido en espanol SIN reparar nada.
		if (ai_lmstudio) {
			std::string why;
			LMStudioProvider lm(cfg);
			if (!lm.isAvailable(why)) {
				cerr << "LM Studio no disponible: " << why << "\n"
				     << "No se ha enviado nada por red y no se ha reparado nada.\n";
				return 1;
			}
		}
		auto rescue = rescueWithoutReference(ok, cfg, profile_override, argv[0]);
		cout << rescue.summary_es << "\n";
		if (rescue.hybrid.onnx_consulted)
			cout << "Modelo ONNX consultado (local, entrenado en esta PC).\n";
		if (rescue.plantilla_usada.empty() && !rescue.synthetic_ref_path.empty())
			cout << "Referencia sintética temporal: " << rescue.synthetic_ref_path << "\n";
		chkHiddenWarnings();
		return rescue.ok ? 0 : 1;
	}

	bool ai_hint_set = false, ai_hint_s = false, ai_hint_dyn = false;
	std::string ai_hint_fuente;
	// Fase 2: asesoramiento de IA antes de la reparacion clasica.
	// Si el usuario pidio IA externa sin clave/endpoint, se falla
	// rapido y claro SIN hacer ninguna llamada de red.
	if ((ai_local || ai_external || ai_lmstudio || ai_onnx || !profile_override.empty()) && !corrupt.empty()) {
		AiConfig cfg = loadAiConfig();
		cfg.external_enabled = ai_external;
		cfg.lmstudio_enabled = ai_lmstudio;
		cfg.onnx_enabled = cfg.onnx_enabled || ai_onnx;
		if (!onnx_model_override.empty()) cfg.onnx_model_path = onnx_model_override;
		if (!lmstudio_endpoint_override.empty()) cfg.lmstudio_endpoint = lmstudio_endpoint_override;
		if (!lmstudio_model_override.empty()) cfg.lmstudio_model = lmstudio_model_override;
		if (ai_external) {
			std::string why;
			ExternalProvider ext(cfg);
			if (!ext.isAvailable(why)) {
				cerr << "IA externa no disponible: " << why << "\n"
				     << "No se ha enviado nada por red y no se ha reparado nada.\n";
				return 1;
			}
		}
		if (ai_lmstudio) {
			std::string why;
			LMStudioProvider lm(cfg);
			if (!lm.isAvailable(why)) {
				cerr << "LM Studio no disponible: " << why << "\n"
				     << "No se ha enviado nada por red y no se ha reparado nada.\n";
				return 1;
			}
		}
		auto hr = hybridAnalyze(corrupt, ok, cfg, profile_override, argv[0]);
		cout << hr.summary_es << "\n";
		// La sugerencia ya no se queda en texto: decide que estrategia
		// (-s / -dyn) se prueba primero en la reparacion de abajo.
		if (hr.suggestion.confidence >= 0.5) {
			ai_hint_set = true;
			ai_hint_s = hr.suggestion.use_step;
			ai_hint_dyn = hr.suggestion.use_dyn;
			ai_hint_fuente = hr.suggestion.source;
		}
		if (hr.onnx_consulted)
			cout << "Modelo ONNX consultado (local, entrenado en esta PC).\n";
		if (hr.lmstudio_consulted)
			cout << "LM Studio (Qwen local) consultado (solo texto de diagnóstico, en localhost).\n";
		if (hr.external_consulted)
			cout << "IA externa consultada (solo texto de diagnóstico).\n";
	}

	g_show_tracks = show_tracks || show_info;

	if (!g_ignore_unknown && arg_step > 0)
		logg(ET, "setting step_size without using '-s'\n");

	// Check -rsv-ben incompatibilities
	if (g_rsv_ben_mode) {
		if (g_ignore_unknown)
			logg(ET, "'-rsv-ben' is not compatible with '-s'\n");
		if (g_dont_exclude)
			logg(ET, "'-rsv-ben' is not compatible with '-k'\n");
		if (g_use_chunk_stats)
			logg(ET, "'-rsv-ben' is not compatible with '-dyn'\n");
	}

	bool skip_info = find_atoms;
	if (!skip_info) {
		logg(I, g_version_str, '\n');
	}

	auto chkC = [&]() {
		if (!corrupt.size())
			logg(ET, "no second file specified\n");
	};

	try {
		if (find_atoms) {Atom::findAtomNames(ok); return 0;}
		if (unite) {chkC(); Mp4::unite(ok, corrupt); return 0;}
		if (shorten) {Mp4::shorten(ok, corrupt.size() ? stoi(corrupt) : 500, force_shorten); return 0;}
		if (listm) {Mp4::listm(ok); return 0;}

		// Fase 1: ruta de reparacion con pre-vuelo, auto-reintento y
		// salida a Reparados/. Los modos de informacion/analisis
		// siguen por la ruta original de abajo.
		if (corrupt.size() && !show_tracks && !show_atoms && !show_stats &&
		    !show_info && !dump_samples && !analyze && !analyze_offset &&
		    !make_streamable) {
			RepairOptions run_opts = RepairOptions::fromGlobals();
			run_opts.hint_set = ai_hint_set;
			run_opts.hint_s = ai_hint_s;
			run_opts.hint_dyn = ai_hint_dyn;
			run_opts.hint_fuente = ai_hint_fuente;
			auto outcome = repairFileWithAutoRetry(ok, corrupt, run_opts, allow_retry);
			if (forzar) {
				std::string rs;
				outcome = maybeForcedRescue(corrupt, outcome, forzar, profile_override,
				                            ai_external, ai_lmstudio, ai_onnx,
				                            onnx_model_override, lmstudio_endpoint_override,
				                            lmstudio_model_override, argv[0], rs);
				if (!rs.empty()) cout << rs << "\n";
			}
			cout << repairSummaryEs(outcome, corrupt) << '\n';
			chkHiddenWarnings();
			return outcome.ok ? 0 : 1;
		}

		Mp4 mp4;
		g_mp4 = &mp4;  // singleton is overkill, this is good enough
		if (arg_step > 0) {
			logg(I, "using step_size=", arg_step, "\n");
			Mp4::step_ = arg_step;
		}

		auto ext = getMovExtension(ok);
		if (make_streamable) { mp4.makeStreamable(ok, ok + "_streamable" + ext); return 0; }
		if (mp4.alreadyRepaired(ok, corrupt)) return 0;

		logg(I, "reading ", ok, '\n');
		mp4.parseOk(ok, (show_atoms || show_info));

		if (show_tracks) mp4.printTracks();
		else if (show_atoms) mp4.printAtoms();
		else if (show_stats) { g_use_chunk_stats = true; mp4.printStats(); }
		else if (show_info) mp4.printMediaInfo();
		else if (dump_samples) mp4.dumpSamples();
		else if (analyze) mp4.analyze();
		else if (analyze_offset) mp4.analyzeOffset(corrupt.empty() ? ok : corrupt, arg_offset);
		else if (corrupt.size()) mp4.repair(corrupt);
	}
	catch (const char* e) {return cerr << e << '\n', 1;}
	catch (string e) {return cerr << e << '\n', 1;}
	catch (const std::exception& e) {return cerr << e.what() << '\n', 1;}

	chkHiddenWarnings();
	return 0;
}
