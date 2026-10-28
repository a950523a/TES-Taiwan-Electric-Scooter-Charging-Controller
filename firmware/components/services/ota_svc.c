#include "services/ota_svc.h"
#include "services/event_bus.h"
#include "services/restart_svc.h"
#include "drivers/adc_driver.h"
#include "esp_timer.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_heap_caps.h"
#include "drivers/psu_driver.h"
#include "mbedtls/sha256.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ota_svc";

static volatile ota_state_t      s_state      = OTA_STATE_IDLE;
static volatile ota_source_t     s_source     = OTA_SRC_NONE;
static volatile int              s_progress   = 0;
static char                      s_url[256];
static char                      s_error[128];

static esp_ota_handle_t          s_ota_handle = 0;
static const esp_partition_t    *s_ota_part   = NULL;
static size_t                    s_upload_total = 0;
static size_t                    s_upload_done  = 0;

// ── 兩條路共用 ────────────────────────────────────────────────────────────────

const char *ota_svc_state_name(ota_state_t s)
{
    switch (s) {
    case OTA_STATE_RUNNING:   return "running";
    case OTA_STATE_ERROR:     return "error";
    case OTA_STATE_REBOOTING: return "rebooting";
    default:                  return "idle";
    }
}

const char *ota_svc_source_name(ota_source_t s)
{
    return s == OTA_SRC_ONLINE ? "online" : s == OTA_SRC_UPLOAD ? "upload" : s == OTA_SRC_NODE ? "node" : "";
}

static void begin_common(ota_source_t src)
{
    s_source   = src;
    s_progress = 0;
    s_error[0] = '\0';
    s_state    = OTA_STATE_RUNNING;
}

static void fail(const char *fmt, const char *detail)
{
    snprintf(s_error, sizeof(s_error), fmt, detail);
    ESP_LOGE(TAG, "%s", s_error);
    s_state = OTA_STATE_ERROR;
}

// 百分比變了才發事件：每 4 KB 一則的話 1.5 MB 是 ~380 則，每則叫醒所有訂閱的任務
static void set_progress(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (pct == s_progress) return;
    s_progress = pct;
    charger_event_t evt = { .type = EVT_OTA_PROGRESS };
    evt.payload[0] = (uint8_t)pct;
    event_bus_publish(&evt);
}

// 這個映像是不是這個產品的韌體：專案名稱要跟現在跑的一樣（例如誤傳 LianMing 的 .bin 進來就擋掉）。
// 晶片型號由 esp_ota_write／esp_https_ota 自己檢查。版本不擋 —— 降版、重灌同一版都是正當的
static bool image_is_ours(const esp_app_desc_t *d)
{
    const esp_app_desc_t *me = esp_app_get_description();
    if (d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        fail("%s", "not an ESP-IDF app image");
        return false;
    }
    if (strncmp(d->project_name, me->project_name, sizeof d->project_name) != 0) {
        char p[sizeof d->project_name + 1];
        memcpy(p, d->project_name, sizeof d->project_name);
        p[sizeof d->project_name] = '\0';
        fail("not TES charger firmware (it is \"%s\")", p);
        return false;
    }
    ESP_LOGI(TAG, "image: %s %.32s", d->project_name, d->version);
    return true;
}

static void mark_done(void)
{
    s_progress = 100;
    s_state    = OTA_STATE_REBOOTING;
    ESP_LOGI(TAG, "OTA complete (%s) - rebooting", ota_svc_source_name(s_source));
    charger_event_t evt = { .type = EVT_OTA_COMPLETE };
    event_bus_publish(&evt);
}

void ota_svc_reboot(void)
{
    vTaskDelay(pdMS_TO_TICKS(1500));   // 讓 OLED 畫出「重新啟動」、網頁和 App 讀到 rebooting
    esp_restart();
}

static void reboot_now(void)
{
    mark_done();
    ota_svc_reboot();
}

// ── 線上更新 ──────────────────────────────────────────────────────────────────

