/*
	Untrunc-mejorado - repair_options.h

	Opciones de UNA operacion de reparacion/analisis, y la sesion
	que las aplica al nucleo sin que dos operaciones se mezclen.

	El nucleo heredado de untrunc (mp4.cpp, codec.cpp, track.cpp...)
	lee sus opciones de variables globales (g_ignore_unknown,
	g_use_chunk_stats, g_dst_path...) y usa g_mp4, asi que no puede
	ejecutar dos operaciones a la vez. Antes:
	  - la GUI escribia esas globales en vivo desde Ajustes: marcar
	    "-s" a mitad de una reparacion la cambiaba a medias;
	  - los botones de Analizar sobrescribian g_mp4 aunque hubiera
	    una reparacion en marcha en otro hilo;
	  - repair_job/hybrid cambiaban y restauraban globales a mano
	    (y una excepcion entre medias las dejaba cambiadas).

	Ahora:
	  - RepairOptions es una FOTO de las opciones, tomada al empezar
	    (en la GUI, al pulsar el boton). Cambiar Ajustes despues no
	    afecta a la operacion en curso.
	  - RepairSession (RAII) toma un mutex global, guarda las
	    globales, aplica las opciones y al destruirse las restaura y
	    suelta el mutex, tambien si hay una excepcion. Es recursivo:
	    una sesion puede abrir otra dentro (p. ej. el reintento -s).
	  - Las operaciones del nucleo quedan en serie: la segunda espera
	    (o, en el hilo de la interfaz, avisa en vez de congelarla).

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#ifndef UNTR_REPAIR_OPTIONS_H
#define UNTR_REPAIR_OPTIONS_H

#include <cstdint>
#include <string>

class Mp4;

extern bool g_verificar_salida;  // --sin-verificar lo pone a false

struct RepairOptions {
	bool ignore_unknown = false;        // -s
	uint64_t step = 1;                  // tamano de paso de -s
	bool use_chunk_stats = false;       // -dyn
	// Sugerencia de la IA (local, ONNX, LM Studio o externa) para la
	// busqueda de estrategias de repairFileWithAutoRetry: se prueba
	// antes que el orden por defecto. No se aplica al nucleo.
	// Comprobar el resultado decodificandolo (verificar.h). Por defecto
	// si; --sin-verificar lo apaga (g_verificar_salida).
	bool verificar = true;
	// Tras una zona danada, volver a encontrar el video (Mp4::tryResync,
	// g_resync_enabled). Si ONNX esta activado, el modelo filtra ademas.
	bool resinc = true;
	// Tras una zona danada, mantener el sonido a la par de la imagen
	// (Mp4::compensarSincro, g_sincro_enabled).
	bool sincro = true;
	bool hint_set = false;
	bool hint_s = false;
	bool hint_dyn = false;
	std::string hint_fuente;            // "local", "onnx"...
	bool stretch_video = false;         // -sv
	bool dont_exclude = false;          // -k
	bool rsv_ben_mode = false;          // -rsv-ben
	bool skip_existing = false;         // -skip
	bool search_mdat = false;
	bool strict_nal_frame_check = true;
	bool allow_large_sample = false;
	bool ignore_forbidden_nal_bit = false;
	bool dont_omit = false;
	bool ignore_keyframe_mismatch = false;
	bool skip_nal_filler_data = false;
	bool ignore_out_of_bound_chunks = false;
	bool no_ctts = false;
	unsigned max_partsize = 0;          // -mp (0 = automatico)
	unsigned max_partsize_default = 1u << 23;
	int64_t range_start = 0;
	int64_t range_end = 0;
	std::string dst_path;               // -dst ("" = Reparados/)
	void (*on_progress)(int) = nullptr; // barra de progreso de quien la lanza

	// Foto de las globales actuales (las del CLI tras leer argv).
	static RepairOptions fromGlobals();
};

class RepairSession {
public:
	// Espera a que termine cualquier otra operacion del nucleo.
	explicit RepairSession(const RepairOptions& opts);
	// try_only = true: no espera; si hay otra en marcha, active()
	// devuelve false y no se toca nada (para el hilo de la interfaz).
	RepairSession(const RepairOptions& opts, bool try_only);
	~RepairSession();
	RepairSession(const RepairSession&) = delete;
	RepairSession& operator=(const RepairSession&) = delete;

	bool active() const { return active_; }

	// true si alguna operacion del nucleo esta en marcha ahora.
	static bool busy();

private:
	void apply(const RepairOptions& opts);
	bool active_ = false;
	RepairOptions saved_;
	Mp4* saved_mp4_ = nullptr;
};

#endif // UNTR_REPAIR_OPTIONS_H
