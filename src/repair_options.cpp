/*
	Untrunc-mejorado - repair_options.cpp (ver repair_options.h)

	Licencia: GNU GPL-2.0 (ver COPYING).
*/

#include "repair_options.h"

#include <atomic>
#include <mutex>

#include "common.h"
#include "mp4.h"

namespace {

std::recursive_mutex& coreMutex() {
	static std::recursive_mutex m;
	return m;
}

// Profundidad de sesiones activas (en cualquier hilo); para busy().
std::atomic<int> g_session_depth{0};

} // namespace

RepairOptions RepairOptions::fromGlobals() {
	RepairOptions o;
	o.ignore_unknown = g_ignore_unknown;
	o.step = Mp4::step_;
	o.use_chunk_stats = g_use_chunk_stats;
	o.stretch_video = g_stretch_video;
	o.dont_exclude = g_dont_exclude;
	o.rsv_ben_mode = g_rsv_ben_mode;
	o.skip_existing = g_skip_existing;
	o.search_mdat = g_search_mdat;
	o.strict_nal_frame_check = g_strict_nal_frame_check;
	o.allow_large_sample = g_allow_large_sample;
	o.ignore_forbidden_nal_bit = g_ignore_forbidden_nal_bit;
	o.dont_omit = g_dont_omit;
	o.ignore_keyframe_mismatch = g_ignore_keyframe_mismatch;
	o.skip_nal_filler_data = g_skip_nal_filler_data;
	o.ignore_out_of_bound_chunks = g_ignore_out_of_bound_chunks;
	o.no_ctts = g_no_ctts;
	o.max_partsize = g_max_partsize;
	o.max_partsize_default = g_max_partsize_default;
	o.range_start = g_range_start;
	o.range_end = g_range_end;
	o.dst_path = g_dst_path;
	o.on_progress = g_onProgress;
	return o;
}

void RepairSession::apply(const RepairOptions& o) {
	g_ignore_unknown = o.ignore_unknown;
	Mp4::step_ = o.step;
	g_use_chunk_stats = o.use_chunk_stats;
	g_stretch_video = o.stretch_video;
	g_dont_exclude = o.dont_exclude;
	g_rsv_ben_mode = o.rsv_ben_mode;
	g_skip_existing = o.skip_existing;
	g_search_mdat = o.search_mdat;
	g_strict_nal_frame_check = o.strict_nal_frame_check;
	g_allow_large_sample = o.allow_large_sample;
	g_ignore_forbidden_nal_bit = o.ignore_forbidden_nal_bit;
	g_dont_omit = o.dont_omit;
	g_ignore_keyframe_mismatch = o.ignore_keyframe_mismatch;
	g_skip_nal_filler_data = o.skip_nal_filler_data;
	g_ignore_out_of_bound_chunks = o.ignore_out_of_bound_chunks;
	g_no_ctts = o.no_ctts;
	g_max_partsize = o.max_partsize;
	g_max_partsize_default = o.max_partsize_default;
	g_range_start = o.range_start;
	g_range_end = o.range_end;
	g_dst_path = o.dst_path;
	g_onProgress = o.on_progress;
}

RepairSession::RepairSession(const RepairOptions& opts) : RepairSession(opts, false) {}

RepairSession::RepairSession(const RepairOptions& opts, bool try_only) {
	if (try_only) {
		if (!coreMutex().try_lock()) return;
	} else {
		coreMutex().lock();
	}
	active_ = true;
	g_session_depth++;
	saved_ = RepairOptions::fromGlobals();
	saved_mp4_ = g_mp4;
	apply(opts);
}

RepairSession::~RepairSession() {
	if (!active_) return;
	apply(saved_);
	g_mp4 = saved_mp4_;
	g_session_depth--;
	coreMutex().unlock();
}

bool RepairSession::busy() { return g_session_depth.load() > 0; }
