#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Before fixer.h, which drags in windows.h on MSVC — fmv.c orders it this way
   too, and that is the only ordering proven to build on Windows. These used to
   sit down in the (then Android-only) implementation block, where windows.h was
   never in the picture. */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/samplefmt.h>

#include <SDL3/SDL.h>

#include "fixer.h"
#include "win95/cd_player.h"
#include "cdplayer.h"

int CDPlayerVolume;

#if SDL_MAJOR_VERSION < 2
/* ------------------------------------------------------------------ */
/* Legacy SDL1/SDL2 CDROM path                                         */
/* ------------------------------------------------------------------ */
static int HaveCDROM = 0;
static SDL_CD *cdrom = NULL;

void CheckCDVolume() {}

void CDDA_Start()
{
	int numdrives;

	if (!HaveCDROM) {
		HaveCDROM = 1;
		SDL_InitSubSystem(SDL_INIT_CDROM);
	}

	if (cdrom != NULL)
		CDDA_End();

	numdrives = SDL_CDNumDrives();

	if (numdrives == 0)
		return;

	cdrom = SDL_CDOpen(0);
}

void CDDA_End()
{
	if (cdrom != NULL) {
		CDDA_Stop();
		SDL_CDClose(cdrom);
	}
	cdrom = NULL;
}

void CDDA_ChangeVolume(int volume)
{
	fprintf(stderr, "CDDA_ChangeVolume(%d)\n", volume);
}

int CDDA_CheckNumberOfTracks()
{
	if (cdrom == NULL)
		return 0;
	return cdrom->numtracks;
}

int CDDA_IsOn()
{
	return (cdrom != NULL);
}

int CDDA_IsPlaying()
{
	if (cdrom == NULL)
		return 0;
	return (SDL_CDStatus(cdrom) == CD_PLAYING);
}

void CDDA_Play(int CDDATrack)
{
	if (cdrom == NULL)
		return;
	if (CD_INDRIVE(SDL_CDStatus(cdrom))) {
		int track = CDDATrack - 1;
		int i;
		if (cdrom->numtracks == 0)
			return;
		track %= cdrom->numtracks;
		for (i = 0; i < cdrom->numtracks; i++) {
			if (cdrom->track[track].type == SDL_AUDIO_TRACK) {
				SDL_CDPlayTracks(cdrom, track, 0, 1, 0);
				return;
			}
			track++;
			track %= cdrom->numtracks;
		}
	}
}

void CDDA_PlayLoop(int CDDATrack)
{
	CDDA_Play(CDDATrack);
}

void CDDA_Stop()
{
	if (cdrom == NULL)
		return;
	if (CD_INDRIVE(SDL_CDStatus(cdrom)))
		SDL_CDStop(cdrom);
}

void CDDA_SwitchOn() {}

/* Nothing to pump: the SDL CD-ROM path plays the disc, not a decoded stream.
   Present only because SoundSys_Management() calls it unconditionally. */
void CDDA_Management(void) {}

/* ------------------------------------------------------------------ */
#else
/* ------------------------------------------------------------------ */
/* SDL3, every platform: decode OGG via FFmpeg, play via OpenAL        */
/*                                                                     */
/* Was Android-only, with desktop falling through to stubs that made   */
/* CDDA_HasMusicFiles() return 0 — so the menu said "No CD music       */
/* found" without ever looking at cd_tracks/. Nothing in here is       */
/* Android-specific: FFmpeg and OpenAL are linked on every target, and */
/* GetGlobalDir() is the game data folder that holds cd_tracks/ on all */
/* of them.                                                            */
/* ------------------------------------------------------------------ */

#include "al.h"
#include "alc.h"
#include "files.h"

/* Streaming playback: the track is decoded a chunk at a time into a small ring
 * of OpenAL buffers that CDDA_Management() recycles, rather than decoded whole
 * into one buffer up front. The old way cost ~40-50 MB and a ~0.4 s stall on
 * every track change, which is audible as a hitch when the music switches.
 *
 * Four buffers of half a second each keeps ~2 s queued ahead. The pump runs
 * from SoundSys_Management(), which the frontend, the loading screens and the
 * game loop all call, so the queue is serviced everywhere music can play. A gap
 * longer than the queue (a long blocking load) underruns; that is recovered
 * from in the pump rather than prevented, so it costs a glitch, not silence. */
#define CDDA_STREAM_BUFFERS 4
#define CDDA_STREAM_FRAMES  22050   /* sample frames per buffer (~0.5 s at 44.1 kHz) */

static ALuint music_source      = 0;
static int    music_initialized = 0;

/* Last volume actually pushed to the OpenAL source. Starts at -1 so the first
 * CheckCDVolume() always syncs the source gain to the menu's CDPlayerVolume. */
static int    last_applied_volume = -1;

