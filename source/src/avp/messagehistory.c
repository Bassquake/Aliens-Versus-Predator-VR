#include <string.h>
#include "3dc.h"
#include "module.h"
#include "inline.h"
#include "stratdef.h"
#include "gamedef.h"
#include "language.h"
#include "messagehistory.h"

#define MAX_NO_OF_MESSAGES_IN_HISTORY 64

extern void NewOnScreenMessage(unsigned char *messagePtr);

struct MessageHistory
{
	enum TEXTSTRING_ID StringID;
	int Hours;
	int Minutes;
	int Seconds;
};

static struct MessageHistory MessageHistoryStore[MAX_NO_OF_MESSAGES_IN_HISTORY];

/* Multiplayer messages are built at run time (player names, chat), so there is no
   string id to store. Their text lives in this parallel array, and the entry's
   StringID is MESSAGE_HISTORY_IS_TEXT - an id no real string has. Kept out of
   struct MessageHistory on purpose: the save game writes that struct as a raw
   block, and a network game is never saved, so the text never needs to be. */
#define MESSAGE_HISTORY_TEXT_LENGTH 256
#define MESSAGE_HISTORY_IS_TEXT     MAX_NEW_TEXTSTRINGS
static char MessageHistoryText[MAX_NO_OF_MESSAGES_IN_HISTORY][MESSAGE_HISTORY_TEXT_LENGTH];
static int NumberOfEntriesInMessageHistory;
static int EntryToNextShow;
static int MessageHistoryAccessedTimer;

void MessageHistory_Initialise(void)
{
	NumberOfEntriesInMessageHistory=0;
	EntryToNextShow=0;
	MessageHistoryAccessedTimer=0;
}

void MessageHistory_Add(enum TEXTSTRING_ID stringID)
{
	if (NumberOfEntriesInMessageHistory<MAX_NO_OF_MESSAGES_IN_HISTORY)
	{
		MessageHistoryStore[NumberOfEntriesInMessageHistory].StringID = stringID;
		MessageHistoryStore[NumberOfEntriesInMessageHistory].Hours = AvP.ElapsedHours;
		MessageHistoryStore[NumberOfEntriesInMessageHistory].Minutes = AvP.ElapsedMinutes;
		MessageHistoryStore[NumberOfEntriesInMessageHistory].Seconds = AvP.ElapsedSeconds/65536;
		NumberOfEntriesInMessageHistory++;
	}
}

/* A multiplayer message, as shown on screen. A long game says far more than the
   history holds, so a full history drops its OLDEST entry, where mission messages
   (MessageHistory_Add) stop being recorded instead - a level has a fixed number. */
void MessageHistory_AddText(const char *text)
{
	struct MessageHistory *entry;

	if (!text || !text[0]) return;
	if (NumberOfEntriesInMessageHistory == MAX_NO_OF_MESSAGES_IN_HISTORY)
	{
		memmove(&MessageHistoryStore[0], &MessageHistoryStore[1],
		        (MAX_NO_OF_MESSAGES_IN_HISTORY - 1) * sizeof MessageHistoryStore[0]);
		memmove(MessageHistoryText[0], MessageHistoryText[1],
		        (MAX_NO_OF_MESSAGES_IN_HISTORY - 1) * sizeof MessageHistoryText[0]);
		NumberOfEntriesInMessageHistory--;
		if (EntryToNextShow > 0) EntryToNextShow--;   /* still the same message */
	}
	entry = &MessageHistoryStore[NumberOfEntriesInMessageHistory];
	entry->StringID = MESSAGE_HISTORY_IS_TEXT;
	entry->Hours    = AvP.ElapsedHours;
	entry->Minutes  = AvP.ElapsedMinutes;
	entry->Seconds  = AvP.ElapsedSeconds/65536;
	strncpy(MessageHistoryText[NumberOfEntriesInMessageHistory], text, MESSAGE_HISTORY_TEXT_LENGTH - 1);
	MessageHistoryText[NumberOfEntriesInMessageHistory][MESSAGE_HISTORY_TEXT_LENGTH - 1] = '\0';
	NumberOfEntriesInMessageHistory++;
}

