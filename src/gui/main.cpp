#include <string.h>  // memset
#include <iostream>

//#include "libavutil/ffversion.h"
#include "../common.h"
#include "../upath.h"
#include "../ai/onnx/onnx_provider.h"
#include "gui.h"

using namespace std;

int main(int argc, char* argv[]) {
	g_interactive = false;
	g_is_gui = true;

	argv_as_utf8(argc, argv);
	// ffmpeg y Python del paquete portable (si vienen junto al .exe).
	upath::usarHerramientasIncluidas();

	// Fase 3 §6: guardar argv[0] para que ONNX encuentre
	// models/onnx_base/ junto al .exe también desde la GUI.
	setExeArgv0(argc > 0 ? argv[0] : nullptr);

	Gui::init();
	if (argc > 1) uiEntrySetText(Gui::Repair::entry_ok_, argv[1]);
	if (argc > 1) uiEntrySetText(Gui::Analyze::entry_ok_, argv[1]);
	if (argc > 2) uiEntrySetText(Gui::Repair::entry_bad_, argv[2]);
	Gui::run();

	return 0;
}
