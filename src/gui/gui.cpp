#include "gui.h"
#include "multi_open.h"

// Punto 10: el diálogo nativo de Windows para elegir varios archivos
// necesita la API de Windows. Solo en Windows: en Linux/macOS este
// bloque no existe y se usa uiOpenFile. WIN32_LEAN_AND_MEAN y
// NOMINMAX van antes, para que windows.h no defina las macros
// min/max (el resto del archivo es C++ estándar).
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <cderr.h>  // FNERR_BUFFERTOOSMALL (WIN32_LEAN_AND_MEAN lo excluye)
#include <ui_windows.h>  // uiWindowsControlMinimumSize (diagnóstico de tamaño)
#endif

#include <string.h>  // memset
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <thread>

extern "C" {
#include "libavutil/log.h"
}

#include "../atom.h"
#include "../common.h"
#include "../diagnoser.h"
#include "../formatos.h"
#include "../mp4.h"
#include "../plantillas.h"
#include "../repair_job.h"
#include "../repair_options.h"
#include "../tarjeta.h"
#include "../shell_util.h"
#include "../upath.h"
#include "../ai/ai_provider.h"
#include "../ai/external/external_provider.h"
#include "../ai/hybrid.h"
#include "../ai/lmstudio/lmstudio_provider.h"
#include "../ai/onnx/onnx_provider.h"
#include <string>
#include <vector>
using namespace std;

// Opciones de la pestana Ajustes (definida mas abajo; ver repair_options.h).
static RepairOptions& guiRepairOptions();

stringstream Gui::buffer_ = stringstream();
uiWindow* Gui::window_;
uiMultilineEntry* Gui::current_text_entry_;
uiEntry* Gui::Repair::entry_ok_;
uiEntry* Gui::Repair::entry_bad_;
uiMultilineEntry* Gui::Repair::text_entry_;
uiLabel* Gui::Repair::label_status_;
uiProgressBar* Gui::Repair::progressbar_;
uiCheckbox* Gui::Repair::chk_forzar_ = nullptr;
uiEntry* Gui::Analyze::entry_ok_;
uiEntry* Gui::Tarjeta::entry_origen_;
uiEntry* Gui::Tarjeta::entry_destino_;
uiEntry* Gui::Tarjeta::entry_ref_;
uiCheckbox* Gui::Tarjeta::chk_solo_buscar_;
uiMultilineEntry* Gui::Tarjeta::text_entry_;
uiProgressBar* Gui::Tarjeta::progressbar_;
uiLabel* Gui::Tarjeta::label_estado_;
uiMultilineEntry* Gui::Analyze::text_entry_;
uiProgressBar* Gui::Analyze::progressbar_;
thread* Gui::thread_ = nullptr;
std::string Gui::thread_desc_;

// Pestaña "IA": controles por bloque (ver gui.h, struct Ai). Se
// guardan aqui para que cada checkbox pueda habilitar/deshabilitar
// los inputs de su bloque en Ai::refresh().
uiCheckbox* Gui::Ai::chk_local_ = nullptr;
uiCombobox* Gui::Ai::combo_profile_ = nullptr;
uiCheckbox* Gui::Ai::chk_lmstudio_ = nullptr;
uiEntry* Gui::Ai::entry_lm_endpoint_ = nullptr;
uiEntry* Gui::Ai::entry_lm_model_ = nullptr;
uiSpinbox* Gui::Ai::spin_lm_timeout_ = nullptr;
uiButton* Gui::Ai::btn_lm_list_ = nullptr;
uiButton* Gui::Ai::btn_lm_test_ = nullptr;
uiCheckbox* Gui::Ai::chk_external_ = nullptr;
uiEntry* Gui::Ai::entry_ext_endpoint_ = nullptr;
uiEntry* Gui::Ai::entry_ext_model_ = nullptr;
uiEntry* Gui::Ai::entry_ext_key_ = nullptr;
uiSpinbox* Gui::Ai::spin_ext_timeout_ = nullptr;
uiButton* Gui::Ai::btn_ext_test_ = nullptr;
uiLabel* Gui::Ai::label_status_ = nullptr;

// Pestaña "Entrenar": listas de vídeos del usuario (ver gui.h,
// struct Train). SOLO EN MEMORIA: nunca se escriben a disco
// (privacidad, §11). Lo que persiste es el dataset + el modelo.
uiMultilineEntry* Gui::Train::list_buenos_ = nullptr;
uiMultilineEntry* Gui::Train::list_corruptos_ = nullptr;
uiLabel* Gui::Train::label_contadores_ = nullptr;
uiLabel* Gui::Train::label_base_ = nullptr;
uiLabel* Gui::Train::label_modelo_ = nullptr;
uiLabel* Gui::Train::label_estado_ = nullptr;
uiProgressBar* Gui::Train::progressbar_ = nullptr;
uiCheckbox* Gui::Train::chk_usar_ = nullptr;
uiButton* Gui::Train::btn_dataset_ = nullptr;
uiButton* Gui::Train::btn_anexar_ = nullptr;
uiButton* Gui::Train::btn_ver_base_ = nullptr;
uiButton* Gui::Train::btn_train_ = nullptr;
uiButton* Gui::Train::btn_test_ = nullptr;
uiButton* Gui::Train::btn_borrar_ = nullptr;
uiCombobox* Gui::Train::combo_motor_ = nullptr;
uiMultilineEntry* Gui::Train::text_detalles_ = nullptr;
bool Gui::Train::confirm_cero_armed_ = false;
bool Gui::Train::confirm_borrar_armed_ = false;
std::vector<std::string> Gui::Train::buenos_;
std::vector<std::string> Gui::Train::corruptos_;
std::thread* Gui::Train::thread_ = nullptr;

// Pestaña "Asistente" (Fase 3, 3.1): ver gui.h, struct Assistant.
uiMultilineEntry* Gui::Assistant::list_danados_ = nullptr;
uiLabel* Gui::Assistant::label_contadores_ = nullptr;
uiLabel* Gui::Assistant::label_formatos_ = nullptr;
uiCombobox* Gui::Assistant::combo_modo_ = nullptr;
uiEntry* Gui::Assistant::entry_ref_ = nullptr;
uiButton* Gui::Assistant::btn_ref_examinar_ = nullptr;
uiCombobox* Gui::Assistant::combo_perfil_ = nullptr;
uiButton* Gui::Assistant::btn_convertir_ = nullptr;
uiButton* Gui::Assistant::btn_reparar_todo_ = nullptr;
uiCheckbox* Gui::Assistant::chk_forzar_ = nullptr;
uiLabel* Gui::Assistant::label_estado_ = nullptr;
uiProgressBar* Gui::Assistant::progressbar_ = nullptr;
uiMultilineEntry* Gui::Assistant::text_informe_ = nullptr;
uiLabel* Gui::Assistant::label_preview_target_ = nullptr;
uiLabel* Gui::Assistant::label_preview_ = nullptr;
uiButton* Gui::Assistant::btn_preview_prev_ = nullptr;
uiButton* Gui::Assistant::btn_preview_next_ = nullptr;
uiButton* Gui::Assistant::btn_preview_5s_ = nullptr;
uiButton* Gui::Assistant::btn_preview_foto_ = nullptr;
uiButton* Gui::Assistant::btn_preview_abrir_ = nullptr;
std::vector<std::string> Gui::Assistant::danados_;
std::vector<std::string> Gui::Assistant::salidas_;
int Gui::Assistant::preview_index_ = 0;
int Gui::Assistant::total_ = 0;
int Gui::Assistant::actual_ = 0;
int Gui::Assistant::last_global_ = -1;

void Gui::init() {
	uiInitOptions o;
	memset(&o, 0, sizeof(uiInitOptions));
	if (uiInit(&o) != NULL) abort();

	window_ = newWindow("untrunc-gui", 820, 640, 0);

	// Preferencias guardadas (ai.conf) -> globales de la GUI, ANTES de
	// construir las pestanas. Asi, p. ej., el ONNX activado sigue
	// activado al volver a abrir el programa.
	{
		AiConfig c = loadAiConfig();  // g_ai_gui_activa aun es false
		g_ai_local_enabled = c.local_enabled;
		g_ai_local_profile = c.local_profile;
		g_onnx_enabled = c.onnx_enabled;
		g_onnx_model_path = c.onnx_model_path;
		g_onnx_threshold = c.onnx_threshold;
		g_ai_lmstudio_enabled = c.lmstudio_enabled;
		g_lmstudio_endpoint = c.lmstudio_endpoint;
		g_lmstudio_model = c.lmstudio_model;
		g_lmstudio_timeout_seconds = c.lmstudio_timeout_seconds;
		g_ai_gui_activa = true;
	}

	build();
	setSpaced(true);

	cout.rdbuf(buffer_.rdbuf());
	cerr.rdbuf(buffer_.rdbuf());
	av_log_set_callback([](void* ptr, int loglevel, const char* msg, va_list vl) {
		if (loglevel > av_log_get_level()) return;

		const uint limit = 256;
		char buffer[limit+1];
		AVClass *avc = ptr ? *(AVClass**)ptr : NULL;
		auto n = snprintf(buffer, limit, "[%s @ %p] ", avc->item_name(ptr), ptr);
		vsnprintf(buffer+n, limit-n, msg, vl);

		cout << buffer << '\n';
	});

	current_text_entry_ = Gui::Repair::text_entry_;
	uiTimer(100, Gui::writeOutput, NULL);

	uiWindowOnClosing(window_, Gui::onClosing, NULL);
}

void Gui::run() {
	uiControlShow(uiControl(window_));
	uiMain();
}

void Gui::build() {
	struct {
		const char* name;
		uiControl* (*f)(void);
	} pages[] = {
	    {"Asistente", Gui::assistantTab},
	    {"Reparar", Gui::repairTab},
	    {"IA", Gui::aiTab},
	    {"Entrenar", Gui::trainTab},
	    {"Ajustes", Gui::settingsTab},
	    {"Tarjeta SD", Gui::tarjetaTab},
	    {"Analizar", Gui::analyzeTab},
	    {"Acerca de", Gui::aboutTab},
	    {NULL, NULL},
    };

	auto tabs = newTab();
	std::vector<uiControl*> ctrls;
	for (uint i = 0; pages[i].name != NULL; i++) {
		ctrls.push_back((*(pages[i].f))());
		uiTabAppend(tabs, pages[i].name, ctrls.back());
	}

	uiWindowSetChild(window_, uiControl(tabs));

#ifdef _WIN32
	// Diagnóstico para desarrollo: con UNTRUNC_GUI_MEDIR=<archivo> se
	// escribe el tamaño mínimo (px) de cada pestaña. La ventana mide lo
	// que la pestaña más grande, y debe caber en pantallas de 1366×768.
	if (const char* f = getenv("UNTRUNC_GUI_MEDIR")) {
		if (FILE* out = fopen(f, "w")) {
			for (size_t i = 0; i < ctrls.size(); i++) {
				int w = 0, h = 0;
				uiWindowsControlMinimumSize(uiWindowsControl(ctrls[i]), &w, &h);
				fprintf(out, "%-12s %5d x %5d\n", pages[i].name, w, h);
			}
			fclose(out);
		}
	}
#endif
}

void Gui::openFile(uiButton* b, void* data) {
	auto fn = uiOpenFile(window_);
	if (fn) {
		uiEntrySetText(uiEntry(data), fn);
		uiFreeText(fn);
	}
}

#ifdef _WIN32
// Convierte UTF-16 (lo que devuelve el diálogo nativo) a UTF-8 (lo
// que usa el resto del programa y libui). Solo Windows.
static std::string utf8FromWide(const wchar_t* s) {
	if (!s) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
	if (n <= 0) return std::string();
	std::vector<char> tmp(n);
	WideCharToMultiByte(CP_UTF8, 0, s, -1, tmp.data(), n, NULL, NULL);
	return std::string(tmp.data());
}
#endif

