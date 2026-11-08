/*
	Untrunc-mejorado - memoria.cpp (ver memoria.h)

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "memoria.h"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#include "plantillas.h"
#include "upath.h"

namespace fs = std::filesystem;

namespace memoria {

namespace {

std::string g_archivo_pruebas;

std::mutex& mutexMemoria() {
	static std::mutex m;
	return m;
}

struct Fila {
	std::string firma;
	bool s = false;
	bool dyn = false;
	int victorias = 0;
	std::string fecha;
};

std::vector<Fila> leer(const std::string& ruta) {
	std::vector<Fila> filas;
	std::ifstream f(upath::toPath(ruta));
	std::string line;
	while (f && std::getline(f, line)) {
		if (line.empty() || line[0] == '#') continue;
		std::vector<std::string> c;
		std::stringstream ss(line);
		std::string campo;
		while (std::getline(ss, campo, '\t')) c.push_back(campo);
		if (c.size() < 4) continue;
		Fila r;
		r.firma = c[0];
		r.s = (c[1] == "1");
		r.dyn = (c[2] == "1");
		try { r.victorias = std::stoi(c[3]); } catch (...) { continue; }
		if (c.size() > 4) r.fecha = c[4];
		filas.push_back(r);
	}
	return filas;
}

} // namespace

std::string rutaMemoria() {
	if (!g_archivo_pruebas.empty()) return g_archivo_pruebas;
	std::string home = upath::getenv("HOME");
	if (home.empty()) home = upath::getenv("USERPROFILE");
	if (home.empty()) return "";
	return upath::fromPath(upath::toPath(home) / ".config" / "untrunc" /
	                       "memoria_reparaciones.tsv");
}

void usarArchivo(const std::string& ruta) { g_archivo_pruebas = ruta; }

std::string firmaReferencia(const std::string& ref_path) {
	plantillas::InfoPlantilla info = plantillas::leerPlantilla(ref_path);
	if (!info.ok) return "";
	std::ostringstream ss;
	ss << info.video_codec << " " << info.width << "x" << info.height << " ftyp:";
	for (char ch : info.brand) ss << ((ch >= 33 && ch < 127) ? ch : '_');
	ss << " pistas:";
	for (size_t i = 0; i < info.codecs.size(); i++) ss << (i ? "," : "") << info.codecs[i];
	std::string firma = ss.str();
	for (char& ch : firma)
		if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
	return firma;
}

Recuerdo consultar(const std::string& firma) {
	Recuerdo r;
	if (firma.empty()) return r;
	std::lock_guard<std::mutex> lock(mutexMemoria());
	const std::string ruta = rutaMemoria();
	if (ruta.empty()) return r;
	for (const Fila& f : leer(ruta)) {
		if (f.firma != firma || f.victorias <= r.victorias) continue;
		r.hay = true;
		r.s = f.s;
		r.dyn = f.dyn;
		r.victorias = f.victorias;
	}
	return r;
}

bool anotar(const std::string& firma, bool s, bool dyn) {
	if (firma.empty()) return false;
	std::lock_guard<std::mutex> lock(mutexMemoria());
	const std::string ruta = rutaMemoria();
	if (ruta.empty()) return false;
	std::vector<Fila> filas = leer(ruta);
	char fecha[32] = {0};
	std::time_t t = std::time(nullptr);
	std::strftime(fecha, sizeof(fecha), "%Y-%m-%d", std::localtime(&t));
	bool hecha = false;
	for (Fila& f : filas) {
		if (f.firma == firma && f.s == s && f.dyn == dyn) {
			f.victorias++;
			f.fecha = fecha;
			hecha = true;
		}
	}
	if (!hecha) {
		Fila f;
		f.firma = firma;
		f.s = s;
		f.dyn = dyn;
		f.victorias = 1;
		f.fecha = fecha;
		filas.push_back(f);
	}
	// Escritura atomica: temporal + renombrar.
	std::error_code ec;
	const fs::path p = upath::toPath(ruta);
	fs::create_directories(p.parent_path(), ec);
	fs::path tmp = p;
	tmp += ".tmp";
	{
		std::ofstream o(tmp, std::ios::trunc);
		if (!o) return false;
		o << "# untrunc: estrategia que mejor funciono por tipo de camara.\n"
		     "# Solo firmas de cabecera (codec, resolucion, marcas); sin rutas ni videos.\n";
		for (const Fila& f : filas)
			o << f.firma << '\t' << (f.s ? 1 : 0) << '\t' << (f.dyn ? 1 : 0) << '\t'
			  << f.victorias << '\t' << f.fecha << '\n';
		if (!o) return false;
	}
	fs::rename(tmp, p, ec);
	return !ec;
}

} // namespace memoria
