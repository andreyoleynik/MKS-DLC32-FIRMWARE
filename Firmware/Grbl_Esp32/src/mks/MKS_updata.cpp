#include "MKS_updata.h"

#include <Update.h>
#include <SPIFFS.h>

#include "MKS_SDCard.h"

MKS_UPDATA_T mks_updata;
UPDATA_PAGE_T updata_page;

static const char* FW_UPDATE_FILE_PATH       = "/firmware.bin";
static const char* FW_UPDATE_FILE_BACKUP     = "/firmware.applied.bin";
static const char* WEBUI_UPDATE_FILE_PATH    = "/index.html.gz";
static const char* WEBUI_UPDATE_FILE_BACKUP  = "/index.html.gz.applied";
static const char* WEBUI_SPIFFS_PATH         = "/index.html.gz";
static const size_t FW_UPDATE_BUFFER_SIZE    = 4096;
static File firmware_update_file;
static bool firmware_update_started = false;

static void mks_updata_set_percent(uint8_t percent) {
    if (mks_updata.updata_persen == percent) {
        return;  // Avoid redundant LVGL redraws: no change, no work.
    }
    char str[16];
    mks_updata.updata_persen = percent;
    mks_lv_bar_updata(updata_page.bar_updata, percent);
    snprintf(str, sizeof(str), "%u%%", (unsigned)percent);
    // Update the existing label text in-place; do NOT create a new label object
    // on every call — label_for_screen() calls lv_label_create() unconditionally,
    // which leaks LVGL heap until the allocator exhausts (~15% in and display freezes).
    if (updata_page.label_updata_persen != NULL) {
        lv_label_set_text(updata_page.label_updata_persen, str);
    }
}

// ---------------------------------------------------------------------------
// WebUI update: copy /index.html.gz from SD to SPIFFS, then delete from SD.
// This runs synchronously during boot (before LVGL loop), so it doesn't need
// the chunk-by-chunk mechanism used by firmware.bin OTA.
// ---------------------------------------------------------------------------
static bool mks_apply_webui_update_from_sd() {
    File src = SD.open(WEBUI_UPDATE_FILE_PATH, FILE_READ);
    if (!src) {
        return false;
    }
    size_t src_size = src.size();
    if (src_size == 0) {
        src.close();
        SD.remove(WEBUI_UPDATE_FILE_PATH);
        grbl_send(CLIENT_SERIAL, "[MSG:SD WebUI update skipped empty file]\r\n");
        return false;
    }
    grbl_sendf(CLIENT_SERIAL,
               "[MSG:SD WebUI update start file=%s size=%u]\r\n",
               WEBUI_UPDATE_FILE_PATH,
               (unsigned)src_size);

    if (!SPIFFS.begin(true)) {
        src.close();
        grbl_send(CLIENT_SERIAL, "[MSG:SD WebUI update SPIFFS mount failed]\r\n");
        return false;
    }
    SPIFFS.remove(WEBUI_SPIFFS_PATH);  // remove old version first
    File dst = SPIFFS.open(WEBUI_SPIFFS_PATH, FILE_WRITE);
    if (!dst) {
        src.close();
        grbl_send(CLIENT_SERIAL, "[MSG:SD WebUI update SPIFFS open for write failed]\r\n");
        return false;
    }

    uint8_t buf[FW_UPDATE_BUFFER_SIZE];
    size_t  written = 0;
    while (src.available()) {
        size_t chunk = src.read(buf, sizeof(buf));
        if (chunk == 0) break;
        if (dst.write(buf, chunk) != chunk) {
            dst.close();
            src.close();
            SPIFFS.remove(WEBUI_SPIFFS_PATH);
            grbl_send(CLIENT_SERIAL, "[MSG:SD WebUI update write to SPIFFS failed]\r\n");
            return false;
        }
        written += chunk;
    }
    dst.close();
    src.close();

    if (written != src_size) {
        SPIFFS.remove(WEBUI_SPIFFS_PATH);
        grbl_sendf(CLIENT_SERIAL,
                   "[MSG:SD WebUI update size mismatch wrote=%u size=%u]\r\n",
                   (unsigned)written, (unsigned)src_size);
        return false;
    }

    if (!SD.remove(WEBUI_UPDATE_FILE_PATH)) {
        if (!SD.rename(WEBUI_UPDATE_FILE_PATH, WEBUI_UPDATE_FILE_BACKUP)) {
            grbl_send(CLIENT_SERIAL, "[MSG:SD WebUI update written but SD cleanup failed]\r\n");
        } else {
            grbl_sendf(CLIENT_SERIAL,
                       "[MSG:SD WebUI update written file renamed to %s]\r\n",
                       WEBUI_UPDATE_FILE_BACKUP);
        }
    } else {
        grbl_send(CLIENT_SERIAL, "[MSG:SD WebUI update written file removed from SD]\r\n");
    }
    grbl_sendf(CLIENT_SERIAL, "[MSG:SD WebUI update done %u bytes written]\r\n", (unsigned)written);
    return true;
}

