// factory_svc.c — 驗證出廠資料。說明見 include/services/factory_svc.h

#include "services/factory_svc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_partition.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"
#include <string.h>

static const char *TAG = "factory";

#define FACTORY_SUBTYPE   0x40          // partitions_16MB.csv：tes_factory, data, 0x40
#define DRAWING_MAX       (32 * 1024)

// 與 tools/provision_factory.py（私有硬體 repo）的 HDR_FMT 一致，小端序
typedef struct __attribute__((packed)) {
    char     magic[4];                  // "TESF"
    uint16_t version;                   // 1
    uint16_t hdr_len;                   // 156
    uint8_t  mac[6];
    uint8_t  hw_level;
    uint8_t  flags;                     // bit0：圖為 gzip
    char     serial[16];
    uint32_t made_unix;
    uint32_t drawing_off;
    uint32_t drawing_len;
    uint8_t  drawing_sha256[32];
    uint8_t  reserved[16];
    uint8_t  sig[64];                   // ECDSA P-256 r‖s（大端）簽 SHA-256(前 92 位元組)
} tes_factory_hdr_t;
_Static_assert(sizeof(tes_factory_hdr_t) == 156, "tes_factory_hdr_t 必須與 provision_factory.py 一致");
#define SIGNED_LEN offsetof(tes_factory_hdr_t, sig)

// 作者的出廠簽名公鑰（未壓縮點 04‖X‖Y）。公鑰本來就可以公開；私鑰只在作者電腦上。
// 換金鑰 = 改這裡、發新韌體、已出廠的板子用新金鑰重燒出廠資料。
static const uint8_t FACTORY_PUBKEY[65] = {
    0x04, 0xD6, 0x8B, 0x31, 0xA1, 0x96, 0x17, 0x20, 0xA4, 0xD9, 0x42, 0xA4, 0xF6,
    0x2F, 0xF3, 0x27, 0x36, 0xF8, 0xB9, 0x01, 0x00, 0xEA, 0xF3, 0xC2, 0x93, 0x59,
    0x33, 0x12, 0x50, 0xE0, 0x7D, 0x1F, 0xFD, 0xA4, 0x05, 0x6E, 0x22, 0xE1, 0x77,
    0x20, 0x1A, 0x35, 0x4E, 0x2B, 0xF5, 0x9A, 0x66, 0xB1, 0xDC, 0x85, 0xEB, 0xF2,
    0xDE, 0x67, 0xD8, 0x53, 0xBF, 0x8F, 0x29, 0x85, 0x13, 0xDB, 0xDC, 0x9B, 0x67,
};

static factory_info_t          s_info;
static bool                    s_done;
static const esp_partition_t  *s_part;
static uint32_t                s_drawing_off;

static bool verify_sig(const uint8_t *msg, size_t len, const uint8_t sig[64])
{
    uint8_t hash[32];
    mbedtls_sha256(msg, len, hash, 0);

    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r, s;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);
    bool ok = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
              mbedtls_ecp_point_read_binary(&grp, &q, FACTORY_PUBKEY, sizeof FACTORY_PUBKEY) == 0 &&
              mbedtls_mpi_read_binary(&r, sig, 32) == 0 &&
              mbedtls_mpi_read_binary(&s, sig + 32, 32) == 0 &&
              mbedtls_ecdsa_verify(&grp, hash, sizeof hash, &q, &r, &s) == 0;
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return ok;
}

static bool drawing_hash_ok(uint32_t off, uint32_t len, const uint8_t expect[32])
{
    mbedtls_sha256_context ctx;
    uint8_t buf[256], out[32];
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    for (uint32_t done = 0; done < len; ) {
        uint32_t n = len - done < sizeof buf ? len - done : sizeof buf;
        if (esp_partition_read(s_part, off + done, buf, n) != ESP_OK) {
            mbedtls_sha256_free(&ctx);
            return false;
        }
        mbedtls_sha256_update(&ctx, buf, n);
        done += n;
    }
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
    return memcmp(out, expect, sizeof out) == 0;
}

static void check(void)
{
    s_info.state  = FACTORY_NONE;
    s_info.reason = "no partition";
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, FACTORY_SUBTYPE, "tes_factory");
    if (!s_part) return;          // 2026-10 以前燒錄的板子沒有這個分區 —— 正常

    tes_factory_hdr_t h;
    if (esp_partition_read(s_part, 0, &h, sizeof h) != ESP_OK) { s_info.reason = "read error"; return; }
    if (memcmp(h.magic, "TESF", 4) != 0) { s_info.reason = "blank"; return; }   // 有分區、沒燒資料

    s_info.state = FACTORY_INVALID;
    if (h.version != 1 || h.hdr_len != sizeof h) { s_info.reason = "bad header"; return; }
    if (h.drawing_off < sizeof h || h.drawing_len == 0 || h.drawing_len > DRAWING_MAX ||
        (uint64_t)h.drawing_off + h.drawing_len > s_part->size) {
        s_info.reason = "bad drawing bounds";
        return;
    }
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) != ESP_OK || memcmp(mac, h.mac, 6) != 0) {
        s_info.reason = "mac mismatch";               // 從別片板子複製來的
        return;
    }
    if (!verify_sig((const uint8_t *)&h, SIGNED_LEN, h.sig)) { s_info.reason = "bad signature"; return; }
    if (!drawing_hash_ok(h.drawing_off, h.drawing_len, h.drawing_sha256)) {
        s_info.reason = "drawing hash mismatch";
        return;
    }

    s_info.state       = FACTORY_VALID;
    s_info.reason      = "ok";
    memcpy(s_info.serial, h.serial, sizeof s_info.serial);
    s_info.serial[sizeof s_info.serial - 1] = '\0';
    s_info.hw_level    = h.hw_level;
    s_info.made_unix   = h.made_unix;
    s_info.drawing_len = h.drawing_len;
    s_drawing_off      = h.drawing_off;
}

const factory_info_t *factory_svc_get(void)
{
    if (!s_done) {
        check();
        s_done = true;
        if (s_info.state == FACTORY_VALID)
            ESP_LOGI(TAG, "factory record valid: serial %s, level %u", s_info.serial, s_info.hw_level);
        else
            ESP_LOGI(TAG, "no valid factory record (%s)", s_info.reason);
    }
    return &s_info;
}

esp_err_t factory_svc_read_drawing(size_t offset, void *buf, size_t len)
{
    const factory_info_t *fi = factory_svc_get();
    if (fi->state != FACTORY_VALID || offset + len > fi->drawing_len) return ESP_ERR_INVALID_STATE;
    return esp_partition_read(s_part, s_drawing_off + offset, buf, len);
}
