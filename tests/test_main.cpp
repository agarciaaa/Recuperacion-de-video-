/*
	Untrunc-mejorado - pruebas unitarias (sin dependencias externas).

	Prueban la logica nueva del fork que no necesita vídeos reales:
	pre-vuelo (diagnoser), deteccion de formato, paquete de
	diagnostico para la IA, utilidades JSON y de shell.

	Los archivos de prueba (MP4 minimos, MKV, basura...) se generan
	al vuelo en una carpeta temporal y se borran al terminar.

	Ejecutar:  make test
	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <iterator>
#include <chrono>
#include <fstream>
#include <thread>
#include <iostream>
#include <string>
#include <vector>

#include "../src/ai/ai_provider.h"
#include "../src/diagnoser.h"
#include "../src/file.h"
#include "../src/formatos.h"
#include "../src/common.h"
#include "../src/repair_job.h"
#include "../src/repair_options.h"
#include "../src/plantillas.h"
#include "../src/memoria.h"
#include "../src/verificar.h"
#include "../src/ai/ai_provider.h"
#include "../src/ai/onnx/onnx_provider.h"
#include "../src/shell_util.h"
#include "../src/tempfs.h"
#include "../src/upath.h"

namespace fs = std::filesystem;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                         \
		g_checks++;                                                              \
		if (!(cond)) {                                                           \
			g_failures++;                                                        \
			std::cerr << "FALLO " << __FILE__ << ":" << __LINE__ << ": " #cond "\n"; \
		}                                                                        \
	} while (0)

#define CHECK_EQ(a, b)                                                           \
	do {                                                                         \
		g_checks++;                                                              \
		auto va_ = (a);                                                          \
		auto vb_ = (b);                                                          \
		if (!(va_ == vb_)) {                                                     \
			g_failures++;                                                        \
			std::cerr << "FALLO " << __FILE__ << ":" << __LINE__ << ": " #a " == " #b \
			          << "\n";                                                   \
		}                                                                        \
	} while (0)

// ---------------------------------------------------------------------------
// Generadores de archivos de prueba
// ---------------------------------------------------------------------------

using Bytes = std::vector<unsigned char>;

static void put32(Bytes& b, uint32_t v) {
	b.push_back((unsigned char)(v >> 24));
	b.push_back((unsigned char)(v >> 16));
	b.push_back((unsigned char)(v >> 8));
	b.push_back((unsigned char)v);
}

// Atomo MP4: tamano (32 bits) + tipo + contenido.
static Bytes atom(const char* type, const Bytes& payload) {
	Bytes b;
	put32(b, (uint32_t)(8 + payload.size()));
	b.insert(b.end(), type, type + 4);
	b.insert(b.end(), payload.begin(), payload.end());
	return b;
}

static Bytes ftyp() {
	Bytes p = {'i', 's', 'o', 'm', 0, 0, 2, 0, 'i', 's', 'o', 'm', 'm', 'p', '4', '1'};
	return atom("ftyp", p);
}

static Bytes concat(std::initializer_list<Bytes> parts) {
	Bytes out;
	for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
	return out;
}

class TempDir {
public:
	TempDir() {
		path_ = fs::temp_directory_path() / ("untrunc-tests-" + std::to_string(std::rand()));
		fs::create_directories(path_);
	}
	~TempDir() {
		std::error_code ec;
		fs::remove_all(path_, ec);
	}
	// `name` en UTF-8 (puede llevar subcarpetas, acentos y ñ).
	std::string write(const std::string& name, const Bytes& data) const {
		fs::path p = path_ / upath::toPath(name);
		fs::create_directories(p.parent_path());
		std::ofstream f(p, std::ios::binary);
		f.write(reinterpret_cast<const char*>(data.data()), (std::streamsize)data.size());
		return upath::fromPath(p);
	}
	std::string path(const std::string& name) const {
		return upath::fromPath(path_ / upath::toPath(name));
	}

private:
	fs::path path_;
};

// MP4 "sano" minimo: ftyp + moov + mdat (con un SPS H.264 Annex-B).
static Bytes healthyMp4(size_t mdat_size = 1024) {
	Bytes mdat_payload(mdat_size, 0x11);
	const unsigned char sps[] = {0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28};
	std::copy(sps, sps + sizeof(sps), mdat_payload.begin());
	return concat({ftyp(), atom("moov", Bytes(32, 0)), atom("mdat", mdat_payload)});
}

// MP4 truncado tipico: ftyp + mdat, sin moov.
static Bytes truncatedMp4(size_t mdat_size = 1024) {
	return concat({ftyp(), atom("mdat", Bytes(mdat_size, 0x22))});
}

// ---------------------------------------------------------------------------
// Pruebas
// ---------------------------------------------------------------------------

static void testShellQuote() {
#ifdef _WIN32
	CHECK_EQ(shell::quote("C:\\Mis Vídeos\\a.mp4"), std::string("\"C:\\Mis Vídeos\\a.mp4\""));
	CHECK_EQ(std::string(shell::kNullDevice), std::string("NUL"));
#else
	CHECK_EQ(shell::quote("/tmp/mis videos/a.mp4"), std::string("'/tmp/mis videos/a.mp4'"));
	// Una comilla simple dentro no debe poder cerrar el argumento.
	CHECK_EQ(shell::quote("a'b"), std::string("'a'\\''b'"));
	CHECK_EQ(std::string(shell::kNullDevice), std::string("/dev/null"));
#endif
	CHECK(shell::discardStderr().find(shell::kNullDevice) != std::string::npos);
	CHECK(shell::discardAll().find("2>&1") != std::string::npos);
	// Un programa que no existe nunca debe darse por disponible
	// (antes, en Windows, ffmpeg nunca se daba por disponible).
	CHECK(!shell::commandAvailable("untrunc-programa-que-no-existe-xyz"));
	// Y un programa que SIEMPRE existe debe detectarse.
#ifdef _WIN32
	CHECK(shell::commandAvailable("cmd"));
#else
	CHECK(shell::commandAvailable("sh"));
#endif
}

static void testFormatDuration() {
	CHECK_EQ(formatDurationEs(0), std::string("desconocida"));
	CHECK_EQ(formatDurationEs(-5), std::string("desconocida"));
	CHECK_EQ(formatDurationEs(42000), std::string("42 s"));
	CHECK_EQ(formatDurationEs(61000), std::string("1 min 1 s"));
	CHECK_EQ(formatDurationEs(3600000), std::string("1 h 0 min 0 s"));
}

static void testHonestPercent() {
	// Caso real: con -s el escaneo llega al final (sin "premature
	// end") pero la salida pesa 1,4 KB y dura 0 s. Antes: "100%".
	CHECK(honestRecoveredPercent(false, 0, 1434, 1500000, 0) == 0.0);
	// Sin salida -> 0 aunque haya duracion.
	CHECK(honestRecoveredPercent(false, 0, 0, 1500000, 5000) == 0.0);
	// Reparacion buena: limitada por lo que acabo en la salida.
	double p = honestRecoveredPercent(true, 99.83, 1430000, 1500000, 14900);
	CHECK(p > 95.0 && p < 95.5);
	// Un solo fotograma (33 ms) no es un video recuperado.
	CHECK(honestRecoveredPercent(true, 0.04, 9000, 1500000, 33) == 0.0);
	// Corte temprano: manda el % escaneado.
	CHECK(honestRecoveredPercent(true, 40.0, 1400000, 1500000, 6000) == 40.0);
	// Nunca mas de 100.
	CHECK(honestRecoveredPercent(false, 0, 2000000, 1500000, 20000) == 100.0);
	// Sin tamano del danado: el % escaneado.
	CHECK(honestRecoveredPercent(false, 0, 1000, 0, 1000) == 100.0);

	// Se elige por duracion recuperada, no por el % escaneado.
	CHECK(!attemptIsBetter(0, 100.0, 33, 0.04));
	CHECK(attemptIsBetter(14900, 50.0, 33, 99.0));
	CHECK(attemptIsBetter(1000, 60.0, 1000, 50.0));
	CHECK(!attemptIsBetter(1000, 50.0, 1000, 50.0));
}

static void testRepairSession() {
	g_ignore_unknown = false;
	g_use_chunk_stats = false;
	g_dst_path = "";
	RepairOptions o = RepairOptions::fromGlobals();
	CHECK(!o.ignore_unknown);
	o.ignore_unknown = true;
	o.dst_path = "salida";
	CHECK(!RepairSession::busy());
	{
		RepairSession s(o);
		CHECK(s.active());
		CHECK(RepairSession::busy());
		CHECK(g_ignore_unknown);
		CHECK_EQ(g_dst_path, std::string("salida"));
		// Sesion anidada (reintento -s -dyn) sobre una copia.
		RepairOptions retry = o;
		retry.use_chunk_stats = true;
		{
			RepairSession s2(retry);
			CHECK(g_use_chunk_stats);
		}
		CHECK(!g_use_chunk_stats);  // vuelve a la de la sesion externa
		CHECK(g_ignore_unknown);
	}
	// Al salir, todo como estaba.
	CHECK(!g_ignore_unknown);
	CHECK_EQ(g_dst_path, std::string(""));
	CHECK(!RepairSession::busy());

	// Una excepcion dentro de la sesion tambien restaura.
	try {
		RepairSession s(o);
		throw std::runtime_error("fallo simulado");
	} catch (const std::exception&) {
	}
	CHECK(!g_ignore_unknown);

	// Otro hilo con try_only no espera ni toca nada si hay otra en marcha.
	{
		RepairSession s(o);
		bool other_active = true;
		std::thread t([&] {
			RepairOptions other;
			RepairSession s2(other, true);
			other_active = s2.active();
		});
		t.join();
		CHECK(!other_active);
		CHECK(g_ignore_unknown);  // la operacion en curso no se altero
	}

	// Y una sesion normal en otro hilo ESPERA a que termine la primera.
	std::atomic<bool> first_done{false};
	std::atomic<bool> second_saw_first_done{false};
	std::thread t;
	{
		RepairSession s(o);
		t = std::thread([&] {
			RepairOptions other;
			RepairSession s2(other);
			second_saw_first_done = first_done.load();
		});
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		first_done = true;
		// s se destruye al cerrar este bloque; solo entonces entra el hilo.
	}
	t.join();
	CHECK(second_saw_first_done.load());
	CHECK(!g_ignore_unknown);
}

// MP4 "de camara" con metadatos personales, para las plantillas.
static Bytes fullBox(const char* type, uint8_t version, const Bytes& rest) {
	Bytes p = {version, 0, 0, 0};
	p.insert(p.end(), rest.begin(), rest.end());
	return atom(type, p);
}

static Bytes cameraMp4(const char* codec, uint16_t w, uint16_t h) {
	Bytes mvhd_rest(96, 0);
	std::fill(mvhd_rest.begin(), mvhd_rest.begin() + 4, 0x11);      // creacion
	std::fill(mvhd_rest.begin() + 4, mvhd_rest.begin() + 8, 0x12);  // modificacion
	Bytes tkhd_rest(80, 0);
	std::fill(tkhd_rest.begin(), tkhd_rest.begin() + 8, 0x13);
	Bytes mdhd_rest(28, 0);                                          // v1: 8+8 fechas
	std::fill(mdhd_rest.begin(), mdhd_rest.begin() + 16, 0x14);
	Bytes hdlr_rest = {0, 0, 0, 0, 'v', 'i', 'd', 'e'};
	hdlr_rest.resize(hdlr_rest.size() + 13, 0);
	Bytes entry;
	put32(entry, 86);
	entry.insert(entry.end(), codec, codec + 4);
	entry.resize(entry.size() + 6 + 2 + 16, 0);
	entry.push_back((unsigned char)(w >> 8)); entry.push_back((unsigned char)w);
	entry.push_back((unsigned char)(h >> 8)); entry.push_back((unsigned char)h);
	entry.resize(86, 0);
	Bytes stsd_rest = {0, 0, 0, 1};
	stsd_rest.insert(stsd_rest.end(), entry.begin(), entry.end());
	const std::string gps = "+40.4168-003.7038/";
	const char xyz[4] = {(char)0xA9, 'x', 'y', 'z'};
	Bytes trak = atom("trak", concat({
	    fullBox("tkhd", 0, tkhd_rest),
	    atom("mdia", concat({
	        fullBox("mdhd", 1, mdhd_rest),
	        fullBox("hdlr", 0, hdlr_rest),
	        atom("minf", atom("stbl", fullBox("stsd", 0, stsd_rest)))}))}));
	Bytes moov = atom("moov", concat({
	    fullBox("mvhd", 0, mvhd_rest), trak,
	    atom("udta", atom(xyz, Bytes(gps.begin(), gps.end()))),
	    atom("meta", Bytes({'s', 'e', 'c', 'r', 'e', 't', 'o'}))}));
	Bytes mdat_payload(5000, 0x5A);
	return concat({ftyp(), atom("free", Bytes(8, 0x7F)),
	               atom("uuid", Bytes({'s', 'e', 'r', 'i', 'a', 'l', '1', '2', '3'})),
	               atom("mdat", mdat_payload), moov});
}

static bool contains(const Bytes& hay, const std::string& needle) {
	return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

static Bytes readAll(const std::string& path) {
	std::ifstream f(upath::toPath(path), std::ios::binary);
	return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static void testPlantillas(const TempDir& dir) {
	using namespace plantillas;
	const Bytes original = cameraMp4("avc1", 1920, 1080);
	const std::string sano = dir.write("Cámara ñ/sano.mp4", original);

	InfoPlantilla vi = leerPlantilla(sano);
	CHECK(vi.ok);
	CHECK_EQ(vi.brand, std::string("isom"));
	CHECK_EQ(vi.video_codec, std::string("avc1"));
	CHECK_EQ(vi.width, 1920);
	CHECK_EQ(vi.height, 1080);
	CHECK_EQ(vi.mdat_bytes, (uint64_t)5000);

	// Escribir: pequena y sin datos personales.
	const std::string tpl = dir.path("Cámara ñ/mi cámara.plantilla");
	int limpiados = -1;
	std::string err;
	CHECK(escribirPlantilla(sano, tpl, limpiados, err));
	CHECK(err.empty());
	const Bytes t = readAll(tpl);
	CHECK(t.size() < original.size() - 4000);  // sin el mdat
	CHECK(!contains(t, "+40.4168"));
	CHECK(!contains(t, "secreto"));
	CHECK(!contains(t, "serial123"));
	CHECK(!contains(t, std::string(4, (char)0x11)));  // fecha de mvhd
	CHECK(!contains(t, std::string(4, (char)0x13)));  // fecha de tkhd
	CHECK(!contains(t, std::string(8, (char)0x14)));  // fecha de mdhd (v1)
	CHECK(limpiados >= 3);  // udta, meta y uuid

	InfoPlantilla ti = leerPlantilla(tpl);
	CHECK(ti.ok);
	CHECK_EQ(ti.id, std::string("mi cámara"));
	CHECK_EQ(ti.video_codec, std::string("avc1"));
	CHECK_EQ(ti.width, 1920);
	CHECK_EQ(ti.mdat_bytes, (uint64_t)5000);
	CHECK_EQ(extensionMaterializada(ti), std::string(".mp4"));

	// Materializar: mismo tamano y mismos desplazamientos que el
	// original; el mdat a cero; el moov igual salvo lo limpiado.
	const std::string mat = dir.path("Cámara ñ/ref.mp4");
	CHECK(materializarPlantilla(tpl, mat, err));
	const Bytes m = readAll(mat);
	CHECK_EQ(m.size(), original.size());
	if (m.size() == original.size()) {
		CHECK(std::equal(original.begin(), original.begin() + 24, m.begin()));  // ftyp
		const size_t mdat_at = 24 + 16 + 17;  // ftyp + free + uuid
		CHECK(std::equal(original.begin() + mdat_at, original.begin() + mdat_at + 8,
		                 m.begin() + mdat_at));  // cabecera del mdat intacta
		CHECK(std::all_of(m.begin() + mdat_at + 8, m.begin() + mdat_at + 5008,
		                  [](unsigned char c) { return c == 0; }));
		const size_t moov_at = mdat_at + 5008;
		CHECK(std::equal(original.begin() + moov_at, original.begin() + moov_at + 8,
		                 m.begin() + moov_at));
	}
	CHECK(leerPlantilla(mat).ok);

	// Un MP4 normal no es una plantilla; un truncado no tiene moov.
	CHECK(!materializarPlantilla(sano, dir.path("Cámara ñ/x.mp4"), err));
	CHECK(!err.empty());
	CHECK(!leerPlantilla(dir.write("Cámara ñ/truncado.mp4", truncatedMp4())).ok);

	// Orden de candidatas.
	auto mk = [](const char* id, const char* c, int w, int h) {
		InfoPlantilla i;
		i.ok = true; i.id = id; i.video_codec = c; i.width = w; i.height = h;
		return i;
	};
	std::vector<InfoPlantilla> todas = {mk("hevc", "hvc1", 1920, 1080),
	                                    mk("hd", "avc1", 1280, 720),
	                                    mk("fhd", "avc1", 1920, 1080)};
	auto o = ordenarCandidatas(todas, "", 1920, 1080, "avc1?");
	CHECK_EQ(o.size(), (size_t)2);  // la HEVC no puede servir para H.264
	if (o.size() == 2) {
		CHECK_EQ(o[0].id, std::string("fhd"));
		CHECK_EQ(o[1].id, std::string("hd"));
	}
	o = ordenarCandidatas(todas, "hevc", 1920, 1080, "avc1");
	CHECK(!o.empty() && o[0].id == "hevc");  // pedida por nombre: primero
	o = ordenarCandidatas(todas, "", 0, 0, "");  // sin pistas: todas
	CHECK_EQ(o.size(), (size_t)3);

	auto dirs = carpetasBusqueda(nullptr);
	CHECK(!dirs.empty() && dirs[0] == carpetaUsuario());
}

// Cambia una variable de entorno (para apuntar HOME/USERPROFILE a una
// carpeta de prueba y no tocar la configuracion real del usuario).
static void ponerEnv(const char* k, const std::string& v) {
#ifdef _WIN32
	// Version ancha: la estrecha interpretaria la "ñ" en ANSI.
	_wputenv_s(fs::path(k).wstring().c_str(), upath::toPath(v).wstring().c_str());
#else
	setenv(k, v.c_str(), 1);
#endif
}

static void testMemoria(const TempDir& dir) {
	memoria::usarArchivo(dir.path("memoria ñ/memoria.tsv"));
	CHECK(!memoria::consultar("avc1 1920x1080").hay);
	CHECK(memoria::anotar("avc1 1920x1080", true, true));
	CHECK(memoria::anotar("avc1 1920x1080", false, false));
	CHECK(memoria::anotar("avc1 1920x1080", true, true));
	CHECK(memoria::anotar("hvc1 3840x2160", false, true));
	memoria::Recuerdo r = memoria::consultar("avc1 1920x1080");
	CHECK(r.hay);
	CHECK(r.s && r.dyn);           // -s -dyn gano 2 veces, normal 1
	CHECK_EQ(r.victorias, 2);
	r = memoria::consultar("hvc1 3840x2160");
	CHECK(r.hay && !r.s && r.dyn);
	CHECK(!memoria::consultar("").hay);
	// Sin rutas ni nombres de archivo: solo firmas.
	const Bytes m = readAll(dir.path("memoria ñ/memoria.tsv"));
	CHECK(!contains(m, dir.path("")));
	// Firma desde la cabecera (sirve igual con video sano o plantilla).
	const std::string sano = dir.write("memoria ñ/sano.mp4", cameraMp4("avc1", 1280, 720));
	CHECK_EQ(memoria::firmaReferencia(sano), std::string("avc1 1280x720 ftyp:isom pistas:avc1"));
	memoria::usarArchivo("");
}

static void testConfigYOnnx(const TempDir& dir) {
	// HOME/USERPROFILE de prueba: ai.conf y el entrenamiento van ahi.
	const std::string home = dir.path("casa ñ");
	fs::create_directories(upath::toPath(home));
	const std::string old_home = upath::getenv("HOME"), old_up = upath::getenv("USERPROFILE");
	ponerEnv("HOME", home);
	ponerEnv("USERPROFILE", home);

	CHECK_EQ(aiConfPath(), upath::fromPath(upath::toPath(home) / ".config" / "untrunc" / "ai.conf"));
	CHECK(!loadAiConfig().onnx_enabled);
	CHECK(guardarAiConf("onnx_enabled", "1"));
	CHECK(guardarAiConf("local_profile", "gopro_h264_1080p60"));
	CHECK(guardarAiConf("onnx_enabled", "1"));  // cambiar, no duplicar
	CHECK(!guardarAiConf("api_key", "secreto"));  // nunca la clave
	AiConfig c = loadAiConfig();
	CHECK(c.onnx_enabled);  // antes se pisaba con el global de la GUI
	CHECK_EQ(c.local_profile, std::string("gopro_h264_1080p60"));
	const Bytes conf = readAll(aiConfPath());
	CHECK(!contains(conf, "secreto"));
	CHECK(guardarAiConf("onnx_enabled", "0"));
	CHECK(!loadAiConfig().onnx_enabled);

	// Modelo activo sin pesos.json (lo que habia en la PC de Angel):
	// antes se elegia igual y el ONNX quedaba muerto; ahora se salta
	// y se usa la copia de seguridad, avisando.
	const std::string base = findBaseModelDir(nullptr);
	CHECK(!base.empty());
	std::string why, aviso;
	OnnxModelInfo info;
	if (!base.empty()) {
		CHECK(onnxModelUsable(base, why));
		const fs::path activo = upath::toPath(onnxModelActivoDir());
		const fs::path anterior = upath::toPath(onnxModelAnteriorDir());
		fs::create_directories(activo);
		fs::create_directories(anterior);
		fs::copy_file(upath::toPath(base) / "model_info.json", activo / "model_info.json");
		CHECK(!onnxModelUsable(onnxModelActivoDir(), why));
		CHECK(why.find("pesos.json") != std::string::npos);
		for (const char* n : {"model_info.json", "pesos.json"})
			fs::copy_file(upath::toPath(base) / n, anterior / n);
		CHECK_EQ(onnxModeloEnUso("", nullptr, info, aviso), onnxModelAnteriorDir());
		CHECK(info.cargado);
		CHECK(!aviso.empty());
		// Sin copia de seguridad: el modelo base incluido.
		fs::remove_all(anterior);
		CHECK_EQ(onnxModeloEnUso("", nullptr, info, aviso), base);
		// El proveedor carga de verdad (antes: weights_loaded_ false).
		AiConfig oc;
		oc.onnx_enabled = true;
		OnnxProvider p(oc);
		CHECK(p.isAvailable(why));
		CHECK(!p.avisoCarga().empty());
	}
	ponerEnv("HOME", old_home);
	ponerEnv("USERPROFILE", old_up);
}

static void testVerificar(const TempDir& dir) {
	CHECK_EQ(formatoTiempo(0), std::string("0:00"));
	CHECK_EQ(formatoTiempo(9.6), std::string("0:09"));
	CHECK_EQ(formatoTiempo(65), std::string("1:05"));
	CHECK_EQ(formatoTiempo(3723), std::string("1:02:03"));

	// Errores a menos de 1 s se juntan en un tramo; separados, no.
	auto t = agruparErrores({7.0, 7.1, 7.5, 14.2, 3.0, -1}, 1.0, 0.04);
	CHECK_EQ(t.size(), (size_t)3);
	// Con tolerancia: en 32 bits (x87, 80 bits) 14.2 no sale exacto y
	// "== 14.2" fallaba solo en la compilacion de 32 bits del CI.
	auto cerca = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
	if (t.size() == 3) {
		CHECK(cerca(t[0].inicio_s, 3.0) && t[0].errores == 1);
		CHECK(cerca(t[1].inicio_s, 7.0) && t[1].errores == 3);
		CHECK(t[1].fin_s > 7.5);
		CHECK(cerca(t[2].inicio_s, 14.2));
	}
	CHECK(agruparErrores({}).empty());

	InformeVerificacion inf;
	inf.hecho = true;
	inf.tiene_video = true;
	inf.fotogramas = 100;
	inf.duracion_s = 10;
	CHECK(inf.limpio());
	CHECK(inf.resumenEs().find("sin errores") != std::string::npos);
	inf.errores_video = 5;
	inf.tramos_video = agruparErrores({2.0, 2.2}, 1.0, 0.5);
	CHECK(!inf.limpio());
	const std::string r = inf.resumenEs();
	CHECK(r.find("0:02") != std::string::npos);
	CHECK(r.find("5 de 105") != std::string::npos);
	CHECK(inf.porcentajeConErrores() > 0 && inf.porcentajeConErrores() < 100);

	// Un archivo que no se puede decodificar: informe "no hecho", sin colgarse.
	InformeVerificacion malo = verificarVideo(dir.write("verificar ñ/roto.mp4", truncatedMp4()));
	CHECK(!malo.hecho);
	CHECK(!malo.error_es.empty());
	CHECK(!verificarVideo(dir.path("verificar ñ/no existe.mp4")).hecho);
}

static void testTempDirs() {
	fs::path kept;
	{
		tempfs::ScopedDir a("prueba");
		tempfs::ScopedDir b("prueba");
		CHECK(a.ok());
		CHECK(b.ok());
		// Dos operaciones a la vez nunca comparten carpeta.
		CHECK(a.path() != b.path());
		CHECK(fs::is_directory(a.path()));
		std::ofstream(a / "x.txt") << "hola";
		kept = a.path();
	}
	// Se borra al salir del ambito, con su contenido.
	CHECK(!fs::exists(kept));

	std::error_code ec;
	fs::path d = tempfs::createUniqueDir("prueba", ec);
	CHECK(!ec);
	CHECK(fs::is_directory(d));
	tempfs::removeDir(d);
	CHECK(!fs::exists(d));

	tempfs::ScopedDir r("prueba");
	fs::path released = r.release();
	CHECK(!r.ok());
	CHECK(fs::is_directory(released));
	tempfs::removeDir(released);
}

static void testPrettyBytes() {
	CHECK_EQ(prettyBytesDiag(512), std::string("512.00 B"));
	CHECK_EQ(prettyBytesDiag(2048), std::string("2.00 KB"));
	CHECK_EQ(prettyBytesDiag(3ull << 20), std::string("3.00 MB"));
}

static void testReparadosPath() {
	fs::path out = repairedPathInReparados((fs::path("carpeta") / "video.mp4").string());
	CHECK_EQ(out.filename().string(), std::string("video.mp4_fixed.mp4"));
	CHECK_EQ(out.parent_path().filename().string(), std::string("Reparados"));
	// Sin extension se usa .mp4
	CHECK_EQ(fs::path(repairedPathInReparados("clip")).filename().string(),
	         std::string("clip_fixed.mp4"));
}

static void testProbeFile(const TempDir& dir) {
	std::string ok = dir.write("sano.mp4", healthyMp4());
	FileProbe p = probeFile(ok);
	CHECK(p.exists);
	CHECK(p.has_ftyp);
	CHECK_EQ(p.ftyp, std::string("isom"));
	CHECK(p.has_moov);
	CHECK(p.has_mdat);
	CHECK_EQ(p.mdat_bytes, (uint64_t)1024);
	CHECK(p.has_avc_sps);
	CHECK(p.is_bmff);

	std::string trunc = dir.write("truncado.mp4", truncatedMp4());
	FileProbe t = probeFile(trunc);
	CHECK(t.has_ftyp);
	CHECK(!t.has_moov);
	CHECK(t.has_mdat);

	FileProbe missing = probeFile(dir.path("no-existe.mp4"));
	CHECK(!missing.exists);

	// Cabecera inicial danada: basura delante y la estructura MP4 despues.
	Bytes garbage(100, 0xFF);
	std::string shifted = dir.write("desplazado.mp4", concat({garbage, truncatedMp4()}));
	FileProbe s = probeFile(shifted);
	CHECK(!s.has_ftyp);
	CHECK(s.has_ftyp_deep);
	CHECK(s.has_mdat_deep);
	CHECK_EQ(s.deep_ftyp_offset, (uint64_t)100);
}

static void testDetectFormat(const TempDir& dir) {
	std::string mp4 = dir.write("a.mp4", healthyMp4());
	CHECK(detectFormat(mp4).kind == FormatKind::Bmff);
	CHECK(detectFormat(mp4).is_bmff);

	// Un MKV renombrado a .mp4 sigue siendo MKV (deteccion por contenido).
	Bytes mkv = {0x1A, 0x45, 0xDF, 0xA3, 0x9F, 0x42, 0x86, 0x81, 0x01};
	mkv.resize(64, 0);
	std::string fake = dir.write("en-realidad-mkv.mp4", mkv);
	FormatInfo fi = detectFormat(fake);
	CHECK(fi.kind == FormatKind::Mkv);
	CHECK(!fi.is_bmff);
	CHECK(fi.convertible);

	Bytes avi = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'A', 'V', 'I', ' '};
	avi.resize(64, 0);
	CHECK(detectFormat(dir.write("b.avi", avi)).kind == FormatKind::Avi);
}

static void testPreflight(const TempDir& dir) {
	std::string ref = dir.write("ref.mp4", healthyMp4());
	std::string bad = dir.write("bad.mp4", truncatedMp4());

	CHECK(preflightCheck(ref, bad).code == DiagCode::Ok);
	CHECK(preflightCheck(ref, ref).code == DiagCode::SameFile);
	CHECK(preflightCheck(dir.path("falta.mp4"), bad).code == DiagCode::RefMissing);
	CHECK(preflightCheck(ref, dir.path("falta.mp4")).code == DiagCode::BadMissing);

	std::string empty = dir.write("vacio.mp4", {});
	CHECK(preflightCheck(empty, bad).code == DiagCode::RefZeroLength);
	CHECK(preflightCheck(ref, empty).code == DiagCode::BadZeroLength);

	// Referencia sin moov, y el "danado" es un MP4 sano grande:
	// lo mas probable es que esten al reves.
	std::string big_ok = dir.write("grande-sano.mp4", healthyMp4(100 * 1024));
	CHECK(preflightCheck(bad, big_ok).code == DiagCode::RefNoMoovSwappedLikely);
	// Referencia sin moov y el danado tambien sin moov.
	std::string bad2 = dir.write("bad2.mp4", truncatedMp4());
	CHECK(preflightCheck(bad, bad2).code == DiagCode::RefNoMoov);

	// Danado sin mdat (solo cabecera).
	std::string no_mdat = dir.write("sin-mdat.mp4", concat({ftyp(), atom("free", Bytes(16, 0))}));
	CHECK(preflightCheck(ref, no_mdat).code == DiagCode::BadNoMdat);

	// Formatos no BMFF: hay que convertir a MP4 primero.
	Bytes mkv = {0x1A, 0x45, 0xDF, 0xA3, 0x9F, 0x42, 0x86, 0x81, 0x01};
	mkv.resize(64, 0);
	std::string mkv_path = dir.write("video.mkv", mkv);
	CHECK(preflightCheck(mkv_path, bad).code == DiagCode::RefFormatNoBmff);
	CHECK(preflightCheck(ref, mkv_path).code == DiagCode::BadFormatNoBmff);

	// Cabecera inicial danada pero estructura MP4 mas adelante.
	Bytes garbage(100, 0xFF);
	std::string shifted = dir.write("desplazado.mp4", concat({garbage, truncatedMp4()}));
	CHECK(preflightCheck(ref, shifted).code == DiagCode::BadCabeceraDaniada);

	// Basura total: no parece un video.
	std::string junk = dir.write("basura.bin", Bytes(4096, 0xAB));
	CHECK(preflightCheck(ref, junk).code == DiagCode::BadNoFtyp);

	// Todo codigo tiene nombre tecnico, mensaje y accion en espanol.
	DiagReport r = preflightCheck(bad, big_ok);
	CHECK_EQ(diagCodeName(r.code), std::string("RefNoMoovSwappedLikely"));
	CHECK(!diagMessageEs(r).empty());
	CHECK(!diagActionEs(r.code).empty());
}

static void testJsonHelpers() {
	CHECK_EQ(jsonEscape("a\"b\\c\n"), std::string("a\\\"b\\\\c\\n"));
	CHECK_EQ(toHexLower((const unsigned char*)"\x00\xab\xff", 3), std::string("00abff"));

	const std::string resp = "{\"modo\": \"perfil\", \"confianza\": 0.83, \"n\":-2e1}";
	std::string s;
	double d = 0;
	CHECK(jsonFindString(resp, "modo", s));
	CHECK_EQ(s, std::string("perfil"));
	CHECK(jsonFindNumber(resp, "confianza", d));
	CHECK(d > 0.829 && d < 0.831);
	CHECK(jsonFindNumber(resp, "n", d));
	CHECK(d == -20.0);
	CHECK(!jsonFindNumber(resp, "no_existe", d));
	CHECK(!jsonFindString(resp, "no_existe", s));
}

static void testDiagnosticPacket() {
	AiDiagnosis d;
	d.file_exists = true;
	d.file_size = 123456;
	d.ftyp = "isom";
	d.has_mdat = true;
	d.codec = "avc1";
	d.width = 1920;
	d.height = 1080;
	d.markers = "texto con \"comillas\"";

	DiagnosticPacket p = buildDiagnosticPacket(d, "RefNoMoov");
	CHECK(p.valid);
	CHECK_EQ(p.bytes, p.json.size());
	CHECK(p.bytes < kMaxDiagnosticPacketBytes);
	CHECK(p.json.find("\"codigo_preflight\":\"RefNoMoov\"") != std::string::npos);
	CHECK(p.json.find("\"ancho\":1920") != std::string::npos);
	// Privacidad: nunca la ruta real, solo nombres genericos.
	CHECK(p.json.find("video_danado.mp4") != std::string::npos);

	std::string why;
	DiagnosticPacket big;
	big.json = "{" + std::string(kMaxDiagnosticPacketBytes, 'x') + "}";
	big.bytes = big.json.size();
	CHECK(!validateDiagnosticPacket(big, why));
	CHECK(!why.empty());

	DiagnosticPacket binary;
	binary.json = std::string("{\"a\":\"") + '\x01' + "\"}";
	binary.bytes = binary.json.size();
	CHECK(!validateDiagnosticPacket(binary, why));

	DiagnosticPacket forbidden;
	forbidden.json = "{\"video_base64\":\"AAAA\"}";
	forbidden.bytes = forbidden.json.size();
	CHECK(!validateDiagnosticPacket(forbidden, why));

	DiagnosticPacket not_json;
	not_json.json = "hola";
	not_json.bytes = not_json.json.size();
	CHECK(!validateDiagnosticPacket(not_json, why));
}

// Rutas con acentos y ñ (p. ej. "C:\Users\José\Mis Vídeos"). Antes, en
// Windows, el pre-vuelo decia "no existe" / "formato desconocido".
static void testUnicodePaths(const TempDir& dir) {
	const std::string carpeta = "Mis Vídeos de José ñandú";
	// Ida y vuelta UTF-8 -> ruta -> UTF-8 sin perder nada.
	CHECK_EQ(upath::fromPath(upath::toPath(carpeta)), carpeta);
#ifdef _WIN32
	CHECK(upath::toPath(carpeta).wstring() == std::wstring(L"Mis Vídeos de José ñandú"));
#endif

	std::string ref = dir.write(carpeta + "/referencia sana.mp4", healthyMp4());
	std::string bad = dir.write(carpeta + "/vídeo dañado.mp4", truncatedMp4());
	CHECK(fs::exists(upath::toPath(ref)));

	FileProbe p = probeFile(bad);
	CHECK(p.exists);
	CHECK(p.has_ftyp);
	CHECK(p.has_mdat);
	CHECK(detectFormat(bad).kind == FormatKind::Bmff);
	CHECK(preflightCheck(ref, bad).code == DiagCode::Ok);
	CHECK(preflightCheck(ref, ref).code == DiagCode::SameFile);

	// La carpeta Reparados/ se crea junto al danado, con su nombre real.
	std::string reparados = ensureReparadosDir(bad);
	CHECK(!reparados.empty());
	CHECK(isdir(reparados));  // el nucleo decide con esto si -dst es carpeta
	CHECK(!isdir(bad));
	CHECK(fs::is_directory(upath::toPath(reparados)));
	CHECK_EQ(upath::fromPath(upath::toPath(reparados).parent_path().filename()), carpeta);
	CHECK_EQ(upath::fromPath(upath::toPath(repairedPathInReparados(bad)).filename()),
	         std::string("vídeo dañado.mp4_fixed.mp4"));
	CHECK_EQ(upath::fromPath(upath::toPath(convertedPathDefault(bad)).filename()),
	         std::string("vídeo dañado_convertido.mp4"));

	// Los comandos externos tambien reciben la ruta bien: se pide a la
	// shell que compruebe que el archivo existe.
#ifdef _WIN32
	CHECK(upath::system("if exist " + shell::quote(bad) + " (exit 0) else (exit 1)") == 0);
#else
	CHECK(upath::system("test -f " + shell::quote(bad)) == 0);
#endif
}

int main() {
	TempDir dir;
	testShellQuote();
	testFormatDuration();
	testTempDirs();
	testRepairSession();
	testHonestPercent();
	testPrettyBytes();
	testReparadosPath();
	testProbeFile(dir);
	testDetectFormat(dir);
	testPreflight(dir);
	testJsonHelpers();
	testDiagnosticPacket();
	testUnicodePaths(dir);
	testPlantillas(dir);
	testMemoria(dir);
	testConfigYOnnx(dir);
	testVerificar(dir);

	std::cout << g_checks << " comprobaciones, " << g_failures << " fallos\n";
	return g_failures == 0 ? 0 : 1;
}