void MessageHistory_DisplayPrevious(void)
{
	if (EntryToNextShow)
	{
		unsigned char buffer[1024];

		EntryToNextShow--;
		sprintf
		(
			buffer,
			"%s %d (%02dh%02dm%02ds) \n \n%s",
			GetTextString(TEXTSTRING_INGAME_MESSAGENUMBER),
			EntryToNextShow+1,
			MessageHistoryStore[EntryToNextShow].Hours,
			MessageHistoryStore[EntryToNextShow].Minutes,
			MessageHistoryStore[EntryToNextShow].Seconds,
			MessageHistoryStore[EntryToNextShow].StringID == MESSAGE_HISTORY_IS_TEXT
				? MessageHistoryText[EntryToNextShow]
				: GetTextString(MessageHistoryStore[EntryToNextShow].StringID)
		);
		NewOnScreenMessage(buffer);
		MessageHistoryAccessedTimer = 65536*4;

		if (!EntryToNextShow && NumberOfEntriesInMessageHistory) EntryToNextShow = NumberOfEntriesInMessageHistory;
	}
}

void MessageHistory_Maintain(void)
{
	if (MessageHistoryAccessedTimer)
	{
		extern int NormalFrameTime;
		MessageHistoryAccessedTimer -= NormalFrameTime;

		if (MessageHistoryAccessedTimer<0)
		{
			MessageHistoryAccessedTimer=0;
		}
	}
	else
	{
		EntryToNextShow = NumberOfEntriesInMessageHistory;
	}
}


/*---------------------------**
** Load/Save message history **
**---------------------------*/
#include "savegame.h"

typedef struct message_history_save_block
{
	SAVE_BLOCK_HEADER header;

	int NumberOfEntriesInMessageHistory;
	int EntryToNextShow;
	int MessageHistoryAccessedTimer;
	//follow by message history array
}MESSAGE_HISTORY_SAVE_BLOCK;


void Load_MessageHistory(SAVE_BLOCK_HEADER* header)
{
	int i;
	MESSAGE_HISTORY_SAVE_BLOCK* block = (MESSAGE_HISTORY_SAVE_BLOCK*) header;
	struct MessageHistory* saved_message = (struct MessageHistory*) (block+1);

	int expected_size;

	//make sure the block is the correct size
	expected_size = sizeof(*block);
	expected_size += sizeof(struct MessageHistory) * block->NumberOfEntriesInMessageHistory;
	if(header->size != expected_size) return;
	
	//load the stuff then
	NumberOfEntriesInMessageHistory = block->NumberOfEntriesInMessageHistory;
	EntryToNextShow = block->EntryToNextShow;
	MessageHistoryAccessedTimer = block->MessageHistoryAccessedTimer;

	//load the message history
	for(i=0;i<block->NumberOfEntriesInMessageHistory;i++)
	{
		MessageHistoryStore[i] = *saved_message++;
	}

	
}

void Save_MessageHistory()
{
	MESSAGE_HISTORY_SAVE_BLOCK* block;
	int i;

	//get memory for header
	GET_SAVE_BLOCK_POINTER(block);

	//fill in header
	block->header.type = SaveBlock_MessageHistory;
	block->header.size = sizeof(*block) + NumberOfEntriesInMessageHistory * sizeof(struct MessageHistory);

	block->NumberOfEntriesInMessageHistory = NumberOfEntriesInMessageHistory;
	block->EntryToNextShow = EntryToNextShow;
	block->MessageHistoryAccessedTimer = MessageHistoryAccessedTimer;

	for(i=0;i<NumberOfEntriesInMessageHistory;i++)
	{
		struct MessageHistory* message = GET_SAVE_BLOCK_POINTER(message);
		*message = MessageHistoryStore[i];
	}

}


