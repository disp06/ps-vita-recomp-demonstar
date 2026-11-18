/* Two-console netplay: delay-based lockstep.
 *
 * Both consoles run the same game from the same start state (same ds.exe,
 * same registry, same date, a virtual clock that advances only at
 * deterministic points).  The only thing exchanged during play is the pad
 * state: the game's input changes once per 16.7 ms of virtual time (a "net
 * frame").  At net frame k each console needs both pads' state for frame k;
 * its own pad for frame k was sampled DELAY frames earlier and sent then, so
 * on a good link nobody waits.  Player 1 is the host's pad, player 2 the
 * joining console's pad (the game's joystick 2 / player 2 path).
 *
 * Transport (host_net_send / host_net_recv) is supplied by the host layer:
 * UDP on Linux and for "same Wi-Fi" on the Vita, PDP for Vita ad-hoc.
 */
#include "win32.h"
#include "games.h"
#include "netplay.h"
#include <sys/stat.h>
#include <time.h>

#define NP_MAGIC   0x504e5344u   /* 'DSNP' */
#define NP_VERSION 3
#define FRAME_US   16667
#define RING       512
#define CHUNK      1024
#define MAXCHUNKS  64

enum { T_HELLO = 1, T_INFO, T_REJECT, T_CHUNK, T_CHUNKACK, T_READY, T_GO, T_INPUT, T_QUIT };

#pragma pack(push, 1)
typedef struct { uint32_t magic; uint8_t type, version; uint16_t pad; } np_hdr;
typedef struct { np_hdr h; uint32_t nonce, crc[NGAMES]; } p_hello;
typedef struct { np_hdr h; uint32_t nonce; int8_t game, difficulty, delay, aspect; int64_t fixed_time; uint32_t reg_len, reg_crc; } p_info;
typedef struct { np_hdr h; uint32_t reason; } p_reject;
typedef struct { np_hdr h; uint32_t offset, len; uint8_t data[CHUNK]; } p_chunk;
typedef struct { np_hdr h; uint64_t have; } p_chunkack;
typedef struct { np_hdr h; uint32_t first; uint8_t count, pad[3]; uint32_t ack, hash_frame, hash; uint32_t in[32]; } p_input;
#pragma pack(pop)

int g_np_role;            /* NP_HOST / NP_CLIENT while a session runs */
int64_t g_fixed_time;     /* the session's date for GetLocalTime */
static int delay = 3;
static uint32_t local_in[RING], remote_in[RING];
static int32_t local_tag[RING], remote_tag[RING];
static int32_t cur = -1;              /* active net frame */
static int32_t remote_contig = -1;    /* all remote frames up to this one received */
static int32_t peer_ack = -1;         /* peer has all our frames up to this one */
static int32_t sent_upto = -1;        /* our newest generated frame */
static host_input_t active;
static uint32_t my_hash[64], my_hash_frame[64];
static uint32_t peer_hash, peer_hash_frame = 0xffffffffu;
static uint32_t last_send_ms;
static int desync_reported, full_hash = -1, desyncs;
static uint32_t frames_waited, total_wait_ms;

static void hdr(np_hdr *h, int type) { h->magic = NP_MAGIC; h->type = type; h->version = NP_VERSION; h->pad = 0; }

uint32_t np_crc32(const uint8_t *p, uint32_t n, uint32_t c) {
    c = ~c;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & -(c & 1)); }
    return ~c;
}
static uint32_t file_crc(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t buf[4096]; size_t n; uint32_t c = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) c = np_crc32(buf, n, c);
    fclose(f);
    return c ? c : 1;
}
static uint32_t game_exe_crc(int g) {
    extern const char *g_game_id;
    const char *save = g_game_id;
    g_game_id = games[g].id;
    char p[600]; uint32_t c = 0;
    if (map_path("ds.exe", p, sizeof p)) c = file_crc(p);
    g_game_id = save;
    return c;
}

/* ------------------------------------------------------------------ input */
static uint32_t pack_input(const host_input_t *in) {
    int ax = in->ax < -128 ? -128 : in->ax > 127 ? 127 : in->ax;
    int ay = in->ay < -128 ? -128 : in->ay > 127 ? 127 : in->ay;
    return (in->buttons & 0xfff) | ((uint32_t)(uint8_t)ax << 12) | ((uint32_t)(uint8_t)ay << 20);
}
static void unpack(uint32_t v, uint32_t *b, int *ax, int *ay) {
    *b = v & 0xfff; *ax = (int8_t)((v >> 12) & 0xff); *ay = (int8_t)((v >> 20) & 0xff);
}

