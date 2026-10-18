/**
 * @file lv_draw_opengles.c
 *
 */

/*********************
 *      INCLUDES
 *********************/

#include "lv_draw_opengles.h"
#if LV_USE_DRAW_OPENGLES

#if LV_USE_DRAW_NANOVG
    #error "LV_USE_DRAW_NANOVG and LV_USE_DRAW_OPENGLES cannot be enabled at the same time. Disable one of them in lv_conf.h or Kconfig."
#endif

#include "../lv_draw_private.h"
#include "../../misc/cache/lv_cache_entry_private.h"
#include "../../drivers/opengles/lv_opengles_debug.h"
#include "../../drivers/opengles/lv_opengles_texture.h"
#include "../../drivers/opengles/lv_opengles_driver.h"
#include "../../drivers/opengles/lv_opengles_private.h"
#include "../../draw/lv_draw_label.h"
#include "../../draw/lv_draw_rect.h"
#include "../../draw/lv_draw_arc.h"
#include "../../draw/lv_draw_image.h"
#include "../../draw/lv_draw_triangle.h"
#include "../../draw/lv_draw_line.h"
#include "../../draw/lv_draw_3d.h"
#include "../../core/lv_obj.h"
#include "../../core/lv_refr_private.h"
#include "../../display/lv_display_private.h"
#include "../../stdlib/lv_string.h"
#include "../../misc/lv_area_private.h"

/*********************
 *      DEFINES
 *********************/

#define DRAW_UNIT_ID_OPENGLES 6

/**********************
 *      TYPEDEFS
 **********************/

typedef struct {
    lv_draw_unit_t base_unit;
    lv_draw_task_t * task_act;
    lv_cache_t * texture_cache;
    unsigned int framebuffer;
    lv_draw_buf_t render_draw_buf;
    /* AERA's browser updates one full-screen variable image continuously.
     * Keep its source-sized texture alive so a new frame is one upload and a
     * GPU scale, rather than a CPU scale plus texture allocation every time. */
    const lv_image_dsc_t * streaming_image;
    unsigned int streaming_texture;
    int32_t streaming_width;
    int32_t streaming_height;
    unsigned int blur_texture;
    unsigned int blur_pass_texture;
    int32_t blur_width;
    int32_t blur_height;
} lv_draw_opengles_unit_t;

/* All descriptors accepted by execute_drawing().  Keeping the transient cache
 * key inline avoids heap-pointer lifetime corruption while still letting us
 * normalize absolute coordinates without mutating LVGL's live task. */
typedef union {
    lv_draw_dsc_base_t base;
    lv_draw_fill_dsc_t fill;
    lv_draw_border_dsc_t border;
    lv_draw_box_shadow_dsc_t box_shadow;
    lv_draw_label_dsc_t label;
    lv_draw_arc_dsc_t arc;
    lv_draw_image_dsc_t image;
    lv_draw_triangle_dsc_t triangle;
    lv_draw_line_dsc_t line;
} draw_dsc_key_t;

typedef struct {
    draw_dsc_key_t draw_dsc;
    int32_t w;
    int32_t h;
    uint32_t text_hash;
    unsigned int texture;
} cache_data_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/

static bool opengles_texture_cache_create_cb(cache_data_t * cached_data, void * user_data);
static void opengles_texture_cache_free_cb(cache_data_t * cached_data, void * user_data);
static lv_cache_compare_res_t opengles_texture_cache_compare_cb(const cache_data_t * lhs, const cache_data_t * rhs);
static uint32_t opengles_text_hash(const char * text);

static void blend_texture_layer(lv_draw_task_t * t);
static bool draw_streaming_image(lv_draw_task_t * t);
static bool draw_backdrop_blur(lv_draw_opengles_unit_t * u, lv_draw_task_t * t);
static void draw_from_cached_texture(lv_draw_task_t * t);

static void execute_drawing(lv_draw_opengles_unit_t * u);

static int32_t dispatch(lv_draw_unit_t * draw_unit, lv_layer_t * layer);

static int32_t evaluate(lv_draw_unit_t * draw_unit, lv_draw_task_t * task);
static bool draw_to_texture(lv_draw_opengles_unit_t * u, cache_data_t * cache_data);

static unsigned int layer_get_texture(lv_layer_t * layer);
static unsigned int get_framebuffer(lv_draw_opengles_unit_t * u);
static unsigned int create_texture(int32_t w, int32_t h, const void * data);

#if LV_USE_3DTEXTURE
    static void lv_draw_opengles_3d(lv_draw_task_t * t, const lv_draw_3d_dsc_t * dsc, const lv_area_t * coords);
#endif

/**********************
 *  STATIC VARIABLES
 **********************/

static lv_draw_opengles_unit_t * g_unit;
static bool runtime_enabled;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_draw_opengles_init(void)
{
    if(!runtime_enabled) return;

    lv_draw_opengles_unit_t * draw_opengles_unit = lv_draw_create_unit(sizeof(lv_draw_opengles_unit_t));
    draw_opengles_unit->base_unit.dispatch_cb = dispatch;
    draw_opengles_unit->base_unit.evaluate_cb = evaluate;
    draw_opengles_unit->base_unit.name = "OPENGLES";
    draw_opengles_unit->texture_cache = lv_cache_create(&lv_cache_class_lru_rb_count,
    sizeof(cache_data_t), LV_DRAW_OPENGLES_TEXTURE_CACHE_COUNT, (lv_cache_ops_t) {
        .compare_cb = (lv_cache_compare_cb_t)opengles_texture_cache_compare_cb,
        .create_cb = (lv_cache_create_cb_t)opengles_texture_cache_create_cb,
        .free_cb = (lv_cache_free_cb_t)opengles_texture_cache_free_cb,
    });
    lv_cache_set_name(draw_opengles_unit->texture_cache, "OPENGLES_TEXTURE");

    lv_draw_buf_init(&draw_opengles_unit->render_draw_buf, 0, 0, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO, NULL, 0);

    g_unit = draw_opengles_unit;
}

