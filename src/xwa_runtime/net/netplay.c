#if !defined(_WIN32) && !defined(_DEFAULT_SOURCE)
#define _DEFAULT_SOURCE /* getaddrinfo, getifaddrs, IFF_* */
#endif
#include "xwa_runtime/net/netplay.h"
#include <SDL3/SDL_stdinc.h>

#include "aeron/log.h"
#include "aeron/sync.h"
#include "aeron/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET NpSocket;
#define NP_INVALID_SOCKET INVALID_SOCKET
#define np_closesocket closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
typedef int NpSocket;
#define NP_INVALID_SOCKET (-1)
#define np_closesocket close
#endif

#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

#define NP_LOG "xwa.netplay"

/* Wire format. All integers little endian. Every datagram starts with
 * u32 magic, u8 protocol, u8 type, u16 zero. */
#define NP_MAGIC 0x504E5758u /* "XWNP" */
#define NP_PROTOCOL 1
#define NP_HEADER 8
#define NP_MTU 1232

enum {
	NP_DG_QUERY = 1, /* u32 nonce */
	NP_DG_INFO = 2, /* u32 nonce, u16 port, u8 players, u8 max, u8 open, u8 0, char game[32], char host[16] */
	NP_DG_HELLO = 3,   /* u32 nonce, char short[16], char long[16] */
	NP_DG_WELCOME = 4, /* u32 nonce, u32 token, i32 you, i32 host, i32 group, u16 epoch, u8 inFlight, u8 n,
						  n * (i32 id, char short[16], char long[16]) */
	NP_DG_REJECT = 5,  /* u32 nonce, u32 reason */
	NP_DG_DATA = 6,    /* u32 token, i32 sender, u32 ts, u32 echoTs, u16 echoDelay, u16 0, u32 ack,
						  u8 ackBits[32] (seq ack+1+i received), u8 count, count * (u32 seq, u16 len, bytes) */
	NP_DG_BYE = 7,     /* u32 token, i32 sender */
	NP_DG_CANCEL = 8,  /* u32 nonce: a client gave up joining */
};

enum {
	NP_REJECT_IN_PROGRESS = 1,
	NP_REJECT_FULL = 2,
	NP_REJECT_VERSION = 3,
};

/* Envelope of a reliable message: u8 kind, u8 route, u16 epoch, i32 from, i32 to, payload. */
#define NP_ENVELOPE 12
#define NP_KIND_FLIGHT_STATE 14 /* host -> clients: u16 epoch, u8 inFlight */

#define NP_SEND_CAP 16384
#define NP_SACK_BITS 256
#define NP_DATA_HEADER (24 + NP_SACK_BITS / 8 + 1)
#define NP_RECV_WINDOW 1024
#define NP_INFLIGHT_LIMIT 256
#define NP_KEEPALIVE_MS 200
#define NP_TIMEOUT_MS 20000
#define NP_HELLO_INTERVAL_MS 300
#define NP_JOIN_TIMEOUT_MS 8000
#define NP_DISCOVERY_INTERVAL_MS 1000
#define NP_DISCOVERY_EXPIRE_MS 4000
#define NP_MAX_GAMES 16
#define NP_ACKED 0xffffffffu

enum {
	NP_MODE_NONE = 0,
	NP_MODE_IDLE = 1, /* socket open for discovery or a pending join */
	NP_MODE_HOST = 2,
	NP_MODE_CLIENT = 3,
};

typedef struct NpSeg {
	uint32_t seq;
	uint32_t lastSendMs; /* 0 = not sent yet, NP_ACKED = acknowledged */
	uint32_t firstSendMs;
	uint16_t len;
	uint16_t sends;
	uint8_t* data;
} NpSeg;

typedef struct NpConn {
	int used;
	int dead;
	int dropPending; /* failed (send queue full); np_tick reports it */
	NetPlayPlayer player;
	struct sockaddr_in addr;
	uint32_t helloNonce;
	uint32_t sendNext;
	NpSeg* segs; /* ring of NP_SEND_CAP, oldest first */
	int segHead;
	int segCount;
	uint32_t recvNext;
	uint8_t* ooo[NP_RECV_WINDOW];
	uint16_t oooLen[NP_RECV_WINDOW];
	uint32_t oooSeq[NP_RECV_WINDOW];
	uint32_t lastRecvMs;
	uint32_t lastSendMs;
	int ackDirty;
	uint32_t peerTs;
	uint32_t peerTsAtMs;
	uint32_t srttMs;
	int rttValid;
} NpConn;

typedef struct NpInbound {
	struct NpInbound* next;
	NetPlayMessage msg;
} NpInbound;

typedef struct NpState {
	AeronMutex* mutex;
	AeronThread* thread;
	volatile int running;
	NpSocket sock;
	uint16_t port;
	int mode;
	int32_t localId;
	int32_t hostId;
	int32_t groupId;
	uint32_t token;
	NetPlayPlayer local;
	char gameName[32];
	int lobbyOpen;
	NpConn conns[NETPLAY_MAX_PLAYERS]; /* host: the clients; client: [0] is the host */
	int32_t nextId;
	NetPlayPlayer roster[NETPLAY_MAX_PLAYERS]; /* client view, the local player included */
	int rosterCount;
	int lostReported;

	int joinState;
	char joinError[96];
	struct sockaddr_in joinAddr;
	uint32_t joinNonce;
	uint32_t joinStartMs;
	uint32_t joinLastHelloMs;

	int curEpoch;
	int inFlight;
	int lastFlownEpoch; /* client: the last flight it took part in */
	int hostEpoch;
	int hostInFlight;

	NpInbound* inHead;
	NpInbound* inTail;
	int inCount;

	int discovering;
	uint16_t discoveryPort;
	uint32_t discoveryLastMs;
	struct sockaddr_in probeAddr;
	int haveProbe;
	NetPlayGameInfo games[NP_MAX_GAMES];
	int gameCount;

	int lossPercent;
	uint32_t rng;
} NpState;

static NpState g_np = { .sock = NP_INVALID_SOCKET };

/* ------------------------------------------------------------------ util */

static uint32_t np_now_ms(void) {
	uint32_t ms = (uint32_t)(Aeron_NowUs() / 1000u);
	return ms ? ms : 1u; /* 0 means "never" in a few places */
}

static uint32_t np_random(void) {
	uint32_t x = g_np.rng ? g_np.rng : (uint32_t)Aeron_NowUs() | 1u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	g_np.rng = x;
	return x;
}

