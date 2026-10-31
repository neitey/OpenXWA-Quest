/*
 * Port addition: the multiplayer setup and join screens.
 *
 * The original screens chose a DirectPlay service provider (IPX, TCP/IP,
 * modem, serial) and listed DirectPlay sessions. NetPlay has one transport, so
 * these screens only offer hosting, the games found on the local network and
 * a typed host address.
 */
#include "xwa_runtime/net/netplay_screens.h"

#include "xwa_runtime/config/modern_options_menu.h"
#include "xwa_runtime/net/netplay.h"
#include "xwa_runtime/net/netplay_game.h"

#include "xwa/config/game_config.h"
#include "xwa/frontend/combat_sim_menu.h"
#include "xwa/frontend/frontend_color.h"
#include "xwa/frontend/frontend_display.h"
#include "xwa/frontend/frontend_draw.h"
#include "xwa/frontend/frontend_image.h"
#include "xwa/frontend/frontend_input.h"
#include "xwa/frontend/frontend_mission_list.h"
#include "xwa/frontend/frontend_mission_session.h"
#include "xwa/frontend/frontend_net.h"
#include "xwa/frontend/frontend_screen.h"
#include "xwa/frontend/frontend_text.h"
#include "xwa/frontend/mission_setup.h"
#include "xwa/frontend/net_transport.h"

#include <SDL3/SDL_timer.h>

#include <stdio.h>
#include <string.h>

#ifndef OPENXWA_VERSION
#define OPENXWA_VERSION "unknown"
#endif

#define NS_ADDRESS_FIELD_ID 77

static char s_status[128];
static int s_statusIsError;
static char s_addressLine[180];

static void ns_set_status(const char* text, int isError) {
	snprintf(s_status, sizeof s_status, "%s", text ? text : "");
	s_statusIsError = isError;
}

static void ns_draw_background(int frameCounter) {
	if (frameCounter == 0) {
		FrontImage_RegisterResourceDefault("frontres\\config\\configb.bmp", "background");
		FrontImage_DrawSpriteOpaque("background", 0, 0);
		FrontendDisplay_LockOffscreenSurface();
		FrontImage_DrawSpriteOpaque("background", 0, 0);
		FrontendDisplay_UnlockOffscreenSurface(1);
	}
}

static void ns_draw_centered(const char* text, int y, int fontSize, int color) {
	FrontendRect rect;

	FrontendDraw_RectAssign(&rect, 20, y, 619, y + fontSize);
	FrontendText_DrawCentered(fontSize, text, &rect, color);
}

static void ns_draw_status(int y) {
	if (s_status[0]) {
		ns_draw_centered(s_status, y, 12, s_statusIsError ? g_colorRed : g_colorYellow);
	}
}

static void ns_rating_info(char* out) {
	out[0] = (char)(g_pilotData.pilotRating + 1);
	out[1] = '\0';
}

/* Hosting: the same steps as the original "host an internet game" start. */
static int ns_host(void) {
	char info[2];
	int localId;

	ns_rating_info(info);
	memset(g_mpRoster, 0, sizeof(g_mpRoster));
	memset(g_mpRosterReadyFlags, 0, sizeof(g_mpRosterReadyFlags));
	snprintf(g_pilotData.multiplayerGameName, sizeof(g_pilotData.multiplayerGameName), "%.20s's game",
			 g_pilotData.name);
	g_gameConfig.networkType = NET_TRANSPORT_TCPIP;
	if (!Net_StartNetworkSession(*(const XwaGuid*)&g_frontendNetXwaDirectPlayAppGuid, info, g_pilotData.name,
								 1, g_pilotData.multiplayerGameName, NET_TRANSPORT_TCPIP, 0, 0, "", NULL)) {
		ns_set_status("Could not start the game: the network port is busy.", 1);
		return 0;
	}
	isHost = 1;
	g_frontendMissionSessionMode = FRONTEND_MISSION_SESSION_NET_HOST;
	localId = Net_GetLocalPlayerId();
	Net_SetPlayerReady(localId);
	strncpy(g_mpRoster[0].name, g_pilotData.name, sizeof(g_mpRoster[0].name) - 1);
	g_mpRoster[0].playerId = localId;
	g_mpRoster[0].rating = g_pilotData.pilotRating;
	g_skipFrontendEntryMovie = 0;
	ns_set_status("", 0);
	FrontendScreen_SetCallbacks(MissionSetup_Update, MissionSetup_Exit);
	return 1;
}

