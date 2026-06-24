#include "MKS_draw_tool.h"
#include <esp_heap_caps.h>

lv_style_t about_src1_style;
lv_style_t btn_tool_style;
static lv_style_t style_line;

lv_obj_t *about_src1; 

lv_obj_t *tool_img_back; 
lv_obj_t *tool_img_wifi; 
lv_obj_t *tool_img_language; 

lv_obj_t *tool_label_line1; 
lv_obj_t *tool_label_line2; 
lv_obj_t *tool_label_line3; 
lv_obj_t *label_tool_back; 
lv_obj_t *label_tool_wifi; 
lv_obj_t *label_tool_language; 

lv_obj_t* label_board_version;
lv_obj_t* label_Firmware_version;
lv_obj_t* label_build_version;
lv_obj_t* label_cpu_info;
lv_obj_t* label_heap_info;
lv_obj_t* tool_btn_beep;
lv_obj_t* label_tool_beep;
lv_obj_t* tool_btn_sculpture_view;
lv_obj_t* label_tool_sculpture_view;

lv_obj_t *tool_line1;
lv_obj_t *tool_line2;
lv_obj_t *tool_line3;

lv_point_t tool_line_points[3][2] = {

    { {about_first_line_x, about_first_line_y}, {about_first_line_x+460, about_first_line_y} },
    { {about_first_line_x, about_first_line_y+50}, {about_first_line_x+460, about_first_line_y+50} },
    { {about_first_line_x, about_first_line_y+100}, {about_first_line_x+460, about_first_line_y+100} },
};

LV_IMG_DECLARE(back);	
LV_IMG_DECLARE(wifi_tool);	
LV_IMG_DECLARE(png_back_pre);
LV_IMG_DECLARE(png_wifi_pre);

static lv_obj_t* label_for_imgbtn_name_side(lv_obj_t* scr, lv_obj_t* lab, lv_obj_t* base, lv_coord_t x, lv_coord_t y, const char* text, lv_align_t align) {
    lab = lv_label_create(scr, NULL);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_EXPAND);
    lv_label_set_recolor(lab, true);
    lv_label_set_text(lab, text);
    lv_obj_align(lab, base, align, x, y);
    return lab;
}

static void update_beep_button_label(void) {
    if(label_tool_beep != NULL) {
        if(beep_status->get()) {
            lv_label_set_text(label_tool_beep, "Beep:ON");
        } else {
            lv_label_set_text(label_tool_beep, "Beep:OFF");
        }
    }
}

static void update_sculpture_view_button_label(void) {
    if(label_tool_sculpture_view != NULL) {
        if(sculpture_list_mode->get()) {
            lv_label_set_text(label_tool_sculpture_view, "View:List");
        } else {
            lv_label_set_text(label_tool_sculpture_view, "View:Icon");
        }
    }
}

static void event_btn_tool_wifi(lv_obj_t* obj, lv_event_t event) {

    if (event == LV_EVENT_PRESSED) {
        mks_clear_tool();
        mks_grbl.wifi_back_from = 1;
        #if defined(ENABLE_WIFI)
            mks_draw_wifi();
        #endif
    }
}


static void event_btn_tool_back(lv_obj_t* obj, lv_event_t event) {

    if (event == LV_EVENT_PRESSED) {
        mks_clear_tool();
        mks_ui_page.mks_ui_page = MKS_UI_PAGE_LOADING;
        mks_ui_page.wait_count = DEFAULT_UI_COUNT;
        mks_draw_ready();
    }
}

static void event_btn_tool_language(lv_obj_t* obj, lv_event_t event) {

    if (event == LV_EVENT_PRESSED) {
        mks_clear_tool();
        mks_ui_page.mks_ui_page = MKS_UI_LANGUAGE;
        mks_ui_page.wait_count = DEFAULT_UI_COUNT;
        draw_language();
    }
}

static void event_btn_tool_beep(lv_obj_t* obj, lv_event_t event) {
    if (event == LV_EVENT_RELEASED) {
        static char beep_off_val[] = "0";
        static char beep_on_val[] = "1";

        if(beep_status->get()) {
            beep_status->setStringValue(beep_off_val);
            ts35_beep_off();
        } else {
            beep_status->setStringValue(beep_on_val);
        }
        update_beep_button_label();
    }
}

static void event_btn_tool_sculpture_view(lv_obj_t* obj, lv_event_t event) {
    if (event == LV_EVENT_RELEASED) {
        static char view_icon_val[] = "0";
        static char view_list_val[] = "1";

        if(sculpture_list_mode->get()) {
            sculpture_list_mode->setStringValue(view_icon_val);
        } else {
            sculpture_list_mode->setStringValue(view_list_val);
        }
        update_sculpture_view_button_label();
    }
}

void mks_tool_heap_info_update(void) {
    if (label_heap_info == NULL) {
        return;
    }

    char heap_info[96];
    uint32_t free_heap = (uint32_t)ESP.getFreeHeap();
    uint32_t min_heap  = (uint32_t)xPortGetMinimumEverFreeHeapSize();
    uint32_t lfb_heap  = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    snprintf(heap_info,
             sizeof(heap_info),
             "Heap free/min/lfb: %u/%u/%u",
             (unsigned)free_heap,
             (unsigned)min_heap,
             (unsigned)lfb_heap);
    lv_label_set_text(label_heap_info, heap_info);
}