static void np_put16(uint8_t* p, uint16_t v) {
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void np_put32(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static uint16_t np_get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t np_get32(const uint8_t* p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void np_copy_name(char* dst, const char* src) {
	memset(dst, 0, NETPLAY_NAME_LEN);
	if (src) {
		strncpy(dst, src, NETPLAY_NAME_LEN - 1);
	}
}

static int np_header(uint8_t* buf, int type) {
	np_put32(buf, NP_MAGIC);
	buf[4] = NP_PROTOCOL;
	buf[5] = (uint8_t)type;
	np_put16(buf + 6, 0);
	return NP_HEADER;
}

static int np_same_addr(const struct sockaddr_in* a, const struct sockaddr_in* b) {
	return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void np_lock(void) {
	if (!g_np.mutex) {
		g_np.mutex = Aeron_MutexCreate();
	}
	Aeron_MutexLock(g_np.mutex);
}

static void np_unlock(void) { Aeron_MutexUnlock(g_np.mutex); }

static void np_set_error(char* error, int errorSize, const char* text) {
	if (error && errorSize > 0) {
		snprintf(error, (size_t)errorSize, "%s", text);
	}
}

/* --------------------------------------------------------------- sockets */

#ifdef __ANDROID__
/* Android drops broadcast packets unless the app holds a Wi-Fi multicast lock
 * (LAN discovery is broadcast). Taken once, for the rest of the run. */
static void np_android_multicast_lock(void) {
	static int done;
	JNIEnv* env;
	jobject activity;
	jobject context = NULL;
	jobject wifi = NULL;
	jobject lock = NULL;
	jstring name = NULL;

	if (done) {
		return;
	}
	done = 1;
	env = (JNIEnv*)SDL_GetAndroidJNIEnv();
	activity = (jobject)SDL_GetAndroidActivity();
	if (!env || !activity) {
		return;
	}
	{
		jclass cls = (*env)->GetObjectClass(env, activity);
		jmethodID getApp =
			(*env)->GetMethodID(env, cls, "getApplicationContext", "()Landroid/content/Context;");
		jmethodID getService =
			(*env)->GetMethodID(env, cls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
		(*env)->DeleteLocalRef(env, cls);
		if (getApp && getService) {
			context = (*env)->CallObjectMethod(env, activity, getApp);
			if ((*env)->ExceptionCheck(env)) {
				context = NULL;
			}
			name = context ? (*env)->NewStringUTF(env, "wifi") : NULL;
			if (context && name) {
				jclass ccls = (*env)->GetObjectClass(env, context);
				jmethodID svc = (*env)->GetMethodID(env, ccls, "getSystemService",
													"(Ljava/lang/String;)Ljava/lang/Object;");
				(*env)->DeleteLocalRef(env, ccls);
				wifi = svc ? (*env)->CallObjectMethod(env, context, svc, name) : NULL;
			}
		}
	}
	if (!(*env)->ExceptionCheck(env) && wifi) {
		jclass wcls = (*env)->GetObjectClass(env, wifi);
		jmethodID create =
			(*env)->GetMethodID(env, wcls, "createMulticastLock",
								"(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;");
		(*env)->DeleteLocalRef(env, wcls);
		if (create) {
			jstring tag = (*env)->NewStringUTF(env, "openxwa-netplay");
			lock = (*env)->CallObjectMethod(env, wifi, create, tag);
			(*env)->DeleteLocalRef(env, tag);
		}
	}
	if (!(*env)->ExceptionCheck(env) && lock) {
		jclass lcls = (*env)->GetObjectClass(env, lock);
		jmethodID refCounted = (*env)->GetMethodID(env, lcls, "setReferenceCounted", "(Z)V");
		jmethodID acquire = (*env)->GetMethodID(env, lcls, "acquire", "()V");
		(*env)->DeleteLocalRef(env, lcls);
		if (refCounted) {
			(*env)->CallVoidMethod(env, lock, refCounted, JNI_FALSE);
		}
		if (acquire && !(*env)->ExceptionCheck(env)) {
			(*env)->CallVoidMethod(env, lock, acquire);
		}
		if (!(*env)->ExceptionCheck(env)) {
			(*env)->NewGlobalRef(env, lock); /* held for the rest of the run */
			Aeron_LogInfo(NP_LOG, "Wi-Fi multicast lock held (LAN discovery)");
		}
	}
	if ((*env)->ExceptionCheck(env)) {
		(*env)->ExceptionClear(env);
		Aeron_LogWarn(NP_LOG, "no Wi-Fi multicast lock; LAN games may not be found (type the address)");
	}
	if (lock) {
		(*env)->DeleteLocalRef(env, lock);
	}
	if (wifi) {
		(*env)->DeleteLocalRef(env, wifi);
	}
	if (name) {
		(*env)->DeleteLocalRef(env, name);
	}
	if (context) {
		(*env)->DeleteLocalRef(env, context);
	}
	(*env)->DeleteLocalRef(env, activity);
}
#endif

static int np_net_init(void) {
#ifdef _WIN32
	static int started;
	if (!started) {
		WSADATA data;
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
			return 0;
		}
		started = 1;
	}
#endif
	return 1;
}

static NpSocket np_open_socket(uint16_t port) {
	NpSocket s;
	struct sockaddr_in addr;
	int one = 1;
	int buf = 1 << 20;

	if (!np_net_init()) {
		return NP_INVALID_SOCKET;
	}
#ifdef __ANDROID__
	np_android_multicast_lock();
#endif
	s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == NP_INVALID_SOCKET) {
		return NP_INVALID_SOCKET;
	}
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&one, sizeof one);
	setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&buf, sizeof buf);
	setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&buf, sizeof buf);
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (bind(s, (struct sockaddr*)&addr, sizeof addr) != 0) {
		np_closesocket(s);
		return NP_INVALID_SOCKET;
	}
#ifdef _WIN32
	{
		u_long nb = 1;
		DWORD off = 0;
		DWORD bytes = 0;
		ioctlsocket(s, FIONBIO, &nb);
		/* Without this an ICMP "port unreachable" makes recvfrom fail. */
		WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, NULL, 0, &bytes, NULL,
				 NULL);
	}
#else
	fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
	return s;
}

static void np_sendto(const struct sockaddr_in* to, const uint8_t* buf, int len) {
	if (g_np.sock == NP_INVALID_SOCKET) {
		return;
	}
	if (g_np.lossPercent > 0 && (int)(np_random() % 100u) < g_np.lossPercent) {
		return;
	}
	sendto(g_np.sock, (const char*)buf, len, 0, (const struct sockaddr*)to, sizeof *to);
}

static int np_resolve(const char* text, uint16_t defaultPort, struct sockaddr_in* out) {
	char host[128];
	const char* colon;
	uint16_t port = defaultPort;
	struct addrinfo hints;
	struct addrinfo* res = NULL;

	if (!text) {
		return 0;
	}
	while (*text == ' ') {
		++text;
	}
	snprintf(host, sizeof host, "%s", text);
	colon = strrchr(host, ':');
	if (colon) {
		int p = atoi(colon + 1);
		if (p > 0 && p < 65536) {
			port = (uint16_t)p;
		}
		host[colon - host] = '\0';
	}
	{
		size_t n = strlen(host);
		while (n > 0 && host[n - 1] == ' ') {
			host[--n] = '\0';
		}
		if (n == 0) {
			return 0;
		}
	}
	memset(out, 0, sizeof *out);
	out->sin_family = AF_INET;
	out->sin_port = htons(port);
	if (inet_pton(AF_INET, host, &out->sin_addr) == 1) {
		return 1;
	}
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
		return 0;
	}
	out->sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
	freeaddrinfo(res);
	return 1;
}

/* ----------------------------------------------------------- connections */

static NpConn* np_conn_by_id(int32_t id) {
	for (int i = 0; i < NETPLAY_MAX_PLAYERS; ++i) {
		if (g_np.conns[i].used && !g_np.conns[i].dead && g_np.conns[i].player.id == id) {
			return &g_np.conns[i];
		}
	}
	return NULL;
}

static NpConn* np_conn_by_addr(const struct sockaddr_in* addr) {
	for (int i = 0; i < NETPLAY_MAX_PLAYERS; ++i) {
		if (g_np.conns[i].used && !g_np.conns[i].dead && np_same_addr(&g_np.conns[i].addr, addr)) {
			return &g_np.conns[i];
		}
	}
	return NULL;
}

static void np_conn_free(NpConn* c) {
	if (c->segs) {
		for (int i = 0; i < c->segCount; ++i) {
			free(c->segs[(c->segHead + i) % NP_SEND_CAP].data);
		}
		free(c->segs);
	}
	for (int i = 0; i < NP_RECV_WINDOW; ++i) {
		free(c->ooo[i]);
	}
	memset(c, 0, sizeof *c);
}

static NpConn* np_conn_create(const struct sockaddr_in* addr, const NetPlayPlayer* player) {
	for (int i = 0; i < NETPLAY_MAX_PLAYERS; ++i) {
		NpConn* c = &g_np.conns[i];
		if (c->used && c->dead) {
			np_conn_free(c);
		}
		if (!c->used) {
			memset(c, 0, sizeof *c);
			c->segs = (NpSeg*)calloc(NP_SEND_CAP, sizeof(NpSeg));
			if (!c->segs) {
				return NULL;
			}
			c->used = 1;
			c->player = *player;
			c->addr = *addr;
			c->sendNext = 1;
			c->recvNext = 1;
			c->lastRecvMs = np_now_ms();
			return c;
		}
	}
	return NULL;
}

static int np_conn_count(void) {
	int n = 0;
	for (int i = 0; i < NETPLAY_MAX_PLAYERS; ++i) {
		n += g_np.conns[i].used && !g_np.conns[i].dead;
	}
	return n;
}

/* Queues one reliable message (an envelope) on a connection. */
static int np_conn_queue(NpConn* c, const uint8_t* data, int len) {
	NpSeg* seg;

	if (!c || c->dead || c->dropPending || len <= 0 || len > NETPLAY_MAX_MESSAGE + NP_ENVELOPE) {
		return 0;
	}
	if (c->segCount >= NP_SEND_CAP) {
		if (!c->dropPending) {
			Aeron_LogError(NP_LOG, "send queue to player %d is full; dropping the connection",
						   (int)c->player.id);
		}
		c->dropPending = 1;
		return 0;
	}
	seg = &c->segs[(c->segHead + c->segCount) % NP_SEND_CAP];
	memset(seg, 0, sizeof *seg);
	seg->data = (uint8_t*)malloc((size_t)len);
	if (!seg->data) {
		return 0;
	}
	memcpy(seg->data, data, (size_t)len);
	seg->len = (uint16_t)len;
	seg->seq = c->sendNext++;
	++c->segCount;
	return 1;
}

static int np_envelope(uint8_t* out, int kind, int route, int epoch, int32_t from, int32_t to,
					   const void* data, int len) {
	out[0] = (uint8_t)kind;
	out[1] = (uint8_t)route;
	np_put16(out + 2, (uint16_t)epoch);
	np_put32(out + 4, (uint32_t)from);
	np_put32(out + 8, (uint32_t)to);
	if (len > 0) {
		memcpy(out + NP_ENVELOPE, data, (size_t)len);
	}
	return NP_ENVELOPE + len;
}

/* ---------------------------------------------------------------- inbound */

