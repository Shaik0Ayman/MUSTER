/*
 * muster.h - Muster runtime: a framework for multi-MCU embedded systems.
 *
 * Single-header C99 library. Include it anywhere; in EXACTLY ONE source file
 * (the muster toolchain generates this file for you) write:
 *
 *     #define MUSTER_IMPLEMENTATION
 *     #include "muster.h"
 *
 * Model
 *   - One LEAD board holds the node table, makes every placement decision and
 *     relays every result (hub and spoke: it sees all traffic, which is what
 *     makes the dashboard possible). WORKERS join, execute, report.
 *   - Code is resident, placement is live: every board is flashed with the
 *     tasks it may run; the lead picks among the boards that hold the task.
 *   - A board may sit on several links (CAN to everyone, SPI to a neighbour).
 *     Unicast goes over the fastest link the peer was recently heard on;
 *     broadcasts and heartbeats go over every link.
 *   - Everything is static. No malloc, no recursion. Single-threaded: call
 *     muster_poll() from one context only (loop() or one RTOS task).
 *
 * Portability rules that bite across 8-bit and 32-bit boards:
 *   - Never send raw structs. Encode with muster_put_* / muster_get_*.
 *   - `int` is 16-bit on AVR, 32-bit on ESP32; `double` is 32-bit on AVR.
 *     Task signatures must use fixed-width types and float.
 */
#ifndef MUSTER_H
#define MUSTER_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ------------------------------------------------------------------ config */
#if defined(__AVR__) /* 2 KB of RAM: keep everything small        */
#define MUSTER_DEF_NODES 4
#define MUSTER_DEF_MSG 40
#define MUSTER_DEF_PENDING 2
#define MUSTER_DEF_RX_SLOTS 2
#define MUSTER_DEF_LINKS 1
#define MUSTER_DEF_DEDUPE 1
#else
#define MUSTER_DEF_NODES 8
#define MUSTER_DEF_MSG 96
#define MUSTER_DEF_PENDING 6
#define MUSTER_DEF_RX_SLOTS 4
#define MUSTER_DEF_LINKS 2
#define MUSTER_DEF_DEDUPE 4
#endif
#ifndef MUSTER_MAX_NODES
#define MUSTER_MAX_NODES MUSTER_DEF_NODES /* includes the lead       */
#endif
#ifndef MUSTER_MAX_TASKS
#define MUSTER_MAX_TASKS 16 /* task IDs 0..15          */
#endif
#ifndef MUSTER_MAX_MSG
#define MUSTER_MAX_MSG MUSTER_DEF_MSG /* reassembled msg bytes   */
#endif
#ifndef MUSTER_MAX_PENDING
#define MUSTER_MAX_PENDING MUSTER_DEF_PENDING /* calls in flight         */
#endif
#ifndef MUSTER_RX_SLOTS
#define MUSTER_RX_SLOTS MUSTER_DEF_RX_SLOTS /* concurrent reassemblies */
#endif
#ifndef MUSTER_MAX_LINKS
#define MUSTER_MAX_LINKS MUSTER_DEF_LINKS /* links per board         */
#endif
#ifndef MUSTER_MAX_FRAME
#define MUSTER_MAX_FRAME 64 /* >= every link's MTU     */
#endif
/* Finished calls remembered so a resent CALL is answered, not re-run: on a
 * worker, calls it executed; on the lead, results it relayed to workers.
 * Each entry holds a full result (MUSTER_MAX_MSG - 5 bytes).               */
#ifndef MUSTER_DEDUPE
#define MUSTER_DEDUPE MUSTER_DEF_DEDUPE
#endif
#ifndef MUSTER_HEARTBEAT_MS
#define MUSTER_HEARTBEAT_MS 200u
#endif
#ifndef MUSTER_NODE_TIMEOUT_MS
#define MUSTER_NODE_TIMEOUT_MS 1000u /* peer (or one link to it) lost */
#endif
#ifndef MUSTER_CALL_TIMEOUT_MS
#define MUSTER_CALL_TIMEOUT_MS 400u /* lead: then retry          */
#endif
#ifndef MUSTER_MAX_RETRIES
#define MUSTER_MAX_RETRIES 2u
#endif
/* A worker resends an unanswered CALL every MUSTER_CALL_TIMEOUT_MS (up to
 * MUSTER_MAX_RETRIES times) and gives up after this deadline. It must
 * outlast a resend plus every retry the lead may make in turn.             */
#ifndef MUSTER_CALL_DEADLINE_MS
#define MUSTER_CALL_DEADLINE_MS (MUSTER_CALL_TIMEOUT_MS * (2u * MUSTER_MAX_RETRIES + 2u))
#endif
/* The lead also runs the application and coordinates the bus, so running a
 * task locally is not free. This penalty is what lets idle workers win
 * placement; set it to 0 for a lead that should do as much as it can.     */
#ifndef MUSTER_LEAD_PENALTY
#define MUSTER_LEAD_PENALTY 3000u
#endif

#if MUSTER_MAX_NODES > 8
#error "MUSTER_MAX_NODES > 8 needs a wider retry mask"
#endif
#if MUSTER_MAX_TASKS > 32
#error "MUSTER_MAX_TASKS > 32: the resident mask is 32 bits"
#endif
#if MUSTER_MAX_LINKS < 1 || MUSTER_MAX_LINKS > 8
#error "MUSTER_MAX_LINKS must be 1..8"
#endif
#if MUSTER_MAX_MSG < 16
#error "MUSTER_MAX_MSG must hold a JOIN (14 bytes)"
#endif
#if MUSTER_DEDUPE < 1
#error "MUSTER_DEDUPE must be at least 1"
#endif

#define MUSTER_PROTO_VERSION 2u
#define MUSTER_BROADCAST 0xFFu
#define MUSTER_NO_NODE 0xFEu

/* largest argument / result payload a single call can carry */
#define MUSTER_MAX_ARGS (MUSTER_MAX_MSG - 6u)
#define MUSTER_MAX_RET (MUSTER_MAX_MSG - 5u)

/* capability flags, matching the board database */
#define MUSTER_CAP_FPU 0x01u
#define MUSTER_CAP_WIFI 0x02u
#define MUSTER_CAP_BLE 0x04u

/* call status */
#define MUSTER_OK 0
#define MUSTER_PENDING 1
#define MUSTER_ERR -1      /* bad arguments, no free slot, buffer too small */
#define MUSTER_ENOHOST -2  /* no live board holds this task             */
#define MUSTER_ETIMEOUT -3 /* retries exhausted, or the lead went away  */
#define MUSTER_ETASK -4    /* task ran but reported failure             */