// Punto 10 (2026-11-07): diálogo para elegir UNO O VARIOS vídeos a
// la vez, con Ctrl o Mayús. libui no tiene selección múltiple
// (`uiOpenFile` devuelve un solo archivo), así que en Windows se usa
// el diálogo nativo `GetOpenFileNameW` con OFN_ALLOWMULTISELECT; en
// Linux/macOS se usa `uiOpenFile` (un archivo, como antes). La
// versión W del diálogo se usa a propósito: con la versión A las
// rutas con acentos (p. ej. "José Alfredo Jiménez.mp4") llegarían
// en la página de códigos del sistema y no en UTF-8, y se romperían
// al compararlas, mostrarlas o pasarlas al script. Al cancelar
// devuelve una lista vacía (no cambia nada). Los textos del diálogo
// nativo van con acentos: en W son UTF-16 y se ven bien.
std::vector<std::string> Gui::openVideoFilesMulti() {
#ifdef _WIN32
	// 32K wchar_t (64 KiB): caben cientos de rutas. Si el usuario
	// elige tantos que no caben, GetOpenFileName falla con
	// FNERR_BUFFERTOOSMALL y se le avisa que elija menos.
	static wchar_t buf[32768];
	memset(buf, 0, sizeof(buf));
	OPENFILENAMEW ofn;
	memset(&ofn, 0, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = NULL;
	ofn.lpstrFilter = L"Vídeos\0*.mp4;*.mov;*.m4v;*.3gp;*.mkv;*.avi;*.wmv;*.webm;*.flv;*.mpg;*.mts\0Todos los archivos\0*.*\0\0";
	ofn.nFilterIndex = 1;
	ofn.lpstrFile = buf;
	ofn.nMaxFile = 32768;
	ofn.lpstrTitle = L"Elige uno o varios vídeos";
	ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	if (!GetOpenFileNameW(&ofn)) {
		if (CommDlgExtendedError() == FNERR_BUFFERTOOSMALL)
			Gui::msgBoxError("Elegiste demasiados vídeos a la vez para el diálogo: elige menos archivos en cada tanda.");
		return std::vector<std::string>();
	}
	std::vector<std::string> out;
	for (auto& w : MultiOpen::parseMultiSelectBuffer(buf, 32768))
		out.push_back(utf8FromWide(w.c_str()));
	return out;
#else
	char* fn = uiOpenFile(window_);
	if (!fn) return std::vector<std::string>();
	std::string path = fn;
	uiFreeText(fn);
	return std::vector<std::string>{path};
#endif
}

int Gui::onClosing(uiWindow* w, void* data) {
	if (thread_)  {
		thread_->join();
		delete(thread_);
	}
	uiQuit();
	return 1;
}

int Gui::writeOutput(void* data) {
	auto txt = uiNewAttributedString(buffer_.str().c_str());
	if (uiAttributedStringLen(txt)) {
		uiMultilineEntryAppend(current_text_entry_, uiAttributedStringString(txt));
		buffer_.str("");
	}
//	uiMultilineEntryAppend(current_text_entry_, buffer_.str().c_str());
	return 1;
}

uiEntry* Gui::addFileOpen(uiBox* parent_box, const char* text) {
	auto h_box = newHorizontalBox();

	auto button = newButton(text);
	auto entry = newEntry();
	uiButtonOnClicked(button, openFile, entry);
	uiBoxAppend(h_box, uiControl(button), 0);
	uiBoxAppend(h_box, uiControl(entry), 1);
	uiBoxAppend(parent_box, uiControl(h_box), 1);
	return entry;
}

uiControl* Gui::repairTab() {
	auto v_box = newVerticalBox();

	auto h_box1 = newHorizontalBox();
	Repair::entry_ok_ = addFileOpen(h_box1, "Vídeo de referencia (sano)");
	Repair::entry_bad_ = addFileOpen(h_box1, "Vídeo dañado");
	uiBoxAppend(v_box, uiControl(h_box1), 0);

	// Formatos (Fase 2.5): Untrunc solo repara MP4, MOV, M4V y
	// 3GP directamente. Los demas formatos se convierten primero
	// a MP4 con FFmpeg (boton de abajo) y se repara el MP4.
	auto label_format = uiNewLabel("Formatos directos: MP4, MOV, M4V, 3GP. "
		"MKV, AVI, WMV, WebM, FLV, MPG, MTS: pulsa «Convertir a MP4» primero.");
	uiBoxAppend(v_box, uiControl(label_format), 0);
	auto h_box_fmt = newHorizontalBox();
	auto b_convert = newButton("Convertir a MP4");
	uiButtonOnClicked(b_convert, convertBadToMp4, NULL);
	auto label_convert = uiNewLabel("Sin re-codificar si se puede; si no, re-codifica a H.264 + AAC y lo avisa.");
	uiBoxAppend(h_box_fmt, uiControl(b_convert), 0);
	uiBoxAppend(h_box_fmt, uiControl(label_convert), 1);
	uiBoxAppend(v_box, uiControl(h_box_fmt), 0);

	// Punto 8 (reparaciones 2026-11-07, con el punto 4): si el
	// pre-vuelo rechaza el dañado (sin ftyp, sin mdat, o con la
	// cabecera inicial dañada pero estructura MP4 más adelante),
	// con esta casilla marcada se intenta el rescate sin
	// referencia en vez de bloquearse. Es justo el caso de Angel
	// con f19132479.mp4: un vídeo que según él sí es bueno y que
	// el pre-vuelo rechazaba con BadNoFtyp.
	Repair::chk_forzar_ = uiNewCheckbox("Intentar de todos modos si el archivo no parece MP4 (rescate)");
	auto label_forzar_rep = uiNewLabel("Solo si el pre-vuelo lo rechaza: salta el pre-vuelo y prueba el rescate sin referencia.");
	uiBoxAppend(v_box, uiControl(Repair::chk_forzar_), 0);
	uiBoxAppend(v_box, uiControl(label_forzar_rep), 0);

	// Fase 1 (1.5): el registro tecnico sigue en ingles, pero con
	// etiqueta en espanol para que se entienda que es el detalle.
	auto label_log = uiNewLabel("Detalles (registro técnico en inglés):");
	uiBoxAppend(v_box, uiControl(label_log), 0);

	Repair::text_entry_ = uiNewMultilineEntry();
	uiMultilineEntrySetReadOnly(Repair::text_entry_, 1);
	uiBoxAppend(v_box, uiControl(Repair::text_entry_), 1);

	auto h_box2 = newHorizontalBox();
	Repair::label_status_ = uiNewLabel("");
	auto b_repair = newButton("Reparar");
	uiButtonOnClicked(b_repair, startRepair, NULL);
	//	uiBoxAppend(v_box, uiControl(b_repair), 0);
	uiBoxAppend(h_box2, uiControl(Repair::label_status_), 1);
	uiBoxAppend(h_box2, uiControl(b_repair), 0);
	uiBoxAppend(v_box, uiControl(h_box2), 0);

	Repair::progressbar_ = uiNewProgressBar();
	uiBoxAppend(v_box, uiControl(Repair::progressbar_), 0);

	return uiControl(v_box);
}

#define CATCH_THEM(R) \
	catch (const char* e) {cerr << e << '\n'; msgBoxError(e, true); R;} \
	catch (string e) {cerr << e << '\n'; msgBoxError(e, true); R;} \
	catch (const std::exception& e) {cerr << e.what() << '\n'; msgBoxError(e.what(), true); R;}

uiControl* Gui::settingsTab() {
	auto v_box = newVerticalBox();

	auto group_general = newGroup("General");
	auto h_box_log = newHorizontalBox();
	auto label_log = uiNewLabel("Nivel de registro");
	auto combo_log = uiNewCombobox();
	uiComboboxAppend(combo_log, "silencioso (-q)");
	uiComboboxAppend(combo_log, "información (por defecto)");
	uiComboboxAppend(combo_log, "advertencias ocultas (-w)");
	uiComboboxAppend(combo_log, "detallado (-v)");
	uiComboboxAppend(combo_log, "muy detallado (-vv)");
	uiBoxAppend(h_box_log, uiControl(label_log), 0);
	uiBoxAppend(h_box_log, uiControl(combo_log), 0);
	uiGroupSetChild(group_general, uiControl(h_box_log));
	uiBoxAppend(v_box, uiControl(group_general), 0);

	auto group_repair = newGroup("Reparación");
	auto v_box_repair = newVerticalBox();
	auto chk_skip = uiNewCheckbox("Omitir secuencias desconocidas (-s)");

	auto h_box_step_size = newHorizontalBox();
	auto label_step_size = uiNewLabel("       Tamaño de paso (-st)");
	auto num_step_size = uiNewSpinbox(1, 1<<16);
	uiBoxAppend(h_box_step_size, uiControl(label_step_size), 0);
	uiBoxAppend(h_box_step_size, uiControl(num_step_size), 0);

	auto chk_stretch = uiNewCheckbox("Estirar vídeo para igualar el audio (-sv)");
	auto chk_keep_unknown = uiNewCheckbox("Conservar secuencias desconocidas (-k)");
	auto chk_use_dyn = uiNewCheckbox("Usar estadísticas dinámicas (-dyn)");
	auto chk_rsv_ben = uiNewCheckbox("Modo de recuperación RSV (-rsv-ben)");

	auto h_box_max_partsize = newHorizontalBox();
	auto label_max_partsize = uiNewLabel("      Tamaño máximo de parte (-mp)");
	auto num_max_partsize = uiNewEntry();
	uiBoxAppend(h_box_max_partsize, uiControl(label_max_partsize), 0);
	uiBoxAppend(h_box_max_partsize, uiControl(num_max_partsize), 0);

	uiBoxAppend(v_box_repair, uiControl(chk_skip), 0);
	uiBoxAppend(v_box_repair, uiControl(h_box_step_size), 0);
	uiBoxAppend(v_box_repair, uiControl(chk_stretch), 0);
	uiBoxAppend(v_box_repair, uiControl(chk_keep_unknown), 0);
	uiBoxAppend(v_box_repair, uiControl(chk_use_dyn), 0);
	uiBoxAppend(v_box_repair, uiControl(chk_rsv_ben), 0);
	uiBoxAppend(v_box_repair, uiControl(h_box_max_partsize), 0);
	uiGroupSetChild(group_repair, uiControl(v_box_repair));
	uiBoxAppend(v_box, uiControl(group_repair), 0);

	// La configuracion de IA vive ahora en la pestana "IA" (ver
	// Gui::aiTab): un bloque por conexion, cada uno con su checkbox
	// y sus inputs. Aqui solo queda este apunte para no duplicar
	// controles que escriben los mismos globales.
	auto label_ai_moved = uiNewLabel("Inteligencia artificial (IA): ver la pestaña «IA».");
	uiBoxAppend(v_box, uiControl(label_ai_moved), 0);

	uiComboboxSetSelected(combo_log, 1);
	uiComboboxOnSelected(combo_log, [](uiCombobox* c_box, void* data){
		auto v = uiComboboxSelected(c_box);
		if (v) v++;  // skip LogMode::W
		g_log_mode = (LogMode)v;

	}, nullptr);

	uiCheckboxSetChecked(chk_skip, guiRepairOptions().ignore_unknown);
	uiCheckboxOnToggled(chk_skip, [](uiCheckbox* chk_box, void* spinbox){
		auto v = uiCheckboxChecked(chk_box);
		if (v) uiControlEnable(uiControl(spinbox));
		else uiControlDisable(uiControl(spinbox));
		guiRepairOptions().ignore_unknown = v;

	}, num_step_size);

	if (!guiRepairOptions().ignore_unknown) uiControlDisable(uiControl(num_step_size));
	uiSpinboxSetValue(num_step_size, (int)guiRepairOptions().step);
	uiSpinboxOnChanged(num_step_size, [](uiSpinbox* spinbox, void* data){
		guiRepairOptions().step = (uint64_t)uiSpinboxValue(spinbox);
	}, nullptr);

	uiCheckboxSetChecked(chk_stretch, guiRepairOptions().stretch_video);
	uiCheckboxOnToggled(chk_stretch, [](uiCheckbox* chk_box, void* data){
		auto v = uiCheckboxChecked(chk_box);
		if (v) msgBox("Esta función está en fase beta, puede que no funcione.");
		guiRepairOptions().stretch_video = v;

	}, nullptr);

	uiCheckboxSetChecked(chk_keep_unknown, guiRepairOptions().dont_exclude);
	uiCheckboxOnToggled(chk_keep_unknown, [](uiCheckbox* chk_box, void* data){
		auto v = uiCheckboxChecked(chk_box);
		guiRepairOptions().dont_exclude = v;

	}, nullptr);

	uiCheckboxSetChecked(chk_use_dyn, guiRepairOptions().use_chunk_stats);
	uiCheckboxOnToggled(chk_use_dyn, [](uiCheckbox* chk_box, void* data){
		auto v = uiCheckboxChecked(chk_box);
		guiRepairOptions().use_chunk_stats = v;

	}, nullptr);

	uiCheckboxSetChecked(chk_rsv_ben, guiRepairOptions().rsv_ben_mode);
	uiCheckboxOnToggled(chk_rsv_ben, [](uiCheckbox* chk_box, void* data){
		auto v = uiCheckboxChecked(chk_box);
		guiRepairOptions().rsv_ben_mode = v;

	}, nullptr);

	string s_max_partsize = to_string(guiRepairOptions().max_partsize);
	uiEntrySetText(num_max_partsize, s_max_partsize.c_str());
	uiEntryOnChanged(num_max_partsize, [](uiEntry *entry, void *data){
		try {
			string s = uiEntryText(entry);
			if (s.empty()) s = "0";
			// Igual que parseMaxPartsize(), pero sobre las opciones
			// de la GUI y no sobre las globales.
			guiRepairOptions().max_partsize_default = 0;
			if (s != "f") guiRepairOptions().max_partsize = (unsigned)parseByteStr(s);
		}
		CATCH_THEM(uiEntrySetText(entry, "0"));
	}, nullptr);

	return uiControl(v_box);
}

// Opciones de reparacion de la GUI (pestana Ajustes). Las casillas
// escriben AQUI, no en las globales del nucleo: cada operacion toma
// una foto al pulsar su boton (repair_options.h), asi que cambiar un
// ajuste a mitad de una reparacion ya no la altera.
static RepairOptions& guiRepairOptions() {
	static RepairOptions opts = RepairOptions::fromGlobals();
	return opts;
}

// Barra de progreso de la pestana Reparar desde el hilo de trabajo:
// libui solo admite cambios de interfaz en el hilo principal, asi que
// se encola (y solo cuando el valor cambia, para no saturar la cola).
static void repairProgressQueued(void* data) {
	int* p = static_cast<int*>(data);
	Gui::Repair::onProgress(*p);
	delete p;
}
static void repairProgressFromWorker(int percentage) {
	static std::atomic<int> last{-1};
	if (last.exchange(percentage) == percentage) return;
	uiQueueMain(repairProgressQueued, new int(percentage));
}

#define defineFileForAnalyze() \
	current_text_entry_ = Analyze::text_entry_; \
	uiMultilineEntrySetText(current_text_entry_, ""); \
	string file_ok = uiEntryText(Analyze::entry_ok_); \
	if (file_ok.empty()) { \
		msgBoxError("Falta el archivo: elige el vídeo de referencia (sano)."); \
		return; \
	} \

#define ASYNC(fn, data) uiQueueMain((void (*)(void*))fn, (void*)data)

#define defineMp4ForAnalyze() \
	defineFileForAnalyze(); \
	RepairOptions analyze_opts_ = guiRepairOptions(); \
	analyze_opts_.on_progress = Gui::Analyze::onProgress; \
	/* Sin esperar: en el hilo de la interfaz no se puede bloquear. */ \
	RepairSession analyze_session_(analyze_opts_, true); \
	if (!analyze_session_.active()) { \
		msgBox("Hay una reparación en marcha. Espera a que termine para analizar."); \
		return; \
	} \
	Mp4 mp4; \
	try { \
		g_mp4 = &mp4; \
		mp4.parseOk(file_ok); \
	} \
	CATCH_THEM(return);

// Perfiles de camara de la IA local (los "id" de
// profiles/profiles.json). El desplegable de la pestana IA muestra
// "Automático (recomendado)" (indice 0) y luego estos 12.
static const char* kAiLocalProfiles[] = {
	"generico_h264_1080p30",
	"generico_h264_720p30",
	"generico_h264_360p30",
	"iphone_h264_1080p30",
	"iphone_hevc_4k30",
	"samsung_h264_1080p30",
	"gopro_h264_1080p60",
	"gopro_hevc_4k30",
	"dji_h264_4k30",
	"sony_xavc_1080p30",
	"canon_hevc_4k30",
	"obs_x264_1080p30",
};
static const int kAiLocalProfilesCount = 12;

// Lee el texto de un uiEntry copiandolo a string (libui devuelve
// un texto que hay que liberar con uiFreeText).
static std::string entryText(uiEntry* e) {
	char* t = uiEntryText(e);
	std::string s = t ? t : "";
	if (t) uiFreeText(t);
	return s;
}

void Gui::Ai::updateStatus() {
	if (!label_status_) return;
	std::vector<std::string> parts;
	if (chk_local_ && uiCheckboxChecked(chk_local_)) {
		int sel = combo_profile_ ? uiComboboxSelected(combo_profile_) : 0;
		if (sel > 0 && sel <= kAiLocalProfilesCount)
			parts.push_back(std::string("Local (perfil ") + kAiLocalProfiles[sel - 1] + ")");
		else
			parts.push_back("Local (perfil automático)");
	}
	if (chk_lmstudio_ && uiCheckboxChecked(chk_lmstudio_)) {
		std::string m = entry_lm_model_ ? entryText(entry_lm_model_) : "";
		parts.push_back(m.empty() ? "LM Studio (Qwen, autodetectar)"
		                          : std::string("LM Studio (") + m + ")");
	}
	if (chk_external_ && uiCheckboxChecked(chk_external_))
		parts.push_back("Externa (API)");
	std::string s = "Conexiones activas: ";
	if (parts.empty()) {
		s += "ninguna — se usa solo el modo clásico con referencia";
	} else {
		for (size_t i = 0; i < parts.size(); i++) {
			if (i) s += "  •  ";
			s += parts[i];
		}
	}
	uiLabelSetText(label_status_, s.c_str());
}

void Gui::Ai::refresh() {
	// Cada checkbox habilita/deshabilita SOLO los inputs de su
	// bloque. Un bloque apagado no hace ninguna llamada de red y
	// el orquestador no lo consulta (ver startRepair).
	if (combo_profile_) {
		if (chk_local_ && uiCheckboxChecked(chk_local_)) uiControlEnable(uiControl(combo_profile_));
		else uiControlDisable(uiControl(combo_profile_));
	}
	bool lm = chk_lmstudio_ && uiCheckboxChecked(chk_lmstudio_);
	uiControl* lm_ctrls[] = {uiControl(entry_lm_endpoint_), uiControl(entry_lm_model_),
	                         uiControl(spin_lm_timeout_), uiControl(btn_lm_list_),
	                         uiControl(btn_lm_test_)};
	for (auto* c : lm_ctrls) {
		if (!c) continue;
		if (lm) uiControlEnable(c); else uiControlDisable(c);
	}
	bool ext = chk_external_ && uiCheckboxChecked(chk_external_);
	uiControl* ext_ctrls[] = {uiControl(entry_ext_endpoint_), uiControl(entry_ext_model_),
	                          uiControl(entry_ext_key_), uiControl(spin_ext_timeout_),
	                          uiControl(btn_ext_test_)};
	for (auto* c : ext_ctrls) {
		if (!c) continue;
		if (ext) uiControlEnable(c); else uiControlDisable(c);
	}
	updateStatus();
}

uiControl* Gui::aiTab() {
	auto v_box = newVerticalBox();

	// Etiqueta de estado: que conexiones estan activas ahora mismo.
	Ai::label_status_ = uiNewLabel("Conexiones activas: Local");
	uiBoxAppend(v_box, uiControl(Ai::label_status_), 0);

	// --- Bloque 1: IA local (offline, siempre disponible) ---
	auto group_local = newGroup("1 · IA local — offline, sin internet");
	auto v_local = newVerticalBox();
	Ai::chk_local_ = uiNewCheckbox("Activar IA local");
	auto label_local_info = uiNewLabel("Offline, el vídeo nunca sale de tu PC. Heurísticas + perfiles de cámara.");
	auto h_local_profile = newHorizontalBox();
	auto label_local_profile = uiNewLabel("      Perfil por defecto");
	Ai::combo_profile_ = uiNewCombobox();
	uiComboboxAppend(Ai::combo_profile_, "Automático (recomendado)");
	for (int i = 0; i < kAiLocalProfilesCount; i++)
		uiComboboxAppend(Ai::combo_profile_, kAiLocalProfiles[i]);
	uiBoxAppend(h_local_profile, uiControl(label_local_profile), 0);
	uiBoxAppend(h_local_profile, uiControl(Ai::combo_profile_), 1);
	uiBoxAppend(v_local, uiControl(Ai::chk_local_), 0);
	uiBoxAppend(v_local, uiControl(label_local_info), 0);
	uiBoxAppend(v_local, uiControl(h_local_profile), 0);
	uiGroupSetChild(group_local, uiControl(v_local));
	// Altura (2026-11-12): las tres secciones iban una debajo de otra
	// (707 px de contenido) y la ventana no cabía en pantallas de 768 px.
	// Ahora cada una es una sub-pestaña; la nota general va con «Local».
	auto sub_ia = newTab();
	auto pag_local = newVerticalBox();
	uiTabAppend(sub_ia, "Local (recomendada)", uiControl(pag_local));
	uiBoxAppend(pag_local, uiControl(group_local), 0);

	// --- Bloque 2: LM Studio — Qwen local (localhost:1234) ---
	auto group_lm = newGroup("2 · LM Studio — Qwen local (avanzado, opcional)");
	auto v_lm = newVerticalBox();
	Ai::chk_lmstudio_ = uiNewCheckbox("Activar LM Studio (Qwen local)");
	auto label_lm_info = newTextLines("Avanzado: necesita LM Studio instalado aparte con Qwen cargado. No hace falta para reparar.\nQwen solo razona sobre el diagnóstico en texto. El vídeo nunca sale de tu PC.");
	auto h_lm_endpoint = newHorizontalBox();
	auto label_lm_endpoint = uiNewLabel("      Endpoint");
	Ai::entry_lm_endpoint_ = uiNewEntry();
	uiBoxAppend(h_lm_endpoint, uiControl(label_lm_endpoint), 0);
	uiBoxAppend(h_lm_endpoint, uiControl(Ai::entry_lm_endpoint_), 1);
	auto h_lm_model = newHorizontalBox();
	auto label_lm_model = uiNewLabel("      Modelo Qwen");
	Ai::entry_lm_model_ = uiNewEntry();
	Ai::btn_lm_list_ = uiNewButton("Listar modelos");
	uiBoxAppend(h_lm_model, uiControl(label_lm_model), 0);
	uiBoxAppend(h_lm_model, uiControl(Ai::entry_lm_model_), 1);
	uiBoxAppend(h_lm_model, uiControl(Ai::btn_lm_list_), 0);
	auto h_lm_timeout = newHorizontalBox();
	auto label_lm_timeout = uiNewLabel("      Tiempo de espera (segundos)");
	Ai::spin_lm_timeout_ = uiNewSpinbox(10, 180);
	uiBoxAppend(h_lm_timeout, uiControl(label_lm_timeout), 0);
	uiBoxAppend(h_lm_timeout, uiControl(Ai::spin_lm_timeout_), 0);
	Ai::btn_lm_test_ = uiNewButton("Probar conexión con LM Studio");
	uiBoxAppend(v_lm, uiControl(Ai::chk_lmstudio_), 0);
	uiBoxAppend(v_lm, uiControl(label_lm_info), 0);
	uiBoxAppend(v_lm, uiControl(h_lm_endpoint), 0);
	uiBoxAppend(v_lm, uiControl(h_lm_model), 0);
	uiBoxAppend(v_lm, uiControl(h_lm_timeout), 0);
	uiBoxAppend(v_lm, uiControl(Ai::btn_lm_test_), 0);
	uiGroupSetChild(group_lm, uiControl(v_lm));
	uiTabAppend(sub_ia, "LM Studio (avanzado)", uiControl(group_lm));

	// --- Bloque 3: IA externa (por API, opt-in) ---
	auto group_ext = newGroup("3 · IA externa — por API (opt-in)");
	auto v_ext = newVerticalBox();
	Ai::chk_external_ = uiNewCheckbox("Activar IA externa por API");
	auto label_ext_warn = uiNewLabel("Solo envía texto de diagnóstico (<50 KB), nunca el vídeo ni fotogramas.");
	auto h_ext_endpoint = newHorizontalBox();
	auto label_ext_endpoint = uiNewLabel("      Endpoint");
	Ai::entry_ext_endpoint_ = uiNewEntry();
	uiBoxAppend(h_ext_endpoint, uiControl(label_ext_endpoint), 0);
	uiBoxAppend(h_ext_endpoint, uiControl(Ai::entry_ext_endpoint_), 1);
	auto h_ext_model = newHorizontalBox();
	auto label_ext_model = uiNewLabel("      Modelo");
	Ai::entry_ext_model_ = uiNewEntry();
	uiBoxAppend(h_ext_model, uiControl(label_ext_model), 0);
	uiBoxAppend(h_ext_model, uiControl(Ai::entry_ext_model_), 1);
	auto h_ext_key = newHorizontalBox();
	auto label_ext_key = uiNewLabel("      API key");
	Ai::entry_ext_key_ = uiNewEntry();
	uiBoxAppend(h_ext_key, uiControl(label_ext_key), 0);
	uiBoxAppend(h_ext_key, uiControl(Ai::entry_ext_key_), 1);
	auto label_ext_key_note = newTextLines("      La clave se escribe visible: libui no tiene campo de contraseña. No se guarda en el archivo ni aparece en los registros; solo vive en memoria en esta sesión. Ten cuidado al compartir pantalla.");
	auto h_ext_timeout = newHorizontalBox();
	auto label_ext_timeout = uiNewLabel("      Tiempo de espera (segundos)");
	Ai::spin_ext_timeout_ = uiNewSpinbox(5, 120);
	uiBoxAppend(h_ext_timeout, uiControl(label_ext_timeout), 0);
	uiBoxAppend(h_ext_timeout, uiControl(Ai::spin_ext_timeout_), 0);
	Ai::btn_ext_test_ = uiNewButton("Probar conexión con la IA externa");
	uiBoxAppend(v_ext, uiControl(Ai::chk_external_), 0);
	uiBoxAppend(v_ext, uiControl(label_ext_warn), 0);
	uiBoxAppend(v_ext, uiControl(h_ext_endpoint), 0);
	uiBoxAppend(v_ext, uiControl(h_ext_model), 0);
	uiBoxAppend(v_ext, uiControl(h_ext_key), 0);
	uiBoxAppend(v_ext, uiControl(label_ext_key_note), 0);
	uiBoxAppend(v_ext, uiControl(h_ext_timeout), 0);
	uiBoxAppend(v_ext, uiControl(Ai::btn_ext_test_), 0);
	uiGroupSetChild(group_ext, uiControl(v_ext));
	uiTabAppend(sub_ia, "IA externa", uiControl(group_ext));

	auto label_ai_note = newTextLines("La IA nunca genera vídeo: solo infiere estructura (códec, perfil, modo) y explica en español. Si todas están apagadas, se usa solo el modo clásico con referencia.");
	uiBoxAppend(pag_local, uiControl(label_ai_note), 0);
	uiBoxAppend(v_box, uiControl(sub_ia), 1);

	// --- Valores iniciales: la configuracion efectiva (ai.conf y
	// variables de entorno, via loadAiConfig), no solo los valores
	// por defecto de los globales. Asi la pestana muestra lo que
	// realmente se usara al reparar.
	AiConfig initCfg = loadAiConfig();
	g_ai_local_enabled = initCfg.local_enabled;
	g_ai_local_profile = initCfg.local_profile;
	g_ai_lmstudio_enabled = initCfg.lmstudio_enabled;
	g_lmstudio_endpoint = initCfg.lmstudio_endpoint;
	g_lmstudio_model = initCfg.lmstudio_model;
	g_lmstudio_timeout_seconds = initCfg.lmstudio_timeout_seconds;
	g_ai_external_enabled = initCfg.external_enabled;
	g_ai_endpoint = initCfg.endpoint;
	g_ai_model = initCfg.model;
	g_ai_api_key = initCfg.api_key;
	g_ai_timeout_seconds = initCfg.timeout_seconds;

	uiCheckboxSetChecked(Ai::chk_local_, g_ai_local_enabled);
	{
		int sel = 0;
		for (int i = 0; i < kAiLocalProfilesCount; i++)
			if (g_ai_local_profile == kAiLocalProfiles[i]) { sel = i + 1; break; }
		uiComboboxSetSelected(Ai::combo_profile_, sel);
	}
	uiCheckboxSetChecked(Ai::chk_lmstudio_, g_ai_lmstudio_enabled);
	uiEntrySetText(Ai::entry_lm_endpoint_, g_lmstudio_endpoint.c_str());
	uiEntrySetText(Ai::entry_lm_model_, g_lmstudio_model.c_str());
	uiSpinboxSetValue(Ai::spin_lm_timeout_, g_lmstudio_timeout_seconds);
	uiCheckboxSetChecked(Ai::chk_external_, g_ai_external_enabled);
	uiEntrySetText(Ai::entry_ext_endpoint_, g_ai_endpoint.c_str());
	uiEntrySetText(Ai::entry_ext_model_, g_ai_model.c_str());
	uiEntrySetText(Ai::entry_ext_key_, g_ai_api_key.c_str());
	uiSpinboxSetValue(Ai::spin_ext_timeout_, g_ai_timeout_seconds);

	// --- Cableado: cada checkbox activa/desactiva su bloque ---
	uiCheckboxOnToggled(Ai::chk_local_, [](uiCheckbox* chk_box, void* data){
		g_ai_local_enabled = uiCheckboxChecked(chk_box);
		Gui::Ai::refresh();
	}, nullptr);
	uiComboboxOnSelected(Ai::combo_profile_, [](uiCombobox* c_box, void* data){
		int sel = uiComboboxSelected(c_box);
		if (sel <= 0 || sel > kAiLocalProfilesCount) g_ai_local_profile = "";
		else g_ai_local_profile = kAiLocalProfiles[sel - 1];
		Gui::Ai::refresh();
	}, nullptr);

	uiCheckboxOnToggled(Ai::chk_lmstudio_, [](uiCheckbox* chk_box, void* data){
		auto v = uiCheckboxChecked(chk_box);
		if (v) Gui::msgBox("LM Studio (Qwen local): todo queda en localhost. Qwen solo recibe "
		                   "un diagnóstico en texto (menos de 50 KB). Abre LM Studio, carga "
		                   "Qwen y pulsa Start Server en el puerto 1234.");
		g_ai_lmstudio_enabled = v;
		Gui::Ai::refresh();
	}, nullptr);
	uiEntryOnChanged(Ai::entry_lm_endpoint_, [](uiEntry* entry, void* data){
		g_lmstudio_endpoint = entryText(entry);
	}, nullptr);
	uiEntryOnChanged(Ai::entry_lm_model_, [](uiEntry* entry, void* data){
		g_lmstudio_model = entryText(entry);
		Gui::Ai::updateStatus();
	}, nullptr);
	uiSpinboxOnChanged(Ai::spin_lm_timeout_, [](uiSpinbox* spinbox, void* data){
		g_lmstudio_timeout_seconds = uiSpinboxValue(spinbox);
	}, nullptr);
	uiButtonOnClicked(Ai::btn_lm_list_, [](uiButton* b, void* data){
		AiConfig cfg = loadAiConfig();
		cfg.lmstudio_enabled = true;
		LMStudioProvider lm(cfg);
		std::vector<std::string> models;
		std::string why;
		if (lm.listModels(models, why)) {
			std::string msg = "Modelos cargados en LM Studio (" + lm.baseUrl() + "):\n";
			for (auto& m : models) msg += "  " + m + "\n";
			msg += "Copia el identificador del Qwen que quieras usar en el campo Modelo.";
			Gui::msgBox(msg.c_str());
		} else {
			Gui::msgBoxError(std::string("LM Studio: ") + why);
		}
	}, nullptr);
	uiButtonOnClicked(Ai::btn_lm_test_, [](uiButton* b, void* data){
		AiConfig cfg = loadAiConfig();
		cfg.lmstudio_enabled = true;
		LMStudioProvider lm(cfg);
		std::vector<std::string> models;
		std::string why;
		if (lm.listModels(models, why)) {
			std::string msg = "Conectado a LM Studio en " + lm.baseUrl() + ". Modelos cargados:\n";
			for (auto& m : models) msg += "  " + m + "\n";
			Gui::msgBox(msg.c_str());
		} else {
			Gui::msgBoxError(std::string("LM Studio: ") + why);
		}
	}, nullptr);

	uiCheckboxOnToggled(Ai::chk_external_, [](uiCheckbox* chk_box, void* data){
		auto v = uiCheckboxChecked(chk_box);
		if (v) Gui::msgBox("IA externa: solo se enviará un diagnóstico en texto (menos de 50 KB: "
		                   "tamaños, SPS en hexadecimal, conteos de NAL). NUNCA se envía el vídeo, "
		                   "fotogramas ni audio. Necesitas una API key; sin ella no se envía nada.");
		g_ai_external_enabled = v;
		Gui::Ai::refresh();
	}, nullptr);
	uiEntryOnChanged(Ai::entry_ext_endpoint_, [](uiEntry* entry, void* data){
		g_ai_endpoint = entryText(entry);
	}, nullptr);
	uiEntryOnChanged(Ai::entry_ext_model_, [](uiEntry* entry, void* data){
		g_ai_model = entryText(entry);
	}, nullptr);
	uiEntryOnChanged(Ai::entry_ext_key_, [](uiEntry* entry, void* data){
		g_ai_api_key = entryText(entry);
	}, nullptr);
	uiSpinboxOnChanged(Ai::spin_ext_timeout_, [](uiSpinbox* spinbox, void* data){
		g_ai_timeout_seconds = uiSpinboxValue(spinbox);
	}, nullptr);
	uiButtonOnClicked(Ai::btn_ext_test_, [](uiButton* b, void* data){
		// "Probar" la externa es solo comprobar que esta configurada
		// (clave + endpoint valido + curl): NO envia ningun diagnostico
		// real ni hace una llamada de red. Si falta algo, isAvailable
		// explica que falta, en espanol.
		AiConfig cfg = loadAiConfig();
		cfg.external_enabled = true;
		ExternalProvider ext(cfg);
		std::string why;
		if (ext.isAvailable(why))
			Gui::msgBox("IA externa disponible: endpoint y API key configurados y curl disponible. "
			            "No se ha enviado nada todavía: la primera llamada real solo ocurre al reparar, "
			            "y solo con un diagnóstico en texto de menos de 50 KB.");
		else
			Gui::msgBoxError(std::string("IA externa no disponible: ") + why);
	}, nullptr);

	// Estado inicial: bloques apagados con sus inputs en gris.
	Ai::refresh();
	return uiControl(v_box);
}

// --- Pestaña "Entrenar" (Fase 2.5, prototipo ONNX) ---
// El usuario agrega vídeos buenos y corruptos de su PC; el script
// train_onnx.py crea el dataset y entrena el modelo pequeño, todo
// en local. La GUI solo orquesta: no entrena en C++ (eso exigiría
// PyTorch/ONNX Runtime dentro de la GUI) y nunca sube nada.
// PRIVACIDAD: ningún vídeo se guarda. Las listas de vídeos viven
// solo en memoria (buenos_/corruptos_) y nunca se escriben a disco;
// las rutas se pasan al script solo como argumentos en memoria y el
// script lee los vídeos por streaming desde su lugar original.

static std::string trainBaseName(const std::string& path) {
	auto pos = path.find_last_of("/\\");
	return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

void Gui::Train::updateLists() {
	if (list_buenos_) {
		std::string t;
		for (auto& p : buenos_) t += trainBaseName(p) + "\n";
		uiMultilineEntrySetText(list_buenos_, t.c_str());
	}
	if (list_corruptos_) {
		std::string t;
		for (auto& p : corruptos_) t += trainBaseName(p) + "\n";
		uiMultilineEntrySetText(list_corruptos_, t.c_str());
	}
	if (label_contadores_) {
		std::string s = "Vídeos buenos: " + std::to_string(buenos_.size()) +
		                "  ·  Vídeos corruptos: " + std::to_string(corruptos_.size());
		uiLabelSetText(label_contadores_, s.c_str());
	}
}

void Gui::Train::updateModelLabel() {
	if (!label_modelo_) return;
	OnnxModelInfo info;
	std::string donde, aviso;
	// El modelo que se usara DE VERDAD (salta los incompletos).
	const std::string dir = onnxModeloEnUso(g_onnx_model_path, g_exe_argv0, info, aviso);
	if (dir == onnxModelActivoDir()) {
		donde = "modelo entrenado por ti";
	} else if (dir == onnxModelAnteriorDir()) {
		donde = "copia de seguridad de tu modelo anterior";
	} else if (!dir.empty() && dir == g_onnx_model_path) {
		donde = "carpeta configurada";
	} else if (!dir.empty()) {
		donde = "modelo base incluido (sin entrenar todavía)";
	} else {
		uiLabelSetText(label_modelo_,
		               ("Modelo actual: ninguno. Restablece el modelo base o entrena "
		                "con tus vídeos para tener uno. " + aviso).c_str());
		if (chk_usar_) {
			uiCheckboxSetChecked(chk_usar_, 0);
			uiControlDisable(uiControl(chk_usar_));
			g_onnx_enabled = false;
		}
		return;
	}
	// Una sola línea corta (~110 caracteres): una uiLabel larga
	// ensanchaba la ventana a más de 1000 px. Sin el nombre interno
	// ni la fecha, que no le dicen nada al usuario.
	std::string s = "Modelo actual: " + donde + "  ·  " +
	                std::to_string(info.tamano_bytes / 1024) + " KB  ·  " +
	                std::to_string(info.ejemplos) + " ejemplos  ·  acierto " +
	                std::to_string((int)(info.precision_validacion * 100)) +
	                " % (heurística " + std::to_string((int)(info.precision_baseline * 100)) +
	                " %)";
	if (!aviso.empty()) s += "\nAviso: " + aviso;
	uiLabelSetText(label_modelo_, s.c_str());
	if (chk_usar_) uiControlEnable(uiControl(chk_usar_));
}

// --- Base persistente (§12) ---
// Lee el manifiesto del dataset guardado y arma el resumen en
// español que se muestra al reabrir la pestaña, sin cargar ningún
// vídeo y sin pedir nada: total de ejemplos, por clase, vídeos
// que aportaron (nombre base + hash corto, nunca ruta) y fecha de
// última actualización. Si no hay base, lo dice.
static std::string trainReadTextFile(const std::string& path) {
	std::ifstream f(upath::toPath(path));
	if (!f) return "";
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static bool trainJsonNumberAfter(const std::string& json, const std::string& key,
                                 size_t from, double& out) {
	size_t pos = json.find(key, from);
	if (pos == std::string::npos) return false;
	pos += key.size();
	while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) pos++;
	size_t end = pos;
	if (end < json.size() && (json[end] == '-' || json[end] == '+')) end++;
	while (end < json.size() && (isdigit((unsigned char)json[end]) || json[end] == '.')) end++;
	if (end == pos) return false;
	try { out = std::stod(json.substr(pos, end - pos)); } catch (...) { return false; }
	return true;
}

static std::string trainJsonStringAfter(const std::string& json, const std::string& key,
                                        size_t from) {
	size_t pos = json.find(key, from);
	if (pos == std::string::npos) return "";
	pos = json.find('"', pos + key.size());
	if (pos == std::string::npos) return "";
	size_t end = json.find('"', pos + 1);
	if (end == std::string::npos) return "";
	return json.substr(pos + 1, end - pos - 1);
}

std::string Gui::Train::baseSummary() {
	std::string man_path = upath::fromPath(upath::toPath(onnxDatasetDir()) /
	                                       "dataset_manifest.json");
	std::string man = trainReadTextFile(man_path);
	if (man.empty())
		return "Sin base todavía: agrega vídeos buenos y pulsa "
		       "«Entrenar modelo». No hay dataset guardado.";
	double total = 0;
	trainJsonNumberAfter(man, "\"total\"", 0, total);
	std::string s = "Base guardada: " + std::to_string((long long)total) + " ejemplos";
	size_t pc = man.find("\"ejemplos_por_clase\"");
	if (pc != std::string::npos) {
		size_t a = man.find('[', pc), b = man.find(']', pc);
		if (a != std::string::npos && b != std::string::npos && b > a) {
			std::string body = man.substr(a + 1, b - a - 1);
			std::stringstream ss(body);
			std::string tok;
			std::vector<long long> vals;
			while (std::getline(ss, tok, ',')) {
				try { vals.push_back(std::stoll(tok)); } catch (...) {}
			}
			static const char* nombres[] = {"basura", "vídeo", "audio", "SPS"};
			s += " (";
			for (size_t i = 0; i < vals.size() && i < 4; i++) {
				if (i) s += " · ";
				s += std::string(nombres[i]) + " " + std::to_string(vals[i]);
			}
			s += ")";
		}
	}
	std::string act = trainJsonStringAfter(man, "\"actualizado\"", 0);
	if (!act.empty()) s += "  ·  actualizada " + act;
	size_t arch = man.find("\"archivos\"");
	if (arch != std::string::npos) {
		std::string lista;
		size_t pos = arch;
		while (true) {
			size_t np = man.find("\"nombre\":", pos);
			if (np == std::string::npos) break;
			// Cada entrada es un objeto {...}: buscar solo dentro
			// de él, para no atribuirle los ejemplos del siguiente.
			size_t fin_obj = man.find('}', np);
			if (fin_obj == std::string::npos) break;
			std::string obj = man.substr(np, fin_obj - np);
			std::string nombre = trainJsonStringAfter(obj, "\"nombre\"", 0);
			std::string hash = trainJsonStringAfter(obj, "\"hash\"", 0);
			double ej = 0;
			trainJsonNumberAfter(obj, "\"ejemplos\"", 0, ej);
			if (!nombre.empty() && ej > 0) {
				if (!lista.empty()) lista += "; ";
				lista += nombre + " (" + (hash.empty() ? "?" : hash) + ") — " +
				         std::to_string((long long)ej) + " ejemplos";
			}
			pos = fin_obj + 1;
		}
		if (!lista.empty()) s += "\nVídeos que aportaron: " + lista + ".";
	}
	s += "\nNo tienes que volver a cargar estos vídeos: la base ya está "
	     "guardada. Para añadir más, agrega vídeos nuevos y pulsa "
	     "«Agregar más videos y re-entrenar».";
	return s;
}

void Gui::Train::updateBaseLabel() {
	if (!label_base_) return;
	// Solo la primera línea (totales): el resumen trae además la lista de
	// archivos, una por línea, y en Windows libui calcula el ancho de la
	// etiqueta con TODO el texto en una línea, así que la ventana se
	// ensanchaba hasta ~1900 px. La lista completa está en «Ver base».
	std::string s = baseSummary();
	size_t nl = s.find('\n');
	if (nl != std::string::npos) s.resize(nl);
	uiLabelSetText(label_base_, s.c_str());
}

void Gui::Train::refresh() {
	updateLists();
	updateBaseLabel();
	updateModelLabel();
	bool hay_buenos = !buenos_.empty();
	bool hay_alguno = hay_buenos || !corruptos_.empty();
	if (btn_dataset_) {
		if (hay_alguno) uiControlEnable(uiControl(btn_dataset_));
		else uiControlDisable(uiControl(btn_dataset_));
	}
	if (btn_anexar_) {
		// Anexar necesita al menos un vídeo bueno nuevo en la lista.
		if (hay_buenos) uiControlEnable(uiControl(btn_anexar_));
		else uiControlDisable(uiControl(btn_anexar_));
	}
	if (btn_ver_base_) {
		std::error_code ec;
		bool hay_base = std::filesystem::exists(
		    upath::toPath(onnxDatasetDir()) / "dataset_manifest.json", ec);
		if (hay_base) uiControlEnable(uiControl(btn_ver_base_));
		else uiControlDisable(uiControl(btn_ver_base_));
	}
	if (btn_borrar_) {
		std::error_code ec;
		bool hay_base = std::filesystem::exists(
		    upath::toPath(onnxDatasetDir()) / "dataset.npz", ec);
		if (hay_base) uiControlEnable(uiControl(btn_borrar_));
		else uiControlDisable(uiControl(btn_borrar_));
	}
	if (btn_train_) {
		// §12: se puede entrenar con vídeos nuevos de la lista
		// (hay_buenos) o, al reabrir, sobre la base guardada que
		// ya hay, sin volver a cargar ningún vídeo.
		std::error_code ec;
		bool hay_base = std::filesystem::exists(
		    upath::toPath(onnxDatasetDir()) / "dataset.npz", ec);
		if (hay_buenos || hay_base) uiControlEnable(uiControl(btn_train_));
		else uiControlDisable(uiControl(btn_train_));
	}
	if (btn_test_) {
		std::error_code ec;
		bool hay_dataset = std::filesystem::exists(
		    upath::toPath(onnxDatasetDir()) / "dataset.npz", ec);
		if (hay_dataset) uiControlEnable(uiControl(btn_test_));
		else uiControlDisable(uiControl(btn_test_));
	}
}

void Gui::Train::addFile(bool bueno) {
	// Punto 10: se pueden elegir varios vídeos a la vez en el mismo
	// diálogo; todos se agregan sin duplicar los que ya están.
	auto files = openVideoFilesMulti();
	if (files.empty()) return;
	if (bueno) MultiOpen::agregarSinDuplicados(buenos_, files);
	else MultiOpen::agregarSinDuplicados(corruptos_, files);
	// Las listas NO se guardan en disco (privacidad): solo en memoria.
	refresh();
}

void Gui::Train::onScriptFinished(void* data) {
	auto* msg = static_cast<std::string*>(data);
	std::string texto = *msg;
	delete msg;
	if (label_estado_) uiLabelSetText(label_estado_, texto.c_str());
	if (progressbar_) uiProgressBarSetValue(progressbar_, 100);
	// Punto 3: mostrar la salida REAL del script en la ventana de
	// detalles (capturada por runTrainScript). Si está vacía, se
	// dice explícitamente, para no volver a la situación anterior
	// ("revisa la ventana de detalles" y nada dentro).
	if (text_detalles_) {
		std::string out = lastTrainScriptOutput();
		uiMultilineEntrySetText(text_detalles_,
		                        out.empty() ? "Sin salida del script." : out.c_str());
	}
	if (thread_) {
		thread_->join();
		delete thread_;
		thread_ = nullptr;
	}
	refresh();
	Gui::msgBox(texto.c_str());
}

// Punto 2: el motor elegido en el combobox de la pestaña Entrenar.
std::string Gui::Train::motorSeleccionado() {
	int sel = combo_motor_ ? uiComboboxSelected(combo_motor_) : 0;
	if (sel == 1) return "numpy";
	if (sel == 2) return "torch";
	return "auto";
}

void Gui::Train::runScriptAsync(const std::vector<std::string>& args,
                                const char* done_msg_es) {
	if (thread_) {
		Gui::msgBox("Ya hay un entrenamiento o una prueba en marcha. Espera a que termine.");
		return;
	}
	if (label_estado_)
		uiLabelSetText(label_estado_, "Trabajando… (esto puede tardar minutos; no cierres la ventana)");
	if (progressbar_) uiProgressBarSetValue(progressbar_, 50);
	// Punto 3: mientras el script corre, la ventana de detalles
	// dice que está trabajando (la salida aparece al terminar).
	if (text_detalles_)
		uiMultilineEntrySetText(text_detalles_,
		                        "Ejecutando el script… (la salida aparecerá aquí al terminar).");
	thread_ = new std::thread([args, done_msg_es]() {
		std::string why;
		int rc = runTrainScript(args, why, g_exe_argv0);
		std::string* msg = new std::string();
		if (rc == 0)
			*msg = std::string("Listo: ") + done_msg_es + ".";
		else if (rc == 4)
			*msg = std::string("El modelo entrenado NO superó a la heurística y NO reemplazó "
			                   "al modelo activo. Quedó como candidato para revisarlo. (") +
			       done_msg_es + ")";
		else
			*msg = std::string("Error al ") + done_msg_es + ": " + why;
		ASYNC(Gui::Train::onScriptFinished, msg);
	});
}

uiControl* Gui::trainTab() {
	auto v_box = newVerticalBox();

	auto label_priv = uiNewLabel("No se guarda ningún video: solo se extraen ventanas de 256 bytes");
	auto label_priv2 = uiNewLabel("con su etiqueta para entrenar. Tus videos quedan en su lugar original.");
	auto label_priv3 = uiNewLabel("Nada se sube a ningún servidor.");
	uiBoxAppend(v_box, uiControl(label_priv), 0);
	uiBoxAppend(v_box, uiControl(label_priv2), 0);
	uiBoxAppend(v_box, uiControl(label_priv3), 0);

	Train::label_contadores_ = uiNewLabel("Vídeos buenos: 0  ·  Vídeos corruptos: 0");
	uiBoxAppend(v_box, uiControl(Train::label_contadores_), 0);

	// §12: resumen de la BASE PERSISTENTE guardada. Se actualiza
	// en Train::refresh() al abrir la pestaña, sin pedir vídeos:
	// si ya entrenaste antes, aquí ves que tu base sigue ahí.
	Train::label_base_ = uiNewLabel("Sin base todavía: agrega vídeos buenos y pulsa "
	                                "«Entrenar modelo». No hay dataset guardado.");
	uiBoxAppend(v_box, uiControl(Train::label_base_), 0);

	// --- Bloque 1: vídeos para entrenar ---
	auto group_listas = newGroup("1 · Vídeos para entrenar (de tu PC)");
	auto v_listas = newVerticalBox();
	auto h_buenos = newHorizontalBox();
	auto btn_add_bueno = newButton("Agregar video bueno");
	auto btn_del_bueno = newButton("Quitar último bueno");
	auto btn_clear_buenos = newButton("Limpiar buenos");
	uiBoxAppend(h_buenos, uiControl(btn_add_bueno), 0);
	uiBoxAppend(h_buenos, uiControl(btn_del_bueno), 0);
	uiBoxAppend(h_buenos, uiControl(btn_clear_buenos), 0);
	Train::list_buenos_ = uiNewMultilineEntry();
	uiMultilineEntrySetReadOnly(Train::list_buenos_, 1);
	auto h_corruptos = newHorizontalBox();
	auto btn_add_corrupto = newButton("Agregar video corrupto");
	auto btn_del_corrupto = newButton("Quitar último corrupto");
	auto btn_clear_corruptos = newButton("Limpiar corruptos");
	uiBoxAppend(h_corruptos, uiControl(btn_add_corrupto), 0);
	uiBoxAppend(h_corruptos, uiControl(btn_del_corrupto), 0);
	uiBoxAppend(h_corruptos, uiControl(btn_clear_corruptos), 0);
	Train::list_corruptos_ = uiNewMultilineEntry();
	uiMultilineEntrySetReadOnly(Train::list_corruptos_, 1);
	auto label_listas_info = uiNewLabel("Buenos: vídeos que se reproducen completos (su moov da las etiquetas reales).");
	auto label_listas_info2 = uiNewLabel("Corruptos: vídeos sin moov; sus etiquetas son débiles y solo refuerzan lo que ya falló.");
	uiBoxAppend(v_listas, uiControl(h_buenos), 0);
	uiBoxAppend(v_listas, uiControl(Train::list_buenos_), 1);
	uiBoxAppend(v_listas, uiControl(h_corruptos), 0);
	uiBoxAppend(v_listas, uiControl(Train::list_corruptos_), 1);
	uiBoxAppend(v_listas, uiControl(label_listas_info), 0);
	uiBoxAppend(v_listas, uiControl(label_listas_info2), 0);
	uiGroupSetChild(group_listas, uiControl(v_listas));
	// Punto 7 (reparaciones 2026-11-07): que los bloques con
	// multilínea CREZCAN con la ventana. Antes los tres grupos
	// iban con stretch 0: libui les daba su altura mínima y las
	// listas de vídeos solo mostraban 1 línea aunque la ventana
	// estuviera a pantalla completa. Ahora group_listas y
	// group_train son stretchy (1) y se reparten el espacio extra;
	// dentro de cada uno, las listas y la ventana de detalles ya
	// eran stretchy (1). group_model sigue fijo (0): solo tiene
	// etiquetas y botones. Sin funciones libui nuevas.
	// Altura (2026-11-12): los tres bloques apilados sumaban 910 px y la
	// ventana no cabía en 768 px. Ahora cada bloque es una sub-pestaña.
	auto sub_train = newTab();
	uiTabAppend(sub_train, "1 · Vídeos", uiControl(group_listas));

	// --- Bloque 2: dataset y entrenamiento ---
	auto group_train = newGroup("2 · Dataset y entrenamiento (en esta PC)");
	auto v_train = newVerticalBox();
	auto h_train_btns = newHorizontalBox();
	Train::btn_dataset_ = newButton("Crear dataset desde cero");
	Train::btn_anexar_ = newButton("Agregar más videos y re-entrenar");
	Train::btn_ver_base_ = newButton("Ver base de entrenamiento");
	Train::btn_train_ = newButton("Entrenar modelo");
	Train::btn_test_ = newButton("Probar modelo");
	uiBoxAppend(h_train_btns, uiControl(Train::btn_dataset_), 0);
	uiBoxAppend(h_train_btns, uiControl(Train::btn_anexar_), 0);
	uiBoxAppend(h_train_btns, uiControl(Train::btn_ver_base_), 0);
	uiBoxAppend(h_train_btns, uiControl(Train::btn_train_), 0);
	uiBoxAppend(h_train_btns, uiControl(Train::btn_test_), 0);
	Train::progressbar_ = uiNewProgressBar();
	Train::label_estado_ = uiNewLabel("Sin entrenar todavía. Añade vídeos buenos y pulsa «Entrenar modelo».");
	// Punto 5 (reparaciones 2026-11-07): la etiqueta larga se
	// cortaba en el borde derecho ("Si el modelo no supera a...").
	// libui no hace scroll ni envuelve: se parte en 4 líneas
	// cortas que caben en 820×640. Mismo contenido, sin añadir
	// funciones libui nuevas.
	auto label_train_info1 = uiNewLabel("«Crear dataset desde cero» BORRA la base guardada (pide confirmación).");
	auto label_train_info2 = uiNewLabel("«Agregar más videos y re-entrenar» AÑADE los nuevos sin borrar nada.");
	auto label_train_info3 = uiNewLabel("Entrenar usa el Python + numpy que vienen incluidos (no hay que instalar nada).");
	auto label_train_info4 = uiNewLabel("Si el modelo no supera a la heurística, NO reemplaza al activo.");
	// Punto 2 (reparaciones 2026-11-07): elegir el motor. En la PC
	// de Angel PyTorch está roto (incompatible con transformers) y
	// con "Automático" el script ya cae a NumPy solo, pero aquí
	// queda la opción visible para forzar NumPy sin depender del
	// fallback. El valor se pasa como --motor a train_onnx.py.
	auto h_motor = newHorizontalBox();
	auto label_motor = uiNewLabel("Motor de entrenamiento:");
	Train::combo_motor_ = uiNewCombobox();
	uiComboboxAppend(Train::combo_motor_, "Automático (recomendado)");
	uiComboboxAppend(Train::combo_motor_, "NumPy (si PyTorch falla en tu PC)");
	uiComboboxAppend(Train::combo_motor_, "PyTorch (CPU)");
	uiComboboxSetSelected(Train::combo_motor_, 0);
	uiBoxAppend(h_motor, uiControl(label_motor), 0);
	uiBoxAppend(h_motor, uiControl(Train::combo_motor_), 0);
	// Punto 3 (reparaciones 2026-11-07): ventana de detalles con la
	// salida REAL del script de entrenamiento. Antes el diálogo
	// decía "revisa el mensaje en la ventana de detalles" y ahí no
	// había nada, porque runTrainScript descartaba stdout/stderr.
	// Ahora la salida se captura y se muestra aquí (y, en error,
	// sus últimas líneas en el diálogo). Solo contiene nombres
	// base de vídeos, conteos y errores: nunca vídeos ni rutas.
	Train::text_detalles_ = uiNewMultilineEntry();
	uiMultilineEntrySetReadOnly(Train::text_detalles_, 1);
	uiMultilineEntrySetText(Train::text_detalles_,
	                        "Detalles del script: aquí aparecerá la salida real del "
	                        "entrenamiento (qué hizo el script y, si falla, por qué).");
	uiBoxAppend(v_train, uiControl(h_train_btns), 0);
	uiBoxAppend(v_train, uiControl(h_motor), 0);
	uiBoxAppend(v_train, uiControl(Train::progressbar_), 0);
	uiBoxAppend(v_train, uiControl(Train::label_estado_), 0);
	uiBoxAppend(v_train, uiControl(label_train_info1), 0);
	uiBoxAppend(v_train, uiControl(label_train_info2), 0);
	uiBoxAppend(v_train, uiControl(label_train_info3), 0);
	uiBoxAppend(v_train, uiControl(label_train_info4), 0);
	uiBoxAppend(v_train, uiControl(Train::text_detalles_), 1);
	uiGroupSetChild(group_train, uiControl(v_train));
	// Punto 7: stretchy (ver group_listas arriba): así la ventana
	// de detalles (ya stretchy dentro de v_train) crece con la
	// ventana y se ven varias líneas de la salida del script.
	uiTabAppend(sub_train, "2 · Entrenar", uiControl(group_train));

	// --- Bloque 3: modelo actual ---
	auto group_model = newGroup("3 · Modelo actual");
	auto v_model = newVerticalBox();
	Train::label_modelo_ = uiNewLabel("Modelo actual: ninguno.");
	Train::chk_usar_ = uiNewCheckbox("Usar modelo entrenado en la reparación");
	auto btn_reset = newButton("Restablecer modelo base");
	Train::btn_borrar_ = newButton("Borrar base de entrenamiento");
	auto label_model_info = uiNewLabel("El modelo base incluido es un MLP pequeño (264->64->32->4, ~77 KB).");
	auto label_model_info2 = uiNewLabel("El modelo mejora en cada PC con sus propios vídeos; con pocos vídeos");
	auto label_model_info3 = uiNewLabel("de una sola cámara puede sobreajustarse a esa cámara.");
	auto label_model_info4 = uiNewLabel("«Borrar base de entrenamiento» borra el dataset y el modelo entrenado");
	auto label_model_info5 = uiNewLabel("y vuelve al modelo base; tus vídeos originales nunca se tocan.");
	uiBoxAppend(v_model, uiControl(Train::label_modelo_), 0);
	uiBoxAppend(v_model, uiControl(Train::chk_usar_), 0);
	uiBoxAppend(v_model, uiControl(btn_reset), 0);
	uiBoxAppend(v_model, uiControl(Train::btn_borrar_), 0);
	uiBoxAppend(v_model, uiControl(label_model_info), 0);
	uiBoxAppend(v_model, uiControl(label_model_info2), 0);
	uiBoxAppend(v_model, uiControl(label_model_info3), 0);
	uiBoxAppend(v_model, uiControl(label_model_info4), 0);
	uiBoxAppend(v_model, uiControl(label_model_info5), 0);
	uiGroupSetChild(group_model, uiControl(v_model));
	uiTabAppend(sub_train, "3 · Modelo", uiControl(group_model));
	uiBoxAppend(v_box, uiControl(sub_train), 1);

	// --- Cableado ---
	// Las listas empiezan vacías en cada sesión: no se cargan de
	// disco a propósito (privacidad, ver gui.h struct Train).
	uiButtonOnClicked(btn_add_bueno, [](uiButton* b, void* data){
		Gui::Train::addFile(true);
	}, nullptr);
	uiButtonOnClicked(btn_del_bueno, [](uiButton* b, void* data){
		if (!Gui::Train::buenos_.empty()) Gui::Train::buenos_.pop_back();
		Gui::Train::refresh();
	}, nullptr);
	uiButtonOnClicked(btn_clear_buenos, [](uiButton* b, void* data){
		Gui::Train::buenos_.clear();
		Gui::Train::refresh();
	}, nullptr);
	uiButtonOnClicked(btn_add_corrupto, [](uiButton* b, void* data){
		Gui::Train::addFile(false);
	}, nullptr);
	uiButtonOnClicked(btn_del_corrupto, [](uiButton* b, void* data){
		if (!Gui::Train::corruptos_.empty()) Gui::Train::corruptos_.pop_back();
		Gui::Train::refresh();
	}, nullptr);
	uiButtonOnClicked(btn_clear_corruptos, [](uiButton* b, void* data){
		Gui::Train::corruptos_.clear();
		Gui::Train::refresh();
	}, nullptr);

	// §12: "Crear dataset desde cero" BORRA la base guardada, así
	// que pide confirmación en dos pasos: el primer clic solo
	// avisa y arma la confirmación; el segundo ejecuta.
	uiButtonOnClicked(Train::btn_dataset_, [](uiButton* b, void* data){
		if (!Gui::Train::confirm_cero_armed_) {
			Gui::Train::confirm_cero_armed_ = true;
			Gui::msgBox("Vas a BORRAR la base de entrenamiento guardada "
			            "(dataset + modelo entrenado) y crear una nueva "
			            "desde cero con los vídeos de la lista. Tus vídeos "
			            "originales no se borran. Si estás seguro, pulsa de "
			            "nuevo «Crear dataset desde cero».");
			if (Gui::Train::label_estado_)
				uiLabelSetText(Gui::Train::label_estado_,
				               "Confirmación: pulsa de nuevo «Crear dataset desde cero» "
				               "para borrar la base guardada y crear una nueva.");
			return;
		}
		Gui::Train::confirm_cero_armed_ = false;
		std::vector<std::string> args = {"crear-dataset", "--salida", onnxDatasetDir()};
		if (!Gui::Train::buenos_.empty()) {
			args.push_back("--buenos");
			for (auto& p : Gui::Train::buenos_) args.push_back(p);
		}
		if (!Gui::Train::corruptos_.empty()) {
			args.push_back("--corruptos");
			for (auto& p : Gui::Train::corruptos_) args.push_back(p);
		}
		Gui::Train::runScriptAsync(args, "dataset creado desde cero");
	}, nullptr);
	// §12: "Agregar más videos y re-entrenar" ANEXA los vídeos
	// nuevos de la lista a la base guardada, sin borrarla y sin
	// volver a cargar los vídeos anteriores (entrenar --anexar).
	uiButtonOnClicked(Train::btn_anexar_, [](uiButton* b, void* data){
		std::vector<std::string> args = {"entrenar", "--anexar", "--motor",
		                               Gui::Train::motorSeleccionado()};
		if (!Gui::Train::buenos_.empty()) {
			args.push_back("--buenos");
			for (auto& p : Gui::Train::buenos_) args.push_back(p);
		}
		if (!Gui::Train::corruptos_.empty()) {
			args.push_back("--corruptos");
			for (auto& p : Gui::Train::corruptos_) args.push_back(p);
		}
		Gui::Train::runScriptAsync(args, "vídeos nuevos anexados y modelo re-entrenado");
	}, nullptr);
	// §12: "Ver base de entrenamiento" muestra el resumen de lo
	// que quedó guardado, sin cargar ningún vídeo.
	uiButtonOnClicked(Train::btn_ver_base_, [](uiButton* b, void* data){
		Gui::msgBox(Gui::Train::baseSummary().c_str());
	}, nullptr);
	uiButtonOnClicked(Train::btn_train_, [](uiButton* b, void* data){
		// Las rutas se pasan como argumentos en memoria; el script
		// las lee por streaming y nunca las escribe a disco.
		// Sin vídeos nuevos en la lista, entrena sobre la base
		// guardada que ya hay (§12: al reabrir no hace falta
		// recargar nada para re-entrenar).
		std::vector<std::string> args = {"entrenar", "--motor",
		                               Gui::Train::motorSeleccionado()};
		if (!Gui::Train::buenos_.empty()) {
			args.push_back("--buenos");
			for (auto& p : Gui::Train::buenos_) args.push_back(p);
		}
		if (!Gui::Train::corruptos_.empty()) {
			args.push_back("--corruptos");
			for (auto& p : Gui::Train::corruptos_) args.push_back(p);
		}
		Gui::Train::runScriptAsync(args, "entrenamiento terminado");
	}, nullptr);
	uiButtonOnClicked(Train::btn_test_, [](uiButton* b, void* data){
		Gui::Train::runScriptAsync(
		    {"probar", "--modelo", onnxModelActivoDir(), "--dataset", onnxDatasetDir()},
		    "prueba del modelo terminada");
	}, nullptr);

	uiCheckboxSetChecked(Train::chk_usar_, g_onnx_enabled);
	uiCheckboxOnToggled(Train::chk_usar_, [](uiCheckbox* chk_box, void* data){
		auto v = uiCheckboxChecked(chk_box);
		OnnxModelInfo info;
		std::string aviso;
		if (v && onnxModeloEnUso(g_onnx_model_path, g_exe_argv0, info, aviso).empty()) {
			Gui::msgBoxError(("No hay ningún modelo ONNX válido para usar. Restablece el "
			                  "modelo base o entrena con tus vídeos. " + aviso).c_str());
			uiCheckboxSetChecked(chk_box, 0);
			g_onnx_enabled = false;
			guardarAiConf("onnx_enabled", "0");
			return;
		}
		g_onnx_enabled = v;
		// Se recuerda para la proxima vez que se abra el programa.
		if (!guardarAiConf("onnx_enabled", v ? "1" : "0"))
			Gui::msgBoxError(("No se pudo guardar la preferencia en " + aiConfPath() +
			                  ". Funcionará en esta sesión, pero habrá que volver a "
			                  "activarla la próxima vez.").c_str());
	}, nullptr);
	uiButtonOnClicked(btn_reset, [](uiButton* b, void* data){
		std::string base = findBaseModelDir(g_exe_argv0);
		std::string destino = onnxModelActivoDir();
		if (base.empty() || destino.empty()) {
			Gui::msgBoxError("No se encontró el modelo base incluido (models/onnx_base).");
			return;
		}
		std::string why;
		int rc = runTrainScript({"restablecer", "--base", base, "--destino", destino}, why, g_exe_argv0);
		if (rc == 0) {
			Gui::msgBox("Modelo base restablecido: se vuelve al modelo mínimo incluido.");
		} else {
			Gui::msgBoxError(std::string("No se pudo restablecer el modelo base: ") + why);
		}
		Gui::Train::refresh();
	}, nullptr);
	// §12: "Borrar base de entrenamiento" borra el dataset y el
	// modelo entrenado y vuelve al modelo base mínimo. También
	// pide confirmación en dos pasos; los vídeos originales del
	// usuario nunca se tocan (no están en la carpeta de
	// entrenamiento).
	uiButtonOnClicked(Train::btn_borrar_, [](uiButton* b, void* data){
		if (!Gui::Train::confirm_borrar_armed_) {
			Gui::Train::confirm_borrar_armed_ = true;
			Gui::msgBox("Vas a BORRAR la base de entrenamiento guardada "
			            "(dataset + modelo entrenado) y volver al modelo "
			            "base mínimo incluido. Tus vídeos originales no se "
			            "borran. Si estás seguro, pulsa de nuevo "
			            "«Borrar base de entrenamiento».");
			if (Gui::Train::label_estado_)
				uiLabelSetText(Gui::Train::label_estado_,
				               "Confirmación: pulsa de nuevo «Borrar base de entrenamiento» "
				               "para borrar la base guardada.");
			return;
		}
		Gui::Train::confirm_borrar_armed_ = false;
		std::string base = findBaseModelDir(g_exe_argv0);
		std::string why;
		std::vector<std::string> args = {"borrar-base", "--confirmar",
		                                 "--dataset", onnxDatasetDir(),
		                                 "--destino", onnxModelActivoDir()};
		if (!base.empty()) { args.push_back("--base"); args.push_back(base); }
		int rc = runTrainScript(args, why, g_exe_argv0);
		if (rc == 0) {
			Gui::msgBox("Base de entrenamiento borrada. Se volvió al modelo "
			            "base mínimo incluido. Tus vídeos originales no se tocaron.");
		} else {
			Gui::msgBoxError(std::string("No se pudo borrar la base: ") + why);
		}
		Gui::Train::refresh();
	}, nullptr);

	Train::refresh();
	return uiControl(v_box);
}

// --- Pestaña "Asistente" (Fase 3, 3.1) ---
// Flujo guiado en 3 pasos, sin siglas y sin linea de comandos.
// Decisiones (ver FASE3_ASISTENTE.md):
//  - Nueva pestana "Asistente" PRIMERA; "Reparar" sigue igual como
//    ruta manual/avanzada. No se renombra ni se toca nada existente.
//  - Lote: NO se reutiliza repairBatch() tal cual, porque no tiene
//    gancho de progreso por archivo; el bucle del asistente llama
//    a la misma repairFileWithAutoRetry() por archivo (Fase 1), asi
//    que el comportamiento de reparacion es identico.
//  - Sin referencia: rescueWithoutReference() por archivo (Fase 2).
//  - Vista previa: libui NO tiene control de imagen, asi que la
//    "miniatura" es el primer fotograma extraido con ffmpeg a
//    Reparados/ y abierto con el visor del sistema; los primeros
//    5 s se reproducen con ffplay si esta disponible. El ZIP actual
//    (2026-11-01) NO trae ffmpeg.exe ni ffplay.exe, solo DLLs: si
//    no estan en el PATH, la extraccion avisa y queda el boton
//    "Abrir video reparado" (reproductor por defecto). Incluir
//    ffmpeg/ffplay en el ZIP es trabajo de 3.2.
//  - Arrastrar y soltar: NO se puede con libui (no hay API de
//    drop). Solo botones Agregar/Quitar. Punto 10: el diálogo ya
//    permite elegir varios archivos a la vez (en Windows, diálogo
//    nativo con OFN_ALLOWMULTISELECT; en Linux/macOS, uiOpenFile
//    sigue siendo de un archivo).
//  - Sin scroll en libui (uiBox no tiene scroll): la pestana se
//    compacta con etiquetas cortas y dos areas de texto con
//    estirado; la ventana sigue en 820x640. Si no cabe en pantallas
//    pequenas, la salida es agrandar la ventana en Gui::init(), no
//    anadir scroll (no existe en libui).
//  - Funciones libui nuevas: 0. Todo lo usado (uiNewLabel,
//    uiButtonOnClicked, uiEntryText, uiNewCombobox,
//    uiComboboxAppend/Selected/SetSelected/OnSelected,
//    uiNewMultilineEntry, uiMultilineEntrySetText/SetReadOnly,
//    uiNewProgressBar, uiProgressBarSetValue, uiGroupSetChild,
//    uiBoxAppend, uiControlEnable/Disable, uiQueueMain, uiOpenFile,
//    uiFreeText, uiMsgBox, mas newButton/newEntry/newGroup de
//    GuiHelper, que envuelven uiNewButton/uiNewEntry/uiNewGroup)
//    ya existia en gui.cpp. Lo demas es C++ estandar y llamadas a
//    repair_job / hybrid / formatos, que ya usa la GUI.

// Solo el nombre del archivo, para que las listas no ocupen tres
// lineas por ruta larga (igual que hace Train con trainBaseName).
static std::string assistantBaseName(const std::string& path) {
	auto pos = path.find_last_of("/\\");
	return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

// Entrecomillado POSIX de un argumento para la shell, igual que el
// shellQuote() de formatos.cpp (rutas con espacios o caracteres
// raros no deben romper el comando ni permitir inyeccion). En
// Windows (MSYS2/cmd) se usa la misma convencion que formatos.cpp.
// Comillas de la shell de std::system(): dobles en Windows (cmd.exe),
// simples en POSIX. Antes eran siempre simples y en Windows ffmpeg
// recibia la ruta con las comillas dentro (la miniatura fallaba).
static std::string assistantQuote(const std::string& s) { return shell::quote(s); }

#ifdef _WIN32
// Entrecomillado para el comando `start` de cmd.exe (comillas
// dobles). Solo se usa para ffplay / abrir con la app por defecto.
static std::string assistantQuoteWin(const std::string& s) {
	std::string out = "\"";
	for (char c : s) {
		if (c == '"') out += "\\\"";
		else out += c;
	}
	out += "\"";
	return out;
}
#endif

// Abre un archivo con la aplicacion por defecto del sistema
// (reproductor de video o visor de imagenes, segun el archivo).
// No bloquea: se lanza en el hilo que llama (los llamantes usan
// un hilo aparte para no congelar la interfaz).
static void assistantOpenWithDefaultApp(const std::string& path) {
	std::string cmd;
#ifdef _WIN32
	cmd = "start \"\" " + assistantQuoteWin(path);
#elif defined(__APPLE__)
	cmd = "open " + assistantQuote(path);
#else
	cmd = "xdg-open " + assistantQuote(path) + " > /dev/null 2>&1 &";
#endif
	upath::system(cmd);
}

// Primer fotograma: <salida>_preview.jpg junto al reparado, en
// Reparados/. p. ej. truncado.mp4_fixed.mp4 -> truncado.mp4_fixed_preview.jpg
static std::string assistantPreviewImagePath(const std::string& output) {
	std::filesystem::path p = upath::toPath(output);
	p.replace_extension("_preview.jpg");
	return upath::fromPath(p);
}

// Datos que viajan al hilo principal cuando una conversion termina:
// que posicion de la lista se reemplaza por el MP4 convertido.
struct AssistantConverted {
	int index;
	std::string path;
};

void Gui::Assistant::updateList() {
	if (list_danados_) {
		std::string t;
		for (auto& p : danados_) t += assistantBaseName(p) + "\n";
		uiMultilineEntrySetText(list_danados_, t.c_str());
	}
	if (label_contadores_) {
		std::string s = "Vídeos dañados: " + std::to_string(danados_.size());
		uiLabelSetText(label_contadores_, s.c_str());
	}
}

void Gui::Assistant::updateFormatsLabel() {
	if (!label_formatos_) return;
	int directos = 0, convertir = 0, desconocidos = 0;
	bool hay_convertible = false;
	for (auto& p : danados_) {
		FormatInfo fi = detectFormat(p);
		if (fi.is_bmff) directos++;
		else if (fi.convertible) { convertir++; hay_convertible = true; }
		else desconocidos++;
	}
	std::string s = "Formatos: " + std::to_string(directos) + " directos (MP4, MOV, M4V, 3GP)";
	s += "  ·  " + std::to_string(convertir) + " para convertir a MP4";
	s += "  ·  " + std::to_string(desconocidos) + " desconocidos (no se pueden reparar)";
	uiLabelSetText(label_formatos_, s.c_str());
	if (btn_convertir_) {
		if (!danados_.empty() && hay_convertible) uiControlEnable(uiControl(btn_convertir_));
		else uiControlDisable(uiControl(btn_convertir_));
	}
}

void Gui::Assistant::updatePreviewTarget() {
	bool hay = !salidas_.empty();
	if (preview_index_ < 0) preview_index_ = 0;
	if (hay && preview_index_ >= (int)salidas_.size())
		preview_index_ = (int)salidas_.size() - 1;
	if (label_preview_target_) {
		if (!hay) {
			uiLabelSetText(label_preview_target_,
			               "Vista previa: todavía no hay ningún vídeo reparado en este lote.");
		} else {
			std::string s = "Vista previa: " + std::to_string(preview_index_ + 1) +
			                " de " + std::to_string(salidas_.size()) + " — " +
			                assistantBaseName(salidas_[preview_index_]);
			uiLabelSetText(label_preview_target_, s.c_str());
		}
	}
	uiButton* btns[] = {btn_preview_prev_, btn_preview_next_, btn_preview_5s_,
	                    btn_preview_foto_, btn_preview_abrir_};
	for (auto* b : btns) {
		if (!b) continue;
		if (hay) uiControlEnable(uiControl(b));
		else uiControlDisable(uiControl(b));
	}
}

// Plantillas de camara en el Asistente. libui solo permite AÑADIR
// opciones a un desplegable, asi que se guarda aparte el id de cada
// plantilla en el mismo orden en que se anadio (despues de los perfiles).
static std::vector<std::string> g_asist_plantillas;
static uiEntry* g_asist_nombre_plantilla = nullptr;
static uiButton* g_asist_btn_plantilla = nullptr;

void Gui::Assistant::anadirPlantilla(const std::string& p_id, const std::string& resumen) {
	for (const auto& id : g_asist_plantillas)
		if (id == p_id) return;  // ya estaba (se reemplazo el archivo)
	std::string t = "Plantilla: " + p_id + "  (" + resumen + ")";
	uiComboboxAppend(combo_perfil_, t.c_str());
	g_asist_plantillas.push_back(p_id);
}

// Guarda la cabecera del video de referencia del Paso 2 como plantilla
// de camara (plantillas.h): asi, la proxima vez, "No tengo referencia"
// recupera como si se tuviera ese video sano.
void Gui::Assistant::guardarPlantilla(uiButton*, void*) {
	if (Gui::thread_) {
		Gui::msgBox("Hay una reparación en marcha. Espera a que termine para guardar la plantilla.");
		return;
	}
	std::string ref = Gui::Assistant::entry_ref_ ? entryText(Gui::Assistant::entry_ref_) : "";
	if (ref.empty()) {
		Gui::msgBoxError("Primero elige en el Paso 2 un vídeo de referencia SANO, grabado con la "
		                 "misma cámara y la misma configuración que los dañados.");
		return;
	}
	std::string nombre = g_asist_nombre_plantilla ? entryText(g_asist_nombre_plantilla) : "";
	plantillas::ResultadoCrear r = plantillas::crearPlantilla(ref, "", nombre);
	if (!r.ok) {
		Gui::msgBoxError(r.error_es);
		return;
	}
	anadirPlantilla(r.info.id, r.info.resumenEs());
	std::string m = "Plantilla «" + r.info.id + "» guardada (" + r.info.resumenEs() + ", " +
	                std::to_string(r.info.file_bytes / 1024 + 1) + " KB, sin imagen ni sonido; "
	                "se quitaron fechas, GPS y otros metadatos).\n\n"
	                "La próxima vez que no tengas referencia de esta cámara, elige "
	                "«No tengo referencia»: se probará esta plantilla primero (o elígela en "
	                "«Perfil de cámara o plantilla»).\n\n"
	                "Se guardó en: " + r.path;
	Gui::msgBox(m.c_str());
}

void Gui::Assistant::refresh() {
	updateList();
	updateFormatsLabel();
	// Paso 2: el modo manda. Con referencia se pide la referencia
	// y el perfil no pinta nada; sin referencia se pide el perfil
	// y la referencia se desactiva (no se borra lo escrito).
	int modo = combo_modo_ ? uiComboboxSelected(combo_modo_) : 0;
	bool con_ref = (modo == 0);
	if (entry_ref_) {
		if (con_ref) uiControlEnable(uiControl(entry_ref_));
		else uiControlDisable(uiControl(entry_ref_));
	}
	if (btn_ref_examinar_) {
		if (con_ref) uiControlEnable(uiControl(btn_ref_examinar_));
		else uiControlDisable(uiControl(btn_ref_examinar_));
	}
	for (uiControl* c : {uiControl(g_asist_btn_plantilla), uiControl(g_asist_nombre_plantilla)}) {
		if (!c) continue;
		if (con_ref) uiControlEnable(c);
		else uiControlDisable(c);
	}
	if (combo_perfil_) {
		if (con_ref) uiControlDisable(uiControl(combo_perfil_));
		else uiControlEnable(uiControl(combo_perfil_));
	}
	if (btn_reparar_todo_) {
		if (!danados_.empty()) uiControlEnable(uiControl(btn_reparar_todo_));
		else uiControlDisable(uiControl(btn_reparar_todo_));
	}
	updatePreviewTarget();
}

// Progreso REAL por archivo: el nucleo llama a g_onProgress via
// outProgress() durante el parseo y durante el bucle de reparacion
// (mp4.cpp:2014, cada 2000 iteraciones; atom.cpp durante el parseo).
// Aqui se mapea el % intra-archivo al % global del lote. Se
// actualiza solo cuando el % global cambia, para no saturar la
// interfaz. OJO: el % puede retroceder brevemente entre fases
// (el parseo llega al 100% y la reparacion vuelve a empezar en 0);
// es el progreso real del nucleo, no un invento, y se documenta.
void Gui::Assistant::onProgress(int percentage) {
	if (total_ <= 0 || !progressbar_) return;
	if (percentage < 0) percentage = 0;
	if (percentage > 100) percentage = 100;
	int global = (actual_ * 100 + percentage) / total_;
	if (global > 100) global = 100;
	if (global == last_global_) return;
	last_global_ = global;
	uiProgressBarSetValue(progressbar_, global);
}

void Gui::Assistant::showStatusQueued(void* data) {
	auto* s = static_cast<std::string*>(data);
	if (label_estado_) uiLabelSetText(label_estado_, s->c_str());
	delete s;
}

void Gui::Assistant::showPreviewQueued(void* data) {
	auto* s = static_cast<std::string*>(data);
	if (label_preview_) uiLabelSetText(label_preview_, s->c_str());
	delete s;
}

// Punto 9 (reparaciones 2026-11-07): punto UNICO de limpieza
// del hilo compartido Gui::thread_. Se ejecuta en el hilo
// principal (ASYNC): espera a que el hilo termine (join), lo
// borra y deja el puntero a nullptr, y vacia la descripcion de lo
// que estaba haciendo. Antes solo el lote del Asistente limpiaba
// el hilo (en showReportQueued); la reparacion de la pestana
// Reparar y la busqueda de atomos de Analizar NUNCA lo limpiaban,
// asi que despues de reparar una vez en Reparar el puntero se
// quedaba puesto para siempre y el Asistente decia "Ya hay una
// reparacion en marcha" aunque no hubiera ninguna (es justo lo que
// le paso a Angel tras reparar 43038801920.mp4 en Reparar y luego
// pulsar "Reparar todo" en el Asistente). Con esto, un thread_ no
// nulo significa de verdad que hay trabajo en marcha.
void Gui::clearThreadQueued(void* data) {
	if (Gui::thread_) {
		Gui::thread_->join();
		delete Gui::thread_;
		Gui::thread_ = nullptr;
	}
	Gui::thread_desc_.clear();
}

void Gui::Assistant::showReportQueued(void* data) {
	auto* s = static_cast<std::string*>(data);
	if (text_informe_) uiMultilineEntrySetText(text_informe_, s->c_str());
	delete s;
	// El registro tecnico que se fue escribiendo en el informe
	// durante el lote ya no hace falta: se vacia el bufer para que
	// el temporizador no lo anada debajo del informe final.
	Gui::buffer_.str("");
	updatePreviewTarget();
	// El hilo del lote ya termino (esta llamada se encolo al
	// final); se espera y se libera aqui, igual que hace
	// Train::onScriptFinished con su hilo. Punto 9: se usa el
	// punto unico de limpieza, no el join a mano.
	Gui::clearThreadQueued(nullptr);
}

void Gui::Assistant::setConvertedQueued(void* data) {
	auto* c = static_cast<AssistantConverted*>(data);
	if (c->index >= 0 && c->index < (int)danados_.size())
		danados_[c->index] = c->path;
	delete c;
	refresh();
}

void Gui::Assistant::addFile() {
	// Punto 10: se pueden elegir varios vídeos a la vez en el mismo
	// diálogo (Ctrl o Mayús); todos se agregan sin duplicar los que
	// ya están en la lista.
	auto files = openVideoFilesMulti();
	if (files.empty()) return;
	MultiOpen::agregarSinDuplicados(danados_, files);
	refresh();
}

void Gui::Assistant::convertNonBmff(uiButton* button, void* data) {
	if (danados_.empty()) {
		msgBoxError("No hay vídeos en la lista del Paso 1: agrega primero los vídeos dañados.");
		return;
	}
	// La conversion (sobre todo si re-codifica) tarda: en un hilo,
	// como hace convertBadToMp4 en la pestana Reparar. El original
	// nunca se toca; el MP4 convertido (<nombre>_convertido.mp4)
	// queda junto al original y reemplaza su posicion en la lista.
	std::vector<std::string> files = danados_;
	GuiHelper::setDisabled(true);
	if (label_estado_) uiLabelSetText(label_estado_, "Convirtiendo a MP4 con FFmpeg…");
	std::thread([](std::vector<std::string> files) {
		for (size_t i = 0; i < files.size(); i++) {
			FormatInfo fi = detectFormat(files[i]);
			if (fi.is_bmff) continue;
			if (!fi.convertible) {
				ASYNC(msgBoxErrorQueued, new std::string(
				    "No se reconoce el formato de " + files[i] +
				    ": no se puede convertir a MP4 ni reparar."));
				continue;
			}
			ConvertResult r = convertToMp4(files[i]);
			if (!r.ok) {
				ASYNC(msgBoxErrorQueued, new std::string(
				    "No se pudo convertir a MP4 " + files[i] + ": " + r.error_es));
				continue;
			}
			AssistantConverted* c = new AssistantConverted{(int)i, r.output};
			ASYNC(Gui::Assistant::setConvertedQueued, c);
		}
		ASYNC(Gui::Assistant::showStatusQueued, new std::string(
		    "Conversión terminada: revisa la lista del Paso 1 (los convertidos ya son MP4)."));
		ASYNC(GuiHelper::setDisabled, false);
	}, files).detach();
}

void Gui::Assistant::startBatch(uiButton* button, void* data) {
	if (danados_.empty()) {
		msgBoxError("No hay vídeos en la lista: en el Paso 1 pulsa «Agregar vídeo dañado».");
		return;
	}
	if (Gui::thread_) {
		// Punto 9: si de verdad hay trabajo en marcha, el aviso
		// dice QUE es (lote del Asistente, reparacion de Reparar
		// o busqueda de atomos), no solo que hay algo en marcha.
		std::string m = "Ya hay una reparación en marcha";
		if (!Gui::thread_desc_.empty()) m += " (" + Gui::thread_desc_ + ")";
		m += ". Espera a que termine.";
		msgBox(m.c_str());
		return;
	}
	int modo = combo_modo_ ? uiComboboxSelected(combo_modo_) : 0;
	bool con_ref = (modo == 0);
	std::string ref = entry_ref_ ? entryText(entry_ref_) : "";
	if (con_ref && ref.empty()) {
		msgBoxError("Falta el vídeo de referencia: en el Paso 2 elige un vídeo sano del "
		            "mismo móvil o cámara, o cambia a «No tengo referencia».");
		return;
	}
	// Formatos ANTES de empezar: un MKV/AVI/etc. en la lista no se
	// repara directo (pre-vuelo BadFormatNoBmff); se avisa aqui y
	// no se gasta tiempo reparando los demas para fallar al final.
	// Punto 4: con "Intentar de todos modos" marcado, un archivo
	// DESCONOCIDO y no convertible (sin ftyp ni nada, como
	// f18931679.mp4) NO se bloquea aqui: pasa al lote y el modo
	// clasico, al fallar el pre-vuelo, intenta el rescate forzado.
	// Un formato convertible (MKV, AVI...) sigue bloqueandose:
	// para ese hay que convertir primero, el rescate no lo arregla.
	bool forzar_checked = chk_forzar_ ? uiCheckboxChecked(chk_forzar_) : false;
	for (auto& p : danados_) {
		FormatInfo fi = detectFormat(p);
		if (!fi.is_bmff) {
			if (forzar_checked && !fi.convertible) continue;
			msgBoxError("«" + assistantBaseName(p) + "» es " + formatNameEs(fi) +
			            ", no MP4/MOV/M4V/3GP. Pulsa «Convertir a MP4» en el Paso 2 "
			            "para convertirlo primero (sin tocar el original) y luego "
			            "«Reparar todo».");
			return;
		}
	}
	// Perfil para el modo sin referencia: manda el desplegable del
	// asistente si se eligio uno; si esta en Automatico, se respeta
	// el perfil de la pestana IA (g_ai_local_profile), como hace
	// startRepair. "" = automatico del todo.
	std::string profile;
	{
		int sel = combo_perfil_ ? uiComboboxSelected(combo_perfil_) : 0;
		const int i_tpl = sel - kAiLocalProfilesCount - 1;
		if (sel > 0 && sel <= kAiLocalProfilesCount)
			profile = kAiLocalProfiles[sel - 1];
		else if (i_tpl >= 0 && i_tpl < (int)g_asist_plantillas.size())
			profile = g_asist_plantillas[i_tpl];  // plantilla: se prueba primero
		else
			profile = g_ai_local_profile;
	}

	Gui::current_text_entry_ = text_informe_;
	uiMultilineEntrySetText(text_informe_, "");
	if (label_estado_)
		uiLabelSetText(label_estado_, "Preparando el lote…");
	if (progressbar_) uiProgressBarSetValue(progressbar_, 0);
	GuiHelper::setDisabled(true);

	std::vector<std::string> files = danados_;
	std::string refCopy = ref;
	std::string profileCopy = profile;
	// Punto 9: se anota que trabajo es, para el aviso de arriba.
	Gui::thread_desc_ = "el lote del Asistente";
	// Foto de los Ajustes al pulsar: cambiarlos durante el lote no
	// lo altera. El progreso real del nucleo va a la barra del
	// Asistente solo mientras dura cada reparacion (RepairSession).
	RepairOptions opts = guiRepairOptions();
	opts.on_progress = Gui::Assistant::onProgress;
	Gui::thread_ = new std::thread([files, refCopy, modeCopy = con_ref ? 0 : 1, profileCopy, forzarCopy = forzar_checked, opts]() {
		AiConfig cfg = loadAiConfig();
		total_ = (int)files.size();
		actual_ = 0;
		last_global_ = -1;
		std::vector<std::pair<std::string, RepairOutcome>> results;
		std::vector<std::string> salidas;
		std::string informe;
		for (size_t i = 0; i < files.size(); i++) {
			actual_ = (int)i;
			std::string base = assistantBaseName(files[i]);
			ASYNC(Gui::Assistant::showStatusQueued, new std::string(
			    "Reparando " + std::to_string(i + 1) + " de " +
			    std::to_string(files.size()) + ": " + base + "…"));
			informe += "--- Archivo " + std::to_string(i + 1) + " de " +
			           std::to_string(files.size()) + ": " + base + " ---\n";
			// Punto 9: si UN archivo lanza una excepcion (el
			// nucleo lanza std::runtime_error / DiagnoserError
			// en logg(ET), por ejemplo), que falle SOLO ese
			// archivo y el lote siga con el siguiente, como ya
			// hace repairBatch() en el CLI. Sin este try, la
			// excepcion escapaba del hilo, el programa terminaba
			// de golpe y NUNCA se encolaban las acciones finales
			// (informe + limpieza de thread_).
			try {
			if (modeCopy == 0) {
				// Modo clasico con referencia (Fase 1): mismo
				// pre-vuelo, mismo reintento automatico y misma
				// salida a Reparados/ que startRepair, archivo a
				// archivo. La sugerencia de IA decide que estrategia
				// (-s / -dyn) se prueba primero en la busqueda.
				RepairOptions o_ia = opts;
				if (g_ai_local_enabled || g_ai_lmstudio_enabled ||
				    g_ai_external_enabled || g_onnx_enabled) {
					AiConfig cfgFile = loadAiConfig();
					auto hr = hybridAnalyze(files[i], refCopy, cfgFile, g_ai_local_profile, g_exe_argv0);
					std::cout << "\n" << hr.summary_es << "\n";
					if (hr.suggestion.confidence >= 0.5) {
						o_ia.hint_set = true;
						o_ia.hint_s = hr.suggestion.use_step;
						o_ia.hint_dyn = hr.suggestion.use_dyn;
						o_ia.hint_fuente = hr.suggestion.source;
					}
				}
				RepairOutcome o = repairFileWithAutoRetry(refCopy, files[i], o_ia, true);
				// Punto 4: si el pre-vuelo rechazo el danado por no
				// parecer MP4/MOV y "Intentar de todos modos" esta
				// marcado, se intenta el rescate forzado en vez de
				// dejar el fallo tal cual. El informe muestra el
				// resumen del rescate (no el del clasico fallido).
				bool forced_tried = false;
				if (!o.ok && !o.skipped && forzarCopy &&
				    (o.code == DiagCode::BadNoFtyp || o.code == DiagCode::BadNoMdat ||
				     o.code == DiagCode::BadCabeceraDaniada)) {
					RescueOutcome r = forcedRescueAttempt(files[i], cfg, opts, profileCopy, g_exe_argv0);
					forced_tried = true;
					informe += r.summary_es;
					informe += "\n";
					if (r.ok) o = r.repair;
					else {
						o.ok = false;
						o.error_es = r.error_es;
						o.message_es = r.error_es;
					}
				}
				if (!forced_tried) {
					informe += repairSummaryEs(o, files[i]);
					informe += "\n";
				}
				if (o.ok && !o.output_path.empty()) salidas.push_back(o.output_path);
				results.emplace_back(files[i], o);
			} else {
				// Sin referencia (Fase 2): perfil / modelo ONNX
				// entrenado / rescate, segun lo que diga la IA
				// hibrida. Si no detecta resolucion, no inventa
				// nada: error_es explica que hace falta una
				// referencia sana real.
				RescueOutcome r = rescueWithoutReference(files[i], cfg, opts, profileCopy, g_exe_argv0);
				informe += r.summary_es;
				informe += "\n";
				RepairOutcome o;
				if (r.attempted) o = r.repair;
				else {
					o.ok = false;
					o.error_es = r.error_es;
					o.message_es = r.error_es;
				}
				if (r.ok && !r.repair.output_path.empty())
					salidas.push_back(r.repair.output_path);
				results.emplace_back(files[i], o);
			}
			} catch (const std::exception& e) {
				// Punto 9: este archivo falla, pero el lote
				// SIGUE con el siguiente y el informe lo anota
				// como fallo, para que el resumen cuente bien.
				RepairOutcome o;
				o.ok = false;
				o.error_es = std::string("Error inesperado al reparar este archivo: ") + e.what();
				o.message_es = o.error_es;
				informe += o.error_es + "\n";
				results.emplace_back(files[i], o);
			} catch (...) {
				RepairOutcome o;
				o.ok = false;
				o.error_es = "Error inesperado al reparar este archivo.";
				o.message_es = o.error_es;
				informe += o.error_es + "\n";
				results.emplace_back(files[i], o);
			}
			Gui::Assistant::onProgress(100);
		}
		chkHiddenWarnings();
		salidas_ = salidas;
		preview_index_ = 0;
		std::string final_informe = "===== INFORME DEL LOTE (ASISTENTE) =====\n";
		final_informe += informe;
		final_informe += batchSummaryEs(results);
		ASYNC(Gui::Assistant::showStatusQueued, new std::string(
		    salidas.empty()
		        ? "Lote terminado con fallos: revisa el informe de abajo."
		        : "Lote terminado: " + std::to_string(salidas.size()) + " de " +
		          std::to_string(files.size()) + " reparados. Revisa la vista previa."));
		// Primero se reactivan los controles y despues se muestra
		// el informe: showReportQueued ajusta que botones de vista
		// previa quedan activos segun haya salidas o no.
		ASYNC(GuiHelper::setDisabled, false);
		ASYNC(Gui::Assistant::showReportQueued, new std::string(final_informe));
	});
}

void Gui::Assistant::previewPrev(uiButton* button, void* data) {
	if (salidas_.empty()) return;
	preview_index_--;
	if (preview_index_ < 0) preview_index_ = (int)salidas_.size() - 1;
	updatePreviewTarget();
}

void Gui::Assistant::previewNext(uiButton* button, void* data) {
	if (salidas_.empty()) return;
	preview_index_++;
	if (preview_index_ >= (int)salidas_.size()) preview_index_ = 0;
	updatePreviewTarget();
}

void Gui::Assistant::previewFirstSeconds(uiButton* button, void* data) {
	if (salidas_.empty()) {
		msgBoxError("Todavía no hay ningún vídeo reparado en este lote.");
		return;
	}
	std::string target = salidas_[preview_index_];
	// ffplay no esta en el ZIP actual (solo DLLs de FFmpeg): si no
	// esta instalado en el equipo, no se abre nada. Por eso el
	// mensaje lo dice antes de intentarlo y queda el boton
	// "Abrir video reparado" como alternativa segura.
	if (label_preview_)
		uiLabelSetText(label_preview_,
		               "Reproduciendo los primeros 5 segundos con ffplay… Si no se abre "
		               "ningún reproductor, ffplay no está disponible: usa «Abrir vídeo reparado».");
	std::thread([](std::string target) {
#ifdef _WIN32
		std::string cmd = "start \"\" /min ffplay -autoexit -t 5 " + assistantQuoteWin(target);
#else
		std::string cmd = "ffplay -autoexit -t 5 -v error " + assistantQuote(target) +
		                  " > /dev/null 2>&1 &";
#endif
		upath::system(cmd);
	}, target).detach();
}

void Gui::Assistant::previewFirstFrame(uiButton* button, void* data) {
	if (salidas_.empty()) {
		msgBoxError("Todavía no hay ningún vídeo reparado en este lote.");
		return;
	}
	std::string target = salidas_[preview_index_];
	std::string img = assistantPreviewImagePath(target);
	if (label_preview_)
		uiLabelSetText(label_preview_, "Extrayendo el primer fotograma con ffmpeg…");
	std::thread([](std::string target, std::string img) {
		std::string cmd = "ffmpeg -y -v error -ss 1 -i " + assistantQuote(target) +
		                  " -vframes 1 " + assistantQuote(img);
		upath::system(cmd);
		// El exito se juzga por la salida (existe y pesa > 0),
		// no por el codigo de ffmpeg: igual que hace convertToMp4
		// con archivos truncados, que casi siempre hacen que
		// ffmpeg se queje al final aunque el archivo sirva.
		std::error_code ec;
		const std::filesystem::path img_path = upath::toPath(img);
		bool ok = std::filesystem::exists(img_path, ec) && !ec &&
		          std::filesystem::file_size(img_path, ec) > 0 && !ec;
		if (ok) {
			ASYNC(Gui::Assistant::showPreviewQueued, new std::string(
			    "Miniatura: " + img + " (primer fotograma, segundo 1)."));
			assistantOpenWithDefaultApp(img);
		} else {
			ASYNC(Gui::Assistant::showPreviewQueued, new std::string(
			    "No se pudo extraer el primer fotograma: ffmpeg no está disponible "
			    "o el vídeo no tiene fotogramas en el segundo 1. El vídeo reparado "
			    "sí existe: ábrelo con «Abrir vídeo reparado»."));
		}
	}, target, img).detach();
}

void Gui::Assistant::openRepaired(uiButton* button, void* data) {
	if (salidas_.empty()) {
		msgBoxError("Todavía no hay ningún vídeo reparado en este lote.");
		return;
	}
	std::string target = salidas_[preview_index_];
	std::thread([](std::string t) { assistantOpenWithDefaultApp(t); }, target).detach();
}

uiControl* Gui::assistantTab() {
	auto v_box = newVerticalBox();

	// Altura (2026-11-12): textos acortados y filas juntas para que la
	// ventana quepa en pantallas de 768 px (antes 825 px de contenido).
	auto label_intro = uiNewLabel("Nada sale de tu PC y el original no se toca: lo reparado va a la carpeta Reparados/.");
	uiBoxAppend(v_box, uiControl(label_intro), 0);

	// --- Paso 1: videos danados (lote) ---
	auto group1 = newGroup("Paso 1 · Elige los vídeos dañados");
	auto v1 = newVerticalBox();
	auto h_btns1 = newHorizontalBox();
	auto btn_add = newButton("Agregar vídeo dañado");
	auto btn_del = newButton("Quitar último");
	auto btn_clear = newButton("Limpiar lista");
	uiBoxAppend(h_btns1, uiControl(btn_add), 0);
	uiBoxAppend(h_btns1, uiControl(btn_del), 0);
	uiBoxAppend(h_btns1, uiControl(btn_clear), 0);
	uiBoxAppend(h_btns1, uiControl(uiNewLabel("Puedes elegir varios con Ctrl o Mayús.")), 0);
	Assistant::list_danados_ = uiNewMultilineEntry();
	uiMultilineEntrySetReadOnly(Assistant::list_danados_, 1);
	Assistant::label_contadores_ = uiNewLabel("Vídeos dañados: 0");
	uiBoxAppend(v1, uiControl(h_btns1), 0);
	uiBoxAppend(v1, uiControl(Assistant::list_danados_), 1);
	uiBoxAppend(v1, uiControl(Assistant::label_contadores_), 0);
	uiGroupSetChild(group1, uiControl(v1));
	// Punto 7 (reparaciones 2026-11-07): el grupo del Paso 1 es
	// stretchy (1). Antes iba con 0: libui le daba su altura
	// mínima y la lista de vídeos dañados solo mostraba 1 línea
	// aunque la ventana estuviera a pantalla completa, y el resto
	// quedaba cortado sin verse. La lista ya era stretchy (1)
	// dentro de v1, así que al crecer el grupo crece ella y se
	// ven varias líneas, con desplazamiento dentro del control.
	// El Paso 2 sigue fijo (0): solo tiene desplegables y botones.
	uiBoxAppend(v_box, uiControl(group1), 1);

	// --- Paso 2: referencia, o sin ella, o convertir primero ---
	auto group2 = newGroup("Paso 2 · Referencia, o intentarlo sin ella");
	auto v2 = newVerticalBox();
	Assistant::combo_modo_ = uiNewCombobox();
	uiComboboxAppend(Assistant::combo_modo_, "Tengo un vídeo de referencia (sano, del mismo móvil o cámara)");
	uiComboboxAppend(Assistant::combo_modo_, "No tengo referencia: intentar sin ella (perfil / modelo entrenado / rescate)");
	uiBoxAppend(v2, uiControl(Assistant::combo_modo_), 0);
	auto h_ref = newHorizontalBox();
	Assistant::btn_ref_examinar_ = newButton("Vídeo de referencia (sano)");
	Assistant::entry_ref_ = newEntry();
	uiButtonOnClicked(Assistant::btn_ref_examinar_, openFile, Assistant::entry_ref_);
	uiBoxAppend(h_ref, uiControl(Assistant::btn_ref_examinar_), 0);
	uiBoxAppend(h_ref, uiControl(Assistant::entry_ref_), 1);
	// Guardar la referencia como plantilla de camara (solo cabecera).
	auto h_tpl = newHorizontalBox();
	g_asist_btn_plantilla = newButton("Guardar plantilla de esta cámara (solo la cabecera, unos KB)");
	auto label_tpl = uiNewLabel("Nombre (opcional):");
	g_asist_nombre_plantilla = newEntry();
	uiBoxAppend(h_tpl, uiControl(g_asist_btn_plantilla), 0);
	uiBoxAppend(h_tpl, uiControl(label_tpl), 0);
	uiBoxAppend(h_tpl, uiControl(g_asist_nombre_plantilla), 1);
	uiButtonOnClicked(g_asist_btn_plantilla, Gui::Assistant::guardarPlantilla, nullptr);
	auto h_perfil = h_ref;
	auto label_perfil = uiNewLabel("Sin referencia:");
	Assistant::combo_perfil_ = uiNewCombobox();
	uiComboboxAppend(Assistant::combo_perfil_, "Automático (recomendado)");
	for (int i = 0; i < kAiLocalProfilesCount; i++)
		uiComboboxAppend(Assistant::combo_perfil_, kAiLocalProfiles[i]);
	g_asist_plantillas.clear();
	for (const auto& p : plantillas::listarPlantillas(g_exe_argv0))
		Assistant::anadirPlantilla(p.id, p.resumenEs());
	uiBoxAppend(h_perfil, uiControl(label_perfil), 0);
	uiBoxAppend(h_perfil, uiControl(Assistant::combo_perfil_), 0);
	uiBoxAppend(v2, uiControl(h_ref), 0);
	uiBoxAppend(v2, uiControl(h_tpl), 0);
	Assistant::label_formatos_ = uiNewLabel("Formatos: 0 directos (MP4, MOV, M4V, 3GP)  ·  0 para convertir a MP4  ·  0 desconocidos (no se pueden reparar)");
	auto h_convert = newHorizontalBox();
	Assistant::btn_convertir_ = newButton("Convertir MKV, AVI... a MP4");
	uiBoxAppend(h_convert, uiControl(Assistant::btn_convertir_), 0);
	uiBoxAppend(h_convert, uiControl(Assistant::label_formatos_), 1);
	uiBoxAppend(v2, uiControl(h_convert), 0);
	// Punto 4 (reparaciones 2026-11-07): "Intentar de todos modos".
	// El pre-vuelo NO se elimina: si un dañado no parece MP4/MOV
	// (sin ftyp/moov/mdat, como el caso real f18931679.mp4 de
	// Recoverit), el Asistente lo rechaza y lo explica. Con esta
	// casilla marcada, en vez de bloquearse se salta el pre-vuelo
	// y se intenta el rescate sin referencia (perfil/ONNX). Si el
	// rescate tampoco encuentra vídeo, el informe lo dice claro
	// y NO se deja un archivo vacío engañoso.
	Assistant::chk_forzar_ = uiNewCheckbox("Intentar de todos modos si no parece MP4 (rescate de archivos sin ftyp/moov/mdat)");
	uiBoxAppend(v2, uiControl(Assistant::chk_forzar_), 0);
	uiGroupSetChild(group2, uiControl(v2));
	uiBoxAppend(v_box, uiControl(group2), 0);

	// --- Paso 3: reparar el lote y vista previa ---
	auto group3 = newGroup("Paso 3 · Repara y revisa");
	auto v3 = newVerticalBox();
	auto h_rep = newHorizontalBox();
	Assistant::btn_reparar_todo_ = newButton("Reparar todo");
	Assistant::label_estado_ = uiNewLabel("Sin empezar. Completa los pasos 1 y 2 y pulsa «Reparar todo».");
	uiBoxAppend(h_rep, uiControl(Assistant::btn_reparar_todo_), 0);
	uiBoxAppend(h_rep, uiControl(Assistant::label_estado_), 1);
	Assistant::progressbar_ = uiNewProgressBar();
	Assistant::text_informe_ = uiNewMultilineEntry();
	uiMultilineEntrySetReadOnly(Assistant::text_informe_, 1);
	auto h_prev_nav = newHorizontalBox();
	Assistant::btn_preview_prev_ = newButton("‹ Anterior");
	Assistant::label_preview_target_ = uiNewLabel("Vista previa: todavía no hay ningún vídeo reparado en este lote.");
	Assistant::btn_preview_next_ = newButton("Siguiente ›");
	uiBoxAppend(h_prev_nav, uiControl(Assistant::btn_preview_prev_), 0);
	uiBoxAppend(h_prev_nav, uiControl(Assistant::label_preview_target_), 1);
	uiBoxAppend(h_prev_nav, uiControl(Assistant::btn_preview_next_), 0);
	auto h_prev_btns = newHorizontalBox();
	Assistant::btn_preview_5s_ = newButton("Ver primeros 5 segundos");
	Assistant::btn_preview_foto_ = newButton("Ver primer fotograma");
	Assistant::btn_preview_abrir_ = newButton("Abrir vídeo reparado");
	uiBoxAppend(h_prev_btns, uiControl(Assistant::btn_preview_5s_), 0);
	uiBoxAppend(h_prev_btns, uiControl(Assistant::btn_preview_foto_), 0);
	uiBoxAppend(h_prev_btns, uiControl(Assistant::btn_preview_abrir_), 0);
	Assistant::label_preview_ = uiNewLabel("La miniatura se abre con tu visor de imágenes.");
	uiBoxAppend(h_prev_btns, uiControl(Assistant::label_preview_), 1);
	uiBoxAppend(v3, uiControl(h_rep), 0);
	uiBoxAppend(v3, uiControl(Assistant::progressbar_), 0);
	uiBoxAppend(v3, uiControl(Assistant::text_informe_), 1);
	uiBoxAppend(v3, uiControl(h_prev_nav), 0);
	uiBoxAppend(v3, uiControl(h_prev_btns), 0);
	uiGroupSetChild(group3, uiControl(v3));
	// Punto 7: el grupo del Paso 3 también es stretchy (1), como
	// el del Paso 1: el informe del lote (ya stretchy (1) dentro
	// de v3) crece con la ventana y se ven varias líneas, con
	// desplazamiento dentro del control. El espacio extra de la
	// ventana se reparte entre el Paso 1 y el Paso 3.
	uiBoxAppend(v_box, uiControl(group3), 1);

	// --- Cableado ---
	uiButtonOnClicked(btn_add, [](uiButton* b, void* data){
		Gui::Assistant::addFile();
	}, nullptr);
	uiButtonOnClicked(btn_del, [](uiButton* b, void* data){
		if (!Gui::Assistant::danados_.empty()) Gui::Assistant::danados_.pop_back();
		Gui::Assistant::refresh();
	}, nullptr);
	uiButtonOnClicked(btn_clear, [](uiButton* b, void* data){
		Gui::Assistant::danados_.clear();
		Gui::Assistant::refresh();
	}, nullptr);
	uiComboboxOnSelected(Assistant::combo_modo_, [](uiCombobox* c_box, void* data){
		Gui::Assistant::refresh();
	}, nullptr);
	uiComboboxOnSelected(Assistant::combo_perfil_, [](uiCombobox* c_box, void* data){
		// El perfil solo se lee al pulsar "Reparar todo"; no hay
		// nada que actualizar aqui.
	}, nullptr);
	uiButtonOnClicked(Assistant::btn_convertir_, Gui::Assistant::convertNonBmff, nullptr);
	uiButtonOnClicked(Assistant::btn_reparar_todo_, Gui::Assistant::startBatch, nullptr);
	uiButtonOnClicked(Assistant::btn_preview_prev_, Gui::Assistant::previewPrev, nullptr);
	uiButtonOnClicked(Assistant::btn_preview_next_, Gui::Assistant::previewNext, nullptr);
	uiButtonOnClicked(Assistant::btn_preview_5s_, Gui::Assistant::previewFirstSeconds, nullptr);
	uiButtonOnClicked(Assistant::btn_preview_foto_, Gui::Assistant::previewFirstFrame, nullptr);
	uiButtonOnClicked(Assistant::btn_preview_abrir_, Gui::Assistant::openRepaired, nullptr);

	uiComboboxSetSelected(Assistant::combo_modo_, 0);
	uiComboboxSetSelected(Assistant::combo_perfil_, 0);
	Assistant::refresh();
	return uiControl(v_box);
}

uiControl* Gui::analyzeTab() {
	auto v_box = newVerticalBox();

	auto h_box1 = newHorizontalBox();
	Analyze::entry_ok_ = addFileOpen(h_box1, "Vídeo de referencia (sano)");
	uiBoxAppend(v_box, uiControl(h_box1), 0);

	Analyze::text_entry_ = uiNewMultilineEntry();
	uiMultilineEntrySetReadOnly(Analyze::text_entry_, 1);
	uiBoxAppend(v_box, uiControl(Analyze::text_entry_), 1);

	auto h_box2 = newHorizontalBox();
	auto label = uiNewLabel("");
	auto b_dump = newButton("Volcar muestras (-d)");
	auto b_analyze = newButton("Analizar (-a)");
	auto b_atoms = newButton("Buscar átomos (-f)");
	auto b_info = newButton("Información (-i)");
	uiBoxAppend(h_box2, uiControl(label), 1);
	uiBoxAppend(h_box2, uiControl(b_dump), 0);
	uiBoxAppend(h_box2, uiControl(b_analyze), 0);
	uiBoxAppend(h_box2, uiControl(b_atoms), 0);
	uiBoxAppend(h_box2, uiControl(b_info), 0);
	uiBoxAppend(v_box, uiControl(h_box2), 0);

	uiButtonOnClicked(b_dump, [](uiButton* b, void* data){
		defineMp4ForAnalyze();
		mp4.dumpSamples();
	}, NULL);

	uiButtonOnClicked(b_analyze, [](uiButton* b, void* data){
		defineMp4ForAnalyze();
		mp4.analyze();
	}, NULL);

	uiButtonOnClicked(b_info, [](uiButton* b, void* data){
		defineMp4ForAnalyze();
		mp4.printMediaInfo();
		cout << "\n\n";
		mp4.printAtoms();
	}, NULL);

	uiButtonOnClicked(b_atoms, [](uiButton* b, void* data){
		defineFileForAnalyze();  // for findAtomNames, file_ok can be truncated as well
		// Punto 9: guarda como en el Asistente y en Reparar: no
		// sobreescribir thread_ si otro trabajo esta en marcha.
		if (thread_) {
			std::string m = "Ya hay una reparación en marcha";
			if (!Gui::thread_desc_.empty()) m += " (" + Gui::thread_desc_ + ")";
			m += ". Espera a que termine.";
			msgBox(m.c_str());
			return;
		}
		setDisabled(true);
		Analyze::onProgress(0);
		// Punto 9: se anota que trabajo es, para el aviso de arriba.
		Gui::thread_desc_ = "la búsqueda de átomos de la pestaña Analizar";
		RepairOptions atoms_opts = guiRepairOptions();
		atoms_opts.on_progress = Analyze::onProgress;
		thread_ = new thread([](const string& file, const RepairOptions& opts){
			try {
				RepairSession session(opts);
				Atom::findAtomNames(file);
				Analyze::onProgress(100);
			}
			CATCH_THEM();

			// Punto 9: antes este hilo nunca limpiaba thread_,
			// asi que despues de buscar atomos una vez el
			// Asistente quedaba bloqueado con "Ya hay una
			// reparación en marcha" hasta cerrar el programa.
			ASYNC(setDisabled, false);
			ASYNC(clearThreadQueued, nullptr);
		}, file_ok, atoms_opts);
	}, NULL);

	Analyze::progressbar_ = uiNewProgressBar();
	uiBoxAppend(v_box, uiControl(Analyze::progressbar_), 0);

	return uiControl(v_box);
}

// Progreso y estado del rescate de tarjeta (desde el hilo de trabajo).
struct EstadoTarjeta {
	int pct;
	std::string txt;
};
static void tarjetaEstadoQueued(void* p) {
	EstadoTarjeta* e = static_cast<EstadoTarjeta*>(p);
	if (e->pct >= 0) uiProgressBarSetValue(Gui::Tarjeta::progressbar_, e->pct);
	if (!e->txt.empty()) uiLabelSetText(Gui::Tarjeta::label_estado_, e->txt.c_str());
	delete e;
}

// Al terminar: el cuadro muestra solo el resumen (el registro de cada
// reparacion y del decodificador llena el cuadro y lo tapaba).
void Gui::Tarjeta::resumenQueued(void* p) {
	std::string* txt = static_cast<std::string*>(p);
	Gui::buffer_.str("");
	uiMultilineEntrySetText(Gui::Tarjeta::text_entry_, txt->c_str());
	delete txt;
}

uiControl* Gui::tarjetaTab() {
	auto v_box = newVerticalBox();
	uiBoxAppend(v_box, uiControl(newTextLines(
	    "Busca los vídeos directamente en una tarjeta SD, un pendrive o una imagen de disco "
	    "(.img/.dd), SOLO EN LECTURA: la tarjeta nunca se modifica. Copia los vídeos completos y "
	    "junta y repara los incompletos o partidos en trozos.\n"
	    "Para leer una unidad (por ejemplo E:) abre el programa como administrador. Si la "
	    "tarjeta falla, haz antes una imagen de disco y rescata desde la imagen.")), 0);

	auto h_origen = newHorizontalBox();
	auto b_imagen = newButton("Imagen de disco…");
	Tarjeta::entry_origen_ = newEntry();
	uiButtonOnClicked(b_imagen, openFile, Tarjeta::entry_origen_);
	uiBoxAppend(h_origen, uiControl(uiNewLabel("Tarjeta (letra, p. ej. E:) o imagen:")), 0);
	uiBoxAppend(h_origen, uiControl(Tarjeta::entry_origen_), 1);
	uiBoxAppend(h_origen, uiControl(b_imagen), 0);
	uiBoxAppend(v_box, uiControl(h_origen), 0);

	auto h_destino = newHorizontalBox();
	Tarjeta::entry_destino_ = newEntry();
	uiBoxAppend(h_destino, uiControl(uiNewLabel("Guardar en (carpeta de OTRO disco):")), 0);
	uiBoxAppend(h_destino, uiControl(Tarjeta::entry_destino_), 1);
	uiBoxAppend(v_box, uiControl(h_destino), 0);

	auto h_ref = newHorizontalBox();
	Tarjeta::entry_ref_ = addFileOpen(h_ref, "Referencia (opcional)");
	uiBoxAppend(v_box, uiControl(h_ref), 0);

	auto h_botones = newHorizontalBox();
	Tarjeta::chk_solo_buscar_ = uiNewCheckbox("Solo buscar (no escribe nada)");
	Tarjeta::label_estado_ = uiNewLabel("");
	auto b_rescatar = newButton("Rescatar");
	uiBoxAppend(h_botones, uiControl(Tarjeta::chk_solo_buscar_), 0);
	uiBoxAppend(h_botones, uiControl(Tarjeta::label_estado_), 1);
	uiBoxAppend(h_botones, uiControl(b_rescatar), 0);
	uiBoxAppend(v_box, uiControl(h_botones), 0);

	Tarjeta::text_entry_ = uiNewMultilineEntry();
	uiMultilineEntrySetReadOnly(Tarjeta::text_entry_, 1);
	uiBoxAppend(v_box, uiControl(Tarjeta::text_entry_), 1);
	Tarjeta::progressbar_ = uiNewProgressBar();
	uiBoxAppend(v_box, uiControl(Tarjeta::progressbar_), 0);

	uiButtonOnClicked(b_rescatar, [](uiButton*, void*) {
		if (thread_) {
			std::string m = "Ya hay un trabajo en marcha";
			if (!Gui::thread_desc_.empty()) m += " (" + Gui::thread_desc_ + ")";
			m += ". Espera a que termine.";
			msgBox(m.c_str());
			return;
		}
		char* t = uiEntryText(Tarjeta::entry_origen_);
		std::string origen = t ? t : "";
		uiFreeText(t);
		t = uiEntryText(Tarjeta::entry_destino_);
		std::string destino = t ? t : "";
		uiFreeText(t);
		t = uiEntryText(Tarjeta::entry_ref_);
		std::string ref = t ? t : "";
		uiFreeText(t);
		const bool solo = uiCheckboxChecked(Tarjeta::chk_solo_buscar_);
		if (origen.empty()) {
			msgBoxError("Escribe la letra de la tarjeta (por ejemplo E:) o elige una imagen de disco.");
			return;
		}
		if (destino.empty() && !solo && tarjeta::esRutaUnidad(origen)) {
			msgBoxError("Escribe en qué carpeta guardar lo rescatado. Tiene que estar en OTRO disco, "
			            "nunca en la tarjeta (escribir en ella puede borrar los vídeos que se buscan).");
			return;
		}
		current_text_entry_ = Tarjeta::text_entry_;
		uiMultilineEntrySetText(current_text_entry_, "");
		uiProgressBarSetValue(Tarjeta::progressbar_, 0);
		setDisabled(true);
		Gui::thread_desc_ = "el rescate de la pestaña Tarjeta SD";
		tarjeta::OpcionesRescate o;
		o.destino = destino;
		o.referencia = ref;
		o.solo_buscar = solo;
		o.reparar = guiRepairOptions();
		o.reparar.on_progress = nullptr;
		o.progreso = [](int pct, const std::string& txt) {
			uiQueueMain(tarjetaEstadoQueued, new EstadoTarjeta{pct, txt});
		};
		thread_ = new thread([](const std::string& origen, const tarjeta::OpcionesRescate& o) {
			try {
				tarjeta::ResultadoRescate r = tarjeta::rescatar(origen, o);
				if (!r.ok) msgBoxError(r.error_es, true);
				else {
					cout << std::flush;
					uiQueueMain(Tarjeta::resumenQueued, new std::string(r.resumen_es));
				}
				uiQueueMain(tarjetaEstadoQueued,
				            new EstadoTarjeta{100, r.ok ? "Terminado." : "No se pudo leer el origen."});
			}
			CATCH_THEM();
			ASYNC(setDisabled, false);
			ASYNC(clearThreadQueued, nullptr);
		}, origen, o);
	}, NULL);

	return uiControl(v_box);
}

uiControl* Gui::aboutTab() {
	// show version + gh link
	auto v_box = newVerticalBox();

	auto label_ver = uiNewLabel(g_version_str.c_str());
	auto text_entry = uiNewMultilineEntry();
	uiMultilineEntrySetReadOnly(text_entry, 1);
	uiBoxAppend(v_box, uiControl(text_entry), 1);

	uiMultilineEntryAppend(text_entry, g_version_str.c_str());
	uiMultilineEntryAppend(text_entry,
						   "\n\n"
						   "Fork mejorado (Fase 1): diagnóstico en español antes de reparar,\n"
						   "detección de archivos intercambiados, reintento automático con -s\n"
						   "y salida a la carpeta Reparados/ sin tocar el original.\n"
						   "Fase 3.1: pestaña «Asistente» en 3 pasos (lote + vista previa),\n"
						   "sin siglas en el flujo principal.\n"
						   "Basado en anthwlock/untrunc (GPL-2.0), fork de ponchio/untrunc.\n"
						   "\n"
						   // Punto 6 (reparaciones 2026-11-07, pedido de Angel):
						   // créditos de la versión modificada. Los créditos
						   // a Ponchio y anthwlock y la nota GPL-2.0 de
						   // arriba NO se tocan: esta línea se añade.
						   "Modificado por Angel Garcia, GeekHost y Muse.\n"
						   "\n"
						   "Perfil de GitHub:\n"
						   "https://github.com/agarciaaa\n"
						   "\n"
						   "Repositorio de esta versión mejorada:\n"
						   "https://github.com/agarciaaa/Recuperacion-de-video-\n"
						   "\n"
						   "Report your issues here:\n"
						   "https://github.com/anthwlock/untrunc"
						   );

	// Punto 6 (reparaciones 2026-11-07): botón que abre el perfil
	// de GitHub de Angel en el navegador. Reutiliza
	// assistantOpenWithDefaultApp (start en Windows, xdg-open en
	// Linux, open en macOS): esas órdenes abren URLs además de
	// archivos. Se lanza en un hilo aparte para no congelar la
	// interfaz, como hacen los botones de vista previa. Solo se
	// abre el perfil; NO se sube nada a GitHub (la subida está
	// bloqueada hasta aclarar la monetización).
	auto h_github = newHorizontalBox();
	auto btn_github = newButton("Abrir perfil de GitHub");
	uiButtonOnClicked(btn_github, [](uiButton* b, void* data){
		std::thread([]() {
			assistantOpenWithDefaultApp("https://github.com/agarciaaa");
		}).detach();
	}, nullptr);
	uiBoxAppend(h_github, uiControl(btn_github), 0);
	uiBoxAppend(v_box, uiControl(h_github), 0);

	return uiControl(v_box);
}

void Gui::msgBox(const char* text) {
	uiMsgBox(window_, "Info", text);
}

void Gui::msgBoxError(const string& text, bool async) {
	if (async)
		ASYNC(msgBoxErrorQueued, new string(text));
	else
		msgBoxErrorSync(text);
}

void Gui::msgBoxErrorSync(const string& text) {
	auto txt = uiNewAttributedString(text.c_str());
	uiMsgBox(window_, "Error", uiAttributedStringString(txt));
	uiFreeAttributedString(txt);
//	uiMsgBox(window_, "Error", text);
}

void Gui::msgBoxErrorQueued(const string* s) {
	msgBoxErrorSync(*s);
	delete s;
}

void Gui::msgBoxQueued(void* data) {
	auto* s = static_cast<string*>(data);
	msgBox(s->c_str());
	delete s;
}

void Gui::printToEntry(const char* text) {
	writeOutput(nullptr);
	uiMultilineEntryAppend(current_text_entry_, text);
}

// Fase 1 (1.6): el resumen en espanol (duracion, % recuperado,
// modo y ruta de salida) se muestra en la etiqueta de estado.
// Se ejecuta en el hilo principal a traves de ASYNC.
void Gui::showSummaryQueued(void* data) {
	auto* s = static_cast<string*>(data);
	// Primera linea del resumen, corta, para la etiqueta.
	auto pos = s->find('\n');
	string first = (pos == string::npos) ? *s : s->substr(0, pos);
	uiLabelSetText(Repair::label_status_, first.c_str());
	delete s;
}

// Formatos (Fase 2.5): se ejecuta en el hilo principal a
// traves de ASYNC. Pone el MP4 convertido en el campo del
// video danado, para que el usuario pulse "Reparar" despues.
void Gui::setBadEntryQueued(void* data) {
	auto* s = static_cast<string*>(data);
	uiEntrySetText(Repair::entry_bad_, s->c_str());
	uiLabelSetText(Repair::label_status_, ("Convertido a MP4: " + *s).c_str());
	delete s;
}

void Gui::convertBadToMp4(uiButton* b, void* data) {
	string file_bad = uiEntryText(Repair::entry_bad_);
	if (file_bad.empty()) {
		msgBoxError("Falta el vídeo dañado: elige primero el archivo a convertir.");
		return;
	}
	FormatInfo fi = detectFormat(file_bad);
	if (fi.is_bmff) {
		msgBox(std::string("Este archivo ya es " + formatNameEs(fi) +
		                   ": no necesita conversión, se puede reparar directamente.").c_str());
		return;
	}
	if (!fi.convertible) {
		msgBoxError("No se reconoce el formato de este archivo; no se puede convertir a MP4.");
		return;
	}
	// La conversion (sobre todo si re-codifica) tarda: en un hilo,
	// como la reparacion. El original nunca se toca.
	uiLabelSetText(Repair::label_status_, "Convirtiendo a MP4 con FFmpeg…");
	std::thread([](const string& input) {
		ConvertResult r = convertToMp4(input);
		if (!r.ok) {
			ASYNC(msgBoxErrorQueued, new string(std::string("No se pudo convertir a MP4: ") + r.error_es));
			return;
		}
		std::string msg = "Convertido a MP4 " + r.method_es + ".\nSalida: " + r.output +
		                  "\nAhora pulsa «Reparar» para reparar este MP4.";
		ASYNC(setBadEntryQueued, new string(r.output));
		ASYNC(msgBoxQueued, new string(msg));
	}, file_bad).detach();
}

void Gui::startRepair(uiButton* b, void* data) {
	current_text_entry_ = Repair::text_entry_;
	uiMultilineEntrySetText(current_text_entry_, "");

	string file_ok = uiEntryText(Repair::entry_ok_);
	string file_bad = uiEntryText(Repair::entry_bad_);
	if (file_ok.empty() || file_bad.empty()) {
		msgBoxError("Faltan archivos: elige el vídeo de referencia (sano) y el vídeo dañado.");
		return;
	}

	// Fase 1 (1.1/1.2): pre-vuelo en espanol ANTES de reparar.
	// Es rapido (solo lee cabeceras) y se hace en el hilo de la
	// interfaz, asi el usuario ve el problema sin esperar.
	// Caso de Angel: si la referencia no tiene moov, aqui se le
	// dice en espanol que archivo es el problema y que hacer,
	// en vez del log crudo "no 'moov' atom found".
	auto report = preflightCheck(file_ok, file_bad);
	// Punto 8 (con el punto 4): si el pre-vuelo rechaza el DANADO
	// (BadNoFtyp, BadNoMdat o BadCabeceraDaniada) y "Intentar de
	// todos modos" esta marcado, NO se bloquea: se intenta el
	// rescate sin referencia. Cualquier otro rechazo (referencia
	// inexistente/vacia/sin moov, mismo archivo, formato no BMFF)
	// sigue bloqueando como antes: ahi el rescate no tiene sentido.
	bool forzar = Repair::chk_forzar_ ? uiCheckboxChecked(Repair::chk_forzar_) : false;
	bool intentable = report.code == DiagCode::BadNoFtyp ||
	                  report.code == DiagCode::BadNoMdat ||
	                  report.code == DiagCode::BadCabeceraDaniada;
	bool forced_rescue = forzar && intentable;
	if (report.code != DiagCode::Ok && !forced_rescue) {
		msgBoxError(diagMessageEs(report));
		return;
	}

	// Punto 9: antes NO habia guarda aqui y se sobreescribia
	// thread_ aunque otro trabajo estuviera en marcha (se perdia
	// el puntero al hilo anterior sin esperarlo). Con la guarda,
	// un thread_ no nulo significa de verdad que hay trabajo en
	// marcha, y el aviso dice cual es.
	if (thread_) {
		std::string m = "Ya hay una reparación en marcha";
		if (!Gui::thread_desc_.empty()) m += " (" + Gui::thread_desc_ + ")";
		m += ". Espera a que termine.";
		msgBox(m.c_str());
		return;
	}

	setDisabled(true);
	Repair::onProgress(0);
	// Punto 9: se anota que trabajo es, para el aviso de arriba.
	Gui::thread_desc_ = "la reparación de la pestaña Reparar";
	// Foto de los Ajustes al pulsar (ver guiRepairOptions). Antes esta
	// pestana no registraba su barra de progreso: el nucleo movia la
	// ultima que hubiera quedado puesta (la de Analizar).
	RepairOptions opts = guiRepairOptions();
	opts.on_progress = repairProgressFromWorker;
	thread_ = new thread([](const string& file_ok, const string& file_bad, bool forced_rescue,
	                        const RepairOptions& opts){
		try {
			if (forced_rescue) {
				// Punto 8: rescate forzado sin referencia
				// (perfil de la pestana IA / ONNX). Si tampoco
				// encuentra video, lo dice con honestidad y no
				// deja un archivo vacio enganoso.
				AiConfig cfg = loadAiConfig();
				RescueOutcome r = forcedRescueAttempt(file_bad, cfg, opts, g_ai_local_profile, g_exe_argv0);
				chkHiddenWarnings();
				Repair::onProgress(100);
				cout << "\n" << r.summary_es << "\n";
				if (r.ok)
					ASYNC(showSummaryQueued, new string(r.summary_es));
				else
					ASYNC(msgBoxErrorQueued, new string(r.error_es));
				// Punto 9: antes este camino hacia `return` y
				// saltaba la limpieza de abajo: los controles se
				// quedaban desactivados y thread_ puesto para
				// siempre. Ahora cae al final comun, como el
				// camino normal (sin return).
			} else {
				// Fase 2: sugerencia de la IA hibrida antes de reparar.
				// Se consulta si ALGUNA conexion esta activa en la
				// pestana IA (antes solo se consultaba si la local
				// estaba activa, asi que apagar la local apagaba
				// tambien a Qwen y a la externa sin avisar), o si el
				// modelo ONNX esta activado en la pestana Entrenar.
				// El perfil por defecto viene de la pestana IA.
				RepairOptions o_ia = opts;
				if (g_ai_local_enabled || g_ai_lmstudio_enabled || g_ai_external_enabled || g_onnx_enabled) {
					AiConfig cfg = loadAiConfig();
					auto hr = hybridAnalyze(file_bad, file_ok, cfg, g_ai_local_profile, g_exe_argv0);
					cout << "\n" << hr.summary_es << "\n";
					if (hr.suggestion.confidence >= 0.5) {
						o_ia.hint_set = true;
						o_ia.hint_s = hr.suggestion.use_step;
						o_ia.hint_dyn = hr.suggestion.use_dyn;
						o_ia.hint_fuente = hr.suggestion.source;
					}
					if (hr.onnx_consulted)
						cout << "Modelo ONNX consultado (local, entrenado en esta PC).\n";
					if (hr.lmstudio_consulted)
						cout << "LM Studio (Qwen local) consultado (solo texto de diagnóstico).\n";
					if (hr.external_consulted)
						cout << "IA externa consultada (solo texto de diagnóstico).\n";
				}
				// Fase 1 (1.3/1.4/1.6): reintento automatico con -s,
				// salida a Reparados/ y resumen en espanol.
				// La logica es la misma que usa el CLI (repair_job.cpp).
				auto outcome = repairFileWithAutoRetry(file_ok, file_bad, o_ia, true);
				chkHiddenWarnings();
				Repair::onProgress(100);
				cout << "\n" << outcome.message_es << "\n";
				if (outcome.ok)
					ASYNC(showSummaryQueued, new string(outcome.message_es));
				else
					ASYNC(msgBoxErrorQueued, new string(outcome.error_es));
			}
		}
		CATCH_THEM();
		// Punto 9: punto UNICO de finalizacion de este hilo:
		// reactivar controles y limpiar thread_ SIEMPRE, termine
		// como termine (exito, fallo, rescate forzado o excepcion
		// capturada por CATCH_THEM). Antes este hilo nunca
		// limpiaba thread_, asi que despues de reparar una vez en
		// esta pestana el Asistente quedaba bloqueado con "Ya hay
		// una reparación en marcha" hasta cerrar el programa.
		ASYNC(setDisabled, false);
		ASYNC(clearThreadQueued, nullptr);
	}, file_ok, file_bad, forced_rescue, opts);
}

void Gui::Repair::onProgress(int percentage) {
	uiProgressBarSetValue(Repair::progressbar_, percentage);
}

void Gui::Repair::onStatus(const string& status) {
	uiLabelSetText(Repair::label_status_, status.c_str());
}

void Gui::Analyze::onProgress(int percentage) {
	uiProgressBarSetValue(Analyze::progressbar_, percentage);
}
