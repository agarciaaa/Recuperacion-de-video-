#ifndef GUI_H
#define GUI_H

#include <ui.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gui-helper.h"
#include "../mp4.h"

class Gui : public GuiHelper {
public:
	Gui() = delete;
	static void init();
	static void run();

	struct Repair {
		static uiEntry* entry_ok_;
		static uiEntry* entry_bad_;
		static uiMultilineEntry* text_entry_;
		static uiLabel* label_status_;
		static uiProgressBar* progressbar_;
		// Punto 8 (reparaciones 2026-11-07): "Intentar de todos
		// modos" también en la pestaña Reparar, no solo en el
		// Asistente (punto 4). Si el pre-vuelo rechaza el dañado
		// (BadNoFtyp/BadNoMdat/BadCabeceraDaniada) y está marcada,
		// se intenta el rescate sin referencia en vez de bloquear.
		static uiCheckbox* chk_forzar_;
		static void onProgress(int percentage);
		static void onStatus(const std::string& status);
	};

	struct Analyze {
		static uiEntry* entry_ok_;
		static uiMultilineEntry* text_entry_;
		static uiProgressBar* progressbar_;
		static void onProgress(int percentage);
	};

private:
	static uiWindow* window_;

	static uiMultilineEntry* current_text_entry_;

	static void build();
	static void openFile(uiButton* b, void* data);
	// Punto 10 (2026-11-07): diálogo para elegir UNO O VARIOS
	// vídeos a la vez. En Windows usa el diálogo nativo con
	// selección múltiple; en Linux/macOS usa uiOpenFile (un archivo).
	// Devuelve las rutas elegidas (vacío si se cancela).
	static std::vector<std::string> openVideoFilesMulti();
	static int onClosing(uiWindow* w, void* data);
	static int writeOutput(void* data);
	static uiEntry* addFileOpen(uiBox* box, const char* text);
	static void startRepair(uiButton* button, void* data);

	// Formatos (Fase 2.5): convierte el video danado a MP4 con
	// FFmpeg antes de repararlo, cuando no es MP4/MOV/M4V/3GP.
	// Corre en un hilo (la re-codificacion tarda) y al terminar
	// pone la ruta del MP4 en el campo del video danado.
	static void convertBadToMp4(uiButton* button, void* data);
	// Se ejecuta en el hilo principal a traves de ASYNC: pone en
	// el campo del video danado la ruta del MP4 convertido.
	static void setBadEntryQueued(void* data);

	// Pestaña "IA" (Fase 2 + LM Studio): un bloque por conexion, cada
	// uno con su checkbox de activado/desactivado y sus inputs.
	// Los controles viven en Gui::Ai para que los checkboxes puedan
	// habilitar/deshabilitar los inputs de su bloque (refresh()).
	struct Ai {
		// Bloque 1: IA local (offline, siempre disponible).
		static uiCheckbox* chk_local_;
		static uiCombobox* combo_profile_;
		// Bloque 2: LM Studio — Qwen local (localhost:1234).
		static uiCheckbox* chk_lmstudio_;
		static uiEntry* entry_lm_endpoint_;
		static uiEntry* entry_lm_model_;
		static uiSpinbox* spin_lm_timeout_;
		static uiButton* btn_lm_list_;
		static uiButton* btn_lm_test_;
		// Bloque 3: IA externa (por API, opt-in).
		static uiCheckbox* chk_external_;
		static uiEntry* entry_ext_endpoint_;
		static uiEntry* entry_ext_model_;
		static uiEntry* entry_ext_key_;
		static uiSpinbox* spin_ext_timeout_;
		static uiButton* btn_ext_test_;
		// Estado: que conexiones estan activas, en espanol.
		static uiLabel* label_status_;
		// Habilita/deshabilita los inputs de cada bloque segun su
		// checkbox y actualiza la etiqueta de estado.
		static void refresh();
		static void updateStatus();
	};

	static uiControl* aiTab();
	static uiControl* repairTab();
	static uiControl* analyzeTab();
	static uiControl* settingsTab();
	static uiControl* aboutTab();

