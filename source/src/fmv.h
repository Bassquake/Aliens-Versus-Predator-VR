#ifndef FMV_H
#define FMV_H

extern void PlayFMV(char *filenamePtr);
extern void StartMenuMusic(void);
extern void PlayMenuMusic(void);
extern void EndMenuMusic(void);

typedef struct
{
	IMAGEHEADER *ImagePtr;
	int SoundVolume;
	int IsTriggeredPlotFMV;
	int StaticImageDrawn;

	int MessageNumber;

	// disabled direct3d stuff
	//LPDIRECTDRAWSURFACE SrcSurface;
	//LPDIRECT3DTEXTURE SrcTexture;
	//LPDIRECT3DTEXTURE DestTexture;
	PALETTEENTRY SrcPalette[256];

	// buffer used for opengl texture uploads
	unsigned char* PalettedBuf;
	unsigned char* RGBBuf;

	int RedScale;
	int GreenScale;
	int BlueScale;

	// FFmpeg decoder state — void* to avoid pulling FFmpeg headers here
	void *fmv_file;        // FILE*
	void *fmv_avio_buf;    // uint8_t* AVIO I/O buffer
	void *fmv_avio_ctx;    // AVIOContext*
	void *fmv_fmt_ctx;     // AVFormatContext*
	void *fmv_codec_ctx;   // AVCodecContext*
	void *fmv_sws_ctx;     // SwsContext*
	void *fmv_frame;       // AVFrame*
	void *fmv_packet;      // AVPacket*
	int   fmv_stream_idx;
	int   fmv_active;
	int   fmv_frame_number;
	int   fmv_frame_duration_ms;   // ms per video frame
	unsigned int fmv_next_frame_ms; // SDL_GetTicks() target for next frame
	unsigned int fmv_start_ms;      // SDL_GetTicks() when playback began (PTS anchor)
	int   fmv_audio_stream_idx;    // -1 if no audio stream
	void *fmv_audio_codec_ctx;     // AVCodecContext* for audio
	void *fmv_audio_frame;         // AVFrame* for audio
	void *fmv_sdl_audio;           // SDL_AudioStream*

} FMVTEXTURE;


extern int NextFMVTextureFrame(FMVTEXTURE *ftPtr, void *bufferPtr);
extern void UpdateFMVTexturePalette(FMVTEXTURE *ftPtr);
extern void InitialiseTriggeredFMVs(void);
extern void StartTriggerPlotFMV(int number);

extern void StartFMVAtFrame(int number, int frame);
extern void GetFMVInformation(int *messageNumberPtr, int *frameNumberPtr);

void UpdateAllFMVTextures(void);
void ScanImagesForFMVs(void);
void ReleaseAllFMVTextures(void);

void PlayBinkedFMV(char *filenamePtr);

#endif
