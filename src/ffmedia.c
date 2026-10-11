#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/time.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>

#ifndef __EMSCRIPTEN__
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#endif

#include <SDL2/SDL.h>

#include "ffmedia.h"
#include <SDL2/SDL_thread.h>

#include <stdlib.h>
#include <math.h>

#ifndef _WIN32
#define USE_POSIX_MEMALIGN
#endif

/* Should a mono channel be split into two equal stero channels (true) or
 * should the energy be split onto two stereo channels with 1/2 the energy
 * (false).
 */
static int audio_equal_mono = 1;

/* The weight of stereo channels when audio_equal_mono is true. */
static double stereo_matrix[] = { 1.0, 1.0 };

/* The output audio sample rate. */
static int audio_sample_rate = 44100;

static int audio_sample_increase = 44100;
static int audio_target_samples = 44100 * 2;

const int CHANNELS = 2;
const int BPC = 2; // Bytes per channel.
const int BPS = 4; // Bytes per sample.

const int FRAMES = 10;

// The alignment of each row of pixels.
const int ROW_ALIGNMENT = 16;

// The number of pixels on each side. This has to be greater that 0 (since
// Ren'Py needs some padding), FRAME_PADDING * BPS has to be a multiple of
// 16 (alignment issues on ARM NEON), and has to match the crop in the
// read_video function of renpysound.pyx.
const int FRAME_PADDING = ROW_ALIGNMENT / 4;

const int SPEED = 1;

// How many seconds early can frames be delivered?
static const double frame_early_delivery = .005;

static SDL_Surface *rgb_surface = NULL;
static SDL_Surface *rgba_surface = NULL;

// http://dranger.com/ffmpeg/

/*******************************************************************************
 * SDL_RWops <-> AVIOContext
 */

static int rwops_read(void *opaque, uint8_t *buf, int buf_size) {
    SDL_RWops *rw = (SDL_RWops *) opaque;

    int rv = rw->read(rw, buf, 1, buf_size);

	if (rv == 0) {
		return AVERROR_EOF;
	}

    return rv;

}

static int rwops_write(void *opaque, const uint8_t *buf, int buf_size) {
    printf("Writing to an SDL_rwops is a really bad idea.\n");
    return -1;
}

static int64_t rwops_seek(void *opaque, int64_t offset, int whence) {
    SDL_RWops *rw = (SDL_RWops *) opaque;

    if (whence == AVSEEK_SIZE) {
    	return rw->size(rw);
    }

    // Ignore flags like AVSEEK_FORCE.
    whence &= (SEEK_SET | SEEK_CUR | SEEK_END);

    int64_t rv = rw->seek(rw, (int) offset, whence);
    return rv;
}

#define RWOPS_BUFFER 65536


static AVIOContext *rwops_open(SDL_RWops *rw) {

    unsigned char *buffer = av_malloc(RWOPS_BUFFER);
	if (buffer == NULL) {
		return NULL;
	}
    AVIOContext *rv = avio_alloc_context(
        buffer,
        RWOPS_BUFFER,
        0,
        rw,
        rwops_read,
        rwops_write,
        rwops_seek);
    if (rv == NULL) {
    	av_free(buffer);
    	return NULL;
    }

    return rv;
}

static void rwops_close(SDL_RWops *rw) {
	rw->close(rw);
}

/* Sample the same monotonic clock for scheduling, position and rate changes.
 * A cached frame time is insufficient when the mixer advances between frames. */
static double media_clock_now(void) {
    return SPEED * av_gettime_relative() * 1e-6;
}

typedef struct PacketQueueEntry {
	AVPacket *pkt;
	struct PacketQueueEntry *next;
} PacketQueueEntry;

typedef struct PacketQueue {
	PacketQueueEntry *first;
	PacketQueueEntry *last;
} PacketQueue;

typedef struct FrameQueue {
	AVFrame *first;
	AVFrame *last;
} FrameQueue;


typedef struct SurfaceQueueEntry {
	struct SurfaceQueueEntry *next;

	SDL_Surface *surf;

	/* Packed YUV420 data for GPU conversion. */
	int yuv;
	int yuv_width, yuv_height;
	int yuv_y_pitch, yuv_uv_pitch;
	uint8_t *yuv_y;
	uint8_t *yuv_uv;
	int yuv_full_range;
	int yuv_bt709;

	/* The pts, converted to seconds. */
	double pts;

	/* The format. This is not refcounted, but it's kept alive by being
	 * the format of one of the sampel surfaces.
	 */
	SDL_PixelFormat *format;

	/* As with SDL_Surface. */
	int w, h, pitch;
	void *pixels;

} SurfaceQueueEntry;

typedef struct MediaState {

    /* The next entry in a list of MediaStates */
    struct MediaState *next;

    /* The thread associated with decoding this media. */
    SDL_Thread *thread;

	/* The condition and lock. */
	SDL_cond* cond;
	SDL_mutex* lock;

	SDL_RWops *rwops;
	char *filename;

	/*
	 * True if we this stream should have video.
	 */
	int want_video;

	/* This becomes true once the decode thread has finished initializing
	 * and the readers and writers can do their thing.
	 */
	int ready;

	/* This is set to true when data has been read, in order to ask the
	 * decode thread to produce more data.
	 */
	int needs_decode; // Lock.

	/*
	 * This is set to true when data has been read, in order to ask the
	 * decode thread to shut down and deallocate all resources.
	 */
	int quit; // Lock

	/* The number of seconds to skip at the start. */
	double skip;

	/* These become true when the audio and video finish. */
	int audio_finished;
	int video_finished;

	/* Runtime seek request. Set by media_seek() from another thread,
	 * processed by the decode_thread loop.  When seek_req is non-zero,
	 * the decoder flushes its queues and jumps to seek_target (seconds). */
	int seek_req;
	int seeking;
	double seek_target;

	/* Indexes of video and audio streams. */
	int video_stream;
	int audio_stream;

	/* The main context. */
	AVFormatContext *ctx;

	/* Contexts for decoding audio and video streams. */
	AVCodecContext *video_context;
	AVCodecContext *audio_context;

	/* Queues of packets going to the audio and video streams. */
	PacketQueue video_packet_queue;
	PacketQueue audio_packet_queue;

	/* The total duration of the video. Only used for information purposes. */
	double total_duration;

	/* Audio Stuff ***********************************************************/

	/* The queue of converted audio frames. */
	FrameQueue audio_queue; // Lock

	/* The size of the audio queue, and the target size in seconds. */
	int audio_queue_samples;
	int audio_queue_target_samples;

	/* A frame used for decoding. */
	AVFrame *audio_decode_frame;

	/* The audio frame being read from, and the index into the audio frame. */
	AVFrame *audio_out_frame; // Lock
	int audio_out_index; // Lock

	SwrContext *swr;

	/* The duration of the audio stream, in samples.
	 * -1 means to play until we run out of data.
	 */
	int audio_duration;

	/* The number of samples that have been read so far. */
	int audio_read_samples; // Lock

    /* Immutable playback parameters. All times are in the source file. */
    double playback_speed;
    double audio_end;

#ifndef __EMSCRIPTEN__
    /* Owned by the decode thread. Output frames use the device sample rate. */
    AVFilterGraph *tempo_graph;
    AVFilterContext *tempo_source;
    AVFilterContext *tempo_sink;
    int tempo_eof;
    int tempo_resampler_drained;
    int64_t tempo_input_samples;
    int64_t tempo_output_samples;
    int64_t tempo_output_limit;
    double tempo_decode_position;
#endif

	/* A frame that video is decoded into. */
	AVFrame *video_decode_frame;

	/* Hardware device context for GPU-accelerated decoding. */
	AVBufferRef *hw_device_ctx;

	/* Temporary frame for GPU→CPU transfer of hardware-decoded frames. */
	AVFrame *hw_frame;

	/* Video Stuff ***********************************************************/

	/* Software rescaling context. */
	struct SwsContext *sws;

	/* Software rescaling context for surface consumers of YUV frames. */
	struct SwsContext *yuv_sws;

	/* A queue of decoded video frames. */
	SurfaceQueueEntry *surface_queue; // Lock
	int surface_queue_size; // Lock

    /* Source-time clock. All fields are protected by lock. Queued frames
     * always retain their original source PTS, including across rate changes. */
    double video_clock_anchor;
    double video_time_anchor;
    double video_speed;
    int video_clock_started;
    int video_clock_ended;
    int video_clock_only;
    int stream_info_ready;
    int video_speed_requested;
    int video_readers;

	/* Are frame drops allowed? */
	int frame_drops;

	/* The time the pause happened, or 0 if we're not paused. */
	double pause_time;

} MediaState;

/* The following helpers require ms->lock. */
static double video_end_time(MediaState *ms) {
    if (ms->audio_end >= 0) return ms->audio_end;
    if (ms->stream_info_ready && ms->total_duration > 0) return ms->total_duration;
    return -1;
}

static double video_time(MediaState *ms, double now) {
    double position = ms->video_time_anchor;
    if (ms->video_clock_started && !ms->video_clock_ended && !ms->pause_time && !ms->seek_req && !ms->seeking) {
        position += fmax(0, now - ms->video_clock_anchor) * ms->video_speed;
    }
    double end = video_end_time(ms);
    if (end >= 0) position = fmin(position, end);
    return position;
}

static void video_start_clock(MediaState *ms, double now) {
    if (!ms->video_clock_started && !ms->pause_time && !ms->seek_req && !ms->seeking) {
        ms->video_clock_anchor = now;
        ms->video_clock_started = 1;
    }
}

static int video_clock_finished(MediaState *ms, double now) {
    if (ms->video_clock_ended) return 1;
    if (!ms->video_clock_started || ms->seek_req || ms->seeking) return 0;
    double position = video_time(ms, now);
    double end = video_end_time(ms);
    if ((end >= 0 && position >= end) || (end < 0 && ms->video_finished && !ms->surface_queue_size)) {
        ms->video_time_anchor = position;
        ms->video_clock_anchor = now;
        ms->video_clock_ended = 1;
        return 1;
    }
    return 0;
}

/* Return -1 for streams that use the audio clock, including unopened streams. */
double media_video_position(MediaState *ms) {
    SDL_LockMutex(ms->lock);
    double rv = ms->video_clock_only ? video_time(ms, media_clock_now()) : -1;
    if (ms->video_clock_only && ms->seek_req) rv = ms->seek_target;
    SDL_UnlockMutex(ms->lock);
    return rv;
}

/* Pending requests may precede stream probing. They change only the video
 * rate, never the immutable audio tempo. An incompatible stream is reported
 * when its metadata is available (also checked by the mixer periodic call). */
