/* KJL 15:25:20 8/16/97
 *
 * smacker.c - functions to handle FMV playback
 *
 */
#include "3dc.h"
#include "module.h"
#include "inline.h"
#include "stratdef.h"
#include "gamedef.h"
#include "fmv.h"
#include "avp_menus.h"
#include "avp_userprofile.h"
#include "oglfunc.h" // move this into opengl.c

extern void OGL_RegenerateMipmaps(void);

#define UseLocalAssert 1
#include "ourasert.h"

#ifdef __ANDROID__
#include "files.h"
#include <ctype.h>
#include <android/log.h>
#define FMV_LOG(...) __android_log_print(ANDROID_LOG_DEBUG, "FMV", __VA_ARGS__)
/* system.h defines 'debug' as a macro (→ 0/1); undef it so avcodec.h's
   AVCodecContext::debug member isn't mangled by the preprocessor. */
#undef debug
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswscale/swscale.h>
#include <SDL3/SDL.h>
#endif

int VolumeOfNearestVideoScreen;
int PanningOfNearestVideoScreen;

extern char *ScreenBuffer;
extern int GotAnyKey;
extern void DirectReadKeyboard(void);
extern IMAGEHEADER ImageHeaderArray[];
#if MaxImageGroups>1
extern int NumImagesArray[];
#else
extern int NumImages;
#endif

void PlayFMV(char *filenamePtr);

void FindLightingValueFromFMV(unsigned short *bufferPtr);
void FindLightingValuesFromTriggeredFMV(unsigned char *bufferPtr, FMVTEXTURE *ftPtr);

int SmackerSoundVolume=ONE_FIXED/512;
int MoviesAreActive;
int IntroOutroMoviesAreActive=1;

int FmvColourRed;
int FmvColourGreen;
int FmvColourBlue;

void ReleaseFMVTexture(FMVTEXTURE *ftPtr);


/* KJL 12:45:23 10/08/98 - FMVTEXTURE stuff */
#define MAX_NO_FMVTEXTURES 10
FMVTEXTURE FMVTexture[MAX_NO_FMVTEXTURES];
int NumberOfFMVTextures;

/* ── Android FFmpeg helpers ──────────────────────────────────────────────── */

#define FMV_AVIO_BUF_SIZE 32768

static int fmv_read_packet(void *opaque, uint8_t *buf, int buf_size)
{
	FILE *f = (FILE *)opaque;
	int n = (int)fread(buf, 1, (size_t)buf_size, f);
	return (n == 0) ? AVERROR_EOF : n;
}

static int64_t fmv_seek(void *opaque, int64_t offset, int whence)
{
	FILE *f = (FILE *)opaque;
	if (whence == AVSEEK_SIZE) {
		long pos = ftell(f);
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		fseek(f, pos, SEEK_SET);
		return (int64_t)size;
	}
	int origin = (whence == SEEK_SET) ? SEEK_SET :
	             (whence == SEEK_CUR) ? SEEK_CUR : SEEK_END;
	if (fseek(f, (long)offset, origin) != 0)
		return -1;
	return (int64_t)ftell(f);
}

static void fmv_close_decoder(FMVTEXTURE *ftPtr)
{
	if (ftPtr->fmv_sdl_audio) {
		SDL_DestroyAudioStream((SDL_AudioStream *)ftPtr->fmv_sdl_audio);
		ftPtr->fmv_sdl_audio = NULL;
	}
	if (ftPtr->fmv_audio_frame) {
		AVFrame *af = (AVFrame *)ftPtr->fmv_audio_frame;
		av_frame_free(&af);
		ftPtr->fmv_audio_frame = NULL;
	}
	if (ftPtr->fmv_audio_codec_ctx) {
		AVCodecContext *actx = (AVCodecContext *)ftPtr->fmv_audio_codec_ctx;
		avcodec_free_context(&actx);
		ftPtr->fmv_audio_codec_ctx = NULL;
	}
	ftPtr->fmv_audio_stream_idx  = -1;
	ftPtr->fmv_frame_duration_ms = 0;
	ftPtr->fmv_next_frame_ms     = 0;

	if (ftPtr->fmv_packet) {
		AVPacket *pkt = (AVPacket *)ftPtr->fmv_packet;
		av_packet_free(&pkt);
		ftPtr->fmv_packet = NULL;
	}
	if (ftPtr->fmv_frame) {
		AVFrame *frm = (AVFrame *)ftPtr->fmv_frame;
		av_frame_free(&frm);
		ftPtr->fmv_frame = NULL;
	}
	if (ftPtr->fmv_sws_ctx) {
		sws_freeContext((struct SwsContext *)ftPtr->fmv_sws_ctx);
		ftPtr->fmv_sws_ctx = NULL;
	}
	if (ftPtr->fmv_codec_ctx) {
		AVCodecContext *ctx = (AVCodecContext *)ftPtr->fmv_codec_ctx;
		avcodec_free_context(&ctx);
		ftPtr->fmv_codec_ctx = NULL;
	}
	if (ftPtr->fmv_fmt_ctx) {
		/* AVFMT_FLAG_CUSTOM_IO was set — avformat_close_input won't touch pb */
		AVFormatContext *fmt = (AVFormatContext *)ftPtr->fmv_fmt_ctx;
		avformat_close_input(&fmt);
		ftPtr->fmv_fmt_ctx = NULL;
	}
	if (ftPtr->fmv_avio_ctx) {
		AVIOContext *avio = (AVIOContext *)ftPtr->fmv_avio_ctx;
		av_freep(&avio->buffer);
		avio_context_free(&avio);
		ftPtr->fmv_avio_ctx = NULL;
	}
	if (ftPtr->fmv_file) {
		fclose((FILE *)ftPtr->fmv_file);
		ftPtr->fmv_file = NULL;
	}
	ftPtr->fmv_avio_buf    = NULL;
	ftPtr->fmv_stream_idx  = -1;
	ftPtr->fmv_active      = 0;
	ftPtr->fmv_frame_number = 0;
}

