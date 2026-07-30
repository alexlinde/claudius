#include "app_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

static uint16_t parse_color565(const char *hex)
{
    if (!hex || hex[0] != '#' || strlen(hex) != 7) return 0xFFFF;
    unsigned long v = strtoul(hex + 1, NULL, 16);
    uint8_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static bool decode_logo_bitmap(const char *b64, uint8_t *out)
{
    if (!b64 || !b64[0]) return false;
    size_t olen = 0;
    int ret = mbedtls_base64_decode(out, LOGO_BYTES, &olen,
                                    (const unsigned char *)b64, strlen(b64));
    return ret == 0 && olen == LOGO_BYTES;
}

static void copy_str(char *dst, size_t dst_len, const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        snprintf(dst, dst_len, "%s", v->valuestring);
    } else {
        dst[0] = '\0';
    }
}

bool app_proto_parse(const char *json, size_t len, app_proto_msg_t *out)
{
    if (!json || !out) return false;
    memset(out, 0, sizeof(*out));

    cJSON *doc = cJSON_ParseWithLength(json, len);
    if (!doc) return false;

    const cJSON *err = cJSON_GetObjectItemCaseSensitive(doc, "error");
    if (cJSON_IsString(err) && err->valuestring &&
        strcmp(err->valuestring, "unauthorized") == 0) {
        out->kind = APP_PROTO_UNAUTHORIZED;
        cJSON_Delete(doc);
        return true;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(doc, "type");
    if (cJSON_IsString(type) && type->valuestring) {
        if (strcmp(type->valuestring, "challenge") == 0) {
            out->kind = APP_PROTO_CHALLENGE;
            copy_str(out->nonce, sizeof(out->nonce), doc, "nonce");
            cJSON_Delete(doc);
            return true;
        }
        if (strcmp(type->valuestring, "config") == 0) {
            out->kind = APP_PROTO_CONFIG;
            const cJSON *off = cJSON_GetObjectItemCaseSensitive(doc, "utc_offset");
            if (cJSON_IsNumber(off)) {
                out->config.utc_offset = (long)off->valuedouble;
                out->config.have_utc_offset = true;
            }
            const cJSON *agents = cJSON_GetObjectItemCaseSensitive(doc, "agents");
            if (cJSON_IsObject(agents)) {
                const cJSON *agent;
                cJSON_ArrayForEach(agent, agents) {
                    if (!cJSON_IsObject(agent)) continue;
                    if (out->config.logo_count >= MAX_AGENT_LOGOS) break;
                    const cJSON *bm = cJSON_GetObjectItemCaseSensitive(agent, "logo_bitmap");
                    const cJSON *col = cJSON_GetObjectItemCaseSensitive(agent, "color");
                    agent_logo_t *logo = &out->config.logos[out->config.logo_count];
                    if (!cJSON_IsString(bm) ||
                        !decode_logo_bitmap(bm->valuestring, logo->bits)) {
                        continue;
                    }
                    logo->color_rgb565 = parse_color565(
                        cJSON_IsString(col) ? col->valuestring : NULL);
                    out->config.logo_count++;
                }
            }
            cJSON_Delete(doc);
            return true;
        }
        /* Any other typed frame (permissions, etc.) — ignore. */
        out->kind = APP_PROTO_IGNORE;
        cJSON_Delete(doc);
        return true;
    }

    /* Untyped → status payload */
    out->kind = APP_PROTO_STATUS;
    status_snapshot_t *s = &out->status;
    s->connected = true;
    s->auth_failed = false;

    const cJSON *wp = cJSON_GetObjectItemCaseSensitive(doc, "weekly_pct");
    const cJSON *sp = cJSON_GetObjectItemCaseSensitive(doc, "session_pct");
    s->weekly_pct = cJSON_IsNumber(wp) ? (float)wp->valuedouble : 0.0f;
    s->session_pct = cJSON_IsNumber(sp) ? (float)sp->valuedouble : 0.0f;

    copy_str(s->weekly_reset, sizeof(s->weekly_reset), doc, "weekly_reset");
    copy_str(s->session_reset, sizeof(s->session_reset), doc, "session_reset");
    copy_str(s->weekly_title, sizeof(s->weekly_title), doc, "weekly_title");
    copy_str(s->session_title, sizeof(s->session_title), doc, "session_title");
    copy_str(s->agent_display, sizeof(s->agent_display), doc, "agent_display");
    copy_str(s->agent_id, sizeof(s->agent_id), doc, "agent_id");

    const cJSON *sessions = cJSON_GetObjectItemCaseSensitive(doc, "sessions");
    s->sessions = cJSON_IsNumber(sessions) ? sessions->valueint : 0;

    const cJSON *st = cJSON_GetObjectItemCaseSensitive(doc, "status");
    const char *st_s = (cJSON_IsString(st) && st->valuestring) ? st->valuestring : "idle";
    if (strcmp(st_s, "working") == 0) s->status = AGENT_STATUS_WORKING;
    else if (strcmp(st_s, "waiting") == 0) s->status = AGENT_STATUS_WAITING;
    else s->status = AGENT_STATUS_IDLE;

    if (!s->weekly_reset[0]) snprintf(s->weekly_reset, sizeof(s->weekly_reset), "--");
    if (!s->session_reset[0]) snprintf(s->session_reset, sizeof(s->session_reset), "--");

    cJSON_Delete(doc);
    return true;
}

int app_proto_build_auth_hmac(const char *secret, const char *nonce,
                              char *buf, size_t buf_len)
{
    if (!secret || !nonce || !buf || buf_len < 80) return -1;

    unsigned char digest[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;
    if (mbedtls_md_hmac(info,
                        (const unsigned char *)secret, strlen(secret),
                        (const unsigned char *)nonce, strlen(nonce),
                        digest) != 0) {
        return -1;
    }

    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return snprintf(buf, buf_len, "{\"auth_hmac\":\"%s\"}", hex);
}

int app_proto_build_subscribe(char *buf, size_t buf_len)
{
    return snprintf(buf, buf_len, "{\"type\":\"subscribe\",\"client\":\"screen\"}");
}
