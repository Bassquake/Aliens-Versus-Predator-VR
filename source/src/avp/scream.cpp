#include "unaligned.h"
#include "3dc.h"
#include "ourasert.h"
#include "psndplat.h"
#include "jsndsup.h"
#include "mempool.h"
#include "scream.h"
#include "dxlog.h"
#include "avp_menus.h"

extern "C"
{
extern DISPLAYBLOCK* Player;


struct ScreamSound
{
	struct loaded_sound const * sound_loaded;
	int pitch;
	int volume;	
};

struct ScreamSoundCategory
{
	int num_sounds;
	ScreamSound* sounds;

	SOUNDINDEX last_sound;
};
 
struct ScreamVoiceType
{
	ScreamSoundCategory* category;
};

struct CharacterSoundEffects
{
	int num_voice_types;
	int num_voice_cats;
	ScreamVoiceType* voice_types;
	SOUNDINDEX global_last_sound;

	void PlaySound(int VoiceType,int SoundCategory,int PitchShift,int* ExternalRef,VECTORCH* Location);
	void UnloadSounds();
	void LoadSounds(const char* filename,const char* directory);
};

static CharacterSoundEffects MarineSounds={0,0,0,SID_NOSOUND};
static CharacterSoundEffects AlienSounds={0,0,0,SID_NOSOUND};
static CharacterSoundEffects PredatorSounds={0,0,0,SID_NOSOUND};
static CharacterSoundEffects QueenSounds={0,0,0,SID_NOSOUND};


//static int num_voice_types=0;
//static int num_voice_cats=0;
//static ScreamVoiceType* voice_types=0;
//static SOUNDINDEX global_last_sound;


#if ALIEN_DEMO
#define ScreamFilePath "alienfastfile/"
#elif LOAD_SCREAMS_FROM_FASTFILES
#define ScreamFilePath "fastfile/"
#else
#define ScreamFilePath "sound/"
#endif


void CharacterSoundEffects::LoadSounds(const char* filename,const char* directory)
{
	if(voice_types) return;

	char path[100]=ScreamFilePath;
	strcat(path,filename);

	FILE *file = OpenGameFile(path, FILEMODE_READONLY, FILETYPE_PERM);
	if(file==NULL)
	{
		LOGDXFMT(("Failed to open %s",path));
		return;
	}

	char* buffer;
	int file_size;

	fseek(file, 0, SEEK_END);
	file_size = ftell(file);
	rewind(file);
	
	buffer=new char[file_size+1];
	
	fread(buffer, file_size, 1, file);
	fclose(file);

	if(strncmp("MARSOUND",buffer,8))
	{
		return;
	}

	char* bufpos=buffer+8;

	num_voice_types=*(unaligned_s32*)bufpos;
	bufpos+=4;
	num_voice_cats=*(unaligned_s32*)bufpos;
	bufpos+=4;
	
	voice_types=(ScreamVoiceType*) PoolAllocateMem(num_voice_types * sizeof(ScreamVoiceType));
	
	char wavpath[200];
	strcpy(wavpath,directory);
	char* wavname=&wavpath[strlen(wavpath)];
	
	for(int i=0;i<num_voice_types;i++)	
	{
		voice_types[i].category=(ScreamSoundCategory*) PoolAllocateMem( num_voice_cats * sizeof(ScreamSoundCategory));
		for(int j=0;j<num_voice_cats;j++)
		{
			ScreamSoundCategory* cat=&voice_types[i].category[j];
			cat->last_sound=SID_NOSOUND;
			cat->num_sounds=*(unaligned_s32*)bufpos;
			bufpos+=4;

			if(cat->num_sounds)
			{
				cat->sounds=(ScreamSound*) PoolAllocateMem(cat->num_sounds * sizeof(ScreamSound));
			}
			else
			{
				cat->sounds=0;
			}

			for(int k=0;k<cat->num_sounds;)
			{
				ScreamSound * sound=&cat->sounds[k];

				strcpy(wavname,bufpos);
				bufpos+=strlen(bufpos)+1;

				sound->pitch=*(unaligned_s32*)bufpos;
				bufpos+=4;
				sound->volume=*(unaligned_s32*)bufpos;
				bufpos+=4;

				sound->sound_loaded=GetSound(wavpath);
				if(sound->sound_loaded)
				{
					k++;
				}
				else
				{
					cat->num_sounds--;
				}

			}

		}
	}

	delete [] buffer;
		
}

void CharacterSoundEffects::UnloadSounds()
{
	if(!voice_types) return;
	#if !NEW_DEALLOCATION_ORDER
	for(int i=0;i<num_voice_types;i++)
	{
		for(int j=0;j<num_voice_cats;j++)
		{
			ScreamSoundCategory* cat=&voice_types[i].category[j];
			for(int k=0;k<cat->num_sounds;k++)
			{
				LoseSound(cat->sounds[k].sound_loaded);
			}
		}
	}
	#endif
	voice_types=0;
	num_voice_types=0;
	num_voice_cats=0;
}



void CharacterSoundEffects::PlaySound(int VoiceType,int SoundCategory,int PitchShift,int* ExternalRef,VECTORCH* Location)
{
//	GLOBALASSERT(Location);
	
	if(!voice_types) return;
	//make sure the values are within bounds
	if(VoiceType<0 || VoiceType>=num_voice_types) return;
	if(SoundCategory<0 || SoundCategory>=num_voice_cats) return;
	if(ExternalRef)
	{
		if(*ExternalRef!=SOUND_NOACTIVEINDEX)
		{
			//already playing a sound
			return;
		}
	}

	#if 0
	if(Location)
	{
		//need to make sure this sound is close enough to be heard
		VECTORCH seperation=Player->ObWorld;
		SubVector(Location,&seperation);
		
		//(default sound range is 32 metres)
		if(Approximate3dMagnitude(&seperation)>32000)
		{
			//too far away . don't bother playing a sound.
			return;
		}
	}
	#endif

	ScreamSoundCategory* cat=&voice_types[VoiceType].category[SoundCategory];
	//make sure there are some sound for this category
	if(!cat->num_sounds) return;

	int index=FastRandom()% cat->num_sounds;
	int num_checked=0;

	//pick a sound , trying to avoid the last one picked
	if(cat->num_sounds>2)
	{
		while(num_checked<cat->num_sounds)
		{
			SOUNDINDEX sound_ind=(SOUNDINDEX)cat->sounds[index].sound_loaded->sound_num;
			if(sound_ind!=cat->last_sound && sound_ind!=global_last_sound) break;
			
			index++;
			num_checked++;
			if(index==cat->num_sounds) index=0;	
		}
	}

	ScreamSound* sound=&cat->sounds[index];

	int pitch=sound->pitch+PitchShift;

	if(Location)
		Sound_Play((SOUNDINDEX)sound->sound_loaded->sound_num,"dvpe",Location,sound->volume,pitch,ExternalRef);
	else
		Sound_Play((SOUNDINDEX)sound->sound_loaded->sound_num,"vpe",sound->volume,pitch,ExternalRef);

	//take note of the last sound played
	global_last_sound=cat->last_sound=(SOUNDINDEX)sound->sound_loaded->sound_num;
}



void UnloadScreamSounds()
{
	MarineSounds.UnloadSounds();
	AlienSounds.UnloadSounds();
	PredatorSounds.UnloadSounds();
	QueenSounds.UnloadSounds();
}


void LoadMarineScreamSounds()
{
	MarineSounds.LoadSounds("marsound.dat","npc\\marinevoice\\");
}
void LoadAlienScreamSounds()
{
	AlienSounds.LoadSounds("AlienSound.dat","npc\\alienvoice\\");
}
void LoadPredatorScreamSounds()
{
	PredatorSounds.LoadSounds("PredSound.dat","npc\\predatorvoice\\");
}

void LoadQueenScreamSounds()
{
	QueenSounds.LoadSounds("QueenSound.dat","npc\\queenvoice\\");
}


/* ---- Vocalisations come out of the HEAD, not the origin --------------------
 *
 * Every character sound is played at sbPtr->DynPtr->Position, which is the object's
 * dynamics origin - and that sits ON THE FLOOR (the player's own ObWorld is ~1833 units
 * below their eye for the same reason). A scream therefore came from the emitter's feet.
 *
 * That was inaudible until VR. The flat listener zeroes the vertical component
 * (or[1] = 0.0, yaw-only panning), so elevation was never reproduced and feet-vs-head
 * could not be heard. The VR listener uses the full head orientation deliberately, so
 * looking up and down pans sounds above and below - at which point a scream from floor
 * level became plainly wrong. The error is worst close up, exactly where it is most
 * noticed: an alien 2 m away puts its head ~40 degrees above its feet.
 *
 * Applied HERE because these four wrappers are the single choke point every species'
 * vocalisations pass through. Doing it at the call sites would have meant touching
 * dozens of them across bh_*.c and picking the vocal ones out from weapon fire, tracker
 * clicks and footsteps by hand - and footsteps in particular must STAY at the feet.
 *
 * Heights are head height above the origin, at GAME_UNITS_PER_METRE = 2200. Body sounds
 * keep the origin: a tail swipe or a claw comes from the limb, not the mouth.
 *
 * The ALIEN figure is MEASURED: the markers showed 3300 (~1.5 m) putting the source
 * about 60 cm above its head, so its head sits near 0.9 m. That 60 cm also CONFIRMS THE
 * SCALE - 1300 units for 0.6 m is ~2170 units/metre, which matches
 * GAME_UNITS_PER_METRE = 2200. So the unit maths was right and the body proportions
 * were not: these origins are not at the soles, and reasoning from standing height
 * overshoots every time.
 *
 * The other three were overshooting in the same direction, so they carry the alien's
 * correction factor (2000/3300 = 0.61) rather than fresh guesses from height. They are
 * still ESTIMATES - check each by watching where the marker lands while it vocalises,
 * and convert what you see at ~22 units per centimetre. */
#define VOICE_HEIGHT_MARINE    2050   /* ~0.93 m - estimate, alien factor applied */
#define VOICE_HEIGHT_ALIEN     2000   /* ~0.90 m - MEASURED with the sound markers */
#define VOICE_HEIGHT_PREDATOR  2550   /* ~1.16 m - estimate, alien factor applied */
#define VOICE_HEIGHT_QUEEN     5350   /* ~2.43 m - estimate, alien factor applied */

static int AlienCategoryIsVoice(int c)
{
	/* Tail and swipe are limb sounds. */
	return (c != ASC_TailSound && c != ASC_Swipe);
}
static int PredatorCategoryIsVoice(int c)
{
	/* Swipe is the wristblade; the medicomp is a device at the wrist. */
	return (c != PSC_Swipe && c != PSC_Medicomp_Special);
}
static int QueenCategoryIsVoice(int c)
{
	/* Object_Bounce is not even the queen - see the note on the enum. */
	return (c != QSC_Object_Bounce);
}

void PlayMarineScream(int VoiceType,int SoundCategory,int PitchShift,int* ExternalRef,VECTORCH* Location)
{
	/* Every marine category is a vocalisation. */
	Sound_SetPendingEmitHeight(Location ? VOICE_HEIGHT_MARINE : 0);
	MarineSounds.PlaySound(VoiceType,SoundCategory,PitchShift,ExternalRef,Location);
}
void PlayAlienSound(int VoiceType,int SoundCategory,int PitchShift,int* ExternalRef,VECTORCH* Location)
{
	Sound_SetPendingEmitHeight((Location && AlienCategoryIsVoice(SoundCategory))
	                           ? VOICE_HEIGHT_ALIEN : 0);
	AlienSounds.PlaySound(VoiceType,SoundCategory,PitchShift,ExternalRef,Location);
}
void PlayPredatorSound(int VoiceType,int SoundCategory,int PitchShift,int* ExternalRef,VECTORCH* Location)
{
	Sound_SetPendingEmitHeight((Location && PredatorCategoryIsVoice(SoundCategory))
	                           ? VOICE_HEIGHT_PREDATOR : 0);
	PredatorSounds.PlaySound(VoiceType,SoundCategory,PitchShift,ExternalRef,Location);
}

void PlayQueenSound(int VoiceType,int SoundCategory,int PitchShift,int* ExternalRef,VECTORCH* Location)
{
	Sound_SetPendingEmitHeight((Location && QueenCategoryIsVoice(SoundCategory))
	                           ? VOICE_HEIGHT_QUEEN : 0);
	QueenSounds.PlaySound(VoiceType,SoundCategory,PitchShift,ExternalRef,Location);
}



};