/* ACK codes (the `ok` byte of an ACK) */
#define MUSTER_ACK_REFUSED 0 /* protocol version mismatch                 */
#define MUSTER_ACK_OK 1
#define MUSTER_ACK_REJOIN 2 /* lead does not know you: send JOIN again   */
#define MUSTER_ACK_FULL 3   /* lead's node table is full                 */

    /* telemetry events (a = task or node, b = node or detail) */
    enum
    {
        MUSTER_EV_JOINED = 1, /* a = node                          (lead)   */
        MUSTER_EV_LOST,       /* a = node                          (lead)   */
        MUSTER_EV_PLACED,     /* a = task, b = node                (lead)   */
        MUSTER_EV_DONE,       /* a = task, b = node that ran it    (lead)   */
        MUSTER_EV_RETRY,      /* a = task, b = node that failed    (lead)   */
        MUSTER_EV_ACCEPTED,   /* a = lead node                     (worker) */
        MUSTER_EV_VERSION,    /* a = peer, b = its protocol version         */
        MUSTER_EV_REFUSED     /* a = peer, b = ACK code (table full)        */
    };

    /* ------------------------------------------------------------ interfaces */
    /* A task: decode args, compute, encode result. Returns bytes written to
     * `ret`, or -1 on failure. The toolchain generates these trampolines around
     * your ordinary typed functions; you rarely write one by hand.
     * A task must not call muster_call() or muster_poll(): it already runs
     * inside muster_poll(). muster_call_async() is allowed.                    */
    typedef int16_t (*muster_task_fn)(const uint8_t *args, uint16_t len,
                                      uint8_t *ret, uint16_t cap);

    /* A link: one transport (CAN, SPI, loopback). Sends one frame of <= mtu
     * bytes; recv returns one frame's length or 0 if nothing is waiting.       */
    typedef struct muster_link
    {
        uint16_t mtu;
        uint16_t kbps;
        int (*send)(struct muster_link *l, uint8_t dst,
                    const uint8_t *frame, uint16_t len);
        uint16_t (*recv)(struct muster_link *l, uint8_t *src,
                         uint8_t *frame, uint16_t cap);
        void *ctx;
    } muster_link;

    typedef void (*muster_event_fn)(uint8_t ev, uint8_t a, uint8_t b, void *user);

    /* ------------------------------------------------------------- state */
    /* How to reach one peer: when it was last heard on each link, which links
     * are live, and which one unicast uses (the fastest live one).            */
    typedef struct
    {
        uint32_t seen[MUSTER_MAX_LINKS];
        uint8_t links_seen; /* bit l: heard on link l recently     */
        uint8_t route;      /* link index for unicast              */
    } muster_route;

    typedef struct
    {
        uint8_t used, id, cores, flags, alive, inflight;
        uint16_t ram_kb, clock_mhz;
        uint32_t resident;
        uint32_t queued; /* cost units dispatched, not yet done */
        muster_route rt;
    } muster_node;

    typedef struct
    {
        uint8_t used, call_id, origin, origin_call, task, node, retries, tried;
        int8_t status;
        uint16_t len, args_crc;
        uint32_t sent_at;
        uint8_t buf[MUSTER_MAX_MSG]; /* args while in flight, then result */
    } muster_pending;

    typedef struct
    {
        uint8_t used, lk, src, next;
        uint16_t len;
        uint32_t started;
        uint8_t buf[MUSTER_MAX_MSG];
    } muster_rx;

    /* A call this worker already ran, kept so a resend gets the same answer. */
    typedef struct
    {
        uint8_t used, src, call_id, task;
        uint16_t args_crc;
        int8_t status;
        uint16_t len;
        uint8_t buf[MUSTER_MAX_RET];
    } muster_done;

    typedef struct
    {
        uint8_t id, is_lead, cores, flags, lead, joined, next_call;
        uint8_t nlinks, busy, done_next;
        uint16_t ram_kb, clock_mhz;
        uint32_t resident, last_beat;
        uint32_t (*millis)(void);
        muster_link *links[MUSTER_MAX_LINKS];
        muster_route lead_rt; /* worker: route to lead  */
        muster_task_fn tasks[MUSTER_MAX_TASKS];
        uint16_t cost[MUSTER_MAX_TASKS];
        muster_node nodes[MUSTER_MAX_NODES]; /* used by the lead only  */
        muster_pending pend[MUSTER_MAX_PENDING];
        muster_rx rx[MUSTER_RX_SLOTS];
        muster_done done[MUSTER_DEDUPE]; /* duplicate suppression  */
        muster_event_fn on_event;
        void *event_user;
    } muster_t;

    /* ----------------------------------------------------------------- API */
    /* `link` becomes link 0. It is refused (and the board stays silent) if its
     * MTU is <= 2 or > MUSTER_MAX_FRAME.                                       */
    void muster_begin(muster_t *m, uint8_t node_id, int is_lead,
                      uint8_t cores, uint16_t ram_kb, uint16_t clock_mhz,
                      uint8_t flags, muster_link *link, uint32_t (*millis)(void));
    /* Attach another link. Returns its index, or -1 (table full, bad MTU). */
    int muster_add_link(muster_t *m, muster_link *link);
    void muster_register_task(muster_t *m, uint8_t id, muster_task_fn fn, uint16_t cost);
    /* Tell the lead what a task costs when the lead does not hold it itself,
     * so placement can account for work queued on workers. Unknown costs count
     * as 1. The toolchain emits one call per task on the lead.                 */
    void muster_set_cost(muster_t *m, uint8_t id, uint16_t cost);
    void muster_on_event(muster_t *m, muster_event_fn fn, void *user);
    void muster_poll(muster_t *m);

    /* Submit a call; returns a handle >= 0, or a negative MUSTER_E* code.
     * args may be at most MUSTER_MAX_ARGS bytes.                               */
    int8_t muster_call_async(muster_t *m, uint8_t task, const uint8_t *args, uint16_t len);
    /* MUSTER_PENDING until done; then MUSTER_OK (result copied) or an error.
     * Frees the handle once it returns anything other than MUSTER_PENDING.
     * If the result is larger than `cap`, nothing is copied, *out_len is set
     * to the size needed and MUSTER_ERR is returned.                           */
    int8_t muster_result(muster_t *m, int8_t handle, uint8_t *ret, uint16_t cap,
                         uint16_t *out_len);
    /* Abandon a call and free its handle, finished or not. */
    int8_t muster_cancel(muster_t *m, int8_t handle);
    /* Blocking convenience: submits, polls until done or timeout_ms.
     * Returns MUSTER_ERR if called from inside a task.                        */
    int8_t muster_call(muster_t *m, uint8_t task, const uint8_t *args, uint16_t len,
                       uint8_t *ret, uint16_t cap, uint16_t *out_len, uint32_t timeout_ms);

    /* introspection, for dashboards and tests (lead only) */
    uint8_t muster_alive_count(const muster_t *m);
    const muster_node *muster_node_by_id(const muster_t *m, uint8_t id);

    /* ---------------------------------------------- wire encoding helpers */
    typedef struct
    {
        uint8_t *p;
        uint16_t n, cap;
        uint8_t err;
    } muster_wbuf;
    typedef struct
    {
        const uint8_t *p;
        uint16_t n, len;
        uint8_t err;
    } muster_rbuf;

    static inline void muster_w_init(muster_wbuf *w, uint8_t *p, uint16_t cap)
    {
        w->p = p;
        w->n = 0;
        w->cap = cap;
        w->err = 0;
    }
    static inline void muster_r_init(muster_rbuf *r, const uint8_t *p, uint16_t len)
    {
        r->p = p;
        r->n = 0;
        r->len = len;
        r->err = 0;
    }

    static inline void muster_put_u8(muster_wbuf *w, uint8_t v)
    {
        if (w->n + 1u > w->cap)
        {
            w->err = 1;
            return;
        }
        w->p[w->n++] = v;
    }
    static inline void muster_put_u16(muster_wbuf *w, uint16_t v)
    {
        muster_put_u8(w, (uint8_t)v);
        muster_put_u8(w, (uint8_t)(v >> 8));
    }
    static inline void muster_put_u32(muster_wbuf *w, uint32_t v)
    {
        muster_put_u16(w, (uint16_t)v);
        muster_put_u16(w, (uint16_t)(v >> 16));
    }
    static inline void muster_put_i8(muster_wbuf *w, int8_t v) { muster_put_u8(w, (uint8_t)v); }
    static inline void muster_put_i16(muster_wbuf *w, int16_t v) { muster_put_u16(w, (uint16_t)v); }
    static inline void muster_put_i32(muster_wbuf *w, int32_t v) { muster_put_u32(w, (uint32_t)v); }
    static inline void muster_put_f32(muster_wbuf *w, float v)
    {
        uint32_t u;
        memcpy(&u, &v, 4);
        muster_put_u32(w, u);
    } /* IEEE-754 on both */

    static inline uint8_t muster_get_u8(muster_rbuf *r)
    {
        if (r->n + 1u > r->len)
        {
            r->err = 1;
            return 0;
        }
        return r->p[r->n++];
    }
    static inline uint16_t muster_get_u16(muster_rbuf *r)
    {
        uint16_t lo = muster_get_u8(r);
        return (uint16_t)(lo | ((uint16_t)muster_get_u8(r) << 8));
    }
    static inline uint32_t muster_get_u32(muster_rbuf *r)
    {
        uint32_t lo = muster_get_u16(r);
        return lo | ((uint32_t)muster_get_u16(r) << 16);
    }
    static inline int8_t muster_get_i8(muster_rbuf *r) { return (int8_t)muster_get_u8(r); }
    static inline int16_t muster_get_i16(muster_rbuf *r) { return (int16_t)muster_get_u16(r); }
    static inline int32_t muster_get_i32(muster_rbuf *r) { return (int32_t)muster_get_u32(r); }
    static inline float muster_get_f32(muster_rbuf *r)
    {
        uint32_t u = muster_get_u32(r);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }

    /* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). Check value: "123456789"
     * -> 0x29B1. Used for duplicate detection and by the SPI link.            */
    static inline uint16_t muster_crc16(const uint8_t *p, uint16_t n)
    {
        uint16_t crc = 0xFFFFu, i;
        uint8_t b;
        for (i = 0; i < n; i++)
        {
            crc ^= (uint16_t)((uint16_t)p[i] << 8);
            for (b = 0; b < 8; b++)
                crc = (crc & 0x8000u) ? (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u)
                                      : (uint16_t)(crc << 1);
        }
        return crc;
    }