static void fmv_open_file(FMVTEXTURE *ftPtr, const char *path)
{
	/* FixFilename in files.c starts tolower() at the SECOND char of the
	   filename, so "FMVs/..." survives as "Fmvs/..." and misses the dir.
	   Pre-lowercase the whole path so OpenGameFile always finds it. */
	char lpath[64];
	int k;
	for (k = 0; path[k] && k < (int)sizeof(lpath) - 1; k++)
		lpath[k] = (char)tolower((unsigned char)path[k]);
	lpath[k] = '\0';

	FMV_LOG("fmv_open_file: trying '%s'", lpath);
	fmv_close_decoder(ftPtr);

	FILE *f = OpenGameFile(lpath, FILEMODE_READONLY, FILETYPE_PERM);
	if (!f) {
		FMV_LOG("fmv_open_file: OpenGameFile failed for '%s'", lpath);
		return;
	}
	FMV_LOG("fmv_open_file: file opened OK");

	uint8_t *avio_buf = (uint8_t *)av_malloc(FMV_AVIO_BUF_SIZE);
	if (!avio_buf) {
		fclose(f);
		return;
	}

	AVIOContext *avio_ctx = avio_alloc_context(
		avio_buf, FMV_AVIO_BUF_SIZE, 0, f,
		fmv_read_packet, NULL, fmv_seek);
	if (!avio_ctx) {
		av_free(avio_buf);
		fclose(f);
		return;
	}

	AVFormatContext *fmt_ctx = avformat_alloc_context();
	if (!fmt_ctx) {
		av_freep(&avio_ctx->buffer);
		avio_context_free(&avio_ctx);
		fclose(f);
		return;
	}
	fmt_ctx->pb     = avio_ctx;
	fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

	if (avformat_open_input(&fmt_ctx, NULL, NULL, NULL) < 0) {
		FMV_LOG("fmv_open_file: avformat_open_input failed");
		/* fmt_ctx freed; avio_ctx spared because AVFMT_FLAG_CUSTOM_IO was set */
		av_freep(&avio_ctx->buffer);
		avio_context_free(&avio_ctx);
		fclose(f);
		return;
	}
	FMV_LOG("fmv_open_file: format opened, %u streams", fmt_ctx->nb_streams);

	/* From here, store partial state so fmv_close_decoder can clean up on any error */
	ftPtr->fmv_file     = f;
	ftPtr->fmv_avio_buf = avio_buf;
	ftPtr->fmv_avio_ctx = avio_ctx;
	ftPtr->fmv_fmt_ctx  = fmt_ctx;

	if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
		FMV_LOG("fmv_open_file: find_stream_info failed");
		fmv_close_decoder(ftPtr);
		return;
	}

	int stream_idx = -1;
	for (int i = 0; i < (int)fmt_ctx->nb_streams; i++) {
		if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			stream_idx = i;
			break;
		}
	}
	if (stream_idx < 0) {
		FMV_LOG("fmv_open_file: no video stream found");
		fmv_close_decoder(ftPtr);
		return;
	}
	ftPtr->fmv_stream_idx = stream_idx;

	AVCodecParameters *codecpar = fmt_ctx->streams[stream_idx]->codecpar;
	const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
	if (!codec) {
		FMV_LOG("fmv_open_file: no decoder for codec_id %d", codecpar->codec_id);
		fmv_close_decoder(ftPtr);
		return;
	}
	FMV_LOG("fmv_open_file: codec='%s' pix_fmt=%d %dx%d",
	        codec->name, codecpar->format,
	        codecpar->width, codecpar->height);

	AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx) {
		fmv_close_decoder(ftPtr);
		return;
	}
	ftPtr->fmv_codec_ctx = codec_ctx;

	if (avcodec_parameters_to_context(codec_ctx, codecpar) < 0
	    || avcodec_open2(codec_ctx, codec, NULL) < 0) {
		fmv_close_decoder(ftPtr);
		return;
	}

	struct SwsContext *sws_ctx = sws_getContext(
		codec_ctx->width, codec_ctx->height, codec_ctx->pix_fmt,
		128, 96, AV_PIX_FMT_RGBA,
		SWS_BILINEAR, NULL, NULL, NULL);
	if (!sws_ctx) {
		fmv_close_decoder(ftPtr);
		return;
	}
	ftPtr->fmv_sws_ctx = sws_ctx;

	AVFrame *frame = av_frame_alloc();
	if (!frame) {
		fmv_close_decoder(ftPtr);
		return;
	}
	ftPtr->fmv_frame = frame;

	AVPacket *packet = av_packet_alloc();
	if (!packet) {
		fmv_close_decoder(ftPtr);
		return;
	}
	ftPtr->fmv_packet       = packet;
	ftPtr->fmv_active       = 1;
	ftPtr->fmv_frame_number = 0;

	/* Frame timing — derive ms/frame from avg_frame_rate */
	{
		AVRational fr = fmt_ctx->streams[stream_idx]->avg_frame_rate;
		ftPtr->fmv_frame_duration_ms = (fr.num > 0 && fr.den > 0)
		    ? (int)(1000 * fr.den / fr.num) : 70;
		ftPtr->fmv_next_frame_ms = (unsigned int)SDL_GetTicks();
		ftPtr->fmv_start_ms      = ftPtr->fmv_next_frame_ms;
		FMV_LOG("fmv_open_file: fps=%d/%d → %dms/frame",
		        fr.num, fr.den, ftPtr->fmv_frame_duration_ms);
	}

	/* Audio stream */
	{
		int audio_idx = -1;
		int i;
		for (i = 0; i < (int)fmt_ctx->nb_streams; i++) {
			if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
				audio_idx = i;
				break;
			}
		}
		ftPtr->fmv_audio_stream_idx = audio_idx;
		if (audio_idx >= 0) {
			AVCodecParameters *apar = fmt_ctx->streams[audio_idx]->codecpar;
			const AVCodec *acodec = avcodec_find_decoder(apar->codec_id);
			if (acodec) {
				AVCodecContext *actx = avcodec_alloc_context3(acodec);
				if (actx) {
					ftPtr->fmv_audio_codec_ctx = actx;
					if (avcodec_parameters_to_context(actx, apar) >= 0 &&
					    avcodec_open2(actx, acodec, NULL) >= 0) {
						AVFrame *aframe = av_frame_alloc();
						ftPtr->fmv_audio_frame = aframe;
						SDL_InitSubSystem(SDL_INIT_AUDIO);
						/* Map FFmpeg sample format to SDL — use packed equivalent
						   so planar (S16P, FLTP) maps to the same SDL format */
						SDL_AudioFormat sdl_fmt;
						switch (av_get_packed_sample_fmt(actx->sample_fmt)) {
							case AV_SAMPLE_FMT_U8:  sdl_fmt = SDL_AUDIO_U8;  break;
							case AV_SAMPLE_FMT_S32: sdl_fmt = SDL_AUDIO_S32; break;
							case AV_SAMPLE_FMT_FLT: sdl_fmt = SDL_AUDIO_F32; break;
							default:                sdl_fmt = SDL_AUDIO_S16; break;
						}
						SDL_AudioSpec spec;
						spec.format   = sdl_fmt;
						spec.channels = actx->ch_layout.nb_channels;
						spec.freq     = actx->sample_rate;
						SDL_AudioStream *astream = SDL_OpenAudioDeviceStream(
						    SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
						if (astream) {
							SDL_ResumeAudioStreamDevice(astream);
							ftPtr->fmv_sdl_audio = astream;
							FMV_LOG("fmv_open_file: audio codec=%s avfmt=%d sdlfmt=%d ch=%d rate=%d",
							        acodec->name, actx->sample_fmt, (int)sdl_fmt,
							        spec.channels, spec.freq);
						} else {
							FMV_LOG("fmv_open_file: SDL audio open failed: %s",
							        SDL_GetError());
						}
					}
				}
			}
		}
	}

	FMV_LOG("fmv_open_file: SUCCESS — decoder ready");
}

/* Decode one video frame into ftPtr->RGBBuf (RGBA, 128×96).
   Returns 1 if a new frame was produced, 0 if not yet time or on error. */
