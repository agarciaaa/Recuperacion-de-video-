/*
	Untrunc-mejorado - verificar.cpp (ver verificar.h)

	Usa la API send_packet/receive_frame (FFmpeg >= 3.1), la que hay
	en todas las versiones con las que compila el proyecto (3.3 a 8.x).

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "verificar.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
}

namespace {

struct Decodificador {
	AVCodecContext* ctx = nullptr;
	int stream = -1;
	AVRational tb{1, 1};
	std::vector<double> errores;  // instantes (s) con error
	int buenos = 0;

	~Decodificador() {
		if (ctx) avcodec_free_context(&ctx);
	}

	bool abrir(AVFormatContext* fmt, int idx) {
		AVStream* st = fmt->streams[idx];
		const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
		if (!codec) return false;
		ctx = avcodec_alloc_context3(codec);
		if (!ctx) return false;
		if (avcodec_parameters_to_context(ctx, st->codecpar) < 0) return false;
		// Que los errores de flujo se NOTEN (sin esto el decodificador
		// los disimula y devuelve imagenes con bloques grises).
		ctx->err_recognition |= AV_EF_EXPLODE | AV_EF_BITSTREAM | AV_EF_BUFFER;
		// Hilos por "slices": el error se atribuye al paquete correcto
		// (con hilos por fotograma llega varios fotogramas tarde).
		ctx->thread_count = 0;
		ctx->thread_type = FF_THREAD_SLICE;
		if (avcodec_open2(ctx, codec, nullptr) < 0) return false;
		stream = idx;
		tb = st->time_base;
		return true;
	}

	double segundos(int64_t ts) const {
		if (ts == AV_NOPTS_VALUE) return -1;
		return ts * av_q2d(tb);
	}

	// Envia un paquete (o nullptr para vaciar) y recoge los fotogramas.
	void decodificar(const AVPacket* pkt, AVFrame* frame, double t_paquete) {
		int r = avcodec_send_packet(ctx, pkt);
		if (r < 0 && r != AVERROR_EOF && r != AVERROR(EAGAIN)) {
			if (t_paquete >= 0) errores.push_back(t_paquete);
		}
		for (;;) {
			r = avcodec_receive_frame(ctx, frame);
			if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
			if (r < 0) {
				if (t_paquete >= 0) errores.push_back(t_paquete);
				break;
			}
			double t = segundos(frame->best_effort_timestamp);
			if (t < 0) t = segundos(frame->pts);
			if (t < 0) t = t_paquete;
			if ((frame->flags & AV_FRAME_FLAG_CORRUPT) || frame->decode_error_flags) {
				if (t >= 0) errores.push_back(t);
			} else {
				buenos++;
			}
			av_frame_unref(frame);
		}
	}
};

} // namespace

std::vector<TramoConErrores> agruparErrores(std::vector<double> t, double separacion_s,
                                            double duracion_min_s) {
	std::vector<TramoConErrores> out;
	std::sort(t.begin(), t.end());
	for (double x : t) {
		if (x < 0) continue;
		if (!out.empty() && x - out.back().fin_s <= separacion_s) {
			out.back().fin_s = std::max(out.back().fin_s, x + duracion_min_s);
			out.back().errores++;
		} else {
			TramoConErrores tr;
			tr.inicio_s = x;
			tr.fin_s = x + duracion_min_s;
			tr.errores = 1;
			out.push_back(tr);
		}
	}
	return out;
}

std::string formatoTiempo(double s) {
	if (s < 0) s = 0;
	long total = (long)std::floor(s + 1e-6);
	long h = total / 3600, m = (total % 3600) / 60, sec = total % 60;
	char buf[32];
	if (h > 0) std::snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", h, m, sec);
	else std::snprintf(buf, sizeof(buf), "%ld:%02ld", m, sec);
	return buf;
}

double InformeVerificacion::porcentajeConErrores() const {
	if (duracion_s <= 0) return 0;
	double malo = 0;
	for (const auto& t : tramos_video) malo += std::max(t.fin_s - t.inicio_s, 0.04);
	double p = 100.0 * malo / duracion_s;
	return std::min(p, 100.0);
}

std::string InformeVerificacion::resumenEs() const {
	std::ostringstream ss;
	if (!hecho) {
		ss << "No se pudo comprobar el vídeo reparado: " << error_es;
		return ss.str();
	}
	auto tramos = [&ss](const std::vector<TramoConErrores>& v) {
		const size_t max = 8;
		for (size_t i = 0; i < v.size() && i < max; i++) {
			ss << (i ? ", " : " ") << formatoTiempo(v[i].inicio_s);
			if (formatoTiempo(v[i].fin_s) != formatoTiempo(v[i].inicio_s))
				ss << "-" << formatoTiempo(v[i].fin_s);
		}
		if (v.size() > max) ss << " y " << (v.size() - max) << " zonas más";
	};
	if (tiene_video && fotogramas == 0 && errores_video == 0) {
		ss << "Comprobación: no se pudo decodificar ningún fotograma; el vídeo "
		      "probablemente no se ve.";
		return ss.str();
	}
	if (limpio()) {
		ss << "Comprobación: se decodificó entero sin errores (" << fotogramas << " fotogramas";
		if (tiene_audio) ss << ", con sonido";
		ss << "). Debería verse bien de principio a fin.";
	} else {
		ss << "Comprobación: hay partes con errores.";
		if (errores_video > 0) {
			ss << "\n    Imagen con errores en:";
			tramos(tramos_video);
			ss << " (" << errores_video << " de " << (fotogramas + errores_video)
			   << " fotogramas; ~" << (int)std::ceil(porcentajeConErrores()) << "% del vídeo)";
		}
		if (errores_audio > 0) {
			ss << "\n    Sonido con errores en:";
			tramos(tramos_audio);
		}
		ss << "\n    El resto se decodificó bien. En esas zonas puede verse imagen "
		      "rota, gris o congelada: revísalas antes de borrar el original.";
	}
	if (truncado_en_limite)
		ss << "\n    (Solo se revisaron los primeros " << formatoTiempo(duracion_s) << ".)";
	return ss.str();
}

InformeVerificacion verificarVideo(const std::string& path, double limite_s) {
	InformeVerificacion inf;
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
	av_register_all();
#endif
	// FFmpeg escribiria cada error de decodificacion en la consola;
	// aqui se cuentan, no se imprimen.
	const int nivel_log = av_log_get_level();
	av_log_set_level(AV_LOG_QUIET);
	struct Restaurar {
		int nivel;
		~Restaurar() { av_log_set_level(nivel); }
	} restaurar{nivel_log};

	AVFormatContext* fmt = nullptr;
	// FFmpeg acepta rutas UTF-8 tambien en Windows (las convierte).
	if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
		inf.error_es = "no se pudo abrir el archivo reparado.";
		return inf;
	}
	struct Cerrar {
		AVFormatContext** f;
		~Cerrar() { avformat_close_input(f); }
	} cerrar{&fmt};
	if (avformat_find_stream_info(fmt, nullptr) < 0) {
		inf.error_es = "el archivo reparado no tiene pistas legibles.";
		return inf;
	}
	if (fmt->duration > 0) inf.duracion_s = fmt->duration / (double)AV_TIME_BASE;

	Decodificador video, audio;
	int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	int ai = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
	inf.tiene_video = vi >= 0 && video.abrir(fmt, vi);
	inf.tiene_audio = ai >= 0 && audio.abrir(fmt, ai);
	if (!inf.tiene_video && !inf.tiene_audio) {
		inf.error_es = "no hay pistas de vídeo ni de audio que se puedan decodificar.";
		return inf;
	}

	AVPacket* pkt = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	double max_t = 0;
	while (av_read_frame(fmt, pkt) >= 0) {
		Decodificador* d = nullptr;
		if (inf.tiene_video && pkt->stream_index == video.stream) d = &video;
		else if (inf.tiene_audio && pkt->stream_index == audio.stream) d = &audio;
		if (d) {
			double t = d->segundos(pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts);
			if (t > max_t) max_t = t;
			if (limite_s > 0 && t > limite_s) {
				inf.truncado_en_limite = true;
				av_packet_unref(pkt);
				break;
			}
			d->decodificar(pkt, frame, t);
		}
		av_packet_unref(pkt);
	}
	if (inf.tiene_video) video.decodificar(nullptr, frame, -1);  // vaciar
	if (inf.tiene_audio) audio.decodificar(nullptr, frame, -1);
	av_frame_free(&frame);
	av_packet_free(&pkt);

	if (inf.duracion_s <= 0 || inf.truncado_en_limite) inf.duracion_s = max_t;
	inf.fotogramas = video.buenos;
	inf.errores_video = (int)video.errores.size();
	inf.muestras_audio = audio.buenos;
	inf.errores_audio = (int)audio.errores.size();
	double fd = 0.04;
	if (inf.tiene_video && video.ctx && video.ctx->framerate.num > 0)
		fd = av_q2d(av_inv_q(video.ctx->framerate));
	inf.tramos_video = agruparErrores(video.errores, 1.0, fd);
	inf.tramos_audio = agruparErrores(audio.errores, 1.0, 0.02);
	inf.hecho = true;
	return inf;
}
