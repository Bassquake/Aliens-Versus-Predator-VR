#ifndef AVP_NET_H
#define AVP_NET_H

BOOL DpExtInit(DWORD cGrntdBufs, DWORD cBytesPerBuf, BOOL bErrChcks);
HRESULT DpExtRecv(int lpDP2A, void *lpidFrom, void *lpidTo, DWORD dwFlags, void *lplpData, LPDWORD lpdwDataSize);
HRESULT DpExtSend(int lpDP2A, DPID idFrom, DPID idTo, DWORD dwFlags, void *lpData, DWORD dwDataSize);
void DpExtUnInit();
int DirectPlay_Disconnect();
HRESULT IDirectPlayX_GetPlayerName(int glpDP, DPID id, void *data, void *size);
int DirectPlay_ConnectingToSession();
int DirectPlay_ConnectingToLobbiedGame(char* playerName);
void DirectPlay_EnumConnections();

/* internet relay (tools/relay) */
void Net_SetRelayServer(const char *hostport);
extern char NetRelayServer[];        /* the Relay Server setting; see relay_proto.h */
int Net_RelayRoomCode(char out[6]);
const char *Net_RelayError(void);
void Net_RelayProbeStart(const char *hostport, int keepShown);   /* RELAYPROBE_* in relay_proto.h */
int  Net_RelayProbeStatus(int *ms);

#endif