static int fmv_decode_next_frame(FMVTEXTURE *ftPtr)
{
	/* Frame-rate throttle: don't decode until it's time for the next frame */
	if (ftPtr->fmv_frame_duration_ms > 0 &&
	    (unsigned int)SDL_GetTicks() < ftPtr->fmv_next_frame_ms)
		return 0;

	AVFormatContext   *fmt_ctx   = (AVFormatContext *)  ftPtr->fmv_fmt_ctx;
	AVCodecContext    *codec_ctx = (AVCodecContext *)   ftPtr->fmv_codec_ctx;
	struct SwsContext *sws_ctx   = (struct SwsContext *)ftPtr->fmv_sws_ctx;
	AVFrame           *frame     = (AVFrame *)          ftPtr->fmv_frame;
	AVPacket          *packet    = (AVPacket *)         ftPtr->fmv_packet;
	int stream_idx               = ftPtr->fmv_stream_idx;
	int audio_idx                = ftPtr->fmv_audio_stream_idx;
	AVCodecContext  *actx        = (AVCodecContext *)  ftPtr->fmv_audio_codec_ctx;
	AVFrame         *aframe      = (AVFrame *)         ftPtr->fmv_audio_frame;
	SDL_AudioStream *sdl_audio   = (SDL_AudioStream *) ftPtr->fmv_sdl_audio;
	for (;;) {
		int ret = av_read_frame(fmt_ctx, packet);
		if (ret == AVERROR_EOF) {
			return -1; /* -1 = video finished, play once only */
		}
		if (ret < 0)
			return 0;

		/* Audio packet — decode and push to SDL */
		if (packet->stream_index == audio_idx && actx && aframe) {
			avcodec_send_packet(actx, packet);
			av_packet_unref(packet);
			while (avcodec_receive_frame(actx, aframe) == 0) {
				if (sdl_audio && aframe->data[0]) {
					int bps = av_get_bytes_per_sample(
					              (enum AVSampleFormat)aframe->format);
					int bytes = av_sample_fmt_is_planar(
					                (enum AVSampleFormat)aframe->format)
					    ? aframe->nb_samples * bps               /* planar: data[0] = ch0 */
					    : aframe->nb_samples * bps               /* packed: all channels */
					      * actx->ch_layout.nb_channels;
					SDL_PutAudioStreamData(sdl_audio, aframe->data[0], bytes);
				}
				av_frame_unref(aframe);
			}
			continue;
		}

		if (packet->stream_index != stream_idx) {
			av_packet_unref(packet);
			continue;
		}

		ret = avcodec_send_packet(codec_ctx, packet);
		av_packet_unref(packet);
		if (ret < 0)
			continue;

		ret = avcodec_receive_frame(codec_ctx, frame);
		if (ret == AVERROR(EAGAIN))
			continue;
		if (ret < 0)
			return 0;

		/* Grab PTS before unref for sync calculation */
		int64_t pts = (frame->pts != AV_NOPTS_VALUE)
		              ? frame->pts : frame->best_effort_timestamp;
		AVRational tb = fmt_ctx->streams[stream_idx]->time_base;

		uint8_t *dst[1]        = { ftPtr->RGBBuf };
		int      dst_stride[1] = { 128 * 4 };
		sws_scale(sws_ctx,
		          (const uint8_t * const *)frame->data, frame->linesize,
		          0, frame->height,
		          dst, dst_stride);

		/* PAL8→RGBA: sws_scale leaves alpha = 0; force fully opaque */
		{
			unsigned char *a = ftPtr->RGBBuf + 3;
			int n;
			for (n = 0; n < 128 * 96; n++, a += 4)
				*a = 255;
		}

		av_frame_unref(frame);
		ftPtr->fmv_frame_number++;

		/* Set next throttle using PTS so video stays anchored to the
		   container clock instead of drifting via fixed accumulation */
		{
			int64_t pts_ms = (pts * 1000LL * tb.num) / tb.den;
			ftPtr->fmv_next_frame_ms = ftPtr->fmv_start_ms
			                         + (unsigned int)pts_ms
			                         + (unsigned int)ftPtr->fmv_frame_duration_ms;
		}
		return 1;
	}
}

static void fmv_compute_lighting_rgb(FMVTEXTURE *ftPtr)
{
	unsigned int totalRed = 0, totalGreen = 0, totalBlue = 0;
	unsigned char *p = ftPtr->RGBBuf;
	int i, pixels = 128 * 96;
	for (i = 0; i < pixels; i++, p += 4) {  /* RGBA: 4 bytes/pixel */
		totalRed   += p[0];
		totalGreen += p[1];
		totalBlue  += p[2];
	}
	FmvColourRed   = (int)(totalRed   / 48 * 16);
	FmvColourGreen = (int)(totalGreen / 48 * 16);
	FmvColourBlue  = (int)(totalBlue  / 48 * 16);
}

/* ── Menu music (audio-only looped SMK playback) ────────────────────────── */
static FMVTEXTURE menu_music_ftex;