static void ota_task(void *arg)
{
    esp_http_client_config_t http_cfg = {
        .url               = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .timeout_ms        = 30000,
        /*
         * GitHub 的 releases/latest/download/... 會 302 轉到 release-assets.githubusercontent.com，
         * 2026 年起那個網址帶一串簽章，超過 1000 字元。預設緩衝區 512 B：收不下 Location 標頭、
         * 轉址後的請求行也送不出去，esp_https_ota_begin() 直接 ESP_FAIL（2026-10-14 TP-0001 實測）。
         * buffer_size 收（含標頭行），buffer_size_tx 送（含請求行）。
         */
        .buffer_size       = 4096,
        .buffer_size_tx    = 2048,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_https_ota_handle_t handle;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        fail("begin: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    esp_app_desc_t desc;
    if (esp_https_ota_get_img_desc(handle, &desc) != ESP_OK || !image_is_ours(&desc)) {
        if (s_state != OTA_STATE_ERROR) fail("%s", "cannot read the image header");
        esp_https_ota_abort(handle);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t perform_ret;
    do {
        perform_ret = esp_https_ota_perform(handle);
        if (perform_ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int total = esp_https_ota_get_image_size(handle);
            int done  = esp_https_ota_get_image_len_read(handle);
            if (total > 0) set_progress((int)((int64_t)done * 100 / total));
        }
    } while (perform_ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (perform_ret != ESP_OK) {
        fail("download: %s", esp_err_to_name(perform_ret));
        esp_https_ota_abort(handle);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t finish_err = esp_https_ota_finish(handle);
    if (finish_err == ESP_OK) {
        reboot_now();
    } else {
        fail("finish: %s", esp_err_to_name(finish_err));
    }
    vTaskDelete(NULL);
}

esp_err_t ota_svc_start(const char *url)
{
    if (ota_svc_busy()) return ESP_ERR_INVALID_STATE;
    ota_svc_confirm_boot();   // 試用中的韌體不能開始下一次更新（esp_ota_begin 會拒絕）；使用者在用它更新，就算正常

    const char *target = (url && url[0] != '\0') ? url : OTA_DEFAULT_URL;
    strncpy(s_url, target, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    begin_common(OTA_SRC_ONLINE);

    ESP_LOGI(TAG, "OTA start: %s", s_url);
    BaseType_t r = xTaskCreate(ota_task, "ota", 16384, NULL, 2, NULL);
    if (r != pdPASS) {
        fail("%s", "task create failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ── 回到上一版 ────────────────────────────────────────────────────────────────

void ota_svc_confirm_boot(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "this firmware is marked valid");
    }
}

static void confirm_cb(void *arg) { (void)arg; ota_svc_confirm_boot(); }

void ota_svc_boot_check(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return;
    ESP_LOGW(TAG, "new firmware on trial: marked valid after %u s without a reset", OTA_CONFIRM_MS / 1000u);
    const esp_timer_create_args_t a = { .callback = confirm_cb, .name = "ota_ok" };
    esp_timer_handle_t t;
    if (esp_timer_create(&a, &t) != ESP_OK || esp_timer_start_once(t, OTA_CONFIRM_MS * 1000ull) != ESP_OK)
        ota_svc_confirm_boot();   // 排不了就直接確認：寧可沒有自動回滾，也不要一直卡在試用
}

// "v3.6.2-36-g1694f05" → 3006002；認不出來回傳 -1
static long ver_num(const char *v)
{
    unsigned a, b, c;
    if (v[0] == 'v') v++;
    if (sscanf(v, "%u.%u.%u", &a, &b, &c) != 3) return -1;
    return (long)a * 1000000L + (long)b * 1000L + (long)c;
}

static const esp_partition_t *previous_part(esp_app_desc_t *d)
{
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    if (!p || p == esp_ota_get_running_partition()) return NULL;
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(p, &st) == ESP_OK &&
        (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED)) return NULL;   // 自動回滾丟掉的那版
    if (esp_ota_get_partition_description(p, d) != ESP_OK) return NULL;
    if (strncmp(d->project_name, esp_app_get_description()->project_name, sizeof d->project_name) != 0)
        return NULL;
    return p;
}

// esp_ota_set_boot_partition() 開了 rollback 時把新的開機項目寫成 NEW：開起來變 PENDING_VERIFY，
// 那一版要自己確認，否則下一次重開機 bootloader 就丟掉它。手動回到的上一版可能比這個功能還舊、
// 不會確認 —— 第二次重開機就跳回現在這版。所以切過去之後直接把那個項目標成 VALID（跟 IDF 寫
// otadata 一樣：讀出、改、抹掉那一個扇區再寫回）。中途斷電只會丟掉這個項目，開回現在這一版
static esp_err_t mark_boot_entry_valid(void)
{
    const esp_partition_t *od = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (!od) return ESP_ERR_NOT_FOUND;
    esp_ota_select_entry_t e[2];
    int best = -1;
    for (int i = 0; i < 2; i++) {
        if (esp_partition_read(od, i * od->erase_size, &e[i], sizeof e[i]) != ESP_OK) return ESP_FAIL;
        bool ok = e[i].ota_seq != UINT32_MAX && e[i].crc == esp_rom_crc32_le(UINT32_MAX, (uint8_t *)&e[i].ota_seq, 4);
        if (ok && (best < 0 || e[i].ota_seq > e[best].ota_seq)) best = i;
    }
    if (best < 0) return ESP_ERR_INVALID_STATE;
    if (e[best].ota_state == ESP_OTA_IMG_VALID) return ESP_OK;
    e[best].ota_state = ESP_OTA_IMG_VALID;
    esp_err_t err = esp_partition_erase_range(od, best * od->erase_size, od->erase_size);
    if (err == ESP_OK) err = esp_partition_write(od, best * od->erase_size, &e[best], sizeof e[best]);
    return err;
}

bool ota_svc_previous(char *ver, size_t n)
{
    esp_app_desc_t d;
    if (!previous_part(&d)) return false;
    snprintf(ver, n, "%.32s", d.version);
    return true;
}

esp_err_t ota_svc_rollback(char *why, size_t n)
{
    esp_app_desc_t d;
    const esp_partition_t *p;
    if (ota_svc_busy())             { snprintf(why, n, "firmware update in progress"); return ESP_ERR_INVALID_STATE; }
    if (!restart_svc_allowed())     { snprintf(why, n, "charging - stop first");       return ESP_ERR_INVALID_STATE; }
    if (!(p = previous_part(&d)))   { snprintf(why, n, "no previous version");         return ESP_ERR_NOT_FOUND; }
    // V1.3 以後的板子要認得板子 ID 的韌體，否則套用舊板的分壓係數，電壓讀成 77 %（CLAUDE.md「Release order」）
    const adc_board_t *b = adc_driver_board();
    long v = ver_num(d.version), min = ver_num(OTA_ROLLBACK_MIN_V13);
    if (b && b->level != 0 && (v < 0 || v < min)) {
        snprintf(why, n, "%.32s does not know this board (needs %s or newer)", d.version, OTA_ROLLBACK_MIN_V13);
        return ESP_ERR_NOT_SUPPORTED;
    }
    ota_svc_confirm_boot();   // 這一版跑得好好的；不確認的話 set_boot 之後狀態會亂
    esp_err_t err = esp_ota_set_boot_partition(p);   // 會先驗證映像
    if (err != ESP_OK) { snprintf(why, n, "previous image is damaged (%s)", esp_err_to_name(err)); return err; }
    if ((err = mark_boot_entry_valid()) != ESP_OK)
        ESP_LOGW(TAG, "could not mark the previous version valid (%s): it may bounce back after its first restart",
                 esp_err_to_name(err));
    ESP_LOGW(TAG, "rolling back to %.32s", d.version);
    restart_svc_request(1500, "rollback");
    return ESP_OK;
}

// ── 轉傳節點更新 ──────────────────────────────────────────────────────────────

#define NODE_IMG_MAX     (2u * 1024u * 1024u)   // 比任何節點的 OTA 分區都大，擋掉傳錯的大檔
#define NODE_CONFIRM_MS  30000u                 // 節點傳完重開，等它用新版本重新連上的時間
static uint8_t  *s_node_img;
static size_t    s_node_total, s_node_done;
static char      s_node_ver[33];
static const char *s_node_result = "";
static int64_t   s_node_result_us;

static const char *fw_reason(uint8_t r)
{
    switch (r) {
    case PSU_FW_BUSY:      return "node busy";
    case PSU_FW_REFUSED:   return "node refused the image (not its firmware, too big, or it cannot be updated)";
    case PSU_FW_FLASH:     return "node could not write its flash";
    case PSU_FW_HASH:      return "image check failed on the node (SHA-256 or image validation)";
    case PSU_FW_NOT_BEGUN: return "node lost the transfer (restarted?)";
    default:               return "link lost or no answer";
    }
}

static void node_free(void)
{
    if (s_node_img) heap_caps_free(s_node_img);
    s_node_img = NULL;
}

esp_err_t ota_svc_node_begin(size_t total, char *why, size_t n)
{
    psu_status_t p = psu_driver_get_status();
    if (ota_svc_busy())         { snprintf(why, n, "an update is already running"); return ESP_ERR_INVALID_STATE; }
    if (!restart_svc_allowed()) { snprintf(why, n, "charging - stop first");        return ESP_ERR_INVALID_STATE; }
    if (!p.connected || !p.caps_known || !(p.caps & PSU_CAP_FW_UPDATE)) {
        snprintf(why, n, "no connected node accepts updates (update the node over USB once)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (total < 1024 || total > NODE_IMG_MAX) { snprintf(why, n, "bad file size"); return ESP_ERR_INVALID_SIZE; }
    node_free();
    s_node_img = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_node_img) { snprintf(why, n, "not enough PSRAM"); return ESP_ERR_NO_MEM; }
    s_node_total = total;
    s_node_done  = 0;
    s_node_result = "";
    begin_common(OTA_SRC_NODE);
    ESP_LOGI(TAG, "node image upload: %u bytes", (unsigned)total);
    return ESP_OK;
}

esp_err_t ota_svc_node_write(const void *data, size_t len)
{
    if (!s_node_img || s_node_done + len > s_node_total) { fail("%s", "upload overflow"); node_free(); return ESP_FAIL; }
    memcpy(s_node_img + s_node_done, data, len);
    s_node_done += len;
    return ESP_OK;
}

void ota_svc_node_abort(const char *why)
{
    node_free();
    fail("node upload: %s", why);
}

static void node_task(void *arg)
{
    (void)arg;
    psu_fw_progress_t pr;
    do {
        vTaskDelay(pdMS_TO_TICKS(200));
        pr = psu_driver_fw_get();
        if (!pr.pending && pr.state != PSU_FW_TX_IDLE) set_progress(pr.pct);
    } while (pr.pending || pr.state == PSU_FW_TX_IDLE || pr.state == PSU_FW_TX_BEGIN ||
             pr.state == PSU_FW_TX_DATA || pr.state == PSU_FW_TX_END);
    node_free();
    if (pr.state != PSU_FW_TX_DONE) {
        fail("node update failed: %s", fw_reason(pr.result));
        vTaskDelete(NULL);
        return;
    }
    // 節點驗證、切換好了，正在重開：等它用新版本重新連上（INF）
    ESP_LOGI(TAG, "node update sent%s; waiting for %s", pr.result ? " (final answer lost)" : "", s_node_ver);
    const char *res = "unconfirmed";
    for (uint32_t t = 0; t < NODE_CONFIRM_MS; t += 500) {
        vTaskDelay(pdMS_TO_TICKS(500));
        psu_status_t p = psu_driver_get_status();
        if (p.connected && p.info_known && strcmp(p.fw_str, s_node_ver) == 0) { res = "ok"; break; }
    }
    s_node_result    = res;
    s_node_result_us = esp_timer_get_time();
    s_progress = 100;
    s_state    = OTA_STATE_IDLE;
    ESP_LOGI(TAG, "node update %s", res);
    vTaskDelete(NULL);
}

esp_err_t ota_svc_node_end(char *why, size_t n)
{
    if (!s_node_img || s_node_done != s_node_total) { snprintf(why, n, "incomplete upload"); ota_svc_node_abort(why); return ESP_FAIL; }
    // ESP-IDF 映像：前 24 B 映像標頭、8 B 段標頭，接著 app 描述（名稱、版本）
    const esp_app_desc_t *d = (const esp_app_desc_t *)(s_node_img + sizeof(esp_image_header_t) +
                                                       sizeof(esp_image_segment_header_t));
    if (s_node_img[0] != ESP_IMAGE_HEADER_MAGIC || d->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        snprintf(why, n, "not an ESP-IDF firmware image");
        ota_svc_node_abort(why);
        return ESP_FAIL;
    }
    if (strncmp(d->project_name, esp_app_get_description()->project_name, sizeof d->project_name) == 0) {
        snprintf(why, n, "this is the controller's own firmware - use the firmware update instead");
        ota_svc_node_abort(why);
        return ESP_FAIL;
    }
    char name[PSU_VER_STR_MAX + 1], ver[PSU_VER_STR_MAX + 1];
    size_t k = 0;
    for (size_t i = 0; i < sizeof d->project_name && d->project_name[i] && k < PSU_VER_STR_MAX; i++) name[k++] = d->project_name[i];
    name[k] = 0;
    k = 0;   // 版本字串只留 PSU-Link 允許的字元（跟節點送 INF 時的處理一樣，才比對得上）
    for (size_t i = 0; i < sizeof d->version && d->version[i] && k < PSU_VER_STR_MAX; i++) {
        char c = d->version[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            c == '.' || c == '_' || c == '+' || c == '-') ver[k++] = c;
    }
    ver[k] = 0;
    uint8_t sha[32];
    mbedtls_sha256(s_node_img, s_node_total, sha, 0);
    strlcpy(s_node_ver, ver, sizeof s_node_ver);
    if (psu_driver_fw_start(s_node_img, (uint32_t)s_node_total, sha, name, ver) != ESP_OK ||
        xTaskCreate(node_task, "node_fw", 4096, NULL, 2, NULL) != pdPASS) {
        snprintf(why, n, "could not start the transfer");
        ota_svc_node_abort(why);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "relaying %s %s (%u bytes) to the node", name, ver, (unsigned)s_node_total);
    return ESP_OK;
}

const char *ota_svc_node_result(const char **ver)
{
    if (ver) *ver = s_node_ver;
    if (!s_node_result[0] || esp_timer_get_time() - s_node_result_us > 60000000LL) return "";
    return s_node_result;
}

ota_state_t  ota_svc_get_state(void)    { return s_state;    }
ota_source_t ota_svc_source(void)       { return s_source;   }
int          ota_svc_progress_pct(void) { return s_progress; }
const char  *ota_svc_get_error(void)    { return s_error;    }

bool ota_svc_busy(void)
{
    ota_state_t s = s_state;
    return s == OTA_STATE_RUNNING || s == OTA_STATE_REBOOTING;
}

// ── 手動上傳 ──────────────────────────────────────────────────────────────────

esp_err_t ota_svc_upload_begin(size_t total_size)
{
    if (ota_svc_busy()) return ESP_ERR_INVALID_STATE;
    ota_svc_confirm_boot();   // 同 ota_svc_start

    s_ota_part = esp_ota_get_next_update_partition(NULL);
    if (!s_ota_part) {
        begin_common(OTA_SRC_UPLOAD);
        fail("%s", "no OTA partition");
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_begin(s_ota_part, OTA_WITH_SEQUENTIAL_WRITES, &s_ota_handle);
    begin_common(OTA_SRC_UPLOAD);
    if (err != ESP_OK) {
        fail("begin: %s", esp_err_to_name(err));
        return err;
    }

    s_upload_total = total_size;
    s_upload_done  = 0;
    ESP_LOGI(TAG, "upload begin, total=%u bytes", (unsigned)total_size);
    return ESP_OK;
}

esp_err_t ota_svc_upload_write(const void *data, size_t len)
{
    // 第一塊就看映像描述（映像標頭 24 B ＋ 第一段標頭 8 B 之後）：不是這個產品的韌體就不用傳完才失敗
    const size_t desc_off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (s_upload_done == 0 && len >= desc_off + sizeof(esp_app_desc_t)) {
        esp_app_desc_t d;
        memcpy(&d, (const uint8_t *)data + desc_off, sizeof d);
        if (!image_is_ours(&d)) {
            esp_ota_abort(s_ota_handle);
            s_ota_handle = 0;
            return ESP_ERR_INVALID_ARG;
        }
    }

    esp_err_t err = esp_ota_write(s_ota_handle, data, len);
    if (err != ESP_OK) {
        fail("write: %s", esp_err_to_name(err));
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
        return err;
    }
    s_upload_done += len;
    if (s_upload_total > 0) set_progress((int)((s_upload_done * 100) / s_upload_total));
    return ESP_OK;
}

void ota_svc_upload_abort(const char *why)
{
    if (s_ota_handle != 0) {
        esp_ota_abort(s_ota_handle);
        s_ota_handle = 0;
    }
    // 以前記成 IDLE：網頁、App、OLED 都看不出更新沒成功
    fail("upload interrupted: %s", why ? why : "connection lost");
}

esp_err_t ota_svc_upload_end(void)
{
    esp_err_t err = esp_ota_end(s_ota_handle);
    s_ota_handle = 0;
    if (err != ESP_OK) {
        fail("end: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(s_ota_part);
    if (err != ESP_OK) {
        fail("set boot: %s", esp_err_to_name(err));
        return err;
    }
    mark_done();   // 呼叫端回應上傳的人之後呼叫 ota_svc_reboot()
    return ESP_OK;
}
