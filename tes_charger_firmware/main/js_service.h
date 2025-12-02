#pragma once
#include "quickjs.h"

// 初始化 JS 引擎並註冊所有硬體功能
void js_service_init(void);

// 執行主迴圈 (如果有需要 polling 的話)
void js_service_loop(void);