static int video_speed_supported(MediaState *ms) {
    return !ms || (ms->want_video && (!ms->stream_info_ready || ms->video_clock_only));
}

int media_video_speed_error(MediaState *ms) {
    if (!ms) return 0;
    SDL_LockMutex(ms->lock);
    int error = ms->video_speed_requested && !video_speed_supported(ms);
    if (error) ms->video_speed_requested = 0;
    SDL_UnlockMutex(ms->lock);
    return error;
}

static void video_set_speed(MediaState *ms, double speed, double now) {
    if (!ms) return;
    ms->video_speed_requested = 1;
    if (ms->video_speed == speed) return;
    ms->video_time_anchor = video_time(ms, now);
    ms->video_clock_anchor = now;
    ms->video_speed = speed;
}

/* The mixer holds its audio lock so playing/queued cannot be replaced.
 * Lock both streams before validating or updating either of them. */
int media_set_video_speed(MediaState *playing, MediaState *queued, double speed) {
    if (playing) SDL_LockMutex(playing->lock);
    if (queued) SDL_LockMutex(queued->lock);
    int supported = isfinite(speed) && speed >= 0.5 && speed <= 3.0 &&
        video_speed_supported(playing) && video_speed_supported(queued);
    if (supported) {
        double now = media_clock_now();
        video_set_speed(playing, speed, now);
        video_set_speed(queued, speed, now);
    }
    if (queued) SDL_UnlockMutex(queued->lock);
    if (playing) SDL_UnlockMutex(playing->lock);
    return supported;
}

static void media_publish_stream_info(MediaState *ms) {
    SDL_LockMutex(ms->lock);
    ms->video_clock_only = ms->video_context && ms->audio_stream == -1;
    ms->stream_info_ready = 1;
    /* An early runtime request cannot change an audio-bearing movie's rate. */
    if (!ms->video_clock_only) ms->video_speed = ms->playback_speed;
    SDL_UnlockMutex(ms->lock);
}

/* Pin the stream while a renderer uses its conversion context. The mixer
 * lock is needed only to acquire the pin, never during RGB conversion. */
void media_acquire_video(MediaState *ms) {
    SDL_LockMutex(ms->lock);
    ms->video_readers++;
    SDL_UnlockMutex(ms->lock);
}

void media_release_video(MediaState *ms) {
    SDL_LockMutex(ms->lock);
    ms->video_readers--;
    if (ms->quit && !ms->video_readers) SDL_CondBroadcast(ms->cond);
    SDL_UnlockMutex(ms->lock);
}

static AVFrame *dequeue_frame(FrameQueue *fq);
static void free_packet_queue(PacketQueue *pq);
static SurfaceQueueEntry *dequeue_surface(SurfaceQueueEntry **queue);

static void free_surface_entry(SurfaceQueueEntry *sqe) {
	if (!sqe) {
		return;
	}

	if (sqe->pixels) {
#ifndef USE_POSIX_MEMALIGN
		SDL_free(sqe->pixels);
#else
		free(sqe->pixels);
#endif
	}

	av_free(sqe->yuv_y);
	av_free(sqe->yuv_uv);
	av_free(sqe);
}


/* A queue of MediaState objects that are awaiting deallocation.*/
static MediaState *deallocate_queue = NULL;

/* A mutex that discards deallocate_queue. */
SDL_mutex *deallocate_mutex = NULL;

/* Deallocates a single MediaState. */
static void deallocate(MediaState *ms) {

    while (1) {
		SurfaceQueueEntry *sqe = dequeue_surface(&ms->surface_queue);

		if (! sqe) {
			break;
		}

		free_surface_entry(sqe);
	}

	if (ms->hw_frame) {
		av_frame_free(&ms->hw_frame);
	}
	if (ms->hw_device_ctx) {
		av_buffer_unref(&ms->hw_device_ctx);
	}

	if (ms->sws) {
		sws_freeContext(ms->sws);
	}

	if (ms->yuv_sws) {
		sws_freeContext(ms->yuv_sws);
	}

	if (ms->video_decode_frame) {
		av_frame_free(&ms->video_decode_frame);
	}

	/* Destroy audio stuff. */
#ifndef __EMSCRIPTEN__
    avfilter_graph_free(&ms->tempo_graph);
#endif
	if (ms->swr) {
		swr_free(&ms->swr);
	}

	if (ms->audio_decode_frame) {
		av_frame_free(&ms->audio_decode_frame);
	}

	if (ms->audio_out_frame) {
		av_frame_free(&ms->audio_out_frame);
	}

	while (1) {
		AVFrame *f = dequeue_frame(&ms->audio_queue);

		if (!f) {
			break;
		}

		av_frame_free(&f);
	}

	/* Destroy/Close core stuff. */
	free_packet_queue(&ms->audio_packet_queue);
	free_packet_queue(&ms->video_packet_queue);

	if (ms->video_context) {
		avcodec_free_context(&ms->video_context);
	}
	if (ms->audio_context) {
		avcodec_free_context(&ms->audio_context);
	}

	if (ms->ctx) {

		if (ms->ctx->pb) {
			if (ms->ctx->pb->buffer) {
				av_freep(&ms->ctx->pb->buffer);
			}
			av_freep(&ms->ctx->pb);
		}

		avformat_close_input(&ms->ctx);
	}

	/* Destroy alloc stuff. */
	if (ms->cond) {
		SDL_DestroyCond(ms->cond);
	}
	if (ms->lock) {
		SDL_DestroyMutex(ms->lock);
	}

	if (ms->rwops) {
		rwops_close(ms->rwops);
	}

	if (ms->filename) {
		av_free(ms->filename);
	}

	/* Add this MediaState to a queue to have its thread ended, and the MediaState
	 * deactivated.
	 */
	SDL_LockMutex(deallocate_mutex);
    ms->next = deallocate_queue;
    deallocate_queue = ms;
    SDL_UnlockMutex(deallocate_mutex);

}

/* Perform the portion of deallocation that's been deferred to the main thread. */
static void deallocate_deferred() {

    SDL_LockMutex(deallocate_mutex);

    while (deallocate_queue) {
        MediaState *ms = deallocate_queue;
        deallocate_queue = ms->next;

        if (ms->thread) {
            SDL_WaitThread(ms->thread, NULL);
        }

        av_free(ms);
    }

    SDL_UnlockMutex(deallocate_mutex);
}

/* Frame queue ***************************************************************/

static void enqueue_frame(FrameQueue *fq, AVFrame *frame) {
	frame->opaque = NULL;

	if (fq->first) {
		fq->last->opaque = frame;
		fq->last = frame;
	} else {
		fq->first = fq->last = frame;
	}
}

static AVFrame *dequeue_frame(FrameQueue *fq) {
	if (!fq->first) {
		return NULL;
	}

	AVFrame *rv = fq->first;
	fq->first = (AVFrame *) rv->opaque;

	if (!fq->first) {
		fq->last = NULL;
	}

	return rv;
}


/* Packet queue **************************************************************/

static void enqueue_packet(PacketQueue *pq, AVPacket *pkt) {
	PacketQueueEntry *pqe = av_malloc(sizeof(PacketQueueEntry));
	if (pqe == NULL) {
		av_packet_free(&pkt);
		return;
	}

	pqe->pkt = pkt;
	pqe->next = NULL;

	if (!pq->first) {
		pq->first = pq->last = pqe;
	} else {
		pq->last->next = pqe;
		pq->last = pqe;
	}
}

static AVPacket *first_packet(PacketQueue *pq) {
	if (pq->first) {
		return pq->first->pkt;
	} else {
		return NULL;
	}
}

static void dequeue_packet(PacketQueue *pq) {
	if (! pq->first) {
		return;
	}

	PacketQueueEntry *pqe = pq->first;
	pq->first = pqe->next;

	if (!pq->first) {
		pq->last = NULL;
	}

	av_packet_free(&pqe->pkt);
	av_free(pqe);
}

static int count_packet_queue(PacketQueue *pq) {
    PacketQueueEntry *pqe = pq->first;

	int rv = 0;

	while (pqe) {
		rv += 1;
		pqe = pqe->next;
	}

	return rv;
}

static void free_packet_queue(PacketQueue *pq) {
	while(first_packet(pq)) {
		dequeue_packet(pq);
	}
}


/**
 * Reads a packet from one of the queues, filling the other queue if
 * necessary. Returns the packet, or NULL if end of file has been reached.
 */
static AVPacket *read_packet(MediaState *ms, PacketQueue *pq) {

	AVPacket *pkt;
	AVPacket *rv;

	while (1) {

		rv = first_packet(pq);
		if (rv) {
			return rv;
		}

		pkt = av_packet_alloc();

		if (!pkt) {
			return NULL;
		}

		if (av_read_frame(ms->ctx, pkt)) {
			av_packet_free(&pkt);
			return NULL;
		}

		if (pkt->stream_index == ms->video_stream && ! ms->video_finished) {
			enqueue_packet(&ms->video_packet_queue, pkt);
		} else if (pkt->stream_index == ms->audio_stream && ! ms->audio_finished) {
			enqueue_packet(&ms->audio_packet_queue, pkt);
		} else {
			av_packet_free(&pkt);
		}
	}
}


/* Surface queue *************************************************************/

static void enqueue_surface(SurfaceQueueEntry **queue, SurfaceQueueEntry *sqe) {
	while (*queue) {
		queue = &(*queue)->next;
	}

	*queue = sqe;
}


static SurfaceQueueEntry *dequeue_surface(SurfaceQueueEntry **queue) {
	SurfaceQueueEntry *rv = *queue;

	if (rv) {
		*queue = rv->next;
	}

	return rv;
}


#if 0
static void check_surface_queue(MediaState *ms) {

	SurfaceQueueEntry **queue = &ms->surface_queue;

	int count = 0;

	while (*queue) {
		count += 1;
		queue = &(*queue)->next;
	}

	if (count != ms->surface_queue_size) {
		abort();
	}

}
#endif

/* Find decoder context ******************************************************/

/* Custom get_format callback that rejects all HW pixel formats.
 * Used in software fallback path to prevent internal hwaccel from activating. */
static enum AVPixelFormat sw_get_format(struct AVCodecContext *s, const enum AVPixelFormat *pix_fmts) {
	for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
		const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
		if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
			return *p;
	}
	return AV_PIX_FMT_NONE;
}

/* Custom get_format for hardware decode. Prefers AV_PIX_FMT_D3D11
 * (the modern D3D11VA2 path) over AV_PIX_FMT_D3D11VA_VLD (the legacy
 * AD_HOC path that does not properly create a decoder). */
