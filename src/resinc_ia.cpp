// Modelo de resincronizacion: datos de entrenamiento, evaluacion y uso.
// Ver resinc_ia.h.

#include "resinc_ia.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
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
	     skip_existing = g_skip_existing, resync = g_resync_enabled, sincro = g_sincro_enabled;
	LogMode log = g_log_mode;
	Mp4* mp4 = g_mp4;
	// La simulacion no debe mover la barra de progreso de la GUI ni usar
	// el modelo activo (los datos tienen que salir del nucleo tal cual).
	void (*progreso)(int) = g_onProgress;
	void (*estado)(const std::string&) = g_onStatus;
	double (*scorer)(const float*, int) = g_resync_scorer;
	double umbral = g_resync_umbral;
	GlobalesSimulacion() {
		g_onProgress = nullptr;
		g_onStatus = nullptr;
		g_resync_scorer = nullptr;
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
		g_sincro_enabled = sincro;
		g_log_mode = log;
		g_mp4 = mp4;
		g_onProgress = progreso;
		g_onStatus = estado;
		g_resync_scorer = scorer;
		g_resync_umbral = umbral;
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
                std::string& err, std::vector<int64_t>* audio = nullptr, double* dv_ms = nullptr,
                double* da_ms = nullptr) {
	try {
		Mp4 mp4;
		g_mp4 = &mp4;
		mp4.parseOk(sano);
		mp4.verdadMuestras(verdad, ini, fin, audio);
		if (dv_ms && da_ms) mp4.duracionesMs(*dv_ms, *da_ms);
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
             std::vector<int64_t>& recuperadas, std::string& err,
             std::vector<int64_t>* audio = nullptr,
             const std::function<void(Mp4&)>& tras = nullptr) {
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
		mp4.recuperadasVideo(recuperadas, audio);
		if (tras) tras(mp4);
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
                         int semillas, ResultadoResinc& r, bool anexar, int grupo) {
	std::error_code ec;
	const bool con_cabecera =
	    !anexar || !fs::exists(upath::toPath(salida_tsv), ec) || fs::file_size(upath::toPath(salida_tsv), ec) == 0;
	std::ofstream out(upath::toPath(salida_tsv),
	                  anexar ? std::ios::binary | std::ios::app : std::ios::binary);
	if (!out) {
		r.resumen_es = "No se pudo crear " + salida_tsv;
		return 1;
	}
	std::ofstream* tsv = &out;
	if (con_cabecera) {
		out << "video\ttipo\tsemilla\tetiqueta\tcadena\tcodec";
		for (int i = 0; i < kResincNFeat; i++) out << '\t' << kResincNombres[i];
		out << '\n';
	}

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
				g_ctx = Contexto{tsv, grupo >= 0 ? grupo : (int)v, t, (uint32_t)s, &verdad, &r};
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

// Tiempos verdaderos de cada muestra del video sano.
bool tiemposVerdad(const std::string& sano, std::vector<Mp4::MuestraT>& tv,
                   std::vector<Mp4::MuestraT>& ta) {
	try {
		Mp4 mp4;
		g_mp4 = &mp4;
		mp4.parseOk(sano);
		tv = mp4.muestrasConTiempo(false, true);
		ta = mp4.muestrasConTiempo(true, true);
		g_mp4 = nullptr;
	} catch (...) {
		g_mp4 = nullptr;
		return false;
	}
	auto orden = [](const Mp4::MuestraT& a, const Mp4::MuestraT& b) { return a.off < b.off; };
	std::sort(tv.begin(), tv.end(), orden);
	std::sort(ta.begin(), ta.end(), orden);
	return !tv.empty() && !ta.empty();
}

// Retraso (verdad - recuperado, ms) de la primera muestra recuperada en
// [desde, hasta) que sea una muestra real (o la ultima, si ultima=true).
bool retraso(const std::vector<Mp4::MuestraT>& rec, const std::vector<Mp4::MuestraT>& verdad,
             int64_t desde, int64_t hasta, bool ultima, double& r) {
	auto en = [&](const Mp4::MuestraT& m) {
		auto it = std::lower_bound(verdad.begin(), verdad.end(), m.off,
		                           [](const Mp4::MuestraT& v, int64_t o) { return v.off < o; });
		if (it == verdad.end() || it->off != m.off) return false;
		r = it->t_ms - m.t_ms;
		return true;
	};
	if (ultima) {
		for (auto it = rec.rbegin(); it != rec.rend(); ++it)
			if (it->off >= desde && it->off < hasta && en(*it)) return true;
	} else {
		for (const auto& m : rec)
			if (m.off >= desde && m.off < hasta && en(m)) return true;
	}
	return false;
}

int evaluarAudio(const std::vector<std::string>& sanos, int semillas, ResultadoAudio& r) {
	std::ostringstream log;
	{
		GlobalesSimulacion globales;
		for (size_t v = 0; v < sanos.size(); v++) {
			std::vector<int64_t> vv, va;
			int64_t ini = 0, fin = 0;
			std::string err;
			double ms_video = 0, ms_audio = 0;
			if (!leerVerdad(sanos[v], vv, ini, fin, err, &va, &ms_video, &ms_audio)) {
				log << "  video " << v << " (" << nombreBase(sanos[v]) << "): se omite, " << err << "\n";
				continue;
			}
			if (va.empty()) {
				log << "  video " << v << " (" << nombreBase(sanos[v]) << "): se omite, sin audio AAC\n";
				continue;
			}
			std::vector<Mp4::MuestraT> tv, ta;
			const bool con_tiempos = tiemposVerdad(sanos[v], tv, ta);
			r.videos++;
			// Escenario -1: sin dano (lo que se pierde aunque el archivo este sano).
			for (int t = -1; t < kNTiposDano; t++)
				for (int s = 1; s <= (t < 0 ? 1 : semillas); s++) {
					const auto zonas = t < 0 ? std::vector<ZonaDanoSimulado>{}
					                         : escenario(t, 2000 + s, ini, fin);
					// Lo que el dano destruye no se puede recuperar: no cuenta.
					auto intacto = [&](int64_t o) {
						for (const auto& z : zonas)
							if (o + 16 > z.ini && o < z.fin) return false;
						return true;
					};
					std::vector<int64_t> rv, ra;
					std::vector<Mp4::MuestraT> mv, ma;
					std::vector<Mp4::HuecoSincro> huecos;
					auto tras = [&](Mp4& m) {
						mv = m.muestrasConTiempo(false, false);
						ma = m.muestrasConTiempo(true, false);
						huecos = m.estimarHuecos();
					};
					r.escenarios++;
					g_sincro_enabled = false;  // primero, tal cual queda sin corregir
					if (!simular(sanos[v], zonas, rv, err, &ra, tras)) {
						r.fallos++;
						continue;
					}
					auto& f = r.por_tipo[t + 1];
					f.reparaciones++;
					if (con_tiempos && !mv.empty() && !ma.empty()) {
						const int64_t inf = std::numeric_limits<int64_t>::max();
						double corr_total = 0;
						for (size_t g = 0; g < huecos.size(); g++) {
							const auto& h = huecos[g];
							const int64_t antes = g ? huecos[g - 1].fin : 0;
							const int64_t despues = g + 1 < huecos.size() ? huecos[g + 1].ini : inf;
							double v0, v1, a0, a1;
							const double est = h.correccion_video_ms;
							corr_total += est;
							if (!retraso(mv, tv, antes, h.ini, true, v0) ||
							    !retraso(mv, tv, h.fin, despues, false, v1) ||
							    !retraso(ma, ta, antes, h.ini, true, a0) ||
							    !retraso(ma, ta, h.fin, despues, false, a1))
								continue;
							const double necesario = (v1 - v0) - (a1 - a0);
							f.huecos++;
							f.hueco_abs_ms += std::fabs(necesario);
							f.hueco_abs_corregido_ms += std::fabs(necesario - est);
							if (std::fabs(necesario) > 45) {
								f.huecos_notables++;
								if (std::fabs(necesario - est) > 45) f.huecos_notables_corregidos++;
							}
						}
						double vf, af;
						if (retraso(mv, tv, 0, inf, true, vf) && retraso(ma, ta, 0, inf, true, af)) {
							f.finales++;
							f.final_abs_ms += std::fabs(vf - af);
							f.final_abs_corregido_ms += std::fabs(vf - af - corr_total);
							f.final_max_ms = std::max(f.final_max_ms, std::fabs(vf - af));
							if (std::fabs(vf - af) > 45) f.finales_notables++;
						}
						// Y ahora reparando de verdad con la correccion.
						std::vector<int64_t> rv2, ra2;
						std::vector<Mp4::MuestraT> mv2, ma2;
						g_sincro_enabled = true;
						const bool ok2 = simular(sanos[v], zonas, rv2, err, &ra2, [&](Mp4& m) {
							mv2 = m.muestrasConTiempo(false, false);
							ma2 = m.muestrasConTiempo(true, false);
						});
						g_sincro_enabled = false;
						if (ok2 && retraso(mv2, tv, 0, inf, true, vf) && retraso(ma2, ta, 0, inf, true, af)) {
							f.finales_aplicados++;
							f.final_abs_aplicado_ms += std::fabs(vf - af);
							f.final_max_aplicado_ms = std::max(f.final_max_aplicado_ms, std::fabs(vf - af));
							if (std::fabs(vf - af) > 45) f.finales_notables_aplicados++;
						}
					}
					if (!vv.empty() && !va.empty()) {
						const double dv = ms_video / vv.size(), da = ms_audio / va.size();
						const double desfase = ((double)va.size() - ra.size()) * da -
						                       ((double)vv.size() - rv.size()) * dv;
						f.desfase_abs_ms += std::fabs(desfase);
						f.desfase_max_ms = std::max(f.desfase_max_ms, std::fabs(desfase));
					}
					// Para cada zona: lo primero recuperado despues de ella.
					std::vector<int64_t> todo(rv);
					todo.insert(todo.end(), ra.begin(), ra.end());
					std::sort(todo.begin(), todo.end());
					std::vector<int64_t> reenganche;
					for (const auto& z : zonas) {
						auto it = std::lower_bound(todo.begin(), todo.end(), z.fin);
						reenganche.push_back(it == todo.end() ? fin : *it);
						f.zonas++;
						f.bytes_hasta_reenganche += (double)(reenganche.back() - z.fin);
					}
					auto antesDeReenganchar = [&](int64_t o) {
						for (size_t k = 0; k < zonas.size(); k++)
							if (o >= zonas[k].fin && o < reenganche[k]) return true;
						return false;
					};
					for (int64_t o : va)
						if (intacto(o)) {
							f.audio_posibles++;
							if (esVerdad(ra, o)) f.audio_bien++;
							else if (antesDeReenganchar(o)) f.audio_antes_reenganche++;
						}
					for (int64_t o : ra)
						if (!esVerdad(va, o)) f.audio_falsos++;
					for (int64_t o : vv)
						if (intacto(o)) {
							f.video_posibles++;
							if (esVerdad(rv, o)) f.video_bien++;
							else if (antesDeReenganchar(o)) f.video_antes_reenganche++;
						}
				}
		}
	}
	std::ostringstream ss;
	ss << "Audio (AAC): " << r.videos << " vídeos con audio, " << r.escenarios << " reparaciones ("
	   << r.fallos << " fallidas).\n" << log.str()
	   << "Solo cuenta lo que el daño no destruyó (lo recuperable).\n";
	const char* nombres[] = {"sin daño", "basura", "ceros", "copia", "denso", "mixto"};
	for (int i = 0; i <= kNTiposDano; i++) {
		const auto& f = r.por_tipo[i];
		if (!f.reparaciones) continue;
		auto pct = [](long a, long b) { return b ? 100.0 * a / b : 0.0; };
		char buf[256];
		snprintf(buf, sizeof buf,
		         "  %-9s audio %6ld/%-6ld (%5.1f %%), falsos %4ld | vídeo %6ld/%-6ld (%5.1f %%)\n",
		         nombres[i], f.audio_bien, f.audio_posibles, pct(f.audio_bien, f.audio_posibles),
		         f.audio_falsos, f.video_bien, f.video_posibles, pct(f.video_bien, f.video_posibles));
		ss << buf;
		if (f.zonas) {
			snprintf(buf, sizeof buf,
			         "            perdidos justo tras la zona: audio %ld, vídeo %ld; "
			         "%.0f bytes de media hasta reenganchar\n",
			         f.audio_antes_reenganche, f.video_antes_reenganche,
			         f.bytes_hasta_reenganche / f.zonas);
			ss << buf;
		}
		snprintf(buf, sizeof buf, "            desfase audio-vídeo al final: media %.0f ms, máximo %.0f ms\n",
		         f.desfase_abs_ms / f.reparaciones, f.desfase_max_ms);
		ss << buf;
		if (f.huecos) {
			snprintf(buf, sizeof buf,
			         "            por zona: desfase medio %.0f ms -> %.0f ms corregido; "
			         "zonas > 45 ms: %ld -> %ld\n",
			         f.hueco_abs_ms / f.huecos, f.hueco_abs_corregido_ms / f.huecos, f.huecos_notables,
			         f.huecos_notables_corregidos);
			ss << buf;
		}
		if (f.finales && f.finales_aplicados) {
			snprintf(buf, sizeof buf,
			         "            desfase al final del vídeo: media %.0f ms -> %.0f ms; máximo %.0f -> %.0f ms; "
			         "> 45 ms: %ld -> %ld de %ld\n",
			         f.final_abs_ms / f.finales, f.final_abs_aplicado_ms / f.finales_aplicados,
			         f.final_max_ms, f.final_max_aplicado_ms, f.finales_notables,
			         f.finales_notables_aplicados, f.finales);
			ss << buf;
		}
	}
	r.resumen_es = ss.str();
	return r.videos > 0 ? 0 : 1;
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