static void np_inbound_push(int kind, int route, int epoch, int32_t from, int32_t to, const void* data,
							int len) {
	NpInbound* node;

	if (len < 0 || len > NETPLAY_MAX_MESSAGE) {
		return;
	}
	if (g_np.inCount >= 8192) {
		/* Mostly flight packets held for a mission this machine is not
		 * flying (yet): the oldest of those make room. */
		NpInbound* prev = NULL;
		NpInbound* it = g_np.inHead;
		while (it && it->msg.kind != NETPLAY_KIND_FLIGHT) {
			prev = it;
			it = it->next;
		}
		if (!it) {
			Aeron_LogWarn(NP_LOG, "inbound queue full; dropping a message");
			return;
		}
		if (prev) {
			prev->next = it->next;
		} else {
			g_np.inHead = it->next;
		}
		if (g_np.inTail == it) {
			g_np.inTail = prev;
		}
		free(it);
		--g_np.inCount;
	}
	node = (NpInbound*)malloc(sizeof *node);
	if (!node) {
		return;
	}
	node->next = NULL;
	node->msg.kind = kind;
	node->msg.route = route;
	node->msg.epoch = epoch;
	node->msg.from = from;
	node->msg.to = to;
	node->msg.len = len;
	if (len > 0) {
		memcpy(node->msg.data, data, (size_t)len);
	}
	if (g_np.inTail) {
		g_np.inTail->next = node;
	} else {
		g_np.inHead = node;
	}
	g_np.inTail = node;
	++g_np.inCount;
}

static void np_inbound_clear(void) {
	NpInbound* node = g_np.inHead;
	while (node) {
		NpInbound* next = node->next;
		free(node);
		node = next;
	}
	g_np.inHead = g_np.inTail = NULL;
	g_np.inCount = 0;
}

/* ----------------------------------------------------------------- roster */

static void np_roster_add(const NetPlayPlayer* p) {
	for (int i = 0; i < g_np.rosterCount; ++i) {
		if (g_np.roster[i].id == p->id) {
			g_np.roster[i] = *p;
			return;
		}
	}
	if (g_np.rosterCount < NETPLAY_MAX_PLAYERS) {
		g_np.roster[g_np.rosterCount++] = *p;
	}
}

static void np_roster_remove(int32_t id) {
	for (int i = 0; i < g_np.rosterCount; ++i) {
		if (g_np.roster[i].id == id) {
			memmove(&g_np.roster[i], &g_np.roster[i + 1],
					(size_t)(g_np.rosterCount - i - 1) * sizeof g_np.roster[0]);
			--g_np.rosterCount;
			return;
		}
	}
}

static void np_player_to_bytes(uint8_t* out, const NetPlayPlayer* p) {
	np_put32(out, (uint32_t)p->id);
	memcpy(out + 4, p->shortName, NETPLAY_NAME_LEN);
	memcpy(out + 4 + NETPLAY_NAME_LEN, p->longName, NETPLAY_NAME_LEN);
	out[4 + NETPLAY_NAME_LEN - 1] = 0;
	out[4 + 2 * NETPLAY_NAME_LEN - 1] = 0;
}

static void np_player_from_bytes(NetPlayPlayer* p, const uint8_t* in) {
	p->id = (int32_t)np_get32(in);
	memcpy(p->shortName, in + 4, NETPLAY_NAME_LEN);
	memcpy(p->longName, in + 4 + NETPLAY_NAME_LEN, NETPLAY_NAME_LEN);
	p->shortName[NETPLAY_NAME_LEN - 1] = 0;
	p->longName[NETPLAY_NAME_LEN - 1] = 0;
}

#define NP_PLAYER_BYTES (4 + 2 * NETPLAY_NAME_LEN)

/* Host: tells every client (except `except`) and the local game about a roster event. */
static void np_host_announce(int kind, const NetPlayPlayer* p, const NpConn* except) {
	uint8_t payload[NP_PLAYER_BYTES];
	uint8_t env[NP_ENVELOPE + NP_PLAYER_BYTES];
	int len;

	np_player_to_bytes(payload, p);
	len = np_envelope(env, kind, NETPLAY_ROUTE_BROADCAST, 0, p->id, 0, payload, NP_PLAYER_BYTES);
	for (int i = 0; i < NETPLAY_MAX_PLAYERS; ++i) {
		NpConn* c = &g_np.conns[i];
		if (c->used && !c->dead && c != except) {
			np_conn_queue(c, env, len);
		}
	}
	np_inbound_push(kind, NETPLAY_ROUTE_BROADCAST, 0, p->id, 0, payload, NP_PLAYER_BYTES);
}

static void np_host_send_flight_state(void) {
	uint8_t payload[4];
	uint8_t env[NP_ENVELOPE + 4];
	int len;

	np_put16(payload, (uint16_t)g_np.curEpoch);
	payload[2] = (uint8_t)g_np.inFlight;
	payload[3] = 0;
	len = np_envelope(env, NP_KIND_FLIGHT_STATE, NETPLAY_ROUTE_BROADCAST, 0, g_np.localId, 0, payload, 4);
	for (int i = 0; i < NETPLAY_MAX_PLAYERS; ++i) {
		if (g_np.conns[i].used && !g_np.conns[i].dead) {
			np_conn_queue(&g_np.conns[i], env, len);
		}
	}
}

static void np_host_drop(NpConn* c, const char* why) {
	NetPlayPlayer p = c->player;

	Aeron_LogInfo(NP_LOG, "player %d (%s) left: %s", (int)p.id, p.shortName, why);
	c->dead = 1;
	np_host_announce(NETPLAY_KIND_LEAVE, &p, c);
}

/* ---------------------------------------------------------------- delivery */

/* One in-order reliable message from connection c. */
static void np_deliver(NpConn* c, const uint8_t* env, int len) {
	int kind;
	int route;
	int epoch;
	int32_t from;
	int32_t to;
	const uint8_t* payload;
	int plen;

	if (len < NP_ENVELOPE || len > NP_ENVELOPE + NETPLAY_MAX_MESSAGE) {
		return;
	}
	kind = env[0];
	route = env[1];
	epoch = np_get16(env + 2);
	from = (int32_t)np_get32(env + 4);
	to = (int32_t)np_get32(env + 8);
	payload = env + NP_ENVELOPE;
	plen = len - NP_ENVELOPE;

	if (g_np.mode == NP_MODE_HOST) {
		from = c->player.id; /* a client speaks only for itself */
		if (kind == NETPLAY_KIND_FRONTEND || kind == NETPLAY_KIND_FLIGHT) {
			uint8_t fwd[NP_ENVELOPE + NETPLAY_MAX_MESSAGE];
			int fwdLen = np_envelope(fwd, kind, route, epoch, from, to, payload, plen);
			if (route == NETPLAY_ROUTE_DIRECT) {
				if (to == g_np.localId) {
					np_inbound_push(kind, route, epoch, from, to, payload, plen);
				} else {
					np_conn_queue(np_conn_by_id(to), fwd, fwdLen);
				}
			} else {
				np_inbound_push(kind, route, epoch, from, to, payload, plen);
				for (int i = 0; i < NETPLAY_MAX_PLAYERS; ++i) {
					NpConn* other = &g_np.conns[i];
					if (other->used && !other->dead && other != c) {
						np_conn_queue(other, fwd, fwdLen);
					}
				}
			}
		} else if (kind == NETPLAY_KIND_RENAME && plen >= NP_PLAYER_BYTES) {
			NetPlayPlayer p;
			np_player_from_bytes(&p, payload);
			p.id = c->player.id;
			c->player = p;
			np_host_announce(NETPLAY_KIND_RENAME, &p, NULL);
		}
		return;
	}

	/* client */
	switch (kind) {
		case NETPLAY_KIND_FRONTEND:
		case NETPLAY_KIND_FLIGHT:
			np_inbound_push(kind, route, epoch, from, to, payload, plen);
			break;
		case NETPLAY_KIND_JOIN:
		case NETPLAY_KIND_RENAME:
			if (plen >= NP_PLAYER_BYTES) {
				NetPlayPlayer p;
				np_player_from_bytes(&p, payload);
				np_roster_add(&p);
				np_inbound_push(kind, route, 0, p.id, 0, &p, (int)sizeof p);
			}
			break;
		case NETPLAY_KIND_LEAVE:
			if (plen >= NP_PLAYER_BYTES) {
				NetPlayPlayer p;
				np_player_from_bytes(&p, payload);
				np_roster_remove(p.id);
				np_inbound_push(kind, route, 0, p.id, 0, &p, (int)sizeof p);
			}
			break;
		case NP_KIND_FLIGHT_STATE:
			if (plen >= 3) {
				g_np.hostEpoch = np_get16(payload);
				g_np.hostInFlight = payload[2] != 0;
			}
			break;
		default:
			break;
	}
}

