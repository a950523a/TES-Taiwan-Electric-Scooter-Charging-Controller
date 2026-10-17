#pragma once
#include <stdbool.h>
#include <stdint.h>

// 記住的 WiFi 網路（最多 WIFI_LIST_MAX 組，2026-10-17）。純資料，可在主機上測。
//
// 帶出門的充電樁要能自己連上公司、朋友家的 WiFi，推播與 MQTT 遠端才收得到——
// 所以不只記一組。清單從 e[0] 開始連續存放（中間沒有空格），最近加入／更新的在最前面；
// 連哪一組不看順序，看掃描時哪一組訊號最強（wifi_list_pick）。

#define WIFI_LIST_MAX      5
#define WIFI_SSID_MAX_LEN  32   // 不含 NUL
#define WIFI_PASS_MAX_LEN  63   // WPA2 上限；空字串 = 開放網路

typedef struct {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char pass[WIFI_PASS_MAX_LEN + 1];
} wifi_entry_t;

typedef struct {
    wifi_entry_t e[WIFI_LIST_MAX];
} wifi_list_t;   // 485 bytes，原樣存進 NVS blob

typedef enum {
    WIFI_LIST_OK = 0,
    WIFI_LIST_INVALID = -1,   // SSID 空的或太長、密碼 1–7 字或太長
    WIFI_LIST_FULL = -2,
    WIFI_LIST_NOT_FOUND = -3,
} wifi_list_err_t;

int  wifi_list_count(const wifi_list_t *l);
int  wifi_list_find(const wifi_list_t *l, const char *ssid);   // 沒有回 -1
// 加入或更新，並移到最前面。pass == NULL：已有的保留原密碼，新的當開放網路。
wifi_list_err_t wifi_list_add(wifi_list_t *l, const char *ssid, const char *pass);
wifi_list_err_t wifi_list_remove(wifi_list_t *l, const char *ssid);
// 載入後整理：截斷字串、把中間的空格往前補（NVS 讀出來的資料不一定乾淨）
void wifi_list_sanitize(wifi_list_t *l);

// 從掃描結果挑一組：seen[i]／rssi[i] 是掃到的 n 個 SSID。回傳清單索引（訊號最強的那組），
// skip_mask 的位元 = 這一輪已經連失敗、先跳過的索引。都沒掃到回 -1。
int  wifi_list_pick(const wifi_list_t *l, const char *const *seen, const int8_t *rssi, int n,
                    uint32_t skip_mask);