static void send_inputs(void) {
    p_input p;
    hdr(&p.h, T_INPUT);
    int32_t first = peer_ack + 1;
    if (first < sent_upto - 31) first = sent_upto - 31;
    if (first < 0) first = 0;
    int n = sent_upto - first + 1;
    if (n < 0) n = 0;
    p.first = first; p.count = n; p.ack = (uint32_t)remote_contig;
    for (int k = 0; k < n; k++) p.in[k] = local_in[(first + k) % RING];
    /* newest checksum we have */
    p.hash_frame = 0xffffffffu; p.hash = 0;
    for (int k = 0; k < 64; k++)
        if (my_hash_frame[k] != 0xffffffffu && (p.hash_frame == 0xffffffffu || my_hash_frame[k] > p.hash_frame)) {
            p.hash_frame = my_hash_frame[k]; p.hash = my_hash[k];
        }
    host_net_send(&p, (int)(sizeof p - sizeof p.in + 4 * n));
    last_send_ms = host_ticks_ms();
}

static void check_hash(void) {
    if (peer_hash_frame == 0xffffffffu) return;
    for (int k = 0; k < 64; k++) if (my_hash_frame[k] == peer_hash_frame) {
        if (my_hash[k] != peer_hash) {
            desyncs++;
            if (!desync_reported) {
                desync_reported = 1;
                rt_log("NETPLAY DESYNC at frame %u (mine %08x, peer %08x)\n", peer_hash_frame, my_hash[k], peer_hash);
                host_net_status("Out of sync with the other Vita (see log)", 3000);
            }
        }
        peer_hash_frame = 0xffffffffu;
        return;
    }
}

static int quit_received;
static void handle_packet(const uint8_t *buf, int len) {
    const np_hdr *h = (const np_hdr *)buf;
    if (len < (int)sizeof *h || h->magic != NP_MAGIC || h->version != NP_VERSION) return;
    if (h->type == T_QUIT) { quit_received = 1; return; }
    if (h->type == T_READY && g_np_role == NP_HOST) {   /* our GO was lost */
        np_hdr g; hdr(&g, T_GO); host_net_send(&g, sizeof g); return;
    }
    if (h->type != T_INPUT || len < (int)(sizeof(p_input) - 4 * 32)) return;
    const p_input *p = (const p_input *)buf;
    if ((int32_t)p->ack > peer_ack) peer_ack = (int32_t)p->ack;
    for (int k = 0; k < p->count && k < 32; k++) {
        int32_t f = (int32_t)p->first + k;
        if (f <= remote_contig || f > remote_contig + RING / 2) continue;
        remote_in[f % RING] = p->in[k]; remote_tag[f % RING] = f;
    }
    while (remote_tag[(remote_contig + 1) % RING] == remote_contig + 1) remote_contig++;
    if (p->hash_frame != 0xffffffffu) { peer_hash = p->hash; peer_hash_frame = p->hash_frame; check_hash(); }
}

static void poll_net(void) {
    uint8_t buf[2048];
    int n;
    while ((n = host_net_recv(buf, sizeof buf)) > 0) handle_packet(buf, n);
}

static void np_quit(const char *why) {
    rt_log("netplay: %s (frame %d, waited %u frames / %u ms, desyncs %d)\n", why, cur, frames_waited, total_wait_ms, desyncs);
    np_hdr q; hdr(&q, T_QUIT);
    for (int k = 0; k < 5; k++) host_net_send(&q, sizeof q);
    reg_save();
    host_net_end(why);
    exit(0);
}

static uint32_t fast_hash(const uint8_t *p, uint32_t n, uint32_t h) {
    const uint32_t *w = (const uint32_t *)p;
    uint32_t a = h ^ 0x9e3779b9u, b = n;
    for (uint32_t k = 0; k < n / 4; k++) { a = (a ^ w[k]) * 0x01000193u; b += a >> 7; }
    return a ^ (b * 0x85ebca6bu);
}
static uint32_t state_hash(int32_t frame) {
    extern uint32_t image_end(void);
    extern uint32_t heap_peak(void);
    if (full_hash < 0) { const char *e = getenv("DS_NETHASH"); full_hash = e && *e == 'f'; }
    if (full_hash) {
        uint32_t c = fast_hash(MEM + 0x400000, image_end() - 0x400000, 0);
        return fast_hash(MEM + image_end(), heap_peak() - image_end(), c);
    }
    /* a rotating 256 KB slice of the program's own data (cheap enough for the Vita) */
    uint32_t lo = 0x400000, hi = image_end(), span = 256 * 1024;
    uint32_t slices = (hi - lo + span - 1) / span;
    uint32_t s = lo + ((uint32_t)(frame / 30) % slices) * span;
    uint32_t e = s + span > hi ? hi : s + span;
    return fast_hash(MEM + s, e - s, (uint32_t)frame);
}