void lv_draw_opengles_deinit(void)
{
    if(g_unit == NULL) return;

    lv_free(g_unit->render_draw_buf.unaligned_data);
    if(g_unit->streaming_texture != 0) {
        GL_CALL(glDeleteTextures(1, &g_unit->streaming_texture));
    }
    if(g_unit->blur_texture != 0) {
        GL_CALL(glDeleteTextures(1, &g_unit->blur_texture));
    }
    if(g_unit->blur_pass_texture != 0) {
        GL_CALL(glDeleteTextures(1, &g_unit->blur_pass_texture));
    }
    lv_cache_destroy(g_unit->texture_cache, g_unit);
    if(g_unit->framebuffer != 0) {
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
        GL_CALL(glDeleteFramebuffers(1, &g_unit->framebuffer));
    }
    g_unit = NULL;
}

void lv_draw_opengles_set_enabled(bool enabled)
{
    runtime_enabled = enabled;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static bool opengles_texture_cache_create_cb(cache_data_t * cached_data, void * user_data)
{
    LV_PROFILER_DRAW_BEGIN;
    bool ret = draw_to_texture((lv_draw_opengles_unit_t *)user_data, cached_data);
    LV_PROFILER_DRAW_END;
    return ret;
}

static void opengles_texture_cache_free_cb(cache_data_t * cached_data, void * user_data)
{
    LV_UNUSED(user_data);
    LV_PROFILER_DRAW_BEGIN;

    GL_CALL(glDeleteTextures(1, &cached_data->texture));
    cached_data->texture = 0;
    LV_PROFILER_DRAW_END;
}

static lv_cache_compare_res_t opengles_texture_cache_compare_cb(const cache_data_t * lhs, const cache_data_t * rhs)
{
    if(lhs == rhs) return 0;

    if(lhs->w != rhs->w) {
        return lhs->w > rhs->w ? 1 : -1;
    }
    if(lhs->h != rhs->h) {
        return lhs->h > rhs->h ? 1 : -1;
    }
    if(lhs->text_hash != rhs->text_hash) {
        return lhs->text_hash > rhs->text_hash ? 1 : -1;
    }

    const lv_draw_dsc_base_t * lhs_draw_dsc = &lhs->draw_dsc.base;
    const lv_draw_dsc_base_t * rhs_draw_dsc = &rhs->draw_dsc.base;
    uint32_t lhs_dsc_size = lhs_draw_dsc->dsc_size;
    uint32_t rhs_dsc_size = rhs_draw_dsc->dsc_size;

    if(lhs_dsc_size != rhs_dsc_size) {
        return lhs_dsc_size > rhs_dsc_size ? 1 : -1;
    }

    const uint8_t * left_draw_dsc = (const uint8_t *)lhs_draw_dsc;
    const uint8_t * right_draw_dsc = (const uint8_t *)rhs_draw_dsc;
    left_draw_dsc += sizeof(lv_draw_dsc_base_t);
    right_draw_dsc += sizeof(lv_draw_dsc_base_t);

    int cmp_res = lv_memcmp(left_draw_dsc, right_draw_dsc,
                           lhs_dsc_size - sizeof(lv_draw_dsc_base_t));

    if(cmp_res != 0) {
        return cmp_res > 0 ? 1 : -1;
    }

    return 0;
}

static uint32_t opengles_text_hash(const char * text)
{
    /* FNV-1a makes cached dynamic labels content-safe even if LVGL later
     * reallocates or reuses their backing text pointer. */
    uint32_t hash = 2166136261u;
    if(text == NULL) return hash;
    while(*text != '\0') {
        hash ^= (uint8_t)*text++;
        hash *= 16777619u;
    }
    return hash;
}

static int32_t dispatch(lv_draw_unit_t * draw_unit, lv_layer_t * layer)
{
    lv_draw_opengles_unit_t * draw_opengles_unit = (lv_draw_opengles_unit_t *) draw_unit;

    /*Return immediately if it's busy with a draw task*/
    if(draw_opengles_unit->task_act) return 0;

    lv_draw_task_t * t = NULL;
    t = lv_draw_get_available_task(layer, NULL, DRAW_UNIT_ID_OPENGLES);
    if(t == NULL) return -1;

    unsigned int texture = layer_get_texture(layer);
    if(texture == 0) {
        lv_display_t * disp = lv_refr_get_disp_refreshing();
        LV_ASSERT(layer != disp->layer_head);
        int32_t w = lv_area_get_width(&layer->buf_area);
        int32_t h = lv_area_get_height(&layer->buf_area);

        texture = create_texture(w, h, NULL);
        layer->user_data = (void *)(uintptr_t)texture;

        /* Transform/opacity layers do not necessarily paint every texel.
         * glTexImage2D(..., NULL) leaves their untouched transparent margins
         * undefined, so rounded animated controls otherwise expose stale GPU
         * memory around their edges. Initialize each off-screen layer once. */
        unsigned int framebuffer = get_framebuffer(draw_opengles_unit);
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, framebuffer));
        GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, texture, 0));
        GL_CALL(glViewport(0, 0, w, h));
        GL_CALL(glDisable(GL_SCISSOR_TEST));
        GL_CALL(glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
        GL_CALL(glClear(GL_COLOR_BUFFER_BIT));
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    }

    t->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    draw_opengles_unit->task_act = t;

    execute_drawing(draw_opengles_unit);

    draw_opengles_unit->task_act->state = LV_DRAW_TASK_STATE_FINISHED;
    draw_opengles_unit->task_act = NULL;

    /*The draw unit is free now. Request a new dispatching as it can get a new task*/
    lv_draw_dispatch_request();
    return 1;
}

