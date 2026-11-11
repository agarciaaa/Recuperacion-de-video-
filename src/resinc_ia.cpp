// Modelo de resincronizacion: datos de entrenamiento, evaluacion y uso.
// Ver resinc_ia.h.

#include "resinc_ia.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>

#include "file.h"
#include "mp4.h"
#include "upath.h"

namespace fs = std::filesystem;
#include "ai/onnx/onnx_provider.h"

const char* const kResincNombres[kResincNFeat] = {
	"cadena_de_4", "cadena_cero", "log_tam_relativo", "clave",
	"nal_corte", "nal_idr", "nal_sei", "nal_parametros", "nal_aud", "nal_otro",
	"log_desconocido", "tras_ceros", "tras_entropia", "tras_encaja",
	"ini_ceros", "ini_entropia", "excede_limite",
};

namespace {

// ---------------------------------------------------------------------------
// Escenarios de dano (deterministas: misma semilla, mismas zonas)
// ---------------------------------------------------------------------------
struct Azar {
	uint64_t s;
	explicit Azar(uint64_t semilla) : s(semilla * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
	uint64_t sig() {
		s ^= s << 13; s ^= s >> 7; s ^= s << 17;
		return s;
	}
	int64_t entre(int64_t a, int64_t b) {  // [a, b]
		if (b <= a) return a;
		return a + (int64_t)(sig() % (uint64_t)(b - a + 1));
	}
};

const char* const kTiposDano[] = {"basura", "ceros", "copia", "denso", "mixto"};
constexpr int kNTiposDano = 5;

std::vector<ZonaDanoSimulado> escenario(int tipo, uint32_t semilla, int64_t ini, int64_t fin) {
	std::vector<ZonaDanoSimulado> z;
	const int64_t L = fin - ini;
	if (L < 4096) return z;
	Azar az(((uint64_t)tipo << 32) ^ semilla ^ (uint64_t)L);
	// El principio queda sano: la reparacion necesita encontrar el arranque.
	const int64_t desde = ini + L / 20;
	auto zona = [&](int t, int64_t tam_min, int64_t tam_max) {
		tam_max = std::max<int64_t>(tam_min, std::min<int64_t>(tam_max, L / 4));
		ZonaDanoSimulado d;
		d.tipo = t;
		const int64_t tam = az.entre(tam_min, tam_max);
		d.ini = az.entre(desde, std::max(desde, fin - tam - 1));
		d.fin = std::min(fin, d.ini + tam);
		d.semilla = (uint32_t)az.sig();
		if (t == ZonaDanoSimulado::Copia)  // bytes reales de otra parte del video
			d.origen = az.entre(ini, std::max(ini, fin - tam - 1));
		z.push_back(d);
	};
	const int64_t grande = std::max<int64_t>(4096, L / 20);
	switch (tipo) {
	case 0:  // 4 zonas de basura
		for (int i = 0; i < 4; i++) zona(ZonaDanoSimulado::Aleatorio, 1024, std::min<int64_t>(65536, grande));
		break;
	case 1:  // 1 o 2 zonas grandes de ceros
		for (int i = 0, n = (int)az.entre(1, 2); i < n; i++)
			zona(ZonaDanoSimulado::Ceros, grande / 2, grande * 2);
		break;
	case 2:  // trozos de video real copiados en otro sitio
		for (int i = 0; i < 2; i++) zona(ZonaDanoSimulado::Copia, 2048, grande);
		break;
	case 3:  // muchas zonas pequenas: aqui la cadena no suele decidir
		for (int i = 0, n = (int)az.entre(8, 16); i < n; i++)
			zona(ZonaDanoSimulado::Aleatorio, 128, 8192);
		break;
	default:  // mezcla
		for (int i = 0; i < 3; i++) zona(ZonaDanoSimulado::Aleatorio, 256, 16384);
		for (int i = 0; i < 3; i++) zona(ZonaDanoSimulado::Ceros, 256, 16384);
		zona(ZonaDanoSimulado::Copia, 1024, grande);
		break;
	}
	std::sort(z.begin(), z.end(),
	          [](const ZonaDanoSimulado& a, const ZonaDanoSimulado& b) { return a.ini < b.ini; });
	return z;
}

// ---------------------------------------------------------------------------
// Reparacion simulada
// ---------------------------------------------------------------------------
struct Contexto {
	std::ofstream* tsv = nullptr;
	int video = 0, tipo = 0;
	uint32_t semilla = 0;
	const std::vector<int64_t>* verdad = nullptr;
	ResultadoResinc* r = nullptr;
};
Contexto g_ctx;

bool esVerdad(const std::vector<int64_t>& v, int64_t off) {
	return std::binary_search(v.begin(), v.end(), off);
}

void sumidero(int64_t off_abs, const char* codec, int cadena, const float* f, int n) {
	if (!g_ctx.tsv || !g_ctx.verdad) return;
	const int et = esVerdad(*g_ctx.verdad, off_abs) ? 1 : 0;
	char buf[64];
	snprintf(buf, sizeof buf, "%d\t%d\t%u\t%d\t%d\t", g_ctx.video, g_ctx.tipo, g_ctx.semilla, et, cadena);
	std::string fila = buf;
	fila += codec;
	for (int i = 0; i < n; i++) {
		snprintf(buf, sizeof buf, "\t%.5g", f[i]);
		fila += buf;
	}
	fila += '\n';
	*g_ctx.tsv << fila;
	auto& r = *g_ctx.r;
	r.filas++;
	(et ? r.positivos : r.negativos)++;
	if (cadena < 2) {
		r.filas_sin_cadena++;
		if (et) r.positivos_sin_cadena++;
	}
}

// Guarda y restaura los globales que la simulacion cambia.
struct GlobalesSimulacion {
	bool ignore_unknown = g_ignore_unknown, interactive = g_interactive,
	     skip_existing = g_skip_existing, resync = g_resync_enabled;
	LogMode log = g_log_mode;
	Mp4* mp4 = g_mp4;
	GlobalesSimulacion() {
		g_ignore_unknown = true;  // -s: la resincronizacion solo actua asi
		g_interactive = false;
		g_skip_existing = false;
		g_resync_enabled = true;
		g_log_mode = LogMode::ET;
		g_solo_simular = true;
	}
	~GlobalesSimulacion() {
		g_dano_simulado_activo = false;
		g_dano_simulado.clear();
		g_solo_simular = false;
		g_resync_sink = nullptr;
		g_ignore_unknown = ignore_unknown;
		g_interactive = interactive;
		g_skip_existing = skip_existing;
		g_resync_enabled = resync;
		g_log_mode = log;
		g_mp4 = mp4;
	}
};

std::string textoExcepcion(std::exception_ptr e) {
	try {
		std::rethrow_exception(e);
	} catch (const char* s) {
		return s;
	} catch (const std::string& s) {
		return s;
	} catch (const std::exception& x) {
		return x.what();
	} catch (...) {
		return "error desconocido";
	}
}

// Verdad del video sano: inicios de fotogramas de video y rango de datos.
bool leerVerdad(const std::string& sano, std::vector<int64_t>& verdad, int64_t& ini, int64_t& fin,
                std::string& err) {
	try {
		Mp4 mp4;
		g_mp4 = &mp4;
		mp4.parseOk(sano);
		mp4.verdadMuestras(verdad, ini, fin);
		g_mp4 = nullptr;
	} catch (...) {
		g_mp4 = nullptr;
		err = textoExcepcion(std::current_exception());
		return false;
	}
	if (verdad.empty()) {
		err = "no tiene pista de video H.264/HEVC";
		return false;
	}
	return true;
}

// Repara `sano` viendolo danado con `zonas`; devuelve los inicios de los
// fotogramas de video recuperados.
bool simular(const std::string& sano, const std::vector<ZonaDanoSimulado>& zonas,
             std::vector<int64_t>& recuperadas, std::string& err) {
	try {
		Mp4 mp4;
		g_mp4 = &mp4;
		mp4.parseOk(sano);  // la referencia se lee sana
		g_dano_simulado = zonas;
		g_dano_simulado_activo = true;  // el "danado" se abre dentro de repair
		// Misma ruta escrita de otra forma: con el mismo texto, repair entra
		// en su modo de depuracion (use_offset_map_), que no es el normal.
		const size_t barra = sano.find_last_of("/\\");
		const std::string alias = barra == std::string::npos
		                              ? "./" + sano
		                              : sano.substr(0, barra + 1) + "./" + sano.substr(barra + 1);
		mp4.repair(alias);
		g_dano_simulado_activo = false;
		mp4.recuperadasVideo(recuperadas);
		g_mp4 = nullptr;
	} catch (...) {
		g_dano_simulado_activo = false;
		g_mp4 = nullptr;
		err = textoExcepcion(std::current_exception());
		return false;
	}
	return true;
}

void contar(const std::vector<int64_t>& verdad, const std::vector<int64_t>& rec, long& v, long& f) {
	for (int64_t o : rec) (esVerdad(verdad, o) ? v : f)++;
}

std::string nombreBase(const std::string& p) {
	const size_t i = p.find_last_of("/\\");
	return i == std::string::npos ? p : p.substr(i + 1);
}

// ---------------------------------------------------------------------------
// Modelo: capas densas (ReLU en las ocultas, sigmoide a la salida)
// ---------------------------------------------------------------------------
struct Capa {
	int entra = 0, sale = 0;
	std::vector<double> w, b;  // w[o*entra + i]
};
struct Modelo {
	bool activo = false;
	double umbral = 1.1;
	std::vector<double> media, escala;
	std::vector<Capa> capas;
};
std::unique_ptr<Modelo> g_modelo;

bool leerModelo(const std::string& ruta, Modelo& m, std::string& why_es) {
	std::ifstream in(upath::toPath(ruta));
	if (!in) {
		why_es = "no se pudo abrir el modelo de resincronización";
		return false;
	}
	std::string linea, clave;
	bool cabecera = false;
	int nfeat = -1;
	while (std::getline(in, linea)) {
		if (linea.empty() || linea[0] == '#') continue;
		std::istringstream ss(linea);
		ss >> clave;
		if (clave == "untrunc-resinc") {
			int ver = 0;
			ss >> ver;
			cabecera = ver == 1;
		} else if (clave == "activo") {
			int a = 0;
			ss >> a;
			m.activo = a == 1;
		} else if (clave == "umbral") {
			ss >> m.umbral;
		} else if (clave == "nfeat") {
			ss >> nfeat;
		} else if (clave == "media" || clave == "escala") {
			auto& v = clave == "media" ? m.media : m.escala;
			double x;
			while (ss >> x) v.push_back(x);
		} else if (clave == "capa") {
			Capa c;
			ss >> c.entra >> c.sale;
			if (c.entra <= 0 || c.sale <= 0 || c.entra > 4096 || c.sale > 4096) break;
			double x;
			while ((int)c.w.size() < c.entra * c.sale && in >> x) c.w.push_back(x);
			while ((int)c.b.size() < c.sale && in >> x) c.b.push_back(x);
			m.capas.push_back(std::move(c));
		}
	}
	bool ok = cabecera && nfeat == kResincNFeat && (int)m.media.size() == kResincNFeat &&
	          (int)m.escala.size() == kResincNFeat && !m.capas.empty() &&
	          m.capas.front().entra == kResincNFeat && m.capas.back().sale == 1;
	for (size_t i = 0; ok && i < m.capas.size(); i++) {
		const Capa& c = m.capas[i];
		ok = (int)c.w.size() == c.entra * c.sale && (int)c.b.size() == c.sale &&
		     (i == 0 || m.capas[i - 1].sale == c.entra);
	}
	if (!ok) why_es = "el modelo de resincronización está incompleto o es de otra versión";
	return ok;
}

double puntuar(const float* f, int n) {
	if (!g_modelo || n != kResincNFeat) return 0.0;
	const Modelo& m = *g_modelo;
	std::vector<double> x(n), y;
	for (int i = 0; i < n; i++) x[i] = (f[i] - m.media[i]) / (m.escala[i] > 1e-9 ? m.escala[i] : 1.0);
	for (size_t k = 0; k < m.capas.size(); k++) {
		const Capa& c = m.capas[k];
		y.assign(c.sale, 0.0);
		for (int o = 0; o < c.sale; o++) {
			double s = c.b[o];
			for (int i = 0; i < c.entra; i++) s += c.w[(size_t)o * c.entra + i] * x[i];
			y[o] = k + 1 < m.capas.size() ? std::max(0.0, s) : s;
		}
		x.swap(y);
	}
	return 1.0 / (1.0 + std::exp(-x[0]));
}

}  // namespace

// ---------------------------------------------------------------------------
int generarDatasetResinc(const std::vector<std::string>& sanos, const std::string& salida_tsv,
                         int semillas, ResultadoResinc& r) {
	std::ofstream out(upath::toPath(salida_tsv), std::ios::binary);
	if (!out) {
		r.resumen_es = "No se pudo crear " + salida_tsv;
		return 1;
	}
	std::ofstream* tsv = &out;
	out << "video\ttipo\tsemilla\tetiqueta\tcadena\tcodec";
	for (int i = 0; i < kResincNFeat; i++) out << '\t' << kResincNombres[i];
	out << '\n';

	std::ostringstream log;
	GlobalesSimulacion globales;
	for (size_t v = 0; v < sanos.size(); v++) {
		std::vector<int64_t> verdad;
		int64_t ini = 0, fin = 0;
		std::string err;
		if (!leerVerdad(sanos[v], verdad, ini, fin, err)) {
			log << "  video " << v << " (" << nombreBase(sanos[v]) << "): se omite, " << err << "\n";
			continue;
		}
		r.videos++;
		const long antes = r.filas;
		for (int t = 0; t < kNTiposDano; t++)
			for (int s = 1; s <= semillas; s++) {
				g_ctx = Contexto{tsv, (int)v, t, (uint32_t)s, &verdad, &r};
				g_resync_sink = sumidero;
				std::vector<int64_t> rec;
				r.escenarios++;
				if (!simular(sanos[v], escenario(t, s, ini, fin), rec, err)) r.fallos++;
				g_resync_sink = nullptr;
			}
		log << "  video " << v << " (" << nombreBase(sanos[v]) << "): " << verdad.size()
		    << " fotogramas, " << (r.filas - antes) << " candidatos\n";
	}
	g_ctx = Contexto{};
	out.close();

	std::ostringstream ss;
	ss << "Dataset de resincronización: " << r.videos << " vídeos, " << r.escenarios
	   << " daños simulados en memoria (" << r.fallos << " fallidos).\n"
	   << log.str() << "Candidatos: " << r.filas << " (" << r.positivos << " reales, "
	   << r.negativos << " falsos). Sin cadena (lo que decidiría el modelo): "
	   << r.filas_sin_cadena << " (" << r.positivos_sin_cadena << " reales).\n"
	   << "Solo números: ni rutas ni bytes de los vídeos.";
	r.resumen_es = ss.str();
	return r.filas > 0 ? 0 : 1;
}

int evaluarResinc(const std::vector<std::string>& sanos, const std::string& ruta_modelo,
                  int semillas, ResultadoResinc& r) {
	std::string why;
	if (!ruta_modelo.empty() && !activarModeloResinc(ruta_modelo, false, why)) {
		r.resumen_es = why;
		return 1;
	}
	const bool con_modelo = !ruta_modelo.empty();
	double (*scorer)(const float*, int) = g_resync_scorer;
	const double umbral = g_resync_umbral;

	std::ostringstream log;
	{
		GlobalesSimulacion globales;
		for (size_t v = 0; v < sanos.size(); v++) {
			std::vector<int64_t> verdad;
			int64_t ini = 0, fin = 0;
			std::string err;
			if (!leerVerdad(sanos[v], verdad, ini, fin, err)) {
				log << "  video " << v << " (" << nombreBase(sanos[v]) << "): se omite, " << err << "\n";
				continue;
			}
			r.videos++;
			long vs = 0, fs = 0, vc = 0, fc = 0;
			for (int t = 0; t < kNTiposDano; t++)
				for (int s = 1; s <= semillas; s++) {
					// Semillas distintas de las del dataset.
					const auto zonas = escenario(t, 1000 + s, ini, fin);
					std::vector<int64_t> rec;
					r.escenarios++;
					g_resync_scorer = nullptr;
					if (!simular(sanos[v], zonas, rec, err)) {
						r.fallos++;
						continue;
					}
					contar(verdad, rec, vs, fs);
					if (!con_modelo) continue;
					g_resync_scorer = scorer;
					g_resync_umbral = umbral;
					if (!simular(sanos[v], zonas, rec, err)) {
						r.fallos++;
						continue;
					}
					r.decisiones_modelo += g_resync_ia_count;
					contar(verdad, rec, vc, fc);
				}
			r.verdaderos_sin += vs;
			r.falsos_sin += fs;
			r.verdaderos_con += vc;
			r.falsos_con += fc;
			log << "  video " << v << " (" << nombreBase(sanos[v]) << "): sin modelo " << vs
			    << " reales / " << fs << " falsos";
			if (con_modelo) log << "; con modelo " << vc << " reales / " << fc << " falsos";
			log << "\n";
		}
	}
	if (con_modelo) desactivarModeloResinc();

	std::ostringstream ss;
	ss << "Evaluación de resincronización: " << r.videos << " vídeos, " << r.escenarios
	   << " reparaciones (" << r.fallos << " fallidas).\n"
	   << log.str() << "Fotogramas recuperados sin modelo: " << r.verdaderos_sin << " reales, "
	   << r.falsos_sin << " falsos.";
	if (con_modelo)
		ss << "\nCon modelo: " << r.verdaderos_con << " reales, " << r.falsos_con << " falsos ("
		   << r.decisiones_modelo << " decisiones del modelo). Diferencia: "
		   << (r.verdaderos_con - r.verdaderos_sin) << " reales, " << (r.falsos_con - r.falsos_sin)
		   << " falsos.";
	r.resumen_es = ss.str();
	return 0;
}

bool activarModeloResinc(const std::string& ruta, bool exigir_activo, std::string& why_es) {
	auto m = std::make_unique<Modelo>();
	if (!leerModelo(ruta, *m, why_es)) {
		desactivarModeloResinc();
		return false;
	}
	if (exigir_activo && !m->activo) {
		why_es = "el modelo de resincronización no pasó la validación (activo 0)";
		desactivarModeloResinc();
		return false;
	}
	g_resync_umbral = m->umbral;
	g_modelo = std::move(m);
	g_resync_scorer = puntuar;
	return true;
}

void desactivarModeloResinc() {
	g_resync_scorer = nullptr;
	g_resync_umbral = 1.1;
	g_modelo.reset();
}

std::string rutaModeloResincEnUso(const char* argv0) {
	auto existe = [](const std::string& p) {
		std::error_code ec;
		return !p.empty() && fs::exists(upath::toPath(p), ec) && !ec;
	};
	const std::string env = upath::getenv("UNTRUNC_MODELO_RESINC");
	if (existe(env)) return env;
	const std::string entrenado = onnxTrainingDir() + "/resinc_modelo.txt";
	if (existe(entrenado)) return entrenado;
	const std::string base = findBaseModelDir(argv0);
	if (!base.empty() && existe(base + "/resinc_modelo.txt")) return base + "/resinc_modelo.txt";
	return "";
}