#ifdef __cplusplus
}
#endif
#endif /* MUSTER_H */

/* ===================================================================== */
#ifdef MUSTER_IMPLEMENTATION
#ifndef MUSTER_IMPLEMENTED
#define MUSTER_IMPLEMENTED

/* message types */
enum
{
    M_JOIN = 1,
    M_ACK,
    M_BEAT,
    M_CALL,
    M_RESULT
};
#define M_HDR 3u /* type, src, dst                               */
#define F_HDR 2u /* per-frame: dst, index | last<<7              */
#define F_LAST 0x80u

#if MUSTER_MAX_TASKS >= 32
#define M_TASK_MASK 0xFFFFFFFFul
#else
#define M_TASK_MASK ((1ul << MUSTER_MAX_TASKS) - 1ul)
#endif

static void m_event(muster_t *m, uint8_t ev, uint8_t a, uint8_t b)
{
    if (m->on_event)
        m->on_event(ev, a, b, m->event_user);
}

/* ---------------------------------------------------- framing / sending */
/* Send header `h` followed by payload `p` as one message on link `lk`,
 * fragmented to that link's MTU. Header and payload are never copied into
 * one buffer, which keeps a message-sized array off the stack.             */
static int m_send_on(muster_t *m, uint8_t lk, uint8_t dst,
                     const uint8_t *h, uint16_t hl, const uint8_t *p, uint16_t pl)
{
    muster_link *l = m->links[lk];
    uint8_t frame[MUSTER_MAX_FRAME];
    uint16_t chunk = (uint16_t)(l->mtu - F_HDR), len = (uint16_t)(hl + pl), off = 0, k;
    uint8_t idx = 0;
    if (len > (uint16_t)(chunk * 128u))
        return -1; /* 7-bit fragment index */
    do
    {
        uint16_t n = (uint16_t)((len - off) > chunk ? chunk : (len - off));
        frame[0] = dst;
        frame[1] = (uint8_t)(idx & 0x7Fu) | (uint8_t)((off + n >= len) ? F_LAST : 0u);
        for (k = 0; k < n; k++, off++)
            frame[F_HDR + k] = off < hl ? h[off] : p[off - hl];
        if (l->send(l, dst, frame, (uint16_t)(n + F_HDR)) != 0)
            return -1;
        idx++;
    } while (off < len);
    return 0;
}

/* Send on every link. Returns 0 if at least one link took it. */
static int m_send_all(muster_t *m, uint8_t dst, const uint8_t *h, uint16_t hl,
                      const uint8_t *p, uint16_t pl)
{
    uint8_t lk;
    int rc = -1;
    for (lk = 0; lk < m->nlinks; lk++)
        if (m_send_on(m, lk, dst, h, hl, p, pl) == 0)
            rc = 0;
    return rc;
}

static muster_node *m_node(muster_t *m, uint8_t id);

/* Unicast over the peer's route; broadcast, or a peer with no known route,
 * goes out on every link.                                                  */
static int m_send_msg(muster_t *m, uint8_t dst, const uint8_t *h, uint16_t hl,
                      const uint8_t *p, uint16_t pl)
{
    const muster_route *rt = 0;
    if (dst != MUSTER_BROADCAST)
    {
        if (m->is_lead)
        {
            const muster_node *n = m_node(m, dst);
            if (n && n->rt.links_seen)
                rt = &n->rt;
        }
        else if (dst == m->lead && m->lead_rt.links_seen)
        {
            rt = &m->lead_rt;
        }
    }
    if (rt)
        return m_send_on(m, rt->route, dst, h, hl, p, pl);
    return m_send_all(m, dst, h, hl, p, pl);
}