static int32_t evaluate(lv_draw_unit_t * draw_unit, lv_draw_task_t * task)
{
    LV_UNUSED(draw_unit);

    if(task->type == LV_DRAW_TASK_TYPE_IMAGE &&
       ((lv_draw_image_dsc_t *)task->draw_dsc)->header.cf >= LV_COLOR_FORMAT_PROPRIETARY_START) {
        return 0;
    }

    /*If not refreshing the display probably it's a canvas rendering
     *which his not supported in OpenGL as it's not a texture.*/
    if(lv_refr_get_disp_refreshing() == NULL) return 0;

    if(((lv_draw_dsc_base_t *)task->draw_dsc)->user_data == NULL) {
        task->preference_score = 0;
        task->preferred_draw_unit_id = DRAW_UNIT_ID_OPENGLES;
    }
    return 0;
}

static bool draw_to_texture(lv_draw_opengles_unit_t * u, cache_data_t * cache_data)
{
    LV_PROFILER_DRAW_BEGIN;
    lv_draw_task_t * task = u->task_act;

    lv_layer_t dest_layer;
    lv_layer_init(&dest_layer);

    int32_t texture_w = lv_area_get_width(&task->_real_area);
    int32_t texture_h = lv_area_get_height(&task->_real_area);

    if(NULL == lv_draw_buf_reshape(&u->render_draw_buf, LV_COLOR_FORMAT_ARGB8888, texture_w, texture_h, LV_STRIDE_AUTO)) {
        uint8_t * data = u->render_draw_buf.unaligned_data;
        uint32_t data_size = LV_DRAW_BUF_SIZE(texture_w, texture_h, LV_COLOR_FORMAT_ARGB8888);
        data = lv_realloc(data, data_size);
        LV_ASSERT_MALLOC(data);
        lv_result_t init_result = lv_draw_buf_init(&u->render_draw_buf, texture_w, texture_h, LV_COLOR_FORMAT_ARGB8888,
                                                   LV_STRIDE_AUTO, data, data_size);
        LV_ASSERT(init_result == LV_RESULT_OK);
    }

    dest_layer.draw_buf = &u->render_draw_buf;
    dest_layer.color_format = LV_COLOR_FORMAT_ARGB8888;

    dest_layer.buf_area = task->_real_area;
    dest_layer._clip_area = task->_real_area;
    dest_layer.phy_clip_area = task->_real_area;
    lv_memzero(u->render_draw_buf.data, lv_area_get_size(&task->_real_area) * 4);

    lv_display_t * disp = lv_refr_get_disp_refreshing();

    lv_obj_t * obj = ((lv_draw_dsc_base_t *)task->draw_dsc)->obj;
    bool original_send_draw_task_event = false;
    if(obj) {
        original_send_draw_task_event = lv_obj_has_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    }

    switch(task->type) {
        case LV_DRAW_TASK_TYPE_FILL: {
                lv_draw_fill_dsc_t * fill_dsc = task->draw_dsc;
                lv_draw_rect_dsc_t rect_dsc;
                lv_draw_rect_dsc_init(&rect_dsc);
                rect_dsc.base.user_data = (void *)(uintptr_t)1;
                rect_dsc.bg_color = fill_dsc->color;
                rect_dsc.bg_grad = fill_dsc->grad;
                rect_dsc.radius = fill_dsc->radius;
                rect_dsc.bg_opa = fill_dsc->opa;

                lv_draw_rect(&dest_layer, &rect_dsc, &task->area);
            }
            break;
        case LV_DRAW_TASK_TYPE_BORDER: {
                lv_draw_border_dsc_t * border_dsc = task->draw_dsc;
                lv_draw_rect_dsc_t rect_dsc;
                lv_draw_rect_dsc_init(&rect_dsc);
                rect_dsc.base.user_data = (void *)(uintptr_t)1;
                rect_dsc.bg_opa = LV_OPA_TRANSP;
                rect_dsc.radius = border_dsc->radius;
                rect_dsc.border_color = border_dsc->color;
                rect_dsc.border_opa = border_dsc->opa;
                rect_dsc.border_side = border_dsc->side;
                rect_dsc.border_width = border_dsc->width;
                lv_draw_rect(&dest_layer, &rect_dsc, &task->area);
                break;
            }
        case LV_DRAW_TASK_TYPE_BOX_SHADOW: {
                lv_draw_box_shadow_dsc_t * box_shadow_dsc = task->draw_dsc;
                lv_draw_rect_dsc_t rect_dsc;
                lv_draw_rect_dsc_init(&rect_dsc);
                rect_dsc.base.user_data = (void *)(uintptr_t)1;
                rect_dsc.bg_opa = LV_OPA_0;
                rect_dsc.radius = box_shadow_dsc->radius;
                rect_dsc.bg_color = box_shadow_dsc->color;
                rect_dsc.shadow_opa = box_shadow_dsc->opa;
                rect_dsc.shadow_width = box_shadow_dsc->width;
                rect_dsc.shadow_spread = box_shadow_dsc->spread;
                rect_dsc.shadow_offset_x = box_shadow_dsc->ofs_x;
                rect_dsc.shadow_offset_y = box_shadow_dsc->ofs_y;
                lv_draw_rect(&dest_layer, &rect_dsc, &task->area);
                break;
            }
        case LV_DRAW_TASK_TYPE_LABEL: {
                lv_draw_label_dsc_t label_dsc;
                lv_memcpy(&label_dsc, task->draw_dsc, sizeof(label_dsc));
                label_dsc.base.user_data = (void *)(uintptr_t)1;
                lv_draw_label(&dest_layer, &label_dsc, &task->area);
            }
            break;
        case LV_DRAW_TASK_TYPE_ARC: {
                lv_draw_arc_dsc_t arc_dsc;
                lv_memcpy(&arc_dsc, task->draw_dsc, sizeof(arc_dsc));
                arc_dsc.base.user_data = (void *)(uintptr_t)1;
                lv_draw_arc(&dest_layer, &arc_dsc);
            }
            break;
        case LV_DRAW_TASK_TYPE_LINE: {
                lv_draw_line_dsc_t line_dsc;
                lv_memcpy(&line_dsc, task->draw_dsc, sizeof(line_dsc));
                line_dsc.base.user_data = (void *)(uintptr_t)1;
                lv_draw_line(&dest_layer, &line_dsc);
            }
            break;
        case LV_DRAW_TASK_TYPE_TRIANGLE: {
                lv_draw_triangle_dsc_t triangle_dsc;
                lv_memcpy(&triangle_dsc, task->draw_dsc, sizeof(triangle_dsc));
                triangle_dsc.base.user_data = (void *)(uintptr_t)1;
                lv_draw_triangle(&dest_layer, &triangle_dsc);
            }
            break;
        case LV_DRAW_TASK_TYPE_IMAGE: {
                lv_draw_image_dsc_t image_dsc;
                lv_memcpy(&image_dsc, task->draw_dsc, sizeof(image_dsc));
                image_dsc.base.user_data = (void *)(uintptr_t)1;
                lv_draw_image(&dest_layer, &image_dsc, &task->area);
                break;
            }
        default:
            LV_PROFILER_DRAW_END;
            return false;
    }

    while(dest_layer.draw_task_head) {
        lv_draw_dispatch_layer(disp, &dest_layer);
        if(dest_layer.draw_task_head) {
            lv_draw_dispatch_wait_for_request();
        }
    }

    unsigned int texture = create_texture(texture_w, texture_h, u->render_draw_buf.data);
    if(task->type == LV_DRAW_TASK_TYPE_IMAGE) {
        /* Image transforms are performed by the final textured quad. Linear
         * filtering keeps animated scale edges smooth without softening
         * cached labels and primitive geometry. */
        GL_CALL(glBindTexture(GL_TEXTURE_2D, texture));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));
    }

    cache_data->w = texture_w;
    cache_data->h = texture_h;
    cache_data->texture = texture;

    if(obj) {
        lv_obj_set_flag(obj, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS, original_send_draw_task_event);
    }

    LV_PROFILER_DRAW_END;
    return true;
}

