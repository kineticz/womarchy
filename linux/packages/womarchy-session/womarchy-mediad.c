/*
 * womarchy-mediad: the session's audio devices, backed by Windows through omarchy.exe (protocol/wdp.h,
 * "media channel").
 *
 * Listens on vsock port WOMARCHY_VSOCK_PORT + 2 for omarchy.exe and authenticates it with the session
 * token WOMARCHY_MEDIA_TOKEN (64 hex chars) the way womarchy-clipd does: the viewer proves the first
 * half (MEDIA_HELLO), we answer with the second (MEDIA_WELCOME). While a viewer is connected we offer
 * two PipeWire nodes:
 *
 *   womarchy-sink   (Audio/Sink)   what Linux apps play goes to the viewer as PCM; it plays it with
 *                                  WASAPI on Windows' default output device.
 *   womarchy-source (Audio/Source) the viewer records Windows' default microphone with WASAPI and
 *                                  sends PCM; Linux apps record it from here.
 *
 * The nodes are ordinary pw_streams; WirePlumber suspends them when nobody uses them, and we follow
 * their state: the sink's stream starts when an app plays (AUDIO_PLAY_START) and stops when the node
 * is suspended (AUDIO_PLAY_STOP); the microphone is only opened on Windows while something records
 * from the source (AUDIO_CAPTURE_START / AUDIO_CAPTURE_STOP), so Windows' "microphone in use"
 * indicator means what it says. Clock drift: the playback side is corrected by the viewer (it adjusts
 * WASAPI's sample rate to its buffer level); the capture side here, by dropping excess frames.
 *
 * Testing: WOMARCHY_MEDIA_UNIX=/path listens on a Unix socket instead of vsock.
 * Runs as the user service womarchy-mediad.service inside the graphical session.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <linux/vm_sockets.h>

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/json.h>
#include <spa/utils/result.h>

/* --- protocol (protocol/wdp.h) */
#define WDP_VERSION 2u
#define WDP_PROOF_BYTES 16u
#define WDP_MEDIA_PORT_OFFSET 2u
#define WDP_MEDIA_HELLO 300u
#define WDP_MEDIA_WELCOME 301u
#define WDP_AUDIO_PLAY_START 310u
#define WDP_AUDIO_PLAY_DATA 311u
#define WDP_AUDIO_PLAY_STOP 312u
#define WDP_AUDIO_CAPTURE_START 320u
#define WDP_AUDIO_CAPTURE_DATA 321u
#define WDP_AUDIO_CAPTURE_STOP 322u
#define WDP_AUDIO_CAPTURE_FAILED 323u
#define WDP_MEDIA_CAP_PLAYBACK (1u << 0)
#define WDP_MEDIA_CAP_CAPTURE (1u << 1)
#define WDP_AUDIO_F32LE 1u
#define WDP_MAX_AUDIO_DATA (1u << 20)

/* --- our formats */
#define RATE 48000u
#define PLAY_CHANNELS 2u
#define CAPTURE_CHANNELS 1u
#define HELLO_TIMEOUT_MS 5000
/* playback: if the viewer stops reading, drop audio rather than queue more than this */
#define MAX_QUEUED_BYTES (RATE * PLAY_CHANNELS * 4 / 2) /* 0.5 s */
/* capture: aim for this much buffered (absorbs the viewer's packet timing), cap at target + slack */
#define CAPTURE_TARGET_FRAMES (RATE * 40 / 1000)
#define CAPTURE_SLACK_FRAMES (RATE * 80 / 1000)
#define CAPTURE_RING_FRAMES (RATE) /* 1 s */

