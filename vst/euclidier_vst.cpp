/* =============================================================================
 * euclidier_vst.cpp - Euclidier as a VST2 MIDI generator for the MPC OS plugin
 * host (see https://github.com/sd88me/mpc-vst-plugins). Unlike Force Acid/Crate
 * Digger, euclidier.cpp is a monolithic standalone app (no plugin_api_v2/v1
 * library split), so this wrapper does NOT link the engine in-process. Instead
 * it drives the existing standalone binary exactly the way force-shadow's GUI
 * already does on the Force:
 *
 *   force-shadow (Force)                    euclidier_vst.cpp (MPC plugin)
 *   ------------------------------------    ---------------------------------
 *   engine started by NSMODULE.json/        posix_spawn's the same binary per
 *   run_euclidier.sh, its own process        instance (--ctrl-sock <unique path>)
 *   real MIDI clock -> engine's RtMidi       audioMasterGetTime (ppqPos/tempo)
 *   virtual input port "Euclidier"           synthesises 24-PPQN 0xF8/0xFA/0xFC,
 *                                             sent via our own ALSA seq client
 *                                             into the engine's virtual input port
 *   GET/SET over ctrl_sock (Unix socket)     same protocol, same socket path
 *   engine's own virtual MIDI output port    unchanged -- MPC hot-detects it like
 *   "Euclidier", user routes it in Force     any other MIDI-generator ALSA port
 *                                             (mpc-vst-plugins docs/NOTES.md); no
 *                                             VST MIDI-out plumbing needed here
 *
 * So processReplacing()'s only job is feeding a synthesized MIDI clock into the
 * child's ALSA port; parameter get/set is short blocking request/reply calls on
 * the engine's existing control-socket protocol (euclidier.cpp's ctrlGet/ctrlSet),
 * off the audio thread (JUCE calls setParameter/getParameter from its own message
 * thread, not processReplacing).
 * ========================================================================== */
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef NO_ALSA
#include <alsa/asoundlib.h>
#endif

#include "params.h"

extern char **environ;

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[64]; } VstEvents;
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;

enum {
    effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10 };
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };

static FILE *g_log;
#define LOG(...) do { if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)
static std::atomic<int> g_instance_count{0};

/* ---- default location of the standalone engine binary, override with
 * EUCLIDIER_BIN for host testing. ../DESIGN.md: already deployed at this path
 * on the Force. -------------------------------------------------------------- */
static const char *engine_path() {
    const char *p = getenv("EUCLIDIER_BIN");
    return p && *p ? p : "/media/662522/AddOns/Euclidier/euclidier";
}

/* ---------------------------------------------------------------------------
 * Unix control-socket client -- same "GET key\n"/"SET key val\n" protocol as
 * euclidier.cpp's ctrlThread(). One connect/send/recv/close per request,
 * short-timeout so a wedged child can't hang the caller.
 * ------------------------------------------------------------------------- */
static bool ctrl_request(const std::string &sockpath, const std::string &req, std::string &reply) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct timeval tv = {0, 200 * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_un a;
    std::memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, sockpath.c_str(), sizeof a.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return false; }
    std::string line = req + "\n";
    if (send(fd, line.c_str(), line.size(), MSG_NOSIGNAL) < 0) { close(fd); return false; }
    char buf[256];
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    reply.assign(buf);
    while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) reply.pop_back();
    return true;
}