static void blend_texture_layer(lv_draw_task_t * t)
{
    LV_PROFILER_DRAW_BEGIN;
    lv_draw_image_dsc_t * draw_dsc = t->draw_dsc;
    lv_draw_opengles_unit_t * u = (lv_draw_opengles_unit_t *)t->draw_unit;
    lv_area_t area;
    area.x1 = -draw_dsc->pivot.x;
    area.y1 = -draw_dsc->pivot.y;
    area.x1 = (area.x1 * draw_dsc->scale_x) / 256;
    area.y1 = (area.y1 * draw_dsc->scale_y) / 256;
    area.x1 += t->area.x1 + draw_dsc->pivot.x;
    area.y1 += t->area.y1 + draw_dsc->pivot.y;
    lv_area_set_width(&area, lv_area_get_width(&t->area) * draw_dsc->scale_x / 256);
    lv_area_set_height(&area, lv_area_get_height(&t->area) * draw_dsc->scale_y / 256);

    lv_layer_t * src_layer = (lv_layer_t *)draw_dsc->src;
    unsigned int src_texture = layer_get_texture(src_layer);


    lv_layer_t * dest_layer = t->target_layer;
    unsigned int target_texture = layer_get_texture(dest_layer);
    int32_t targ_tex_w = lv_area_get_width(&dest_layer->buf_area);
    int32_t targ_tex_h = lv_area_get_height(&dest_layer->buf_area);
    lv_area_move(&area, -dest_layer->buf_area.x1,
                 -dest_layer->buf_area.y1);

    if(target_texture) {
        unsigned int framebuffer = get_framebuffer(u);
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, framebuffer));
        GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_texture, 0));
    }

    lv_opengles_viewport(0, 0, targ_tex_w, targ_tex_h);
    // TODO rotation
    bool h_flip = false;
    bool v_flip = false;
#if LV_USE_3DTEXTURE
    if(t->type == LV_DRAW_TASK_TYPE_3D) {
        lv_draw_3d_dsc_t * _3d_dsc = (lv_draw_3d_dsc_t *)t->draw_dsc;
        h_flip = _3d_dsc->h_flip;
        v_flip = _3d_dsc->v_flip;
    }
#endif
    lv_area_t clip_area = t->clip_area;
    lv_area_move(&clip_area, -dest_layer->buf_area.x1,
                 -dest_layer->buf_area.y1);
    lv_opengles_render_texture(src_texture, &area, draw_dsc->opa, targ_tex_w, targ_tex_h, &clip_area, h_flip,
                               !v_flip);

    if(target_texture) {
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    }

    GL_CALL(glDeleteTextures(1, &src_texture));
    LV_PROFILER_DRAW_END;
}