int XwaNetScreens_MultiplayerSetup(int frameCounter) {
	static int cursorRow;
	XwaModernOptionsMenu menu;
	char addresses[160];
	char line[200];
	int y;

	ns_draw_background(frameCounter);
	if (frameCounter == 0) {
		cursorRow = 0;
		/* Coming back from a game: make sure nothing is left open. */
		if (XwaNet_IsAttached()) {
			XwaNet_DetachSession();
		}
		NetPlay_Shutdown();
	}

	XwaModernOptionsMenu_Begin(&menu, 320, 110, &cursorRow, 3);
	XwaModernOptionsMenu_DrawTitle(&menu, "Multiplayer");
	menu.y += 10;
	if (XwaModernOptionsMenu_DrawAction(&menu, "Host a game", 60, 0)) {
		if (ns_host()) {
			return 0;
		}
	}
	if (XwaModernOptionsMenu_DrawAction(&menu, "Join a game", 61, 0)) {
		ns_set_status("", 0);
		FrontendScreen_SetCallbacks(FrontendNet_JoinGameScreen, FrontendMissionList_FreeScreenResources);
		return 0;
	}
	if (XwaModernOptionsMenu_DrawAction(&menu, "Back", 62, 0) || XwaModernOptionsMenu_TakeEscape(&menu)) {
		ns_set_status("", 0);
		FrontendScreen_SetCallbacks(CombatSimMenu_Update, CombatSimMenu_Exit);
		return 0;
	}

	y = menu.y + 30;
	/* Looked up now and then, not every frame. */
	if (frameCounter % 120 == 0) {
		if (NetPlay_GetLocalAddresses(addresses, (int)sizeof addresses) > 0) {
			snprintf(s_addressLine, sizeof s_addressLine, "This machine: %s", addresses);
		} else {
			snprintf(s_addressLine, sizeof s_addressLine, "This machine has no network address.");
		}
	}
	snprintf(line, sizeof line, "%s", s_addressLine);
	ns_draw_centered(line, y, 12, g_colorLightBlue);
	y += 18;
	snprintf(line, sizeof line, "Games use UDP port %d. To host over the internet,", NETPLAY_DEFAULT_PORT);
	ns_draw_centered(line, y, 12, g_colorPaleBlue);
	y += 16;
	ns_draw_centered("forward that port on your router to this machine.", y, 12, g_colorPaleBlue);
	y += 16;
	ns_draw_centered("Everyone needs the same game files and version.", y, 12, g_colorPaleBlue);
	ns_draw_status(y + 30);
	return 0;
}

static int ns_join_done(void) {
	NetPlay_DiscoveryStop();
	XwaNet_AttachSession();
	isHost = 0;
	memset(g_mpRoster, 0, sizeof(g_mpRoster));
	memset(g_mpRosterReadyFlags, 0, sizeof(g_mpRosterReadyFlags));
	g_frontendMissionSessionMode = FRONTEND_MISSION_SESSION_NET_CLIENT;
	g_gameConfig.networkType = NET_TRANSPORT_TCPIP;
	g_skipFrontendEntryMovie = 0;
	ns_set_status("", 0);
	FrontendScreen_SetCallbacks(MissionSetup_Update, MissionSetup_Exit);
	return 0;
}