#define SINK_NAME "womarchy-sink"
#define SOURCE_NAME "womarchy-source"

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("womarchy-mediad: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

struct ring {
    float *data;
    uint32_t cap, rd, len; /* in samples */
};

struct mediad {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct spa_hook core_listener;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    struct pw_metadata *metadata;
    struct spa_hook metadata_listener;
    char configured_sink[256], configured_source[256];
    int defaults_sync;

    uint8_t token[2 * WDP_PROOF_BYTES];
    int listen_fd;
    struct spa_source *listen_src;

    /* the viewer connection (one at a time) */
    int fd;
    struct spa_source *conn_src;
    uint8_t *in;
    size_t in_len, in_cap;
    uint8_t *out;
    size_t out_len, out_cap;
    uint32_t caps;
    bool dropping; /* playback data is being dropped because the viewer is not reading */

    struct pw_stream *sink;
    struct spa_hook sink_listener;
    bool playing;

    struct pw_stream *source;
    struct spa_hook source_listener;
    bool capturing, priming;
    struct ring ring;
};

/* --- output queue */

static void conn_close(struct mediad *m, const char *why);

static void flush_out(struct mediad *m) {
    while (m->out_len > 0) {
        ssize_t n = send(m->fd, m->out, m->out_len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            conn_close(m, strerror(errno));
            return;
        }
        memmove(m->out, m->out + n, m->out_len - n);
        m->out_len -= n;
    }
    if (m->fd >= 0)
        pw_loop_update_io(pw_main_loop_get_loop(m->loop), m->conn_src, SPA_IO_IN | (m->out_len ? SPA_IO_OUT : 0));
}

/* Queue one message. Audio data is dropped (not control messages) while the viewer lags far behind. */
static void send_msg(struct mediad *m, uint32_t type, const void *a, size_t alen, const void *b, size_t blen) {
    if (m->fd < 0)
        return;
    size_t size = alen + blen, need = m->out_len + 8 + size;
    if (type == WDP_AUDIO_PLAY_DATA && m->out_len + size > MAX_QUEUED_BYTES) {
        if (!m->dropping)
            logf_("the viewer is not keeping up; dropping playback");
        m->dropping = true;
        return;
    }
    m->dropping = false;
    if (need > m->out_cap) {
        size_t cap = m->out_cap ? m->out_cap : 65536;
        while (cap < need)
            cap *= 2;
        uint8_t *p = realloc(m->out, cap);
        if (!p)
            return;
        m->out = p;
        m->out_cap = cap;
    }
    uint32_t hdr[2] = {htole32(type), htole32((uint32_t)size)};
    memcpy(m->out + m->out_len, hdr, 8);
    if (alen)
        memcpy(m->out + m->out_len + 8, a, alen);
    if (blen)
        memcpy(m->out + m->out_len + 8 + alen, b, blen);
    m->out_len = need;
    flush_out(m);
}

static void send_format(struct mediad *m, uint32_t type, uint32_t channels) {
    uint32_t f[3] = {htole32(RATE), htole32(channels), htole32(WDP_AUDIO_F32LE)};
    send_msg(m, type, f, sizeof f, NULL, 0);
}

/* --- capture ring (mono/interleaved float samples) */

static void ring_clear(struct ring *r) { r->rd = r->len = 0; }

static void ring_push(struct ring *r, const float *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (r->len == r->cap) { /* full: the oldest go */
            r->rd = (r->rd + 1) % r->cap;
            r->len--;
        }
        r->data[(r->rd + r->len) % r->cap] = s[i];
        r->len++;
    }
}

static uint32_t ring_pop(struct ring *r, float *s, uint32_t n) {
    if (n > r->len)
        n = r->len;
    for (uint32_t i = 0; i < n; i++)
        s[i] = r->data[(r->rd + i) % r->cap];
    r->rd = (r->rd + n) % r->cap;
    r->len -= n;
    return n;
}

static void ring_skip(struct ring *r, uint32_t n) {
    if (n > r->len)
        n = r->len;
    r->rd = (r->rd + n) % r->cap;
    r->len -= n;
}

/* --- the sink: Linux apps -> viewer */

static void on_sink_process(void *data) {
    struct mediad *m = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(m->sink);
    if (!b)
        return;
    struct spa_data *d = &b->buffer->datas[0];
    if (d->data && d->chunk->size > 0 && m->fd >= 0) {
        uint32_t off = SPA_MIN(d->chunk->offset, d->maxsize);
        uint32_t size = SPA_MIN(d->chunk->size, d->maxsize - off);
        if (!m->playing) {
            send_format(m, WDP_AUDIO_PLAY_START, PLAY_CHANNELS);
            m->playing = true;
        }
        send_msg(m, WDP_AUDIO_PLAY_DATA, SPA_PTROFF(d->data, off, void), size, NULL, 0);
    }
    pw_stream_queue_buffer(m->sink, b);
}

static void on_sink_state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    struct mediad *m = data;
    (void)old;
    if (state == PW_STREAM_STATE_ERROR)
        logf_("sink: %s", error ? error : "error");
    if (state != PW_STREAM_STATE_STREAMING && m->playing) {
        send_msg(m, WDP_AUDIO_PLAY_STOP, NULL, 0, NULL, 0);
        m->playing = false;
    }
}