static bool draw_streaming_image(lv_draw_task_t * t)
{
    if(t->type != LV_DRAW_TASK_TYPE_IMAGE) return false;

    lv_draw_image_dsc_t * draw_dsc = (lv_draw_image_dsc_t *)t->draw_dsc;
    if((draw_dsc->header.flags & (LV_IMAGE_FLAGS_MODIFIABLE | LV_IMAGE_FLAGS_USER1)) !=
       (LV_IMAGE_FLAGS_MODIFIABLE | LV_IMAGE_FLAGS_USER1) ||
       lv_image_src_get_type(draw_dsc->src) != LV_IMAGE_SRC_VARIABLE ||
       draw_dsc->rotation != 0 || draw_dsc->skew_x != 0 || draw_dsc->skew_y != 0 ||
       draw_dsc->recolor_opa != LV_OPA_TRANSP || draw_dsc->tile ||
       draw_dsc->clip_radius != 0 || draw_dsc->bitmap_mask_src != NULL) {
        return false;
    }

    const lv_image_dsc_t * image = (const lv_image_dsc_t *)draw_dsc->src;
    if(image == NULL || image->data == NULL || image->header.magic != LV_IMAGE_HEADER_MAGIC ||
       image->header.cf != LV_COLOR_FORMAT_ARGB8888 || image->header.w == 0 ||
       image->header.h == 0 || image->header.stride != image->header.w * 4U ||
       image->data_size < (uint32_t)image->header.stride * image->header.h) {
        return false;
    }

    lv_draw_opengles_unit_t * u = (lv_draw_opengles_unit_t *)t->draw_unit;
    if(u->streaming_texture == 0 || u->streaming_image != image ||
       u->streaming_width != image->header.w || u->streaming_height != image->header.h) {
        if(u->streaming_texture != 0) GL_CALL(glDeleteTextures(1, &u->streaming_texture));
        u->streaming_texture = create_texture(image->header.w, image->header.h, image->data);
        u->streaming_image = image;
        u->streaming_width = image->header.w;
        u->streaming_height = image->header.h;
        GL_CALL(glBindTexture(GL_TEXTURE_2D, u->streaming_texture));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    }
    else {
        GL_CALL(glBindTexture(GL_TEXTURE_2D, u->streaming_texture));
        GL_CALL(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));
        GL_CALL(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image->header.w,
                               image->header.h, GL_RGBA, GL_UNSIGNED_BYTE, image->data));
    }

    lv_layer_t * dest_layer = t->target_layer;
    unsigned int target_texture = layer_get_texture(dest_layer);
    const int32_t target_width = lv_area_get_width(&dest_layer->buf_area);
    const int32_t target_height = lv_area_get_height(&dest_layer->buf_area);
    if(target_texture != 0) {
        unsigned int framebuffer = get_framebuffer(u);
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, framebuffer));
        GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, target_texture, 0));
    }

    lv_opengles_viewport(0, 0, target_width, target_height);
    lv_area_t clip_area = t->clip_area;
    lv_area_move(&clip_area, -dest_layer->buf_area.x1, -dest_layer->buf_area.y1);
    lv_area_t render_area = t->_real_area;
    lv_area_move(&render_area, -dest_layer->buf_area.x1, -dest_layer->buf_area.y1);
    lv_opengles_render_texture(u->streaming_texture, &render_area, draw_dsc->opa,
                               target_width, target_height, &clip_area, false, false);
    if(target_texture != 0) GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    return true;
}

static bool draw_backdrop_blur(lv_draw_opengles_unit_t * u, lv_draw_task_t * t)
{
    if(t->type != LV_DRAW_TASK_TYPE_BLUR) return false;
    const lv_draw_blur_dsc_t * dsc = (const lv_draw_blur_dsc_t *)t->draw_dsc;
    if(dsc->blur_radius <= 0) return true;

    lv_layer_t * layer = t->target_layer;
    lv_area_t area;
    if(!lv_area_intersect(&area, &t->area, &t->clip_area) ||
       !lv_area_intersect(&area, &area, &layer->buf_area)) return true;

    const int32_t width = lv_area_get_width(&area);
    const int32_t height = lv_area_get_height(&area);
    const int32_t target_width = lv_area_get_width(&layer->buf_area);
    const int32_t target_height = lv_area_get_height(&layer->buf_area);
    const unsigned int target_texture = layer_get_texture(layer);
    if(target_texture != 0) {
        const unsigned int framebuffer = get_framebuffer(u);
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, framebuffer));
        GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, target_texture, 0));
    }
    else {
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    }

    if(u->blur_texture == 0) GL_CALL(glGenTextures(1, &u->blur_texture));
    if(u->blur_pass_texture == 0) GL_CALL(glGenTextures(1, &u->blur_pass_texture));
    GL_CALL(glBindTexture(GL_TEXTURE_2D, u->blur_texture));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    if(u->blur_width != width || u->blur_height != height) {
        GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, NULL));
        GL_CALL(glBindTexture(GL_TEXTURE_2D, u->blur_pass_texture));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, NULL));
        GL_CALL(glBindTexture(GL_TEXTURE_2D, u->blur_texture));
        u->blur_width = width;
        u->blur_height = height;
    }

    const int32_t source_x = area.x1 - layer->buf_area.x1;
    const int32_t source_y = target_height -
        (area.y2 - layer->buf_area.y1 + 1);
    GL_CALL(glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, source_x, source_y,
                               width, height));

    const unsigned int framebuffer = get_framebuffer(u);
    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, framebuffer));
    GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, u->blur_pass_texture, 0));
    lv_area_t pass_area;
    lv_area_set(&pass_area, 0, 0, width - 1, height - 1);
    lv_opengles_viewport(0, 0, width, height);
    lv_opengles_render_blur_texture(u->blur_texture, &pass_area, width, height,
                                    (float)dsc->blur_radius, 0.f, 1.f, 0.f);

    if(target_texture != 0) {
        GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_2D, target_texture, 0));
    }
    else {
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    }

    lv_area_t render_area = area;
    lv_area_move(&render_area, -layer->buf_area.x1, -layer->buf_area.y1);
    lv_opengles_viewport(0, 0, target_width, target_height);
    lv_opengles_render_blur_texture(u->blur_pass_texture, &render_area,
                                    target_width, target_height,
                                    (float)dsc->blur_radius,
                                    (float)dsc->corner_radius, 0.f, 1.f);
    if(target_texture != 0) GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    return true;
}

