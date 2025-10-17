#ifndef UI_H
#define UI_H

#include "Charger_Defs.h"

// 函數原型
void ui_init();
void ui_show_boot_screen(const char* line1, const char* line2);
void ui_handle_input(const DisplayData& data);
void ui_update_display(const DisplayData& data);
UIState ui_get_current_state();

#endif // UI_H