static void fmv_open_audio_only(FMVTEXTURE *ftPtr, const char *path)
{
	char lpath[64];
	int k;
	for (k = 0; path[k] && k < (int)sizeof(lpath) - 1; k++)
		lpath[k] = (char)tolower((unsigned char)path[k]);
	lpath[k] = '\0';

	FMV_LOG("menu_music: opening '%s'", lpath);
	fmv_close_decoder(ftPtr);

	FILE *f = OpenGameFile(lpath, FILEMODE_READONLY, FILETYPE_PERM);
	if (!f) { FMV_LOG("menu_music: OpenGameFile failed for '%s'", lpath); return; }

	uint8_t *avio_buf = (uint8_t *)av_malloc(FMV_AVIO_BUF_SIZE);
	if (!avio_buf) { fclose(f); return; }

	AVIOContext *avio_ctx = avio_alloc_context(
		avio_buf, FMV_AVIO_BUF_SIZE, 0, f, fmv_read_packet, NULL, fmv_seek);
	if (!avio_ctx) { av_free(avio_buf); fclose(f); return; }

	AVFormatContext *fmt_ctx = avformat_alloc_context();
	if (!fmt_ctx) {
		av_freep(&avio_ctx->buffer);
		avio_context_free(&avio_ctx);
		fclose(f);
		return;
	}
	fmt_ctx->pb     = avio_ctx;
	fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

	if (avformat_open_input(&fmt_ctx, NULL, NULL, NULL) < 0) {
		av_freep(&avio_ctx->buffer);
		avio_context_free(&avio_ctx);
		fclose(f);
		return;
	}

	ftPtr->fmv_file       = f;
	ftPtr->fmv_avio_buf   = avio_buf;
	ftPtr->fmv_avio_ctx   = avio_ctx;
	ftPtr->fmv_fmt_ctx    = fmt_ctx;
	ftPtr->fmv_stream_idx = -1;

	if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
		fmv_close_decoder(ftPtr);
		return;
	}

	int audio_idx = -1;
	int i;
	for (i = 0; i < (int)fmt_ctx->nb_streams; i++) {
		if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			audio_idx = i;
			break;
		}
	}
	if (audio_idx < 0) {
		FMV_LOG("menu_music: no audio stream in '%s'", lpath);
		fmv_close_decoder(ftPtr);
		return;
	}
	ftPtr->fmv_audio_stream_idx = audio_idx;

	AVCodecParameters *apar = fmt_ctx->streams[audio_idx]->codecpar;
	const AVCodec *acodec = avcodec_find_decoder(apar->codec_id);
	if (!acodec) {
		FMV_LOG("menu_music: no decoder for audio codec_id %d", apar->codec_id);
		fmv_close_decoder(ftPtr);
		return;
	}

	AVCodecContext *actx = avcodec_alloc_context3(acodec);
	if (!actx) { fmv_close_decoder(ftPtr); return; }
	ftPtr->fmv_audio_codec_ctx = actx;

	if (avcodec_parameters_to_context(actx, apar) < 0 ||
	    avcodec_open2(actx, acodec, NULL) < 0) {
		fmv_close_decoder(ftPtr);
		return;
	}

	AVFrame *aframe = av_frame_alloc();
	if (!aframe) { fmv_close_decoder(ftPtr); return; }
	ftPtr->fmv_audio_frame = aframe;

	AVPacket *pkt = av_packet_alloc();
	if (!pkt) { fmv_close_decoder(ftPtr); return; }
	ftPtr->fmv_packet = pkt;

	SDL_InitSubSystem(SDL_INIT_AUDIO);
	SDL_AudioFormat sdl_fmt;
	switch (av_get_packed_sample_fmt(actx->sample_fmt)) {
		case AV_SAMPLE_FMT_U8:  sdl_fmt = SDL_AUDIO_U8;  break;
		case AV_SAMPLE_FMT_S32: sdl_fmt = SDL_AUDIO_S32; break;
		case AV_SAMPLE_FMT_FLT: sdl_fmt = SDL_AUDIO_F32; break;
		default:                sdl_fmt = SDL_AUDIO_S16; break;
	}
	SDL_AudioSpec spec;
	spec.format   = sdl_fmt;
	spec.channels = actx->ch_layout.nb_channels;
	spec.freq     = actx->sample_rate;
	SDL_AudioStream *astream = SDL_OpenAudioDeviceStream(
		SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
	if (!astream) {
		FMV_LOG("menu_music: SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
		fmv_close_decoder(ftPtr);
		return;
	}
	SDL_ResumeAudioStreamDevice(astream);
	ftPtr->fmv_sdl_audio = astream;
	ftPtr->fmv_active    = 1;
	FMV_LOG("menu_music: ready — codec=%s ch=%d freq=%d",
	        acodec->name, spec.channels, spec.freq);
}

static void fmv_pump_menu_audio(void)
{
	FMVTEXTURE *ftPtr = &menu_music_ftex;
	if (!ftPtr->fmv_active || !ftPtr->fmv_sdl_audio) return;

	SDL_AudioStream *sdlaudio = (SDL_AudioStream *)ftPtr->fmv_sdl_audio;
	/* Don't over-buffer: keep at most ~65 KB queued (~0.75 s at 44100 stereo S16) */
	if (SDL_GetAudioStreamAvailable(sdlaudio) > 65536) return;

	AVFormatContext *fmt_ctx = (AVFormatContext *)ftPtr->fmv_fmt_ctx;
	AVCodecContext  *actx    = (AVCodecContext  *)ftPtr->fmv_audio_codec_ctx;
	AVFrame         *aframe  = (AVFrame         *)ftPtr->fmv_audio_frame;
	AVPacket        *packet  = (AVPacket        *)ftPtr->fmv_packet;
	int audio_idx = ftPtr->fmv_audio_stream_idx;

	int tries;
	for (tries = 0; tries < 64; tries++) {
		int ret = av_read_frame(fmt_ctx, packet);
		if (ret == AVERROR_EOF) {
			/* Loop: seek back to start and flush the audio decoder */
			av_seek_frame(fmt_ctx, -1, 0, AVSEEK_FLAG_BACKWARD);
			avcodec_flush_buffers(actx);
			return;
		}
		if (ret < 0) return;

		if (packet->stream_index != audio_idx) {
			av_packet_unref(packet);
			continue;
		}

		avcodec_send_packet(actx, packet);
		av_packet_unref(packet);
		while (avcodec_receive_frame(actx, aframe) == 0) {
			if (aframe->data[0]) {
				int bps   = av_get_bytes_per_sample((enum AVSampleFormat)aframe->format);
				int bytes = av_sample_fmt_is_planar((enum AVSampleFormat)aframe->format)
				    ? aframe->nb_samples * bps
				    : aframe->nb_samples * bps * actx->ch_layout.nb_channels;
				SDL_PutAudioStreamData(sdlaudio, aframe->data[0], bytes);
			}
			av_frame_unref(aframe);
		}
		return; /* one audio packet per call — keeps pump cost predictable */
	}
}

/* ── Full-screen blocking BIK/SMK video playback ───────────────────────── */
static unsigned char bik_frame_rgba[640 * 480 * 4]; /* ~1.2 MB decode scratch, shared */

void PlayBinkedFMV(char *filenamePtr)
{
	char lpath[64];
	int k, i, vid_idx = -1, aud_idx = -1;
	FILE *f                  = NULL;
	uint8_t *avio_buf        = NULL;
	AVIOContext *avio_ctx    = NULL;
	AVFormatContext *fmt_ctx = NULL;
	AVCodecContext *vctx = NULL, *actx = NULL;
	struct SwsContext *sws   = NULL;
	AVFrame *vframe = NULL, *aframe = NULL;
	AVPacket *packet         = NULL;
	SDL_AudioStream *audio   = NULL;
	AVRational vtb, vfr;
	int frame_ms, out_w = 640, out_h = 480, x_off = 0, y_off = 0;
	unsigned int sms, nfms;
	extern SDL_Surface *surface;
	extern void FlipBuffers(void);
	extern void ReadJoysticks(void);

	if (!filenamePtr || !filenamePtr[0]) return;
	for (k = 0; filenamePtr[k] && k < (int)sizeof(lpath) - 1; k++)
		lpath[k] = (char)tolower((unsigned char)filenamePtr[k]);
	lpath[k] = '\0';
	FMV_LOG("PlayBinkedFMV: '%s'", lpath);

	/* Silence menu music while the video plays */
	if (menu_music_ftex.fmv_sdl_audio)
		SDL_PauseAudioStreamDevice((SDL_AudioStream *)menu_music_ftex.fmv_sdl_audio);

	f = OpenGameFile(lpath, FILEMODE_READONLY, FILETYPE_PERM);
	if (!f) { FMV_LOG("PlayBinkedFMV: cannot open '%s'", lpath); goto bik_done; }

	avio_buf = (uint8_t *)av_malloc(FMV_AVIO_BUF_SIZE);
	if (!avio_buf) goto bik_done;

	avio_ctx = avio_alloc_context(avio_buf, FMV_AVIO_BUF_SIZE, 0, f,
	                              fmv_read_packet, NULL, fmv_seek);
	if (!avio_ctx) { av_free(avio_buf); avio_buf = NULL; goto bik_done; }

	fmt_ctx = avformat_alloc_context();
	if (!fmt_ctx) goto bik_done;
	fmt_ctx->pb     = avio_ctx;
	fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
	/* avformat_open_input frees fmt_ctx + zeros it on failure */
	if (avformat_open_input(&fmt_ctx, NULL, NULL, NULL) < 0) goto bik_done;
	if (avformat_find_stream_info(fmt_ctx, NULL) < 0) goto bik_done;

	for (i = 0; i < (int)fmt_ctx->nb_streams; i++) {
		if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vid_idx < 0) vid_idx = i;
		if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && aud_idx < 0) aud_idx = i;
	}
	if (vid_idx < 0) goto bik_done;

	{
		AVCodecParameters *vpar = fmt_ctx->streams[vid_idx]->codecpar;
		const AVCodec *vc = avcodec_find_decoder(vpar->codec_id);
		if (!vc) goto bik_done;
		vctx = avcodec_alloc_context3(vc);
		if (!vctx) goto bik_done;
		if (avcodec_parameters_to_context(vctx, vpar) < 0 || avcodec_open2(vctx, vc, NULL) < 0) goto bik_done;
		/* Letterbox: fit source aspect ratio into 640×480 */
		out_w = 640; out_h = (640 * vctx->height + vctx->width / 2) / vctx->width;
		if (out_h > 480) { out_h = 480; out_w = (480 * vctx->width + vctx->height / 2) / vctx->height; }
		x_off = (640 - out_w) / 2;
		y_off = (480 - out_h) / 2;
		sws = sws_getContext(vctx->width, vctx->height, vctx->pix_fmt,
		                     out_w, out_h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
		if (!sws) goto bik_done;
	}
	vframe = av_frame_alloc();
	packet = av_packet_alloc();
	if (!vframe || !packet) goto bik_done;

	if (aud_idx >= 0) {
		AVCodecParameters *apar = fmt_ctx->streams[aud_idx]->codecpar;
		const AVCodec *ac = avcodec_find_decoder(apar->codec_id);
		if (ac) {
			actx = avcodec_alloc_context3(ac);
			if (actx && avcodec_parameters_to_context(actx, apar) >= 0 &&
			            avcodec_open2(actx, ac, NULL) >= 0) {
				aframe = av_frame_alloc();
				SDL_InitSubSystem(SDL_INIT_AUDIO);
				SDL_AudioFormat sfmt;
				switch (av_get_packed_sample_fmt(actx->sample_fmt)) {
					case AV_SAMPLE_FMT_U8:  sfmt = SDL_AUDIO_U8;  break;
					case AV_SAMPLE_FMT_S32: sfmt = SDL_AUDIO_S32; break;
					case AV_SAMPLE_FMT_FLT: sfmt = SDL_AUDIO_F32; break;
					default:                sfmt = SDL_AUDIO_S16; break;
				}
				SDL_AudioSpec spec;
				spec.format   = sfmt;
				spec.channels = actx->ch_layout.nb_channels;
				spec.freq     = actx->sample_rate;
				audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
				if (audio) SDL_ResumeAudioStreamDevice(audio);
			}
		}
	}

	vtb      = fmt_ctx->streams[vid_idx]->time_base;
	vfr      = fmt_ctx->streams[vid_idx]->avg_frame_rate;
	frame_ms = (vfr.num > 0 && vfr.den > 0) ? (int)(1000 * vfr.den / vfr.num) : 33;
	sms = nfms = (unsigned int)SDL_GetTicks();
	GotAnyKey = 0;

	for (;;) {
		int ret = av_read_frame(fmt_ctx, packet);
		if (ret == AVERROR_EOF || ret < 0) break;

		if (packet->stream_index == aud_idx && actx && aframe) {
			avcodec_send_packet(actx, packet);
			av_packet_unref(packet);
			while (avcodec_receive_frame(actx, aframe) == 0) {
				if (audio && aframe->data[0]) {
					int bps   = av_get_bytes_per_sample((enum AVSampleFormat)aframe->format);
					int bytes = av_sample_fmt_is_planar((enum AVSampleFormat)aframe->format)
					    ? aframe->nb_samples * bps
					    : aframe->nb_samples * bps * actx->ch_layout.nb_channels;
					SDL_PutAudioStreamData(audio, aframe->data[0], bytes);
				}
				av_frame_unref(aframe);
			}
		} else if (packet->stream_index == vid_idx) {
			avcodec_send_packet(vctx, packet);
			av_packet_unref(packet);
			if (avcodec_receive_frame(vctx, vframe) == 0) {
				unsigned int now = (unsigned int)SDL_GetTicks();
				if ((int)(nfms - now) > 2) SDL_Delay(nfms - now);

				uint8_t *dst[1] = { bik_frame_rgba };
				int  dst_s[1]   = { out_w * 4 };
				sws_scale(sws, (const uint8_t * const *)vframe->data, vframe->linesize,
				          0, vframe->height, dst, dst_s);

				if (surface) {
					/* Clear black bars, then blit centered frame */
					memset(surface->pixels, 0, 640 * 480 * 2);
					const unsigned char *src = bik_frame_rgba;
					int row;
					for (row = 0; row < out_h; row++) {
						Uint16 *px = (Uint16 *)surface->pixels + (y_off + row) * 640 + x_off;
						int col;
						for (col = 0; col < out_w; col++) {
							*px++ = ((src[0]>>3)<<11)|((src[1]>>2)<<5)|(src[2]>>3);
							src += 4;
						}
					}
				}

				{
					int64_t pts = (vframe->pts != AV_NOPTS_VALUE) ? vframe->pts : vframe->best_effort_timestamp;
					int64_t pms = (pts * 1000LL * vtb.num) / vtb.den;
					nfms = sms + (unsigned int)pms + (unsigned int)frame_ms;
				}
				av_frame_unref(vframe);
				FlipBuffers();
				ReadJoysticks();
				if (GotAnyKey) break;
			}
		} else {
			av_packet_unref(packet);
		}
	}

bik_done:
	if (audio)  SDL_DestroyAudioStream(audio);
	if (aframe) av_frame_free(&aframe);
	if (actx)   avcodec_free_context(&actx);
	if (packet) av_packet_free(&packet);
	if (vframe) av_frame_free(&vframe);
	if (sws)    sws_freeContext(sws);
	if (vctx)   avcodec_free_context(&vctx);
	if (fmt_ctx) avformat_close_input(&fmt_ctx);
	if (avio_ctx) { av_freep(&avio_ctx->buffer); avio_context_free(&avio_ctx); }
	if (f) fclose(f);

	GotAnyKey = 0; /* don't let a skip press bleed into the next menu frame */
	if (menu_music_ftex.fmv_sdl_audio)
		SDL_ResumeAudioStreamDevice((SDL_AudioStream *)menu_music_ftex.fmv_sdl_audio);
	FMV_LOG("PlayBinkedFMV: done '%s'", lpath);
}