static void draw_from_cached_texture(lv_draw_task_t * t)
{
    LV_PROFILER_DRAW_BEGIN;
    if(draw_streaming_image(t)) {
        LV_PROFILER_DRAW_END;
        return;
    }
    lv_draw_opengles_unit_t * u = (lv_draw_opengles_unit_t *)t->draw_unit;
    lv_draw_dsc_base_t * live_draw_dsc = (lv_draw_dsc_base_t *)t->draw_dsc;
    if(live_draw_dsc->dsc_size > sizeof(draw_dsc_key_t)) {
        LV_LOG_WARN("OpenGL draw descriptor is too large for its cache key");
        LV_PROFILER_DRAW_END;
        return;
    }
    cache_data_t data_to_find;
    lv_memzero(&data_to_find, sizeof(data_to_find));
    lv_memcpy(&data_to_find.draw_dsc, live_draw_dsc,
              live_draw_dsc->dsc_size);
    bool h_flip = false;
    bool v_flip = false;
#if LV_USE_3DTEXTURE
    if(t->type == LV_DRAW_TASK_TYPE_3D) {
        lv_draw_3d_dsc_t * _3d_dsc = (lv_draw_3d_dsc_t *)t->draw_dsc;
        h_flip = _3d_dsc->h_flip;
        v_flip = _3d_dsc->v_flip;
    }
#endif
    data_to_find.w = lv_area_get_width(&t->_real_area);
    data_to_find.h = lv_area_get_height(&t->_real_area);
    data_to_find.text_hash = 0;
    data_to_find.texture = 0;

    if(t->type == LV_DRAW_TASK_TYPE_LABEL) {
        const lv_draw_label_dsc_t * label_dsc =
            &data_to_find.draw_dsc.label;
        data_to_find.text_hash = opengles_text_hash(label_dsc->text);
    }

    /* user_data stores the renderer to differentiate it from SW rendered
     * tasks. The cache comparator skips the base descriptor, but clear it in
     * the transient key as well so cache creation receives a clean copy. */
    data_to_find.draw_dsc.base.user_data = NULL;

    /*img_dsc->image_area is an absolute coordinate so it's different
     *for the same image on a different position. So make it relative before using for cache. */
    lv_area_t original_area = t->area;
    lv_area_t original_real_area = t->_real_area;
    bool gpu_scaled_image = false;
    if(t->type == LV_DRAW_TASK_TYPE_IMAGE) {
        lv_draw_image_dsc_t * img_dsc = &data_to_find.draw_dsc.image;
        if(lv_image_src_get_type(img_dsc->src) == LV_IMAGE_SRC_VARIABLE &&
           img_dsc->rotation == 0 && img_dsc->skew_x == 0 && img_dsc->skew_y == 0 &&
           !img_dsc->tile && img_dsc->clip_radius == 0 && img_dsc->bitmap_mask_src == NULL) {
            const lv_image_dsc_t * image = (const lv_image_dsc_t *)img_dsc->src;
            if(image != NULL && image->header.magic == LV_IMAGE_HEADER_MAGIC &&
               image->header.w > 0 && image->header.h > 0) {
                /* Cache one source-sized, recolored texture. Scale and opacity
                 * are live quad state and must not create a new CPU-rendered
                 * texture on every animation frame. */
                gpu_scaled_image = true;
                data_to_find.w = image->header.w;
                data_to_find.h = image->header.h;
                img_dsc->scale_x = LV_SCALE_NONE;
                img_dsc->scale_y = LV_SCALE_NONE;
                img_dsc->gpu_scale_x_q16 = 0;
                img_dsc->gpu_scale_y_q16 = 0;
                img_dsc->opa = LV_OPA_COVER;
                img_dsc->pivot.x = 0;
                img_dsc->pivot.y = 0;
                lv_area_set(&img_dsc->image_area, 0, 0,
                            image->header.w - 1, image->header.h - 1);
            }
        }
        if(!gpu_scaled_image)
            lv_area_move(&img_dsc->image_area,
                         -original_area.x1, -original_area.y1);
    }
    else if(t->type == LV_DRAW_TASK_TYPE_TRIANGLE) {
        lv_draw_triangle_dsc_t * tri_dsc = &data_to_find.draw_dsc.triangle;
        tri_dsc->p[0].x -= t->area.x1;
        tri_dsc->p[0].y -= t->area.y1;
        tri_dsc->p[1].x -= t->area.x1;
        tri_dsc->p[1].y -= t->area.y1;
        tri_dsc->p[2].x -= t->area.x1;
        tri_dsc->p[2].y -= t->area.y1;
    }
    else if(t->type == LV_DRAW_TASK_TYPE_LINE) {
        lv_draw_line_dsc_t * line_dsc = &data_to_find.draw_dsc.line;
        line_dsc->p1.x -= t->area.x1;
        line_dsc->p1.y -= t->area.y1;
        line_dsc->p2.x -= t->area.x1;
        line_dsc->p2.y -= t->area.y1;
    }
    else if(t->type == LV_DRAW_TASK_TYPE_ARC) {
        lv_draw_arc_dsc_t * arc_dsc = &data_to_find.draw_dsc.arc;
        arc_dsc->center.x -= t->area.x1;
        arc_dsc->center.y -= t->area.y1;
    }

    if(gpu_scaled_image) {
        lv_area_set(&t->area, 0, 0, data_to_find.w - 1, data_to_find.h - 1);
        t->_real_area = t->area;
    }
    else {
        lv_area_move(&t->area, -original_area.x1, -original_area.y1);
        lv_area_move(&t->_real_area, -original_area.x1, -original_area.y1);
    }

    /* Cache creation renders with the normalized descriptor and stores that
     * descriptor in the cache. Restore the live task pointer immediately
     * afterwards; the task itself is never modified. */
    t->draw_dsc = &data_to_find.draw_dsc.base;
    lv_cache_entry_t * entry_cached = lv_cache_acquire_or_create(u->texture_cache, &data_to_find, u);
    t->draw_dsc = live_draw_dsc;

    t->area = original_area;
    t->_real_area = original_real_area;

    if(!entry_cached) {
        LV_PROFILER_DRAW_END;
        return;
    }

    cache_data_t * data_cached = lv_cache_entry_get_data(entry_cached);
    unsigned int texture = data_cached->texture;

    lv_layer_t * dest_layer = t->target_layer;

    unsigned int target_texture = layer_get_texture(dest_layer);
    int32_t targ_tex_w = lv_area_get_width(&dest_layer->buf_area);
    int32_t targ_tex_h = lv_area_get_height(&dest_layer->buf_area);

    if(target_texture) {
        unsigned int framebuffer = get_framebuffer(u);
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, framebuffer));
        GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_texture, 0));
    }

    lv_opengles_viewport(0, 0, targ_tex_w, targ_tex_h);
    lv_area_t clip_area = t->clip_area;
    lv_area_move(&clip_area, -dest_layer->buf_area.x1, -dest_layer->buf_area.y1);
    lv_area_t render_area = t->_real_area;
    lv_area_move(&render_area, -dest_layer->buf_area.x1, -dest_layer->buf_area.y1);
    lv_opa_t render_opa = LV_OPA_COVER;
    if(gpu_scaled_image && t->type == LV_DRAW_TASK_TYPE_IMAGE)
        render_opa = ((lv_draw_image_dsc_t *)live_draw_dsc)->opa;
    const lv_draw_image_dsc_t * live_image_dsc =
        t->type == LV_DRAW_TASK_TYPE_IMAGE ? (const lv_draw_image_dsc_t *)live_draw_dsc : NULL;
    if(gpu_scaled_image && live_image_dsc != NULL &&
       live_image_dsc->gpu_scale_x_q16 != 0 && live_image_dsc->gpu_scale_y_q16 != 0) {
        const float scale_x = (float)live_image_dsc->gpu_scale_x_q16 / 65536.f;
        const float scale_y = (float)live_image_dsc->gpu_scale_y_q16 / 65536.f;
        const float source_w = (float)data_cached->w;
        const float source_h = (float)data_cached->h;
        const float x = (float)t->area.x1 + (float)live_image_dsc->pivot.x * (1.f - scale_x)
                        - (float)dest_layer->buf_area.x1;
        const float y = (float)t->area.y1 + (float)live_image_dsc->pivot.y * (1.f - scale_y)
                        - (float)dest_layer->buf_area.y1;
        lv_opengles_render_texture_precise(texture, x, y, source_w * scale_x, source_h * scale_y,
                                           render_opa, targ_tex_w, targ_tex_h, &clip_area, h_flip, v_flip);
    }
    else {
        lv_opengles_render_texture(texture, &render_area, render_opa,
                                   targ_tex_w, targ_tex_h, &clip_area, h_flip, v_flip);
    }

    if(target_texture) {
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    }

    lv_cache_release(u->texture_cache, entry_cached, u);

    /*Do not cache modifiable images as they might change in the next frame
     *resulting in stale textures in the cache. */
    if(t->type == LV_DRAW_TASK_TYPE_IMAGE) {
        lv_draw_image_dsc_t * img_dsc = (lv_draw_image_dsc_t *)t->draw_dsc;
        if(img_dsc->header.flags & LV_IMAGE_FLAGS_MODIFIABLE) {
            lv_cache_drop(u->texture_cache, &data_to_find, u);
        }
    }
    /*Do not cache lines rendered from points as dsc->points will be freed. */
    if(t->type == LV_DRAW_TASK_TYPE_LINE) {
        lv_draw_line_dsc_t * line_dsc = t->draw_dsc;
        if(line_dsc->points) {
            lv_cache_drop(u->texture_cache, &data_to_find, u);
        }
    }
    LV_PROFILER_DRAW_END;
}