static enum AVPixelFormat hw_get_format(struct AVCodecContext *s, const enum AVPixelFormat *pix_fmts) {
	/* Prefer AV_PIX_FMT_D3D11 (modern D3D11VA2, pix_fmt 171). */
	for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == AV_PIX_FMT_D3D11)
			return *p;
	}
	return pix_fmts[0];
}

static enum AVHWDeviceType get_preferred_hw_device_type(void) {
#ifdef _WIN32
    return AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(__ANDROID__)
    return AV_HWDEVICE_TYPE_MEDIACODEC;
#else
    return AV_HWDEVICE_TYPE_NONE;
#endif
}

#ifdef __ANDROID__
#include <jni.h>
#include <SDL2/SDL_system.h>
#include <libavcodec/jni.h>

static int init_android_jni(void) {
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    if (!env) return -1;

    JavaVM *jvm = NULL;
    (*env)->GetJavaVM(env, &jvm);
    if (!jvm) return -1;

    return av_jni_set_java_vm(jvm, NULL);
}
#endif

static AVCodecContext *find_context(
    AVFormatContext *ctx, int index,
    AVBufferRef **hw_device_ctx_out
) {

    AVDictionary *opts = NULL;

	if (index == -1) {
		return NULL;
	}

	AVCodecContext *codec_ctx = avcodec_alloc_context3(NULL);

	if (codec_ctx == NULL) {
		return NULL;
	}

	if (avcodec_parameters_to_context(codec_ctx, ctx->streams[index]->codecpar) < 0) {
		goto fail;
	}

	codec_ctx->pkt_timebase = ctx->streams[index]->time_base;

	const AVCodec *codec = avcodec_find_decoder(codec_ctx->codec_id);

	if (codec == NULL) {
		goto fail;
	}

	codec_ctx->codec_id = codec->id;

	av_dict_set(&opts, "threads", "auto", 0);

	// Attempt hardware decoding.
	// We skip avcodec_get_hw_config() pre-check because some codecs
	// (e.g. AV1) register D3D11VA hwaccel via the internal AVHWAccel
	// mechanism, not through hw_configs. Just try av_hwdevice_ctx_create
	// and let avcodec_open2 be the gate.
	//
	// AV1 hardware decode via D3D11VA is skipped because the GPU→CPU
	// av_hwframe_transfer_data per frame is a blocking PCIe read that
	// is slower than software decode (180 MB/s for 1080p 10-bit).
	if (hw_device_ctx_out && codec_ctx->codec_id != AV_CODEC_ID_AV1) {
		enum AVHWDeviceType hw_type = get_preferred_hw_device_type();
		av_log(NULL, AV_LOG_ERROR, "[ffmedia] find_context: hw_type=%d codec=%s\n", hw_type, avcodec_get_name(codec_ctx->codec_id));

		if (hw_type != AV_HWDEVICE_TYPE_NONE) {
#ifdef __ANDROID__
			if (hw_type == AV_HWDEVICE_TYPE_MEDIACODEC) {
				init_android_jni();
			}
#endif
			int ret = av_hwdevice_ctx_create(hw_device_ctx_out, hw_type, NULL, NULL, 0);
			if (ret >= 0) {
				codec_ctx->hw_device_ctx = av_buffer_ref(*hw_device_ctx_out);

				// Set get_format to prefer AV_PIX_FMT_D3D11
				// (D3D11VA2) over AV_PIX_FMT_D3D11VA_VLD (legacy
				// AD_HOC D3D11VA which doesn't create a decoder).
				codec_ctx->get_format = hw_get_format;

				if (avcodec_open2(codec_ctx, codec, &opts) >= 0) {
					av_log(NULL, AV_LOG_ERROR, "[ffmedia] using hardware decode for codec=%s\n", avcodec_get_name(codec_ctx->codec_id));
					av_dict_free(&opts);
					return codec_ctx;
				}

				// HW open failed, clean up and fall back to software.
				av_log(NULL, AV_LOG_ERROR, "[ffmedia] HW open failed, falling back to SW for codec=%s\n", avcodec_get_name(codec_ctx->codec_id));
				av_buffer_unref(&codec_ctx->hw_device_ctx);
				av_buffer_unref(hw_device_ctx_out);
				*hw_device_ctx_out = NULL;
			}
		}
	}

	// Software decode fallback.
	// Must call avcodec_open2 BEFORE setting get_format because some
	// codecs (e.g. AV1) override get_format in their init function.
	av_log(NULL, AV_LOG_ERROR, "[ffmedia] using software decode for codec=%s\n", avcodec_get_name(codec_ctx->codec_id));
	if (avcodec_open2(codec_ctx, codec, &opts) < 0) {
		goto fail;
	}
	codec_ctx->get_format = sw_get_format;

	av_dict_free(&opts);
	return codec_ctx;

fail:

    av_dict_free(&opts);

	avcodec_free_context(&codec_ctx);
	return NULL;
}


