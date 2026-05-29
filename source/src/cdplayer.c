#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ------------------------------------------------------------------ */
#elif defined(__ANDROID__)
/* ------------------------------------------------------------------ */
/* SDL3 + Android: decode OGG via FFmpeg, play via OpenAL             */
/* ------------------------------------------------------------------ */

#include "al.h"
#include "alc.h"
#include "files.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/samplefmt.h>

static ALuint music_source      = 0;
static ALuint music_buffer      = 0;
static int    music_initialized = 0;

void CheckCDVolume() {}

void CDDA_Start()
{
	if (music_initialized) return;
	alGenSources(1, &music_source);
	alSourcef(music_source, AL_GAIN, 1.0f);
	alSourcef(music_source, AL_PITCH, 1.0f);
	alSource3f(music_source, AL_POSITION, 0.0f, 0.0f, 0.0f);
	alSource3f(music_source, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
	alSourcei(music_source, AL_SOURCE_RELATIVE, AL_TRUE);
	music_initialized = 1;
}

void CDDA_End()
{
	CDDA_Stop();
	if (music_source) {
		alDeleteSources(1, &music_source);
		music_source = 0;
	}
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
	if (!music_source) return 0;
	ALint state = AL_STOPPED;
	alGetSourcei(music_source, AL_SOURCE_STATE, &state);
	return (state == AL_PLAYING);
}

/* Decode an OGG/Vorbis file at filepath to interleaved int16 PCM.
   Returns heap-allocated buffer (caller must free), or NULL on failure. */
static int16_t *decode_ogg(const char *filepath, int *out_samples, int *out_channels, int *out_rate)
{
	AVFormatContext *fmt_ctx = NULL;
	if (avformat_open_input(&fmt_ctx, filepath, NULL, NULL) < 0)
		return NULL;

	if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
		avformat_close_input(&fmt_ctx);
		return NULL;
	}

	int audio_stream = -1;
	for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
		if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			audio_stream = (int)i;
			break;
		}
	}
	if (audio_stream < 0) {
		avformat_close_input(&fmt_ctx);
		return NULL;
	}

	AVCodecParameters *par    = fmt_ctx->streams[audio_stream]->codecpar;
	const AVCodec     *codec  = avcodec_find_decoder(par->codec_id);
	if (!codec) {
		avformat_close_input(&fmt_ctx);
		return NULL;
	}

	AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(codec_ctx, par);
	if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
		avcodec_free_context(&codec_ctx);
		avformat_close_input(&fmt_ctx);
		return NULL;
	}

	int channels    = codec_ctx->ch_layout.nb_channels;
	int sample_rate = codec_ctx->sample_rate;
	enum AVSampleFormat sample_fmt = codec_ctx->sample_fmt;

	int16_t *pcm     = NULL;
	int      pcm_len = 0;  /* samples written (per-channel frames) */
	int      pcm_cap = 0;

	AVPacket *pkt   = av_packet_alloc();
	AVFrame  *frame = av_frame_alloc();

	/* Decode all packets */
	while (av_read_frame(fmt_ctx, pkt) >= 0) {
		if (pkt->stream_index != audio_stream) {
			av_packet_unref(pkt);
			continue;
		}
		avcodec_send_packet(codec_ctx, pkt);
		av_packet_unref(pkt);

		while (avcodec_receive_frame(codec_ctx, frame) == 0) {
			int n = frame->nb_samples;
			if (pcm_len + n > pcm_cap) {
				pcm_cap = (pcm_len + n + 8192) * 2;
				pcm = (int16_t *)realloc(pcm, (size_t)pcm_cap * channels * sizeof(int16_t));
			}
			int16_t *dst = pcm + (size_t)pcm_len * channels;
			if (sample_fmt == AV_SAMPLE_FMT_FLTP) {
				for (int s = 0; s < n; s++) {
					for (int ch = 0; ch < channels; ch++) {
						float f = ((const float *)frame->extended_data[ch])[s];
						if (f >  1.0f) f =  1.0f;
						if (f < -1.0f) f = -1.0f;
						dst[s * channels + ch] = (int16_t)(f * 32767.0f);
					}
				}
			} else if (sample_fmt == AV_SAMPLE_FMT_S16P) {
				for (int s = 0; s < n; s++) {
					for (int ch = 0; ch < channels; ch++) {
						dst[s * channels + ch] = ((const int16_t *)frame->extended_data[ch])[s];
					}
				}
			} else if (sample_fmt == AV_SAMPLE_FMT_S16) {
				memcpy(dst, frame->data[0], (size_t)n * channels * sizeof(int16_t));
			}
			pcm_len += n;
			av_frame_unref(frame);
		}
	}

	/* Flush decoder */
	avcodec_send_packet(codec_ctx, NULL);
	while (avcodec_receive_frame(codec_ctx, frame) == 0) {
		int n = frame->nb_samples;
		if (pcm_len + n > pcm_cap) {
			pcm_cap = (pcm_len + n + 8192) * 2;
			pcm = (int16_t *)realloc(pcm, (size_t)pcm_cap * channels * sizeof(int16_t));
		}
		int16_t *dst = pcm + (size_t)pcm_len * channels;
		if (sample_fmt == AV_SAMPLE_FMT_FLTP) {
			for (int s = 0; s < n; s++) {
				for (int ch = 0; ch < channels; ch++) {
					float f = ((const float *)frame->extended_data[ch])[s];
					if (f >  1.0f) f =  1.0f;
					if (f < -1.0f) f = -1.0f;
					dst[s * channels + ch] = (int16_t)(f * 32767.0f);
				}
			}
		}
		pcm_len += n;
		av_frame_unref(frame);
	}

	av_frame_free(&frame);
	av_packet_free(&pkt);
	avcodec_free_context(&codec_ctx);
	avformat_close_input(&fmt_ctx);

	*out_samples  = pcm_len;
	*out_channels = channels;
	*out_rate     = sample_rate;
	return pcm;
}