static void execute_drawing(lv_draw_opengles_unit_t * u)
{
    lv_draw_task_t * t = u->task_act;
    t->draw_unit = (lv_draw_unit_t *)u;

    if(draw_backdrop_blur(u, t)) return;

    /* the shader-based fill is not working reliably with EGL. */
    if(t->type == LV_DRAW_TASK_TYPE_FILL) {
        lv_draw_fill_dsc_t * fill_dsc = t->draw_dsc;
        if(fill_dsc->radius == 0 && fill_dsc->grad.dir == LV_GRAD_DIR_NONE) {
            lv_layer_t * layer = t->target_layer;
            lv_area_t fill_area = t->area;
            lv_area_intersect(&fill_area, &fill_area, &t->clip_area);
            lv_area_move(&fill_area, -layer->buf_area.x1, -layer->buf_area.y1);

            unsigned int target_texture = layer_get_texture(layer);
            int32_t targ_tex_w = lv_area_get_width(&layer->buf_area);
            int32_t targ_tex_h = lv_area_get_height(&layer->buf_area);

            if(target_texture) {
                unsigned int framebuffer = get_framebuffer(u);
                GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, framebuffer));
                GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_texture, 0));
            }

            if(fill_dsc->opa >= LV_OPA_MAX) {
                float tex_w = (float)lv_area_get_width(&fill_area);
                float tex_h = (float)lv_area_get_height(&fill_area);
                GL_CALL(glEnable(GL_SCISSOR_TEST));
                GL_CALL(glScissor(fill_area.x1, targ_tex_h - fill_area.y1 - tex_h, tex_w, tex_h));
                /* swap red and blue channels here as they will be swapped back during flushing*/
                GL_CALL(glClearColor((float)fill_dsc->color.blue / 255.0f, (float)fill_dsc->color.green / 255.0f,
                                     (float)fill_dsc->color.red / 255.0f, 1.0f));
                GL_CALL(glClearDepthf(1.0f));
                GL_CALL(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
                GL_CALL(glDisable(GL_SCISSOR_TEST));
            }
            else {
                lv_opengles_viewport(0, 0, targ_tex_w, targ_tex_h);
                lv_opengles_render_fill(fill_dsc->color, &fill_area, fill_dsc->opa, targ_tex_w, targ_tex_h);
            }

            if(target_texture) {
                GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
            }

            return;
        }
    }

    if(t->type == LV_DRAW_TASK_TYPE_LAYER) {
        blend_texture_layer(t);
        return;
    }