int XwaNetScreens_JoinGame(int frameCounter) {
	static int cursorRow;
	static int connecting;
	static char target[96];
	static char lastProbe[64];
	static uint64_t searchStartMs;
	static char ownAddresses[160];
	static uint64_t ownAddressesMs;
	XwaModernOptionsMenu menu;
	NetPlayGameInfo games[6];
	FrontendRect rect;
	char line[160];
	char error[128];
	char info[2];
	const char* joinAddress = NULL;
	uint16_t joinPort = 0;
	int gameCount;
	int fieldEnter;
	int y;

	ns_draw_background(frameCounter);
	if (frameCounter == 0) {
		cursorRow = 0;
		connecting = 0;
		lastProbe[0] = '\0';
		if (XwaNet_IsAttached()) {
			XwaNet_DetachSession();
		}
		NetPlay_DiscoveryStart(0);
		g_activeTextFieldId = NS_ADDRESS_FIELD_ID;
		searchStartMs = SDL_GetTicks();
		ownAddressesMs = 0;
	}

	if (connecting) {
		int state = NetPlay_JoinPoll(error, (int)sizeof error);

		if (state == NETPLAY_JOIN_DONE) {
			return ns_join_done();
		}
		if (state == NETPLAY_JOIN_FAILED) {
			connecting = 0;
			ns_set_status(error[0] ? error : "Could not join the game.", 1);
			NetPlay_Shutdown();
			NetPlay_DiscoveryStart(0);
		} else {
			XwaModernOptionsMenu_Begin(&menu, 320, 150, &cursorRow, 1);
			XwaModernOptionsMenu_DrawTitle(&menu, "Join a game");
			snprintf(line, sizeof line, "Connecting to %s ...", target);
			ns_draw_centered(line, menu.y + 20, 15, g_colorYellow);
			menu.y += 60;
			if (XwaModernOptionsMenu_DrawAction(&menu, "Cancel", 70, 0) ||
				XwaModernOptionsMenu_TakeEscape(&menu)) {
				connecting = 0;
				NetPlay_Shutdown();
				NetPlay_DiscoveryStart(0);
				ns_set_status("", 0);
			}
			return 0;
		}
	}

	/* The typed address comes first: the field takes the typed characters
	 * before the menu reads its keys. */
	FrontendText_Draw(12, "Host address:", 150, 330, g_colorLightBlue);
	FrontendDraw_RectAssign(&rect, 250, 328, 490, 344);
	FrontendDraw_RectOutline(&rect, 0, 0, g_colorPaleBlue);
	g_frontendEditFieldNoKeyboard = 0;
	fieldEnter =
		FrontendText_DrawEditableField(&rect, g_gameConfig.ipAddress, 48, NS_ADDRESS_FIELD_ID, 12, NULL);
	if (strcmp(lastProbe, g_gameConfig.ipAddress) != 0) {
		snprintf(lastProbe, sizeof lastProbe, "%s", g_gameConfig.ipAddress);
		NetPlay_DiscoveryProbe(g_gameConfig.ipAddress);
	}

	gameCount = NetPlay_DiscoveryList(games, 6);
	XwaModernOptionsMenu_Begin(&menu, 320, 90, &cursorRow, gameCount + 2);
	XwaModernOptionsMenu_DrawTitle(&menu, "Join a game");
	menu.y += 6;
	if (gameCount == 0) {
		const uint64_t nowMs = SDL_GetTicks();
		ns_draw_centered("Looking for games on your network...", menu.y + 10, 12, g_colorPaleBlue);
		menu.y += 40;
		if (nowMs - searchStartMs >= 5000) {
			/* Nothing answered: say why that usually is, and what to type. */
			if (!ownAddressesMs || nowMs - ownAddressesMs >= 2000) {
				char addresses[128];
				ownAddressesMs = nowMs;
				if (NetPlay_GetLocalAddresses(addresses, (int)sizeof addresses) > 0) {
					snprintf(ownAddresses, sizeof ownAddresses, "This machine: %s", addresses);
				} else {
					snprintf(ownAddresses, sizeof ownAddresses, "This machine has no network address.");
				}
			}
			ns_draw_centered("No games found. Both devices must be on the same Wi-Fi", menu.y, 12, g_colorLightBlue);
			snprintf(line, sizeof line, "(not a guest network) and the host must allow UDP port %d.",
					 NETPLAY_DEFAULT_PORT);
			ns_draw_centered(line, menu.y + 16, 12, g_colorLightBlue);
			ns_draw_centered("Or type the host's address below.", menu.y + 32, 12, g_colorLightBlue);
			ns_draw_centered(ownAddresses, menu.y + 56, 12, g_colorPaleBlue);
			menu.y += 80;
		}
	}
	for (int i = 0; i < gameCount; ++i) {
		NetPlayGameInfo* g = &games[i];
		int usable = g->open && g->compatible && g->players < g->maxPlayers;

		char version[32] = "";
		if (g->version[0] && strcmp(g->version, OPENXWA_VERSION) != 0) {
			snprintf(version, sizeof version, " v%s", g->version);
		}
		snprintf(line, sizeof line, "%s  (%d/%d%s%s)  %u ms", g->gameName[0] ? g->gameName : g->hostName,
				 g->players, g->maxPlayers,
				 !g->compatible ? ", other version"
				 : !g->open     ? ", in progress"
								: "",
				 version, (unsigned)g->pingMs);
		if (XwaModernOptionsMenu_DrawAction(&menu, line, 71 + i, !usable) && usable && !joinAddress) {
			snprintf(target, sizeof target, "%s:%u", g->address, (unsigned)g->port);
			joinAddress = target;
			joinPort = g->port;
		}
	}
	menu.y = menu.y < 300 ? 300 : menu.y;
	menu.y += 50;
	if ((XwaModernOptionsMenu_DrawAction(&menu, "Connect to the address", 78,
										 g_gameConfig.ipAddress[0] == '\0') ||
		 fieldEnter) &&
		g_gameConfig.ipAddress[0] && !joinAddress) {
		snprintf(target, sizeof target, "%s", g_gameConfig.ipAddress);
		joinAddress = target;
		joinPort = 0;
	}
	if (XwaModernOptionsMenu_DrawAction(&menu, "Back", 79, 0) || XwaModernOptionsMenu_TakeEscape(&menu)) {
		NetPlay_DiscoveryStop();
		NetPlay_Shutdown();
		ns_set_status("", 0);
		FrontendScreen_SetCallbacks(MultiplayerSetup_Update, MultiplayerSetup_Exit);
		return 0;
	}
	y = menu.y + 10;
	ns_draw_status(y);

	if (joinAddress) {
		ns_rating_info(info);
		if (NetPlay_JoinBegin(joinAddress, joinPort, g_pilotData.name, info)) {
			connecting = 1;
			ns_set_status("", 0);
		} else {
			NetPlay_JoinPoll(error, (int)sizeof error);
			ns_set_status(error, 1);
		}
	}
	return 0;
}