/* ---- per-instance state ----------------------------------------------------- */
struct Plugin {
    AEffect fx;
    audioMasterCallback master;
    std::string sockpath;
    pid_t child = -1;
    std::mutex lock;               /* serialises requests to the child */
    volatile char release[NPARAMS] = {0};
    double last_ppq = 0.0;
    bool was_playing = false;
#ifndef NO_ALSA
    snd_seq_t *seq = nullptr;
    int seq_port = -1;
    int dest_client = -1, dest_port = -1;
#endif
    char chunk[4096] = {0};
};

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static void copy_str(void *dst, const std::string &s, size_t max) {
    std::strncpy((char *)dst, s.c_str(), max - 1);
    ((char *)dst)[max - 1] = 0;
}
static void norm_to_str(const param_t *p, float n, char *buf, int len) {
    if (p->nopts) std::snprintf(buf, len, "%d", (int)std::lround(clamp01(n) * (p->nopts - 1)));
    else std::snprintf(buf, len, "%d", (int)std::lround(p->min + (p->max - p->min) * clamp01(n)));
}
static float str_to_norm(const param_t *p, const std::string &s) {
    if (p->nopts) {
        int idx = std::atoi(s.c_str());
        if (idx < 0) idx = 0;
        if (idx > p->nopts - 1) idx = p->nopts - 1;
        return p->nopts > 1 ? (float)idx / (p->nopts - 1) : 0.0f;
    }
    return p->max > p->min ? clamp01((float)((std::atof(s.c_str()) - p->min) / (p->max - p->min))) : 0.0f;
}
static float get_norm(Plugin *w, int i) {
    std::string reply;
    std::lock_guard<std::mutex> lk(w->lock);
    if (!ctrl_request(w->sockpath, "GET " + std::string(PARAMS[i].key), reply) || reply == "ERR")
        return PARAMS[i].def;
    return str_to_norm(&PARAMS[i], reply);
}

/* ---------------------------------------------------------------------------
 * Spawn the standalone engine. posix_spawn (never fork() -- mpc-vst-plugins
 * docs/NOTES.md "Beyond synths"), cleaned environment (drop LD_PRELOAD so a
 * MockbaMod-preloaded MPC doesn't hand it a C++ lib the child can't use).
 * ------------------------------------------------------------------------- */