/* advance to net frame f: sample our pad for f + delay, wait for the peer's pad for f */
static void advance_to(int32_t f) {
    host_input_t in;
    memset(&in, 0, sizeof in);
    host_poll(&in);
    if (in.quit) np_quit("you left the game");
    int32_t gen = f + delay;
    local_in[gen % RING] = pack_input(&in); local_tag[gen % RING] = gen;
    sent_upto = gen;
    send_inputs();
    uint32_t t0 = host_ticks_ms(), shown = 0;
    while (remote_contig < f) {
        poll_net();
        if (quit_received) np_quit("the other player left");
        if (remote_contig >= f) break;
        uint32_t now = host_ticks_ms();
        if (now - last_send_ms >= 20) send_inputs();
        if (!shown && now - t0 > 400) { shown = 1; host_net_status("Waiting for the other Vita...", 0); }
        if (now - t0 > 20000) np_quit("connection lost");
        host_poll(&in);
        if (in.quit) np_quit("you left the game");
        host_sleep_ms(1);
    }
    uint32_t w = host_ticks_ms() - t0;
    if (w > 2) { frames_waited++; total_wait_ms += w; }
    if (shown) host_net_status(NULL, 0);
    poll_net();
    if (quit_received) np_quit("the other player left");
    if (host_ticks_ms() - last_send_ms >= 8) send_inputs();
    /* the active input of frame f */
    uint32_t mine = local_in[f % RING], theirs = remote_in[f % RING];
    uint32_t p1 = g_np_role == NP_HOST ? mine : theirs, p2 = g_np_role == NP_HOST ? theirs : mine;
    memset(&active, 0, sizeof active);
    unpack(p1, &active.buttons, &active.ax, &active.ay);
    unpack(p2, &active.buttons2, &active.ax2, &active.ay2);
    active.p2_present = 1;
    cur = f;
    if (full_hash > 0 || f % 30 == 0) {
        int k = (f / (full_hash > 0 ? 1 : 30)) % 64;
        my_hash[k] = state_hash(f); my_hash_frame[k] = (uint32_t)f;
        check_hash();
    }
    if (f % 600 == 0 && f) rt_log("netplay: frame %d, waits %u (%u ms), desyncs %d\n", f, frames_waited, total_wait_ms, desyncs);
    if (getenv("DS_NETDBG") && f % 60 == 0) rt_log("netdbg f=%d vt=%llu real=%llu\n", f, (unsigned long long)rt_us(), (unsigned long long)host_ticks_us());
}

/* the game's input source: the real pad normally, the lockstep input in netplay */
void rt_poll_input(host_input_t *in) {
    if (!g_netplay) { host_poll(in); return; }
    int32_t f = (int32_t)(rt_us() / FRAME_US);
    while (cur < f) advance_to(cur + 1);
    *in = active;
}

/* ------------------------------------------------------------- handshake */
static int wait_packet(uint8_t *buf, int max, int type, uint32_t timeout_ms, void (*resend)(void)) {
    uint32_t t0 = host_ticks_ms(), last = 0;
    for (;;) {
        int n = host_net_recv(buf, max);
        if (n > 0) {
            np_hdr *h = (np_hdr *)buf;
            if (h->magic == NP_MAGIC && h->version != NP_VERSION && h->type <= T_INFO) return -2;
            if (h->magic == NP_MAGIC && h->version == NP_VERSION && (type == 0 || h->type == type)) return n;
            continue;
        }
        uint32_t now = host_ticks_ms();
        if (resend && now - last >= 100) { last = now; resend(); }
        if (host_net_cancelled()) return -1;
        if (timeout_ms && now - t0 > timeout_ms) return 0;
        host_sleep_ms(2);
    }
}

static p_hello hello;
static void send_hello(void) { host_net_send(&hello, sizeof hello); }

static uint8_t reg_blob[MAXCHUNKS * CHUNK];
static uint32_t reg_len;

