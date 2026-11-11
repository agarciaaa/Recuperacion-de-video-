/*
	Untrunc-mejorado - tarjeta.cpp (ver tarjeta.h)

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "tarjeta.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "plantillas.h"
#include "repair_job.h"
#include "upath.h"
#include "verificar.h"
#include "ai/local/sps_detector.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace tarjeta {

namespace {

constexpr size_t kBloque = 4u * 1024 * 1024;  // lectura del origen
constexpr size_t kSector = 512;
constexpr uint64_t kPasoVacio = 1024 * 1024;  // para buscar el final de datos
// Un video incompleto sin tamano conocido no se alarga mas de esto.
constexpr uint64_t kMaxIncompleto = 32ull * 1024 * 1024 * 1024;
// Referencias que se prueban como mucho con cada video incompleto.
constexpr size_t kMaxCandidatas = 5;

uint32_t be32(const unsigned char* p) {
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
uint64_t be64(const unsigned char* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

bool imprimible4(const unsigned char* p) {
	for (int i = 0; i < 4; i++)
		if (!(std::isalnum(p[i]) || p[i] == ' ')) return false;
	return true;
}

// Cajas de primer nivel que se aceptan al seguir un video.
bool cajaConocida(const std::string& t) {
	static const char* k[] = {"ftyp", "moov", "mdat", "free", "skip", "wide", "uuid",
	                          "udta", "meta", "pnot", "PICT", "junk", "pdin", "styp",
	                          "sidx", "moof", "mfra", "prfl"};
	for (const char* x : k)
		if (t == x) return true;
	return false;
}

std::string conMarca(const std::string& marca) { return marca == "qt  " ? ".mov" : ".mp4"; }

std::string mb(uint64_t b) {
	std::ostringstream o;
	if (b >= 1024ull * 1024 * 1024)
		o << (double)(b * 10 / (1024ull * 1024 * 1024)) / 10 << " GB";
	else
		o << (b + 512 * 1024) / (1024 * 1024) << " MB";
	return o.str();
}

}  // namespace

// ------------------------------------------------------------ Fuente

struct Fuente::Impl {
#ifdef _WIN32
	HANDLE h = INVALID_HANDLE_VALUE;
#else
	int fd = -1;
#endif
	unsigned char* buf = nullptr;
	Impl() {
#ifdef _WIN32
		buf = (unsigned char*)VirtualAlloc(nullptr, kBloque, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
		buf = new unsigned char[kBloque];
#endif
	}
	~Impl() {
#ifdef _WIN32
		if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
		if (buf) VirtualFree(buf, 0, MEM_RELEASE);
#else
		if (fd >= 0) close(fd);
		delete[] buf;
#endif
	}
	// Lectura cruda alineada; false si falla.
	bool leerCrudo(uint64_t off, unsigned char* dst, size_t n) {
#ifdef _WIN32
		LARGE_INTEGER li;
		li.QuadPart = (LONGLONG)off;
		if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
		DWORD leidos = 0;
		if (!ReadFile(h, dst, (DWORD)n, &leidos, nullptr)) return false;
		if (leidos < n) std::memset(dst + leidos, 0, n - leidos);
		return true;
#else
		size_t hecho = 0;
		while (hecho < n) {
			ssize_t r = pread(fd, dst + hecho, n - hecho, (off_t)(off + hecho));
			if (r < 0) return false;
			if (r == 0) break;
			hecho += (size_t)r;
		}
		if (hecho < n) std::memset(dst + hecho, 0, n - hecho);
		return true;
#endif
	}
};

Fuente::Fuente() : impl_(new Impl) {}
Fuente::~Fuente() = default;

bool esRutaUnidad(const std::string& r) {
#ifdef _WIN32
	if (r.size() >= 2 && std::isalpha((unsigned char)r[0]) && r[1] == ':' &&
	    (r.size() == 2 || (r.size() == 3 && (r[2] == '\\' || r[2] == '/'))))
		return true;
	return r.rfind("\\\\.\\", 0) == 0 || r.rfind("//./", 0) == 0;
#else
	return r.rfind("/dev/", 0) == 0;
#endif
}

#ifdef _WIN32
static std::string rutaDispositivo(const std::string& r) {
	if (r.size() >= 2 && r[1] == ':' && r.size() <= 3)
		return std::string("\\\\.\\") + (char)std::toupper((unsigned char)r[0]) + ":";
	std::string s = r;
	std::replace(s.begin(), s.end(), '/', '\\');
	return s;
}
#endif

bool Fuente::abrir(const std::string& ruta, std::string& err) {
	unidad_ = esRutaUnidad(ruta);
	if (!impl_->buf) {
		err = "No hay memoria para leer el origen.";
		return false;
	}
#ifdef _WIN32
	const std::string r = unidad_ ? rutaDispositivo(ruta) : ruta;
	const std::wstring w = unidad_ ? std::wstring(r.begin(), r.end()) : upath::toPath(r).wstring();
	impl_->h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
	                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (impl_->h == INVALID_HANDLE_VALUE) {
		const DWORD e = GetLastError();
		if (e == ERROR_ACCESS_DENIED)
			err = "Windows no deja leer la unidad directamente: abre untrunc como administrador "
			      "(clic derecho, «Ejecutar como administrador»). Solo se lee, nunca se escribe.";
		else if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
			err = "No existe: " + ruta + ". Pon la letra de la tarjeta (por ejemplo E:) o una "
			      "imagen de disco.";
		else
			err = "No se pudo abrir " + ruta + " (error " + std::to_string(e) + ").";
		return false;
	}
	if (unidad_) {
		DWORD x = 0;
		DeviceIoControl(impl_->h, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0, &x, nullptr);
		GET_LENGTH_INFORMATION gl;
		if (!DeviceIoControl(impl_->h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &gl, sizeof gl, &x,
		                     nullptr)) {
			err = "No se pudo saber el tamaño de la unidad " + ruta + ".";
			return false;
		}
		tam_ = (uint64_t)gl.Length.QuadPart;
	} else {
		LARGE_INTEGER li;
		if (!GetFileSizeEx(impl_->h, &li)) {
			err = "No se pudo saber el tamaño de " + ruta + ".";
			return false;
		}
		tam_ = (uint64_t)li.QuadPart;
	}
#else
	impl_->fd = ::open(ruta.c_str(), O_RDONLY | O_CLOEXEC);
	if (impl_->fd < 0) {
		err = errno == EACCES ? "Sin permiso para leer " + ruta +
		                            ": ejecútalo con sudo (solo se lee, nunca se escribe)."
		                      : "No se pudo abrir " + ruta + ".";
		return false;
	}
	struct stat st;
	if (fstat(impl_->fd, &st) != 0) {
		err = "No se pudo leer " + ruta + ".";
		return false;
	}
	if (S_ISBLK(st.st_mode)) {
		unidad_ = true;
		off_t fin = lseek(impl_->fd, 0, SEEK_END);
		tam_ = fin > 0 ? (uint64_t)fin : 0;
	} else {
		tam_ = (uint64_t)st.st_size;
	}
#endif
	if (tam_ == 0) {
		err = "El origen está vacío: " + ruta + ".";
		return false;
	}
	return true;
}

bool Fuente::cargar(uint64_t off) {
	if (off == bloque_off_) return true;
	if (off >= tam_) return false;
	size_t n = (size_t)std::min<uint64_t>(kBloque, tam_ - off);
	// En unidades, la lectura tiene que ser de sectores enteros.
	const size_t n_alineado = (n + kSector - 1) / kSector * kSector;
	if (!impl_->leerCrudo(off, impl_->buf, n_alineado)) {
		// Sector ilegible: se lee de sector en sector y lo que falla
		// queda a cero (como hacen los programas de clonado).
		for (size_t s = 0; s < n_alineado; s += kSector)
			if (!impl_->leerCrudo(off + s, impl_->buf + s, kSector)) {
				std::memset(impl_->buf + s, 0, kSector);
				ilegibles_++;
			}
	}
	bloque_off_ = off;
	bloque_len_ = n;
	return true;
}

size_t Fuente::leer(uint64_t off, void* dst, size_t n) {
	unsigned char* out = static_cast<unsigned char*>(dst);
	size_t hecho = 0;
	while (hecho < n && off + hecho < tam_) {
		const uint64_t pos = off + hecho;
		const uint64_t base = pos / kBloque * kBloque;
		if (!cargar(base)) break;
		const size_t dentro = (size_t)(pos - base);
		if (dentro >= bloque_len_) break;
		const size_t k = std::min(n - hecho, bloque_len_ - dentro);
		std::memcpy(out + hecho, impl_->buf + dentro, k);
		hecho += k;
	}
	return hecho;
}

uint64_t VideoEncontrado::bytes() const {
	uint64_t b = 0;
	for (const auto& t : tramos) b += t.fin - t.ini;
	return b;
}

// ------------------------------------------------------------ Busqueda

namespace {

// Sigue las cajas desde un 'ftyp'.
struct Cadena {
	VideoEncontrado v;
	uint64_t mdat_ini = 0, mdat_fin = 0;  // datos del mdat (mdat_fin 0 = desconocido)
	uint64_t fin_cadena = 0;
};

Cadena seguir(Fuente& f, uint64_t inicio) {
	Cadena c;
	c.v.inicio = inicio;
	uint64_t pos = inicio;
	while (pos + 8 <= f.tam()) {
		unsigned char h[16] = {0};
		if (f.leer(pos, h, 16) < 8) break;
		uint64_t tam = be32(h);
		const std::string t(reinterpret_cast<char*>(h + 4), 4);
		if (!cajaConocida(t)) break;
		if (t == "ftyp" && pos != inicio) break;  // otro video
		uint64_t cab = 8;
		if (tam == 1) {
			tam = be64(h + 8);
			cab = 16;
		} else if (tam == 0) {
			if (t == "mdat") {
				c.v.tiene_mdat = true;
				c.v.mdat_abierto = true;
				c.mdat_ini = pos + cab;
			}
			c.fin_cadena = pos + cab;
			break;
		}
		if (tam < cab) break;
		if (t == "ftyp") c.v.marca.assign(reinterpret_cast<char*>(h + 8), 4);
		const uint64_t fin = pos + tam;
		if (t == "mdat") {
			c.v.tiene_mdat = true;
			c.mdat_ini = pos + cab;
			c.mdat_fin = std::min(fin, f.tam());
			if (fin > f.tam()) {
				c.v.mdat_abierto = true;
				c.fin_cadena = f.tam();
				break;
			}
		} else if (t == "moov") {
			// La primera caja de dentro tiene que parecerlo.
			unsigned char d[8];
			if (fin > f.tam() || f.leer(pos + 8, d, 8) < 8 || !imprimible4(d + 4)) break;
			c.v.tiene_moov = true;
		} else if (fin > f.tam()) {
			break;
		}
		pos = fin;
		c.fin_cadena = fin;
	}
	c.v.completo = c.v.tiene_moov && c.v.tiene_mdat && !c.v.mdat_abierto;
	return c;
}

bool bloqueVacio(Fuente& f, uint64_t off, std::vector<unsigned char>& buf) {
	const size_t n = f.leer(off, buf.data(), buf.size());
	if (n == 0) return true;
	const unsigned char c = buf[0];
	if (c != 0x00 && c != 0xFF) return false;
	for (size_t i = 1; i < n; i++)
		if (buf[i] != c) return false;
	return true;
}

}  // namespace

std::vector<VideoEncontrado> buscarVideos(Fuente& f, const std::function<void(int)>& progreso) {
	// 1) Principios de video: 'ftyp' al principio de un sector.
	std::vector<uint64_t> inicios;
	std::vector<unsigned char> buf(kBloque);
	int ultimo = -1;
	for (uint64_t off = 0; off < f.tam(); off += kBloque) {
		const size_t n = f.leer(off, buf.data(), kBloque);
		for (size_t p = 0; p + 16 <= n; p += kSector) {
			const unsigned char* h = buf.data() + p;
			if (std::memcmp(h + 4, "ftyp", 4) != 0) continue;
			const uint32_t tam = be32(h);
			if (tam < 16 || tam > 1024 || tam % 4 != 0 || !imprimible4(h + 8)) continue;
			inicios.push_back(off + p);
		}
		if (progreso) {
			const int pct = (int)(100.0 * (double)(off + n) / (double)f.tam());
			if (pct != ultimo) progreso(ultimo = pct);
		}
	}

	// 2) Estructura de cada uno; los completos marcan zonas ocupadas.
	std::vector<Cadena> cadenas;
	std::vector<Tramo> ocupado;
	for (uint64_t ini : inicios) {
		Cadena c = seguir(f, ini);
		// Principio de video dentro de otro que parecia completo: si es un
		// video de verdad, el primero estaba partido alrededor de este (con
		// el moov al principio la cadena de cajas no lo nota). Si no, era
		// un 'ftyp' casual dentro de los datos.
		for (auto& otra : cadenas)
			if (otra.v.completo && ini > otra.v.inicio && ini < otra.fin_cadena) {
				if (!(c.v.tiene_mdat && (c.v.tiene_moov || c.v.mdat_abierto))) {
					c.v.tiene_mdat = false;  // se descarta
					break;
				}
				otra.v.completo = false;
			}
		if (!c.v.tiene_mdat && !c.v.tiene_moov) continue;
		cadenas.push_back(c);
	}
	for (auto& c : cadenas)
		if (c.v.completo) {
			c.v.fin = c.fin_cadena;
			c.v.tramos = {{c.v.inicio, c.fin_cadena}};
			ocupado.push_back({c.v.inicio, c.fin_cadena});
		}
	std::sort(ocupado.begin(), ocupado.end(),
	          [](const Tramo& a, const Tramo& b) { return a.ini < b.ini; });
	// Tamano de cluster: cada archivo empieza al principio de un cluster,
	// asi que es la mayor potencia de 2 que divide a todos los
	// principios (entre 512 B y 64 KB; con un solo video, 512). Tras
	// saltar un video que esta en medio, la continuacion empieza en el
	// siguiente cluster, no justo donde acaba.
	uint64_t cluster = kSector;
	if (cadenas.size() >= 2) {
		uint64_t g = 0;
		for (const auto& c : cadenas) {
			uint64_t a = g, b = c.v.inicio;
			while (b) {
				const uint64_t t = a % b;
				a = b;
				b = t;
			}
			g = a;
		}
		while (cluster < 65536 && g % (cluster * 2) == 0) cluster *= 2;
	}

	// 3) Incompletos: desde su ftyp hasta el final de sus datos, saltando
	// los videos completos que haya en medio (tarjeta fragmentada) y
	// parando en el siguiente video incompleto o en una zona vacia.
	std::vector<uint64_t> otros;  // principios de incompletos
	for (const auto& c : cadenas)
		if (!c.v.completo) otros.push_back(c.v.inicio);
	std::vector<unsigned char> vacio(kPasoVacio);
	std::vector<VideoEncontrado> res;
	for (auto& c : cadenas) {
		if (c.v.completo) {
			res.push_back(c.v);
			continue;
		}
		if (!c.v.tiene_mdat) continue;  // ni datos: no hay nada que rescatar
		uint64_t tope = std::min(f.tam(), c.v.inicio + kMaxIncompleto);
		for (uint64_t o : otros)
			if (o > c.v.inicio && o < tope) tope = o;
		// Con el tamano del mdat conocido se juntan exactamente esos bytes
		// (contando desde el ftyp), aunque esten repartidos alrededor de
		// otros videos.
		const bool tam_conocido = c.mdat_fin > c.mdat_ini && !c.v.mdat_abierto;
		const uint64_t falta_total = tam_conocido ? c.mdat_fin - c.v.inicio : 0;
		uint64_t juntado = 0;
		std::vector<Tramo> tramos;
		uint64_t pos = c.v.inicio;
		uint64_t datos_desde = c.mdat_ini;
		while (pos < tope && (!tam_conocido || juntado < falta_total)) {
			// Siguiente video completo dentro del tramo.
			uint64_t hasta = tope;
			if (tam_conocido) hasta = std::min(hasta, pos + (falta_total - juntado));
			const Tramo* salto = nullptr;
			for (const auto& t : ocupado)
				if (t.fin > pos && t.ini < hasta) {
					if (t.ini <= pos) {
						salto = &t;
						hasta = pos;
						break;
					}
					hasta = t.ini;
					salto = &t;
					break;
				}
			if (!tam_conocido) {
				// Final de los datos: primera zona vacia (0x00 o 0xFF).
				uint64_t p = std::max(pos, datos_desde);
				p = (p + kPasoVacio - 1) / kPasoVacio * kPasoVacio;
				for (; p < hasta; p += kPasoVacio)
					if (bloqueVacio(f, p, vacio)) {
						// Se afina hasta el ultimo sector con datos.
						uint64_t q = p;
						std::vector<unsigned char> sec(kSector);
						while (q > std::max(pos, datos_desde) && q >= kSector) {
							const size_t n = f.leer(q - kSector, sec.data(), kSector);
							bool vacio_sec = n > 0;
							for (size_t z = 0; z < n && vacio_sec; z++)
								vacio_sec = sec[z] == sec[0] && (sec[0] == 0x00 || sec[0] == 0xFF);
							if (!vacio_sec) break;
							q -= kSector;
						}
						hasta = std::max(pos, q);
						tope = hasta;  // no se sigue despues
						salto = nullptr;
						break;
					}
			}
			if (hasta > pos) {
				tramos.push_back({pos, hasta});
				juntado += hasta - pos;
			}
			if (!salto) break;
			pos = std::max(hasta, (salto->fin + cluster - 1) / cluster * cluster);
			datos_desde = pos;
		}
		c.v.tramos = tramos;
		c.v.fin = tramos.empty() ? c.v.inicio : tramos.back().fin;
		if (c.v.bytes() > c.mdat_ini - c.v.inicio) res.push_back(c.v);
	}
	std::sort(res.begin(), res.end(),
	          [](const VideoEncontrado& a, const VideoEncontrado& b) { return a.inicio < b.inicio; });
	return res;
}

// ------------------------------------------------------------ Destino

bool destinoSeguro(const std::string& origen, const std::string& destino, std::string& err) {
	std::error_code ec;
	const fs::path dst = fs::absolute(upath::toPath(destino), ec);
	if (!esRutaUnidad(origen)) {
		const fs::path img = fs::absolute(upath::toPath(origen), ec);
		if (fs::equivalent(img, dst, ec)) {
			err = "El destino no puede ser la propia imagen.";
			return false;
		}
		return true;
	}
#ifdef _WIN32
	const std::string o = rutaDispositivo(origen);  // \\.\E:
	if (o.size() == 6 && o[5] == ':') {
		const std::string d = upath::fromPath(dst);
		if (!d.empty() && std::toupper((unsigned char)d[0]) == o[4] && d.size() > 1 && d[1] == ':') {
			err = std::string("El destino está en la misma unidad que se rescata (") + o[4] +
			      ":). Guarda lo rescatado en otro disco: escribir en la tarjeta puede borrar "
			      "justo los vídeos que se buscan.";
			return false;
		}
	}
	return true;
#else
	std::ifstream m("/proc/mounts");
	std::string dev, punto, resto;
	const std::string d = dst.string();
	while (m >> dev >> punto && std::getline(m, resto)) {
		if (dev.rfind(origen, 0) != 0) continue;
		if (d == punto || d.rfind(punto + "/", 0) == 0) {
			err = "El destino está en la unidad que se rescata (" + punto +
			      "). Guarda lo rescatado en otro disco.";
			return false;
		}
	}
	return true;
#endif
}

// ------------------------------------------------------------ Rescate

namespace {

bool copiarTramos(Fuente& f, const std::vector<Tramo>& tramos, const std::string& salida,
                  std::string& err) {
	std::ofstream out(upath::toPath(salida), std::ios::binary | std::ios::trunc);
	if (!out) {
		err = "No se pudo crear " + salida;
		return false;
	}
	std::vector<unsigned char> buf(kBloque);
	for (const auto& t : tramos)
		for (uint64_t p = t.ini; p < t.fin;) {
			const size_t n = f.leer(p, buf.data(), (size_t)std::min<uint64_t>(kBloque, t.fin - p));
			if (n == 0) break;
			out.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)n);
			if (!out) {
				err = "No se pudo escribir " + salida + " (¿disco lleno?)";
				return false;
			}
			p += n;
		}
	return true;
}

// Milisegundos de video que se ven bien (si se comprobo), si no la
// duracion recuperada.
double puntos(const RepairOutcome& x) {
	if (!x.ok) return -1;
	if (!x.verificacion.hecho) return (double)x.duration_ms;
	const double malo = std::min(100.0, std::max(0.0, x.verificacion.porcentajeConErrores()));
	return (double)x.duration_ms * (100.0 - malo) / 100.0;
}

std::string nombreLibre(const fs::path& dir, const std::string& base, const std::string& ext) {
	std::error_code ec;
	for (int i = 0;; i++) {
		const std::string n = base + (i ? "_" + std::to_string(i) : "") + ext;
		if (!fs::exists(dir / upath::toPath(n), ec)) return upath::fromPath(dir / upath::toPath(n));
	}
}

}  // namespace

ResultadoRescate rescatar(const std::string& origen, const OpcionesRescate& o) {
	ResultadoRescate r;
	auto avisar = [&o](int pct, const std::string& txt) {
		if (o.progreso) o.progreso(pct, txt);
	};

	Fuente f;
	if (!f.abrir(origen, r.error_es)) return r;
	r.bytes_origen = f.tam();

	std::string destino = o.destino;
	if (destino.empty() && !o.solo_buscar) {
		if (f.esUnidad()) {
			r.error_es = "Indica dónde guardar lo rescatado con -dst <carpeta> (en otro disco, "
			             "nunca en la tarjeta).";
			return r;
		}
		destino = upath::fromPath(upath::toPath(origen).parent_path() / "Rescatados");
	}
	if (!o.solo_buscar) {
		if (!destinoSeguro(origen, destino, r.error_es)) return r;
		std::error_code ec;
		fs::create_directories(upath::toPath(destino), ec);
		if (ec) {
			r.error_es = "No se pudo crear la carpeta de destino " + destino + ".";
			return r;
		}
	}

	avisar(0, "Buscando vídeos en " + mb(f.tam()) + "...");
	r.encontrados = buscarVideos(f, [&](int pct) { avisar(pct / 2, ""); });
	r.ok = true;

	if (!o.solo_buscar) {
		const fs::path dir = upath::toPath(destino);
		r.rescatados.resize(r.encontrados.size());
		auto nombreBase = [](size_t i) {
			char base[32];
			std::snprintf(base, sizeof base, "rescatado_%03zu", i + 1);
			return std::string(base);
		};
		// Pasada 1: los completos se copian tal cual. Los que se ven bien
		// sirven despues de referencia para los incompletos de la misma
		// camara (lo normal: toda la tarjeta es de una camara).
		struct RefTarjeta {
			std::string ruta, codec;
			int w = 0, h = 0;
		};
		std::vector<RefTarjeta> refs_tarjeta;
		size_t hechos = 0;
		for (size_t i = 0; i < r.encontrados.size(); i++) {
			const VideoEncontrado& v = r.encontrados[i];
			if (!v.completo) continue;
			VideoRescatado& vr = r.rescatados[i];
			vr.v = v;
			avisar(50 + (int)(50 * hechos++ / r.encontrados.size()),
			       "Vídeo " + std::to_string(i + 1) + " de " + std::to_string(r.encontrados.size()) +
			           " (completo)");
			std::string err;
			vr.salida = nombreLibre(dir, nombreBase(i), conMarca(v.marca));
			if (!copiarTramos(f, v.tramos, vr.salida, err)) {
				vr.estado_es = err;
				continue;
			}
			vr.ok = true;
			vr.estado_es = "completo (" + mb(v.bytes()) + ")";
			bool sirve = true;
			if (o.verificar) {
				const InformeVerificacion inf = verificarVideo(vr.salida);
				vr.duracion_ms = (int64_t)(inf.duracion_s * 1000);
				sirve = inf.hecho && inf.limpio();
				if (!inf.hecho)
					vr.estado_es += ", no se pudo comprobar";
				else if (inf.limpio())
					vr.estado_es += ", " + formatDurationEs(vr.duracion_ms) + ", se ve bien";
				else
					vr.estado_es += ", " + formatDurationEs(vr.duracion_ms) + ", con errores en el " +
					                std::to_string((int)inf.porcentajeConErrores()) +
					                " % (la tarjeta lo guardó en trozos; prueba a repararlo con una "
					                "referencia)";
			}
			if (sirve) {
				const plantillas::InfoPlantilla info = plantillas::leerPlantilla(vr.salida);
				if (info.ok) refs_tarjeta.push_back({vr.salida, info.video_codec, info.width, info.height});
			}
		}

		// Pasada 2: incompletos o en trozos: se juntan sus datos y se
		// reparan. Referencia, por orden: la dada; un video completo de la
		// tarjeta con la misma resolucion y codec; la plantilla elegida o
		// las guardadas que coincidan (las de otra resolucion se descartan
		// si hay alguna que coincide: con otros fps "recuperan" mas segundos
		// falsos y ganarian).
		const std::vector<plantillas::InfoPlantilla> todas =
		    o.referencia.empty() && o.plantilla.empty() ? plantillas::listarPlantillas(o.argv0)
		                                                : std::vector<plantillas::InfoPlantilla>{};
		for (size_t i = 0; i < r.encontrados.size(); i++) {
			const VideoEncontrado& v = r.encontrados[i];
			if (v.completo) continue;
			VideoRescatado& vr = r.rescatados[i];
			vr.v = v;
			avisar(50 + (int)(50 * hechos++ / r.encontrados.size()),
			       "Vídeo " + std::to_string(i + 1) + " de " + std::to_string(r.encontrados.size()) +
			           " (reparando)");
			const std::string ext = conMarca(v.marca);
			std::string err;
			const std::string crudo = nombreLibre(dir, nombreBase(i) + "_sin_reparar", ext);
			if (!copiarTramos(f, v.tramos, crudo, err)) {
				vr.estado_es = err;
				continue;
			}
			const std::string que = v.tramos.size() > 1
			                            ? "estaba en " + std::to_string(v.tramos.size()) + " trozos (juntados)"
			                            : v.mdat_abierto ? "grabación cortada" : "estaba incompleto";
			if (v.tiene_moov && v.tramos.size() > 1 && o.verificar) {
				const InformeVerificacion inf = verificarVideo(crudo);
				if (inf.hecho && inf.limpio() && inf.duracion_s > 0) {
					std::error_code ec;
					vr.salida = nombreLibre(dir, nombreBase(i), ext);
					fs::rename(upath::toPath(crudo), upath::toPath(vr.salida), ec);
					if (ec) vr.salida = crudo;
					vr.ok = true;
					vr.duracion_ms = (int64_t)(inf.duracion_s * 1000);
					vr.estado_es = que + " con su índice intacto: " + formatDurationEs(vr.duracion_ms) +
					               ", se ve bien (sin reparar)";
					continue;
				}
			}
			const SpsDetected sps = detectSpsFromFile(crudo);
			const bool con_tam = sps.found && sps.valid && sps.width > 0 && sps.height > 0;
			auto mismoCodec = [&](const std::string& c) {
				if (!sps.found || c.empty()) return true;
				auto n = [](std::string x) { return x == "hev1" ? std::string("hvc1") : x == "avc3" ? std::string("avc1") : x; };
				return n(c) == n(sps.codec);
			};
			auto mismoTam = [&](int w, int h) {
				return con_tam && ((w == sps.width && h == sps.height) || (w == sps.height && h == sps.width));
			};

			// Candidatas a referencia, por orden: la dada (solo esa); videos
			// completos de la tarjeta del mismo codec (y resolucion, si el
			// SPS va dentro de los datos); plantillas. Se prueban varias y
			// se queda la que da mas segundos SIN errores al decodificar:
			// una referencia de otra camara o con otros fps "recupera" mas
			// segundos, pero rotos.
			struct Candidata {
				std::string ref;                 // video de referencia
				plantillas::InfoPlantilla plantilla;  // o plantilla
				std::string desc;
			};
			std::vector<Candidata> cands;
			if (!o.referencia.empty()) {
				cands.push_back({o.referencia, {}, "con tu referencia"});
			} else {
				for (const auto& rt : refs_tarjeta)
					if (mismoCodec(rt.codec) && (!con_tam || mismoTam(rt.w, rt.h)))
						cands.push_back({rt.ruta, {},
						                 "con " + upath::fromPath(upath::toPath(rt.ruta).filename()) +
						                     " (de la tarjeta)"});
				std::vector<plantillas::InfoPlantilla> pl;
				if (!o.plantilla.empty()) {
					const std::string pp = plantillas::resolverPlantilla(o.plantilla, o.argv0);
					if (!pp.empty()) pl.push_back(plantillas::leerPlantilla(pp));
				} else {
					pl = plantillas::ordenarCandidatas(todas, "", con_tam ? sps.width : 0,
					                                   con_tam ? sps.height : 0, sps.found ? sps.codec : "");
					std::vector<plantillas::InfoPlantilla> iguales;
					for (const auto& c : pl)
						if (mismoTam(c.width, c.height)) iguales.push_back(c);
					if (!iguales.empty()) pl = iguales;
				}
				for (const auto& c : pl)
					if (c.ok) cands.push_back({"", c, "con la plantilla " + c.id});
			}
			if (cands.size() > kMaxCandidatas) cands.resize(kMaxCandidatas);

			RepairOptions ro = o.reparar;
			ro.verificar = o.verificar;
			RepairOutcome rep;
			bool intentado = false;
			std::string con_que;
			double mejor_puntos = -1;
			std::vector<std::string> probadas;
			const fs::path prueba = dir / ".probando";
			for (size_t k = 0; k < cands.size(); k++) {
				std::error_code ec;
				fs::remove_all(prueba, ec);
				fs::create_directories(prueba, ec);
				ro.dst_path = upath::fromPath(prueba);
				RepairOutcome x;
				if (!cands[k].ref.empty()) {
					x = repairFileWithAutoRetry(cands[k].ref, crudo, ro, true);
				} else {
					plantillas::ResultadoPlantillas rp =
					    plantillas::repararConPlantillas({cands[k].plantilla}, crudo, ro, 1);
					if (!rp.attempted) continue;
					x = rp.repair;
				}
				intentado = true;
				const double pts = puntos(x);
				probadas.push_back(cands[k].desc.substr(cands[k].desc.rfind("con ", 0) == 0 ? 4 : 0) +
				                   ": " +
				                   (x.ok ? std::to_string((int)(pts / 1000)) + " s bien"
				                         : std::string("nada útil")));
				if (x.ok && pts > mejor_puntos) {
					// Se aparta como la mejor hasta ahora.
					const fs::path dest = dir / upath::toPath(".mejor" + upath::fromPath(upath::toPath(x.output_path).extension()));
					fs::remove(dest, ec);
					fs::rename(upath::toPath(x.output_path), dest, ec);
					if (ec) continue;
					x.output_path = upath::fromPath(dest);
					rep = x;
					con_que = cands[k].desc;
					mejor_puntos = pts;
					// Limpio y casi entero: no hace falta probar mas.
					if (x.verificacion.hecho && x.verificacion.limpio() && x.recovered_percent >= 90) break;
				} else if (!rep.ok && !x.ok) {
					rep = x;  // para el mensaje de error
				}
			}
			{
				std::error_code ec;
				fs::remove_all(prueba, ec);
			}
			if (probadas.size() > 1) {
				con_que += " (la mejor de " + std::to_string(probadas.size()) + ":";
				for (const auto& l : probadas) con_que += " " + l + ";";
				con_que.back() = ')';
			}
			if (intentado && rep.ok && !rep.output_path.empty()) {
				std::error_code ec;
				const fs::path salida_rep = upath::toPath(rep.output_path);
				vr.salida = nombreLibre(dir, nombreBase(i) + "_reparado",
				                        upath::fromPath(salida_rep.extension()));
				fs::rename(salida_rep, upath::toPath(vr.salida), ec);
				if (ec) vr.salida = rep.output_path;
				if (!o.conservar_crudos) fs::remove(upath::toPath(crudo), ec);
				vr.ok = true;
				vr.duracion_ms = rep.duration_ms;
				vr.estado_es = que + "; reparado " + con_que + ": " + formatDurationEs(rep.duration_ms) +
				               " (" + std::to_string((int)rep.recovered_percent) + " %)";
				if (rep.verificacion.hecho) {
					const int pct = (int)rep.verificacion.porcentajeConErrores();
					vr.estado_es += rep.verificacion.limpio() ? ", se ve bien"
					                : pct < 1                 ? ", algún error suelto"
					                                          : ", con errores en el " + std::to_string(pct) + " %";
				}
			} else {
				vr.salida = crudo;
				vr.estado_es =
				    que + "; guardado sin reparar (" + mb(v.bytes()) + "). " +
				    (intentado ? "No se pudo reparar: " +
				                     (rep.error_es.empty() ? std::string("nada útil")
				                                           : rep.error_es.substr(0, rep.error_es.find('\n')))
				               : std::string("Para repararlo indica un vídeo sano de la misma cámara "
				                             "(o crea una plantilla) y vuelve a rescatar."));
			}
		}
	}
	r.sectores_ilegibles = f.sectoresIlegibles();
	avisar(100, "");

	std::ostringstream s;
	s << "Rescate de " << origen << " (" << mb(r.bytes_origen) << ", solo lectura):\n";
	int completos = 0;
	for (const auto& v : r.encontrados) completos += v.completo;
	s << "  Vídeos encontrados: " << r.encontrados.size() << " (" << completos << " completos, "
	  << r.encontrados.size() - completos << " incompletos o en trozos).\n";
	if (o.solo_buscar) {
		for (size_t i = 0; i < r.encontrados.size(); i++) {
			const auto& v = r.encontrados[i];
			s << "  " << i + 1 << ". en " << mb(v.inicio) << ": " << mb(v.bytes()) << ", "
			  << (v.completo ? "completo"
			      : v.tramos.size() > 1 ? "en trozos"
			      : v.mdat_abierto ? "grabación cortada (sin índice)"
			                       : "incompleto");
			if (v.tramos.size() > 1) s << ", " << v.tramos.size() << " trozos";
			s << "\n";
		}
		s << "  No se ha escrito nada (solo búsqueda).\n";
	} else {
		int ok = 0;
		for (const auto& v : r.rescatados) ok += v.ok;
		for (size_t i = 0; i < r.rescatados.size(); i++)
			s << "  " << i + 1 << ". " << upath::fromPath(upath::toPath(r.rescatados[i].salida).filename())
			  << ": " << r.rescatados[i].estado_es << "\n";
		s << "  Rescatados: " << ok << " de " << r.rescatados.size() << ", en " << destino << "\n";
	}
	if (r.sectores_ilegibles > 0)
		s << "  Aviso: " << r.sectores_ilegibles
		  << " sectores no se pudieron leer (se tomaron como ceros). La tarjeta puede estar "
		     "fallando: haz una imagen de disco y rescata desde la imagen.\n";
	if (r.encontrados.empty())
		s << "  No apareció ningún principio de vídeo. Si la tarjeta se formateó o se grabó encima, "
		     "las cabeceras pueden haberse perdido.\n";
	s << "  La tarjeta no se ha modificado.";
	r.resumen_es = s.str();
	return r;
}

}  // namespace tarjeta