static int seek_media(MediaState *ms, double position) {
	double seek_position = position;

	if (ms->audio_stream >= 0) {
		AVCodecParameters *parameters = ms->ctx->streams[ms->audio_stream]->codecpar;
		if (parameters->codec_id == AV_CODEC_ID_OPUS) {
			/* The 80 ms minimum does not fully converge on noisy audio. */
			double preroll = 0.25;
			if (parameters->sample_rate > 0) {
				preroll = fmax(preroll, 1.0 * parameters->seek_preroll / parameters->sample_rate);
			}
			seek_position = fmax(0.0, position - preroll);
		}
	}

	int ret = av_seek_frame(ms->ctx, -1, (int64_t) (seek_position * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
	if (ret < 0) {
		av_log(ms->ctx, AV_LOG_ERROR, "Could not seek media to %.6f seconds (%d).\n", position, ret);
	}
	return ret;
}


/* Audio decoding *************************************************************/

#ifndef __EMSCRIPTEN__

/* Time stretching produces a different number of samples from its input.
 * Keep it on the decoder thread, ahead of the fixed-size mixer filters. */
static int tempo_prepare(MediaState *ms) {
    int ret;
    char args[128];
    AVFilterContext *previous;

    ms->tempo_graph = avfilter_graph_alloc();
    if (!ms->tempo_graph) return AVERROR(ENOMEM);
    ms->tempo_graph->nb_threads = 1;

    snprintf(args, sizeof(args),
        "time_base=1/%d:sample_rate=%d:sample_fmt=s16:channel_layout=stereo",
        audio_sample_rate, audio_sample_rate);
    ret = avfilter_graph_create_filter(&ms->tempo_source,
        avfilter_get_by_name("abuffer"), "voice_input", args, NULL, ms->tempo_graph);
    if (ret < 0) return ret;
    previous = ms->tempo_source;

    int stages = ms->playback_speed > 2.0 ? 2 : 1;
    double factor = stages == 2 ? sqrt(ms->playback_speed) : ms->playback_speed;
    const AVFilter *filter = avfilter_get_by_name("atempo");
    if (!filter) return AVERROR_FILTER_NOT_FOUND;

    for (int i = 0; i < stages; i++) {
        AVFilterContext *tempo;
        snprintf(args, sizeof(args), "voice_tempo_%d", i);
        ret = avfilter_graph_create_filter(&tempo, filter, args, NULL, NULL, ms->tempo_graph);
        if (ret < 0) return ret;
        ret = av_opt_set_double(tempo->priv, "tempo", factor, 0);
        if (ret < 0) return ret;
        ret = avfilter_link(previous, 0, tempo, 0);
        if (ret < 0) return ret;
        previous = tempo;
    }

    ret = avfilter_graph_create_filter(&ms->tempo_sink,
        avfilter_get_by_name("abuffersink"), "voice_output", NULL, NULL, ms->tempo_graph);
    if (ret < 0) return ret;
    ret = avfilter_link(previous, 0, ms->tempo_sink, 0);
    if (ret < 0) return ret;
    return avfilter_graph_config(ms->tempo_graph, NULL);
}

static int tempo_drain(MediaState *ms) {
    while (1) {
        AVFrame *frame = av_frame_alloc();
        if (!frame) return AVERROR(ENOMEM);
        int ret = av_buffersink_get_frame(ms->tempo_sink, frame);
        if (ret < 0) {
            av_frame_free(&frame);
            return (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ? 0 : ret;
        }

        SDL_LockMutex(ms->lock);
        if (ms->tempo_output_limit >= 0) {
            int64_t remaining = ms->tempo_output_limit - ms->tempo_output_samples;
            if (frame->nb_samples > remaining) frame->nb_samples = (int) remaining;
        }
        if (ms->seek_req || ms->quit || !frame->nb_samples) {
            av_frame_free(&frame);
        } else {
            ms->tempo_output_samples += frame->nb_samples;
            ms->audio_queue_samples += frame->nb_samples;
            enqueue_frame(&ms->audio_queue, frame);
        }
        SDL_UnlockMutex(ms->lock);
    }
}

static int tempo_feed(MediaState *ms, AVFrame *frame) {
    if (!ms->tempo_graph) {
        int ret = tempo_prepare(ms);
        if (ret < 0) return ret;
    }
    if (frame) {
        frame->pts = ms->tempo_input_samples;
        SDL_LockMutex(ms->lock);
        ms->tempo_input_samples += frame->nb_samples;
        SDL_UnlockMutex(ms->lock);
    }
    int ret = av_buffersrc_add_frame_flags(ms->tempo_source, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    if (ret < 0) return ret;
    return tempo_drain(ms);
}

static AVFrame *tempo_frame(void) {
    AVFrame *frame = av_frame_alloc();
    if (frame) {
        frame->sample_rate = audio_sample_rate;
        frame->ch_layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_STEREO;
        frame->format = AV_SAMPLE_FMT_S16;
    }
    return frame;
}

/* Crop BEFORE stretching: both script offsets and seek targets are source
 * times. A partially skipped frame is never exposed directly to the mixer. */
static int tempo_convert(MediaState *ms, AVFrame *input) {
    AVFrame *frame = tempo_frame();
    if (!frame) return AVERROR(ENOMEM);
    int ret;

    if (input && input->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_default(&input->ch_layout, input->ch_layout.nb_channels);
    }
    if (input && !swr_is_initialized(ms->swr)) {
        ret = swr_alloc_set_opts2(&ms->swr, &frame->ch_layout, frame->format, frame->sample_rate,
            &input->ch_layout, input->format, input->sample_rate, 0, NULL);
        if (ret < 0) goto done;
        if (audio_equal_mono && input->ch_layout.nb_channels == 1) {
            ret = swr_set_matrix(ms->swr, stereo_matrix, 1);
            if (ret < 0) goto done;
        }
    }

    double start = ms->tempo_decode_position;
    if (input && input->best_effort_timestamp != AV_NOPTS_VALUE) {
        start = input->best_effort_timestamp * av_q2d(ms->ctx->streams[ms->audio_stream]->time_base);
        /* Account for samples still buffered in the resampler. */
        start -= (double) swr_get_delay(ms->swr, audio_sample_rate) / audio_sample_rate;
    }
    ret = swr_convert_frame(ms->swr, frame, input);
    if (ret < 0) goto done;
    ms->tempo_decode_position = start + (double) frame->nb_samples / audio_sample_rate;

    int first = (int) fmin(frame->nb_samples, fmax(0, llround((ms->skip - start) * audio_sample_rate)));
    int last = frame->nb_samples;
    if (ms->audio_end >= 0) {
        last = (int) fmin(last, fmax(0, llround((ms->audio_end - start) * audio_sample_rate)));
        if (ms->tempo_decode_position >= ms->audio_end) ms->tempo_eof = 1;
    }

    if (last > first) {
        ret = av_frame_make_writable(frame);
        if (ret < 0) goto done;
        if (first) memmove(frame->data[0], frame->data[0] + first * BPS, (last - first) * BPS);
        frame->nb_samples = last - first;
        ret = tempo_feed(ms, frame);
    }

done:
    av_frame_free(&frame);
    return ret;
}

static void decode_audio_tempo(MediaState *ms) {
    int ret = 0;
    if (!ms->audio_decode_frame) ms->audio_decode_frame = av_frame_alloc();
    if (!ms->audio_decode_frame) {
        ret = AVERROR(ENOMEM);
        goto finished;
    }
    if (!ms->audio_context) ms->tempo_eof = 1;

    while (1) {
        SDL_LockMutex(ms->lock);
        int full = ms->audio_queue_samples >= audio_target_samples;
        int interrupted = ms->seek_req || ms->quit;
        SDL_UnlockMutex(ms->lock);
        if (full || interrupted) return;

        if (ms->tempo_eof) {
            if (!ms->tempo_resampler_drained) {
                ms->tempo_resampler_drained = 1;
                if (swr_is_initialized(ms->swr)) {
                    ret = tempo_convert(ms, NULL);
                    if (ret < 0) goto finished;
                }
            }

            /* Explicit <to> / <silence> padding is source audio and must
             * pass through the stretcher as well. Never pad natural EOF
             * to the container's (possibly estimated) original duration. */
            int64_t remaining = ms->audio_end < 0 ? 0 :
                llround(fmax(0, ms->audio_end - ms->skip) * audio_sample_rate) - ms->tempo_input_samples;
            if (remaining > 0) {
                AVFrame *silence = tempo_frame();
                if (!silence) { ret = AVERROR(ENOMEM); goto finished; }
                silence->nb_samples = (int) fmin(remaining, 4096);
                ret = av_frame_get_buffer(silence, 0);
                if (ret >= 0) {
                    memset(silence->data[0], 0, silence->nb_samples * BPS);
                    ret = tempo_feed(ms, silence);
                }
                av_frame_free(&silence);
                if (ret < 0) goto finished;
                continue;
            }

            if (ms->tempo_graph) {
                /* atempo cannot flush an incomplete first analysis window.
                 * Give very short clips lookahead, then discard the synthetic
                 * tail. Padding is not counted as source audio or progress. */
                if (!ms->tempo_output_samples && ms->tempo_input_samples) {
                    ms->tempo_output_limit = llround(ms->tempo_input_samples / ms->playback_speed);
                    if (!ms->tempo_output_limit) ms->tempo_output_limit = 1;
                    AVFrame *padding = tempo_frame();
                    if (!padding) { ret = AVERROR(ENOMEM); goto finished; }
                    padding->pts = ms->tempo_input_samples;
                    padding->nb_samples = audio_sample_rate / 4;
                    ret = av_frame_get_buffer(padding, 0);
                    if (ret >= 0) {
                        memset(padding->data[0], 0, padding->nb_samples * BPS);
                        ret = av_buffersrc_add_frame_flags(ms->tempo_source, padding, AV_BUFFERSRC_FLAG_KEEP_REF);
                    }
                    av_frame_free(&padding);
                    if (ret < 0) goto finished;
                    ret = tempo_drain(ms);
                    if (ret < 0) goto finished;
                }
                ret = tempo_feed(ms, NULL);
            }
            goto finished;
        }

        ret = avcodec_receive_frame(ms->audio_context, ms->audio_decode_frame);
        if (ret == AVERROR_EOF) {
            ms->tempo_eof = 1;
            continue;
        }
        if (ret == AVERROR(EAGAIN)) {
            AVPacket *pkt = read_packet(ms, &ms->audio_packet_queue);
            ret = avcodec_send_packet(ms->audio_context, pkt);
            if (ret == 0) dequeue_packet(&ms->audio_packet_queue);
            else if (ret != AVERROR(EAGAIN)) goto finished;
            continue;
        }
        if (ret < 0) goto finished;

        ret = tempo_convert(ms, ms->audio_decode_frame);
        av_frame_unref(ms->audio_decode_frame);
        if (ret < 0) goto finished;
    }

finished:
    if (ret < 0) {
        char message[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, message, sizeof(message));
        av_log(NULL, AV_LOG_ERROR, "[ffmedia] voice tempo (%s): %s\n", ms->filename, message);
    }
    SDL_LockMutex(ms->lock);
    ms->audio_finished = 1;
    SDL_UnlockMutex(ms->lock);
}
#endif

static void decode_audio(MediaState *ms) {
    if (ms->video_clock_only) {
        SDL_LockMutex(ms->lock);
        ms->audio_finished = 1;
        SDL_UnlockMutex(ms->lock);
        return;
    }
#ifndef __EMSCRIPTEN__
    if (ms->playback_speed != 1.0) {
        decode_audio_tempo(ms);
        return;
    }
#endif
	int ret;
	AVPacket *pkt;
	AVFrame *converted_frame;

	if (!ms->audio_context) {
		ms->audio_finished = 1;
		return;
	}

	if (ms->audio_decode_frame == NULL) {
		ms->audio_decode_frame = av_frame_alloc();
	}

	if (ms->audio_decode_frame == NULL) {
		ms->audio_finished = 1;
		return;
	}

	double timebase = av_q2d(ms->ctx->streams[ms->audio_stream]->time_base);

	if (ms->audio_queue_target_samples < audio_target_samples) {
	    ms->audio_queue_target_samples += audio_sample_increase;
	}

	while (ms->audio_queue_samples < ms->audio_queue_target_samples) {

		/** Read a packet, and send it to the decoder. */
		pkt = read_packet(ms, &ms->audio_packet_queue);
		ret = avcodec_send_packet(ms->audio_context, pkt);

		if (ret == 0) {
			dequeue_packet(&ms->audio_packet_queue);
		} else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			// pass
		} else {
			ms->audio_finished = 1;
			return;
		}

		while (1) {

			ret = avcodec_receive_frame(ms->audio_context, ms->audio_decode_frame);

			// More input is needed.
			if (ret == AVERROR(EAGAIN)) {
				break;
			}

			if (ret < 0) {
				ms->audio_finished = 1;
				return;
			}

            converted_frame = av_frame_alloc();

			if (converted_frame == NULL) {
				ms->audio_finished = 1;
				return;
			}

            converted_frame->sample_rate = audio_sample_rate;
            converted_frame->ch_layout = (AVChannelLayout) AV_CHANNEL_LAYOUT_STEREO;
            converted_frame->format = AV_SAMPLE_FMT_S16;

			if (ms->audio_decode_frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
				av_channel_layout_default(&ms->audio_decode_frame->ch_layout, ms->audio_decode_frame->ch_layout.nb_channels);

				if (audio_equal_mono && (ms->audio_decode_frame->ch_layout.nb_channels == 1)) {
				    swr_alloc_set_opts2(
                        &ms->swr,
                        &converted_frame->ch_layout,
                        converted_frame->format,
                        converted_frame->sample_rate,
                        &ms->audio_decode_frame->ch_layout,
                        ms->audio_decode_frame->format,
                        ms->audio_decode_frame->sample_rate,
                        0,
                        NULL);

				    swr_set_matrix(ms->swr, stereo_matrix, 1);
				}
			}

			if(swr_convert_frame(ms->swr, converted_frame, ms->audio_decode_frame)) {
				av_frame_free(&converted_frame);
				continue;
			}

			double start = ms->audio_decode_frame->best_effort_timestamp * timebase;
			double end = start + 1.0 * converted_frame->nb_samples / audio_sample_rate;

			SDL_LockMutex(ms->lock);

			if (start >= ms->skip) {

				// Normal case, queue the frame.
				ms->audio_queue_samples += converted_frame->nb_samples;
				enqueue_frame(&ms->audio_queue, converted_frame);

			} else if (end < ms->skip) {
				// Totally before, drop the frame.
				av_frame_free(&converted_frame);

			} else {
				// The frame straddles skip, so we queue the (necessarily single)
				// frame and set the index into the frame.
				ms->audio_out_frame = converted_frame;
				ms->audio_out_index = BPS * (int) ((ms->skip - start) * audio_sample_rate);

			}

			SDL_UnlockMutex(ms->lock);
		}

	}

	return;

}


/* Video decoding *************************************************************/

static enum AVPixelFormat get_pixel_format(SDL_Surface *surf) {
    uint32_t pixel;
    uint8_t *bytes = (uint8_t *) &pixel;

	pixel = SDL_MapRGBA(surf->format, 1, 2, 3, 4);

	enum AVPixelFormat fmt;

    if ((bytes[0] == 4 || bytes[0] == 0) && bytes[1] == 1) {
        fmt = AV_PIX_FMT_ARGB;
    } else if ((bytes[0] == 4  || bytes[0] == 0) && bytes[1] == 3) {
        fmt = AV_PIX_FMT_ABGR;
    } else if (bytes[0] == 1) {
        fmt = AV_PIX_FMT_RGBA;
    } else {
        fmt = AV_PIX_FMT_BGRA;
    }

    return fmt;
}


static void finish_video(MediaState *ms) {
    SDL_LockMutex(ms->lock);
    ms->video_finished = 1;
    SDL_UnlockMutex(ms->lock);
}

static SurfaceQueueEntry *decode_video_frame(MediaState *ms) {
	int ret;

	while (1) {

		AVPacket *pkt = read_packet(ms, &ms->video_packet_queue);
		ret = avcodec_send_packet(ms->video_context, pkt);


		if (ret == 0) {
			dequeue_packet(&ms->video_packet_queue);
		} else if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			// pass
		} else {
			av_log(NULL, AV_LOG_ERROR, "[ffmedia] send_packet error ret=%d hw_device_ctx=%p\n", ret, (void*)ms->hw_device_ctx);
			// HW decode may fail on unsupported codec+GPU combos
			if (ms->hw_device_ctx) {
				av_log(NULL, AV_LOG_ERROR, "[ffmedia] HW→SW fallback (send_packet) triggered, reopening codec\n");
				avcodec_free_context(&ms->video_context);
				av_buffer_unref(&ms->hw_device_ctx);
				ms->hw_device_ctx = NULL;
				ms->video_context = find_context(ms->ctx, ms->video_stream, NULL);
				if (!ms->video_context) {
					continue;
				}
				continue;
			}
				continue;
		}

		ret = avcodec_receive_frame(ms->video_context, ms->video_decode_frame);

		// More input is needed.

			if (ret == AVERROR_EOF) {
				finish_video(ms);
				return NULL;
			}

		if (ret == AVERROR(EAGAIN)) {
			continue;
		}

		if (ret < 0) {
			av_log(NULL, AV_LOG_ERROR, "[ffmedia] receive_frame error ret=%d hw_device_ctx=%p\n", ret, (void*)ms->hw_device_ctx);
			// HW decode may fail on unsupported codec+GPU combos
			if (ms->hw_device_ctx) {
				av_log(NULL, AV_LOG_ERROR, "[ffmedia] HW→SW fallback triggered, reopening codec\n");
				avcodec_free_context(&ms->video_context);
				av_buffer_unref(&ms->hw_device_ctx);
				ms->hw_device_ctx = NULL;
				ms->video_context = find_context(ms->ctx, ms->video_stream, NULL);
				av_log(NULL, AV_LOG_ERROR, "[ffmedia] fallback find_context returned %p\n", (void*)ms->video_context);
				if (!ms->video_context) {
					continue;
				}
				continue;
			}
			// Skip this frame and keep decoding.
			continue;
		}

		break;
	}

	double pts = ms->video_decode_frame->best_effort_timestamp * av_q2d(ms->ctx->streams[ms->video_stream]->time_base);

	if (pts < ms->skip) {
		return NULL;
	}

    SDL_LockMutex(ms->lock);
    int drop = ms->frame_drops && ms->video_clock_started && !ms->pause_time &&
        pts < video_time(ms, media_clock_now()) - frame_early_delivery * ms->video_speed;
    SDL_UnlockMutex(ms->lock);
    if (drop) return NULL;

	// If this is a hardware frame, transfer it back to CPU memory.
	AVFrame *sw_frame = ms->video_decode_frame;

	if (ms->hw_device_ctx) {
		const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(
			ms->video_decode_frame->format);

		if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
			if (!ms->hw_frame) {
				ms->hw_frame = av_frame_alloc();
				if (!ms->hw_frame) return NULL;
			}

			int ret = av_hwframe_transfer_data(ms->hw_frame,
			                                   ms->video_decode_frame, 0);
			if (ret < 0) return NULL;

			av_frame_copy_props(ms->hw_frame, ms->video_decode_frame);
			sw_frame = ms->hw_frame;
		}
	}

	if (sw_frame->format == AV_PIX_FMT_YUV420P ||
		sw_frame->format == AV_PIX_FMT_YUVJ420P) {
		int width = sw_frame->width;
		int height = sw_frame->height;
		int chroma_width = (width + 1) / 2;
		int chroma_height = (height + 1) / 2;
		SurfaceQueueEntry *rv = av_mallocz(sizeof(SurfaceQueueEntry));

		if (!rv) {
			finish_video(ms);
			return NULL;
		}

		rv->yuv_y_pitch = width;
		rv->yuv_uv_pitch = chroma_width * 2;
		rv->yuv_y = av_malloc((size_t) width * height);
		rv->yuv_uv = av_malloc((size_t) rv->yuv_uv_pitch * chroma_height);

		if (!rv->yuv_y || !rv->yuv_uv) {
			free_surface_entry(rv);
			finish_video(ms);
			return NULL;
		}

		for (int y = 0; y < height; y++) {
			memcpy(
				rv->yuv_y + (size_t) y * rv->yuv_y_pitch,
				sw_frame->data[0] + y * sw_frame->linesize[0],
				width
			);
		}

		for (int y = 0; y < chroma_height; y++) {
			const uint8_t *src_u = sw_frame->data[1] + y * sw_frame->linesize[1];
			const uint8_t *src_v = sw_frame->data[2] + y * sw_frame->linesize[2];
			uint8_t *dst_uv = rv->yuv_uv + (size_t) y * rv->yuv_uv_pitch;

			for (int x = 0; x < chroma_width; x++) {
				dst_uv[x * 2] = src_u[x];
				dst_uv[x * 2 + 1] = src_v[x];
			}
		}

		rv->yuv = 1;
		rv->yuv_width = width;
		rv->yuv_height = height;
		rv->yuv_full_range =
			sw_frame->color_range == AVCOL_RANGE_JPEG ||
			sw_frame->format == AV_PIX_FMT_YUVJ420P;
		rv->yuv_bt709 = sw_frame->colorspace == AVCOL_SPC_BT709;
		rv->pts = pts;
		return rv;
	}

	SDL_Surface *sample = rgba_surface;

	if (ms->sws == NULL) {
		ms->sws = sws_getContext(
			sw_frame->width,
			sw_frame->height,
			sw_frame->format,

			sw_frame->width,
			sw_frame->height,
			get_pixel_format(sample),

			SWS_POINT | SWS_FULL_CHR_H_INP | SWS_FULL_CHR_H_INT,

			NULL,
			NULL,
			NULL
			);


		if (!ms->sws) {
					finish_video(ms);
					return NULL;
		}

		int colorspace = sw_frame->colorspace;
		if (colorspace == AVCOL_SPC_UNSPECIFIED) {
			colorspace = SWS_CS_DEFAULT;
		}

		int src_range = (sw_frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;

		sws_setColorspaceDetails(ms->sws,
			sws_getCoefficients(colorspace), src_range,
			sws_getCoefficients(SWS_CS_DEFAULT), 0,
			0, 1 << 16, 1 << 16);
	}

	SurfaceQueueEntry *rv = av_malloc(sizeof(SurfaceQueueEntry));
	if (rv == NULL) {
					finish_video(ms);
					return NULL;
	}
	memset(rv, 0, sizeof(*rv));
	rv->w = sw_frame->width + FRAME_PADDING * 2;
	rv->h = sw_frame->height + FRAME_PADDING * 2;

	rv->pitch = rv->w * sample->format->BytesPerPixel;

	if (rv->pitch % ROW_ALIGNMENT) {
	    rv->pitch += ROW_ALIGNMENT - (rv->pitch % ROW_ALIGNMENT);
	}

#ifndef USE_POSIX_MEMALIGN
    rv->pixels = SDL_calloc(rv->pitch * rv->h, 1);
#else
	if (posix_memalign(&rv->pixels, ROW_ALIGNMENT, rv->pitch * rv->h)) {
		av_free(rv);
		return NULL;
	}
    memset(rv->pixels, 0, rv->pitch * rv->h);
#endif

	rv->format = sample->format;
	rv->next = NULL;
	rv->pts = pts;

	uint8_t *surf_pixels = (uint8_t *) rv->pixels;
	uint8_t *surf_data[] = { &surf_pixels[FRAME_PADDING * rv->pitch + FRAME_PADDING * sample->format->BytesPerPixel] };
	int surf_linesize[] = { rv->pitch };

	sws_scale(
		ms->sws,

		(const uint8_t * const *) sw_frame->data,
		sw_frame->linesize,

		0,
		sw_frame->height,

		surf_data,
		surf_linesize
		);

	return rv;
}


static void decode_video(MediaState *ms) {
	if (!ms->video_context) {
		// Skip this frame and keep decoding.
		return;
	}

	if (!ms->video_decode_frame) {
		ms->video_decode_frame = av_frame_alloc();
	}

	if (!ms->video_decode_frame) {
		// Skip this frame and keep decoding.
		return;
	}

	SDL_LockMutex(ms->lock);

	while (!ms->video_finished && (ms->surface_queue_size < FRAMES)) {

		SDL_UnlockMutex(ms->lock);

		SurfaceQueueEntry *sqe = decode_video_frame(ms);

		SDL_LockMutex(ms->lock);

		if (sqe) {
			enqueue_surface(&ms->surface_queue, sqe);
			ms->surface_queue_size += 1;
		} else {
			/* video_finished → EOF reached, stop decoding. */
			if (ms->video_finished) {
				break;
			}
			/* No more packets queued → can't make progress, stop. */
			if (!ms->video_packet_queue.first) {
				break;
			}
			/* Otherwise the frame was dropped (PTS behind real-time).
			 * There may be more packets in the queue, so keep trying. */
		}
	}

	if (!ms->video_finished && (ms->surface_queue_size < FRAMES)) {
		ms->needs_decode = 1;
	}

	SDL_UnlockMutex(ms->lock);
}


static int decode_sync_start(void *arg);
void media_read_sync(struct MediaState *ms);
void media_read_sync_finish(struct MediaState *ms);


/**
 * Returns 1 if there is a video frame ready on this channel, or 0 otherwise.
 */
int media_video_ready(struct MediaState *ms) {

	int consumed = 0;
	int rv = 0;

	SDL_LockMutex(ms->lock);

	if (!ms->ready) {
		goto done;
	}

    if (ms->video_stream == -1) {
        rv = 1;
        goto done;
    }

	if (ms->pause_time > 0 || ms->seek_req || ms->seeking) {
	    goto done;
	}

    double position = video_time(ms, media_clock_now());

	/*
	 * If we have an obsolete frame, drop it.
	 */
    if (ms->frame_drops && ms->video_clock_started) {
        while (ms->surface_queue && ms->surface_queue->next) {

			/* Keep the latest frame due on the same source clock. */
            if (ms->surface_queue->next->pts > position) {
				break;
			}

			/* Otherwise, drop it without display. */
			SurfaceQueueEntry *sqe = dequeue_surface(&ms->surface_queue);
			ms->surface_queue_size -= 1;

			free_surface_entry(sqe);

			consumed = 1;
		}
	}


	/*
	 * Otherwise, check to see if we have a frame with a PTS that has passed.
	 */

	if (ms->surface_queue) {
		if (ms->video_clock_started) {
            if (ms->surface_queue->pts <= position + frame_early_delivery * ms->video_speed) {
				rv = 1;
			}
		} else {
			rv = 1;
		}
	}

done:

	/* Only signal if we've consumed something. */
	if (consumed) {
		ms->needs_decode = 1;
		SDL_CondBroadcast(ms->cond);
	}

	SDL_UnlockMutex(ms->lock);

	return rv;
}


int media_read_video_yuv(MediaState *ms, MediaVideoYUV *rv) {
	SurfaceQueueEntry *sqe = NULL;

	memset(rv, 0, sizeof(*rv));

	SDL_LockMutex(ms->lock);

    if (!ms->ready || ms->video_stream == -1) goto done;

	if (ms->pause_time > 0 || ms->seek_req || ms->seeking || !ms->surface_queue_size || !ms->surface_queue->yuv) {
		goto done;
	}

    double now = media_clock_now();
    if (!ms->video_clock_only && !ms->video_clock_started) ms->video_time_anchor = ms->surface_queue->pts;
    video_start_clock(ms, now);
    if (ms->surface_queue->pts <= video_time(ms, now) + frame_early_delivery * ms->video_speed) {
		sqe = dequeue_surface(&ms->surface_queue);
		ms->surface_queue_size -= 1;
	}

done:
	if (sqe) {
		ms->needs_decode = 1;
		SDL_CondBroadcast(ms->cond);
	}

	SDL_UnlockMutex(ms->lock);

	if (!sqe) {
		return 0;
	}

	rv->width = sqe->yuv_width;
	rv->height = sqe->yuv_height;
	rv->y = sqe->yuv_y;
	rv->uv = sqe->yuv_uv;
	rv->full_range = sqe->yuv_full_range;
	rv->bt709 = sqe->yuv_bt709;

	sqe->yuv_y = NULL;
	sqe->yuv_uv = NULL;
	free_surface_entry(sqe);
	return 1;
}


void media_free_video_yuv(MediaVideoYUV *frame) {
	if (!frame) {
		return;
	}

	av_free(frame->y);
	av_free(frame->uv);
	memset(frame, 0, sizeof(*frame));
}


static SDL_Surface *convert_yuv_surface(MediaState *ms, SurfaceQueueEntry *sqe) {
	SDL_Surface *sample = rgba_surface;

	ms->yuv_sws = sws_getCachedContext(
		ms->yuv_sws,
		sqe->yuv_width,
		sqe->yuv_height,
		AV_PIX_FMT_NV12,
		sqe->yuv_width,
		sqe->yuv_height,
		get_pixel_format(sample),
		SWS_POINT | SWS_FULL_CHR_H_INP | SWS_FULL_CHR_H_INT,
		NULL,
		NULL,
		NULL
	);

	if (!ms->yuv_sws) {
		return NULL;
	}

	int colorspace = sqe->yuv_bt709 ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
	int src_range = sqe->yuv_full_range ? 1 : 0;

	sws_setColorspaceDetails(
		ms->yuv_sws,
		sws_getCoefficients(colorspace),
		src_range,
		sws_getCoefficients(SWS_CS_DEFAULT),
		0,
		0,
		1 << 16,
		1 << 16
	);

	int w = sqe->yuv_width + FRAME_PADDING * 2;
	int h = sqe->yuv_height + FRAME_PADDING * 2;
	int pitch = w * sample->format->BytesPerPixel;

	if (pitch % ROW_ALIGNMENT) {
		pitch += ROW_ALIGNMENT - (pitch % ROW_ALIGNMENT);
	}

	void *pixels;
#ifndef USE_POSIX_MEMALIGN
	pixels = SDL_calloc(pitch * h, 1);
#else
	if (posix_memalign(&pixels, ROW_ALIGNMENT, pitch * h)) {
		return NULL;
	}
	memset(pixels, 0, pitch * h);
#endif

	uint8_t *surf_pixels = (uint8_t *) pixels;
	uint8_t *surf_data[] = {
		&surf_pixels[FRAME_PADDING * pitch + FRAME_PADDING * sample->format->BytesPerPixel]
	};
	int surf_linesize[] = { pitch };
	const uint8_t *yuv_data[] = { sqe->yuv_y, sqe->yuv_uv };
	int yuv_linesize[] = { sqe->yuv_y_pitch, sqe->yuv_uv_pitch };

	sws_scale(
		ms->yuv_sws,
		yuv_data,
		yuv_linesize,
		0,
		sqe->yuv_height,
		surf_data,
		surf_linesize
	);

	SDL_Surface *rv = SDL_CreateRGBSurfaceFrom(
		pixels,
		w,
		h,
		sample->format->BitsPerPixel,
		pitch,
		sample->format->Rmask,
		sample->format->Gmask,
		sample->format->Bmask,
		sample->format->Amask
	);

	if (!rv) {
#ifndef USE_POSIX_MEMALIGN
		SDL_free(pixels);
#else
		free(pixels);
#endif
		return NULL;
	}

	/* Force SDL to take over management of pixels. */
	rv->flags &= ~SDL_PREALLOC;
	return rv;
}


SDL_Surface *media_read_video(MediaState *ms) {

	SDL_Surface *rv = NULL;
	SurfaceQueueEntry *sqe = NULL;

	SDL_LockMutex(ms->lock);

    if (!ms->ready || ms->video_stream == -1) goto done;

	if (ms->pause_time > 0 || ms->seek_req || ms->seeking) {
	    goto done;
	}

	if (!ms->surface_queue_size) {
		goto done;
	}

    double now = media_clock_now();
    if (!ms->video_clock_only && !ms->video_clock_started) ms->video_time_anchor = ms->surface_queue->pts;
    video_start_clock(ms, now);
    if (ms->surface_queue->pts <= video_time(ms, now) + frame_early_delivery * ms->video_speed) {
		sqe = dequeue_surface(&ms->surface_queue);
		ms->surface_queue_size -= 1;

	}

done:

    /* Only signal if we've consumed something. */
	if (sqe) {
		ms->needs_decode = 1;
		SDL_CondBroadcast(ms->cond);
	}

	SDL_UnlockMutex(ms->lock);

	if (sqe && sqe->yuv) {
		rv = convert_yuv_surface(ms, sqe);
		free_surface_entry(sqe);
		return rv;
	}

	if (sqe) {
		rv = SDL_CreateRGBSurfaceFrom(
			sqe->pixels,
			sqe->w,
			sqe->h,
			sqe->format->BitsPerPixel,
			sqe->pitch,
			sqe->format->Rmask,
			sqe->format->Gmask,
			sqe->format->Bmask,
			sqe->format->Amask
		);

		/* Force SDL to take over management of pixels. */
		rv->flags &= ~SDL_PREALLOC;
		sqe->pixels = NULL;
		free_surface_entry(sqe);
	}

	return rv;
}


static int decode_thread(void *arg) {
	MediaState *ms = (MediaState *) arg;

	int err = AVERROR(ENOMEM);
	const char *stage = "allocate media context";

	AVFormatContext *ctx = avformat_alloc_context();
	if (ctx == NULL) {
		goto finish;
	}
	ms->ctx = ctx;

	stage = "allocate media IO";
	AVIOContext *io_context = rwops_open(ms->rwops);
	if (io_context == NULL) {
		goto finish;
	}
	ctx->pb = io_context;

	stage = "open input";
	err = avformat_open_input(&ctx, ms->filename, NULL, NULL);
	if (err) {
		ms->ctx = NULL;
		/* The format context is freed on failure, but custom IO is ours. */
		av_freep(&io_context->buffer);
		avio_context_free(&io_context);
		goto finish;
	}

	stage = "find stream info";
	err = avformat_find_stream_info(ctx, NULL);
	if (err) {
		goto finish;
	}

	ms->video_stream = -1;
	ms->audio_stream = -1;

	for (unsigned int i = 0; i < ctx->nb_streams; i++) {
		if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			if (ms->want_video && ms->video_stream == -1) {
				ms->video_stream = i;
			}
		}

		if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			if (ms->audio_stream == -1) {
				ms->audio_stream = i;
			}
		}
	}

	ms->video_context = find_context(ctx, ms->video_stream, &ms->hw_device_ctx);
	ms->audio_context = find_context(ctx, ms->audio_stream, NULL);

	ms->swr = swr_alloc();
	if (ms->swr == NULL) {
		stage = "allocate resampler";
		err = AVERROR(ENOMEM);
		goto finish;
	}

	// Duration remains the full source duration, even for a trimmed stream.
    if (ctx->duration > 0 && ctx->duration != AV_NOPTS_VALUE &&
            ctx->duration_estimation_method != AVFMT_DURATION_FROM_BITRATE) {
        ms->total_duration = (double) ctx->duration / AV_TIME_BASE;
    }
	// Compute the number of samples we need to play back.
	if (ms->audio_duration < 0) {
		if (ctx->duration_estimation_method != AVFMT_DURATION_FROM_BITRATE &&
				ctx->duration >= 0 && ctx->duration <= 3600LL * AV_TIME_BASE) {

			long long duration = ((long long) ctx->duration) * audio_sample_rate;
			ms->audio_duration = (unsigned int) (duration /  AV_TIME_BASE);

			ms->total_duration = 1.0 * ctx->duration / AV_TIME_BASE;

			// Check that the duration is reasonable (between 0s and 3600s). If not,
			// reject it.
			if (ms->audio_duration < 0 || ms->audio_duration > 3600 * audio_sample_rate) {
				ms->audio_duration = -1;
			}

			ms->audio_duration -= (unsigned int) (ms->skip * audio_sample_rate);


		} else {
			ms->audio_duration = -1;
		}
	}

	if (ms->skip != 0.0) {
		seek_media(ms, ms->skip);
	}

    media_publish_stream_info(ms);

	while (!ms->quit) {

		/* ── Runtime seek ──────────────────────────────────
		 * If media_seek() was called from another thread,
		 * jump to the requested position without tearing down
		 * the decode pipeline.
		 */
		SDL_LockMutex(ms->lock);
		int seek = ms->seek_req;
		double target = ms->seek_target;
		if (seek) {
			ms->seek_req = 0;
			ms->seeking = 1;
		}
		SDL_UnlockMutex(ms->lock);
		if (seek) {

			/* Seek the demuxer to the target position. */
			int seek_ret = seek_media(ms, target);

			/* Drain stale packets from both queues. */
			free_packet_queue(&ms->audio_packet_queue);
			free_packet_queue(&ms->video_packet_queue);

			/* Drain decoded audio frames. */
			SDL_LockMutex(ms->lock);
			while (1) {
				AVFrame *f = dequeue_frame(&ms->audio_queue);
				if (!f) break;
				av_frame_free(&f);
			}

			/* Drain decoded video surfaces. */
			while (1) {
				SurfaceQueueEntry *sqe =
					dequeue_surface(&ms->surface_queue);
				if (!sqe) break;
				free_surface_entry(sqe);
			}
			ms->surface_queue_size = 0;

			/* Flush decoder-internal buffers so old frames
			 * aren't returned after the seek. */
			if (ms->audio_context)
				avcodec_flush_buffers(ms->audio_context);
			if (ms->video_context)
				avcodec_flush_buffers(ms->video_context);

			/* Reset stream state.
			 *
			 * IMPORTANT: audio_read_samples must be 0 (not target*sr).
			 * media_read_audio() computes remaining = (audio_duration -
			 * audio_read_samples) * BPS.  Since audio_duration was just
			 * reduced to the *remaining* samples after target, setting
			 * audio_read_samples to target*sr would double-subtract and
			 * underflow for any seek past the halfway point, producing
			 * a negative length → memset() crash. */
			ms->audio_finished = 0;
			ms->video_finished = 0;
			ms->audio_read_samples = 0;
			ms->audio_queue_samples = 0;
			ms->skip = target;
			if (ms->audio_out_frame) {
				av_frame_free(&ms->audio_out_frame);
				ms->audio_out_index = 0;
			}

			/* Recompute remaining duration from the full
			 * file duration (ms->total_duration), NOT
			 * from the already-reduced audio_duration.
			 * Subtracting from audio_duration on every
			 * seek causes it to decay to zero after a
			 * few drags, which kills playback. */
			double end = ms->audio_end >= 0 ? ms->audio_end : ms->total_duration;
			ms->audio_duration = end > 0 ? (int) (fmax(0, end - target) * audio_sample_rate) : -1;
#ifndef __EMSCRIPTEN__
            ms->tempo_input_samples = 0;
            ms->tempo_output_samples = 0;
            ms->tempo_output_limit = -1;
#endif
			SDL_UnlockMutex(ms->lock);

            /* These objects are owned exclusively by the decoder. Rebuild
             * them after seeking, without reopening the (possibly encrypted)
             * file, so no overlap or resampler history survives the seek. */
            swr_close(ms->swr);
#ifndef __EMSCRIPTEN__
            avfilter_graph_free(&ms->tempo_graph);
            ms->tempo_source = ms->tempo_sink = NULL;
            ms->tempo_eof = 0;
            ms->tempo_resampler_drained = 0;
            ms->tempo_decode_position = target;
#endif
			SDL_LockMutex(ms->lock);

			/* Reset the decode target so the queue refills. */
			ms->audio_queue_target_samples = 0;
			ms->needs_decode = 1;

            /* Seeking resets source position but preserves the rate.
             * Runtime rate changes never enter this decode/reset path. */
            ms->video_clock_started = 0;
            ms->video_clock_ended = 0;
            ms->video_time_anchor = target;
            ms->video_clock_anchor = media_clock_now();
			ms->seeking = 0;
            if (seek_ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "[ffmedia] seek failed for %s: %d\n", ms->filename, seek_ret);
                ms->audio_finished = ms->video_finished = 1;
                ms->audio_duration = 0;
            }
			SDL_UnlockMutex(ms->lock);
		}

		if (! ms->audio_finished) {
			decode_audio(ms);
		}

		if (! ms->video_finished) {
			decode_video(ms);
		}

		SDL_LockMutex(ms->lock);

		if (!ms->ready) {
			ms->ready = 1;
			SDL_CondBroadcast(ms->cond);
		}

		if (!(ms->needs_decode || ms->quit)) {
			SDL_CondWait(ms->cond, ms->lock);
		}

		ms->needs_decode = 0;

		SDL_UnlockMutex(ms->lock);
	}