/* ---- stream state (all NULL/0 when no track is loaded) ---- */
static AVFormatContext    *st_fmt        = NULL;
static AVCodecContext     *st_codec      = NULL;
static AVPacket           *st_pkt        = NULL;
static AVFrame            *st_frame      = NULL;
static int                 st_stream_idx = -1;
static int                 st_channels   = 0;
static int                 st_rate       = 0;
static enum AVSampleFormat st_sample_fmt = AV_SAMPLE_FMT_NONE;
static ALenum              st_al_format  = AL_FORMAT_STEREO16;
static int                 st_loop       = 0;
/* A decoded frame can straddle a chunk boundary, so the leftover is carried
 * across fills rather than dropped: st_have_frame says st_frame still holds
 * undelivered samples, st_frame_pos is how many of them are already consumed. */
static int                 st_have_frame = 0;
static int                 st_frame_pos  = 0;
/* The flush packet is sent exactly once per pass, otherwise the decode loop
 * would spin forever on EAGAIN at end of file. Cleared on rewind. */
static int                 st_sent_flush = 0;
/* 1 from the moment a track is loaded until its queue has fully drained. This,
 * not the OpenAL source state, is what CDDA_IsPlaying() reports — a momentary
 * underrun must not read as "track finished", or psndproj.c would restart the
 * music from the top every time one happened. */
static int                 st_active     = 0;

static ALuint   st_buffers[CDDA_STREAM_BUFFERS];
static int      st_buffers_made = 0;
static int16_t *st_pcm          = NULL;  /* CDDA_STREAM_FRAMES * st_channels */
static int      st_pcm_channels = 0;     /* channel count st_pcm was sized for */

/* Called every frame (psnd.c). The CD-volume menu slider writes straight into
 * CDPlayerVolume, so poll it here and push any change to the source gain. */
void CheckCDVolume()
{
	if (CDPlayerVolume != last_applied_volume) {
		CDDA_ChangeVolume(CDPlayerVolume);
		last_applied_volume = CDPlayerVolume;
	}
}