static void play_internal(int track, int loop)
{
	if (!music_initialized) CDDA_Start();

	char filepath[512];
	snprintf(filepath, sizeof(filepath), "%s/cd_tracks/track%02d.ogg", GetGlobalDir(), track);

	/* Stop and release any previous buffer */
	if (music_source) {
		alSourceStop(music_source);
		alSourcei(music_source, AL_BUFFER, 0);
	}
	if (music_buffer) {
		alDeleteBuffers(1, &music_buffer);
		music_buffer = 0;
	}

	int      samples, channels, rate;
	int16_t *pcm = decode_ogg(filepath, &samples, &channels, &rate);
	if (!pcm)
		return;

	alGenBuffers(1, &music_buffer);
	ALenum format = (channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
	alBufferData(music_buffer, format, pcm, samples * channels * (int)sizeof(int16_t), rate);
	free(pcm);

	alSourcei(music_source, AL_BUFFER,  (ALint)music_buffer);
	alSourcei(music_source, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
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
	if (music_buffer) {
		alDeleteBuffers(1, &music_buffer);
		music_buffer = 0;
	}
}

void CDDA_SwitchOn()
{
	if (!music_initialized) CDDA_Start();
}

/* ------------------------------------------------------------------ */
#else
/* ------------------------------------------------------------------ */
/* Non-Android stub (Windows etc.)                                     */
/* ------------------------------------------------------------------ */

void CheckCDVolume() {}
void CDDA_Start() {}
void CDDA_End() {}
void CDDA_ChangeVolume(int volume) { (void)volume; }
int  CDDA_CheckNumberOfTracks() { return 0; }
int  CDDA_IsOn() { return 0; }
int  CDDA_IsPlaying() { return 0; }
void CDDA_Play(int CDDATrack) { (void)CDDATrack; }
void CDDA_PlayLoop(int CDDATrack) { (void)CDDATrack; }
void CDDA_Stop() {}
void CDDA_SwitchOn() {}

#endif