finish:
	if (err < 0) {
		char message[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(err, message, sizeof(message));
		av_log(NULL, AV_LOG_ERROR, "[ffmedia] %s (%s): %s\n", stage, ms->filename, message);
	}
	/* Data used by the decoder should be freed here, while data shared with
	 * the readers should be freed in media_close.
	 */

	SDL_LockMutex(ms->lock);
	ms->audio_finished = ms->video_finished = 1;

	/* Ensures that every stream becomes ready. */
	if (!ms->ready) {
		ms->ready = 1;
		SDL_CondBroadcast(ms->cond);
	}

	while (!ms->quit || ms->video_readers) {
		SDL_CondWait(ms->cond, ms->lock);
	}

	SDL_UnlockMutex(ms->lock);

	deallocate(ms);

	return 0;
}


void media_read_sync_finish(struct MediaState *ms) {
	// copy/paste from end of decode_thread

	/* Data used by the decoder should be freed here, while data shared with
	 * the readers should be freed in media_close.
	 */

	SDL_LockMutex(ms->lock);

	/* Ensures that every stream becomes ready. */
	if (!ms->ready) {
		ms->ready = 1;
		SDL_CondBroadcast(ms->cond);
	}

	while (!ms->quit) {
		/* SDL_CondWait(ms->cond, ms->lock); */
	}

	SDL_UnlockMutex(ms->lock);

	deallocate(ms);
}