static bool spawn_engine(Plugin *w) {
    const char *bin = engine_path();
    if (access(bin, X_OK) != 0) { LOG("[euclidier_vst] engine binary not found/executable: %s\n", bin); return false; }

    std::vector<std::string> keep_env;
    for (char **e = environ; *e; e++) {
        if (std::strncmp(*e, "LD_PRELOAD=", 11) == 0) continue;
        keep_env.push_back(*e);
    }
    std::vector<char *> envp;
    for (auto &s : keep_env) envp.push_back(&s[0]);
    envp.push_back(nullptr);

    char *argv[] = {
        (char *)bin, (char *)"-v", (char *)"--ctrl-sock", (char *)w->sockpath.c_str(), nullptr
    };
    pid_t pid;
    int rc = posix_spawn(&pid, bin, nullptr, nullptr, argv, envp.data());
    if (rc != 0) { LOG("[euclidier_vst] posix_spawn failed: %s\n", strerror(rc)); return false; }
    w->child = pid;
    LOG("[euclidier_vst] spawned engine pid=%d sock=%s\n", (int)pid, w->sockpath.c_str());

    /* wait for the control socket to come up (engine binds it after ~1s of its
     * own startup delay, see euclidier.cpp main()'s sleep(1)) */
    for (int i = 0; i < 60; i++) {
        std::string reply;
        if (ctrl_request(w->sockpath, "GET transport", reply)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    LOG("[euclidier_vst] engine control socket never came up\n");
    return true; /* keep going -- it may still come up a little late */
}

#ifndef NO_ALSA
/* ---------------------------------------------------------------------------
 * ALSA seq: open our own client/port and connect it to the child engine's
 * virtual "Euclidier" input port so we can feed it a synthesized clock. The
 * engine's own "Euclidier" *output* port needs no help from us -- MPC OS
 * hot-detects any ALSA seq port and the user routes it as a track's MIDI
 * input, same as every other MIDI-generator port (docs/NOTES.md).
 * ------------------------------------------------------------------------- */
static bool find_engine_input_port(int &client, int &port) {
    snd_seq_t *probe;
    if (snd_seq_open(&probe, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) return false;
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    bool found = false;
    while (!found && snd_seq_query_next_client(probe, cinfo) >= 0) {
        int cl = snd_seq_client_info_get_client(cinfo);
        snd_seq_port_info_set_client(pinfo, cl);
        snd_seq_port_info_set_port(pinfo, -1);
        while (!found && snd_seq_query_next_port(probe, pinfo) >= 0) {
            const char *name = snd_seq_port_info_get_name(pinfo);
            unsigned int caps = snd_seq_port_info_get_capability(pinfo);
            if (name && std::strstr(name, "Euclidier") && (caps & SND_SEQ_PORT_CAP_WRITE)) {
                client = cl; port = snd_seq_port_info_get_port(pinfo);
                found = true;
            }
        }
    }
    snd_seq_close(probe);
    return found;
}
static void alsa_open(Plugin *w) {
    if (snd_seq_open(&w->seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) { w->seq = nullptr; return; }
    int n = g_instance_count.fetch_add(1);
    char name[32];
    if (n == 0) std::snprintf(name, sizeof name, "Euclidier Clock");
    else std::snprintf(name, sizeof name, "Euclidier Clock %d", n + 1);
    snd_seq_set_client_name(w->seq, name);
    w->seq_port = snd_seq_create_simple_port(w->seq, "Clock Out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
}
static void alsa_send_byte(Plugin *w, unsigned char b) {
    if (!w->seq || w->seq_port < 0) return;
    if (w->dest_client < 0 && !find_engine_input_port(w->dest_client, w->dest_port)) return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, w->seq_port);
    snd_seq_ev_set_dest(&ev, w->dest_client, w->dest_port);
    snd_seq_ev_set_direct(&ev);
    ev.type = SND_SEQ_EVENT_CLOCK;
    if (b == 0xFA) ev.type = SND_SEQ_EVENT_START;
    else if (b == 0xFB) ev.type = SND_SEQ_EVENT_CONTINUE;
    else if (b == 0xFC) ev.type = SND_SEQ_EVENT_STOP;
    snd_seq_event_output_direct(w->seq, &ev);
}
static void alsa_close(Plugin *w) {
    if (w->seq) snd_seq_close(w->seq);
    w->seq = nullptr;
}
#else
static void alsa_open(Plugin *) {}
static void alsa_send_byte(Plugin *, unsigned char) {}
static void alsa_close(Plugin *) {}
#endif

/* ---------------------------------------------------------------------------
 * Transport / clock synthesis: audioMasterGetTime -> synthetic 24-PPQN clock
 * sent to the child over ALSA seq, same math as acid_vst.cpp's feed_transport.
 * ------------------------------------------------------------------------- */
static void feed_transport(Plugin *w) {
    VstTimeInfo *ti = (VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0,
                                                kVstTempoValid | kVstPpqPosValid, 0, 0);
    bool playing = ti && (ti->flags & kVstTransportPlaying);

    if (playing && !w->was_playing) { w->last_ppq = ti->ppqPos; alsa_send_byte(w, 0xFA); }
    else if (!playing && w->was_playing) alsa_send_byte(w, 0xFC);
    w->was_playing = playing;

    if (playing && ti) {
        const double step = 1.0 / 24.0;
        double start = w->last_ppq, end = ti->ppqPos;
        if (end < start) start = end;
        double next = std::ceil(start / step) * step;
        for (; next < end + 1e-9; next += step) alsa_send_byte(w, 0xF8);
        w->last_ppq = ti->ppqPos;
    }
}

/* ---------------------------------------------------------------------------
 * VST callbacks
 * ------------------------------------------------------------------------- */
static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    (void)in;
    Plugin *w = (Plugin *)e->object;
    feed_transport(w);
    for (int i = 0; i < NPARAMS; i++)
        if (w->release[i]) { w->release[i] = 0; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
    for (int32_t i = 0; i < n; i++) out[0][i] = out[1][i] = 0.0f;   /* MIDI generator: no audio */
}

static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (p->momentary) {
        if (n > 0.5f) {
            std::string reply;
            std::lock_guard<std::mutex> lk(w->lock);
            ctrl_request(w->sockpath, std::string("SET ") + p->key + " 1", reply);
            w->release[i] = 1;
        }
        return;
    }
    char buf[32];
    if (p->nopts > 1) {
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = get_norm(w, i) * (p->nopts - 1);
            int idx = (int)std::lround(cur) + (pos > cur ? 1 : -1);
            if (idx < 0) idx = 0;
            if (idx > p->nopts - 1) idx = p->nopts - 1;
            n = (float)idx / (p->nopts - 1);
        }
    }
    norm_to_str(p, n, buf, sizeof buf);
    std::string reply;
    std::lock_guard<std::mutex> lk(w->lock);
    ctrl_request(w->sockpath, std::string("SET ") + p->key + " " + buf, reply);
}

static float getParameter(AEffect *e, int32_t i) { return get_norm((Plugin *)e->object, i); }

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    (void)o;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        alsa_close(w);
        if (w->child > 0) { kill(w->child, SIGTERM); int st; waitpid(w->child, &st, 0); }
        delete w;
        return 1;
    case effGetPlugCategory: return 2;
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS;
    case effGetParamName:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32);
        return 1;
    case effGetParamLabel:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8);
        return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        const param_t *pp = &PARAMS[idx];
        if (pp->momentary) { copy_str(p, "", 24); return 1; }
        if (pp->nopts) {
            int k = (int)std::lround(get_norm(w, idx) * (pp->nopts - 1));
            copy_str(p, pp->opts[k], 24);
        } else {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%d",
                (int)std::lround(pp->min + (pp->max - pp->min) * get_norm(w, idx)));
            copy_str(p, buf, 24);
        }
        return 1;
    }
    case effSetSampleRate: case effSetBlockSize: case effMainsChanged: return 1;
    case effProcessEvents: return 1; /* MPC ignores plugin MIDI in for a generator; no-op */
    case effCanDo:
        return (!std::strcmp((char *)p, "receiveVstTimeInfo")) ? 1 : -1;
    case effGetChunk: {
        std::string s;
        for (int i = 0; i < NPARAMS; i++) {
            if (PARAMS[i].momentary) continue;
            std::string reply;
            { std::lock_guard<std::mutex> lk(w->lock);
              if (!ctrl_request(w->sockpath, std::string("GET ") + PARAMS[i].key, reply)) continue; }
            if (reply == "ERR") continue;
            s += PARAMS[i].key; s += '='; s += reply; s += ';';
        }
        copy_str(w->chunk, s, sizeof w->chunk);
        *(void **)p = w->chunk;
        return (intptr_t)std::strlen(w->chunk) + 1;
    }
    case effSetChunk: {
        if (v <= 0 || (size_t)v > sizeof w->chunk) return 0;
        std::memcpy(w->chunk, p, (size_t)v);
        w->chunk[v - 1] = 0;
        std::lock_guard<std::mutex> lk(w->lock);
        char *s = w->chunk, *save = nullptr;
        for (char *tok = strtok_r(s, ";", &save); tok; tok = strtok_r(nullptr, ";", &save)) {
            char *eq = std::strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            std::string reply;
            ctrl_request(w->sockpath, std::string("SET ") + tok + " " + (eq + 1), reply);
        }
        return 1;
    }
    default: return 0;
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    if (!g_log) g_log = std::fopen("/tmp/euclidier_vst.log", "a");
    Plugin *w = new Plugin();
    w->master = master;
    int n = g_instance_count.load();
    char sp[64];
    std::snprintf(sp, sizeof sp, "/tmp/euclidier_vst_%d_%d.sock", (int)getpid(), n);
    w->sockpath = sp;
    spawn_engine(w);
    alsa_open(w);

    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450; /* 'VstP' */
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsIsSynth | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    LOG("[euclidier_vst] up, %d params\n", NPARAMS);
    return e;
}