#if LV_USE_3DTEXTURE
    if(t->type == LV_DRAW_TASK_TYPE_3D) {
        lv_draw_opengles_3d(t, t->draw_dsc, &t->area);
        return;
    }
#endif

    draw_from_cached_texture(t);
}

static unsigned int layer_get_texture(lv_layer_t * layer)
{
    return (unsigned int)(uintptr_t)layer->user_data;
}

static unsigned int get_framebuffer(lv_draw_opengles_unit_t * u)
{
    if(u->framebuffer == 0) {
        GL_CALL(glGenFramebuffers(1, &u->framebuffer));
    }
    return u->framebuffer;
}

static unsigned int create_texture(int32_t w, int32_t h, const void * data)
{
    LV_PROFILER_DRAW_BEGIN;
    unsigned int texture;
    GL_CALL(glGenTextures(1, &texture));
    GL_CALL(glBindTexture(GL_TEXTURE_2D, texture));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    GL_CALL(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));

    /* LV_COLOR_DEPTH 32, 16 are supported but the cached textures will always
     * have full ARGB pixels since the alpha channel is required for blending.
     */
    GL_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data));
#if 0
    GL_CALL(glGenerateMipmap(GL_TEXTURE_2D));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 20));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
    /* GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST));
     * Alternatively, the above form can be used in some cases for slightly faster performance, but
     * visual quality when using image scales that are not exactly 1:1 (or 2:1 or some other increment)
     * will be not as good.
     */
#endif

    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    GL_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));

    LV_PROFILER_DRAW_END;
    return texture;
}

#if LV_USE_3DTEXTURE
static void lv_draw_opengles_3d(lv_draw_task_t * t, const lv_draw_3d_dsc_t * dsc, const lv_area_t * coords)
{
    LV_PROFILER_DRAW_BEGIN;
    lv_draw_opengles_unit_t * u = (lv_draw_opengles_unit_t *) t->draw_unit;

    lv_layer_t * dest_layer = t->target_layer;
    unsigned int target_texture = layer_get_texture(dest_layer);
    int32_t targ_tex_w = lv_area_get_width(&dest_layer->buf_area);
    int32_t targ_tex_h = lv_area_get_height(&dest_layer->buf_area);

    if(target_texture) {
        unsigned int framebuffer = get_framebuffer(u);
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, framebuffer));
        GL_CALL(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_texture, 0));
    }

    lv_opengles_viewport(0, 0, targ_tex_w, targ_tex_h);
    lv_area_t clip_area = t->clip_area;
    lv_area_move(&clip_area, -dest_layer->buf_area.x1, -dest_layer->buf_area.y1);

    lv_opengles_render_params_t params;
    lv_opengles_render_params_init(&params);
    params.texture = dsc->tex_id;
    params.texture_area = coords;
    params.opa = dsc->opa;
    params.disp_w = targ_tex_w;
    params.disp_h = targ_tex_h;
    params.texture_clip_area = &clip_area;
    params.h_flip = dsc->h_flip;
    params.v_flip = dsc->v_flip;
    params.blend_opt = true;
    lv_opengles_render(&params);

    if(target_texture) {
        GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    }
    LV_PROFILER_DRAW_END;
}
#endif /*LV_USE_3DTEXTURE*/

#endif /*LV_USE_DRAW_OPENGLES*/