static void m_send_join(muster_t *m)
{
    uint8_t b[M_HDR + 11];
    muster_wbuf w;
    muster_w_init(&w, b, sizeof b);
    muster_put_u8(&w, M_JOIN);
    muster_put_u8(&w, m->id);
    muster_put_u8(&w, MUSTER_BROADCAST);
    muster_put_u8(&w, MUSTER_PROTO_VERSION);
    muster_put_u8(&w, m->cores);
    muster_put_u8(&w, m->flags);
    muster_put_u16(&w, m->ram_kb);
    muster_put_u16(&w, m->clock_mhz);
    muster_put_u32(&w, m->resident);
    m_send_all(m, MUSTER_BROADCAST, b, w.n, 0, 0);
}

/* Heartbeats go out on every link so the peer learns each way to reach us. */
static void m_send_beat(muster_t *m, uint8_t dst)
{
    uint8_t b[M_HDR];
    b[0] = M_BEAT;
    b[1] = m->id;
    b[2] = dst;
    m_send_all(m, dst, b, sizeof b, 0, 0);
}

static void m_send_ack(muster_t *m, uint8_t dst, uint8_t code)
{
    uint8_t b[M_HDR + 2];
    b[0] = M_ACK;
    b[1] = m->id;
    b[2] = dst;
    b[3] = MUSTER_PROTO_VERSION;
    b[4] = code;
    m_send_msg(m, dst, b, sizeof b, 0, 0);
}

/* `lk` is the link to answer on (the one the CALL came in on, which is known
 * to work right now), or M_ROUTED to use the peer's route.                 */
#define M_ROUTED 0xFFu
static void m_send_result(muster_t *m, uint8_t lk, uint8_t dst, uint8_t call_id,
                          int8_t status, const uint8_t *ret, uint16_t len)
{
    uint8_t b[M_HDR + 2];
    if (len > MUSTER_MAX_RET)
    {
        len = 0;
        status = MUSTER_ERR;
    }
    b[0] = M_RESULT;
    b[1] = m->id;
    b[2] = dst;
    b[3] = call_id;
    b[4] = (uint8_t)status;
    if (lk < m->nlinks)
        m_send_on(m, lk, dst, b, sizeof b, ret, len);
    else
        m_send_msg(m, dst, b, sizeof b, ret, len);
}

static int m_send_call(muster_t *m, uint8_t dst, uint8_t origin, uint8_t call_id,
                       uint8_t task, const uint8_t *args, uint16_t len)
{
    uint8_t b[M_HDR + 3];
    if (len > MUSTER_MAX_ARGS)
        return -1;
    b[0] = M_CALL;
    b[1] = m->id;
    b[2] = dst;
    b[3] = origin;
    b[4] = call_id;
    b[5] = task;
    return m_send_msg(m, dst, b, sizeof b, args, len);
}

/* ---------------------------------------------------- local execution */
/* `busy` marks that we are inside muster_poll() or a task, so a task that
 * calls muster_call() is refused instead of re-entering the core.          */
static int8_t m_run_local(muster_t *m, uint8_t task, const uint8_t *args, uint16_t len,
                          uint8_t *ret, uint16_t cap, uint16_t *out_len)
{
    int16_t n;
    uint8_t was_busy = m->busy;
    *out_len = 0;
    if (task >= MUSTER_MAX_TASKS || !m->tasks[task])
        return MUSTER_ENOHOST;
    m->busy = 1;
    n = m->tasks[task](args, len, ret, cap);
    m->busy = was_busy;
    if (n < 0 || (uint16_t)n > cap)
        return MUSTER_ETASK;
    *out_len = (uint16_t)n;
    return MUSTER_OK;
}

/* ---------------------------------------------------- duplicate cache */
/* A call is the same call if sender, call ID, task and arguments all match;
 * the argument CRC guards against call IDs reused after a restart.        */
static muster_done *m_done_find(muster_t *m, uint8_t src, uint8_t call_id,
                                uint8_t task, uint16_t crc)
{
    uint8_t i;
    for (i = 0; i < MUSTER_DEDUPE; i++)
    {
        muster_done *d = &m->done[i];
        if (d->used && d->src == src && d->call_id == call_id &&
            d->task == task && d->args_crc == crc)
            return d;
    }
    return 0;
}

/* Claim the oldest entry for a new call (ring order). */
static muster_done *m_done_claim(muster_t *m, uint8_t src, uint8_t call_id,
                                 uint8_t task, uint16_t crc)
{
    muster_done *d = &m->done[m->done_next];
    m->done_next = (uint8_t)((m->done_next + 1u) % MUSTER_DEDUPE);
    d->used = 0;
    d->src = src;
    d->call_id = call_id;
    d->task = task;
    d->args_crc = crc;
    d->status = MUSTER_ERR;
    d->len = 0;
    return d;
}

/* Worker: run a CALL from the lead, unless we already ran this exact call
 * - then resend that answer instead of running the task twice. */
static void m_run_call(muster_t *m, uint8_t lk, uint8_t src, uint8_t call_id, uint8_t task,
                       const uint8_t *args, uint16_t alen)
{
    uint16_t crc = muster_crc16(args, alen);
    muster_done *d = m_done_find(m, src, call_id, task, crc);
    if (!d)
    {
        d = m_done_claim(m, src, call_id, task, crc);
        d->status = m_run_local(m, task, args, alen, d->buf, sizeof d->buf, &d->len);
        d->used = 1;
    }
    m_send_result(m, lk, src, call_id, d->status, d->buf, d->len);
}

/* ---------------------------------------------------- routes */
static void m_route_pick(muster_t *m, muster_route *rt)
{
    uint8_t l, best = MUSTER_NO_NODE;
    for (l = 0; l < m->nlinks; l++)
        if ((rt->links_seen & (1u << l)) &&
            (best == MUSTER_NO_NODE || m->links[l]->kbps > m->links[best]->kbps))
            best = l;
    rt->route = best == MUSTER_NO_NODE ? 0 : best;
}

static void m_route_heard(muster_t *m, muster_route *rt, uint8_t lk, uint32_t now)
{
    rt->seen[lk] = now;
    if (!(rt->links_seen & (1u << lk)))
    {
        rt->links_seen = (uint8_t)(rt->links_seen | (1u << lk));
        m_route_pick(m, rt);
    }
}

/* Drop links the peer has been silent on for MUSTER_NODE_TIMEOUT_MS.
 * Returns the links still live (0 = the peer is gone).                    */
