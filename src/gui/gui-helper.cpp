#include "gui-helper.h"
#include <string>

using namespace std;

vector<GuiHelper::Thing> GuiHelper::elements_;
vector<void*> GuiHelper::to_disable_;

void GuiHelper::setSpacedThing(Thing& t, bool spaced) {
	size_t j, n;
	void* p = t.ptr;

	// Cada uiXxxSetPadded/SetMargined hace que libui recalcule el tamano
	// minimo de todo el arbol de controles, aunque el valor no cambie.
	// Se omite si ya tiene ese valor. Las pestanas se reaplican siempre:
	// pueden tener paginas nuevas desde la ultima vez.
	if (t.type != tab && t.spaced == (spaced ? 1 : 0))
		return;
	t.spaced = spaced ? 1 : 0;

	switch (t.type) {
		case window:
			uiWindowSetMargined(uiWindow(p), spaced);
			break;
		case box:
			uiBoxSetPadded(uiBox(p), spaced);
			break;
		case tab:
			n = uiTabNumPages(uiTab(p));
			for (j = 0; j < n; j++)
				uiTabSetMargined(uiTab(p), j, spaced);
			break;
		case group:
			uiGroupSetMargined(uiGroup(p), spaced);
			break;
		case form:
			uiFormSetPadded(uiForm(p), spaced);
			break;
		case grid:
			uiGridSetPadded(uiGrid(p), spaced);
			break;
	}
}

void GuiHelper::setSpaced(bool spaced) {
	for (Thing& t : elements_)
		setSpacedThing(t, spaced);
}

void GuiHelper::setDisabled(bool disable) {
	if (disable)
		for (void* c : to_disable_)
			uiControlDisable(uiControl(c));
	else
		for (void* c : to_disable_)
			uiControlEnable(uiControl(c));
}

uiWindow* GuiHelper::newWindow(const char* title, int width, int height,
                               int hasMenubar) {
	auto w = uiNewWindow(title, width, height, hasMenubar);
	append(w, window);
	return w;
}

uiBox* GuiHelper::newHorizontalBox(void) {
	auto b = uiNewHorizontalBox();
	append(b, box);
	return b;
}

uiBox* GuiHelper::newVerticalBox(void) {
	auto b = uiNewVerticalBox();
	append(b, box);
	return b;
}

uiTab* GuiHelper::newTab(void) {
	auto t = uiNewTab();
	append(t, tab);
//	GuiHelper::to_disable_.emplace_back(t);
	return t;
}

// Caracteres (no bytes) de un texto UTF-8.
static size_t utf8Len(const std::string& s) {
	size_t n = 0;
	for (unsigned char c : s)
		if ((c & 0xC0) != 0x80) n++;
	return n;
}

uiBox* GuiHelper::newTextLines(const char* text, size_t max_chars) {
	auto b = uiNewVerticalBox();  // sin separación: se lee como un párrafo
	std::string todo = text ? text : "";
	size_t ini = 0;
	while (ini <= todo.size()) {
		size_t fin = todo.find('\n', ini);
		if (fin == std::string::npos) fin = todo.size();
		std::string parrafo = todo.substr(ini, fin - ini);
		// Sangría inicial (espacios): se repite en cada línea.
		size_t k = parrafo.find_first_not_of(' ');
		std::string sangria = (k == std::string::npos) ? "" : parrafo.substr(0, k);
		std::string resto = (k == std::string::npos) ? "" : parrafo.substr(k);
		std::string linea;
		size_t pos = 0;
		while (pos <= resto.size()) {
			size_t esp = resto.find(' ', pos);
			if (esp == std::string::npos) esp = resto.size();
			std::string palabra = resto.substr(pos, esp - pos);
			if (!linea.empty() &&
			    utf8Len(sangria + linea) + 1 + utf8Len(palabra) > max_chars) {
				uiBoxAppend(b, uiControl(uiNewLabel((sangria + linea).c_str())), 0);
				linea.clear();
			}
			if (!palabra.empty()) linea += (linea.empty() ? "" : " ") + palabra;
			pos = esp + 1;
		}
		uiBoxAppend(b, uiControl(uiNewLabel((sangria + linea).c_str())), 0);
		ini = fin + 1;
	}
	return b;
}

uiGroup* GuiHelper::newGroup(const char* text) {
	auto g = uiNewGroup(text);
	append(g, group);
	return g;
}

uiForm* GuiHelper::newForm(void) {
	auto f = uiNewForm();
	append(f, form);
	return f;
}

uiGrid* GuiHelper::newGrid(void) {
	auto g = uiNewGrid();
	append(g, grid);
	return g;
}

uiButton* GuiHelper::newButton(const char* text) {
	auto b = uiNewButton(text);
	GuiHelper::to_disable_.emplace_back(b);
	return b;
}

uiEntry* GuiHelper::newEntry(void) {
	auto e = uiNewEntry();
	GuiHelper::to_disable_.emplace_back(e);
	return e;
}

void GuiHelper::append(void* ptr, int type) {
	elements_.emplace_back(ptr, type);

	// En MinGW se aplica el espaciado al crear cada elemento. Antes se
	// llamaba aqui a setSpaced(true), que lo reaplicaba a TODOS los
	// elementos creados hasta ahora: O(n^2) llamadas a uiBoxSetPadded /
	// uiGroupSetMargined / uiTabSetMargined, y cada una hace que libui
	// recalcule el tamano minimo de todo el arbol (unos 65 s de arranque
	// con el procesador al 100 %). Basta con el elemento nuevo: los
	// anteriores ya estan espaciados, y Gui llama a setSpaced(true) tras
	// build() para las paginas de la pestana.
	#ifdef __MINGW32__
	setSpacedThing(elements_.back(), true);
	#endif
}


GuiHelper::Thing::Thing(void* ptr, int type) : ptr(ptr), type(type) {}
