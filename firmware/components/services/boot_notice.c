#include "services/boot_notice.h"
#include <stdio.h>

const char *boot_rst_label(boot_rst_t rst, bool en)
{
    switch (rst) {
    case BOOT_RST_POWERON:  return en ? "power restored"       : "斷電後復電";
    case BOOT_RST_EXT:      return en ? "external reset (EN)"  : "外部重置（EN 鍵）";
    case BOOT_RST_SW:       return en ? "software restart"     : "軟體重新啟動";
    case BOOT_RST_PANIC:    return en ? "crash (panic)"        : "當機（panic）";
    case BOOT_RST_WDT:      return en ? "watchdog timeout"     : "看門狗逾時";
    case BOOT_RST_BROWNOUT: return en ? "supply voltage dip (brownout)" : "電源電壓不足（brownout）";
    default:                return en ? "unknown cause"        : "原因不明";
    }
}

static bool abnormal(boot_rst_t rst)
{
    return rst == BOOT_RST_PANIC || rst == BOOT_RST_WDT || rst == BOOT_RST_BROWNOUT || rst == BOOT_RST_OTHER;
}

bool boot_notice_build(boot_rst_t rst, bool was_charging, int soc_start, bool en,
                       char *title, size_t title_len, char *body, size_t body_len)
{
    const char *why = boot_rst_label(rst, en);
    if (was_charging) {
        snprintf(title, title_len, en ? "Charging interrupted" : "充電中斷");
        if (soc_start >= 0) {
            snprintf(body, body_len,
                     en ? "The controller restarted (%s). Charging has stopped; start it again by hand. SOC at start %d%%"
                        : "控制器重新啟動（%s），充電已停止，要手動重新開始。開始時 SOC %d%%",
                     why, soc_start);
        } else {
            snprintf(body, body_len,
                     en ? "The controller restarted (%s). Charging has stopped; start it again by hand."
                        : "控制器重新啟動（%s），充電已停止，要手動重新開始。",
                     why);
        }
        return true;
    }
    if (abnormal(rst)) {
        snprintf(title, title_len, en ? "Controller restarted unexpectedly" : "控制器異常重啟");
        snprintf(body, body_len, en ? "Cause: %s. Now on standby." : "原因：%s。目前待機中。", why);
        return true;
    }
    return false;
}
