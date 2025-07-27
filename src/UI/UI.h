#ifndef UI_H
#define UI_H

#include "Charger_Defs.h"

// 函數原型
void ui_init();
void ui_handle_input();
void ui_update_display();
UIState ui_get_current_state(); // 提供給 main.cpp 判斷

#endif // UI_H