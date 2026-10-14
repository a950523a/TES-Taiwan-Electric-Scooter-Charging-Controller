#include "services/boot_notice.h"
#include <stdio.h>

const char *boot_rst_label(boot_rst_t rst)
{
    switch (rst) {
    case BOOT_RST_POWERON:  return "斷電後復電";
    case BOOT_RST_EXT:      return "外部重置（EN 鍵）";
    case BOOT_RST_SW:       return "軟體重新啟動";
    case BOOT_RST_PANIC:    return "當機（panic）";
    case BOOT_RST_WDT:      return "看門狗逾時";
    case BOOT_RST_BROWNOUT: return "電源電壓不足（brownout）";
    default:                return "原因不明";
    }
}

static bool abnormal(boot_rst_t rst)
{
    return rst == BOOT_RST_PANIC || rst == BOOT_RST_WDT || rst == BOOT_RST_BROWNOUT || rst == BOOT_RST_OTHER;
}

bool boot_notice_build(boot_rst_t rst, bool was_charging, int soc_start,
                       char *title, size_t title_len, char *body, size_t body_len)
{
    if (was_charging) {
        snprintf(title, title_len, "充電中斷");
        if (soc_start >= 0) {
            snprintf(body, body_len, "控制器重新啟動（%s），充電已停止，要手動重新開始。開始時 SOC %d%%",
                     boot_rst_label(rst), soc_start);
        } else {
            snprintf(body, body_len, "控制器重新啟動（%s），充電已停止，要手動重新開始。",
                     boot_rst_label(rst));
        }
        return true;
    }
    if (abnormal(rst)) {
        snprintf(title, title_len, "控制器異常重啟");
        snprintf(body, body_len, "原因：%s。目前待機中。", boot_rst_label(rst));
        return true;
    }
    return false;
}