/* ── Looping menu background video ─────────────────────────────────────── */
static FMVTEXTURE menu_bik_ftex;

static void fmv_open_background(FMVTEXTURE *ftPtr, const char *path)
{
	char lpath[64];
	int k, i;
	for (k = 0; path[k] && k < (int)sizeof(lpath) - 1; k++)
		lpath[k] = (char)tolower((unsigned char)path[k]);
	lpath[k] = '\0';

	FMV_LOG("fmv_open_background: '%s'", lpath);
	fmv_close_decoder(ftPtr);

	FILE *f = OpenGameFile(lpath, FILEMODE_READONLY, FILETYPE_PERM);
	if (!f) { FMV_LOG("fmv_open_background: cannot open '%s'", lpath); return; }

	uint8_t *avio_buf = (uint8_t *)av_malloc(FMV_AVIO_BUF_SIZE);
	if (!avio_buf) { fclose(f); return; }
	AVIOContext *avio_ctx = avio_alloc_context(avio_buf, FMV_AVIO_BUF_SIZE, 0, f,
	                                           fmv_read_packet, NULL, fmv_seek);
	if (!avio_ctx) { av_free(avio_buf); fclose(f); return; }

	AVFormatContext *fmt_ctx = avformat_alloc_context();
	if (!fmt_ctx) { av_freep(&avio_ctx->buffer); avio_context_free(&avio_ctx); fclose(f); return; }
	fmt_ctx->pb     = avio_ctx;
	fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;

	if (avformat_open_input(&fmt_ctx, NULL, NULL, NULL) < 0) {
		av_freep(&avio_ctx->buffer); avio_context_free(&avio_ctx); fclose(f); return;
	}
	ftPtr->fmv_file = f; ftPtr->fmv_avio_buf = avio_buf;
	ftPtr->fmv_avio_ctx = avio_ctx; ftPtr->fmv_fmt_ctx = fmt_ctx;

	if (avformat_find_stream_info(fmt_ctx, NULL) < 0) { fmv_close_decoder(ftPtr); return; }

	int vid_idx = -1, aud_idx = -1;
	for (i = 0; i < (int)fmt_ctx->nb_streams; i++) {
		if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vid_idx < 0) vid_idx = i;
		if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && aud_idx < 0) aud_idx = i;
	}
	if (vid_idx < 0) { fmv_close_decoder(ftPtr); return; }
	ftPtr->fmv_stream_idx = vid_idx;

	{
		AVCodecParameters *vpar = fmt_ctx->streams[vid_idx]->codecpar;
		const AVCodec *vc = avcodec_find_decoder(vpar->codec_id);
		if (!vc) { fmv_close_decoder(ftPtr); return; }
		AVCodecContext *vctx = avcodec_alloc_context3(vc);
		if (!vctx) { fmv_close_decoder(ftPtr); return; }
		ftPtr->fmv_codec_ctx = vctx;
		if (avcodec_parameters_to_context(vctx, vpar) < 0 || avcodec_open2(vctx, vc, NULL) < 0)
			{ fmv_close_decoder(ftPtr); return; }
		struct SwsContext *sws = sws_getContext(vctx->width, vctx->height, vctx->pix_fmt,
		                                        640, 480, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
		if (!sws) { fmv_close_decoder(ftPtr); return; }
		ftPtr->fmv_sws_ctx = sws;
	}

	AVFrame *vf = av_frame_alloc();  if (!vf)  { fmv_close_decoder(ftPtr); return; }
	AVPacket *pk = av_packet_alloc(); if (!pk) { av_frame_free(&vf); fmv_close_decoder(ftPtr); return; }
	ftPtr->fmv_frame = vf;
	ftPtr->fmv_packet = pk;

	if (aud_idx >= 0) {
		ftPtr->fmv_audio_stream_idx = aud_idx;
		AVCodecParameters *apar = fmt_ctx->streams[aud_idx]->codecpar;
		const AVCodec *ac = avcodec_find_decoder(apar->codec_id);
		if (ac) {
			AVCodecContext *actx = avcodec_alloc_context3(ac);
			if (actx) {
				ftPtr->fmv_audio_codec_ctx = actx;
				if (avcodec_parameters_to_context(actx, apar) >= 0 && avcodec_open2(actx, ac, NULL) >= 0) {
					AVFrame *af = av_frame_alloc();
					ftPtr->fmv_audio_frame = af;
					SDL_InitSubSystem(SDL_INIT_AUDIO);
					SDL_AudioFormat sfmt;
					switch (av_get_packed_sample_fmt(actx->sample_fmt)) {
						case AV_SAMPLE_FMT_U8:  sfmt = SDL_AUDIO_U8;  break;
						case AV_SAMPLE_FMT_S32: sfmt = SDL_AUDIO_S32; break;
						case AV_SAMPLE_FMT_FLT: sfmt = SDL_AUDIO_F32; break;
						default:                sfmt = SDL_AUDIO_S16; break;
					}
					SDL_AudioSpec spec;
					spec.format = sfmt; spec.channels = actx->ch_layout.nb_channels; spec.freq = actx->sample_rate;
					SDL_AudioStream *as = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
					if (as) { SDL_ResumeAudioStreamDevice(as); ftPtr->fmv_sdl_audio = as; }
				}
			}
		}
	}

	{
		AVRational fr = fmt_ctx->streams[vid_idx]->avg_frame_rate;
		ftPtr->fmv_frame_duration_ms = (fr.num > 0 && fr.den > 0) ? (int)(1000 * fr.den / fr.num) : 33;
		ftPtr->fmv_start_ms = ftPtr->fmv_next_frame_ms = (unsigned int)SDL_GetTicks();
	}
	ftPtr->fmv_active = 1;
	FMV_LOG("fmv_open_background: ready");
}