static void set_session_dirs(int game) {
    extern const char *g_game_id;
    g_game_id = games[game].id;
    snprintf(g_save_dir_override, sizeof g_save_dir_override, "%s/net", host_save_dir());
    mkdir(g_save_dir_override, 0777);
}

/* host: wait for a player, send the session.  Returns 0 when the game can start. */
int np_host(int game, launch_opts *opt, char *err, int errlen) {
    g_np_role = NP_HOST;
    host_net_status("Waiting for player 2 to join...", 0);
    uint8_t buf[2048];
    uint32_t mycrc = game_exe_crc(game);
    int n;
    for (;;) {
        n = wait_packet(buf, sizeof buf, T_HELLO, 0, NULL);
        if (n == -2) { snprintf(err, errlen, "The other Vita runs a different version of this port."); return -1; }
        if (n <= 0) { snprintf(err, errlen, "Cancelled."); return -1; }
        if (n >= (int)sizeof(p_hello)) break;
    }
    p_hello *hh = (p_hello *)buf;
    uint32_t nonce = hh->nonce;
    if (hh->crc[game] != mycrc) {
        p_reject r; hdr(&r.h, T_REJECT); r.reason = hh->crc[game] ? 2 : 1;
        for (int k = 0; k < 5; k++) host_net_send(&r, sizeof r);
        snprintf(err, errlen, hh->crc[game] ? "Player 2 has a different ds.exe for %s." : "Player 2 has no game data for %s.", games[game].title);
        return -1;
    }
    /* the session's start state: our registry (copied into the session folder) */
    set_session_dirs(game);
    reg_len = 0;
    {
        char p[600];
        snprintf(p, sizeof p, "%s/registry.txt", host_save_dir());
        FILE *f = fopen(p, "rb");
        if (f) { reg_len = fread(reg_blob, 1, sizeof reg_blob, f); fclose(f); }
        snprintf(p, sizeof p, "%s/registry.txt", g_save_dir_override);
        f = fopen(p, "wb");
        if (f) { fwrite(reg_blob, 1, reg_len, f); fclose(f); }
    }
    g_fixed_time = (int64_t)time(NULL);
    p_info info; hdr(&info.h, T_INFO);
    info.nonce = nonce; info.game = game; info.difficulty = opt->ds1_difficulty; info.delay = delay; info.aspect = 0;
    info.fixed_time = g_fixed_time; info.reg_len = reg_len; info.reg_crc = np_crc32(reg_blob, reg_len, 0);
    host_net_status("Player 2 found. Sending game settings...", 0);
    uint32_t nch = (reg_len + CHUNK - 1) / CHUNK;
    uint64_t have = 0, all = nch >= 64 ? ~0ull : ((1ull << nch) - 1);
    uint32_t t0 = host_ticks_ms(), last = 0;
    for (;;) {
        uint32_t now = host_ticks_ms();
        if (now - last >= 60) {
            last = now;
            host_net_send(&info, sizeof info);
            for (uint32_t c = 0; c < nch; c++) if (!(have & (1ull << c))) {
                p_chunk ch; hdr(&ch.h, T_CHUNK);
                ch.offset = c * CHUNK; ch.len = reg_len - ch.offset > CHUNK ? CHUNK : reg_len - ch.offset;
                memcpy(ch.data, reg_blob + ch.offset, ch.len);
                host_net_send(&ch, (int)(sizeof ch - CHUNK + ch.len));
            }
        }
        n = host_net_recv(buf, sizeof buf);
        if (n > 0) {
            np_hdr *h = (np_hdr *)buf;
            if (h->magic != NP_MAGIC || h->version != NP_VERSION) continue;
            if (h->type == T_CHUNKACK) have |= ((p_chunkack *)buf)->have;
            if (h->type == T_READY && (have & all) == all) break;
            if (h->type == T_QUIT) { snprintf(err, errlen, "Player 2 left."); return -1; }
            continue;
        }
        if (host_net_cancelled()) { snprintf(err, errlen, "Cancelled."); return -1; }
        if (now - t0 > 30000) { snprintf(err, errlen, "Player 2 stopped answering."); return -1; }
        host_sleep_ms(2);
    }
    np_hdr go; hdr(&go, T_GO);
    for (int k = 0; k < 3; k++) host_net_send(&go, sizeof go);
    opt->netplay = 1;
    g_netplay = 1;
    np_init_state();
    host_net_status(NULL, 0);
    rt_log("netplay: hosting %s, registry %u bytes\n", games[game].id, reg_len);
    return 0;
}

