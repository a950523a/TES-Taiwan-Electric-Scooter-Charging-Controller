#include "services/wifi_list.h"
#include <string.h>

int wifi_list_count(const wifi_list_t *l)
{
    int n = 0;
    while (n < WIFI_LIST_MAX && l->e[n].ssid[0]) n++;
    return n;
}

int wifi_list_find(const wifi_list_t *l, const char *ssid)
{
    if (!ssid || !ssid[0]) return -1;
    for (int i = 0; i < WIFI_LIST_MAX && l->e[i].ssid[0]; i++)
        if (strcmp(l->e[i].ssid, ssid) == 0) return i;
    return -1;
}

// 把 e[i] 拿出來、其他往後挪，空出 e[0]
static void move_to_front(wifi_list_t *l, int i)
{
    if (i <= 0) return;
    wifi_entry_t t = l->e[i];
    memmove(&l->e[1], &l->e[0], (size_t)i * sizeof(wifi_entry_t));
    l->e[0] = t;
}

wifi_list_err_t wifi_list_add(wifi_list_t *l, const char *ssid, const char *pass)
{
    if (!ssid) return WIFI_LIST_INVALID;
    size_t sl = strlen(ssid);
    if (sl == 0 || sl > WIFI_SSID_MAX_LEN) return WIFI_LIST_INVALID;
    if (pass) {
        size_t pl = strlen(pass);
        if (pl > WIFI_PASS_MAX_LEN || (pl > 0 && pl < 8)) return WIFI_LIST_INVALID;
    }
    int i = wifi_list_find(l, ssid);
    if (i < 0) {
        int n = wifi_list_count(l);
        if (n >= WIFI_LIST_MAX) return WIFI_LIST_FULL;
        i = n;
        memset(&l->e[i], 0, sizeof(l->e[i]));
        memcpy(l->e[i].ssid, ssid, sl);
    }
    if (pass) {
        memset(l->e[i].pass, 0, sizeof(l->e[i].pass));
        memcpy(l->e[i].pass, pass, strlen(pass));
    }
    move_to_front(l, i);
    return WIFI_LIST_OK;
}

wifi_list_err_t wifi_list_remove(wifi_list_t *l, const char *ssid)
{
    int i = wifi_list_find(l, ssid);
    if (i < 0) return WIFI_LIST_NOT_FOUND;
    memmove(&l->e[i], &l->e[i + 1], (size_t)(WIFI_LIST_MAX - 1 - i) * sizeof(wifi_entry_t));
    memset(&l->e[WIFI_LIST_MAX - 1], 0, sizeof(wifi_entry_t));
    return WIFI_LIST_OK;
}

void wifi_list_sanitize(wifi_list_t *l)
{
    wifi_list_t out;
    memset(&out, 0, sizeof(out));
    int n = 0;
    for (int i = 0; i < WIFI_LIST_MAX; i++) {
        l->e[i].ssid[WIFI_SSID_MAX_LEN] = '\0';
        l->e[i].pass[WIFI_PASS_MAX_LEN] = '\0';
        if (l->e[i].ssid[0] && wifi_list_find(&out, l->e[i].ssid) < 0) out.e[n++] = l->e[i];
    }
    *l = out;
}

int wifi_list_pick(const wifi_list_t *l, const char *const *seen, const int8_t *rssi, int n,
                   uint32_t skip_mask)
{
    int best = -1, best_rssi = -128;
    for (int k = 0; k < n; k++) {
        int i = wifi_list_find(l, seen[k]);
        if (i < 0 || (skip_mask & (1u << i))) continue;
        if (best < 0 || rssi[k] > best_rssi) { best = i; best_rssi = rssi[k]; }
    }
    return best;
}
