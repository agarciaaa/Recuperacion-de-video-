/*
	Untrunc-mejorado - aprendizaje.cpp (ver aprendizaje.h)

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "aprendizaje.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "common.h"
#include "plantillas.h"
#include "resinc_ia.h"
#include "upath.h"
#include "ai/onnx/onnx_provider.h"

namespace fs = std::filesystem;

namespace aprendizaje {

namespace {

std::string g_carpeta_pruebas;
long g_umbral_pruebas = 0;

// Candidatos nuevos sin cadena (los que decide el modelo) que hacen
// falta para reentrenar. Con menos, el modelo no cambiaria.
constexpr long kUmbralReentreno = 200;
// Referencias grandes: menos danos simulados (cada uno repara el video
// entero en memoria).
constexpr uint64_t kMdatDosSemillas = 256ull * 1024 * 1024;
constexpr uint64_t kMdatUnaSemilla = 1024ull * 1024 * 1024;
// La base no crece sin limite (verificar-privacidad rechaza archivos de
// mas de 110 MB para base.tsv). 100 MB son ~800.000 casos: de sobra
// para un PC dedicado a recuperar videos de muchas camaras.
constexpr uint64_t kMaxBase = 100ull * 1024 * 1024;

std::string fechaHoy() {
	std::time_t t = std::time(nullptr);
	char buf[32] = {0};
	std::strftime(buf, sizeof buf, "%Y-%m-%d", std::localtime(&t));
	return buf;
}

uint32_t fnv1a(const void* data, size_t n, uint32_t h = 2166136261u) {
	const unsigned char* p = static_cast<const unsigned char*>(data);
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

// Resumen del contenido de la referencia: tamano + 1 MB del principio y
// 1 MB del final. Solo sirve para no aprender dos veces del mismo video.
std::string huellaReferencia(const std::string& ruta) {
	std::error_code ec;
	const fs::path p = upath::toPath(ruta);
	const uint64_t tam = fs::file_size(p, ec);
	if (ec || tam == 0) return "";
	std::ifstream f(p, std::ios::binary);
	if (!f) return "";
	const size_t kTrozo = 1024 * 1024;
	std::vector<char> buf(kTrozo);
	f.read(buf.data(), (std::streamsize)std::min<uint64_t>(kTrozo, tam));
	uint32_t a = fnv1a(buf.data(), (size_t)f.gcount());
	uint32_t b = a ^ 0x9e3779b9u;
	if (tam > kTrozo) {
		f.clear();
		f.seekg((std::streamoff)(tam - std::min<uint64_t>(kTrozo, tam)));
		f.read(buf.data(), (std::streamsize)std::min<uint64_t>(kTrozo, tam));
		b = fnv1a(buf.data(), (size_t)f.gcount(), b);
	}
	char out[64];
	std::snprintf(out, sizeof out, "%llx-%08x-%08x", (unsigned long long)tam, a, b);
	return out;
}

std::string limpio(std::string s) {
	for (char& c : s)
		if (c == '\t' || c == '\n' || c == '\r') c = ' ';
	return s;
}

struct Camara {
	int id = 0;
	std::string firma;
	long reparaciones = 0, referencias = 0, fotogramas = 0, claves = 0, audio = 0;
	double bytes_video = 0;
	long resincs = 0, sincros = 0;
	int64_t duracion_ms = 0;
	int plantilla = 0;
	std::string fecha;
};

const char* kCabeceraCamaras =
    "camara\tfirma\treparaciones\treferencias\tfotogramas\tbytes_video\tclaves\taudio\t"
    "resincs\tsincros\tduracion_ms\tplantilla\tfecha";

std::map<int, Camara> leerCamaras(const std::string& ruta) {
	std::map<int, Camara> m;
	std::ifstream f(upath::toPath(ruta), std::ios::binary);
	std::string l;
	while (std::getline(f, l)) {
		if (!l.empty() && l.back() == '\r') l.pop_back();
		if (l.empty() || l.rfind("camara\t", 0) == 0) continue;
		std::vector<std::string> c;
		std::stringstream ss(l);
		std::string x;
		while (std::getline(ss, x, '\t')) c.push_back(x);
		if (c.size() < 13) continue;
		try {
			Camara k;
			k.id = std::stoi(c[0]);
			k.firma = c[1];
			k.reparaciones = std::stol(c[2]);
			k.referencias = std::stol(c[3]);
			k.fotogramas = std::stol(c[4]);
			k.bytes_video = std::stod(c[5]);
			k.claves = std::stol(c[6]);
			k.audio = std::stol(c[7]);
			k.resincs = std::stol(c[8]);
			k.sincros = std::stol(c[9]);
			k.duracion_ms = std::stoll(c[10]);
			k.plantilla = std::stoi(c[11]);
			k.fecha = c[12];
			m[k.id] = k;
		} catch (...) {
			// linea rota: se ignora
		}
	}
	return m;
}

bool escribirAtomico(const std::string& ruta, const std::string& texto) {
	const fs::path p = upath::toPath(ruta);
	fs::path tmp = p;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f) return false;
		f << texto;
		if (!f) return false;
	}
	std::error_code ec;
	fs::rename(tmp, p, ec);
	if (ec) {
		fs::remove(p, ec);
		fs::rename(tmp, p, ec);
	}
	return !ec;
}

bool escribirCamaras(const std::string& ruta, const std::map<int, Camara>& m) {
	std::ostringstream o;
	o << kCabeceraCamaras << '\n';
	for (const auto& kv : m) {
		const Camara& k = kv.second;
		o << k.id << '\t' << limpio(k.firma) << '\t' << k.reparaciones << '\t' << k.referencias
		  << '\t' << k.fotogramas << '\t' << (long long)k.bytes_video << '\t' << k.claves << '\t'
		  << k.audio << '\t' << k.resincs << '\t' << k.sincros << '\t' << k.duracion_ms << '\t'
		  << k.plantilla << '\t' << k.fecha << '\n';
	}
	return escribirAtomico(ruta, o.str());
}

std::map<std::string, std::string> leerEstado(const std::string& ruta) {
	std::map<std::string, std::string> m;
	std::ifstream f(upath::toPath(ruta), std::ios::binary);
	std::string l;
	while (std::getline(f, l)) {
		if (!l.empty() && l.back() == '\r') l.pop_back();
		const size_t sp = l.find(' ');
		if (sp == std::string::npos) continue;
		m[l.substr(0, sp)] = l.substr(sp + 1);
	}
	return m;
}

bool escribirEstado(const std::string& ruta, const std::map<std::string, std::string>& m) {
	std::ostringstream o;
	for (const auto& kv : m) o << kv.first << ' ' << limpio(kv.second) << '\n';
	return escribirAtomico(ruta, o.str());
}

long numero(const std::map<std::string, std::string>& m, const char* k) {
	auto it = m.find(k);
	if (it == m.end()) return 0;
	try {
		return std::stol(it->second);
	} catch (...) {
		return 0;
	}
}

bool referenciaConocida(const std::string& ruta, const std::string& huella) {
	std::ifstream f(upath::toPath(ruta), std::ios::binary);
	std::string l;
	while (std::getline(f, l))
		if (l.rfind(huella + "\t", 0) == 0) return true;
	return false;
}

std::string leerTodo(const std::string& ruta) {
	std::ifstream f(upath::toPath(ruta), std::ios::binary);
	std::ostringstream o;
	o << f.rdbuf();
	return o.str();
}

} // namespace

std::string carpeta() {
	if (!g_carpeta_pruebas.empty()) return g_carpeta_pruebas;
	return upath::fromPath(upath::toPath(onnxTrainingDir()) / "aprendizaje");
}

void usarCarpeta(const std::string& ruta) { g_carpeta_pruebas = ruta; }
void umbralReentreno(long filas) { g_umbral_pruebas = filas; }

int codigoCamara(const std::string& firma) {
	return (int)(fnv1a(firma.data(), firma.size()) & 0x7fffffffu);
}

Resultado aprenderDeReparacion(const std::string& ref_path, const std::string& firma,
                               const DatosSeguros& datos) {
	Resultado r;
	const std::string env = upath::getenv("UNTRUNC_NO_APRENDER");
	if (!env.empty() && env != "0") return r;
	if (firma.empty()) return r;

	const std::string dir = carpeta();
	std::error_code ec;
	fs::create_directories(upath::toPath(dir), ec);
	if (ec) return r;
	auto en = [&dir](const char* nombre) { return upath::fromPath(upath::toPath(dir) / nombre); };
	const std::string f_base = en("base.tsv"), f_cam = en("camaras.tsv"),
	                  f_refs = en("referencias.tsv"), f_estado = en("estado.txt");

	const int cam = codigoCamara(firma);
	std::map<int, Camara> camaras = leerCamaras(f_cam);
	r.camara_nueva = camaras.find(cam) == camaras.end();
	Camara& k = camaras[cam];
	k.id = cam;
	k.firma = firma;
	k.fecha = fechaHoy();

	// 2) Lo seguro del video danado.
	k.reparaciones++;
	k.fotogramas += datos.fotogramas;
	k.bytes_video += datos.bytes_video;
	k.claves += datos.claves;
	k.audio += datos.audio;
	k.resincs += datos.resincs;
	k.sincros += datos.sincros;
	k.duracion_ms += datos.duracion_ms;

	// 1) La referencia, danada en memoria (una sola vez por video).
	std::map<std::string, std::string> estado = leerEstado(f_estado);
	const std::string huella = huellaReferencia(ref_path);
	r.referencia_repetida = huella.empty() || referenciaConocida(f_refs, huella);
	long sin_cadena = 0;
	const uint64_t tam_base = fs::exists(upath::toPath(f_base), ec)
	                              ? (uint64_t)fs::file_size(upath::toPath(f_base), ec)
	                              : 0;
	const bool base_llena = tam_base >= kMaxBase;
	if (!r.referencia_repetida && !base_llena) {
		const plantillas::InfoPlantilla info = plantillas::leerPlantilla(ref_path);
		const int semillas = !info.ok ? 0
		                     : info.mdat_bytes <= kMdatDosSemillas ? 2
		                     : info.mdat_bytes <= kMdatUnaSemilla  ? 1
		                                                           : 0;
		if (semillas > 0) {
			ResultadoResinc rr;
			Mp4* mp4 = g_mp4;
			generarDatasetResinc({ref_path}, f_base, semillas, rr, true, cam);
			g_mp4 = mp4;
			r.filas = rr.filas;
			sin_cadena = rr.filas_sin_cadena;
		}
		std::ofstream refs(upath::toPath(f_refs), std::ios::binary | std::ios::app);
		refs << huella << '\t' << cam << '\t' << fechaHoy() << '\n';
		k.referencias++;
		r.aprendido = r.filas > 0;
	}

	// 3) Plantilla automatica para esta camara.
	if (!k.plantilla) {
		const std::string id = "auto_" + std::to_string(cam);
		const fs::path destino =
		    upath::toPath(plantillas::carpetaUsuario()) / upath::toPath(id + plantillas::kExtension);
		if (fs::exists(destino, ec)) {
			k.plantilla = 1;
		} else {
			Mp4* mp4 = g_mp4;
			const plantillas::ResultadoCrear pc = plantillas::crearPlantilla(ref_path, "", id);
			g_mp4 = mp4;
			if (pc.ok) {
				k.plantilla = 1;
				r.plantilla_creada = true;
			}
		}
	}
	escribirCamaras(f_cam, camaras);
	r.camaras = (int)camaras.size();

	// 4) Reentrenar cuando hay datos nuevos suficientes o una camara
	// nueva (con al menos 2 camaras se puede validar con una no vista).
	long pendientes = numero(estado, "pendientes") + sin_cadena;
	const long umbral = g_umbral_pruebas > 0 ? g_umbral_pruebas : kUmbralReentreno;
	const bool hay_base = fs::exists(upath::toPath(f_base), ec);
	const bool toca = hay_base && pendientes > 0 &&
	                  (pendientes >= umbral || (r.camara_nueva && r.aprendido && camaras.size() >= 2));
	std::string nota_entreno;
	if (toca) {
		const std::string modelo =
		    upath::fromPath(upath::toPath(onnxTrainingDir()) / "resinc_modelo.txt");
		const std::string antes = leerTodo(modelo);
		std::string why;
		const int rc = runTrainScript({"entrenar-resinc", "--dataset", f_base, "--salida", modelo,
		                               "--solo-si-mejora"},
		                              why);
		if (rc == 0) {
			r.reentrenado = true;
			const std::string despues = leerTodo(modelo);
			r.modelo_cambiado = despues != antes;
			pendientes = 0;
			estado["entrenamientos"] = std::to_string(numero(estado, "entrenamientos") + 1);
			estado["ultimo_entreno"] = fechaHoy();
			const bool activo = despues.find("\nactivo 1") != std::string::npos;
			nota_entreno = r.modelo_cambiado
			                   ? (activo ? "modelo nuevo validado y en uso"
			                             : "modelo nuevo guardado (no validado: no se usa)")
			                   : "el modelo en uso sigue siendo el mejor";
			estado["ultimo_resultado"] = nota_entreno;
		} else {
			nota_entreno = rc < 0 ? "sin Python: se reentrenará cuando esté instalado"
			                      : "el reentrenamiento falló; se reintentará";
			estado["ultimo_resultado"] = nota_entreno;
		}
	}
	estado["pendientes"] = std::to_string(pendientes);
	escribirEstado(f_estado, estado);

	std::ostringstream o;
	o << "Aprendizaje: ";
	if (r.referencia_repetida)
		o << "esta referencia ya se había aprendido";
	else if (base_llena)
		o << "la base de casos está llena (100 MB); solo se apuntan los datos de la cámara";
	else if (r.aprendido)
		o << r.filas << " casos nuevos de esta cámara (de la referencia, dañada en memoria)";
	else
		o << "la referencia no dio casos nuevos";
	o << (r.camara_nueva ? "; cámara nueva" : "") << "; " << r.camaras
	  << (r.camaras == 1 ? " cámara" : " cámaras") << " en la base";
	if (r.plantilla_creada) o << "; plantilla automática creada para esta cámara";
	if (!nota_entreno.empty()) o << "; reentrenamiento: " << nota_entreno;
	o << ".";
	r.resumen_es = o.str();
	return r;
}

std::string resumenEs() {
	const std::string dir = carpeta();
	auto en = [&dir](const char* nombre) { return upath::fromPath(upath::toPath(dir) / nombre); };
	const std::map<int, Camara> camaras = leerCamaras(en("camaras.tsv"));
	const std::map<std::string, std::string> estado = leerEstado(en("estado.txt"));
	std::ostringstream o;
	o << "Aprendizaje del motor (" << dir << "):\n";
	if (camaras.empty()) {
		o << "  Todavía no hay nada: se aprende solo al reparar con un vídeo de referencia.\n";
		return o.str();
	}
	long filas = -1;  // sin la cabecera
	{
		std::ifstream f(upath::toPath(en("base.tsv")), std::ios::binary);
		std::string l;
		while (std::getline(f, l)) filas++;
	}
	o << "  Cámaras: " << camaras.size() << "  |  casos de resincronización: " << std::max(0L, filas)
	  << "  |  pendientes de entrenar: " << numero(estado, "pendientes")
	  << "  |  entrenamientos: " << numero(estado, "entrenamientos") << "\n";
	auto it = estado.find("ultimo_resultado");
	if (it != estado.end()) o << "  Último entrenamiento: " << it->second << "\n";
	for (const auto& kv : camaras) {
		const Camara& k = kv.second;
		o << "  - " << k.firma << " [" << k.id << "]: " << k.reparaciones
		  << (k.reparaciones == 1 ? " reparación, " : " reparaciones, ") << k.referencias
		  << (k.referencias == 1 ? " referencia" : " referencias");
		if (k.fotogramas > 0)
			o << ", " << k.fotogramas << " fotogramas recuperados (media "
			  << (long)(k.bytes_video / k.fotogramas / 1024) << " KB, "
			  << k.claves << " clave)";
		if (k.resincs > 0) o << ", " << k.resincs << " resincronizaciones";
		if (k.sincros > 0) o << ", " << k.sincros << " correcciones de sincro";
		o << (k.plantilla ? ", con plantilla" : "") << " (" << k.fecha << ")\n";
	}
	o << "  Solo números y la firma de cada cámara: ni vídeos ni rutas.\n";
	return o.str();
}

} // namespace aprendizaje