static int decode_sync_start(void *arg) {
    // copy/paste from start of decode_thread
	MediaState *ms = (MediaState *) arg;

	int err;

	AVFormatContext *ctx = avformat_alloc_context();
	if (ctx == NULL) {
		media_read_sync_finish(ms);
	}
	ms->ctx = ctx;

	AVIOContext *io_context = rwops_open(ms->rwops);
	if (io_context == NULL) {
		media_read_sync_finish(ms);
	}
	ctx->pb = io_context;

	err = avformat_open_input(&ctx, ms->filename, NULL, NULL);
	if (err) {
		ms->ctx = NULL;
		media_read_sync_finish(ms);
	}

	err = avformat_find_stream_info(ctx, NULL);
	if (err) {
		media_read_sync_finish(ms);
	}


	ms->video_stream = -1;
	ms->audio_stream = -1;

	for (unsigned int i = 0; i < ctx->nb_streams; i++) {
		if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			if (ms->want_video && ms->video_stream == -1) {
				ms->video_stream = i;
			}
		}

		if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			if (ms->audio_stream == -1) {
				ms->audio_stream = i;
			}
		}
	}

	ms->video_context = find_context(ctx, ms->video_stream, &ms->hw_device_ctx);
	ms->audio_context = find_context(ctx, ms->audio_stream, NULL);

	ms->swr = swr_alloc();
	if (ms->swr == NULL) {
		media_read_sync_finish(ms);
	}

	// Compute the number of samples we need to play back.
	if (ms->audio_duration < 0) {
		if (ctx->duration_estimation_method != AVFMT_DURATION_FROM_BITRATE) {

			long long duration = ((long long) ctx->duration) * audio_sample_rate;
			ms->audio_duration = (unsigned int) (duration /  AV_TIME_BASE);

			ms->total_duration = 1.0 * ctx->duration / AV_TIME_BASE;

			// Check that the duration is reasonable (between 0s and 3600s). If not,
			// reject it.
			if (ms->audio_duration < 0 || ms->audio_duration > 3600 * audio_sample_rate) {
				ms->audio_duration = -1;
			}

			ms->audio_duration -= (unsigned int) (ms->skip * audio_sample_rate);


		} else {
			ms->audio_duration = -1;
		}
	}

	if (ms->skip != 0.0) {
		seek_media(ms, ms->skip);
	}

	// [snip!]

    media_publish_stream_info(ms);

	return 0;
}


