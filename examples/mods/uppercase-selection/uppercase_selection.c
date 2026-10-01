/* Example Hyprnotes mod: command "uppercase-selection" uppercases the selected text (ASCII letters) as one undo step. */
#include "hyprnotes/mod_api.h"
#include <stdlib.h>

static const hn_host_api *g_host;

static void run(hn_host *h, void *user) {
    (void)user;
    size_t n = 0;
    char *s = g_host->get_selection_text(h, &n);
    if (!s) return;
    if (n) {
        for (size_t i = 0; i < n; ++i)
            if (s[i] >= 'a' && s[i] <= 'z') s[i] = (char)(s[i] - 'a' + 'A'); /* UTF-8 multibyte bytes are >= 0x80: untouched */
        if (g_host->begin_transaction(h, "Uppercase selection") == HN_OK) {
            g_host->replace_selection(h, s, n);
            g_host->end_transaction(h);
        }
    }
    g_host->free_buffer(h, s);
}

hn_status hn_mod_entry(const hn_host_api *host, hn_mod_api *out) {
    if (!HN_HOST_COMPATIBLE(host) || !out) return HN_ERR_INVALID;
    g_host = host;
    out->struct_size = sizeof(hn_mod_api);
    out->api_version = HN_MOD_API_VERSION;
    out->name = "uppercase-selection";
    out->shutdown = NULL;
    return host->register_command(host->handle, "uppercase-selection", "Uppercase selection", run, NULL);
}
