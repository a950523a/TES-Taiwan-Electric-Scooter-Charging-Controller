#include "services/isense_check.h"

void isense_check_init(isense_check_t *c)
{
    c->low_since_ms = 0;
    c->low = false;
    c->tripped = false;
}

bool isense_check_step(isense_check_t *c, bool charging, uint16_t req_01a, int32_t meas_ma,
                       uint32_t now_ms)
{
    if (c->tripped) {
        return false;
    }
    if (!charging || req_01a < ISENSE_CHECK_REQ_01A || meas_ma >= ISENSE_CHECK_LOW_MA) {
        c->low = false;
        return false;
    }
    if (!c->low) {
        c->low = true;
        c->low_since_ms = now_ms;
        return false;
    }
    if (now_ms - c->low_since_ms >= ISENSE_CHECK_MS) {
        c->tripped = true;
        return true;
    }
    return false;
}
