/*
	Untrunc-mejorado - plantillas.cpp (ver plantillas.h)

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "plantillas.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "common.h"
#include "diagnoser.h"
#include "file.h"
#include "mp4.h"
#include "tempfs.h"
#include "upath.h"

namespace fs = std::filesystem;

namespace plantillas {

const char* const kExtension = ".plantilla";

namespace {

// Atomo marcador al principio de cada plantilla: distingue una
// plantilla (solo ftyp y moov llevan contenido) de un MP4 normal.
const char kMarcaTipo[4] = {'U', 'T', 'P', 'L'};
const char kMarcaVersion[8] = {'p', 'l', 'a', 'n', 't', 'i', 'l', '1'};
const uint64_t kMarcaBytes = 16;

// Ver kMaxMdatBytes en el .h.
const uint64_t kLimiteMdat = kMaxMdatBytes;
// Un moov de camara pesa KB o pocos MB; 256 MB es claramente roto.
const uint64_t kLimiteMoov = 256ull * 1024 * 1024;

uint32_t be32(const uint8_t* p) {
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
void putBe32(uint8_t* p, uint32_t v) {
	p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

std::string fourcc(const uint8_t* p) { return std::string(reinterpret_cast<const char*>(p), 4); }

// Fourcc legible (los de QuickTime pueden llevar bytes no ASCII).
std::string fourccLegible(const std::string& t) {
	std::string s;
	for (unsigned char c : t) s += (c >= 32 && c < 127) ? char(c) : '?';
	return s;
}

struct Caja {
	size_t start = 0;     // inicio de la cabecera
	size_t payload = 0;   // inicio del contenido
	size_t end = 0;       // fin (exclusivo)
	std::string type;
};

// Recorre las cajas hijas en [start, end) de un buffer en memoria.
std::vector<Caja> hijas(const std::vector<uint8_t>& b, size_t start, size_t end) {
	std::vector<Caja> out;
	size_t pos = start;
	while (pos + 8 <= end) {
		uint64_t size = be32(&b[pos]);
		size_t hdr = 8;
		if (size == 1) {
			if (pos + 16 > end) break;
			size = be64(&b[pos + 8]);
			hdr = 16;
		} else if (size == 0) {
			size = end - pos;
		}
		if (size < hdr || size > end - pos) break;
		Caja c;
		c.start = pos;
		c.payload = pos + hdr;
		c.end = pos + size_t(size);
		c.type = fourcc(&b[pos + 4]);
		out.push_back(c);
		pos = c.end;
	}
	return out;
}

bool buscar(const std::vector<uint8_t>& b, size_t start, size_t end, const char* tipo, Caja& out) {
	for (const Caja& c : hijas(b, start, end))
		if (c.type == tipo) { out = c; return true; }
	return false;
}

bool esContenedor(const std::string& t) {
	static const std::set<std::string> k = {"moov", "trak", "mdia", "minf", "stbl",
	                                        "edts", "dinf", "mvex"};
	return k.count(t) > 0;
}

// Cajas que pueden llevar datos personales (fecha, GPS, numero de
// serie, nombre del dueno, XMP...). El nucleo no las necesita.
bool esPersonal(const std::string& t) {
	if (t == "udta" || t == "meta" || t == "uuid" || t == "XMP_") return true;
	return !t.empty() && (unsigned char)t[0] == 0xA9;  // "(c)nam", "(c)xyz"...
}

// Vacia (a 'free' con ceros, mismo tamano) las cajas personales y pone
// a cero las fechas de mvhd/tkhd/mdhd. Los tamanos no cambian, asi que
// los desplazamientos del stco/co64 siguen siendo validos.
void limpiar(std::vector<uint8_t>& b, size_t start, size_t end, int& n) {
	for (const Caja& c : hijas(b, start, end)) {
		if (esPersonal(c.type)) {
			std::memcpy(&b[c.start + 4], "free", 4);
			std::fill(b.begin() + c.payload, b.begin() + c.end, 0);
			n++;
		} else if (c.type == "mvhd" || c.type == "tkhd" || c.type == "mdhd") {
			size_t len = c.end - c.payload;
			if (len >= 1) {
				size_t fechas = (b[c.payload] == 1) ? 16 : 8;  // v1: 2x64 bits
				if (len >= 4 + fechas)
					std::fill(b.begin() + c.payload + 4, b.begin() + c.payload + 4 + fechas, 0);
			}
		} else if (esContenedor(c.type)) {
			limpiar(b, c.payload, c.end, n);
		}
	}
}

// Rellena codecs/resolucion a partir del moov en memoria.
void infoDeMoov(const std::vector<uint8_t>& b, InfoPlantilla& info) {
	Caja moov;
	if (!buscar(b, 0, b.size(), "moov", moov)) return;
	for (const Caja& trak : hijas(b, moov.payload, moov.end)) {
		if (trak.type != "trak") continue;
		Caja mdia, hdlr, minf, stbl, stsd;
		if (!buscar(b, trak.payload, trak.end, "mdia", mdia)) continue;
		std::string handler;
		if (buscar(b, mdia.payload, mdia.end, "hdlr", hdlr) && hdlr.end - hdlr.payload >= 12)
			handler = fourcc(&b[hdlr.payload + 8]);
		if (!buscar(b, mdia.payload, mdia.end, "minf", minf)) continue;
		if (!buscar(b, minf.payload, minf.end, "stbl", stbl)) continue;
		if (!buscar(b, stbl.payload, stbl.end, "stsd", stsd)) continue;
		if (stsd.end - stsd.payload < 16) continue;
		const size_t entry = stsd.payload + 8;
		std::string codec = fourccLegible(fourcc(&b[entry + 4]));
		info.codecs.push_back(codec);
		if (handler == "vide" && info.video_codec.empty()) {
			info.video_codec = codec;
			if (entry + 36 <= stsd.end) {
				info.width = be16(&b[entry + 32]);
				info.height = be16(&b[entry + 34]);
			}
		}
	}
}

struct Cabecera {
	uint64_t size = 0;    // tamano total (0 = hasta el final)
	uint32_t hdr = 8;
	std::string type;
	uint8_t raw[16] = {};
};

bool leerCabecera(std::istream& f, Cabecera& c) {
	if (!f.read(reinterpret_cast<char*>(c.raw), 8)) return false;
	c.size = be32(c.raw);
	c.type = fourcc(c.raw + 4);
	c.hdr = 8;
	if (c.size == 1) {
		if (!f.read(reinterpret_cast<char*>(c.raw + 8), 8)) return false;
		c.size = be64(c.raw + 8);
		c.hdr = 16;
	}
	return true;
}

bool esPlantillaStream(std::istream& f) {
	uint8_t m[8];
	f.seekg(0);
	bool ok = bool(f.read(reinterpret_cast<char*>(m), 8)) && std::memcmp(m + 4, kMarcaTipo, 4) == 0;
	f.clear();
	f.seekg(0);
	return ok;
}

std::string carpetaExe() {
	std::string self = upath::executablePath();
	if (!self.empty()) return upath::fromPath(upath::toPath(self).parent_path());
	return "";
}

std::string normalizarCodec(std::string c) {
	c.erase(std::remove(c.begin(), c.end(), '?'), c.end());
	if (c == "avc3") c = "avc1";
	if (c == "hev1") c = "hvc1";
	return c;
}

bool moverArchivo(const fs::path& de, const fs::path& a, std::string& err) {
	std::error_code ec;
	fs::rename(de, a, ec);
	if (!ec) return true;
	// Otra unidad (temporal en C:, video en D:): copiar y borrar.
	ec.clear();
	fs::copy_file(de, a, fs::copy_options::overwrite_existing, ec);
	if (ec) { err = ec.message(); return false; }
	fs::remove(de, ec);
	return true;
}

void reemplazarTodo(std::string& s, const std::string& de, const std::string& a) {
	if (de.empty()) return;
	for (size_t p = s.find(de); p != std::string::npos; p = s.find(de, p + a.size()))
		s.replace(p, de.size(), a);
}

} // namespace

std::string InfoPlantilla::resumenEs() const {
	std::ostringstream ss;
	ss << (video_codec.empty() ? "sin vídeo" : video_codec);
	if (width > 0 && height > 0) ss << " " << width << "x" << height;
	if (codecs.size() > 1) {
		ss << " (pistas:";
		for (const auto& c : codecs) ss << " " << c;
		ss << ")";
	}
	if (!brand.empty()) ss << ", ftyp '" << brand << "'";
	return ss.str();
}

InfoPlantilla leerPlantilla(const std::string& path) {
	InfoPlantilla info;
	info.path = path;
	info.id = upath::fromPath(upath::toPath(path).stem());
	std::ifstream f(upath::toPath(path), std::ios::binary);
	if (!f) { info.error_es = "no se puede abrir " + path; return info; }
	f.seekg(0, std::ios::end);
	const uint64_t total = uint64_t(f.tellg());
	info.file_bytes = total;
	const bool plantilla = esPlantillaStream(f);
	if (plantilla) f.seekg(kMarcaBytes);

	std::vector<uint8_t> moov;
	uint64_t pos = uint64_t(f.tellg());
	Cabecera c;
	while (pos < total && leerCabecera(f, c)) {
		uint64_t size = c.size ? c.size : total - pos;
		if (size < c.hdr) { info.error_es = "átomo '" + fourccLegible(c.type) + "' con tamaño inválido"; return info; }
		const uint64_t payload = size - c.hdr;
		const bool con_contenido = !plantilla || c.type == "ftyp" || c.type == "moov";
		if (c.type == "ftyp" && payload >= 4) {
			char brand[4];
			f.read(brand, 4);
			info.brand = std::string(brand, 4);
			f.seekg(std::streamoff(pos + c.hdr));
		} else if (c.type == "moov") {
			if (payload > kLimiteMoov) { info.error_es = "moov demasiado grande"; return info; }
			moov.resize(size_t(size));
			std::memcpy(moov.data(), c.raw, c.hdr);
			if (!f.read(reinterpret_cast<char*>(moov.data() + c.hdr), std::streamsize(payload))) {
				info.error_es = "el moov está cortado";
				return info;
			}
		} else if (c.type == "mdat") {
			info.mdat_bytes += payload;
		}
		pos += con_contenido ? size : c.hdr;
		f.seekg(std::streamoff(pos));
	}
	if (moov.empty()) {
		info.error_es = "no tiene átomo moov (no es una plantilla ni un vídeo sano)";
		return info;
	}
	infoDeMoov(moov, info);
	if (info.codecs.empty()) {
		info.error_es = "el moov no tiene pistas reconocibles";
		return info;
	}
	info.ok = true;
	return info;
}

std::string extensionMaterializada(const InfoPlantilla& info) {
	return info.brand == "qt  " ? ".mov" : ".mp4";
}

bool materializarPlantilla(const std::string& plantilla, const std::string& salida_mp4, std::string& err) {
	std::ifstream f(upath::toPath(plantilla), std::ios::binary);
	if (!f) { err = "no se puede abrir la plantilla " + plantilla; return false; }
	if (!esPlantillaStream(f)) { err = plantilla + " no es una plantilla de untrunc"; return false; }
	f.seekg(0, std::ios::end);
	const uint64_t total = uint64_t(f.tellg());
	f.seekg(kMarcaBytes);
	std::ofstream o(upath::toPath(salida_mp4), std::ios::binary | std::ios::trunc);
	if (!o) { err = "no se puede crear " + salida_mp4; return false; }

	uint64_t pos = kMarcaBytes;
	std::vector<char> buf(1 << 20);
	Cabecera c;
	while (pos < total && leerCabecera(f, c)) {
		if (c.size < c.hdr) { err = "plantilla dañada (tamaño inválido)"; return false; }
		const uint64_t payload = c.size - c.hdr;
		o.write(reinterpret_cast<const char*>(c.raw), c.hdr);
		if (c.type == "ftyp" || c.type == "moov") {
			uint64_t left = payload;
			while (left > 0) {
				std::streamsize n = std::streamsize(std::min<uint64_t>(left, buf.size()));
				if (!f.read(buf.data(), n)) { err = "plantilla cortada"; return false; }
				o.write(buf.data(), n);
				left -= uint64_t(n);
			}
			pos += c.size;
		} else {
			// mdat, free...: solo la cabecera en la plantilla; el
			// contenido se rellena con ceros (saltar y escribir el
			// ultimo byte deja el hueco a cero).
			if (payload > 0) {
				o.seekp(std::streamoff(payload - 1), std::ios::cur);
				o.put('\0');
			}
			pos += c.hdr;
		}
		if (!o) { err = "no se pudo escribir " + salida_mp4 + " (¿disco lleno?)"; return false; }
	}
	o.close();
	if (!o) { err = "no se pudo escribir " + salida_mp4; return false; }
	return true;
}

bool escribirPlantilla(const std::string& video_sano, const std::string& destino,
                       int& limpiados, std::string& err) {
	limpiados = 0;
	std::ifstream f(upath::toPath(video_sano), std::ios::binary);
	f.seekg(0, std::ios::end);
	const uint64_t total = uint64_t(f.tellg());
	f.seekg(0);

	// Se arma en memoria (solo cabeceras + ftyp + moov: KB o pocos MB).
	std::vector<uint8_t> out;
	out.resize(kMarcaBytes);
	putBe32(out.data(), uint32_t(kMarcaBytes));
	std::memcpy(out.data() + 4, kMarcaTipo, 4);
	std::memcpy(out.data() + 8, kMarcaVersion, 8);

	uint64_t pos = 0;
	Cabecera c;
	while (pos < total && leerCabecera(f, c)) {
		uint64_t size = c.size;
		if (size == 0) {
			// "Hasta el final": se escribe el tamano real (cabe en 32
			// bits porque el mdat esta limitado a kLimiteMdat).
			size = total - pos;
			if (c.hdr != 8 || size > 0xFFFFFFFFull) {
				err = "Átomo '" + fourccLegible(c.type) + "' demasiado grande.";
				return false;
			}
			putBe32(c.raw, uint32_t(size));
		}
		const uint64_t payload = size - c.hdr;
		if (c.type == "ftyp" || c.type == "moov") {
			size_t at = out.size();
			out.resize(at + size_t(size));
			std::memcpy(out.data() + at, c.raw, c.hdr);
			if (!f.read(reinterpret_cast<char*>(out.data() + at + c.hdr), std::streamsize(payload))) {
				err = "No se pudo leer el vídeo sano (¿está cortado?).";
				return false;
			}
			if (c.type == "moov") {
				std::vector<uint8_t> moov(out.begin() + at, out.end());
				limpiar(moov, 0, moov.size(), limpiados);
				std::copy(moov.begin(), moov.end(), out.begin() + at);
			}
		} else {
			// Cualquier otro atomo de nivel superior: solo la cabecera.
			// Si no es mdat/free/skip/wide se marca como 'free' (su
			// contenido no se guarda y no lo necesita el nucleo).
			if (c.type != "mdat" && c.type != "free" && c.type != "skip" && c.type != "wide") {
				std::memcpy(c.raw + 4, "free", 4);
				limpiados++;
			}
			out.insert(out.end(), c.raw, c.raw + c.hdr);
		}
		pos += size;
		f.seekg(std::streamoff(pos));
	}

	{
		std::ofstream o(upath::toPath(destino), std::ios::binary | std::ios::trunc);
		if (!o) { err = "No se puede crear " + destino + "."; return false; }
		o.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
		if (!o) { err = "No se pudo escribir " + destino + "."; return false; }
	}
	return true;
}

ResultadoCrear crearPlantilla(const std::string& video_sano, const std::string& dest_dir_in,
                              const std::string& id_in) {
	ResultadoCrear r;
	InfoPlantilla info = leerPlantilla(video_sano);
	if (!info.ok) {
		r.error_es = "El vídeo sano no sirve como plantilla: " + info.error_es + ".";
		return r;
	}
	if (info.mdat_bytes > kLimiteMdat) {
		r.error_es = "El vídeo sano es demasiado largo para una plantilla (" +
		             std::to_string(info.mdat_bytes / (1024 * 1024)) +
		             " MB de datos). Graba un clip corto de 10-15 segundos con la misma "
		             "cámara y la misma configuración y úsalo para la plantilla.";
		return r;
	}

	std::string id = id_in.empty() ? info.id : id_in;
	for (char& ch : id)
		if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' ||
		    ch == '<' || ch == '>' || ch == '|' || ch == ' ')
			ch = '_';
	std::string dest_dir = dest_dir_in.empty() ? carpetaUsuario() : dest_dir_in;
	std::error_code ec;
	fs::create_directories(upath::toPath(dest_dir), ec);
	const fs::path dest = upath::toPath(dest_dir) / upath::toPath(id + kExtension);

	if (!escribirPlantilla(video_sano, upath::fromPath(dest), r.atomos_limpiados, r.error_es))
		return r;
	r.path = upath::fromPath(dest);

	// Comprobacion: la plantilla materializada debe poder leerse como
	// referencia (el mismo parseOk que usara la reparacion).
	tempfs::ScopedDir tmp("plantilla-check");
	std::string err;
	InfoPlantilla creada = leerPlantilla(r.path);
	if (!creada.ok || !tmp.ok() ||
	    !materializarPlantilla(r.path, upath::fromPath(tmp.path() / upath::toPath("ref" + extensionMaterializada(creada))), err)) {
		fs::remove(dest, ec);
		r.path.clear();
		r.error_es = "La plantilla creada no es válida: " + (err.empty() ? creada.error_es : err);
		return r;
	}
	try {
		RepairOptions quiet = RepairOptions::fromGlobals();
		quiet.on_progress = nullptr;
		RepairSession session(quiet);
		LogMode prev = g_log_mode;
		g_log_mode = E;
		Mp4 mp4;
		g_mp4 = &mp4;
		try {
			mp4.parseOk(upath::fromPath(tmp.path() / upath::toPath("ref" + extensionMaterializada(creada))));
		} catch (...) {
			g_log_mode = prev;
			throw;
		}
		g_log_mode = prev;
	} catch (...) {
		fs::remove(dest, ec);
		r.path.clear();
		r.error_es = "Untrunc no puede usar este vídeo como referencia, así que tampoco "
		             "como plantilla. Prueba con otro vídeo sano de la misma cámara.";
		return r;
	}
	creada.id = id;
	r.info = creada;
	r.ok = true;
	return r;
}

std::string carpetaUsuario() {
#ifdef _WIN32
	std::string base = upath::getenv("APPDATA");
	if (base.empty()) base = upath::getenv("USERPROFILE");
	if (base.empty()) return "plantillas";
	return upath::fromPath(upath::toPath(base) / "untrunc" / "plantillas");
#else
	std::string base = upath::getenv("XDG_CONFIG_HOME");
	if (base.empty()) {
		std::string home = upath::getenv("HOME");
		if (home.empty()) return "plantillas";
		base = upath::fromPath(upath::toPath(home) / ".config");
	}
	return upath::fromPath(upath::toPath(base) / "untrunc" / "plantillas");
#endif
}

std::vector<std::string> carpetasBusqueda(const char* argv0) {
	std::vector<std::string> dirs;
	dirs.push_back(carpetaUsuario());
	std::string exe = carpetaExe();
	if (exe.empty() && argv0 && *argv0) {
		std::error_code ec;
		fs::path p = fs::absolute(upath::toPath(argv0), ec);
		if (!ec) exe = upath::fromPath(p.parent_path());
	}
	if (!exe.empty()) {
		dirs.push_back(upath::fromPath(upath::toPath(exe) / "plantillas"));
		dirs.push_back(upath::fromPath(upath::toPath(exe) / "profiles" / "plantillas"));
	}
	dirs.push_back("profiles/plantillas");
	dirs.push_back("../profiles/plantillas");
	return dirs;
}

std::vector<InfoPlantilla> listarPlantillas(const char* argv0) {
	std::vector<InfoPlantilla> out;
	std::set<std::string> ids;
	std::set<fs::path> vistas;
	for (const std::string& d : carpetasBusqueda(argv0)) {
		std::error_code ec;
		fs::path dir = upath::toPath(d);
		if (!fs::is_directory(dir, ec)) continue;
		fs::path canon = fs::weakly_canonical(dir, ec);
		if (!vistas.insert(ec ? dir : canon).second) continue;
		std::vector<fs::path> files;
		for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
			if (it->is_regular_file(ec) && it->path().extension() == kExtension)
				files.push_back(it->path());
		std::sort(files.begin(), files.end());
		for (const fs::path& p : files) {
			InfoPlantilla info = leerPlantilla(upath::fromPath(p));
			if (!info.ok || !ids.insert(info.id).second) continue;
			out.push_back(info);
		}
	}
	return out;
}

std::string resolverPlantilla(const std::string& ruta_o_id, const char* argv0) {
	std::error_code ec;
	if (fs::is_regular_file(upath::toPath(ruta_o_id), ec)) return ruta_o_id;
	for (const InfoPlantilla& p : listarPlantillas(argv0))
		if (p.id == ruta_o_id) return p.path;
	return "";
}

std::vector<InfoPlantilla> ordenarCandidatas(std::vector<InfoPlantilla> todas,
                                             const std::string& preferida,
                                             int width, int height,
                                             const std::string& codec_in) {
	const std::string codec = normalizarCodec(codec_in);
	std::vector<std::pair<int, InfoPlantilla>> puntuadas;
	for (InfoPlantilla& p : todas) {
		int score = 0;
		const bool es_preferida = !preferida.empty() && p.id == preferida;
		if (es_preferida) score += 1000;
		const std::string pc = normalizarCodec(p.video_codec);
		if (!codec.empty() && !pc.empty()) {
			// Un codec distinto no puede funcionar: se descarta (salvo
			// que el usuario la haya pedido por su nombre).
			if (pc != codec && !es_preferida) continue;
			score += 100;
		}
		if (width > 0 && height > 0) {
			if (p.width == width && p.height == height) score += 50;
			else if (p.width * p.height == width * height) score += 20;  // vertical
		}
		puntuadas.emplace_back(score, std::move(p));
	}
	std::stable_sort(puntuadas.begin(), puntuadas.end(),
	                 [](const auto& a, const auto& b) { return a.first > b.first; });
	std::vector<InfoPlantilla> out;
	for (auto& s : puntuadas) out.push_back(std::move(s.second));
	return out;
}

ResultadoPlantillas repararConPlantillas(const std::vector<InfoPlantilla>& candidatas,
                                         const std::string& danado,
                                         const RepairOptions& opts,
                                         size_t max_intentos) {
	ResultadoPlantillas r;
	if (candidatas.empty() || max_intentos == 0) return r;
	r.attempted = true;

	// Cada intento escribe su salida en una carpeta temporal propia,
	// para que no se pisen; la mejor se guarda aparte y al final se
	// mueve a donde iria una reparacion normal.
	tempfs::ScopedDir guardada("plantilla-mejor");
	bool hay_mejor = false;
	for (size_t i = 0; i < candidatas.size() && i < max_intentos; i++) {
		const InfoPlantilla& p = candidatas[i];
		tempfs::ScopedDir work("plantilla");
		std::string err;
		const fs::path ref = work.path() / upath::toPath(p.id + extensionMaterializada(p));
		const fs::path salida = work.path() / "salida";
		std::error_code ec;
		fs::create_directories(salida, ec);
		if (!work.ok() || ec || !materializarPlantilla(p.path, upath::fromPath(ref), err)) {
			r.probadas_es.push_back(p.id + ": no se pudo preparar (" +
			                        (err.empty() ? std::string("sin carpeta temporal") : err) + ")");
			continue;
		}
		RepairOptions o = opts;
		o.dst_path = upath::fromPath(salida);
		o.verificar = false;  // solo se comprueba la elegida, al final
		o.aprender = false;   // la plantilla no tiene datos de los que aprender
		RepairOutcome rep = repairFileWithAutoRetry(upath::fromPath(ref), danado, o, true);
		std::ostringstream line;
		line << p.id << " (" << p.resumenEs() << "): ";
		if (rep.ok)
			line << formatDurationEs(rep.duration_ms) << " recuperados";
		else
			line << "nada útil";
		r.probadas_es.push_back(line.str());
		if (!rep.ok) continue;
		if (hay_mejor && !attemptIsBetter(rep.duration_ms, rep.recovered_percent,
		                                  r.repair.duration_ms, r.repair.recovered_percent))
			continue;
		// Nueva mejor: se aparta (la carpeta de este intento se borra).
		const fs::path origen = upath::toPath(rep.output_path);
		const fs::path destino = guardada.path() / origen.filename();
		if (hay_mejor) fs::remove(upath::toPath(r.repair.output_path), ec);
		if (!moverArchivo(origen, destino, err)) continue;
		const std::string antes = rep.output_path;
		rep.output_path = upath::fromPath(destino);
		reemplazarTodo(rep.message_es, antes, rep.output_path);
		r.repair = rep;
		r.usada = p;
		hay_mejor = true;
	}
	if (!hay_mejor) return r;

	// Destino final: el mismo que una reparacion normal.
	const fs::path origen = upath::toPath(r.repair.output_path);
	fs::path destino;
	if (opts.dst_path.empty()) {
		std::string dir = ensureReparadosDir(danado);
		fs::path base = dir.empty() ? upath::toPath(danado).parent_path() : upath::toPath(dir);
		destino = base / origen.filename();
	} else if (isdir(opts.dst_path)) {
		destino = upath::toPath(opts.dst_path) / origen.filename();
	} else {
		destino = upath::toPath(opts.dst_path);
	}
	std::string err;
	if (!moverArchivo(origen, destino, err)) {
		r.repair.ok = false;
		r.repair.error_es = "Se recuperó el vídeo pero no se pudo guardar en " +
		                    upath::fromPath(destino) + ": " + err;
		return r;
	}
	const std::string antes = r.repair.output_path;
	r.repair.output_path = upath::fromPath(destino);
	r.repair.first_output_path = r.repair.output_path;
	r.repair.retry_output_path.clear();
	reemplazarTodo(r.repair.message_es, antes, r.repair.output_path);
	if (opts.verificar) {
		r.repair.verificacion = verificarVideo(r.repair.output_path);
		r.repair.message_es += "\n  " + r.repair.verificacion.resumenEs();
	}
	r.ok = true;
	return r;
}

} // namespace plantillas