	// Pestaña "Entrenar" (Fase 2.5, prototipo ONNX): el usuario
	// agrega vídeos buenos y corruptos de su propia PC; con ellos
	// se crea un dataset (ventanas de 256 bytes + etiqueta) y se
	// entrena el modelo ONNX pequeño, todo en local, sin subir nada.
	// PRIVACIDAD (2026-11-07, a petición de Angel): las listas de
	// vídeos viven SOLO EN MEMORIA mientras la ventana está abierta
	// y NUNCA se escriben a disco — antes se guardaban las rutas
	// completas en buenos.txt/corruptos.txt y eso se eliminó, porque
	// una ruta completa permite re-identificar el vídeo sin el
	// usuario. Al cerrar el programa las listas se pierden (hay que
	// volver a agregar los vídeos); el dataset y el modelo entrenado
	// sí quedan, porque solo contienen ventanas de 256 bytes y pesos.
	// Las rutas se pasan al script solo como argumentos en memoria.
	// BASE PERSISTENTE (§12, 2026-11-07, a petición de Angel): lo
	// que SE QUEDA al cerrar es el DATASET + el MODELO, no las
	// listas. Al reabrir, la pestaña muestra el resumen de la base
	// (updateBaseLabel/baseSummary, leído del manifiesto, nunca de
	// los vídeos) sin pedir nada. "Agregar más videos y re-entrenar"
	// anexa sin borrar; "Crear dataset desde cero" y "Borrar base"
	// piden confirmación en dos pasos porque borran la base guardada.
	struct Train {
		static uiMultilineEntry* list_buenos_;
		static uiMultilineEntry* list_corruptos_;
		static uiLabel* label_contadores_;
		static uiLabel* label_base_;
		static uiLabel* label_modelo_;
		static uiLabel* label_estado_;
		static uiProgressBar* progressbar_;
		static uiCheckbox* chk_usar_;
		static uiButton* btn_dataset_;
		static uiButton* btn_anexar_;
		static uiButton* btn_ver_base_;
		static uiButton* btn_train_;
		static uiButton* btn_test_;
		static uiButton* btn_borrar_;
		// Reparaciones (2026-11-07): selector del motor de
		// entrenamiento (punto 2) y ventana de detalles con la
		// salida real del script (punto 3). Ambos usan solo
		// controles libui que ya existen en esta pestaña.
		static uiCombobox* combo_motor_;
		static uiMultilineEntry* text_detalles_;
		// Confirmación en dos pasos para las acciones que borran
		// la base guardada (primer clic avisa, segundo ejecuta).
		static bool confirm_cero_armed_;
		static bool confirm_borrar_armed_;
		static std::vector<std::string> buenos_;
		static std::vector<std::string> corruptos_;
		static std::thread* thread_;
		// Refresca contadores, listas, resumen de la base,
		// etiqueta del modelo y qué botones están activos.
		static void refresh();
		static void updateModelLabel();
		static void updateLists();
		// Resumen en español de la base PERSISTENTE guardada
		// (dataset.npz + manifiesto + modelo): nº de ejemplos,
		// por clase, vídeos que aportaron (nombre base + hash
		// corto, nunca ruta) y fecha. Si no hay base, dice que
		// no hay ninguna todavía. Lee solo el manifiesto.
		static std::string baseSummary();
		static void updateBaseLabel();
		// NOTA: no hay saveLists()/loadLists(): a propósito, las
		// listas NO se persisten en disco (ver el comentario del
		// struct Train). Solo viven en buenos_/corruptos_ en memoria.
		static void addFile(bool bueno);
		// Ejecuta train_onnx.py en un hilo (el entrenamiento tarda
		// minutos) y al terminar actualiza la etiqueta de estado en
		// el hilo principal. done_msg_es: qué se hizo ("dataset",
		// "entrenamiento", "prueba").
		static void runScriptAsync(const std::vector<std::string>& args,
		                           const char* done_msg_es);
		static void onScriptFinished(void* data);
		// Devuelve el motor elegido en el combobox de la pestaña
		// Entrenar: "auto", "numpy" o "torch" (punto 2).
		static std::string motorSeleccionado();
	};

	static uiControl* trainTab();