static const struct pw_stream_events sink_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_sink_state,
    .process = on_sink_process,
};

/* --- the source: viewer -> Linux apps */

static void on_source_process(void *data) {
    struct mediad *m = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(m->source);
    if (!b)
        return;
    struct spa_data *d = &b->buffer->datas[0];
    const uint32_t stride = 4 * CAPTURE_CHANNELS;
    if (!d->data) {
        pw_stream_queue_buffer(m->source, b);
        return;
    }
    uint32_t frames = d->maxsize / stride;
    if (b->requested && b->requested < frames)
        frames = b->requested;
    float *out = d->data;
    uint32_t want = frames * CAPTURE_CHANNELS, got = 0;
    uint32_t have = m->ring.len / CAPTURE_CHANNELS;
    if (m->priming && have >= CAPTURE_TARGET_FRAMES)
        m->priming = false;
    if (!m->priming) {
        if (have > CAPTURE_TARGET_FRAMES + CAPTURE_SLACK_FRAMES + frames) /* the viewer's clock runs fast */
            ring_skip(&m->ring, (have - CAPTURE_TARGET_FRAMES - frames) * CAPTURE_CHANNELS);
        got = ring_pop(&m->ring, out, want);
        if (got < want)
            m->priming = true; /* underrun: build the cushion up again */
    }
    memset(out + got, 0, (want - got) * sizeof(float));
    d->chunk->offset = 0;
    d->chunk->stride = stride;
    d->chunk->size = frames * stride;
    pw_stream_queue_buffer(m->source, b);
}

static void on_source_state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    struct mediad *m = data;
    (void)old;
    if (state == PW_STREAM_STATE_ERROR)
        logf_("source: %s", error ? error : "error");
    bool want = state == PW_STREAM_STATE_STREAMING;
    if (want == m->capturing)
        return;
    m->capturing = want;
    ring_clear(&m->ring);
    m->priming = true;
    if (want)
        send_format(m, WDP_AUDIO_CAPTURE_START, CAPTURE_CHANNELS);
    else
        send_msg(m, WDP_AUDIO_CAPTURE_STOP, NULL, 0, NULL, 0);
    logf_("microphone %s", want ? "opened (an app is recording)" : "closed");
}

static const struct pw_stream_events source_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_source_state,
    .process = on_source_process,
};

static struct pw_stream *make_stream(struct mediad *m, bool sink) {
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, sink ? "Playback" : "Capture",
        PW_KEY_MEDIA_CLASS, sink ? "Audio/Sink" : "Audio/Source",
        PW_KEY_NODE_NAME, sink ? SINK_NAME : SOURCE_NAME,
        PW_KEY_NODE_DESCRIPTION, sink ? "Windows audio" : "Windows microphone",
        "device.icon-name", sink ? "audio-card" : "audio-input-microphone",
        /* above WSLg's tunnels (2000), so a fresh default picks us */
        "priority.session", "2500",
        "priority.driver", "2500",
        NULL);
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%u/%u", RATE / 100, RATE); /* 10 ms quanta */
    struct pw_stream *s = pw_stream_new(m->core, sink ? SINK_NAME : SOURCE_NAME, props);
    if (!s)
        return NULL;
    pw_stream_add_listener(s, sink ? &m->sink_listener : &m->source_listener, sink ? &sink_events : &source_events, m);

    uint8_t buf[1024];
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    struct spa_audio_info_raw info = {
        .format = SPA_AUDIO_FORMAT_F32_LE,
        .rate = RATE,
        .channels = sink ? PLAY_CHANNELS : CAPTURE_CHANNELS,
    };
    if (sink) {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    } else {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    }
    const struct spa_pod *params[1] = {spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat, &info)};
    if (pw_stream_connect(s, sink ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT, PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0) {
        pw_stream_destroy(s);
        return NULL;
    }
    return s;
}

/* --- defaults: when the configured default is WSLg's tunnel (or nothing), make ours the default */

static void json_name(const char *value, char *out, size_t len) {
    out[0] = 0;
    if (value && spa_json_str_object_find(value, strlen(value), "name", out, len) < 0)
        out[0] = 0;
}

