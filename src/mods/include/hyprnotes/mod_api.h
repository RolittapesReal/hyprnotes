/* Hyprnotes native mod API, version 1. Pure C. No Qt/C++/Rust types, no exceptions across this boundary.
 * All strings are UTF-8. Pointers passed to callbacks are valid only for the duration of the call unless stated.
 * Buffers returned by the host (get_selection_text) are owned by the caller until released with free_buffer.
 * All host functions and all callbacks run on the UI thread. Mods must not create threads/timers for polling;
 * use subscribe() and schedule_work(). Callbacks should finish in well under 4 ms. */
#ifndef HYPRNOTES_MOD_API_H
#define HYPRNOTES_MOD_API_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HN_MOD_API_VERSION 1u

typedef struct hn_host hn_host; /* opaque per-mod host handle */

typedef enum { HN_OK = 0, HN_ERR_INVALID = 1, HN_ERR_NO_DOCUMENT = 2, HN_ERR_STATE = 3, HN_ERR_FAILED = 4 } hn_status;
typedef enum { HN_LOG_DEBUG = 0, HN_LOG_INFO = 1, HN_LOG_WARN = 2, HN_LOG_ERROR = 3 } hn_log_level;
typedef enum {
    HN_EVENT_NOTE_OPENED = 1, HN_EVENT_NOTE_SAVED = 2, HN_EVENT_NOTE_CLOSED = 3, HN_EVENT_SELECTION_CHANGED = 4
} hn_event_type;

typedef struct hn_event {
    uint32_t struct_size;
    uint32_t type;          /* hn_event_type */
    const char *note_id;    /* valid during callback only */
} hn_event;

typedef void (*hn_command_fn)(hn_host *host, void *user);
typedef void (*hn_event_fn)(hn_host *host, const hn_event *ev, void *user);
typedef void (*hn_work_fn)(hn_host *host, void *user);

/* Table provided by the host. Always pass `handle` as first argument. */
typedef struct hn_host_api {
    uint32_t struct_size;   /* sizeof of the host's table; check with HN_HOST_HAS */
    uint32_t api_version;
    hn_host *handle;
    hn_status (*register_command)(hn_host *h, const char *id, const char *title, hn_command_fn cb, void *user);
    void (*log)(hn_host *h, int level, const char *msg);
    /* Returns NUL-terminated copy (caller frees with free_buffer), or NULL if no document. len may be NULL. */
    char *(*get_selection_text)(hn_host *h, size_t *len);
    /* Mutations are only valid while a command runs (a document is bound) and inside a transaction.
     * One begin/end pair becomes one undo step and triggers autosave. Unbalanced transactions are closed by the host. */
    hn_status (*begin_transaction)(hn_host *h, const char *name);
    hn_status (*replace_selection)(hn_host *h, const char *utf8, size_t len);
    hn_status (*insert_text)(hn_host *h, const char *utf8, size_t len);
    hn_status (*end_transaction)(hn_host *h);
    /* Events are coalesced by the host (selection-changed/note-saved: latest per note). */
    hn_status (*subscribe)(hn_host *h, uint32_t event_type, hn_event_fn cb, void *user);
    /* Host-scheduled one-shot work on the UI thread after delay_ms. No document is bound. */
    hn_status (*schedule_work)(hn_host *h, hn_work_fn cb, void *user, uint32_t delay_ms);
    void (*free_buffer)(hn_host *h, void *p);
} hn_host_api;

/* Filled in by the module inside hn_mod_entry. The host sets struct_size to its own sizeof before the call. */
typedef struct hn_mod_api {
    uint32_t struct_size;   /* module must set to sizeof(hn_mod_api) it was compiled with */
    uint32_t api_version;   /* module must set to HN_MOD_API_VERSION it was compiled with */
    const char *name;       /* static string */
    void (*shutdown)(hn_host *h); /* optional, called at application exit */
} hn_mod_api;

/* Modules export this symbol (name from manifest "entry", default hn_mod_entry). Register commands/subscriptions here.
 * Return HN_OK on success; anything else aborts activation and all registrations made are discarded. */
typedef hn_status (*hn_mod_entry_fn)(const hn_host_api *host, hn_mod_api *out);
hn_status hn_mod_entry(const hn_host_api *host, hn_mod_api *out);

#define HN_HOST_API_V1_SIZE (offsetof(hn_host_api, free_buffer) + sizeof(void *))
#define HN_MOD_API_V1_SIZE (offsetof(hn_mod_api, shutdown) + sizeof(void *))
/* Module-side check that the host table is new enough and compatible. */
#define HN_HOST_COMPATIBLE(host) ((host) && (host)->api_version == HN_MOD_API_VERSION && (host)->struct_size >= HN_HOST_API_V1_SIZE)

#ifdef __cplusplus
}
#endif
#endif
