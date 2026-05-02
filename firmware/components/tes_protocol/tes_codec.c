#include "tes_protocol/tes_codec.h"
#include <string.h>

// ─── 解碼 ─────────────────────────────────────────────────────────────────────

void tes_codec_decode_vehicle_status(const uint8_t *data, uint8_t dlc,
                                     tes_vehicle_status_t *out)
{
    if (!data || !out || dlc < 8) return;
    out->fault_flags          = data[0];
    out->status_flags         = data[1];
    out->charge_current_cmd   = (uint16_t)(data[3] << 8 | data[2]);
    out->charge_voltage_limit = (uint16_t)(data[5] << 8 | data[4]);
    out->max_charge_voltage   = (uint16_t)(data[7] << 8 | data[6]);
}

void tes_codec_decode_vehicle_params(const uint8_t *data, uint8_t dlc,
                                     tes_vehicle_params_t *out)
{
    if (!data || !out || dlc < 4) return;
    out->seq_num             = data[0];
    out->soc                 = data[1];
    out->max_charge_time_min = (uint16_t)(data[3] << 8 | data[2]);
    out->est_end_time_min    = (dlc >= 6) ? (uint16_t)(data[5] << 8 | data[4]) : 0xFFFF;
}

void tes_codec_decode_vehicle_emergency(const uint8_t *data, uint8_t dlc,
                                        tes_vehicle_emergency_t *out)
{
    if (!data || !out || dlc < 1) return;
    out->error_request_flags = data[0];
}

// ─── 編碼 ─────────────────────────────────────────────────────────────────────

void tes_codec_encode_charger_status(const tes_charger_status_t *in, uint8_t *data)
{
    if (!in || !data) return;
    data[0] = in->fault_flags;
    data[1] = in->status_flags;
    data[2] = (uint8_t)(in->available_voltage & 0xFF);
    data[3] = (uint8_t)(in->available_voltage >> 8);
    data[4] = (uint8_t)(in->available_current & 0xFF);
    data[5] = (uint8_t)(in->available_current >> 8);
    data[6] = (uint8_t)(in->fault_detect_voltage & 0xFF);
    data[7] = (uint8_t)(in->fault_detect_voltage >> 8);
}

void tes_codec_encode_charger_params(const tes_charger_params_t *in, uint8_t *data)
{
    if (!in || !data) return;
    data[0] = in->seq_num;
    data[1] = in->rated_power_50w;
    data[2] = (uint8_t)(in->actual_voltage & 0xFF);
    data[3] = (uint8_t)(in->actual_voltage >> 8);
    data[4] = (uint8_t)(in->actual_current & 0xFF);
    data[5] = (uint8_t)(in->actual_current >> 8);
    data[6] = (uint8_t)(in->remaining_time_min & 0xFF);
    data[7] = (uint8_t)(in->remaining_time_min >> 8);
}

void tes_codec_encode_charger_emergency(const tes_charger_emergency_t *in, uint8_t *data)
{
    if (!in || !data) return;
    memset(data, 0, 8);
    data[0] = in->emergency_flags;
    data[4] = (uint8_t)(in->manufacturer_id & 0xFF);
    data[5] = (uint8_t)(in->manufacturer_id >> 8);
}
