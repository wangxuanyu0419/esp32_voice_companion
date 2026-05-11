#ifndef LAUNCHER_APP_H
#define LAUNCHER_APP_H

/*
 * launcher_app.h — Desktop OS home screen.
 *
 * Displays a 2-column grid of app tiles.
 * Tile tap → app_registry_launch(id) + lv_scr_load().
 * BOOT long-press from any app → EVT_APP_SWITCH("launcher") returns here.
 */

#include "app_interface.h"
#include "esp_err.h"

/** Returns the singleton launcher app_t. */
app_t *launcher_app_get(void);

/** Initialise launcher (call once from main). */
esp_err_t launcher_app_init(void);

#endif /* LAUNCHER_APP_H */
