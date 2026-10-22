#pragma once
#include <stdbool.h>
#include <stdint.h>

// 每月充入電量的累計（充電紀錄的月圖表）。純函式，可在主機上測。
//
// 充電紀錄只留最近 20 筆，不夠算月份，所以另外累計：每月一筆，最多 ENERGY_MONTHS 個月，
// 超過就丟掉最舊的。一輪充電算進「開始充電」那天所在的月份，時區固定 UTC+8（台灣沒有
// 夏令時間，不依賴 TZ 環境變數）。開始時間不知道（一直沒對到時）的那幾輪不算進來，
// 只記次數 undated，圖表註明「N 次未計入」。

#define ENERGY_MONTHS 36

typedef struct {
    uint16_t year;
    uint8_t  month;          // 1–12
    uint8_t  _pad;
    uint16_t sessions;
    uint16_t est_sessions;   // 其中電量是估算（沒接 PSU，電流用設定值）的次數
    float    wh;             // 全部充入電量（含估算）
    float    est_wh;         // 其中估算的部分
    float    ah;             // 充入 Ah（含估算），只算有 Ah 的那幾次（2026-10-22）
    uint16_t ah_sessions;    // 有 Ah 的次數；≠ sessions 表示這個月有更新前的紀錄，Ah 不完整
    uint16_t _pad2;
} energy_month_t;            // 24 bytes

typedef struct {
    uint16_t       undated;  // 沒有開始時間、沒算進月份的次數
    uint8_t        count;
    uint8_t        _pad;
    energy_month_t m[ENERGY_MONTHS];   // 由舊到新
} energy_book_t;             // 4 + 36×24 = 868 bytes（NVS blob，大小就是版本檢查；580 = 沒有 Ah 的舊版）

// epoch（Unix 秒）→ UTC+8 的年、月
void energy_month_of(uint32_t epoch, uint16_t *year, uint8_t *month);

// 把一輪充電加進去。start_epoch = 0 時只累計 undated；ah < 0 = 這一輪沒有 Ah
void energy_book_add(energy_book_t *b, uint32_t start_epoch, float wh, float ah, bool estimated);