static bool mks_has_firmware_update_file() {
    File firmware = SD.open(FW_UPDATE_FILE_PATH, FILE_READ);
    if (!firmware) {
        return false;
    }
    size_t firmware_size = firmware.size();
    firmware.close();
    if (firmware_size == 0) {
        SD.remove(FW_UPDATE_FILE_PATH);
        grbl_send(CLIENT_SERIAL, "[MSG:SD firmware update skipped empty file]\r\n");
        return false;
    }
    mks_updata.updata_total_bytes   = firmware_size;
    mks_updata.updata_written_bytes = 0;
    return true;
}

static void mks_finish_firmware_update_cleanup_and_reboot() {
    if (!SD.remove(FW_UPDATE_FILE_PATH)) {
        if (!SD.rename(FW_UPDATE_FILE_PATH, FW_UPDATE_FILE_BACKUP)) {
            grbl_send(CLIENT_SERIAL, "[MSG:SD firmware update flashed but cleanup failed]\r\n");
            mks_updata.updata_flag = UD_UPDATA_FAIL;
            return;
        }
        grbl_sendf(CLIENT_SERIAL,
                   "[MSG:SD firmware update flashed file renamed to %s]\r\n",
                   FW_UPDATE_FILE_BACKUP);
    } else {
        grbl_send(CLIENT_SERIAL, "[MSG:SD firmware update flashed file removed]\r\n");
    }

    mks_updata_set_percent(100);
    grbl_send(CLIENT_SERIAL, "[MSG:SD firmware update reboot]\r\n");
    delay(200);
    ESP.restart();
}

static void mks_process_firmware_update() {
    if (!firmware_update_started) {
        firmware_update_file = SD.open(FW_UPDATE_FILE_PATH, FILE_READ);
        if (!firmware_update_file) {
            grbl_send(CLIENT_SERIAL, "[MSG:SD firmware update open failed]\r\n");
            mks_updata.updata_flag = UD_UPDATA_FAIL;
            return;
        }
        mks_updata.updata_total_bytes = firmware_update_file.size();
        grbl_sendf(CLIENT_SERIAL,
                   "[MSG:SD firmware update start file=%s size=%u]\r\n",
                   FW_UPDATE_FILE_PATH,
                   (unsigned)mks_updata.updata_total_bytes);
        if (!Update.begin(mks_updata.updata_total_bytes)) {
            firmware_update_file.close();
            grbl_sendf(CLIENT_SERIAL,
                       "[MSG:SD firmware update begin failed err=%u]\r\n",
                       (unsigned)Update.getError());
            mks_updata.updata_flag = UD_UPDATA_FAIL;
            return;
        }
        firmware_update_started = true;
        mks_updata.updata_written_bytes = 0;
        mks_updata_set_percent(0);
    }

    uint8_t buffer[FW_UPDATE_BUFFER_SIZE];
    size_t chunk = firmware_update_file.read(buffer, sizeof(buffer));
    if (chunk == 0) {
        firmware_update_file.close();
        if (!Update.end(true)) {
            grbl_sendf(CLIENT_SERIAL,
                       "[MSG:SD firmware update finalize failed err=%u]\r\n",
                       (unsigned)Update.getError());
            mks_updata.updata_flag = UD_UPDATA_FAIL;
            firmware_update_started = false;
            return;
        }
        if (mks_updata.updata_written_bytes != mks_updata.updata_total_bytes) {
            grbl_sendf(CLIENT_SERIAL,
                       "[MSG:SD firmware update size mismatch wrote=%u size=%u]\r\n",
                       (unsigned)mks_updata.updata_written_bytes,
                       (unsigned)mks_updata.updata_total_bytes);
            mks_updata.updata_flag = UD_UPDATA_FAIL;
            firmware_update_started = false;
            return;
        }
        firmware_update_started = false;
        mks_finish_firmware_update_cleanup_and_reboot();
        return;
    }

    if (Update.write(buffer, chunk) != chunk) {
        Update.abort();
        firmware_update_file.close();
        grbl_sendf(CLIENT_SERIAL,
                   "[MSG:SD firmware update write failed err=%u]\r\n",
                   (unsigned)Update.getError());
        firmware_update_started = false;
        mks_updata.updata_flag = UD_UPDATA_FAIL;
        return;
    }

    mks_updata.updata_written_bytes += chunk;
    uint8_t percent = 0;
    if (mks_updata.updata_total_bytes > 0) {
        percent = (uint8_t)((mks_updata.updata_written_bytes * 100U) / mks_updata.updata_total_bytes);
    }
    mks_updata_set_percent(percent);
}