static uint8_t m_route_expire(muster_t *m, muster_route *rt, uint32_t now)
{
    uint8_t l, before = rt->links_seen;
    for (l = 0; l < m->nlinks; l++)
        if ((rt->links_seen & (1u << l)) && now - rt->seen[l] > MUSTER_NODE_TIMEOUT_MS)
            rt->links_seen = (uint8_t)(rt->links_seen & ~(1u << l));
    if (rt->links_seen != before)
        m_route_pick(m, rt);
    return rt->links_seen;
}

/* Has the peer said anything on any link since time t? */
static int m_heard_since(const muster_t *m, const muster_route *rt, uint32_t t, uint32_t now)
{
    uint8_t l;
    for (l = 0; l < m->nlinks; l++)
        if ((rt->links_seen & (1u << l)) && now - rt->seen[l] < now - t)
            return 1;
    return 0;
}

/* A call to a peer timed out although the peer kept talking. If it has been
 * silent on the route link since time t, that link is broken (e.g. the SPI
 * cable came out): stop using it now instead of after MUSTER_NODE_TIMEOUT_MS.
 * Heartbeats on that link will bring it back.                              */
static void m_route_demote(muster_t *m, muster_route *rt, uint32_t t, uint32_t now)
{
    uint8_t r = rt->route;
    if (now - rt->seen[r] >= now - t)
    {
        rt->links_seen = (uint8_t)(rt->links_seen & ~(1u << r));
        m_route_pick(m, rt);
    }
}

/* ---------------------------------------------------- lead: node table */
static uint8_t m_node_row(const muster_t *m, uint8_t id)
{
    uint8_t i;
    for (i = 0; i < MUSTER_MAX_NODES; i++)
        if (m->nodes[i].used && m->nodes[i].id == id)
            return i;
    return MUSTER_NO_NODE;
}

static muster_node *m_node(muster_t *m, uint8_t id)
{
    uint8_t i = m_node_row(m, id);
    return i == MUSTER_NO_NODE ? 0 : &m->nodes[i];
}

/* Find or add a node. A full table recycles the row of a lost node. */
static muster_node *m_node_add(muster_t *m, uint8_t id)
{
    uint8_t i;
    muster_node *n = m_node(m, id), *spare = 0;
    if (n)
        return n;
    for (i = 0; i < MUSTER_MAX_NODES && !spare; i++)
        if (!m->nodes[i].used)
            spare = &m->nodes[i];
    for (i = 0; i < MUSTER_MAX_NODES && !spare; i++)
        if (!m->nodes[i].alive && m->nodes[i].id != m->id)
            spare = &m->nodes[i];
    if (!spare)
        return 0;
    memset(spare, 0, sizeof *spare);
    spare->used = 1;
    spare->id = id;
    return spare;
}

/* Placement. Lower score = earlier expected completion. Units are
 * arbitrary but consistent:
 *   wait+run = (work already queued there + this task's cost) / clock
 *   transfer = bytes / speed of the link we would use to reach it
 *              (zero when running on the lead itself)
 *   lead     = MUSTER_LEAD_PENALTY, because the lead has its own job
 * `exclude` is a bitmask of node indices already tried for this call.     */
static uint32_t m_cost(const muster_t *m, uint8_t task)
{
    return m->cost[task] ? m->cost[task] : 1u;
}

static uint8_t m_place(muster_t *m, uint8_t task, uint16_t bytes, uint8_t exclude)
{
    uint32_t best = 0xFFFFFFFFu, score;
    uint8_t i, pick = MUSTER_NO_NODE;
    for (i = 0; i < MUSTER_MAX_NODES; i++)
    {
        muster_node *n = &m->nodes[i];
        if (!n->used || !n->alive || !(n->resident & (1ul << task)))
            continue;
        if (exclude & (1u << i))
            continue;
        score = ((n->queued + m_cost(m, task)) * 1000u) / (n->clock_mhz ? n->clock_mhz : 1u);
        if (n->id != m->id)
        {
            uint16_t kbps = m->links[n->rt.route]->kbps;
            score += ((uint32_t)bytes * 8000u) / (kbps ? kbps : 1u);
        }
        else
        {
            score += MUSTER_LEAD_PENALTY;
        }
        if (score < best)
        {
            best = score;
            pick = i;
        }
    }
    return pick;
}

/* Best board not yet tried; failing that, any live holder again (a worker
 * answers a resent call from its duplicate cache, so this is safe).       */
static uint8_t m_choose(muster_t *m, const muster_pending *p)
{
    uint8_t idx = m_place(m, p->task, p->len, p->tried);
    if (idx == MUSTER_NO_NODE && p->tried)
        idx = m_place(m, p->task, p->len, 0);
    return idx;
}

static void m_release(muster_t *m, muster_pending *p)
{
    muster_node *n;
    if (p->node == MUSTER_NO_NODE)
        return;
    n = &m->nodes[p->node];
    if (n->inflight)
        n->inflight--;
    n->queued = n->queued > m_cost(m, p->task) ? n->queued - m_cost(m, p->task) : 0;
    p->node = MUSTER_NO_NODE;
}

static muster_pending *m_pend_alloc(muster_t *m)
{
    uint8_t i;
    for (i = 0; i < MUSTER_MAX_PENDING; i++)
        if (!m->pend[i].used)
        {
            memset(&m->pend[i], 0, sizeof m->pend[i]);
            m->pend[i].used = 1;
            m->pend[i].call_id = m->next_call++;
            m->pend[i].node = MUSTER_NO_NODE;
            m->pend[i].status = MUSTER_PENDING;
            return &m->pend[i];
        }
    return 0;
}

static void m_finish(muster_t *m, muster_pending *p, int8_t status,
                     const uint8_t *ret, uint16_t len)
{
    m_release(m, p);
    if (p->origin != m->id)
    { /* relay to the asker  */
        /* keep the answer: if this RESULT is lost the worker resends */
        muster_done *d = m_done_claim(m, p->origin, p->origin_call, p->task, p->args_crc);
        if (len > MUSTER_MAX_RET)
        {
            len = 0;
            status = MUSTER_ERR;
        }
        d->status = status;
        d->len = len;
        if (len)
            memcpy(d->buf, ret, len);
        d->used = 1;
        m_send_result(m, M_ROUTED, p->origin, p->origin_call, status, ret, len);
        p->used = 0;
        return;
    }
    if (len > MUSTER_MAX_MSG)
        len = MUSTER_MAX_MSG;
    if (len)
        memcpy(p->buf, ret, len);
    p->len = len;
    p->status = status;
}

