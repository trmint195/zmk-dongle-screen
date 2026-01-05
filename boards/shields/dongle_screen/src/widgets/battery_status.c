/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/services/bas.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/split/central.h>
#include <zmk/display.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/usb.h>

#include "battery_status.h"
#include "../brightness.h"

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_DONGLE_BATTERY)
    #define SOURCE_OFFSET 1
#else
    #define SOURCE_OFFSET 0
#endif

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

/* ===================== DATA STRUCTURES ===================== */

struct battery_state {
    uint8_t source;
    uint8_t level;
    bool usb_present;
};

struct battery_object {
    lv_obj_t *container;
    lv_obj_t *fill;
    lv_obj_t *label;
};

static struct battery_object
battery_objects[ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET];

/* Peripheral reconnection tracking */
static int8_t last_battery_levels
    [ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET];

/* ===================== PERIPHERAL TRACKING ===================== */

static void init_peripheral_tracking(void) {
    for (int i = 0;
         i < (ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET);
         i++) {
        last_battery_levels[i] = -1;
    }
}

static bool is_peripheral_reconnecting(uint8_t source, uint8_t new_level) {
    if (source >= (ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET)) {
        return false;
    }

    int8_t previous = last_battery_levels[source];
    bool reconnecting = (previous < 1) && (new_level >= 1);

    if (reconnecting) {
        LOG_INF("Peripheral %d reconnected (%d%%)", source, new_level);
    }

    return reconnecting;
}

/* ===================== BATTERY DRAW (LVGL 9) ===================== */

static void draw_battery(struct battery_object *bat,
                         uint8_t level,
                         bool usb_present)
{
    ARG_UNUSED(usb_present);

    if (level > 100) level = 100;

    /* Background color by level */
    if (level < 1) {
        lv_obj_set_style_bg_color(
            bat->container,
            lv_palette_main(LV_PALETTE_RED), 0);
        lv_obj_set_style_bg_opa(
            bat->container, LV_OPA_COVER, 0);
    } else if (level <= 10) {
        lv_obj_set_style_bg_color(
            bat->container,
            lv_palette_main(LV_PALETTE_YELLOW), 0);
        lv_obj_set_style_bg_opa(
            bat->container, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_opa(
            bat->container, LV_OPA_TRANSP, 0);
    }

    /* Fill */
    if (level == 0) {
        lv_obj_add_flag(bat->fill, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(bat->fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(bat->fill, level, 3);
    }
}

/* ===================== UPDATE UI ===================== */

static void set_battery_symbol(lv_obj_t *widget,
                               struct battery_state state)
{
    if (state.source >=
        (ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET)) {
        return;
    }

    bool reconnecting =
        is_peripheral_reconnecting(state.source, state.level);

    last_battery_levels[state.source] = state.level;

#if CONFIG_DONGLE_SCREEN_IDLE_TIMEOUT_S > 0
    if (reconnecting) {
        brightness_wake_screen_on_reconnect();
    }
#endif

    struct battery_object *bat =
        &battery_objects[state.source];

    draw_battery(bat, state.level, state.usb_present);

    /* Label */
    if (state.level < 1) {
        lv_obj_set_style_text_color(
            bat->label,
            lv_palette_main(LV_PALETTE_RED), 0);
        lv_label_set_text(bat->label, "X");
    } else if (state.level <= 10) {
        lv_obj_set_style_text_color(
            bat->label,
            lv_palette_main(LV_PALETTE_YELLOW), 0);
        lv_label_set_text_fmt(bat->label, "%4u", state.level);
    } else {
        lv_obj_set_style_text_color(
            bat->label,
            lv_color_white(), 0);
        lv_label_set_text_fmt(bat->label, "%4u", state.level);
    }

    lv_obj_clear_flag(bat->container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(bat->label, LV_OBJ_FLAG_HIDDEN);
}

/* ===================== EVENT HANDLING ===================== */

void battery_status_update_cb(struct battery_state state) {
    struct zmk_widget_dongle_battery_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        set_battery_symbol(widget->obj, state);
    }
}

static struct battery_state
peripheral_battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    return (struct battery_state){
        .source = ev->source + SOURCE_OFFSET,
        .level = ev->state_of_charge,
    };
}

static struct battery_state
central_battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev =
        as_zmk_battery_state_changed(eh);
    return (struct battery_state){
        .source = 0,
        .level = (ev != NULL)
            ? ev->state_of_charge
            : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif
    };
}

static struct battery_state
battery_status_get_state(const zmk_event_t *eh) {
    if (as_zmk_peripheral_battery_state_changed(eh) != NULL) {
        return peripheral_battery_status_get_state(eh);
    }
    return central_battery_status_get_state(eh);
}

/* ===================== ZMK MACROS ===================== */

ZMK_DISPLAY_WIDGET_LISTENER(widget_dongle_battery_status,
    struct battery_state,
    battery_status_update_cb,
    battery_status_get_state)

ZMK_SUBSCRIPTION(widget_dongle_battery_status,
                 zmk_peripheral_battery_state_changed);

#if IS_ENABLED(CONFIG_ZMK_DONGLE_DISPLAY_DONGLE_BATTERY)
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || \
     IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

ZMK_SUBSCRIPTION(widget_dongle_battery_status,
                 zmk_battery_state_changed);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_dongle_battery_status,
                 zmk_usb_conn_state_changed);
#endif
#endif
#endif

/* ===================== INIT ===================== */

int zmk_widget_dongle_battery_status_init(
    struct zmk_widget_dongle_battery_status *widget,
    lv_obj_t *parent)
{
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 240, 40);

    for (int i = 0;
         i < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT + SOURCE_OFFSET;
         i++) {

        lv_obj_t *container = lv_obj_create(widget->obj);
        lv_obj_t *fill = lv_obj_create(container);
        lv_obj_t *label = lv_label_create(widget->obj);

        /* Container */
        lv_obj_set_size(container, 102, 5);
        lv_obj_set_style_border_width(container, 1, 0);
        lv_obj_set_style_border_color(
            container, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(
            container, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(container,
            LV_OBJ_FLAG_SCROLLABLE);

        /* Fill */
        lv_obj_set_pos(fill, 1, 1);
        lv_obj_set_size(fill, 100, 3);
        lv_obj_set_style_bg_color(
            fill, lv_color_black(), 0);
        lv_obj_set_style_border_width(fill, 0, 0);

        /* Align */
        lv_obj_align(container, LV_ALIGN_BOTTOM_MID,
                     -60 + (i * 120), -8);
        lv_obj_align(label, LV_ALIGN_TOP_MID,
                     -60 + (i * 120), 0);

        lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);

        battery_objects[i] = (struct battery_object){
            .container = container,
            .fill = fill,
            .label = label,
        };
    }

    sys_slist_append(&widgets, &widget->node);
    init_peripheral_tracking();
    widget_dongle_battery_status_init();

    return 0;
}

lv_obj_t *
zmk_widget_dongle_battery_status_obj(
    struct zmk_widget_dongle_battery_status *widget)
{
    return widget->obj;
}