void media_read_sync(struct MediaState *ms) {
	// copy/paste from middle of decode_thread
	// printf("---* media_read_sync %p\n", ms);

	//while (!ms->quit) {
	if (!ms->quit) {
		// printf("     audio_finished: %d, video_finished: %d\n", ms->audio_finished, ms->video_finished);
		if (! ms->audio_finished) {
			decode_audio(ms);
		}

		if (! ms->video_finished) {
			decode_video(ms);
		}

		SDL_LockMutex(ms->lock);

		if (!ms->ready) {
			ms->ready = 1;
			SDL_CondBroadcast(ms->cond);
		}

		if (!(ms->needs_decode || ms->quit)) {
			/* SDL_CondWait(ms->cond, ms->lock); */
		}

		ms->needs_decode = 0;

		SDL_UnlockMutex(ms->lock);
	}
}


#ifndef __EMSCRIPTEN__
static int media_read_tempo(MediaState *ms, Uint8 *stream, int len) {
    int rv = 0;
    SDL_LockMutex(ms->lock);
    if (!ms->ready || ms->seek_req || ms->seeking) {
        SDL_UnlockMutex(ms->lock);
        return 0;
    }
    /* A movie's lifetime follows its video, including silent movies and
     * soundtracks shorter than the picture. atempo can also produce a few
     * samples less/more than the requested duration. Bound the output clock
     * in playback samples, without changing source timestamps or duration. */
    if (ms->video_context && ms->audio_duration >= 0) {
        int64_t remaining = (int64_t) ceil(ms->audio_duration / ms->playback_speed) - ms->audio_read_samples;
        if (remaining < len / BPS) len = (int) fmax(0, remaining) * BPS;
    }
    while (len > 0) {
        if (!ms->audio_out_frame) {
            ms->audio_out_frame = dequeue_frame(&ms->audio_queue);
            ms->audio_out_index = 0;
        }
        if (!ms->audio_out_frame) break;
        AVFrame *frame = ms->audio_out_frame;
        int count = frame->nb_samples * BPS - ms->audio_out_index;
        if (count > len) count = len;
        memcpy(stream + rv, frame->data[0] + ms->audio_out_index, count);
        ms->audio_out_index += count;
        ms->audio_read_samples += count / BPS;
        ms->audio_queue_samples -= count / BPS;
        len -= count;
        rv += count;
        if (ms->audio_out_index == frame->nb_samples * BPS) {
            av_frame_free(&ms->audio_out_frame);
            ms->audio_out_index = 0;
        }
    }
    if (len > 0 && ms->audio_finished && ms->video_context &&
            (ms->audio_duration >= 0 || !ms->video_finished || ms->surface_queue_size)) {
        memset(stream + rv, 0, len);
        ms->audio_read_samples += len / BPS;
        rv += len;
    }
    /* An empty read is not a refill request. In particular, finished streams
     * remain open for seeking, but must not wake their decoder on every audio
     * callback. Seeking explicitly wakes it through media_seek(). */
    if (rv && !ms->audio_finished) {
        ms->needs_decode = 1;
        SDL_CondBroadcast(ms->cond);
    }
    SDL_UnlockMutex(ms->lock);
    return rv;
}
#endif