	// Pestaña "Asistente" (Fase 3, 3.1): flujo guiado en 3 pasos
	// para reparar sin saber siglas ni linea de comandos.
	//   Paso 1: elige uno o varios videos danados (lote).
	//   Paso 2: elige referencia sana, o "sin referencia"
	//           (perfil / modelo ONNX entrenado / rescate), o
	//           convierte primero a MP4 si el formato no es BMFF.
	//   Paso 3: repara el lote y muestra vista previa completa.
	// NO es un reemplazo de la pestana "Reparar": esa sigue
	// existiendo, tal cual, como ruta manual/avanzada de un solo
	// archivo. El asistente es la entrada recomendada; "Reparar"
	// queda para quien ya sabe lo que hace. No se renombra ni se
	// reordena nada de lo anterior (menos disruptivo).
	// Sin siglas en el flujo: -s/-dyn/-sv quedan solo en Ajustes.
	// La salida es siempre a Reparados/ con el original intacto
	// (repairFileWithAutoRetry, Fase 1). La vista previa NO usa
	// un widget de imagen: libui no tiene control de imagen en
	// esta version, asi que el primer fotograma se extrae con
	// ffmpeg a Reparados/ y se abre con el visor del sistema, y
	// los primeros 5 segundos se reproducen con ffplay si esta
	// disponible; si no, "Abrir video reparado" usa el
	// reproductor por defecto del sistema.
	struct Assistant {
		static uiMultilineEntry* list_danados_;
		static uiLabel* label_contadores_;
		static uiLabel* label_formatos_;
		static uiCombobox* combo_modo_;
		static uiEntry* entry_ref_;
		static uiButton* btn_ref_examinar_;
		static uiCombobox* combo_perfil_;
		static uiButton* btn_convertir_;
		static uiButton* btn_reparar_todo_;
		// Reparaciones (2026-11-07, punto 4): "Intentar de todos
		// modos" — si el pre-vuelo rechaza un dañado por no parecer
		// MP4/MOV, se salta el pre-vuelo y se intenta el rescate
		// sin referencia (perfil/ONNX) en vez de bloquear.
		static uiCheckbox* chk_forzar_;
		static uiLabel* label_estado_;
		static uiProgressBar* progressbar_;
		static uiMultilineEntry* text_informe_;
		static uiLabel* label_preview_target_;
		static uiLabel* label_preview_;
		static uiButton* btn_preview_prev_;
		static uiButton* btn_preview_next_;
		static uiButton* btn_preview_5s_;
		static uiButton* btn_preview_foto_;
		static uiButton* btn_preview_abrir_;
		// Lista de danados (rutas completas, en memoria) y
		// salidas reparadas del ultimo lote, en el mismo orden
		// que la lista (solo las que terminaron OK).
		static std::vector<std::string> danados_;
		static std::vector<std::string> salidas_;
		static int preview_index_;  // cual de salidas_ se previsualiza
		// Hilo del lote: se reutiliza Gui::thread_ (el mismo que
		// usa startRepair y que onClosing() espera al cerrar), no
		// uno propio, para no dejar un hilo sin esperar al salir.
		// Progreso real por archivo: total_ / actual_ los pone el
		// hilo del lote; onProgress() (via g_onProgress, que el
		// nucleo llama con outProgress durante parseo y reparacion)
		// mapea el % intra-archivo al % global del lote.
		static int total_;
		static int actual_;
		static int last_global_;
		static void refresh();
		static void updateList();
		static void updateFormatsLabel();
		static void updatePreviewTarget();
		static void onProgress(int percentage);
		static void showStatusQueued(void* data);
		static void showReportQueued(void* data);
		static void showPreviewQueued(void* data);
		static void setConvertedQueued(void* data);
		static void addFile();
		static void convertNonBmff(uiButton* button, void* data);
		static void startBatch(uiButton* button, void* data);
		static void previewPrev(uiButton* button, void* data);
		static void previewNext(uiButton* button, void* data);
		static void previewFirstSeconds(uiButton* button, void* data);
		static void previewFirstFrame(uiButton* button, void* data);
		static void openRepaired(uiButton* button, void* data);
	};

	static uiControl* assistantTab();

	static void msgBox(const char* text);
	static void msgBoxError(const std::string& text, bool async=false);
	static void msgBoxErrorSync(const std::string& text);
	static void msgBoxErrorQueued(const std::string* s);
	// Cuadro informativo en el hilo principal (ASYNC), para
	// mensajes que no son errores (p. ej. conversion terminada).
	static void msgBoxQueued(void* data);
	// Fase 1 (1.5/1.6): muestra el resumen en espanol en la etiqueta
	// de estado (se llama desde el hilo principal con ASYNC).
	static void showSummaryQueued(void* data);
	static void printToEntry(const char* text);

	static std::thread* thread_;

	// Punto 9 (reparaciones 2026-11-07): descripcion en espanol
	// de lo que esta haciendo el hilo compartido thread_ ("el lote
	// del Asistente", "la reparación de la pestaña Reparar", "la
	// búsqueda de átomos de la pestaña Analizar"), para que el
	// aviso de "ya hay una reparación en marcha" diga QUE esta en
	// marcha. Se pone al crear el hilo y se vacia en
	// clearThreadQueued().
	static std::string thread_desc_;
	// Punto unico de limpieza del hilo compartido thread_: se
	// ejecuta en el hilo principal a traves de ASYNC, espera a que
	// el hilo termine, lo borra y deja el puntero a nullptr.
	// TODOS los trabajos que usan thread_ (lote del Asistente,
	// reparacion de Reparar, busqueda de atomos de Analizar) deben
	// encolar esta limpieza como ULTIMA accion, termine como
	// termine, para que la guarda no se quede atorada.
	static void clearThreadQueued(void* data);

	static std::stringstream buffer_;
};

#endif  // GUI_H
