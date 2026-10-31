#ifndef XWA_RUNTIME_NET_NETPLAY_H
#define XWA_RUNTIME_NET_NETPLAY_H

/*
 * NetPlay: the port's replacement for DirectPlay (port addition).
 *
 * One host and up to seven clients over UDP. Clients talk only to the host,
 * which relays broadcasts and player-to-player messages (a star), so only the
 * host needs a reachable port for internet play. Every message is delivered
 * reliably and in order per sender. A background thread owns the socket, so
 * acknowledgements and keepalives continue while the game thread is busy
 * (loading a mission, for example).
 *
 * Message layers:
 *  - FRONTEND: the lobby, briefing and debrief packets (FrontendNet_*).
 *  - FLIGHT:   the in-flight packets (NetSession_*). These are stamped with a
 *              flight number so packets from a finished mission are dropped
 *              and packets from a mission this machine has not started yet
 *              wait for it.
 *  - JOIN / LEAVE / RENAME / LOST: system events, like DirectPlay's.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	NETPLAY_DEFAULT_PORT = 47630,
	NETPLAY_MAX_PLAYERS = 8,
	NETPLAY_MAX_MESSAGE = 1100,
	NETPLAY_NAME_LEN = 16,
};

typedef enum NetPlayKind {
	NETPLAY_KIND_FRONTEND = 1,
	NETPLAY_KIND_FLIGHT = 2,
	NETPLAY_KIND_JOIN = 10,   /* data: NetPlayPlayer */
	NETPLAY_KIND_LEAVE = 11,  /* data: NetPlayPlayer (id set) */
	NETPLAY_KIND_RENAME = 12, /* data: NetPlayPlayer */
	NETPLAY_KIND_LOST = 13,   /* the connection to the host is gone; data[0] = 1 if the host closed it */
} NetPlayKind;

typedef enum NetPlayRoute {
	NETPLAY_ROUTE_BROADCAST = 0,
	NETPLAY_ROUTE_DIRECT = 1,
	NETPLAY_ROUTE_GROUP = 2,
} NetPlayRoute;

typedef struct NetPlayPlayer {
	int32_t id;
	char shortName[NETPLAY_NAME_LEN]; /* the pilot name */
	char longName[NETPLAY_NAME_LEN];  /* the rating byte + 1, as the game sets it */
} NetPlayPlayer;

typedef struct NetPlayMessage {
	int kind;  /* NetPlayKind */
	int route; /* NetPlayRoute */
	int epoch;
	int32_t from;
	int32_t to;
	int len;
	uint8_t data[NETPLAY_MAX_MESSAGE];
} NetPlayMessage;

typedef struct NetPlayGameInfo {
	char address[48];
	uint16_t port;
	char gameName[32];
	char hostName[NETPLAY_NAME_LEN];
	int players;
	int maxPlayers;
	int open; /* accepting players (the host is in the lobby) */
	int compatible;
	char version[16]; /* the host's build version; empty for older builds */
	uint32_t pingMs;
	uint32_t lastSeenMs;
} NetPlayGameInfo;

typedef enum NetPlayJoinState {
	NETPLAY_JOIN_IDLE = 0,
	NETPLAY_JOIN_PENDING = 1,
	NETPLAY_JOIN_DONE = 2,
	NETPLAY_JOIN_FAILED = 3,
} NetPlayJoinState;

/* Starts hosting on `port` (0 = default). Returns 1 on success. */
int NetPlay_Host(const char* gameName, const char* shortName, const char* longName, uint16_t port,
				 char* error, int errorSize);
/* Starts joining the host at `address` ("1.2.3.4", "name" or "1.2.3.4:port"). Poll NetPlay_JoinPoll. */
int NetPlay_JoinBegin(const char* address, uint16_t port, const char* shortName, const char* longName);
/* Returns a NetPlayJoinState; on failure `error` gets the reason. */
int NetPlay_JoinPoll(char* error, int errorSize);
/* Leaves or closes the session (tells the others) and stops the thread. */
void NetPlay_Shutdown(void);

int NetPlay_IsActive(void);
int NetPlay_IsHost(void);
int32_t NetPlay_LocalId(void);
int32_t NetPlay_HostId(void);
int32_t NetPlay_GroupId(void);
/* The current players, the local one first. Returns the count. */
int NetPlay_GetPlayers(NetPlayPlayer* out, int max);
/* Round trip to that player in ms (clients only know the host's). 0 = unknown. */
uint32_t NetPlay_GetRttMs(int32_t id);

/* Sends to a player id, to 0 (everyone else) or to the group id (everyone else). */
int NetPlay_Send(int kind, int32_t to, const void* data, int len);
/* Next received message. Flight messages are held back unless includeFlight
 * and this machine is flying the mission they belong to. Returns 1 if one. */
int NetPlay_Receive(NetPlayMessage* out, int includeFlight);
/* True if a frontend message with this leading dword is waiting. */
int NetPlay_HasFrontendPacket(uint32_t packetType);

/* Changes the local player's names; everyone gets a RENAME. */
void NetPlay_Rename(const char* shortName, const char* longName);
/* Host: whether new players may join (only while in the lobby). */
void NetPlay_SetLobbyOpen(int open);
void NetPlay_SetGameName(const char* gameName);

/* Bracket a mission. */
void NetPlay_BeginFlight(void);
void NetPlay_EndFlight(void);
int NetPlay_InFlight(void);

/* LAN discovery: broadcasts a query every second while started, and probes
 * `probeAddress` too when given (a typed host address). */
int NetPlay_DiscoveryStart(uint16_t port);
void NetPlay_DiscoveryProbe(const char* address);
void NetPlay_DiscoveryStop(void);
int NetPlay_DiscoveryList(NetPlayGameInfo* out, int max);

/* This machine's IPv4 addresses, comma separated (for "tell your friends"). */
int NetPlay_GetLocalAddresses(char* out, int size);
uint16_t NetPlay_Port(void);
/* Waits a few milliseconds (for the rare blocking waits). */
void NetPlay_SleepMs(int ms);

#ifdef __cplusplus
}
#endif

#endif
