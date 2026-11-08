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

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../src/ai/ai_provider.h"
#include "../src/diagnoser.h"
#include "../src/file.h"
#include "../src/formatos.h"
#include "../src/repair_job.h"
#include "../src/shell_util.h"
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
	testHonestPercent();
	testPrettyBytes();
	testReparadosPath();
	testProbeFile(dir);
	testDetectFormat(dir);
	testPreflight(dir);
	testJsonHelpers();
	testDiagnosticPacket();
	testUnicodePaths(dir);

	std::cout << g_checks << " comprobaciones, " << g_failures << " fallos\n";
	return g_failures == 0 ? 0 : 1;
}
