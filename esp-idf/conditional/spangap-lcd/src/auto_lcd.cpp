/**
 * auto_lcd.cpp — on-device Settings pane for AutoInterface (LVGL).
 *
 * Settings → Reticulum → Transports → AutoInterface. Mirrors AutoPanel.
 *
 * This whole file lives under conditional/spangap-lcd/, compiled only when the
 * lcd straddle is staged, so no #if is needed. It registers via the when:-gated
 * autoLcdRegister init: hook (spangap/spangap-lcd).
 */
#include "lcd.h"

/* Settings → Reticulum → Transports → AutoInterface. Mirrors AutoPanel. */
static void autoSettingsPane(void* arg) {
    lv_obj_t* p = static_cast<lv_obj_t*>(arg);
    lcdSettingSection (p, "AutoInterface");
    lcdSettingSwitch  (p, "Enable", "s.auto.enable");
    lcdSettingText    (p, "Group",  "s.auto.group");
    lcdSettingDropdown(p, "Mode", "s.auto.mode",
                       "gateway,full,access_point,roaming,boundary");
    lcdSettingSection (p, "Status");
    lcdSettingValue   (p, "State", "auto.state");
    lcdSettingValue   (p, "Peers", "auto.peers");
    lcdSettingValue   (p, "Address", "auto.addr");
}

/* Register the AutoInterface settings pane — a when:-gated init: hook
 * (spangap/spangap-lcd). Plain C++ linkage to match the generated dispatcher's
 * forward decl. */
void autoLcdRegister(void) {
    lcdRegisterSettings("Mesh Network/RNS Interfaces/AutoInterface", "AutoInterface", autoSettingsPane);
}