/* client: find the host, take its session.  Returns the game index or -1. */
int np_join(launch_opts *opt, char *err, int errlen) {
    g_np_role = NP_CLIENT;
    host_net_status("Looking for player 1's Vita...", 0);
    hdr(&hello.h, T_HELLO);
    hello.nonce = host_ticks_us() ^ 0x5a5a1234u;
    for (int g = 0; g < NGAMES; g++) hello.crc[g] = game_exe_crc(g);
    uint8_t buf[2048];
    p_info info;
    for (;;) {
        int n = wait_packet(buf, sizeof buf, 0, 0, send_hello);
        if (n == -2) { snprintf(err, errlen, "Player 1's Vita runs a different version of this port."); return -1; }
        if (n <= 0) { snprintf(err, errlen, "Cancelled."); return -1; }
        np_hdr *h = (np_hdr *)buf;
        if (h->type == T_REJECT) {
            snprintf(err, errlen, ((p_reject *)buf)->reason == 1 ? "You don't have the game player 1 picked (check ux0:data/DemonStar)." :
                                                                 "Your ds.exe differs from player 1's (both need the same game files).");
            return -1;
        }
        if (h->type == T_INFO && n >= (int)sizeof info && ((p_info *)buf)->nonce == hello.nonce) { memcpy(&info, buf, sizeof info); break; }
    }
    int game = info.game;
    if (game < 0 || game >= NGAMES) { snprintf(err, errlen, "Bad session data."); return -1; }
    host_net_status("Connected. Receiving game settings...", 0);
    reg_len = info.reg_len > sizeof reg_blob ? sizeof reg_blob : info.reg_len;
    uint32_t nch = (reg_len + CHUNK - 1) / CHUNK;
    uint64_t have = 0, all = nch >= 64 ? ~0ull : ((1ull << nch) - 1);
    uint32_t t0 = host_ticks_ms(), last = 0;
    int ready = 0;
    for (;;) {
        int n = host_net_recv(buf, sizeof buf);
        uint32_t now = host_ticks_ms();
        if (n > 0) {
            np_hdr *h = (np_hdr *)buf;
            if (h->magic != NP_MAGIC || h->version != NP_VERSION) continue;
            if (h->type == T_CHUNK) {
                p_chunk *c = (p_chunk *)buf;
                if (c->offset + c->len <= reg_len && c->len <= CHUNK) { memcpy(reg_blob + c->offset, c->data, c->len); have |= 1ull << (c->offset / CHUNK); }
            } else if (h->type == T_GO && ready) break;
            else if (h->type == T_QUIT) { snprintf(err, errlen, "Player 1 left."); return -1; }
            continue;
        }
        if ((have & all) == all && !ready) {
            if (np_crc32(reg_blob, reg_len, 0) != info.reg_crc) { have = 0; continue; }
            ready = 1;
        }
        if (now - last >= 50) {
            last = now;
            p_chunkack a; hdr(&a.h, T_CHUNKACK); a.have = have; host_net_send(&a, sizeof a);
            if (ready) { np_hdr r; hdr(&r, T_READY); host_net_send(&r, sizeof r); }
        }
        if (host_net_cancelled()) { snprintf(err, errlen, "Cancelled."); return -1; }
        if (now - t0 > 30000) { snprintf(err, errlen, "Player 1 stopped answering."); return -1; }
        host_sleep_ms(2);
    }
    set_session_dirs(game);
    {
        char p[600]; snprintf(p, sizeof p, "%s/registry.txt", g_save_dir_override);
        FILE *f = fopen(p, "wb");
        if (f) { fwrite(reg_blob, 1, reg_len, f); fclose(f); }
    }
    g_fixed_time = info.fixed_time;
    delay = info.delay;
    opt->ds1_difficulty = info.difficulty;
    opt->netplay = 1;
    g_netplay = 1;
    np_init_state();
    host_net_status(NULL, 0);
    rt_log("netplay: joined %s, registry %u bytes, delay %d\n", games[game].id, reg_len, delay);
    return game;
}

void np_init_state(void) {
    for (int k = 0; k < RING; k++) { local_tag[k] = remote_tag[k] = -1; local_in[k] = remote_in[k] = 0; }
    for (int k = 0; k < 64; k++) my_hash_frame[k] = 0xffffffffu;
    /* frames before the first delayed input are empty for both */
    for (int f = 0; f < delay; f++) { local_tag[f] = remote_tag[f] = f; }
    remote_contig = delay - 1;
    sent_upto = delay - 1;
}