static int fmv_render_bik_frame(FMVTEXTURE *ftPtr)
{
	/* Returns 1 if the surface has a valid video frame (new or held). */
	if (!ftPtr->fmv_active) return 0;

	/* Still within current frame's display window — nothing new to decode */
	if ((int)((unsigned int)SDL_GetTicks() - ftPtr->fmv_next_frame_ms) < 0) return 1;

	AVFormatContext   *fmt_ctx = (AVFormatContext   *)ftPtr->fmv_fmt_ctx;
	AVCodecContext    *vctx    = (AVCodecContext    *)ftPtr->fmv_codec_ctx;
	struct SwsContext *sws     = (struct SwsContext *)ftPtr->fmv_sws_ctx;
	AVFrame           *vframe  = (AVFrame           *)ftPtr->fmv_frame;
	AVFrame           *aframe  = (AVFrame           *)ftPtr->fmv_audio_frame;
	AVCodecContext    *actx    = (AVCodecContext    *)ftPtr->fmv_audio_codec_ctx;
	SDL_AudioStream   *audio   = (SDL_AudioStream   *)ftPtr->fmv_sdl_audio;
	AVPacket          *packet  = (AVPacket          *)ftPtr->fmv_packet;
	int vid_idx = ftPtr->fmv_stream_idx;
	int aud_idx = ftPtr->fmv_audio_stream_idx;
	AVRational vtb = fmt_ctx->streams[vid_idx]->time_base;
	extern SDL_Surface *surface;

	int tries;
	for (tries = 0; tries < 128; tries++) {
		int ret = av_read_frame(fmt_ctx, packet);
		if (ret == AVERROR_EOF) {
			av_seek_frame(fmt_ctx, vid_idx, 0, AVSEEK_FLAG_BACKWARD);
			avcodec_flush_buffers(vctx);
			if (actx) avcodec_flush_buffers(actx);
			ftPtr->fmv_start_ms = ftPtr->fmv_next_frame_ms = (unsigned int)SDL_GetTicks();
			return 1; /* keep showing last frame while seeking */
		}
		if (ret < 0) return 1;

		if (packet->stream_index == aud_idx && actx && aframe) {
			avcodec_send_packet(actx, packet);
			av_packet_unref(packet);
			while (avcodec_receive_frame(actx, aframe) == 0) {
				if (audio && aframe->data[0]) {
					int bps   = av_get_bytes_per_sample((enum AVSampleFormat)aframe->format);
					int bytes = av_sample_fmt_is_planar((enum AVSampleFormat)aframe->format)
					    ? aframe->nb_samples * bps
					    : aframe->nb_samples * bps * actx->ch_layout.nb_channels;
					SDL_PutAudioStreamData(audio, aframe->data[0], bytes);
				}
				av_frame_unref(aframe);
			}
			continue;
		}

		if (packet->stream_index != vid_idx) { av_packet_unref(packet); continue; }

		avcodec_send_packet(vctx, packet);
		av_packet_unref(packet);
		if (avcodec_receive_frame(vctx, vframe) == 0) {
			uint8_t *dst[1] = { bik_frame_rgba };
			int  dst_s[1]   = { 640 * 4 };
			sws_scale(sws, (const uint8_t * const *)vframe->data, vframe->linesize,
			          0, vframe->height, dst, dst_s);
			if (surface) {
				const unsigned char *src = bik_frame_rgba;
				Uint16 *px = (Uint16 *)surface->pixels;
				int n = 640 * 480;
				while (n--) { *px++ = ((src[0]>>3)<<11)|((src[1]>>2)<<5)|(src[2]>>3); src += 4; }
			}
			{
				int64_t pts = (vframe->pts != AV_NOPTS_VALUE) ? vframe->pts : vframe->best_effort_timestamp;
				int64_t pms = (pts * 1000LL * vtb.num) / vtb.den;
				ftPtr->fmv_next_frame_ms = ftPtr->fmv_start_ms + (unsigned int)pms
				                         + (unsigned int)ftPtr->fmv_frame_duration_ms;
			}
			av_frame_unref(vframe);
			return 1;
		}
	}
	return 1;
}

/* ─────────────────────────────────────────────────────────────────────────── */

void StartMenuMusic(void)
{

	if (!menu_music_ftex.fmv_active)
		fmv_open_audio_only(&menu_music_ftex, "fmvs/introsound.smk");

}

void PlayMenuMusic(void)
{

	fmv_pump_menu_audio();

}

void EndMenuMusic(void)
{

	fmv_close_decoder(&menu_music_ftex);

}

void StartMenuBackgroundBink(void)
{
#if 1
	if (!menu_bik_ftex.fmv_active);
		//fmv_open_background(&menu_bik_ftex, "fmvs/menubackground.bik");
#endif
}

int PlayMenuBackgroundBink(void)
{

	return fmv_render_bik_frame(&menu_bik_ftex);

}

void EndMenuBackgroundBink(void)
{

	fmv_close_decoder(&menu_bik_ftex);

}