/* Send a pending call to node row `idx` (or run it, if that is the lead). */
static void m_dispatch(muster_t *m, muster_pending *p, uint8_t idx)
{
    uint8_t out[MUSTER_MAX_RET];
    uint16_t out_len = 0;
    int8_t st;
    if (idx == MUSTER_NO_NODE)
    {
        m_finish(m, p, p->retries ? MUSTER_ETIMEOUT : MUSTER_ENOHOST, 0, 0);
        return;
    }
    p->tried |= (uint8_t)(1u << idx);
    p->node = idx;
    p->sent_at = m->millis();
    m->nodes[idx].inflight++;
    m->nodes[idx].queued += m_cost(m, p->task);
    m_event(m, MUSTER_EV_PLACED, p->task, m->nodes[idx].id);
    if (m->nodes[idx].id == m->id)
    { /* the lead is best     */
        st = m_run_local(m, p->task, p->buf, p->len, out, sizeof out, &out_len);
        m_event(m, MUSTER_EV_DONE, p->task, m->id);
        m_finish(m, p, st, out, out_len);
    }
    else
    { /* lost sends: timeout  */
        m_send_call(m, m->nodes[idx].id, m->id, p->call_id, p->task, p->buf, p->len);
    }
}

/* ---------------------------------------------------- message handling */
static void m_on_join(muster_t *m, muster_rbuf *r, uint8_t lk, uint8_t src)
{
    uint8_t ver = muster_get_u8(r), cores, flags;
    uint16_t ram_kb, clock_mhz;
    uint32_t resident;
    muster_node *n;
    if (!m->is_lead)
        return;
    if (ver != MUSTER_PROTO_VERSION)
    { /* refuse loudly        */
        m_event(m, MUSTER_EV_VERSION, src, ver);
        m_send_ack(m, src, MUSTER_ACK_REFUSED);
        return;
    }
    cores = muster_get_u8(r);
    flags = muster_get_u8(r);
    ram_kb = muster_get_u16(r);
    clock_mhz = muster_get_u16(r);
    resident = muster_get_u32(r);
    if (r->err)
        return; /* truncated JOIN       */
    n = m_node_add(m, src);
    if (!n)
    {
        m_event(m, MUSTER_EV_REFUSED, src, MUSTER_ACK_FULL);
        m_send_ack(m, src, MUSTER_ACK_FULL);
        return;
    }
    n->cores = cores;
    n->flags = flags;
    n->ram_kb = ram_kb;
    n->clock_mhz = clock_mhz;
    n->resident = resident & M_TASK_MASK; /* ignore IDs we can't index */
    m_route_heard(m, &n->rt, lk, m->millis());
    if (!n->alive)
    {
        n->alive = 1;
        m_event(m, MUSTER_EV_JOINED, src, 0);
    }
    m_send_ack(m, src, MUSTER_ACK_OK);
}

static void m_on_ack(muster_t *m, muster_rbuf *r, uint8_t lk, uint8_t src)
{
    uint8_t ver = muster_get_u8(r), code = muster_get_u8(r);
    if (m->is_lead || r->err)
        return;
    if (code == MUSTER_ACK_OK)
    {
        if (m->joined)
            return; /* duplicate ACK        */
        m->lead = src;
        m->joined = 1;
        memset(&m->lead_rt, 0, sizeof m->lead_rt);
        m_route_heard(m, &m->lead_rt, lk, m->millis());
        memset(m->done, 0, sizeof m->done); /* new lead, new call IDs */
        m_event(m, MUSTER_EV_ACCEPTED, src, 0);
    }
    else if (code == MUSTER_ACK_REJOIN)
    {
        if (!m->joined || src != m->lead)
            return;
        m->joined = 0; /* JOIN on the next poll */
        m->last_beat = m->millis() - MUSTER_HEARTBEAT_MS;
    }
    else if (code == MUSTER_ACK_REFUSED)
    {
        m_event(m, MUSTER_EV_VERSION, src, ver);
    }
    else
    {
        m_event(m, MUSTER_EV_REFUSED, src, code);
    }
}

static void m_on_call(muster_t *m, muster_rbuf *r, const uint8_t *b, uint16_t len,
                      uint8_t lk, uint8_t src)
{
    uint8_t origin = muster_get_u8(r), call_id = muster_get_u8(r);
    uint8_t task = muster_get_u8(r);
    const uint8_t *args = b + r->n;
    uint16_t alen = (uint16_t)(len - r->n), crc;
    muster_pending *p;
    muster_done *d;
    uint8_t i;
    (void)origin; /* hub and spoke: == src */
    if (r->err)
        return;
    if (!m->is_lead)
    { /* lead told us: run    */
        if (m->joined && src == m->lead)
            m_run_call(m, lk, src, call_id, task, args, alen);
        return;
    }
    if (task >= MUSTER_MAX_TASKS)
    { /* worker asked: place  */
        m_send_result(m, lk, src, call_id, MUSTER_ENOHOST, 0, 0);
        return;
    }
    for (i = 0; i < MUSTER_MAX_PENDING; i++) /* a resend: in progress */
        if (m->pend[i].used && m->pend[i].origin == src &&
            m->pend[i].origin_call == call_id && m->pend[i].task == task)
            return;
    crc = muster_crc16(args, alen);
    d = m_done_find(m, src, call_id, task, crc); /* a resend: answered    */
    if (d)
    {
        m_send_result(m, lk, src, call_id, d->status, d->buf, d->len);
        return;
    }
    p = m_pend_alloc(m);
    if (!p)
    {
        m_send_result(m, lk, src, call_id, MUSTER_ERR, 0, 0);
        return;
    }
    p->origin = src;
    p->origin_call = call_id;
    p->task = task;
    p->args_crc = crc;
    if (alen)
        memcpy(p->buf, args, alen);
    p->len = alen;
    m_dispatch(m, p, m_choose(m, p));
}

static void m_on_result(muster_t *m, muster_rbuf *r, const uint8_t *b, uint16_t len, uint8_t src)
{
    uint8_t call_id = muster_get_u8(r), i, row;
    int8_t st = (int8_t)muster_get_u8(r);
    const uint8_t *ret = b + r->n;
    uint16_t rl = (uint16_t)(len - r->n);
    if (r->err)
        return;
    if (st > 0)
        st = MUSTER_ERR; /* only OK or an error  */
    row = m->is_lead ? m_node_row(m, src) : MUSTER_NO_NODE;
    for (i = 0; i < MUSTER_MAX_PENDING; i++)
    {
        muster_pending *p = &m->pend[i];
        if (!p->used || p->call_id != call_id || p->status != MUSTER_PENDING)
            continue;
        if (m->is_lead)
        {
            /* any board this call was sent to may answer; the first wins */
            if (row == MUSTER_NO_NODE || !(p->tried & (1u << row)))
                continue;
            m_event(m, MUSTER_EV_DONE, p->task, src);
            m_finish(m, p, st, ret, rl);
        }
        else
        {
            if (src != m->lead)
                return;
            if (rl > MUSTER_MAX_MSG)
                rl = MUSTER_MAX_MSG;
            if (rl)
                memcpy(p->buf, ret, rl);
            p->len = rl;
            p->status = st;
        }
        return;
    }
}