static void np_receive_segment(NpConn* c, uint32_t seq, const uint8_t* data, int len) {
	int32_t ahead = (int32_t)(seq - c->recvNext);

	c->ackDirty = 1;
	if (ahead < 0) {
		return; /* already have it */
	}
	if (ahead == 0) {
		np_deliver(c, data, len);
		++c->recvNext;
		for (;;) {
			int slot = (int)(c->recvNext % NP_RECV_WINDOW);
			if (!c->ooo[slot] || c->oooSeq[slot] != c->recvNext || c->dead) {
				break;
			}
			np_deliver(c, c->ooo[slot], c->oooLen[slot]);
			free(c->ooo[slot]);
			c->ooo[slot] = NULL;
			++c->recvNext;
		}
		return;
	}
	if (ahead < NP_RECV_WINDOW) {
		int slot = (int)(seq % NP_RECV_WINDOW);
		if (!c->ooo[slot]) {
			c->ooo[slot] = (uint8_t*)malloc((size_t)len);
			if (c->ooo[slot]) {
				memcpy(c->ooo[slot], data, (size_t)len);
				c->oooLen[slot] = (uint16_t)len;
				c->oooSeq[slot] = seq;
			}
		}
	}
}

static void np_process_acks(NpConn* c, uint32_t ack, const uint8_t* bits) {
	uint32_t now = np_now_ms();
	int32_t highestSacked = 0;

	for (int i = 0; i < c->segCount; ++i) {
		NpSeg* seg = &c->segs[(c->segHead + i) % NP_SEND_CAP];
		int32_t d = (int32_t)(seg->seq - ack);
		if (d >= 1 && d <= NP_SACK_BITS && (bits[(d - 1) >> 3] & (1u << ((d - 1) & 7)))) {
			highestSacked = d;
		}
		if (d < 0 || (d >= 1 && d <= NP_SACK_BITS && (bits[(d - 1) >> 3] & (1u << ((d - 1) & 7))))) {
			if (seg->lastSendMs != NP_ACKED) {
				if (seg->sends == 1 && seg->firstSendMs) {
					uint32_t sample = now - seg->firstSendMs;
					if (sample < 10000u) {
						c->srttMs = c->rttValid ? (7u * c->srttMs + sample) / 8u : sample;
						c->rttValid = 1;
					}
				}
				seg->lastSendMs = NP_ACKED;
				free(seg->data);
				seg->data = NULL;
			}
		}
	}
	/* Fast retransmit: a hole well below what the peer already has. */
	if (highestSacked > 0) {
		uint32_t rtt = c->rttValid ? c->srttMs : 100u;
		for (int i = 0; i < c->segCount; ++i) {
			NpSeg* seg = &c->segs[(c->segHead + i) % NP_SEND_CAP];
			int32_t d = (int32_t)(seg->seq - ack);
			if (d > highestSacked - 3) {
				break;
			}
			if (seg->lastSendMs != NP_ACKED && seg->lastSendMs != 0 && now - seg->lastSendMs > rtt + 10u) {
				seg->lastSendMs = 1; /* due now */
			}
		}
	}
	while (c->segCount > 0 && c->segs[c->segHead].lastSendMs == NP_ACKED) {
		c->segHead = (c->segHead + 1) % NP_SEND_CAP;
		--c->segCount;
	}
}

/* ------------------------------------------------------------ datagrams */

static void np_send_welcome(NpConn* c) {
	uint8_t buf[NP_MTU];
	int n = np_header(buf, NP_DG_WELCOME);
	int count = 0;
	uint8_t* countPtr;

	np_put32(buf + n, c->helloNonce);
	np_put32(buf + n + 4, g_np.token);
	np_put32(buf + n + 8, (uint32_t)c->player.id);
	np_put32(buf + n + 12, (uint32_t)g_np.localId);
	np_put32(buf + n + 16, (uint32_t)g_np.groupId);
	np_put16(buf + n + 20, (uint16_t)g_np.curEpoch);
	buf[n + 22] = (uint8_t)g_np.inFlight;
	countPtr = buf + n + 23;
	n += 24;
	np_player_to_bytes(buf + n, &g_np.local);
	n += NP_PLAYER_BYTES;
	++count;
	for (int i = 0; i < NETPLAY_MAX_PLAYERS; ++i) {
		if (g_np.conns[i].used && !g_np.conns[i].dead) {
			np_player_to_bytes(buf + n, &g_np.conns[i].player);
			n += NP_PLAYER_BYTES;
			++count;
		}
	}
	*countPtr = (uint8_t)count;
	np_sendto(&c->addr, buf, n);
}

static void np_send_reject(const struct sockaddr_in* to, uint32_t nonce, uint32_t reason) {
	uint8_t buf[32];
	int n = np_header(buf, NP_DG_REJECT);
	np_put32(buf + n, nonce);
	np_put32(buf + n + 4, reason);
	np_sendto(to, buf, n + 8);
}

static void np_send_bye(const struct sockaddr_in* to) {
	uint8_t buf[32];
	int n = np_header(buf, NP_DG_BYE);
	np_put32(buf + n, g_np.token);
	np_put32(buf + n + 4, (uint32_t)g_np.localId);
	np_sendto(to, buf, n + 8);
}

static void np_handle_hello(const struct sockaddr_in* from, const uint8_t* p, int len) {
	NetPlayPlayer player;
	NpConn* c;
	uint32_t nonce;

	if (len < 4 + 2 * NETPLAY_NAME_LEN) {
		return;
	}
	nonce = np_get32(p);
	c = np_conn_by_addr(from);
	if (c) {
		if (c->helloNonce == nonce) {
			np_send_welcome(c); /* our welcome got lost */
			return;
		}
		np_host_drop(c, "joined again"); /* a new attempt from the same socket */
	}
	if (!g_np.lobbyOpen || g_np.inFlight) {
		np_send_reject(from, nonce, NP_REJECT_IN_PROGRESS);
		return;
	}
	if (np_conn_count() >= NETPLAY_MAX_PLAYERS - 1) {
		np_send_reject(from, nonce, NP_REJECT_FULL);
		return;
	}
	memset(&player, 0, sizeof player);
	player.id = g_np.nextId++;
	memcpy(player.shortName, p + 4, NETPLAY_NAME_LEN);
	memcpy(player.longName, p + 4 + NETPLAY_NAME_LEN, NETPLAY_NAME_LEN);
	player.shortName[NETPLAY_NAME_LEN - 1] = 0;
	player.longName[NETPLAY_NAME_LEN - 1] = 0;
	c = np_conn_create(from, &player);
	if (!c) {
		np_send_reject(from, nonce, NP_REJECT_FULL);
		return;
	}
	c->helloNonce = nonce;
	Aeron_LogInfo(NP_LOG, "player %d (%s) joined from %s:%u", (int)player.id, player.shortName,
				  inet_ntoa(from->sin_addr), (unsigned)ntohs(from->sin_port));
	np_send_welcome(c);
	np_host_announce(NETPLAY_KIND_JOIN, &player, c);
	/* the newcomer learns the flight state from the welcome */
}

static void np_handle_welcome(const struct sockaddr_in* from, const uint8_t* p, int len) {
	int count;
	NpConn* c;
	NetPlayPlayer host;

	if (g_np.joinState != NETPLAY_JOIN_PENDING || len < 24 || np_get32(p) != g_np.joinNonce ||
		!np_same_addr(from, &g_np.joinAddr)) {
		return;
	}
	count = p[23];
	if (len < 24 + count * NP_PLAYER_BYTES || count < 1) {
		return;
	}
	g_np.token = np_get32(p + 4);
	g_np.localId = (int32_t)np_get32(p + 8);
	g_np.hostId = (int32_t)np_get32(p + 12);
	g_np.groupId = (int32_t)np_get32(p + 16);
	g_np.hostEpoch = np_get16(p + 20);
	g_np.hostInFlight = p[22] != 0;
	g_np.curEpoch = g_np.hostEpoch;
	g_np.lastFlownEpoch = g_np.hostEpoch;
	g_np.inFlight = 0;
	g_np.local.id = g_np.localId;
	g_np.rosterCount = 0;
	np_roster_add(&g_np.local);
	memset(&host, 0, sizeof host);
	for (int i = 0; i < count; ++i) {
		NetPlayPlayer pl;
		np_player_from_bytes(&pl, p + 24 + i * NP_PLAYER_BYTES);
		if (pl.id == g_np.hostId) {
			host = pl;
		}
		if (pl.id != g_np.localId) {
			np_roster_add(&pl);
		}
	}
	host.id = g_np.hostId;
	c = np_conn_create(from, &host);
	if (!c) {
		g_np.joinState = NETPLAY_JOIN_FAILED;
		snprintf(g_np.joinError, sizeof g_np.joinError, "Out of memory.");
		return;
	}
	g_np.mode = NP_MODE_CLIENT;
	g_np.lostReported = 0;
	g_np.joinState = NETPLAY_JOIN_DONE;
	g_np.joinNonce = 0;
	Aeron_LogInfo(NP_LOG, "joined %s:%u as player %d (host %d, %d players)", inet_ntoa(from->sin_addr),
				  (unsigned)ntohs(from->sin_port), (int)g_np.localId, (int)g_np.hostId, count + 0);
}

