#pragma once
// tes_codec.h — TES-0D-02-01 CAN 訊框編解碼
// 純資料轉換，無 OS 依賴，無副作用
#include "tes_protocol/tes_types.h"
#include <stdint.h>

// ─── 解碼：原始位元組 → 結構體（vehicle → charger 方向）────────────────────

void tes_codec_decode_vehicle_status   (const uint8_t *data, uint8_t dlc, tes_vehicle_status_t    *out);
void tes_codec_decode_vehicle_params   (const uint8_t *data, uint8_t dlc, tes_vehicle_params_t    *out);
void tes_codec_decode_vehicle_emergency(const uint8_t *data, uint8_t dlc, tes_vehicle_emergency_t *out);

// ─── 編碼：結構體 → 原始位元組（charger → vehicle 方向，固定 8 bytes）───────

void tes_codec_encode_charger_status   (const tes_charger_status_t    *in, uint8_t *data);
void tes_codec_encode_charger_params   (const tes_charger_params_t    *in, uint8_t *data);
void tes_codec_encode_charger_emergency(const tes_charger_emergency_t *in, uint8_t *data);