static int on_metadata_property(void *data, uint32_t subject, const char *key, const char *type, const char *value) {
    struct mediad *m = data;
    (void)type;
    if (subject != PW_ID_CORE || !key)
        return 0;
    if (strcmp(key, "default.configured.audio.sink") == 0)
        json_name(value, m->configured_sink, sizeof m->configured_sink);
    else if (strcmp(key, "default.configured.audio.source") == 0)
        json_name(value, m->configured_source, sizeof m->configured_source);
    return 0;
}

static const struct pw_metadata_events metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = on_metadata_property,
};

static void on_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const struct spa_dict *props) {
    struct mediad *m = data;
    (void)permissions;
    (void)version;
    if (m->metadata || strcmp(type, PW_TYPE_INTERFACE_Metadata) != 0)
        return;
    const char *name = props ? spa_dict_lookup(props, PW_KEY_METADATA_NAME) : NULL;
    if (!name || strcmp(name, "default") != 0)
        return;
    m->metadata = pw_registry_bind(m->registry, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0);
    if (m->metadata)
        pw_metadata_add_listener(m->metadata, &m->metadata_listener, &metadata_events, m);
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_global,
};

static bool replaceable_default(const char *name) { return !name[0] || strncmp(name, "wslg-", 5) == 0; }

static void claim_defaults(struct mediad *m) {
    if (!m->metadata)
        return;
    if (m->sink && replaceable_default(m->configured_sink)) {
        logf_("default output: %s -> " SINK_NAME, m->configured_sink[0] ? m->configured_sink : "(none)");
        pw_metadata_set_property(m->metadata, PW_ID_CORE, "default.configured.audio.sink", "Spa:String:JSON", "{ \"name\": \"" SINK_NAME "\" }");
    }
    if (m->source && replaceable_default(m->configured_source)) {
        logf_("default input: %s -> " SOURCE_NAME, m->configured_source[0] ? m->configured_source : "(none)");
        pw_metadata_set_property(m->metadata, PW_ID_CORE, "default.configured.audio.source", "Spa:String:JSON", "{ \"name\": \"" SOURCE_NAME "\" }");
    }
}

static void on_core_done(void *data, uint32_t id, int seq) {
    struct mediad *m = data;
    /* the registry and the "default" metadata were bound at startup, long before a viewer connects;
     * this round trip after creating our nodes only makes sure their properties have arrived */
    if (id != PW_ID_CORE || !m->defaults_sync || seq != m->defaults_sync)
        return;
    m->defaults_sync = 0;
    claim_defaults(m);
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message) {
    struct mediad *m = data;
    (void)seq;
    logf_("pipewire error on %u: %s (%s)", id, message, spa_strerror(res));
    if (id == PW_ID_CORE && res == -EPIPE)
        pw_main_loop_quit(m->loop); /* PipeWire went away; systemd restarts us */
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
    .error = on_core_error,
};

/* --- the connection */

static void streams_stop(struct mediad *m) {
    if (m->sink) {
        pw_stream_destroy(m->sink);
        m->sink = NULL;
    }
    if (m->source) {
        pw_stream_destroy(m->source);
        m->source = NULL;
    }
    m->playing = m->capturing = false;
    ring_clear(&m->ring);
}

static void conn_close(struct mediad *m, const char *why) {
    if (m->fd < 0)
        return;
    logf_("viewer disconnected (%s)", why);
    pw_loop_destroy_source(pw_main_loop_get_loop(m->loop), m->conn_src);
    m->conn_src = NULL;
    close(m->fd);
    m->fd = -1;
    m->in_len = m->out_len = 0;
    streams_stop(m);
}

static void handle_msg(struct mediad *m, uint32_t type, const uint8_t *p, uint32_t size) {
    switch (type) {
    case WDP_AUDIO_CAPTURE_DATA:
        if (m->capturing && size % 4 == 0)
            ring_push(&m->ring, (const float *)p, size / 4); /* little-endian float on both ends */
        break;
    case WDP_AUDIO_CAPTURE_FAILED: {
        uint32_t hr = size >= 4 ? le32toh(*(const uint32_t *)p) : 0;
        logf_("the viewer cannot open the Windows microphone (HRESULT 0x%08x; microphone access for desktop apps off?)", hr);
        break;
    }
    default:
        break; /* unknown: skip */
    }
}