static void np_handle_reject(const struct sockaddr_in* from, const uint8_t* p, int len) {
	uint32_t reason;

	if (g_np.joinState != NETPLAY_JOIN_PENDING || len < 8 || np_get32(p) != g_np.joinNonce ||
		!np_same_addr(from, &g_np.joinAddr)) {
		return;
	}
	reason = np_get32(p + 4);
	g_np.joinState = NETPLAY_JOIN_FAILED;
	snprintf(g_np.joinError, sizeof g_np.joinError, "%s",
			 reason == NP_REJECT_IN_PROGRESS ? "The game has already started."
			 : reason == NP_REJECT_FULL      ? "The game is full."
											 : "The host runs a different version.");
}

static void np_handle_data(const struct sockaddr_in* from, const uint8_t* p, int len) {
	NpConn* c;
	int32_t sender;
	uint32_t ts;
	uint32_t echoTs;
	uint32_t echoDelay;
	uint32_t ack;
	const uint8_t* bits;
	int count;
	int off;
	uint32_t now = np_now_ms();

	if (len < NP_DATA_HEADER) {
		return;
	}
	if (np_get32(p) != g_np.token) {
		return;
	}
	sender = (int32_t)np_get32(p + 4);
	c = np_conn_by_addr(from);
	if (!c || c->player.id != sender) {
		return;
	}
	ts = np_get32(p + 8);
	echoTs = np_get32(p + 12);
	echoDelay = np_get16(p + 16);
	ack = np_get32(p + 20);
	bits = p + 24;
	count = p[24 + NP_SACK_BITS / 8];
	c->lastRecvMs = now;
	c->peerTs = ts;
	c->peerTsAtMs = now;
	if (echoTs) {
		uint32_t sample = now - echoTs - echoDelay;
		if (sample < 10000u) {
			c->srttMs = c->rttValid ? (7u * c->srttMs + sample) / 8u : sample;
			c->rttValid = 1;
		}
	}
	np_process_acks(c, ack, bits);
	off = NP_DATA_HEADER;
	for (int i = 0; i < count; ++i) {
		uint32_t seq;
		int slen;
		if (off + 6 > len) {
			return;
		}
		seq = np_get32(p + off);
		slen = np_get16(p + off + 4);
		off += 6;
		if (off + slen > len || slen > NP_ENVELOPE + NETPLAY_MAX_MESSAGE) {
			return;
		}
		np_receive_segment(c, seq, p + off, slen);
		off += slen;
		if (c->dead) {
			return;
		}
	}
}

static void np_handle_query(const struct sockaddr_in* from, const uint8_t* p, int len) {
	uint8_t buf[128];
	int n;

	if (g_np.mode != NP_MODE_HOST || len < 4) {
		return;
	}
	n = np_header(buf, NP_DG_INFO);
	np_put32(buf + n, np_get32(p));
	np_put16(buf + n + 4, g_np.port);
	buf[n + 6] = (uint8_t)(np_conn_count() + 1);
	buf[n + 7] = NETPLAY_MAX_PLAYERS;
	buf[n + 8] = (uint8_t)(g_np.lobbyOpen && !g_np.inFlight);
	buf[n + 9] = 0;
	memset(buf + n + 10, 0, 32 + NETPLAY_NAME_LEN);
	strncpy((char*)buf + n + 10, g_np.gameName, 31);
	memcpy(buf + n + 42, g_np.local.shortName, NETPLAY_NAME_LEN);
	buf[n + 42 + NETPLAY_NAME_LEN - 1] = 0;
	np_sendto(from, buf, n + 42 + NETPLAY_NAME_LEN);
}

static void np_handle_info(const struct sockaddr_in* from, const uint8_t* p, int len, int protocol) {
	NetPlayGameInfo* g = NULL;
	char addr[48];
	uint16_t port;
	uint32_t now = np_now_ms();

	if (!g_np.discovering || len < 42 + NETPLAY_NAME_LEN) {
		return;
	}
	port = np_get16(p + 4);
	snprintf(addr, sizeof addr, "%s", inet_ntoa(from->sin_addr));
	for (int i = 0; i < g_np.gameCount; ++i) {
		if (strcmp(g_np.games[i].address, addr) == 0 && g_np.games[i].port == port) {
			g = &g_np.games[i];
			break;
		}
	}
	if (!g) {
		if (g_np.gameCount >= NP_MAX_GAMES) {
			return;
		}
		g = &g_np.games[g_np.gameCount++];
		memset(g, 0, sizeof *g);
		snprintf(g->address, sizeof g->address, "%s", addr);
		g->port = port;
	}
	g->players = p[6];
	g->maxPlayers = p[7];
	g->open = p[8] != 0;
	memcpy(g->gameName, p + 10, 31);
	g->gameName[31] = 0;
	memcpy(g->hostName, p + 42, NETPLAY_NAME_LEN);
	g->hostName[NETPLAY_NAME_LEN - 1] = 0;
	g->compatible = protocol == NP_PROTOCOL;
	g->pingMs = now - np_get32(p);
	g->lastSeenMs = now;
}

static void np_handle_bye(const struct sockaddr_in* from, const uint8_t* p, int len) {
	NpConn* c;

	if (len < 8 || np_get32(p) != g_np.token) {
		return;
	}
	c = np_conn_by_addr(from);
	if (!c || c->player.id != (int32_t)np_get32(p + 4)) {
		return;
	}
	if (g_np.mode == NP_MODE_HOST) {
		np_host_drop(c, "left the game");
	} else if (g_np.mode == NP_MODE_CLIENT && !g_np.lostReported) {
		uint8_t closed = 1; /* the host closed the game */
		c->dead = 1;
		g_np.lostReported = 1;
		np_inbound_push(NETPLAY_KIND_LOST, 0, 0, 0, 0, &closed, 1);
	}
}

static void np_handle_datagram(const struct sockaddr_in* from, const uint8_t* buf, int len) {
	int type;
	int protocol;

	if (len < NP_HEADER || np_get32(buf) != NP_MAGIC) {
		return;
	}
	protocol = buf[4];
	type = buf[5];
	if (protocol != NP_PROTOCOL) {
		if (type == NP_DG_HELLO && g_np.mode == NP_MODE_HOST && len >= NP_HEADER + 4) {
			np_send_reject(from, np_get32(buf + NP_HEADER), NP_REJECT_VERSION);
		} else if (type == NP_DG_INFO) {
			np_handle_info(from, buf + NP_HEADER, len - NP_HEADER, protocol);
		} else if (type == NP_DG_REJECT) {
			np_handle_reject(from, buf + NP_HEADER, len - NP_HEADER);
		}
		return;
	}
	buf += NP_HEADER;
	len -= NP_HEADER;
	switch (type) {
		case NP_DG_QUERY:
			np_handle_query(from, buf, len);
			break;
		case NP_DG_INFO:
			np_handle_info(from, buf, len, protocol);
			break;
		case NP_DG_HELLO:
			if (g_np.mode == NP_MODE_HOST) {
				np_handle_hello(from, buf, len);
			}
			break;
		case NP_DG_WELCOME:
			np_handle_welcome(from, buf, len);
			break;
		case NP_DG_REJECT:
			np_handle_reject(from, buf, len);
			break;
		case NP_DG_DATA:
			if (g_np.mode == NP_MODE_HOST || g_np.mode == NP_MODE_CLIENT) {
				np_handle_data(from, buf, len);
			}
			break;
		case NP_DG_BYE:
			np_handle_bye(from, buf, len);
			break;
		case NP_DG_CANCEL:
			if (g_np.mode == NP_MODE_HOST && len >= 4) {
				NpConn* c = np_conn_by_addr(from);
				if (c && c->helloNonce == np_get32(buf)) {
					np_host_drop(c, "gave up joining");
				}
			}
			break;
		default:
			break;
	}
}

/* ------------------------------------------------------------- transmit */

static uint32_t np_rto(const NpConn* c, int sends) {
	uint32_t rto = c->rttValid ? c->srttMs + c->srttMs / 2u + 30u : 200u;
	if (rto < 40u) {
		rto = 40u;
	}
	if (sends > 2) {
		rto <<= (sends > 4 ? 2 : 1);
	}
	return rto > 1000u ? 1000u : rto;
}