int media_audio_finished(MediaState *ms) {
    SDL_LockMutex(ms->lock);
    if (ms->video_clock_only) {
        int finished = ms->ready && video_clock_finished(ms, media_clock_now());
        SDL_UnlockMutex(ms->lock);
        return finished;
    }
    int finished = ms->ready && !ms->seek_req && !ms->seeking &&
        ((ms->playback_speed == 1.0 && ms->audio_duration >= 0 &&
          ms->audio_read_samples >= ms->audio_duration) ||
         (ms->audio_finished && ms->audio_queue_samples <= 0 && !ms->audio_out_frame));
    if (ms->ready && !ms->seek_req && !ms->seeking && ms->video_context && ms->playback_speed != 1.0) {
        if (ms->audio_duration >= 0) {
            finished = ms->audio_read_samples >= (int64_t) ceil(ms->audio_duration / ms->playback_speed);
        } else {
            finished = finished && ms->video_finished && !ms->surface_queue_size;
        }
    }
    SDL_UnlockMutex(ms->lock);
    return finished;
}

double media_audio_position(MediaState *ms) {
    SDL_LockMutex(ms->lock);
    if (ms->video_clock_only) {
        double position = ms->seek_req ? ms->seek_target : video_time(ms, media_clock_now());
        SDL_UnlockMutex(ms->lock);
        return position;
    }
    double position = ms->skip + ms->audio_read_samples * ms->playback_speed / audio_sample_rate;
#ifndef __EMSCRIPTEN__
    double end = ms->skip + (double) ms->tempo_input_samples / audio_sample_rate;
    if (ms->video_context) {
        if (ms->audio_duration >= 0) {
            end = ms->skip + (double) ms->audio_duration / audio_sample_rate;
            if (position > end) position = end;
        }
    } else if (ms->audio_finished) {
        if (position > end || (!ms->audio_out_frame && !ms->audio_queue.first)) position = end;
    }
#endif
    if (ms->seek_req) position = ms->seek_target;
    SDL_UnlockMutex(ms->lock);
    return position;
}

int media_read_audio(struct MediaState *ms, Uint8 *stream, int len) {
#ifdef __EMSCRIPTEN__
    media_read_sync(ms);
#endif
    SDL_LockMutex(ms->lock);
    if (ms->want_video && !ms->ready) {
        SDL_UnlockMutex(ms->lock);
        return 0;
    }
    if (ms->video_clock_only) {
        if (!ms->ready || ms->seek_req || ms->seeking || ms->pause_time) {
            SDL_UnlockMutex(ms->lock);
            return 0;
        }
        double now = media_clock_now();
        video_start_clock(ms, now);
        if (video_clock_finished(ms, now)) len = 0;
        memset(stream, 0, len);
        SDL_UnlockMutex(ms->lock);
        return len;
    }
    SDL_UnlockMutex(ms->lock);
#ifndef __EMSCRIPTEN__
    if (ms->playback_speed != 1.0) return media_read_tempo(ms, stream, len);
#endif

	SDL_LockMutex(ms->lock);

    if (ms->seek_req || ms->seeking) {
        SDL_UnlockMutex(ms->lock);
        return 0;
    }

    if(!ms->ready) {
	    SDL_UnlockMutex(ms->lock);
	    return 0;
	}

	int rv = 0;

	if (ms->audio_duration >= 0) {
		int remaining = (int) fmax(0, ms->audio_duration - ms->audio_read_samples) * BPS;
		if (len > remaining) {
			len = remaining;
		}

		if (!remaining) {
			ms->audio_finished = 1;
		}

	}

	while (len) {

		if (!ms->audio_out_frame) {
			ms->audio_out_frame = dequeue_frame(&ms->audio_queue);
			ms->audio_out_index = 0;
		}

		if (!ms->audio_out_frame) {
			break;
		}

		AVFrame *f = ms->audio_out_frame;

		int avail = f->nb_samples * BPS - ms->audio_out_index;
		int count;

		if (len > avail) {
			count = avail;
		} else {
			count = len;
		}

		memcpy(stream, &f->data[0][ms->audio_out_index], count);

		ms->audio_out_index += count;

		ms->audio_read_samples += count / BPS;
		ms->audio_queue_samples -= count / BPS;

		rv += count;
		len -= count;
		stream += count;

		if (ms->audio_out_index >= f->nb_samples * BPS) {
			av_frame_free(&ms->audio_out_frame);
			ms->audio_out_index = 0;
		}
	}

	/* Only signal if we've consumed something before decoder EOF. */
	if (rv && !ms->audio_finished) {
		ms->needs_decode = 1;
		SDL_CondBroadcast(ms->cond);
	}

	/* An underrun is not silence in the source. Preserve the source clock
	 * until the decoder supplies real samples. After decoder EOF, preserve
	 * padding to a known/requested end and a movie's video clock. Keep this
	 * under the stream lock, shared with the decoder and seek operations. */
	if (len > 0 && ms->audio_finished &&
			(ms->audio_duration >= 0 || (ms->video_context &&
			 (!ms->video_finished || ms->surface_queue_size)))) {
		memset(stream, 0, len);
		ms->audio_read_samples += len / BPS;
		rv += len;
	}

	SDL_UnlockMutex(ms->lock);

	return rv;
}

int media_is_ready(struct MediaState *ms) {
    SDL_LockMutex(ms->lock);
    int ready = ms->ready;
    SDL_UnlockMutex(ms->lock);
    return ready;
}


double media_duration(MediaState *ms) {
    SDL_LockMutex(ms->lock);
    double duration = ms->stream_info_ready ? ms->total_duration : 0;
    SDL_UnlockMutex(ms->lock);
    return duration;
}

void media_start(MediaState *ms) {

#ifdef __EMSCRIPTEN__
    decode_sync_start(ms);
#else

    char buf[1024];

	snprintf(buf, 1024, "decode: %s", ms->filename);
	SDL_Thread *t = SDL_CreateThread(decode_thread, buf, (void *) ms);
	ms->thread = t;
#endif
}


MediaState *media_open(SDL_RWops *rwops, const char *filename) {

    deallocate_deferred();

    MediaState *ms = av_calloc(1, sizeof(MediaState));
	if (ms == NULL) {
		return NULL;
	}

	ms->filename = av_strdup(filename);
	if (ms->filename == NULL) {
		deallocate(ms);
		return NULL;
	}
	ms->rwops = rwops;

#ifndef __EMSCRIPTEN__
	ms->cond = SDL_CreateCond();
	if (ms->cond == NULL) {
		deallocate(ms);
		return NULL;
	}
	ms->lock = SDL_CreateMutex();
	if (ms->lock == NULL) {
		deallocate(ms);
		return NULL;
	}
#endif

	ms->audio_duration = -1;
	ms->audio_end = -1;
	ms->playback_speed = 1.0;
    ms->video_speed = 1.0;
#ifndef __EMSCRIPTEN__
    ms->tempo_output_limit = -1;
#endif
	ms->frame_drops = 1;

	return ms;
}

/**
 * Sets the start and end of the stream. This must be called before
 * media_start.
 *
 * start
 *    The time in the stream at which the media starts playing.
 * end
 *    If not 0, the time at which the stream is forced to end if it has not
 *    already. If 0, the stream plays until its natural end.
 */
void media_start_end(MediaState *ms, double start, double end) {
	ms->skip = start;
	ms->audio_end = end;
    ms->video_time_anchor = start;

	if (end >= 0) {
		if (end < start) {
			ms->audio_duration = 0;
		} else {
			ms->audio_duration = (int) ((end - start) * audio_sample_rate);
		}
	}
}

/* Called before media_start; the setting cannot change underneath a stream. */
void media_set_playback_speed(MediaState *ms, double speed) {
#ifndef __EMSCRIPTEN__
    ms->playback_speed = isfinite(speed) && speed >= 0.5 && speed <= 3.0 ? speed : 1.0;
    ms->video_speed = ms->playback_speed;
#endif
}

/**
 * Marks the channel as having video.
 */
void media_want_video(MediaState *ms, int video) {
	ms->want_video = 1;
	ms->frame_drops = (video == 2);
}


/**
 * Requests a runtime seek to `position` seconds within the currently playing
 * stream.  This avoids tearing down and rebuilding the entire decode pipeline
 * (which would re-open, re-decrypt, and re-parse the file).  Instead, it
 * signals the decode thread to call av_seek_frame() on the already-open
 * AVFormatContext.
 *
 * Safe to call from any thread.  The seek is processed asynchronously by the
 * decode thread on its next loop iteration.
 */
double media_seek(struct MediaState *ms, double position) {
	SDL_LockMutex(ms->lock);

    if (!isfinite(position)) {
        position = ms->skip;
        SDL_UnlockMutex(ms->lock);
        return position;
    }
    position = fmax(0, position);
    if (ms->total_duration > 0) position = fmin(position, ms->total_duration);
    if (ms->audio_end >= 0) position = fmin(position, ms->audio_end);

	ms->seek_req = 1;
	ms->seek_target = position;

	/* Wake up the decode thread so it processes the seek immediately,
	 * even if it was blocked on needs_decode / cond_wait. */
	ms->needs_decode = 1;
	SDL_CondBroadcast(ms->cond);

	SDL_UnlockMutex(ms->lock);
    return position;
}


void media_pause(MediaState *ms, int pause) {
    SDL_LockMutex(ms->lock);
    double now = media_clock_now();
    if (pause && (ms->pause_time == 0)) {
        ms->video_time_anchor = video_time(ms, now);
        ms->video_clock_anchor = now;
        ms->pause_time = now;
    } else if ((!pause) && (ms->pause_time > 0)) {
        ms->video_clock_anchor = now;
        ms->pause_time = 0;
    }
    SDL_UnlockMutex(ms->lock);
}

void media_close(MediaState *ms) {

	if (!ms->thread) {
		deallocate(ms);
		return;
	}

	/* Tell the decoder to terminate. It will deallocate everything for us. */
	SDL_LockMutex(ms->lock);
	ms->quit = 1;

#ifdef __EMSCRIPTEN__
	media_read_sync_finish(ms);
#endif

	SDL_CondBroadcast(ms->cond);
	SDL_UnlockMutex(ms->lock);

}

void media_advance_time(void) {
    /* Retained for the backend API. Video clocks now sample monotonic time
     * at the point of use, including between display updates. */
}

void media_sample_surfaces(SDL_Surface *rgb, SDL_Surface *rgba) {
	rgb_surface = rgb;
	rgba_surface = rgba;
}

void media_init(int rate, int status, int equal_mono) {

    deallocate_mutex = SDL_CreateMutex();

	audio_sample_rate = rate / SPEED;
	audio_equal_mono = equal_mono;

    if (status) {
        av_log_set_level(AV_LOG_INFO);
    } else {
        av_log_set_level(AV_LOG_ERROR);
    }

}