static void on_conn_io(void *data, int fd, uint32_t mask) {
    struct mediad *m = data;
    if (mask & SPA_IO_OUT)
        flush_out(m);
    if (m->fd < 0 || !(mask & (SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR)))
        return;
    for (;;) {
        if (m->in_cap - m->in_len < 65536) {
            size_t cap = m->in_cap ? m->in_cap * 2 : 131072;
            uint8_t *p = realloc(m->in, cap);
            if (!p) {
                conn_close(m, "out of memory");
                return;
            }
            m->in = p;
            m->in_cap = cap;
        }
        ssize_t n = recv(fd, m->in + m->in_len, m->in_cap - m->in_len, MSG_DONTWAIT);
        if (n == 0) {
            conn_close(m, "closed");
            return;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                conn_close(m, strerror(errno));
            break;
        }
        m->in_len += n;
    }
    size_t off = 0;
    while (m->fd >= 0 && m->in_len - off >= 8) {
        uint32_t type = le32toh(*(uint32_t *)(m->in + off)), size = le32toh(*(uint32_t *)(m->in + off + 4));
        if (size > WDP_MAX_AUDIO_DATA) {
            conn_close(m, "oversized message");
            return;
        }
        if (m->in_len - off < 8 + (size_t)size)
            break;
        handle_msg(m, type, m->in + off + 8, size);
        off += 8 + size;
    }
    if (m->fd >= 0 && off) {
        memmove(m->in, m->in + off, m->in_len - off);
        m->in_len -= off;
    }
}

static bool recv_exact(int fd, void *buf, size_t n, int64_t deadline_ms) {
    uint8_t *p = buf;
    while (n > 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t left = deadline_ms - (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
        if (left <= 0)
            return false;
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (poll(&pfd, 1, (int)left) <= 0)
            return false;
        ssize_t r = recv(fd, p, n, 0);
        if (r <= 0)
            return false;
        p += r;
        n -= r;
    }
    return true;
}

/* Read exactly one MEDIA_HELLO in time and check its proof; returns the viewer's capabilities, or -1. */
static int64_t authenticate(struct mediad *m, int fd) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t deadline = ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + HELLO_TIMEOUT_MS;
    uint32_t hdr[2];
    uint8_t body[256];
    if (!recv_exact(fd, hdr, 8, deadline))
        return -1;
    uint32_t type = le32toh(hdr[0]), size = le32toh(hdr[1]);
    if (type != WDP_MEDIA_HELLO || size < 4 + WDP_PROOF_BYTES || size > sizeof body || !recv_exact(fd, body, size, deadline))
        return -1;
    uint32_t version = le32toh(*(uint32_t *)body);
    uint8_t diff = 0;
    for (unsigned i = 0; i < WDP_PROOF_BYTES; i++)
        diff |= body[4 + i] ^ m->token[i];
    if (version != WDP_VERSION || diff)
        return -1;
    uint32_t caps = size >= 4 + WDP_PROOF_BYTES + 4 ? le32toh(*(uint32_t *)(body + 4 + WDP_PROOF_BYTES)) : 0;
    uint8_t welcome[8 + 4 + WDP_PROOF_BYTES];
    uint32_t w[3] = {htole32(WDP_MEDIA_WELCOME), htole32(4 + WDP_PROOF_BYTES), htole32(WDP_VERSION)};
    memcpy(welcome, w, 12);
    memcpy(welcome + 12, m->token + WDP_PROOF_BYTES, WDP_PROOF_BYTES);
    if (send(fd, welcome, sizeof welcome, MSG_NOSIGNAL) != (ssize_t)sizeof welcome)
        return -1;
    return caps;
}

static void on_accept(void *data, int fd, uint32_t mask) {
    struct mediad *m = data;
    (void)mask;
    int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
    if (c < 0)
        return;
    int64_t caps = authenticate(m, c);
    if (caps < 0) {
        close(c);
        return;
    }
    conn_close(m, "replaced by a new connection");
    fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
    m->fd = c;
    m->caps = (uint32_t)caps;
    m->dropping = false;
    m->conn_src = pw_loop_add_io(pw_main_loop_get_loop(m->loop), c, SPA_IO_IN, false, on_conn_io, m);
    logf_("viewer connected (%s%s)", caps & WDP_MEDIA_CAP_PLAYBACK ? "speakers " : "", caps & WDP_MEDIA_CAP_CAPTURE ? "microphone" : "");
    if (caps & WDP_MEDIA_CAP_PLAYBACK)
        m->sink = make_stream(m, true);
    if (caps & WDP_MEDIA_CAP_CAPTURE)
        m->source = make_stream(m, false);
    if ((caps & WDP_MEDIA_CAP_PLAYBACK) && !m->sink)
        logf_("could not create the " SINK_NAME " node");
    if ((caps & WDP_MEDIA_CAP_CAPTURE) && !m->source)
        logf_("could not create the " SOURCE_NAME " node");
    if (m->sink || m->source)
        m->defaults_sync = pw_core_sync(m->core, PW_ID_CORE, 0);
}