static void np_flush_conn(NpConn* c, uint32_t now) {
	uint8_t buf[NP_MTU];
	int n = 0;
	int count = 0;
	uint8_t* countPtr = NULL;
	int sentAny = 0;
	int inFlightLimitSeq;
	uint8_t ackBits[NP_SACK_BITS / 8];
	uint32_t firstSeq;

	memset(ackBits, 0, sizeof ackBits);
	for (int i = 0; i < NP_SACK_BITS; ++i) {
		uint32_t seq = c->recvNext + 1u + (uint32_t)i;
		int slot = (int)(seq % NP_RECV_WINDOW);
		if (c->ooo[slot] && c->oooSeq[slot] == seq) {
			ackBits[i >> 3] |= (uint8_t)(1u << (i & 7));
		}
	}
	firstSeq = c->segCount ? c->segs[c->segHead].seq : c->sendNext;
	inFlightLimitSeq = (int)(firstSeq + NP_INFLIGHT_LIMIT);

#define NP_BEGIN_DG()                                                                                        \
	do {                                                                                                     \
		n = np_header(buf, NP_DG_DATA);                                                                      \
		np_put32(buf + n, g_np.token);                                                                       \
		np_put32(buf + n + 4, (uint32_t)g_np.localId);                                                       \
		np_put32(buf + n + 8, now);                                                                          \
		np_put32(buf + n + 12, c->peerTs);                                                                   \
		np_put16(buf + n + 16, (uint16_t)(c->peerTs ? now - c->peerTsAtMs : 0));                             \
		np_put16(buf + n + 18, 0);                                                                           \
		np_put32(buf + n + 20, c->recvNext);                                                                 \
		memcpy(buf + n + 24, ackBits, sizeof ackBits);                                                       \
		countPtr = buf + n + 24 + (int)sizeof ackBits;                                                       \
		*countPtr = 0;                                                                                       \
		n += NP_DATA_HEADER;                                                                                 \
		count = 0;                                                                                           \
	} while (0)

	NP_BEGIN_DG();
	for (int i = 0; i < c->segCount; ++i) {
		NpSeg* seg = &c->segs[(c->segHead + i) % NP_SEND_CAP];
		if (seg->lastSendMs == NP_ACKED) {
			continue;
		}
		if ((int32_t)(seg->seq - (uint32_t)inFlightLimitSeq) >= 0) {
			break;
		}
		if (seg->lastSendMs != 0 && now - seg->lastSendMs < np_rto(c, seg->sends)) {
			continue;
		}
		if (n + 6 + seg->len > NP_MTU || count == 255) {
			*countPtr = (uint8_t)count;
			np_sendto(&c->addr, buf, n);
			sentAny = 1;
			NP_BEGIN_DG();
		}
		np_put32(buf + n, seg->seq);
		np_put16(buf + n + 4, seg->len);
		memcpy(buf + n + 6, seg->data, seg->len);
		n += 6 + seg->len;
		++count;
		if (!seg->firstSendMs) {
			seg->firstSendMs = now;
		}
		seg->lastSendMs = now;
		++seg->sends;
	}
	if (count > 0 || c->ackDirty || now - c->lastSendMs >= NP_KEEPALIVE_MS || !c->lastSendMs) {
		*countPtr = (uint8_t)count;
		np_sendto(&c->addr, buf, n);
		sentAny = 1;
	}
#undef NP_BEGIN_DG
	if (sentAny) {
		c->lastSendMs = now;
		c->ackDirty = 0;
	}
}

static void np_tick(uint32_t now) {
	if (g_np.mode == NP_MODE_HOST || g_np.mode == NP_MODE_CLIENT) {
		for (int i = 0; i < NETPLAY_MAX_PLAYERS; ++i) {
			NpConn* c = &g_np.conns[i];
			if (!c->used || c->dead) {
				continue;
			}
			if (now - c->lastRecvMs > NP_TIMEOUT_MS || c->dropPending) {
				if (g_np.mode == NP_MODE_HOST) {
					np_host_drop(c, c->dropPending ? "too far behind" : "timed out");
				} else if (!g_np.lostReported) {
					Aeron_LogWarn(NP_LOG, "the host stopped answering");
					c->dead = 1;
					g_np.lostReported = 1;
					np_inbound_push(NETPLAY_KIND_LOST, 0, 0, 0, 0, NULL, 0);
				}
				continue;
			}
			np_flush_conn(c, now);
		}
	}
	if (g_np.joinState == NETPLAY_JOIN_PENDING) {
		if (now - g_np.joinStartMs > NP_JOIN_TIMEOUT_MS) {
			g_np.joinState = NETPLAY_JOIN_FAILED;
			snprintf(g_np.joinError, sizeof g_np.joinError, "No answer from the host.");
		} else if (now - g_np.joinLastHelloMs >= NP_HELLO_INTERVAL_MS) {
			uint8_t buf[64];
			int n = np_header(buf, NP_DG_HELLO);
			np_put32(buf + n, g_np.joinNonce);
			memcpy(buf + n + 4, g_np.local.shortName, NETPLAY_NAME_LEN);
			memcpy(buf + n + 4 + NETPLAY_NAME_LEN, g_np.local.longName, NETPLAY_NAME_LEN);
			np_sendto(&g_np.joinAddr, buf, n + 4 + 2 * NETPLAY_NAME_LEN);
			g_np.joinLastHelloMs = now;
		}
	}
	if (g_np.discovering && now - g_np.discoveryLastMs >= NP_DISCOVERY_INTERVAL_MS) {
		uint8_t buf[16];
		struct sockaddr_in to;
		int n = np_header(buf, NP_DG_QUERY);
		np_put32(buf + n, now);
		n += 4;
		memset(&to, 0, sizeof to);
		to.sin_family = AF_INET;
		to.sin_port = htons(g_np.discoveryPort);
		to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
		np_sendto(&to, buf, n);
#ifndef _WIN32
		{
			struct ifaddrs* ifs = NULL;
			if (getifaddrs(&ifs) == 0) {
				for (struct ifaddrs* it = ifs; it; it = it->ifa_next) {
					if (it->ifa_addr && it->ifa_addr->sa_family == AF_INET &&
						(it->ifa_flags & IFF_BROADCAST) && (it->ifa_flags & IFF_UP) && it->ifa_broadaddr) {
						to.sin_addr = ((struct sockaddr_in*)it->ifa_broadaddr)->sin_addr;
						np_sendto(&to, buf, n);
					}
				}
				freeifaddrs(ifs);
			}
		}
#endif
		/* the local machine too: a host in another window or on loopback */
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		np_sendto(&to, buf, n);
		if (g_np.haveProbe) {
			np_sendto(&g_np.probeAddr, buf, n);
		}
		g_np.discoveryLastMs = now;
		for (int i = 0; i < g_np.gameCount;) {
			if (now - g_np.games[i].lastSeenMs > NP_DISCOVERY_EXPIRE_MS) {
				g_np.games[i] = g_np.games[--g_np.gameCount];
			} else {
				++i;
			}
		}
	}
}

static int np_thread_main(void* user) {
	uint8_t buf[2048];

	(void)user;
	while (Aeron_AtomicLoad(&g_np.running)) {
#ifdef _WIN32
		{
			fd_set set;
			struct timeval tv = { 0, 3000 };
			FD_ZERO(&set);
			FD_SET(g_np.sock, &set);
			select(0, &set, NULL, NULL, &tv);
		}
#else
		{
			struct pollfd pfd;
			pfd.fd = g_np.sock;
			pfd.events = POLLIN;
			pfd.revents = 0;
			poll(&pfd, 1, 3);
		}
#endif
		np_lock();
		for (int i = 0; i < 256; ++i) {
			struct sockaddr_in from;
#ifdef _WIN32
			int fromLen = sizeof from;
#else
			socklen_t fromLen = sizeof from;
#endif
			int len = (int)recvfrom(g_np.sock, (char*)buf, sizeof buf, 0, (struct sockaddr*)&from, &fromLen);
			if (len <= 0) {
				break;
			}
			if (g_np.lossPercent > 0 && (int)(np_random() % 100u) < g_np.lossPercent) {
				continue;
			}
			np_handle_datagram(&from, buf, len);
		}
		np_tick(np_now_ms());
		np_unlock();
	}
	return 0;
}

/* Opens the socket and starts the thread. Called with the lock held. */
static int np_start(uint16_t port) {
	const char* loss = SDL_getenv("XWA_NETPLAY_LOSS");

	g_np.sock = np_open_socket(port);
	if (g_np.sock == NP_INVALID_SOCKET) {
		return 0;
	}
	{
		struct sockaddr_in addr;
#ifdef _WIN32
		int addrLen = sizeof addr;
#else
		socklen_t addrLen = sizeof addr;
#endif
		memset(&addr, 0, sizeof addr);
		getsockname(g_np.sock, (struct sockaddr*)&addr, &addrLen);
		g_np.port = ntohs(addr.sin_port);
	}
	g_np.lossPercent = loss ? atoi(loss) : 0;
	g_np.rng = (uint32_t)Aeron_NowUs() * 2654435761u | 1u;
	Aeron_AtomicStore(&g_np.running, 1);
	g_np.thread = Aeron_ThreadCreate("xwa-netplay", np_thread_main, NULL);
	if (!g_np.thread) {
		Aeron_AtomicStore(&g_np.running, 0);
		np_closesocket(g_np.sock);
		g_np.sock = NP_INVALID_SOCKET;
		return 0;
	}
	if (g_np.mode == NP_MODE_NONE) {
		g_np.mode = NP_MODE_IDLE;
	}
	return 1;
}