void CDDA_Start()
{
	if (music_initialized) return;
	alGenSources(1, &music_source);
	alSourcef(music_source, AL_GAIN, 1.0f);
	alSourcef(music_source, AL_PITCH, 1.0f);
	alSource3f(music_source, AL_POSITION, 0.0f, 0.0f, 0.0f);
	alSource3f(music_source, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
	alSourcei(music_source, AL_SOURCE_RELATIVE, AL_TRUE);

	alGenBuffers(CDDA_STREAM_BUFFERS, st_buffers);
	st_buffers_made = 1;

	music_initialized = 1;
}

void CDDA_End()
{
	CDDA_Stop();
	if (st_buffers_made) {
		alDeleteBuffers(CDDA_STREAM_BUFFERS, st_buffers);
		st_buffers_made = 0;
	}
	if (music_source) {
		alDeleteSources(1, &music_source);
		music_source = 0;
	}
	free(st_pcm);
	st_pcm          = NULL;
	st_pcm_channels = 0;
	music_initialized = 0;
}

void CDDA_ChangeVolume(int volume)
{
	CDPlayerVolume = volume;
	if (music_source) {
		float gain = (float)volume / (float)CDDA_VOLUME_MAX;
		alSourcef(music_source, AL_GAIN, gain < 0.0f ? 0.0f : (gain > 1.0f ? 1.0f : gain));
	}
}

int CDDA_CheckNumberOfTracks()
{
	return 15;
}

int CDDA_IsOn()
{
	return music_initialized;
}

int CDDA_IsPlaying()
{
	/* Deliberately the stream flag and not AL_SOURCE_STATE — see st_active. */
	return st_active;
}

/* ------------------------------------------------------------------ */
/* Streaming decoder                                                    */
/* ------------------------------------------------------------------ */

/* Tear the current track down. Safe to call with nothing open. */
static void stream_close(void)
{
	if (st_frame) av_frame_free(&st_frame);
	if (st_pkt)   av_packet_free(&st_pkt);
	if (st_codec) avcodec_free_context(&st_codec);
	if (st_fmt)   avformat_close_input(&st_fmt);

	st_stream_idx = -1;
	st_have_frame = 0;
	st_frame_pos  = 0;
	st_sent_flush = 0;
	st_active     = 0;
}

/* Seek back to the start for a looping track. Returns 0 if the seek failed, in
   which case the caller ends the track rather than spinning on it. */
static int stream_rewind(void)
{
	if (av_seek_frame(st_fmt, st_stream_idx, 0, AVSEEK_FLAG_BACKWARD) < 0)
		return 0;
	avcodec_flush_buffers(st_codec);
	if (st_have_frame) av_frame_unref(st_frame);
	st_have_frame = 0;
	st_frame_pos  = 0;
	st_sent_flush = 0;
	return 1;
}

/* Advance to the next decoded frame, feeding the decoder and handling looping
   and end of file. Returns 1 with st_frame valid, 0 when the track is over. */
static int stream_next_frame(void)
{
	for (;;) {
		int r = avcodec_receive_frame(st_codec, st_frame);
		if (r == 0)
			return 1;

		if (r == AVERROR_EOF) {
			/* Decoder fully drained. Loop by rewinding; otherwise we are done. */
			if (st_loop && stream_rewind())
				continue;
			return 0;
		}
		if (r != AVERROR(EAGAIN))
			return 0;   /* a real decode error */

		/* Decoder wants more input. */
		int got_packet = 0;
		while (av_read_frame(st_fmt, st_pkt) >= 0) {
			if (st_pkt->stream_index == st_stream_idx) {
				avcodec_send_packet(st_codec, st_pkt);
				av_packet_unref(st_pkt);
				got_packet = 1;
				break;
			}
			av_packet_unref(st_pkt);
		}
		if (got_packet)
			continue;

		/* File exhausted. A looping track rewinds straight away rather than
		   draining, so the seam carries no gap. Otherwise flush the decoder
		   once and let the AVERROR_EOF above end the track. */
		if (st_loop && stream_rewind())
			continue;
		if (st_sent_flush)
			return 0;
		avcodec_send_packet(st_codec, NULL);
		st_sent_flush = 1;
	}
}

/* Fill st_pcm with up to CDDA_STREAM_FRAMES sample frames. Returns how many it
   got — fewer than asked at the end of a track, 0 once finished. */
static int stream_decode_chunk(void)
{
	int written = 0;

	while (written < CDDA_STREAM_FRAMES) {
		if (!st_have_frame) {
			if (!stream_next_frame())
				break;
			st_have_frame = 1;
			st_frame_pos  = 0;
		}

		int avail = st_frame->nb_samples - st_frame_pos;
		int n     = CDDA_STREAM_FRAMES - written;
		if (n > avail) n = avail;

		int16_t *dst = st_pcm + (size_t)written * st_channels;

		if (st_sample_fmt == AV_SAMPLE_FMT_FLTP) {
			for (int s = 0; s < n; s++) {
				for (int ch = 0; ch < st_channels; ch++) {
					float f = ((const float *)st_frame->extended_data[ch])[st_frame_pos + s];
					if (f >  1.0f) f =  1.0f;
					if (f < -1.0f) f = -1.0f;
					dst[s * st_channels + ch] = (int16_t)(f * 32767.0f);
				}
			}
		} else if (st_sample_fmt == AV_SAMPLE_FMT_S16P) {
			for (int s = 0; s < n; s++) {
				for (int ch = 0; ch < st_channels; ch++) {
					dst[s * st_channels + ch] =
						((const int16_t *)st_frame->extended_data[ch])[st_frame_pos + s];
				}
			}
		} else if (st_sample_fmt == AV_SAMPLE_FMT_S16) {
			memcpy(dst,
			       (const int16_t *)st_frame->data[0] + (size_t)st_frame_pos * st_channels,
			       (size_t)n * st_channels * sizeof(int16_t));
		} else {
			/* Unsupported sample format: silence beats noise. Vorbis is FLTP, so
			   this is only reachable if someone drops in an exotic codec. */
			memset(dst, 0, (size_t)n * st_channels * sizeof(int16_t));
		}

		written      += n;
		st_frame_pos += n;

		if (st_frame_pos >= st_frame->nb_samples) {
			av_frame_unref(st_frame);
			st_have_frame = 0;
			st_frame_pos  = 0;
		}
	}

	return written;
}

/* Decode one chunk into an OpenAL buffer. Returns 0 when the track is over. */
static int stream_fill_buffer(ALuint buffer)
{
	int frames = stream_decode_chunk();
	if (frames <= 0)
		return 0;

	alBufferData(buffer, st_al_format, st_pcm,
	             (ALsizei)((size_t)frames * st_channels * sizeof(int16_t)), st_rate);
	return 1;
}

/* Open a track ready for streaming. Returns 0 on any failure, having cleaned up. */
static int stream_open(const char *filepath, int loop)
{
	if (avformat_open_input(&st_fmt, filepath, NULL, NULL) < 0)
		return 0;
	if (avformat_find_stream_info(st_fmt, NULL) < 0)
		goto fail;

	st_stream_idx = -1;
	for (unsigned int i = 0; i < st_fmt->nb_streams; i++) {
		if (st_fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			st_stream_idx = (int)i;
			break;
		}
	}
	if (st_stream_idx < 0)
		goto fail;

	AVCodecParameters *par   = st_fmt->streams[st_stream_idx]->codecpar;
	const AVCodec     *codec = avcodec_find_decoder(par->codec_id);
	if (!codec)
		goto fail;

	st_codec = avcodec_alloc_context3(codec);
	if (!st_codec)
		goto fail;
	avcodec_parameters_to_context(st_codec, par);
	if (avcodec_open2(st_codec, codec, NULL) < 0)
		goto fail;

	st_channels   = st_codec->ch_layout.nb_channels;
	st_rate       = st_codec->sample_rate;
	st_sample_fmt = st_codec->sample_fmt;
	if (st_channels <= 0 || st_rate <= 0)
		goto fail;
	st_al_format  = (st_channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;

	st_pkt   = av_packet_alloc();
	st_frame = av_frame_alloc();
	if (!st_pkt || !st_frame)
		goto fail;

	/* One scratch chunk for the whole session, resized only if a track turns up
	   with a different channel count. */
	if (!st_pcm || st_pcm_channels != st_channels) {
		free(st_pcm);
		st_pcm = (int16_t *)malloc((size_t)CDDA_STREAM_FRAMES * st_channels * sizeof(int16_t));
		if (!st_pcm) {
			st_pcm_channels = 0;
			goto fail;
		}
		st_pcm_channels = st_channels;
	}

	st_loop       = loop;
	st_have_frame = 0;
	st_frame_pos  = 0;
	st_sent_flush = 0;
	return 1;

fail:
	stream_close();
	return 0;
}

static void play_internal(int track, int loop)
{
	if (!music_initialized) CDDA_Start();
	if (!music_source) return;

	/* Detach whatever was playing. Setting AL_BUFFER to 0 on a stopped source
	   unqueues everything at once, which is what leaves the pool free to
	   re-queue below. */
	alSourceStop(music_source);
	alSourcei(music_source, AL_BUFFER, 0);
	stream_close();

	char filepath[512];
	snprintf(filepath, sizeof(filepath), "%s/cd_tracks/track%02d.ogg", GetGlobalDir(), track);

	if (!stream_open(filepath, loop))
		return;

	/* Prime the queue; a track shorter than the ring just queues fewer. */
	int queued = 0;
	for (int i = 0; i < CDDA_STREAM_BUFFERS; i++) {
		if (!stream_fill_buffer(st_buffers[i]))
			break;
		alSourceQueueBuffers(music_source, 1, &st_buffers[i]);
		queued++;
	}
	if (queued == 0) {
		stream_close();
		return;
	}

	st_active = 1;
	alSourcePlay(music_source);

	/* Apply the current menu volume to the freshly (re)started source — its gain
	 * would otherwise default to full until the slider is next moved. */
	CDDA_ChangeVolume(CDPlayerVolume);
}

/* Recycle spent buffers. Called every frame from SoundSys_Management(), which
   the frontend, the loading screens and the game loop all drive. */
void CDDA_Management(void)
{
	if (!st_active || !music_source) return;

	ALint processed = 0;
	alGetSourcei(music_source, AL_BUFFERS_PROCESSED, &processed);

	while (processed-- > 0) {
		ALuint buffer = 0;
		alSourceUnqueueBuffers(music_source, 1, &buffer);
		/* At the end of a track the spent buffer is simply not re-queued; the
		   ones still ahead of it play the tail out. */
		if (stream_fill_buffer(buffer))
			alSourceQueueBuffers(music_source, 1, &buffer);
	}

	ALint queued = 0;
	alGetSourcei(music_source, AL_BUFFERS_QUEUED, &queued);
	if (queued == 0) {
		/* Everything queued has played out, so the track is genuinely over.
		   This is what finally clears st_active for CDDA_IsPlaying(). */
		stream_close();
		return;
	}

	/* Underrun: the source drained faster than we refilled — a long blocking
	   load, typically — and stopped itself with audio still queued. Restart it,
	   or the track stays silent for good. */
	ALint state = AL_STOPPED;
	alGetSourcei(music_source, AL_SOURCE_STATE, &state);
	if (state != AL_PLAYING && state != AL_PAUSED)
		alSourcePlay(music_source);
}

void CDDA_Play(int CDDATrack)
{
	play_internal(CDDATrack, 0);
}

void CDDA_PlayLoop(int CDDATrack)
{
	play_internal(CDDATrack, 1);
}

void CDDA_Stop()
{
	if (music_source) {
		alSourceStop(music_source);
		alSourcei(music_source, AL_BUFFER, 0);
	}
	stream_close();
}

void CDDA_SwitchOn()
{
	if (!music_initialized) CDDA_Start();
}

int CDDA_HasMusicFiles()
{
	if (!GetGlobalDir()) return 0;
	char path[512];
	/* Check if at least track01 exists */
	snprintf(path, sizeof(path), "%s/cd_tracks/track01.ogg", GetGlobalDir());
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	fclose(f);
	return 1;
}

#endif