void mks_draw_tool(void) {

    char cpu_info[128]="CPU:Freq:";
    char build_info[64];
    mks_global.mks_src_1 = lv_obj_create(mks_global.mks_src, NULL);
	lv_obj_set_size(mks_global.mks_src_1, about_src1_x_size, about_src1_y_size);
    lv_obj_set_pos(mks_global.mks_src_1, about_src1_x, about_src1_y);
    lv_obj_set_style(mks_global.mks_src_1, &mks_global.mks_src_1_style);

    tool_img_back = lv_imgbtn_creat_mks(mks_global.mks_src_1, tool_img_back, &png_back_pre, &back, LV_ALIGN_IN_LEFT_MID, 10, -6, event_btn_tool_back);

#if defined(ENABLE_WIFI)
    tool_img_wifi = lv_imgbtn_creat_mks(mks_global.mks_src_1, tool_img_wifi, &png_wifi_pre, &wifi_tool, LV_ALIGN_IN_RIGHT_MID, -20, -6, event_btn_tool_wifi);
#endif

    tool_img_language = lv_imgbtn_creat_mks(mks_global.mks_src_1, tool_img_language, &png_language_pre, &png_language, LV_ALIGN_IN_RIGHT_MID, -120, -6, event_btn_tool_language);

    lv_style_copy(&style_line, &lv_style_plain);
    style_line.line.color = LV_COLOR_MAKE(0x00, 0x3b, 0x75);
    style_line.line.width = 1;
    style_line.line.rounded = 1;

    tool_line1 = mks_lv_set_line(mks_global.mks_src, tool_line1, tool_line_points[0]);
    lv_line_set_style(tool_line1, LV_LINE_STYLE_MAIN, &style_line);
    tool_line2 = mks_lv_set_line(mks_global.mks_src, tool_line2, tool_line_points[1]);
    lv_line_set_style(tool_line2, LV_LINE_STYLE_MAIN, &style_line);
    tool_line3 = mks_lv_set_line(mks_global.mks_src, tool_line3, tool_line_points[2]);
    lv_line_set_style(tool_line3, LV_LINE_STYLE_MAIN, &style_line);

    label_for_imgbtn_name_side(mks_global.mks_src_1, label_tool_back, tool_img_back, 8, 0, "Back", LV_ALIGN_OUT_RIGHT_MID);

#if defined(ENABLE_WIFI)
    label_for_imgbtn_name_side(mks_global.mks_src_1, label_tool_wifi, tool_img_wifi, -8, 0, "Wifi", LV_ALIGN_OUT_LEFT_MID);
#endif

    label_for_imgbtn_name_side(mks_global.mks_src_1, label_tool_language, tool_img_language, -8, 0, "Language", LV_ALIGN_OUT_LEFT_MID);

    tool_btn_beep = mks_lv_btn_set(mks_global.mks_src, tool_btn_beep, 110, 32, 350, 82, event_btn_tool_beep);
    label_tool_beep = label_for_btn_name(tool_btn_beep, label_tool_beep, 0, 0, "Beep:ON");
    update_beep_button_label();

    tool_btn_sculpture_view = mks_lv_btn_set(mks_global.mks_src, tool_btn_sculpture_view, 110, 32, 350, 132, event_btn_tool_sculpture_view);
    label_tool_sculpture_view = label_for_btn_name(tool_btn_sculpture_view, label_tool_sculpture_view, 0, 0, "View:List");
    update_sculpture_view_button_label();
    

    mks_lvgl_long_sroll_label_with_wight_set_center(mks_global.mks_src, label_board_version, 10, 100, BOARD_NAME, 400);

    mks_lvgl_long_sroll_label_with_wight_set_center(mks_global.mks_src, label_Firmware_version, 10, 145, FW_NAME, 400);

    snprintf(build_info, sizeof(build_info), "Build: %s %s", __DATE__, __TIME__);
    mks_lvgl_long_sroll_label_with_wight_set_center(mks_global.mks_src, label_build_version, 10, 235, build_info, 400);

    label_heap_info = mks_lvgl_long_sroll_label_with_wight_set_center(mks_global.mks_src, label_heap_info, 10, 270, "Heap free/min/lfb: -/-/-", 400);
    mks_tool_heap_info_update();

    snprintf(cpu_info,
             sizeof(cpu_info),
             "CPU:Freq:%uMHz/ T:%.1fC/ ID:%u",
             (unsigned)ESP.getCpuFreqMHz(),
             (double)temperatureRead(),
             (unsigned)((uint16_t)(ESP.getEfuseMac() >> 32)));
    mks_lvgl_long_sroll_label_with_wight_set_center(mks_global.mks_src, label_cpu_info, 10, 190, cpu_info, 400);
    mks_ui_page.mks_ui_page = MKS_UI_Tool; 
}

void mks_clear_tool(void) {
    lv_obj_clean(mks_global.mks_src);
}


