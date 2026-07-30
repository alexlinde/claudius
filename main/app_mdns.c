#include "app_mdns.h"
#include "app_config.h"
#include "app_dbg.h"
#include "app_wifi.h"

#include <ctype.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"

static void sanitise_hostname(const char *in, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < out_len; i++) {
        char c = in[i];
        if (isalnum((unsigned char)c)) out[j++] = (char)tolower((unsigned char)c);
        else if (c == '-' || c == ' ') out[j++] = '-';
    }
    out[j] = '\0';
    if (j == 0) snprintf(out, out_len, "claudius");
}

esp_err_t app_mdns_start(void)
{
    if (!app_wifi_is_sta_connected()) {
        /* Still advertise in AP mode so the setup page is findable if needed. */
    }

    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    char host[64];
    sanitise_hostname(g_cfg.device_name, host, sizeof(host));
    mdns_hostname_set(host);
    mdns_instance_name_set(host);
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    app_dbg_log("mdns: %s.local", host);
    return ESP_OK;
}

bool app_mdns_discover_companion(char *ip_out, size_t ip_len, uint16_t *port_out,
                                 int timeout_ms)
{
    if (!ip_out || !port_out) return false;

    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_claudius", "_tcp", timeout_ms, 8, &results);
    if (err != ESP_OK || !results) {
        app_dbg_log("mdns: no _claudius._tcp");
        return false;
    }

    bool found = false;
    for (mdns_result_t *r = results; r; r = r->next) {
        const char *inst = r->instance_name ? r->instance_name : "";
        app_dbg_log("mdns: found %s", inst);

        if (g_cfg.companion_name[0]) {
            /* Compare instance name (without .local). */
            if (strcasecmp(inst, g_cfg.companion_name) != 0) {
                app_dbg_log("mdns: skip %s (want %s)", inst, g_cfg.companion_name);
                continue;
            }
        }

        *port_out = r->port ? r->port : 8765;

        /* Prefer an IPv4 from the PTR result's address list. */
        for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(ip_out, ip_len, IPSTR, IP2STR(&a->addr.u_addr.ip4));
                found = true;
                break;
            }
        }
        if (found) break;

        /* Fall back: resolve SRV hostname (or instance name) via A query. */
        const char *host = r->hostname && r->hostname[0] ? r->hostname : inst;
        if (host && host[0]) {
            esp_ip4_addr_t ip4 = {0};
            if (mdns_query_a(host, 1500, &ip4) == ESP_OK) {
                snprintf(ip_out, ip_len, IPSTR, IP2STR(&ip4));
                found = true;
                break;
            }
            app_dbg_log("mdns: no A for %s", host);
        } else {
            app_dbg_log("mdns: %s has no address", inst);
        }
    }

    mdns_query_results_free(results);
    if (found) app_dbg_log("mdns: using %s:%u", ip_out, (unsigned)*port_out);
    return found;
}