/* Stops the thread and frees everything. Called without the lock. */
static void np_stop(void) {
	AeronThread* thread;

	np_lock();
	thread = g_np.thread;
	g_np.thread = NULL;
	Aeron_AtomicStore(&g_np.running, 0);
	np_unlock();
	if (thread) {
		Aeron_ThreadJoin(thread);
	}
	np_lock();
	if (g_np.sock != NP_INVALID_SOCKET) {
		np_closesocket(g_np.sock);
		g_np.sock = NP_INVALID_SOCKET;
	}
	for (int i = 0; i < NETPLAY_MAX_PLAYERS; ++i) {
		if (g_np.conns[i].used) {
			np_conn_free(&g_np.conns[i]);
		}
	}
	np_inbound_clear();
	g_np.mode = NP_MODE_NONE;
	g_np.localId = g_np.hostId = g_np.groupId = 0;
	g_np.rosterCount = 0;
	g_np.joinState = NETPLAY_JOIN_IDLE;
	g_np.discovering = 0;
	g_np.gameCount = 0;
	g_np.inFlight = 0;
	g_np.lobbyOpen = 0;
	g_np.port = 0;
	np_unlock();
}

/* ------------------------------------------------------------------- API */

int NetPlay_Host(const char* gameName, const char* shortName, const char* longName, uint16_t port,
				 char* error, int errorSize) {
	NetPlay_Shutdown();
	np_lock();
	memset(&g_np.local, 0, sizeof g_np.local);
	np_copy_name(g_np.local.shortName, shortName);
	np_copy_name(g_np.local.longName, longName);
	snprintf(g_np.gameName, sizeof g_np.gameName, "%s", gameName ? gameName : "");
	if (!np_start(port ? port : NETPLAY_DEFAULT_PORT)) {
		np_unlock();
		np_set_error(error, errorSize, "Could not open the network port (is another game hosting?).");
		Aeron_LogError(NP_LOG, "could not bind UDP port %u", (unsigned)(port ? port : NETPLAY_DEFAULT_PORT));
		return 0;
	}
	g_np.mode = NP_MODE_HOST;
	g_np.token = np_random() | 1u;
	g_np.localId = 0x10;
	g_np.local.id = g_np.localId;
	g_np.hostId = g_np.localId;
	g_np.groupId = 0x7fff0001;
	g_np.nextId = g_np.localId + 1;
	g_np.curEpoch = 0;
	g_np.inFlight = 0;
	g_np.lobbyOpen = 1;
	g_np.rosterCount = 0;
	Aeron_LogInfo(NP_LOG, "hosting \"%s\" on UDP port %u", g_np.gameName, (unsigned)g_np.port);
	np_unlock();
	return 1;
}

int NetPlay_JoinBegin(const char* address, uint16_t port, const char* shortName, const char* longName) {
	struct sockaddr_in addr;

	if (!np_resolve(address, port ? port : NETPLAY_DEFAULT_PORT, &addr)) {
		np_lock();
		g_np.joinState = NETPLAY_JOIN_FAILED;
		snprintf(g_np.joinError, sizeof g_np.joinError, "Unknown host address.");
		np_unlock();
		return 0;
	}
	np_lock();
	if (g_np.mode == NP_MODE_HOST || g_np.mode == NP_MODE_CLIENT) {
		np_unlock();
		NetPlay_Shutdown();
		np_lock();
	}
	if (g_np.sock == NP_INVALID_SOCKET && !np_start(0)) {
		g_np.joinState = NETPLAY_JOIN_FAILED;
		snprintf(g_np.joinError, sizeof g_np.joinError, "Could not open a network socket.");
		np_unlock();
		return 0;
	}
	memset(&g_np.local, 0, sizeof g_np.local);
	np_copy_name(g_np.local.shortName, shortName);
	np_copy_name(g_np.local.longName, longName);
	g_np.joinAddr = addr;
	g_np.joinNonce = np_random() | 1u;
	g_np.joinStartMs = np_now_ms();
	g_np.joinLastHelloMs = 0;
	g_np.joinError[0] = 0;
	g_np.joinState = NETPLAY_JOIN_PENDING;
	np_unlock();
	return 1;
}

int NetPlay_JoinPoll(char* error, int errorSize) {
	int state;

	np_lock();
	state = g_np.joinState;
	if (state == NETPLAY_JOIN_FAILED) {
		np_set_error(error, errorSize, g_np.joinError);
	}
	np_unlock();
	return state;
}

static int np_pending_data(void) {
	for (int i = 0; i < NETPLAY_MAX_PLAYERS; ++i) {
		if (g_np.conns[i].used && !g_np.conns[i].dead && g_np.conns[i].segCount > 0) {
			return 1;
		}
	}
	return 0;
}

void NetPlay_Shutdown(void) {
	/* Give the last messages (a "host quits" packet, say) a moment to arrive. */
	for (int waited = 0; waited < 500; waited += 10) {
		int pending;
		np_lock();
		pending = (g_np.mode == NP_MODE_HOST || g_np.mode == NP_MODE_CLIENT) && np_pending_data();
		np_unlock();
		if (!pending) {
			break;
		}
		NetPlay_SleepMs(10);
	}
	np_lock();
	if (g_np.joinState == NETPLAY_JOIN_PENDING || (g_np.joinState == NETPLAY_JOIN_FAILED && g_np.joinNonce)) {
		/* A welcome may be on its way; tell the host not to keep the seat. */
		uint8_t buf[16];
		int n = np_header(buf, NP_DG_CANCEL);
		np_put32(buf + n, g_np.joinNonce);
		np_sendto(&g_np.joinAddr, buf, n + 4);
		np_sendto(&g_np.joinAddr, buf, n + 4);
		g_np.joinNonce = 0;
	}
	if (g_np.mode == NP_MODE_HOST) {
		for (int i = 0; i < NETPLAY_MAX_PLAYERS; ++i) {
			if (g_np.conns[i].used && !g_np.conns[i].dead) {
				np_flush_conn(&g_np.conns[i], np_now_ms());
				np_send_bye(&g_np.conns[i].addr);
				np_send_bye(&g_np.conns[i].addr);
				np_send_bye(&g_np.conns[i].addr);
			}
		}
	} else if (g_np.mode == NP_MODE_CLIENT && g_np.conns[0].used && !g_np.conns[0].dead) {
		np_flush_conn(&g_np.conns[0], np_now_ms());
		np_send_bye(&g_np.conns[0].addr);
		np_send_bye(&g_np.conns[0].addr);
		np_send_bye(&g_np.conns[0].addr);
	}
	if (g_np.mode == NP_MODE_NONE) {
		np_unlock();
		return;
	}
	np_unlock();
	np_stop();
}

int NetPlay_IsActive(void) {
	int active;
	np_lock();
	active = g_np.mode == NP_MODE_HOST || g_np.mode == NP_MODE_CLIENT;
	np_unlock();
	return active;
}

int NetPlay_IsHost(void) {
	int host;
	np_lock();
	host = g_np.mode == NP_MODE_HOST;
	np_unlock();
	return host;
}

int32_t NetPlay_LocalId(void) {
	int32_t id;
	np_lock();
	id = g_np.localId;
	np_unlock();
	return id;
}

int32_t NetPlay_HostId(void) {
	int32_t id;
	np_lock();
	id = g_np.hostId;
	np_unlock();
	return id;
}

int32_t NetPlay_GroupId(void) {
	int32_t id;
	np_lock();
	id = g_np.groupId;
	np_unlock();
	return id;
}

uint16_t NetPlay_Port(void) {
	uint16_t port;
	np_lock();
	port = g_np.port;
	np_unlock();
	return port;
}

int NetPlay_GetPlayers(NetPlayPlayer* out, int max) {
	int n = 0;

	np_lock();
	if (g_np.mode == NP_MODE_HOST) {
		if (n < max) {
			out[n++] = g_np.local;
		}
		for (int i = 0; i < NETPLAY_MAX_PLAYERS && n < max; ++i) {
			if (g_np.conns[i].used && !g_np.conns[i].dead) {
				out[n++] = g_np.conns[i].player;
			}
		}
	} else if (g_np.mode == NP_MODE_CLIENT) {
		if (n < max) {
			out[n++] = g_np.local;
		}
		for (int i = 0; i < g_np.rosterCount && n < max; ++i) {
			if (g_np.roster[i].id != g_np.localId) {
				out[n++] = g_np.roster[i];
			}
		}
	}
	np_unlock();
	return n;
}

uint32_t NetPlay_GetRttMs(int32_t id) {
	uint32_t rtt = 0;
	NpConn* c;

	np_lock();
	c = g_np.mode == NP_MODE_CLIENT ? (id == g_np.hostId ? &g_np.conns[0] : NULL) : np_conn_by_id(id);
	if (c && c->used && c->rttValid) {
		rtt = c->srttMs ? c->srttMs : 1u;
	}
	np_unlock();
	return rtt;
}

