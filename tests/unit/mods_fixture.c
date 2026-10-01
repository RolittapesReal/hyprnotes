/* Test fixture module; behaviour selected by compile definitions. */
#include "hyprnotes/mod_api.h"
#include <time.h>

static int g_events, g_work;
static const hn_host_api *g_api;
int hn_fixture_events(void) { return g_events; }
int hn_fixture_work(void) { return g_work; }

static void spin(hn_host *h, void *u) { (void)h; (void)u; struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    do { clock_gettime(CLOCK_MONOTONIC, &b); } while ((b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec) < 7000000L); }
static void leak(hn_host *h, void *u) { (void)u; const hn_host_api **p = u; (*p)->begin_transaction(h, "leak"); }
static void ev(hn_host *h, const hn_event *e, void *u) { (void)h; (void)e; (void)u; ++g_events; }
static void work(hn_host *h, void *u) { (void)h; (void)u; ++g_work; }
static void sched(hn_host *h, void *u) { (void)u; const hn_host_api **p = u; (*p)->schedule_work(h, work, 0, 1); }

hn_status hn_mod_entry(const hn_host_api *host, hn_mod_api *out) {
    if (!HN_HOST_COMPATIBLE(host)) return HN_ERR_INVALID;
    g_api = host;
    out->struct_size = sizeof(hn_mod_api);
    out->api_version = HN_MOD_API_VERSION;
    out->name = "fixture";
    out->shutdown = 0;
#ifdef FIX_BAD_VERSION
    out->api_version = 99;
#endif
#ifdef FIX_SMALL
    out->struct_size = 4;
#endif
#ifdef FIX_FAIL
    host->register_command(host->handle, "ghost", "Ghost", spin, 0);
    return HN_ERR_FAILED;
#endif
    host->register_command(host->handle, "slow", "Slow", spin, 0);
    host->register_command(host->handle, "leak", "Leak", leak, &g_api);
    host->register_command(host->handle, "sched", "Sched", sched, &g_api);
    for (unsigned t = 1; t <= 4; ++t) host->subscribe(host->handle, t, ev, 0);
    return HN_OK;
}