static void m_handle(muster_t *m, uint8_t lk, const uint8_t *b, uint16_t len)
{
    muster_rbuf r;
    uint8_t type, src, dst;
    muster_r_init(&r, b, len);
    type = muster_get_u8(&r);
    src = muster_get_u8(&r);
    dst = muster_get_u8(&r);
    if (r.err || src == m->id)
        return;
    if (dst != m->id && dst != MUSTER_BROADCAST)
        return;

    if (m->is_lead)
    {
        /* A node we don't know (we restarted) or wrote off (it went quiet
         * but is back) is told to rejoin, so its capabilities are fresh.  */
        muster_node *n = m_node(m, src);
        if (n && n->alive)
            m_route_heard(m, &n->rt, lk, m->millis());
        else if (type != M_JOIN)
        {
            m_send_ack(m, src, MUSTER_ACK_REJOIN);
            return;
        }
    }
    else if (m->joined && src == m->lead)
    {
        m_route_heard(m, &m->lead_rt, lk, m->millis());
    }

    switch (type)
    {
    case M_JOIN:
        m_on_join(m, &r, lk, src);
        return;
    case M_ACK:
        m_on_ack(m, &r, lk, src);
        return;
    case M_BEAT:
        return; /* route updated above  */
    case M_CALL:
        m_on_call(m, &r, b, len, lk, src);
        return;
    case M_RESULT:
        m_on_result(m, &r, b, len, src);
        return;
    default:
        return;
    }
}

/* Reassemble frames into messages. Frames for other boards are dropped
 * before they cost any RAM, which is what lets a Nano share a busy bus.
 * Slots are keyed by (link, source): one source may be mid-message on two
 * links at once.                                                          */
static void m_rx_frame(muster_t *m, uint8_t lk, uint8_t src, const uint8_t *f, uint16_t len)
{
    uint8_t i, dst, idx, last;
    muster_rx *s = 0, *spare = 0;
    if (len < F_HDR)
        return;
    dst = f[0];
    idx = (uint8_t)(f[1] & 0x7Fu);
    last = (uint8_t)(f[1] & F_LAST);
    if (dst != m->id && dst != MUSTER_BROADCAST)
        return;
    f += F_HDR;
    len = (uint16_t)(len - F_HDR);

    if (idx == 0 && last)
    {
        m_handle(m, lk, f, len);
        return;
    } /* fast path */

    for (i = 0; i < MUSTER_RX_SLOTS; i++)
    {
        muster_rx *x = &m->rx[i];
        if (x->used && x->src == src && x->lk == lk)
        {
            s = x;
            break;
        }
        if (!x->used && !spare)
            spare = x;
    }
    if (idx == 0)
    { /* new message from this source */
        if (!s)
            s = spare;
        if (!s)
            return; /* no slot: drop, sender retries */
        s->used = 1;
        s->lk = lk;
        s->src = src;
        s->len = 0;
        s->next = 0;
        s->started = m->millis();
    }
    if (!s || idx != s->next || (uint16_t)(s->len + len) > MUSTER_MAX_MSG)
    {
        if (s)
            s->used = 0; /* gap or overflow: discard     */
        return;
    }
    memcpy(s->buf + s->len, f, len);
    s->len = (uint16_t)(s->len + len);
    s->next++;
    if (last)
    {
        s->used = 0;
        m_handle(m, lk, s->buf, s->len);
    }
}

/* ----------------------------------------------------------- polling */
static void m_poll_worker(muster_t *m, uint32_t now)
{
    uint8_t i;
    if (m->joined && !m_route_expire(m, &m->lead_rt, now))
        m->joined = 0; /* lead went quiet    */
    /* resend unanswered calls; the lead drops or answers duplicates */
    for (i = 0; m->joined && i < MUSTER_MAX_PENDING; i++)
    {
        muster_pending *p = &m->pend[i];
        if (!p->used || p->status != MUSTER_PENDING || p->retries >= MUSTER_MAX_RETRIES)
            continue;
        if (now - p->sent_at > (uint32_t)(p->retries + 1u) * MUSTER_CALL_TIMEOUT_MS)
        {
            if (m_heard_since(m, &m->lead_rt, p->sent_at, now))
                m_route_demote(m, &m->lead_rt, p->sent_at, now);
            p->retries++;
            m_send_call(m, m->lead, m->id, p->call_id, p->task, p->buf, p->len);
        }
    }
    if (now - m->last_beat >= MUSTER_HEARTBEAT_MS)
    {
        m->last_beat = now;
        if (m->joined)
            m_send_beat(m, m->lead);
        else
            m_send_join(m); /* (re)join           */
    }
}

static void m_poll_lead(muster_t *m, uint32_t now)
{
    uint8_t i;
    /* heartbeat on every link so workers know the lead is alive, however
     * busy the lead is talking to other boards */
    if (now - m->last_beat >= MUSTER_HEARTBEAT_MS)
    {
        m->last_beat = now;
        m_send_beat(m, MUSTER_BROADCAST);
    }
    /* liveness: drop silent links; a node with no live link is lost */
    for (i = 0; i < MUSTER_MAX_NODES; i++)
    {
        muster_node *nd = &m->nodes[i];
        if (!nd->used || !nd->alive || nd->id == m->id)
            continue;
        if (!m_route_expire(m, &nd->rt, now))
        {
            nd->alive = 0;
            nd->inflight = 0;
            nd->queued = 0;
            m_event(m, MUSTER_EV_LOST, nd->id, 0);
        }
    }
    /* timeouts and failover */
    for (i = 0; i < MUSTER_MAX_PENDING; i++)
    {
        muster_pending *p = &m->pend[i];
        muster_node *nd;
        uint8_t idx, again;
        if (!p->used || p->status != MUSTER_PENDING || p->node == MUSTER_NO_NODE)
            continue;
        idx = p->node;
        nd = &m->nodes[idx];
        if (nd->alive && now - p->sent_at <= MUSTER_CALL_TIMEOUT_MS)
            continue;
        /* Still talking since we sent it? Then the CALL or RESULT was lost,
         * not the board: resend to it (its duplicate cache makes that safe).
         * Silent: assume it is dead or stuck and move the call elsewhere. */
        again = (uint8_t)(nd->alive && m_heard_since(m, &nd->rt, p->sent_at, now));
        if (again)
            m_route_demote(m, &nd->rt, p->sent_at, now);
        m_event(m, MUSTER_EV_RETRY, p->task, nd->id);
        m_release(m, p);
        if (++p->retries > MUSTER_MAX_RETRIES)
        {
            m_finish(m, p, MUSTER_ETIMEOUT, 0, 0);
            continue;
        }
        m_dispatch(m, p, again ? idx : m_choose(m, p));
    }
}