int NetPlay_Send(int kind, int32_t to, const void* data, int len) {
	uint8_t env[NP_ENVELOPE + NETPLAY_MAX_MESSAGE];
	int route;
	int envLen;
	int ok = 1;

	if (len < 0 || len > NETPLAY_MAX_MESSAGE) {
		Aeron_LogError(NP_LOG, "message of %d bytes is too large", len);
		return 0;
	}
	np_lock();
	if (g_np.mode != NP_MODE_HOST && g_np.mode != NP_MODE_CLIENT) {
		np_unlock();
		return 0;
	}
	if (to == g_np.localId) {
		np_unlock();
		return 1;
	}
	route = to == 0              ? NETPLAY_ROUTE_BROADCAST
			: to == g_np.groupId ? NETPLAY_ROUTE_GROUP
								 : NETPLAY_ROUTE_DIRECT;
	envLen = np_envelope(env, kind, route, kind == NETPLAY_KIND_FLIGHT ? g_np.curEpoch : 0, g_np.localId, to,
						 data, len);
	if (g_np.mode == NP_MODE_CLIENT) {
		ok = np_conn_queue(&g_np.conns[0], env, envLen);
	} else if (route == NETPLAY_ROUTE_DIRECT) {
		NpConn* c = np_conn_by_id(to);
		ok = c ? np_conn_queue(c, env, envLen) : 0;
	} else {
		for (int i = 0; i < NETPLAY_MAX_PLAYERS; ++i) {
			if (g_np.conns[i].used && !g_np.conns[i].dead) {
				np_conn_queue(&g_np.conns[i], env, envLen);
			}
		}
	}
	np_unlock();
	return ok;
}

int NetPlay_Receive(NetPlayMessage* out, int includeFlight) {
	NpInbound* prev = NULL;
	NpInbound* node;

	np_lock();
	node = g_np.inHead;
	while (node) {
		NpInbound* next = node->next;
		int take = 0;
		int drop = 0;

		if (node->msg.kind == NETPLAY_KIND_FLIGHT) {
			int epoch = node->msg.epoch;
			if (epoch < g_np.curEpoch || (epoch == g_np.curEpoch && !g_np.inFlight)) {
				drop = 1;
			} else if (epoch == g_np.curEpoch && includeFlight) {
				take = 1;
			}
		} else {
			take = 1;
		}
		if (take || drop) {
			if (prev) {
				prev->next = next;
			} else {
				g_np.inHead = next;
			}
			if (g_np.inTail == node) {
				g_np.inTail = prev;
			}
			--g_np.inCount;
			if (take) {
				*out = node->msg;
				free(node);
				np_unlock();
				return 1;
			}
			free(node);
		} else {
			prev = node;
		}
		node = next;
	}
	np_unlock();
	return 0;
}

int NetPlay_HasFrontendPacket(uint32_t packetType) {
	int found = 0;

	np_lock();
	for (NpInbound* node = g_np.inHead; node; node = node->next) {
		if (node->msg.kind == NETPLAY_KIND_FRONTEND && node->msg.len >= 4 &&
			np_get32(node->msg.data) == packetType) {
			found = 1;
			break;
		}
	}
	np_unlock();
	return found;
}

void NetPlay_Rename(const char* shortName, const char* longName) {
	NetPlayPlayer p;
	uint8_t payload[NP_PLAYER_BYTES];
	uint8_t env[NP_ENVELOPE + NP_PLAYER_BYTES];

	np_lock();
	np_copy_name(g_np.local.shortName, shortName);
	np_copy_name(g_np.local.longName, longName);
	p = g_np.local;
	if (g_np.mode == NP_MODE_HOST) {
		np_host_announce(NETPLAY_KIND_RENAME, &p, NULL);
	} else if (g_np.mode == NP_MODE_CLIENT) {
		np_player_to_bytes(payload, &p);
		np_conn_queue(&g_np.conns[0], env,
					  np_envelope(env, NETPLAY_KIND_RENAME, NETPLAY_ROUTE_BROADCAST, 0, p.id, 0, payload,
								  NP_PLAYER_BYTES));
	}
	np_unlock();
}

void NetPlay_SetLobbyOpen(int open) {
	np_lock();
	g_np.lobbyOpen = open != 0;
	np_unlock();
}

void NetPlay_SetGameName(const char* gameName) {
	np_lock();
	snprintf(g_np.gameName, sizeof g_np.gameName, "%s", gameName ? gameName : "");
	np_unlock();
}

void NetPlay_BeginFlight(void) {
	np_lock();
	if (g_np.mode == NP_MODE_HOST) {
		++g_np.curEpoch;
		g_np.inFlight = 1;
		np_host_send_flight_state();
	} else if (g_np.mode == NP_MODE_CLIENT) {
		/* The host may already have finished this flight (a team that
		 * launched late): then it is hostEpoch, not the next one. */
		if (g_np.hostInFlight || g_np.hostEpoch > g_np.lastFlownEpoch) {
			g_np.curEpoch = g_np.hostEpoch;
		} else {
			g_np.curEpoch = g_np.hostEpoch + 1;
		}
		g_np.lastFlownEpoch = g_np.curEpoch;
		g_np.inFlight = 1;
	}
	Aeron_LogInfo(NP_LOG, "flight %d begins", g_np.curEpoch);
	np_unlock();
}

void NetPlay_EndFlight(void) {
	np_lock();
	if (g_np.inFlight) {
		g_np.inFlight = 0;
		if (g_np.mode == NP_MODE_HOST) {
			np_host_send_flight_state();
		}
		Aeron_LogInfo(NP_LOG, "flight %d ends", g_np.curEpoch);
	}
	np_unlock();
}

int NetPlay_InFlight(void) {
	int f;
	np_lock();
	f = g_np.inFlight;
	np_unlock();
	return f;
}

int NetPlay_DiscoveryStart(uint16_t port) {
	int ok = 1;

	np_lock();
	if (g_np.sock == NP_INVALID_SOCKET) {
		ok = np_start(0);
	}
	if (ok) {
		g_np.discovering = 1;
		g_np.discoveryPort = port ? port : NETPLAY_DEFAULT_PORT;
		g_np.discoveryLastMs = 0;
	}
	np_unlock();
	return ok;
}

void NetPlay_DiscoveryProbe(const char* address) {
	struct sockaddr_in addr;
	int ok = address && address[0] && np_resolve(address, NETPLAY_DEFAULT_PORT, &addr);

	np_lock();
	g_np.haveProbe = ok;
	if (ok) {
		g_np.probeAddr = addr;
	}
	np_unlock();
}

void NetPlay_DiscoveryStop(void) {
	int idle;

	np_lock();
	g_np.discovering = 0;
	g_np.gameCount = 0;
	g_np.haveProbe = 0;
	idle = g_np.mode == NP_MODE_IDLE && g_np.joinState != NETPLAY_JOIN_PENDING;
	np_unlock();
	if (idle) {
		np_stop();
	}
}

int NetPlay_DiscoveryList(NetPlayGameInfo* out, int max) {
	int n;

	np_lock();
	n = g_np.gameCount < max ? g_np.gameCount : max;
	memcpy(out, g_np.games, (size_t)n * sizeof *out);
	np_unlock();
	return n;
}

int NetPlay_GetLocalAddresses(char* out, int size) {
	int n = 0;

	if (!out || size <= 0) {
		return 0;
	}
	out[0] = 0;
#ifndef _WIN32
	{
		struct ifaddrs* ifs = NULL;
		if (getifaddrs(&ifs) != 0) {
			return 0;
		}
		for (struct ifaddrs* it = ifs; it; it = it->ifa_next) {
			char text[INET_ADDRSTRLEN];
			if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET || (it->ifa_flags & IFF_LOOPBACK) ||
				!(it->ifa_flags & IFF_UP)) {
				continue;
			}
			inet_ntop(AF_INET, &((struct sockaddr_in*)it->ifa_addr)->sin_addr, text, sizeof text);
			n += snprintf(out + strlen(out), (size_t)(size - (int)strlen(out)), "%s%s", n ? ", " : "", text) >
				 0;
			if ((int)strlen(out) >= size - 1) {
				break;
			}
		}
		freeifaddrs(ifs);
	}
#else
	{
		char name[256];
		struct addrinfo hints;
		struct addrinfo* res = NULL;
		if (!np_net_init() || gethostname(name, sizeof name) != 0) {
			return 0;
		}
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_INET;
		if (getaddrinfo(name, NULL, &hints, &res) == 0) {
			for (struct addrinfo* it = res; it; it = it->ai_next) {
				char text[INET_ADDRSTRLEN];
				inet_ntop(AF_INET, &((struct sockaddr_in*)it->ai_addr)->sin_addr, text, sizeof text);
				n += snprintf(out + strlen(out), (size_t)(size - (int)strlen(out)), "%s%s", n ? ", " : "",
							  text) > 0;
			}
			freeaddrinfo(res);
		}
	}
#endif
	return n;
}

void NetPlay_SleepMs(int ms) {
#ifdef _WIN32
	Sleep((DWORD)ms);
#else
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
#endif
}
