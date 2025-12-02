#include "tes_io_manager.h"
#include "board_bsp.h"

void tes_io_init_all(void) {
    // Outputs
    tes_io_init(&PIN_LED_STANDBY, true);
    tes_io_init(&PIN_LED_CHARGING, true);
    tes_io_init(&PIN_LED_ERROR, true);
    tes_io_init(&PIN_RELAY_MAIN, true);
    tes_io_init(&PIN_RELAY_VP, true);
    tes_io_init(&PIN_RELAY_LOCK, true);
    
    // Inputs
    tes_io_init(&PIN_BTN_START, false);
    tes_io_init(&PIN_BTN_SETTING, false);
    tes_io_init(&PIN_BTN_STOP, false);
    tes_io_init(&PIN_BTN_EMERGENCY, false);

}