static int listen_socket(uint32_t port) {
    const char *unix_path = getenv("WOMARCHY_MEDIA_UNIX");
    int fd;
    if (unix_path && *unix_path) {
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        snprintf(a.sun_path, sizeof a.sun_path, "%s", unix_path);
        unlink(unix_path);
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0)
            goto fail;
        logf_("listening on %s", unix_path);
    } else {
        struct sockaddr_vm a = {.svm_family = AF_VSOCK, .svm_cid = VMADDR_CID_ANY, .svm_port = port};
        fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0)
            goto fail;
        logf_("listening on vsock port %u", port);
    }
    if (listen(fd, 2) < 0)
        goto fail;
    return fd;
fail:
    logf_("cannot listen: %s", strerror(errno));
    if (fd >= 0)
        close(fd);
    return -1;
}

static void on_signal(void *data, int sig) {
    (void)sig;
    pw_main_loop_quit(((struct mediad *)data)->loop);
}

int main(void) {
    static struct mediad m = {.fd = -1, .listen_fd = -1};
    const char *tok = getenv("WOMARCHY_MEDIA_TOKEN"), *port_s = getenv("WOMARCHY_VSOCK_PORT");
    if (!tok || strlen(tok) != 4 * WDP_PROOF_BYTES || !port_s || !*port_s) {
        logf_("WOMARCHY_MEDIA_TOKEN (64 hex characters) and WOMARCHY_VSOCK_PORT are required");
        return 1;
    }
    for (unsigned i = 0; i < 2 * WDP_PROOF_BYTES; i++) {
        unsigned v;
        if (sscanf(tok + 2 * i, "%2x", &v) != 1) {
            logf_("WOMARCHY_MEDIA_TOKEN is not hex");
            return 1;
        }
        m.token[i] = (uint8_t)v;
    }
    char *end;
    unsigned long port = strtoul(port_s, &end, 10);
    if (*end || port == 0 || port > 0xfffffffful - WDP_MEDIA_PORT_OFFSET) {
        logf_("WOMARCHY_VSOCK_PORT is not a port number");
        return 1;
    }

    m.ring.cap = CAPTURE_RING_FRAMES * CAPTURE_CHANNELS;
    m.ring.data = calloc(m.ring.cap, sizeof(float));

    pw_init(NULL, NULL);
    m.loop = pw_main_loop_new(NULL);
    struct pw_loop *loop = pw_main_loop_get_loop(m.loop);
    pw_loop_add_signal(loop, SIGINT, on_signal, &m);
    pw_loop_add_signal(loop, SIGTERM, on_signal, &m);
    m.context = pw_context_new(loop, NULL, 0);
    m.core = m.context ? pw_context_connect(m.context, NULL, 0) : NULL;
    if (!m.core) {
        logf_("cannot connect to PipeWire: %s", strerror(errno));
        return 1;
    }
    pw_core_add_listener(m.core, &m.core_listener, &core_events, &m);
    m.registry = pw_core_get_registry(m.core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(m.registry, &m.registry_listener, &registry_events, &m);

    m.listen_fd = listen_socket((uint32_t)port + WDP_MEDIA_PORT_OFFSET);
    if (m.listen_fd < 0)
        return 1;
    m.listen_src = pw_loop_add_io(loop, m.listen_fd, SPA_IO_IN, false, on_accept, &m);

    pw_main_loop_run(m.loop);

    conn_close(&m, "exiting");
    if (m.metadata)
        pw_proxy_destroy((struct pw_proxy *)m.metadata);
    pw_proxy_destroy((struct pw_proxy *)m.registry);
    pw_core_disconnect(m.core);
    pw_context_destroy(m.context);
    pw_main_loop_destroy(m.loop);
    pw_deinit();
    return 0;
}
