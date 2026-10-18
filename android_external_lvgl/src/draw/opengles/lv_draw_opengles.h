/**
 * @file lv_draw_opengles.h
 *
 */

#ifndef LV_DRAW_OPENGLES_H
#define LV_DRAW_OPENGLES_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "../../lv_conf_internal.h"
#if LV_USE_DRAW_OPENGLES

#include <stdbool.h>

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

void lv_draw_opengles_init(void);
void lv_draw_opengles_deinit(void);

/** Enable or disable registration of the OpenGL draw unit at lv_init(). */
void lv_draw_opengles_set_enabled(bool enabled);

/**********************
 *      MACROS
 **********************/

#endif /*LV_USE_DRAW_OPENGLES*/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_DRAW_OPENGLES_H*/
