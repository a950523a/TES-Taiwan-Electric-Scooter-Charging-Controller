#include "services/energy_month.h"
#include <string.h>

#define TZ_OFFSET_S (8 * 3600)

void energy_month_of(uint32_t epoch, uint16_t *year, uint8_t *month)
{
    // 不用 gmtime_r：純整數換算，主機與 ESP32 結果一定相同（Howard Hinnant 的 civil_from_days）
    int64_t days = ((int64_t)epoch + TZ_OFFSET_S) / 86400;
    days += 719468;
    int64_t era = days / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y   = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp  = (5 * doy + 2) / 153;
    int64_t m   = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    *year  = (uint16_t)y;
    *month = (uint8_t)m;
}

static uint32_t key_of(uint16_t y, uint8_t m) { return (uint32_t)y * 12u + (m - 1u); }

void energy_book_add(energy_book_t *b, uint32_t start_epoch, float wh, bool estimated)
{
    if (b->count > ENERGY_MONTHS) b->count = ENERGY_MONTHS;   // 壞掉的 blob 不能讓下面寫出界
    if (start_epoch == 0) {
        if (b->undated < UINT16_MAX) b->undated++;
        return;
    }
    if (wh < 0.0f) wh = 0.0f;

    uint16_t y; uint8_t mo;
    energy_month_of(start_epoch, &y, &mo);
    uint32_t k = key_of(y, mo);

    int i = 0;
    while (i < b->count && key_of(b->m[i].year, b->m[i].month) < k) i++;

    if (i == b->count || key_of(b->m[i].year, b->m[i].month) != k) {
        // 新的月份插在 i
        if (b->count == ENERGY_MONTHS) {
            if (i == 0) return;              // 比保留的最舊月份還舊：丟掉
            memmove(&b->m[0], &b->m[1], (size_t)(i - 1) * sizeof b->m[0]);
            i--;
        } else {
            memmove(&b->m[i + 1], &b->m[i], (size_t)(b->count - i) * sizeof b->m[0]);
            b->count++;
        }
        memset(&b->m[i], 0, sizeof b->m[i]);
        b->m[i].year  = y;
        b->m[i].month = mo;
    }

    energy_month_t *e = &b->m[i];
    if (e->sessions < UINT16_MAX) e->sessions++;
    e->wh += wh;
    if (estimated) {
        if (e->est_sessions < UINT16_MAX) e->est_sessions++;
        e->est_wh += wh;
    }
}
