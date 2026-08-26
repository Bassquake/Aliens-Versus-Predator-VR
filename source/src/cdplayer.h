#ifndef CDPLAYER_H
#define CDPLAYER_H

void CDDA_Start();
void CDDA_Stop();
void CDDA_End();
void CheckCDVolume();
int  CDDA_HasMusicFiles();
/* Per-frame pump for the streamed CD music (see cdplayer.c). Also declared in
   win95/cd_player.h as part of the original API, but psnd.c reaches it through
   this header. */
void CDDA_Management(void);

#endif