void ScanImagesForFMVs(void)
{

	extern void SetupFMVTexture(FMVTEXTURE *ftPtr);
	int i;
	IMAGEHEADER *ihPtr;
	NumberOfFMVTextures=0;

	#if MaxImageGroups>1
	for (j=0; j<MaxImageGroups; j++)
	{
		if (NumImagesArray[j])
		{
			ihPtr = &ImageHeaderArray[j*MaxImages];
			for (i = 0; i<NumImagesArray[j]; i++, ihPtr++)
			{
	#else
	{
		if(NumImages)
		{
			ihPtr = &ImageHeaderArray[0];
			for (i = 0; i<NumImages; i++, ihPtr++)
			{
	#endif
				char *strPtr;
				if((strPtr = strstr(ihPtr->ImageName,"FMVs")))
				{
					char filename[30];
					{
						char *filenamePtr = filename;
						do
						{
							*filenamePtr++ = *strPtr;
						}
						while(*strPtr++!='.');

						*filenamePtr++='s';
						*filenamePtr++='m';
						*filenamePtr++='k';
						*filenamePtr=0;
					}

					{
						FMVTexture[NumberOfFMVTextures].IsTriggeredPlotFMV = 1;
					}

					{
						FMVTexture[NumberOfFMVTextures].ImagePtr = ihPtr;
						FMVTexture[NumberOfFMVTextures].StaticImageDrawn=0;
						SetupFMVTexture(&FMVTexture[NumberOfFMVTextures]);

						/* Open the video immediately — filename was built above */
						//fmv_open_file(&FMVTexture[NumberOfFMVTextures], filename);

						NumberOfFMVTextures++;
					}
				}
			}
		}
	}

#ifdef __ANDROID__
	//FMV_LOG("ScanImagesForFMVs: done, NumberOfFMVTextures=%d", NumberOfFMVTextures);
#endif
}

void UpdateAllFMVTextures(void)
{
	extern void UpdateFMVTexture(FMVTEXTURE *ftPtr);
	int i = NumberOfFMVTextures;

	while(i--)
	{
		UpdateFMVTexture(&FMVTexture[i]);
	}

}

void ReleaseAllFMVTextures(void)
{
	extern void UpdateFMVTexture(FMVTEXTURE *ftPtr);
	int i = NumberOfFMVTextures;

	while(i--)
	{
		ReleaseFMVTexture(&FMVTexture[i]);
	}

}


int NextFMVTextureFrame(FMVTEXTURE *ftPtr, void *bufferPtr)
{
	int w = 128;
	
	/* FFmpeg path — frame decoded directly into RGBBuf by UpdateFMVTexture */
	/*if (ftPtr->fmv_active) {
		return 1;
	}*/

	if (!ftPtr->StaticImageDrawn)
	{
		int i = w*96/4;
		unsigned int seed = FastRandom();
		int *ptr = (int*)bufferPtr;
		do
		{
			seed = ((seed*1664525)+1013904223);
			*ptr++ = seed;
		}
		while(--i);
	}
	FindLightingValuesFromTriggeredFMV((unsigned char*)bufferPtr,ftPtr);
	return 1;

}

void UpdateFMVTexturePalette(FMVTEXTURE *ftPtr)
{
	unsigned char *c;
	int i;
	
	if (MoviesAreActive && ftPtr->fmv_active)
	{
	}
	else
	{
		{
			unsigned int seed = FastRandom();
			for(i=0;i<256;i++)
			{
				seed = ((seed*1664525)+1013904223);
				ftPtr->SrcPalette[i].peRed   = (seed      ) & 0xFF;
				ftPtr->SrcPalette[i].peGreen = (seed >>  8) & 0xFF;
				ftPtr->SrcPalette[i].peBlue  = (seed >> 16) & 0xFF;
			}
		}
	}
}

extern void StartTriggerPlotFMV(int number)
{
#if 0
	#ifdef __ANDROID__
		char path[64];
		int i = NumberOfFMVTextures;
		snprintf(path, sizeof(path), "fmvs/message%d.smk", number);
		while (i--) {
			if (FMVTexture[i].IsTriggeredPlotFMV) {
				FMVTexture[i].MessageNumber = number;
				fmv_open_file(&FMVTexture[i], path);
			}
		}
	#else
		(void) number;
	#endif
#endif
	
	int i = NumberOfFMVTextures;
	char buffer[25];
	//char path[64];
	
	if (CheatMode_Active != CHEATMODE_NONACTIVE) return;
	
	sprintf(buffer, "fmvs/message%d.smk", number);
	#if 0
		{
			FILE* fmv_file = fopen(buffer,"rb");
			if(!fmv_file)
			{
				return;
			}
			fclose(fmv_file);
		}
	#endif
	while(i--)
	{
		if (FMVTexture[i].IsTriggeredPlotFMV) {
			FMVTexture[i].MessageNumber = number;
			fmv_open_file(&FMVTexture[i], buffer);
			//fmv_open_file(&FMVTexture[i], filename);
		}
	}
	
}

extern void StartFMVAtFrame(int number, int frame)
{
#if 0
	#ifdef __ANDROID__
		int i = NumberOfFMVTextures;
		(void) frame;
		while (i--) {
			if (FMVTexture[i].IsTriggeredPlotFMV && FMVTexture[i].fmv_active
				&& FMVTexture[i].MessageNumber == number) {
				AVFormatContext *fmt_ctx = (AVFormatContext *)FMVTexture[i].fmv_fmt_ctx;
				AVCodecContext *codec_ctx = (AVCodecContext *)FMVTexture[i].fmv_codec_ctx;
				int sidx = FMVTexture[i].fmv_stream_idx;
				av_seek_frame(fmt_ctx, sidx, 0, AVSEEK_FLAG_BACKWARD);
				avcodec_flush_buffers(codec_ctx);
				FMVTexture[i].fmv_frame_number = 0;
			}
		}
	#else
		(void) number; (void) frame;
	#endif
#endif
}

extern void GetFMVInformation(int *messageNumberPtr, int *frameNumberPtr)
{

	int i = NumberOfFMVTextures;
	while (i--) {
		if (FMVTexture[i].IsTriggeredPlotFMV) {
			*messageNumberPtr = FMVTexture[i].MessageNumber;
			*frameNumberPtr   = 0;
			return;
		}
	}

	*messageNumberPtr = 0;
	*frameNumberPtr   = 0;
}


extern void InitialiseTriggeredFMVs(void)
{
	int i = NumberOfFMVTextures;
	while (i--) {
		if (FMVTexture[i].IsTriggeredPlotFMV) {
			
			// Check if any active FFmpeg resources exist
			if (FMVTexture[i].fmv_fmt_ctx || FMVTexture[i].fmv_codec_ctx) {
				FMVTexture[i].MessageNumber = 0;
			}
			
			// 1. Free Video Decoder Context
			if (FMVTexture[i].fmv_codec_ctx) {
				AVCodecContext *codec_ctx = (AVCodecContext *)FMVTexture[i].fmv_codec_ctx;
				avcodec_free_context(&codec_ctx);
				FMVTexture[i].fmv_codec_ctx = NULL;
			}
			
			// 2. Free Audio Decoder Context (if applicable)
			if (FMVTexture[i].fmv_audio_codec_ctx) {
				AVCodecContext *audio_codec_ctx = (AVCodecContext *)FMVTexture[i].fmv_audio_codec_ctx;
				avcodec_free_context(&audio_codec_ctx);
				FMVTexture[i].fmv_audio_codec_ctx = NULL;
			}
			
			// 3. Close Demuxer / Input Container
			if (FMVTexture[i].fmv_fmt_ctx) {
				AVFormatContext *fmt_ctx = (AVFormatContext *)FMVTexture[i].fmv_fmt_ctx;
				avformat_close_input(&fmt_ctx);
				FMVTexture[i].fmv_fmt_ctx = NULL;
			}
			
			// 4. Free Scaling / Color Conversion Context
			if (FMVTexture[i].fmv_sws_ctx) {
				SwsContext *sws_ctx = (SwsContext *)FMVTexture[i].fmv_sws_ctx;
				sws_freeContext(sws_ctx);
				FMVTexture[i].fmv_sws_ctx = NULL;
			}
			
			// 5. Free Struct Buffers (Frames and Packets)
			if (FMVTexture[i].fmv_frame) {
				AVFrame *frame = (AVFrame *)FMVTexture[i].fmv_frame;
				av_frame_free(&frame);
				FMVTexture[i].fmv_frame = NULL;
			}
			if (FMVTexture[i].fmv_audio_frame) {
				AVFrame *audio_frame = (AVFrame *)FMVTexture[i].fmv_audio_frame;
				av_frame_free(&audio_frame);
				FMVTexture[i].fmv_audio_frame = NULL;
			}
			if (FMVTexture[i].fmv_packet) {
				AVPacket *packet = (AVPacket *)FMVTexture[i].fmv_packet;
				av_packet_free(&packet);
				FMVTexture[i].fmv_packet = NULL;
			}
			
			// 6. Free Custom IO layers if you used avio_alloc_context
			if (FMVTexture[i].fmv_avio_ctx) {
				AVIOContext *avio_ctx = (AVIOContext *)FMVTexture[i].fmv_avio_ctx;
				// Important: Only av_freep the internal buffer if FFmpeg didn't take ownership
				av_freep(&avio_ctx->buffer);
				avio_context_free(&avio_ctx);
				FMVTexture[i].fmv_avio_ctx = NULL;
			}
			if (FMVTexture[i].fmv_avio_buf) {
				// If it wasn't freed via avio_context_free above
				av_freep(&FMVTexture[i].fmv_avio_buf);
			}
			
			// 7. Close underlying file handle if opened manually via standard I/O
			if (FMVTexture[i].fmv_file) {
				fclose((FILE *)FMVTexture[i].fmv_file);
				FMVTexture[i].fmv_file = NULL;
			}
			
			// 8. Free SDL Audio Streams (if initialized)
			if (FMVTexture[i].fmv_sdl_audio) {
				// SDL3: SDL_DestroyAudioStream((SDL_AudioStream*)FMVTexture[i].fmv_sdl_audio);
				// SDL2: SDL_FreeAudioStream((SDL_AudioStream*)FMVTexture[i].fmv_sdl_audio);
				FMVTexture[i].fmv_sdl_audio = NULL;
			}
			
			// Reset primitive states safely
			FMVTexture[i].fmv_active            = 0;
			FMVTexture[i].fmv_stream_idx        = -1;
			FMVTexture[i].fmv_audio_stream_idx  = -1;
			FMVTexture[i].fmv_frame_number      = 0;
			FMVTexture[i].fmv_frame_duration_ms = 0;
			FMVTexture[i].fmv_next_frame_ms     = 0;
		}
	}
}

void FindLightingValuesFromTriggeredFMV(unsigned char *bufferPtr, FMVTEXTURE *ftPtr)
{
	unsigned int totalRed=0;
	unsigned int totalBlue=0;
	unsigned int totalGreen=0;

	int pixels = 128*96;
	unsigned int *source = (unsigned int*) (bufferPtr);
	do
	{
		int s = *source++;
		{
			int t = s&255;
			totalBlue += ftPtr->SrcPalette[t].peBlue;
			totalGreen += ftPtr->SrcPalette[t].peGreen;
			totalRed += ftPtr->SrcPalette[t].peRed;
		}
	}
	while(--pixels);

	FmvColourRed = totalRed/48*16;
	FmvColourGreen = totalGreen/48*16;
	FmvColourBlue = totalBlue/48*16;

}

void SetupFMVTexture(FMVTEXTURE *ftPtr)
{
	if (ftPtr->PalettedBuf == NULL)
	{
		ftPtr->PalettedBuf = (unsigned char*) calloc(1, 128*128+128*128*4);
	}

	if (ftPtr->RGBBuf == NULL)
	{
		if (ftPtr->PalettedBuf == NULL)
		{
			return;
		}

		ftPtr->RGBBuf = &ftPtr->PalettedBuf[128*128];
	}

#ifdef __ANDROID__
	ftPtr->fmv_stream_idx       = -1;
	ftPtr->fmv_audio_stream_idx = -1;
	/* All other fmv_* fields are zero-initialised in the global FMVTexture array */
#endif
}
// Originally in d3d_render.cpp
void UpdateFMVTexture(FMVTEXTURE *ftPtr)
{
#if 1
	if (ftPtr->fmv_active) {
		int decoded = fmv_decode_next_frame(ftPtr);
		if (decoded > 0) {
			unsigned int texid = ftPtr->ImagePtr->D3DTexture->id;
			fmv_compute_lighting_rgb(ftPtr);
			pglBindTexture(GL_TEXTURE_2D, texid);
			pglTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 128,
			                 GL_RGBA, GL_UNSIGNED_BYTE, ftPtr->RGBBuf);
			OGL_RegenerateMipmaps();
			if (ftPtr->fmv_frame_number <= 3) {
				/* center pixel at (64,48) = offset (48*128+64)*4 */
				int mid = (48 * 128 + 64) * 4;
				GLenum err = pglGetError();
				FMV_LOG("UpdateFMVTexture: frame %d texid=%u "
				        "tl=[%d,%d,%d] mid=[%d,%d,%d] glerr=%u",
				        ftPtr->fmv_frame_number, texid,
				        ftPtr->RGBBuf[0], ftPtr->RGBBuf[1], ftPtr->RGBBuf[2],
				        ftPtr->RGBBuf[mid], ftPtr->RGBBuf[mid+1], ftPtr->RGBBuf[mid+2],
				        (unsigned)err);
			}
			return;
		} else if (decoded < 0) {
			FMV_LOG("UpdateFMVTexture: video finished, falling through to static");
			fmv_close_decoder(ftPtr); /* sets fmv_active = 0, fall through */
		} else {
			return; /* throttled — not time for next frame yet */
		}
	}
#endif
	unsigned char *srcPtr;
	unsigned char *dstPtr;

	int pixels = 128*96;

	// get the next frame into the paletted buffer
	if (!NextFMVTextureFrame(ftPtr, &ftPtr->PalettedBuf[0]))
	{
		return;
	}

	// update the texture palette
	UpdateFMVTexturePalette(ftPtr);

	srcPtr = &ftPtr->PalettedBuf[0];
	dstPtr = &ftPtr->RGBBuf[0];

	// not using paletted textures, so convert to rgb manually
	do
	{
		unsigned char source = (*srcPtr++);
		dstPtr[0] = ftPtr->SrcPalette[source].peRed;
		dstPtr[1] = ftPtr->SrcPalette[source].peGreen;
		dstPtr[2] = ftPtr->SrcPalette[source].peBlue;
		dstPtr[3] = 255;

		dstPtr += 4;
	} while(--pixels);

//#warning move this into opengl.c
	// update the opengl texture
	pglBindTexture(GL_TEXTURE_2D, ftPtr->ImagePtr->D3DTexture->id);

	pglTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 128, 128, GL_RGBA, GL_UNSIGNED_BYTE, &ftPtr->RGBBuf[0]);
	OGL_RegenerateMipmaps();
}

void ReleaseFMVTexture(FMVTEXTURE *ftPtr)
{
	ftPtr->MessageNumber = 0;

#ifdef __ANDROID__
	fmv_close_decoder(ftPtr);
#endif

	if (ftPtr->PalettedBuf != NULL)
	{
		free(ftPtr->PalettedBuf);
		ftPtr->PalettedBuf = NULL;
	}

	ftPtr->RGBBuf = NULL;
}