void mks_updata_init(void) {
    bool is_exit_file = false;
    mks_updata.updata_flag = UD_NONE;       // 默认初始没有flag
    mks_updata.updata_persen = 0;           // 默认初始0%
    mks_updata.updata_cont = 0;             // 默认初始第0个 
    mks_updata.updata_line = 0;             // 默认初始第0行
    mks_updata.updata_total_bytes = 0;
    mks_updata.updata_written_bytes = 0;
    mks_updata.is_firmware_update = false;
    mks_updata.is_webui_update = false;
    tf.init();

    // WebUI update runs synchronously (no progress bar needed — it's fast, <1 second).
    mks_apply_webui_update_from_sd();

    if (mks_has_firmware_update_file()) {
        mks_updata.is_firmware_update = true;
        mks_updata.updata_flag = UD_HAD_FILE;
        return;
    }

    mks_updata.updata_flag = UD_CHEAK;

    is_exit_file = tf.file_check(CFG_FILE_PATG);

    if(is_exit_file == true) {
        mks_updata.updata_flag = UD_HAD_FILE;   // exit cfg updata file
    }else {
        mks_updata.updata_flag = UD_NO_FILE;   // exit cfg updata file
    }
}

void mks_cfg_find(void) {
    String p;
    char send_data[128];

    p = tf.readFileLine(CFG_FILE_PATG, mks_updata.updata_line);
    if( (strstr(p.c_str() ,"-") == NULL) 
         && (strstr(p.c_str() ,"/*") == NULL) 
         && (strstr(p.c_str() ,"*-") == NULL) 
         && (strstr(p.c_str() ,"//") == NULL)
         && (p.length() < 127) 
        )  
    {   
        if( (strstr(p.c_str() ,"=") != NULL)) {
            mks_updata.is_have_data_ud = true;
        }else if(strstr(p.c_str() ,"[ESP110]") != NULL) {
            mks_updata.is_have_data_ud = true;
        }else if(strstr(p.c_str() ,"[ESP100]") != NULL) {
            mks_updata.is_have_data_ud = true;
        }else if(strstr(p.c_str() ,"[ESP101]") != NULL) {
            mks_updata.is_have_data_ud = true;
        }else if(strstr(p.c_str() ,"[ESP131]") != NULL) {
            mks_updata.is_have_data_ud = true;
        }else {
            mks_updata.is_have_data_ud = false;
        }
    }else{

    }

    if(mks_updata.is_have_data_ud == true) {
        mks_updata.updata_cont++;
        memset(send_data, 0, sizeof(send_data));
        strcpy(send_data, p.c_str());
        strcat(send_data, "\n");
        MKS_GRBL_CMD_SEND(send_data);
        mks_updata_data(); // 更新进度条
        mks_updata.is_have_data_ud = false;
    }

    mks_updata.updata_line++;

    if(mks_updata.updata_line > CFG_FILE_MAX_LINE) {   //搜索完毕，开始校验

        if(mks_updata.updata_cont >= CFG_FILE_NUM ) {
            
            mks_updata.updata_flag = UD_UPDATA_FINSH;
        }else {
            mks_updata.updata_flag = UD_UPDATA_FAIL;
        } 
    }
}

void mks_cfg_rename(const char* path1) {
    tf.renameFile(CFG_FILE_PATG, path1);
}

void mks_draw_updata(void) {
    updata_page.bar_updata = mks_lv_bar_set(mks_global.mks_src, updata_page.bar_updata, 440, 50, 20, 140, 0);
    const char* title = "Updata...";
    if (mks_updata.is_firmware_update)  title = "Firmware Update...";
    updata_page.label_updata_title = label_for_screen(mks_global.mks_src,
                                                      updata_page.label_updata_title,
                                                      0,
                                                      -80,
                                                      title);
    updata_page.label_updata_persen = label_for_screen(updata_page.label_updata_title, updata_page.label_updata_persen, 0, -100, "0%");
    mks_ui_page.mks_ui_page = MKS_UI_UPDATA;
    mks_updata.updata_flag = UD_UPDATA_ING;
}

void mks_updata_data(void) {
    uint8_t percent = 0;
    if(mks_updata.updata_cont > CFG_FILE_NUM) mks_updata.updata_cont = CFG_FILE_NUM;
    percent = (mks_updata.updata_cont *100 / CFG_FILE_NUM);
    mks_updata_set_percent(percent);
}

void mks_updata_process(void) {
    if (mks_updata.is_firmware_update) {
        mks_process_firmware_update();
    } else {
        mks_cfg_find();
    }
}