/* ----------------------------------------------------------- public */
void muster_begin(muster_t *m, uint8_t node_id, int is_lead,
                  uint8_t cores, uint16_t ram_kb, uint16_t clock_mhz,
                  uint8_t flags, muster_link *link, uint32_t (*millis)(void))
{
    memset(m, 0, sizeof *m);
    m->id = node_id;
    m->is_lead = (uint8_t)(is_lead ? 1 : 0);
    m->cores = cores;
    m->ram_kb = ram_kb;
    m->clock_mhz = clock_mhz;
    m->flags = flags;
    m->millis = millis;
    m->lead = MUSTER_NO_NODE;
    muster_add_link(m, link);
    if (m->is_lead)
    { /* the lead is a node too       */
        muster_node *self = m_node_add(m, node_id);
        self->alive = 1;
        self->cores = cores;
        self->flags = flags;
        self->ram_kb = ram_kb;
        self->clock_mhz = clock_mhz;
        m->lead = node_id;
        m->joined = 1;
    }
}

int muster_add_link(muster_t *m, muster_link *link)
{
    if (!link || m->nlinks >= MUSTER_MAX_LINKS)
        return -1;
    if (link->mtu <= F_HDR || link->mtu > MUSTER_MAX_FRAME)
        return -1;
    m->links[m->nlinks] = link;
    return m->nlinks++;
}

void muster_register_task(muster_t *m, uint8_t id, muster_task_fn fn, uint16_t cost)
{
    if (id >= MUSTER_MAX_TASKS)
        return;
    m->tasks[id] = fn;
    m->cost[id] = cost ? cost : 1u;
    m->resident |= (1ul << id);
    if (m->is_lead)
    {
        muster_node *s = m_node(m, m->id);
        if (s)
            s->resident = m->resident;
    }
}

void muster_set_cost(muster_t *m, uint8_t id, uint16_t cost)
{
    if (id < MUSTER_MAX_TASKS)
        m->cost[id] = cost;
}

void muster_on_event(muster_t *m, muster_event_fn fn, void *user)
{
    m->on_event = fn;
    m->event_user = user;
}

void muster_poll(muster_t *m)
{
    uint8_t frame[MUSTER_MAX_FRAME], src, lk, i;
    uint16_t n;
    uint32_t now;
    if (m->busy)
        return; /* called from a task */
    m->busy = 1;
    for (lk = 0; lk < m->nlinks; lk++)
    {
        muster_link *l = m->links[lk];
        while ((n = l->recv(l, &src, frame, sizeof frame)) > 0)
            m_rx_frame(m, lk, src, frame, n);
    }
    now = m->millis();
    for (i = 0; i < MUSTER_RX_SLOTS; i++) /* stale reassembly    */
        if (m->rx[i].used && now - m->rx[i].started > MUSTER_CALL_TIMEOUT_MS)
            m->rx[i].used = 0;
    if (m->is_lead)
        m_poll_lead(m, now);
    else
        m_poll_worker(m, now);
    m->busy = 0;
}

int8_t muster_call_async(muster_t *m, uint8_t task, const uint8_t *args, uint16_t len)
{
    muster_pending *p;
    if (task >= MUSTER_MAX_TASKS || len > MUSTER_MAX_ARGS || (len && !args))
        return MUSTER_ERR;
    if (!m->is_lead && !m->joined)
    {
        /* not on the bus yet: run it here if we can, otherwise refuse     */
        if (!m->tasks[task])
            return MUSTER_ENOHOST;
    }
    p = m_pend_alloc(m);
    if (!p)
        return MUSTER_ERR;
    p->origin = m->id;
    p->origin_call = p->call_id;
    p->task = task;
    if (len)
        memcpy(p->buf, args, len);
    p->len = len;
    if (m->is_lead)
    {
        m_dispatch(m, p, m_choose(m, p));
    }
    else if (m->joined)
    {
        p->sent_at = m->millis();
        if (m_send_call(m, m->lead, m->id, p->call_id, task, args, len) != 0)
        {
            p->used = 0;
            return MUSTER_ERR;
        }
    }
    else
    {
        uint8_t out[MUSTER_MAX_RET];
        uint16_t ol = 0;
        int8_t st = m_run_local(m, task, args, len, out, sizeof out, &ol);
        if (ol)
            memcpy(p->buf, out, ol);
        p->len = ol;
        p->status = st;
    }
    return (int8_t)(p - m->pend);
}

int8_t muster_result(muster_t *m, int8_t h, uint8_t *ret, uint16_t cap, uint16_t *out_len)
{
    muster_pending *p;
    int8_t st;
    if (h < 0 || h >= MUSTER_MAX_PENDING || !m->pend[h].used)
        return MUSTER_ERR;
    p = &m->pend[h];
    if (p->status == MUSTER_PENDING)
    {
        /* workers give up once the lead is gone or has had every chance   */
        if (!m->is_lead &&
            (!m->joined || m->millis() - p->sent_at > MUSTER_CALL_DEADLINE_MS))
        {
            p->used = 0;
            return MUSTER_ETIMEOUT;
        }
        return MUSTER_PENDING;
    }
    st = p->status;
    if (st == MUSTER_OK)
    {
        if (out_len)
            *out_len = p->len;
        if (p->len > cap)
            st = MUSTER_ERR; /* caller's buffer too small */
        else if (p->len)
            memcpy(ret, p->buf, p->len);
    }
    p->used = 0;
    return st;
}

int8_t muster_cancel(muster_t *m, int8_t h)
{
    if (h < 0 || h >= MUSTER_MAX_PENDING || !m->pend[h].used)
        return MUSTER_ERR;
    m_release(m, &m->pend[h]); /* give back its load   */
    m->pend[h].used = 0;
    return MUSTER_OK;
}

int8_t muster_call(muster_t *m, uint8_t task, const uint8_t *args, uint16_t len,
                   uint8_t *ret, uint16_t cap, uint16_t *out_len, uint32_t timeout_ms)
{
    uint32_t t0 = m->millis();
    int8_t h, st;
    if (m->busy)
        return MUSTER_ERR; /* inside a task        */
    h = muster_call_async(m, task, args, len);
    if (h < 0)
        return h;
    for (;;)
    {
        st = muster_result(m, h, ret, cap, out_len);
        if (st != MUSTER_PENDING)
            return st;
        if (m->millis() - t0 > timeout_ms)
        {
            muster_cancel(m, h);
            return MUSTER_ETIMEOUT;
        }
        muster_poll(m);
    }
}

uint8_t muster_alive_count(const muster_t *m)
{
    uint8_t i, c = 0;
    for (i = 0; i < MUSTER_MAX_NODES; i++)
        if (m->nodes[i].used && m->nodes[i].alive)
            c++;
    return c;
}

const muster_node *muster_node_by_id(const muster_t *m, uint8_t id)
{
    uint8_t i = m_node_row(m, id);
    return i == MUSTER_NO_NODE ? 0 : &m->nodes[i];
}

#endif /* MUSTER_IMPLEMENTED */
#endif /* MUSTER_IMPLEMENTATION */
