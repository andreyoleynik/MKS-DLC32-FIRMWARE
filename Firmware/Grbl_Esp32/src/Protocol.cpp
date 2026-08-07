/*
  Protocol.cpp - controls Grbl execution protocol and procedures
  Part of Grbl

  Copyright (c) 2011-2016 Sungeun K. Jeon for Gnea Research LLC
  Copyright (c) 2009-2011 Simen Svale Skogsrud

    2018 -	Bart Dring This file was modifed for use on the ESP32
                    CPU. Do not use this with Grbl for atMega328P

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "Grbl.h"
#include "mks/MKS_TS35.h"
#include "lvgl.h"
#include "mks/MKS_draw_print.h"
#include "mks/MKS_draw_wifi.h"

#ifdef ENABLE_EXTERNAL_BOARD
    #include "ExternalBoard.h"
#endif

#include "SDJobPolicy.h"
#include <ctype.h>
#include <stdarg.h>

static void protocol_exec_rt_suspend();

static char    line[LINE_BUFFER_SIZE];     // Line to be executed. Zero-terminated.
static char    comment[LINE_BUFFER_SIZE];  // Line to be executed. Zero-terminated.
static uint8_t line_flags           = 0;
static uint8_t char_counter         = 0;
static uint8_t comment_char_counter = 0;

typedef struct {
    char buffer[LINE_BUFFER_SIZE];
    int  len;
    int  line_number;
} client_line_t;
client_line_t client_lines[CLIENT_COUNT];

static void empty_line(uint8_t client) {
    client_line_t* cl = &client_lines[client];
    cl->len           = 0;
    cl->buffer[0]     = '\0';
}
static void empty_lines() {
    for (uint8_t client = 0; client < CLIENT_COUNT; client++) {
        empty_line(client);
    }
}

// Возвращает true, если у какого-либо клиента (Serial/Telnet/...) в буфере лежит
// НАЧАТАЯ, но ещё НЕ ЗАВЕРШЕННАЯ строка (символы уже приняты, но '\r'/'\n' ещё не
// пришёл). Используется, чтобы не дать резюму (cycleStart) прервать suspend-цикл
// ровно в момент, когда пользователь уже начал отправлять (или ещё не полностью
// доставлена по сети) команда $MJ=... — иначе её "хвост" придёт уже ПОСЛЕ выхода
// из suspend-цикла, где manual_adjust_poll_clients() больше не вызывается, и эти
// байты либо потеряются, либо будут неверно интерпретированы как G-код.
static bool manual_adjust_input_pending() {
    for (uint8_t client = 0; client < CLIENT_COUNT; client++) {
        if (client_lines[client].len > 0) {
            return true;
        }
    }
    return false;
}

Error add_char_to_line(char c, uint8_t client) {
    client_line_t* cl = &client_lines[client];
    // Simple editing for interactive input
    if (c == '\b') {
        // Backspace erases
        if (cl->len) {
            --cl->len;
            cl->buffer[cl->len] = '\0';
        }
        return Error::Ok;
    }
    if (cl->len == (LINE_BUFFER_SIZE - 1)) {
        return Error::Overflow;
    }
    if (c == '\r' || c == '\n') {
        cl->len = 0;
        cl->line_number++;
        return Error::Eol;
    }
    cl->buffer[cl->len++] = c;
    cl->buffer[cl->len]   = '\0';
    return Error::Ok;
}

// ---------------------------------------------------------------------------
// "Ручная коррекция при паузе" (для сверловки плат): пока задание стоит на
// паузе (Hold, движение полностью остановлено), позволяет через Serial/Telnet
// сместить шпиндель командой $MJ=<ось><смещение>...[F<подача>], например:
//   $MJ=X2Y-1.5F200
// Смещения ВСЕГДА относительны текущему (реальному) положению.
//
// Принцип коррекции: станок физически двигается И это движение остаётся в
// трекинге (без "скрытия"/отката sys_position). Когда пауза завершается,
// суммарная поправка за сессию MJ переносится в активную рабочую систему
// координат (G54..G59) — так остаток текущего файла и следующие файлы идут с
// нужным сдвигом, но без рассинхрона между реальной и логической позицией.
//
// Перемещения выполняются как отдельные "парковочные" движения (тот же
// механизм mc_parking_motion(), которым уже пользуется retract/restore
// защитной дверцы) в стороне от обычного буфера планировщика, поэтому уже
// поставленный туда прерванный блок не портится и не меняет порядок
// выполнения.
//
// ВНИМАНИЕ: во время правки шпиндель остаётся в том состоянии, в котором был
// на момент паузы (если он вращался — он продолжит вращаться). При
// необходимости остановите его отдельно перед сверловочной коррекцией.
// ---------------------------------------------------------------------------
static const float MANUAL_ADJUST_DEFAULT_RATE = 300.0;  // мм/мин, если F не указана

static bool    manual_adjust_used_in_hold = false;
static bool    manual_adjust_session_open = false;
static bool    manual_adjust_command_active = false;
static bool    manual_adjust_resume_delay_logged = false;
static int32_t manual_adjust_session_start_steps[MAX_N_AXIS] = { 0 };  // sys_position at first $MJ= in this hold
static int32_t manual_adjust_session_delta_steps[MAX_N_AXIS] = { 0 };  // accumulated REAL steps from executed $MJ moves

// MJ-диагностика: обычный канал + гарантированно USB (в обход фильтра $Message/Level).
static void manual_adjust_logf(const char* format, ...) {
    char    line_buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line_buf, sizeof(line_buf), format, args);
    va_end(args);
    grbl_msg_sendf(CLIENT_ALL, MsgLevel::Info, "%s", line_buf);
    grbl_sendf(CLIENT_SERIAL, "%s\r\n", line_buf);
}

static void manual_adjust_report_hidden_delta(const char* tag) {
    (void)tag;
}

static bool manual_adjust_allowed() {
    // Разрешено только когда задание реально стоит (полная остановка после Hold),
    // а не во время замедления/движения (sys.state == Cycle).
    return sys.state == State::Hold && sys.suspend.bit.holdComplete;
}

// КОРЕНЬ ПРОБЛЕМЫ "после resume пара верных ходов, потом скачок": $MJ физически двигает
// станок и синхронизирует sys_position/gc_state.position (MPos) с реальностью — это верно
// для бухгалтерии, но G-код с АБСОЛЮТНЫМИ координатами (G90, подавляющее большинство файлов)
// переводит целевую точку в MPos как programmed_WPos + coord_system[csys] + coord_offset(G92).
// Если coord_system не сдвинуть на ту же величину, что и физическую позицию, то первая же
// абсолютная команда после resume своим target'ом "утащит" станок назад к НЕисправленной
// координате — внешне выглядит как рывок/уход в сторону после нескольких верных движений
// (это были уже заранее посчитанные в планировщике блоки ДО паузы, которые двигаются как чисто
// относительный шаговый путь и потому корректно едут параллельно вместе со сдвигом).
// Поэтому при выходе из паузы переносим суммарный физический сдвиг за сессию $MJ в активную
// рабочую систему координат (G54..G59) ОДНИМ разом — тогда все дальнейшие абсолютные цели в
// текущем и следующих файлах автоматически считаются уже с поправкой.
static void manual_adjust_apply_wcs_shift_from_session() {
    if (!manual_adjust_session_open) {
        return;
    }
    manual_adjust_session_open = false;
    auto n_axis = number_axis->get();
    if (n_axis > MAX_N_AXIS) {
        n_axis = MAX_N_AXIS;
    }
    bool any_delta = false;
    for (int i = 0; i < n_axis; i++) {
        if (manual_adjust_session_delta_steps[i] != 0) {
            any_delta = true;
            break;
        }
    }
    if (!any_delta) {
        memset(manual_adjust_session_delta_steps, 0, sizeof(manual_adjust_session_delta_steps));
        return;
    }
    float delta_mm[MAX_N_AXIS] = { 0 };
    for (int i = 0; i < n_axis; i++) {
        float spm   = axis_settings[i]->steps_per_mm->get();
        delta_mm[i] = ((float)manual_adjust_session_delta_steps[i]) / spm;
    }
    float wcs[MAX_N_AXIS];
    coords[gc_state.modal.coord_select]->get(wcs);
    for (int i = 0; i < n_axis; i++) {
        wcs[i] += delta_mm[i];
        gc_state.coord_system[i] = wcs[i];
    }
    coords[gc_state.modal.coord_select]->set(wcs);
    // ФИКС "10.02mm вместо 0.2mm" (подтверждено диагностикой PLAN_NEW_BLOCK):
    // Если в момент срабатывания $MJ строка G-кода уже была распарсена (target[] в мм
    // посчитан ДО сдвига WCS), но mc_line() ещё ждёт свободного места в буфере планировщика
    // (см. цикл ожидания в MotionControl.cpp), то к моменту, когда место освободится,
    // pl.position/sys_position уже сдвинуты этим $MJ, а сохранённый target[] — ещё старый.
    // plan_buffer_line() посчитает шаги как (СТАРЫЙ target) - (НОВАЯ база) — получится
    // лишний пробег ровно на величину сдвига. Копим сдвиг здесь; mc_line() применит
    // эту же поправку к своему "зависшему" target[] перед постановкой блока в очередь.
    for (int i = 0; i < n_axis; i++) {
        mc_wcs_shift_accum[i] += delta_mm[i];
    }
    // ВАЖНО: синхронизировать parser/planner с текущим MPos можно только когда очередь
    // планировщика пуста. Если в ней ещё есть недоеханные блоки (часто после паузы в середине
    // длинного перемещения), принудительный gc_sync/plan_sync перезапишет базу расчёта для
    // последующих строк, но сами уже-очередные блоки останутся со старыми целями, что даёт
    // "скачки" после резюма. Поэтому при непустой очереди оставляем parser/planner как есть.
    if (plan_get_current_block() == NULL) {
        gc_sync_position();
        plan_sync_position();
    } else {
        manual_adjust_logf("[MSG:MJ skip_sync queue_not_empty] ");
    }
    manual_adjust_logf("[MSG:MJ apply_wcs_shift d=%.3f,%.3f,%.3f coord=%d]",
                       delta_mm[0],
                       delta_mm[1],
                       delta_mm[2],
                       (int)gc_state.modal.coord_select);
    mc_trace_arm_after_manual_adjust("resume_after_mj", 80);
    memset(manual_adjust_session_delta_steps, 0, sizeof(manual_adjust_session_delta_steps));
}

// Сбрасывает "активную сессию" правки. Вызывается когда пауза завершается
// (любым способом), чтобы следующая пауза начала захват позиции с чистого листа.
static void manual_adjust_end_session() {
    manual_adjust_apply_wcs_shift_from_session();
    manual_adjust_used_in_hold = false;
    manual_adjust_resume_delay_logged = false;
}

// Выполняет одно изолированное перемещение к абсолютной станочной координате,
// блокируясь до завершения. Состояние шпинделя/СОЖ не меняется этим вызовом.
static void manual_adjust_move_to(float* target_mpos, float feed_rate) {
    plan_line_data_t  plan_data;
    plan_line_data_t* pl_data = &plan_data;
    memset(pl_data, 0, sizeof(plan_line_data_t));
    pl_data->motion                = {};
    pl_data->motion.systemMotion   = 1;
    pl_data->motion.noFeedOverride = 1;
    pl_data->feed_rate             = feed_rate;
    pl_data->spindle                = gc_state.modal.spindle;
    pl_data->coolant                = gc_state.modal.coolant;
    pl_data->spindle_speed          = (uint32_t)gc_state.spindle_speed;
    mc_parking_motion(target_mpos, pl_data);
}

// Разбирает и выполняет "$MJ=<ось><число>...[F<число>]" — относительное смещение
// от ТЕКУЩЕГО (реального) положения по осям X/Y/Z (и т.д.), с необязательной подачей F.
// client передаётся только для отметки источника команды в диагностическом логе.
static Error manual_adjust_jog(const char* value, uint8_t client) {
    if (!manual_adjust_allowed()) {
        return Error::IdleError;
    }
    manual_adjust_logf("[MSG:MJ raw client=%d value=\"%s\"]", (int)client, value);
    static const char axis_letters[MAX_N_AXIS] = { 'X', 'Y', 'Z' };
    float   delta[MAX_N_AXIS]     = { 0 };
    bool    axis_seen[MAX_N_AXIS] = { false };
    float   feed_rate             = MANUAL_ADJUST_DEFAULT_RATE;
    uint8_t pos                   = 0;
    auto    n_axis                = number_axis->get();
    if (n_axis > MAX_N_AXIS) {
        n_axis = MAX_N_AXIS;
    }
    while (value[pos] != '\0') {
        char c        = toupper((unsigned char)value[pos]);
        int  axis_idx = -1;
        for (int i = 0; i < n_axis; i++) {
            if (axis_letters[i] == c) {
                axis_idx = i;
                break;
            }
        }
        if (axis_idx >= 0) {
            pos++;
            float f;
            if (!read_float(value, &pos, &f)) {
                manual_adjust_logf("[MSG:MJ error BadNumberFormat axis=%c pos=%d]", c, (int)pos);
                return Error::BadNumberFormat;
            }
            delta[axis_idx]     = f;
            axis_seen[axis_idx] = true;
            continue;
        }
        if (c == 'F') {
            pos++;
            float f;
            if (!read_float(value, &pos, &f) || f <= 0) {
                manual_adjust_logf("[MSG:MJ error BadNumberFormat F pos=%d]", (int)pos);
                return Error::BadNumberFormat;
            }
            feed_rate = f;
            continue;
        }
        manual_adjust_logf("[MSG:MJ error InvalidStatement char='%c' pos=%d]", c, (int)pos);
        return Error::InvalidStatement;
    }
    bool any_axis = false;
    for (int i = 0; i < n_axis; i++) {
        if (axis_seen[i]) {
            any_axis = true;
        }
    }
    if (!any_axis) {
        return Error::InvalidStatement;
    }
    if (!manual_adjust_session_open) {
        memcpy(manual_adjust_session_start_steps, sys_position, sizeof(int32_t) * MAX_N_AXIS);
        memset(manual_adjust_session_delta_steps, 0, sizeof(manual_adjust_session_delta_steps));
        manual_adjust_session_open = true;
    }
    manual_adjust_used_in_hold = true;
    float   target[MAX_N_AXIS];
    int32_t target_steps[MAX_N_AXIS];
    memcpy(target, system_get_mpos(), sizeof(float) * MAX_N_AXIS);
    memcpy(target_steps, sys_position, sizeof(int32_t) * MAX_N_AXIS);
    for (int i = 0; i < n_axis; i++) {
        if (!axis_seen[i]) {
            continue;
        }
        target[i] += delta[i];
        float steps_per_mm = axis_settings[i]->steps_per_mm->get();
        target_steps[i] = lround(target[i] * steps_per_mm);
    }
    manual_adjust_logf("[MSG:MJ parsed dX=%.3f(%d) dY=%.3f(%d) dZ=%.3f(%d) F=%.1f from=%.3f,%.3f,%.3f to=%.3f,%.3f,%.3f]",
                       delta[0],
                       (int)axis_seen[0],
                       delta[1],
                       (int)axis_seen[1],
                       delta[2],
                       (int)axis_seen[2],
                       feed_rate,
                       system_get_mpos()[0],
                       system_get_mpos()[1],
                       system_get_mpos()[2],
                       target[0],
                       target[1],
                       target[2]);
    if (soft_limits->get() && limitsCheckTravel(target)) {
        manual_adjust_logf("[MSG:MJ error TravelExceeded]");
        return Error::TravelExceeded;
    }
    manual_adjust_command_active = true;
    // ВАЖНО: корректируем реальную позицию станка и затем синхронизируем parser/planner
    // с новой физической точкой. Это делает MJ обычной ручной подстройкой, а не скрытым
    // смещением.
    int32_t mj_before_steps[MAX_N_AXIS];
    memcpy(mj_before_steps, sys_position, sizeof(int32_t) * MAX_N_AXIS);
    st_debug_dump_prep("MJ prep_before_move");
    manual_adjust_move_to(target, feed_rate);
    st_debug_dump_prep("MJ prep_after_move");
    int32_t mj_after_steps[MAX_N_AXIS];
    memcpy(mj_after_steps, sys_position, sizeof(int32_t) * MAX_N_AXIS);
    if (manual_adjust_session_open) {
        for (int i = 0; i < n_axis; i++) {
            manual_adjust_session_delta_steps[i] += (mj_after_steps[i] - mj_before_steps[i]);
        }
    }
    manual_adjust_logf("[MSG:MJ steps before=(%ld,%ld,%ld) target=(%ld,%ld,%ld) after=(%ld,%ld,%ld) d=(%ld,%ld,%ld)]",
                       (long)mj_before_steps[0],
                       (long)mj_before_steps[1],
                       (long)mj_before_steps[2],
                       (long)target_steps[0],
                       (long)target_steps[1],
                       (long)target_steps[2],
                       (long)mj_after_steps[0],
                       (long)mj_after_steps[1],
                       (long)mj_after_steps[2],
                       (long)(mj_after_steps[0] - mj_before_steps[0]),
                       (long)(mj_after_steps[1] - mj_before_steps[1]),
                       (long)(mj_after_steps[2] - mj_before_steps[2]));
    // НАСТОЯЩАЯ ПРИЧИНА "несколько верных ходов, потом скачок, потом снова нормально":
    // gc_state.position и pl.position — это НЕ "текущая физическая позиция". Это позиция
    // ПОСЛЕ ПОСЛЕДНЕГО УЖЕ ПОСТАВЛЕННОГО В ОЧЕРЕДЬ БЛОКА из файла (она проставляется сразу
    // при постановке блока в очередь, а не по факту физического прохождения). Если в момент
    // паузы в очереди планировщика оставался не один блок, а несколько (обычная ситуация —
    // main-loop успевает поставить в очередь несколько строк файла, пока идёт замедление),
    // то gc_state.position/pl.position указывают на КОНЕЦ последнего из них — а физически
    // степпер стоит где-то РАНЬШЕ по этой цепочке (соответствует rem=... в resume-логах).
    // Раньше здесь стояли gc_sync_position()/plan_sync_position() — они принудительно
    // затирали gc_state.position/pl.position ТЕКУЩЕЙ физической позицией (sys_position),
    // разрушая эту "позицию после очереди". Уже поставленные в очередь блоки это не портит
    // (у них abs target_steps и относительные Bresenham-шаги уже зафиксированы — потому и
    // едут корректно, "несколько верных ходов" в логах) — но ПЕРВЫЙ НОВЫЙ блок из
    // продолжающегося файла считает свою цель/дистанцию именно от gc_state.position/
    // pl.position, и получает НЕВЕРНОЕ значение → тот самый "скачок" (размер зависит от
    // того, сколько блоков было в очереди на момент паузы — то самое "иногда пара мм, иногда
    // 50-100мм"). Поэтому больше НЕ трогаем gc_state.position/pl.position здесь — они
    // корректно переживают всю MJ-сессию без вмешательства, как и предусмотрено тем, что
    // plan_buffer_line() сам не обновляет pl.position для systemMotion-блоков (см. Planner.cpp).
    manual_adjust_command_active = false;
    manual_adjust_logf("[MSG:MJ done reported_mpos=%.3f,%.3f,%.3f]", system_get_mpos()[0], system_get_mpos()[1], system_get_mpos()[2]);
    return Error::Ok;
}

// Считывает строки от клиентов (Serial/Telnet/...) прямо во время паузы, пока
// основной protocol_main_loop() заблокирован внутри protocol_exec_rt_suspend().
// Распознаёт только "$MJ=..."; всё остальное отклоняется отдельной ошибкой,
// чтобы исключить случайное исполнение G-кода мимо обычных проверок.
static void manual_adjust_poll_clients() {
    if (!manual_adjust_allowed()) {
        return;
    }
    int c;
    for (uint8_t client = 0; client < CLIENT_COUNT; client++) {
        while ((c = client_read(client)) != -1) {
            Error res = add_char_to_line((char)c, client);
            if (res == Error::Eol) {
                char* ln = client_lines[client].buffer;
                Error result;
                if (ln[0] == '\0') {
                    result = Error::Ok;
                } else if (!strncmp(ln, "$MJ=", 4)) {
                    result = manual_adjust_jog(ln + 4, client);
                } else {
                    result = Error::AnotherInterfaceBusy;
                }
                // IMPORTANT: Do not use report_status_message() here.
                // During SD printing, report_status_message(error) closes the SD file
                // and reports "error:... in SD file at line ...", even when this error
                // actually came from a side-channel pause command.
                if (result == Error::Ok) {
                    grbl_send(client, "ok\r\n");
                } else {
                    grbl_sendf(client, "error:%d\r\n", static_cast<int>(result));
                }
                empty_line(client);
            } else if (res == Error::Overflow) {
                grbl_sendf(client, "error:%d\r\n", static_cast<int>(Error::Overflow));
                empty_line(client);
            }
        }
    }
}

Error execute_line(char* line, uint8_t client, WebUI::AuthenticationLevel auth_level) {
    Error result = Error::Ok;
    // Empty or comment line. For syncing purposes.
    if (line[0] == 0) {
        return Error::Ok;
    }

    // Grbl '$' or WebUI '[ESPxxx]' system command
    if (line[0] == '$' || line[0] == '[') {
        return system_execute_line(line, client, auth_level);
    }
    // Everything else is gcode. Block if in alarm or jog mode.
    if (sys.state == State::Alarm || sys.state == State::Jog) {
        return Error::SystemGcLock;
    }
    return gc_execute_line(line, client);
}

bool can_park() {
    return
#ifdef ENABLE_PARKING_OVERRIDE_CONTROL
        sys.override_ctrl == Override::ParkingMotion &&
#endif
        homing_enable->get() && !spindle->inLaserMode();
}

/*
  GRBL PRIMARY LOOP:
*/
void protocol_main_loop() {

    static bool first_restart = true;
    const char re_cmd[] = {0x18, '\0'}; // Ctrl-X reset command as null-terminated byte string

    client_reset_read_buffer(CLIENT_ALL);
    empty_lines();
    //uint8_t client = CLIENT_SERIAL; // default client
    // Perform some machine checks to make sure everything is good to go.
#ifdef CHECK_LIMITS_AT_INIT
    if (hard_limits->get()) {
        if (limits_get_state()) {
            sys.state = State::Alarm;  // Ensure alarm state is active.
            report_feedback_message(Message::CheckLimits);
        }
    }
#endif
    // Check for and report alarm state after a reset, error, or an initial power up.
    // NOTE: Sleep mode disables the stepper drivers and position can't be guaranteed.
    // Re-initialize the sleep state as an ALARM mode to ensure user homes or acknowledges.
    if (sys.state == State::Alarm || sys.state == State::Sleep) {
        report_feedback_message(Message::AlarmLock);
        sys.state = State::Alarm;  // Ensure alarm state is set.
    } else {
        // Check if the safety door is open.
        sys.state = State::Idle; 
        if (system_check_safety_door_ajar()) {
            sys_rt_exec_state.bit.safetyDoor = true;
            protocol_execute_realtime();  // Enter safety door mode. Should return as IDLE state.
        }
        // All systems go!
        system_execute_startup(line);  // Execute startup script.
    }
    // ---------------------------------------------------------------------------------
    // Primary loop! Upon a system abort, this exits back to main() to reset the system.
    // This is also where Grbl idles while waiting for something to do.
    // ---------------------------------------------------------------------------------
    MKS_GRBL_CMD_SEND("$x\n");   // 主动解锁

    if(first_restart == true) {
      MKS_GRBL_CMD_SEND(re_cmd);  
      first_restart = false; 
    }
    
    int c;
    bool is_need_next = false;
    for (;;) {
#if 1
#ifdef ENABLE_SD_CARD
        if (SD_ready_next) {
            char fileLine[255];
                if (readFileLine(fileLine, 255)) {
                    SD_ready_next = false;
                    // ДИАГНОСТИКА (ищем причину чередования shifted/unshifted координат круга):
                    // печатаем КАЖДУЮ прочитанную с SD строку с её номером и текущим coord_system
                    // сдвигом, чтобы увидеть, не читается/выполняется ли какая-то строка повторно
                    // или не в том порядке относительно момента apply_wcs_shift.
                    // ВАЖНО: некоторые файлы (в т.ч. из реальных CAM-программ) используют "\r" без
                    // "\n" как разделитель строк. readFileLine() отдаёт fileLine БЕЗ этого "\r" в
                    // конце (сам разделитель), но если внутри строки случайно остался "\r" (или
                    // предыдущая строка была прочитана с хвостовым "\r" из-за особенностей CR-only
                    // файла), он ломает вывод лога построчно. Заменяем ЛЮБЫЕ control-символы внутри
                    // строки на пробел перед печатью — на исполнение G-кода это не влияет (это
                    // только для логирования), но делает вывод читаемым построчно.
                    {
                        char clean_line[255];
                        size_t clean_len = 0;
                        for (const char* p = fileLine; *p != '\0' && clean_len < sizeof(clean_line) - 1; p++) {
                            clean_line[clean_len++] = ((unsigned char)*p < 0x20) ? ' ' : *p;
                        }
                        clean_line[clean_len] = '\0';
                        grbl_sendf(CLIENT_SERIAL,
                                   "[MSG:SD_LINE #%u wcs_x=%.3f line=\"%s\"]\r\n",
                                   (unsigned)sd_get_current_line_number(),
                                   gc_state.coord_system[0],
                                   clean_line);
                    }
                    report_status_message(execute_line(fileLine, SD_client, SD_auth_level), SD_client);
                } 
                else {
                    if(mks_grbl.carve_times != 0) mks_grbl.carve_times--;

                    if(mks_grbl.carve_times > 0) {
                        setFilePos(0);
                        grbl_sendf(CLIENT_SERIAL , "times:%d\n", mks_grbl.carve_times);
                    }else {
                        char temp[128];
                        sd_get_current_filename(temp, sizeof(temp));
                        if (mks_grbl.is_mks_ts35_flag == true) { 
                            mks_ui_page.mks_ui_page = MKS_UI_PAGE_LOADING;
                            mks_ui_page.wait_count = DEFAULT_UI_COUNT;
                            mks_draw_finsh_pupop(); // show print finsh 
                        }
                        grbl_notifyf("SD print done", "%s print is successful", temp);
                        grbl_send(CLIENT_ALL, "SD Print Finish!\n");
                        // MKS_GRBL_CMD_SEND("G0 X0 Y0 Z0 F300\n");

                        sys_rt_f_override                    = FeedOverride::Default;
                        sys_rt_r_override                    = RapidOverride::Default;
                        sys_rt_s_override                    = SpindleSpeedOverride::Default;

                        closeFile();  // close file and clear SD ready/running flags
                    }
                }
        }
#endif
#else
#ifdef ENABLE_SD_CARD
        if (SD_ready_next) {
            char fileLine[255];
            if (readFileLine(fileLine, 255)) {
                if (is_rb_empty(&rb_sd) == true) {
                    rb_write(&rb_sd, fileLine);
                }
                SD_ready_next = false;
                rb_read(&rb_sd, fileLine);
                report_status_message(execute_line(fileLine, SD_client, SD_auth_level), SD_client);
            } 
            else {
                char temp[128];
                sd_get_current_filename(temp, sizeof(temp));
                if (mks_grbl.is_mks_ts35_flag == true) { 
                    mks_ui_page.mks_ui_page = MKS_UI_PAGE_LOADING;
                    mks_ui_page.wait_count = DEFAULT_UI_COUNT;
                    mks_draw_finsh_pupop(); // show print finsh 
                }
                grbl_notifyf("SD print done", "%s print is successful", temp);
                closeFile();  // close file and clear SD ready/running flags
            }
        }
        else {
                if((sys.state == State::Cycle)) {  
                    if(is_rb_full(&rb_sd) == false) {
                        char fileLine[255];
                        if (readFileLine(fileLine, 255)) {
                            rb_write(&rb_sd, fileLine);
                        }
                    }
                }
        }
#endif
#endif
        // Receive one line of incoming serial data, as the data becomes available.
        // Filtering, if necessary, is done later in gc_execute_line(), so the
        // filtering is the same with serial and file input.
        uint8_t client = CLIENT_SERIAL;
        char*   line;
        for (client = 0; client < CLIENT_COUNT; client++) {
            while ((c = client_read(client)) != -1) {
                Error res = add_char_to_line(c, client);
                switch (res) {
                    case Error::Ok:
                        break;
                    case Error::Eol:
                        protocol_execute_realtime();  // Runtime command check point.
                        if (sys.abort) {
                            return;  // Bail to calling function upon system abort
                        }
                        line = client_lines[client].buffer;
#ifdef REPORT_ECHO_RAW_LINE_RECEIVED
                        report_echo_line_received(line, client);
#endif
#ifdef ENABLE_SD_CARD
                        // Пока с SD идёт задание, строки со 2-го интерфейса (USB/сеть) не должны
                        // влиять на SD-поток. report_status_message() при BusyPrinting завязан на SD
                        // (Ok -> следующая строка файла; ошибка -> closeFile/ОБРЫВ печати), поэтому
                        // обрабатываем их здесь и квитируем НАПРЯМУЮ, минуя его: read-only запросы
                        // ($$/$#/$G/$I/$N) выполняем, остальное (движение/исполнение) отклоняем.
                        if (get_sd_state(false) >= SDState::Busy) {
                            Error sd_gate_err = line_safe_during_sd_job(line)
                                ? execute_line(line, client, WebUI::AuthenticationLevel::LEVEL_GUEST)
                                : Error::AnotherInterfaceBusy;
                            if (sd_gate_err == Error::Ok) {
                                grbl_send(client, "ok\r\n");
                            } else {
                                grbl_sendf(client, "error:%d\r\n", static_cast<int>(sd_gate_err));
                            }
                            empty_line(client);
                            break;
                        }
#endif
                        // auth_level can be upgraded by supplying a password on the command line
                        report_status_message(execute_line(line, client, WebUI::AuthenticationLevel::LEVEL_GUEST), client);
                        empty_line(client);
                        break;
                    case Error::Overflow:
                        report_status_message(Error::Overflow, client);
                        empty_line(client);
                        break;
                    default:
                        break;
                }
            }  // while serial read
        }      // for clients
        // If there are no more characters in the serial read buffer to be processed and executed,
        // this indicates that g-code streaming has either filled the planner buffer or has
        // completed. In either case, auto-cycle start, if enabled, any queued moves.
        protocol_auto_cycle_start();
        protocol_execute_realtime();  // Runtime command check point.
        if (sys.abort) {
            return;  // Bail to main() program loop to reset system.
        }

        #ifdef ENABLE_EXTERNAL_BOARD
            ext_board.handle();
        #endif
        // check to see if we should disable the stepper drivers ... esp32 work around for disable in main loop.
        if (stepper_idle && stepper_idle_lock_time->get() != 0xff) {
            
            // Отключать драйверы только когда станок реально простаивает — НЕ в Cycle и НЕ в Hold.
            // (Было `||` -> тавтология «всегда истина»: моторы отключались и на паузе Hold,
            //  теряя удерживающий момент; ось, особенно Z, могла уехать.)
            if((sys.state != State::Cycle) && (sys.state != State::Hold)) {

                if (esp_timer_get_time() > stepper_idle_counter) {
                    motors_set_disable(true);
                }
            }
        }
        // spindle_check();
        if(mks_ui_page.mks_ui_page == MKS_UI_Wifi) {   
            #if defined(ENABLE_WIFI)
            mks_wifi_connect(wifi_send_username, wifi_send_password);   // 扫描wifi是否需要被发送指令连接
            #endif
        }
    }
    return; /* Never reached */
}

// Безопасно снимает "зависший" текущий блок планировщика с очереди — используется и в
// protocol_buffer_synchronize(), и в cycle_stop-ветке protocol_exec_rt_system() ниже (см.

// ПОПЫТКА "БЕСШОВНОГО" САМОВОССТАНОВЛЕНИЯ (по просьбе пользователя: "исправлять на лету, чтобы
// было незаметно"): вызывается ПЕРЕД discard_stale_planner_block() из cycle_stop-ветки. Идея:
// когда ISR ставит cycle_stop, потому что буфер СЕГМЕНТОВ опустел, module-static состояние
// подготовки в Stepper.cpp (pl_block/prep) НЕ сбрасывается st_go_idle() — оно продолжает
// указывать на тот же самый недоеденный блок планировщика с тем же прогрессом (dt_remainder
// и т.п.). Это ЗНАЧИТ, что можно буквально повторить те же самые действия, которые Grbl делает
// при обычном старте цикла (см. cycleStart-ветку выше: `st_prep_buffer(); st_wake_up();`), и
// движение продолжится СО ТОЧКИ, ГДЕ РЕАЛЬНО ОСТАНОВИЛСЯ СТЕППЕР — то есть подготовит сегменты
// для оставшейся дистанции текущего блока и снова включит таймер/моторы. Внешне (для оператора
// и для файла) это будет выглядеть как обычное продолжение сверления/движения без видимого сбоя.
// Возвращает true, если была предпринята содержательная попытка резюме (были реальные mm to go)
// — тогда discard_stale_planner_block() НЕ вызывается, работа продолжается. Возвращает false,
// если восстанавливать было нечего (millimeters ~ 0) — тогда как и раньше используется тихий
// discard. Мы НЕ можем на 100% гарантировать, что резюме сработает (напр. если сама причина
// задержки ещё активна прямо сейчас) — но именно поэтому alarm-путь в discard_stale_planner_block()
// остаётся как надёжный fallback: если резюме не помогло, на СЛЕДУЮЩЕМ cycle_stop (буфер сегментов
// снова опустеет от того же блока, либо сработает 12с self-recovery в protocol_buffer_synchronize())
// сработает уже он.
static bool try_resume_stale_block(const char* context) {
    // ПОВТОРНО ВКЛЮЧЕНО по просьбе пользователя: коренная причина "10.02mm вместо 0.2mm"
    // (target[] в mc_line() оставался в СТАРОЙ системе координат, пока $MJ сдвигал WCS во
    // время ожидания места в буфере планировщика — см. фикс в MotionControl.cpp mc_line()
    // и mc_wcs_shift_accum в System.h/Protocol.cpp) теперь устранена и подтверждена. Именно
    // эта гонка и приводила к ложным алармам здесь (стабильный/устаревший блок с "лишним"
    // остатком ровно на величину сдвига). Раньше эта функция была отключена (safety-first),
    // т.к. до фикса выше resume "устаревшего" блока молча доезжал этот лишний, ошибочный
    // остаток. Теперь просто возобновляем прерванный decel-блок напрямую (минуя обычный
    // cycleStart/разгон) — если он всё-таки останется "зависшим", основной alarm-путь в
    // discard_stale_planner_block() ниже сработает как и раньше (fallback не убран).
    plan_block_t* block = plan_get_current_block();
    if (!block || block->millimeters <= 0.01f) {
        return false;
    }
    manual_adjust_logf("%s auto-resume: resuming stale block rem=%.3fmm", context, block->millimeters);
    sys.state = State::Cycle;
    st_wake_up();
    return true;
}


// комментарии там). КРИТИЧНО: тихо discard'ить безопасно ТОЛЬКО когда у блока не осталось
// реальной непройденной дистанции (millimeters ~ 0, т.е. степпер физически докрутил весь
// путь, просто бухгалтерия планировщика не успела вовремя это оформить). Если дистанция
// заметно больше нуля — значит степпер физически НЕ проехал остаток пути (сегменты для
// него так и не были сгенерированы до того, как ISR объявил буфер сегментов пустым), а
// gc_state.position/pl.position/отчёт координат УЖЕ считают цель достигнутой (парсер
// проставляет их сразу при постановке в очередь, не дожидаясь физического исполнения).
// Тихий discard в этом случае оставляет РЕАЛЬНУЮ физическую позицию станка (sys_position)
// отставшей от того, что показывает и на чём считает вся остальная программа — именно
// так проявился баг "сверлит в воздухе, не доезжая до заготовки на 5-6мм": короткий
// быстрый G0/G1 по Z был недовыполнен физически, но помечен как выполненный, и все
// последующие относительные Z-цели в файле считались от неверной точки отсчёта, пока
// пользователь не перепробировал высоту (что заново синхронизирует sys_position). Поэтому
// при заметном остатке — не продолжаем молча, а глушим шпиндель/СОЖ и поднимаем тревогу,
// чтобы оператор заметил проблему и заново взял референс, а не сверлил вслепую.
//
// Максимально подробная диагностика (по просьбе пользователя, т.к. проблема стала повторяться
// чаще): логируем и в "тихом" (millimeters~0), и в "реальная потеря движения" случаях —
// разница по осям между РЕАЛЬНОЙ физической позицией (system_get_mpos(), из sys_position/ISR)
// и позицией, которую считает достигнутой парсер (gc_state.position) — это и есть прямое
// числовое подтверждение/измерение рассинхрона, если он произошёл. Плюс параметры блока
// (запрограммированная/максимальная скорость), состояние системы, SD-файл/строка, свободная
// куча (heap) — просадки кучи часто коррелируют с WiFi/LVGL-активностью, которая, по гипотезе,
// и задерживает st_prep_buffer() настолько, что возникает эта гонка — и аптайм для сопоставления
// с прочими логами/временем на часах станка.
static void discard_stale_planner_block(const char* context) {
    plan_block_t* stale_block = plan_get_current_block();
    if (!stale_block) {
        return;
    }
    // Копируем сразу — system_get_mpos() возвращает указатель на статический буфер,
    // переиспользуемый при каждом вызове.
    float real_mpos[MAX_N_AXIS];
    memcpy(real_mpos, system_get_mpos(), sizeof(float) * MAX_N_AXIS);
    auto n_axis = number_axis->get();
    if (n_axis > MAX_N_AXIS) {
        n_axis = MAX_N_AXIS;
    }
    char axes_diag[MAX_N_AXIS * 40];
    axes_diag[0] = '\0';
    static const char axis_letters[MAX_N_AXIS] = { 'X', 'Y', 'Z' };
    for (int i = 0; i < n_axis; i++) {
        char one[48];
        snprintf(one,
                 sizeof(one),
                 "%s%c:real=%.3f,parser=%.3f,diff=%.3f",
                 i ? " " : "",
                 axis_letters[i],
                 real_mpos[i],
                 gc_state.position[i],
                 gc_state.position[i] - real_mpos[i]);
        strncat(axes_diag, one, sizeof(axes_diag) - strlen(axes_diag) - 1);
    }
    // Отправка по USB (CLIENT_SERIAL) намеренно ДУБЛИРУЕТСЯ отдельным raw-вызовом
    // grbl_sendf() ниже, а не только через grbl_msg_sendf(CLIENT_ALL, ...). Причина:
    // grbl_msg_sendf() САМ фильтрует по текущей настройке $Message/Level (по умолчанию
    // Info, но могла быть понижена, в т.ч. случайно через WebUI) — если уровень окажется
    // ниже MsgLevel::Info, сообщение НЕ уйдёт вообще никуда, включая USB. Пользователь
    // сообщил, что проблема часто совпадает с подключением/обновлением WebUI во время
    // выполнения файла — это ФАКТИЧЕСКИЙ ПОДОЗРЕВАЕМЫЙ ТРИГГЕР самой гонки (WebUI-запросы
    // грузят main-loop и могут задерживать st_prep_buffer(), что и приводит к рассинхрону),
    // а не просто совпадение по каналу вывода. Поэтому для этих диагностических сообщений
    // критично гарантировать вывод на USB НЕЗАВИСИМО от $Message/Level и от того, что в
    // это же самое время происходит с WebUI/WiFi-клиентами.
    char line_buf[MAX_N_AXIS * 40 + 160];
    snprintf(line_buf,
             sizeof(line_buf),
             "%s stale_block rem=%.3fmm prog_rate=%.1f rapid_rate=%.1f state=%d spindle_state=%d t=%lums heap=%u/%u",
             context,
             stale_block->millimeters,
             stale_block->programmed_rate,
             stale_block->rapid_rate,
             static_cast<int>(sys.state),
             static_cast<int>(spindle->get_state()),
             (unsigned long)millis(),
             (unsigned)ESP.getFreeHeap(),
             (unsigned)xPortGetMinimumEverFreeHeapSize());
    grbl_msg_sendf(CLIENT_ALL, MsgLevel::Info, "%s", line_buf);
    grbl_sendf(CLIENT_SERIAL, "[MSG:%s]\r\n", line_buf);  // guaranteed on USB, bypasses $Message/Level

    snprintf(line_buf, sizeof(line_buf), "%s axes %s", context, axes_diag);
    grbl_msg_sendf(CLIENT_ALL, MsgLevel::Info, "%s", line_buf);
    grbl_sendf(CLIENT_SERIAL, "[MSG:%s]\r\n", line_buf);
#ifdef ENABLE_SD_CARD
    snprintf(line_buf,
             sizeof(line_buf),
             "%s sd_state=%d sd_line=%u",
             context,
             static_cast<int>(get_sd_state(false)),
             (unsigned)sd_get_current_line_number());
    grbl_msg_sendf(CLIENT_ALL, MsgLevel::Info, "%s", line_buf);
    grbl_sendf(CLIENT_SERIAL, "[MSG:%s]\r\n", line_buf);
#endif
    if (stale_block->millimeters > 0.01f) {
        snprintf(line_buf,
                 sizeof(line_buf),
                 "%s lost motion %.3fmm not executed - alarm instead of silent continue",
                 context,
                 stale_block->millimeters);
        grbl_msg_sendf(CLIENT_ALL, MsgLevel::Info, "%s", line_buf);
        grbl_sendf(CLIENT_SERIAL, "[MSG:%s]\r\n", line_buf);
        spindle->stop();
        coolant_stop();
        sys_rt_exec_alarm = ExecAlarm::AbortCycle;
    } else {
        snprintf(line_buf, sizeof(line_buf), "%s stale block discarded (fully executed)", context);
        grbl_msg_sendf(CLIENT_ALL, MsgLevel::Info, "%s", line_buf);
        grbl_sendf(CLIENT_SERIAL, "[MSG:%s]\r\n", line_buf);
    }
    plan_discard_current_block();
}

// Block until all buffered steps are executed or in a cycle state. Works with feed hold
// during a synchronize call, if it should happen. Also, waits for clean cycle end.
void protocol_buffer_synchronize() {
    // If system is queued, ensure cycle resumes if the auto start flag is present.
    protocol_auto_cycle_start();
    // Диагностика редкого зависания при выполнении файла (напр. станок «висит» на M3/M4 —
    // spindle->sync() ждёт тут завершения предыдущего блока движения, которое почему-то не
    // приходит). Раньше это молча зависало навсегда без единой строчки в логе, что делало
    // проблему недиагностируемой. Если ожидание длится дольше порога — один раз пишем в лог
    // состояние (sys.state, есть ли текущий блок в планировщике, номер строки SD-файла), это
    // НЕ прерывает ожидание и не меняет поведение — просто даёт зацепку в логе при следующем
    // повторении. Порог короче времени задержки watchdog'а намеренно не выбирается — сам цикл
    // ожидания не меняется.
    const uint32_t STALL_WARN_MS    = 8000;
    const uint32_t STALL_RECOVER_MS = 12000;
    uint32_t       sync_start_ms    = millis();
    bool           stall_logged     = false;
    bool           stall_recovered  = false;
    do {
        // Re-assert auto-start while waiting: in rare races we can be left Idle with
        // queued blocks and a cleared cycleStart bit.
        protocol_auto_cycle_start();
        protocol_execute_realtime();  // Check and execute run-time commands
        if (sys.abort) {
            return;  // Check for system abort
        }
        uint32_t elapsed_ms = millis() - sync_start_ms;
        if (!stall_logged && elapsed_ms > STALL_WARN_MS) {
            stall_logged = true;
#ifdef ENABLE_SD_CARD
            grbl_msg_sendf(CLIENT_ALL,
                           MsgLevel::Info,
                           "buffer_synchronize stall >%lums state=%d block=%s sd_state=%d sd_line=%u",
                           (unsigned long)STALL_WARN_MS,
                           static_cast<int>(sys.state),
                           plan_get_current_block() ? "yes" : "no",
                           static_cast<int>(get_sd_state(false)),
                           (unsigned)sd_get_current_line_number());
#else
            grbl_msg_sendf(CLIENT_ALL,
                           MsgLevel::Info,
                           "buffer_synchronize stall >%lums state=%d block=%s",
                           (unsigned long)STALL_WARN_MS,
                           static_cast<int>(sys.state),
                           plan_get_current_block() ? "yes" : "no");
#endif
        }
        // Self-recovery for a stale planner head: if we're already Idle but the planner
        // still reports a current block for too long, that block is non-executable stale state.
        // Try the seamless on-the-fly resume first (see try_resume_stale_block()); only if that's
        // not applicable/didn't apply (block already fully done) does discard_stale_planner_block()
        // decide whether it's safe to silently drop or whether it's real lost motion requiring an alarm.
        if (!stall_recovered && elapsed_ms > STALL_RECOVER_MS && sys.state == State::Idle && plan_get_current_block() &&
            gc_state.modal.motion != Motion::ProbeToward && gc_state.modal.motion != Motion::ProbeTowardNoError &&
            gc_state.modal.motion != Motion::ProbeAway && gc_state.modal.motion != Motion::ProbeAwayNoError) {
            stall_recovered = true;
            if (!try_resume_stale_block("buffer_synchronize")) {
                discard_stale_planner_block("buffer_synchronize");
            }
        }
    } while (plan_get_current_block() || (sys.state == State::Cycle));
}

// Auto-cycle start triggers when there is a motion ready to execute and if the main program is not
// actively parsing commands.
// NOTE: This function is called from the main loop, buffer sync, and mc_line() only and executes
// when one of these conditions exist respectively: There are no more blocks sent (i.e. streaming
// is finished, single commands), a command that needs to wait for the motions in the buffer to
// execute calls a buffer sync, or the planner buffer is full and ready to go.
void protocol_auto_cycle_start() {
    if (plan_get_current_block() != NULL) {       // Check if there are any blocks in the buffer.
        sys_rt_exec_state.bit.cycleStart = true;  // If so, execute them!
    }
}

// This function is the general interface to Grbl's real-time command execution system. It is called
// from various check points in the main program, primarily where there may be a while loop waiting
// for a buffer to clear space or any point where the execution time from the last check point may
// be more than a fraction of a second. This is a way to execute realtime commands asynchronously
// (aka multitasking) with grbl's g-code parsing and planning functions. This function also serves
// as an interface for the interrupts to set the system realtime flags, where only the main program
// handles them, removing the need to define more computationally-expensive volatile variables. This
// also provides a controlled way to execute certain tasks without having two or more instances of
// the same task, such as the planner recalculating the buffer upon a feedhold or overrides.
// NOTE: The sys_rt_exec_state.bit variable flags are set by any process, step or serial interrupts, pinouts,
// limit switches, or the main program.
void protocol_execute_realtime() {
    protocol_exec_rt_system();
    if (sys.suspend.value) {
        protocol_exec_rt_suspend();
    }
}

// Executes run-time commands, when required. This function primarily operates as Grbl's state
// machine and controls the various real-time features Grbl has to offer.
// NOTE: Do not alter this unless you know exactly what you are doing!
void protocol_exec_rt_system() {
    ExecAlarm alarm = sys_rt_exec_alarm;  // Temp variable to avoid calling volatile multiple times.
    if (alarm != ExecAlarm::None) {       // Enter only if an alarm is pending
        // System alarm. Everything has shutdown by something that has gone severely wrong. Report
        // the source of the error to the user. If critical, Grbl disables by entering an infinite
        // loop until system reset/abort.
        sys.state = State::Alarm;  // Set system alarm state
        report_alarm_message(alarm);
        // Halt everything upon a critical event flag. Currently hard and soft limits flag this.
        if ((alarm == ExecAlarm::HardLimit) || (alarm == ExecAlarm::SoftLimit)) {
            // report_feedback_message(Message::CriticalEvent);

            // if(ui_move_ctrl.limit_dis_delay_count == 2) {
            // НЕ строить LVGL-виджет из protocol-task: одновременная мутация дерева LVGL
            // с lv_task_handler (LVGL-задача) рушит его -> краш в момент лимита во время
            // задания. Ставим флаг; попап строит LVGL-задача (mks_page_data_updata).
            if(alarm == ExecAlarm::HardLimit) {
                if(mks_ui_page.mks_ui_page != MKS_UI_TEST) {
                    mks_grbl.pending_limit_popup = 1;
                }

            }else if(alarm == ExecAlarm::SoftLimit) {
                if(mks_ui_page.mks_ui_page != MKS_UI_TEST) {
                    mks_grbl.pending_limit_popup = 2;
                }
            }
            
                // ui_move_ctrl.limit_dis_delay_count = 0;
            // }
            // ui_move_ctrl.limit_dis_delay_count++;
            
            // sys_rt_exec_state.bit.reset = false;  // Disable any existing reset
            // do {  // mks limit disable
            //     // Block everything, except reset and status reports, until user issues reset or power
            //     // cycles. Hard limits typically occur while unattended or not paying attention. Gives
            //     // the user and a GUI time to do what is needed before resetting, like killing the
            //     // incoming stream. The same could be said about soft limits. While the position is not
            //     // lost, continued streaming could cause a serious crash if by chance it gets executed.
            // } while (!sys_rt_exec_state.bit.reset);
        }
        sys_rt_exec_alarm = ExecAlarm::None;
    }
    ExecState rt_exec_state;
    rt_exec_state.value = sys_rt_exec_state.value;  // Copy volatile sys_rt_exec_state.
    if (rt_exec_state.value != 0 || cycle_stop) {   // Test if any bits are on
        // Execute system abort.
        if (rt_exec_state.bit.reset) {
            sys.abort = true;  // Only place this is set true.
            return;            // Nothing else to do but exit.
        }
        // Execute and serial print status
        if (rt_exec_state.bit.statusReport) {
            report_realtime_status(CLIENT_ALL);
            sys_rt_exec_state.bit.statusReport = false;
        }
        // NOTE: Once hold is initiated, the system immediately enters a suspend state to block all
        // main program processes until either reset or resumed. This ensures a hold completes safely.
        if (rt_exec_state.bit.motionCancel || rt_exec_state.bit.feedHold || rt_exec_state.bit.safetyDoor || rt_exec_state.bit.sleep) {
            // State check for allowable states for hold methods.
            if (!(sys.state == State::Alarm || sys.state == State::CheckMode)) {
                // If in CYCLE or JOG states, immediately initiate a motion HOLD.
                if (sys.state == State::Cycle || sys.state == State::Jog) {
                    if (!(sys.suspend.bit.motionCancel || sys.suspend.bit.jogCancel)) {  // Block, if already holding.
                        st_update_plan_block_parameters();  // Notify stepper module to recompute for hold deceleration.
                        sys.step_control             = {};
                        sys.step_control.executeHold = true;  // Initiate suspend state with active flag.
                        if (sys.state == State::Jog) {        // Jog cancelled upon any hold event, except for sleeping.
                            if (!rt_exec_state.bit.sleep) {
                                sys.suspend.bit.jogCancel = true;
                            }
                        }
                    }
                }
                // If IDLE, Grbl is not in motion. Simply indicate suspend state and hold is complete.
                if (sys.state == State::Idle) {
                    sys.suspend.value            = 0;
                    sys.suspend.bit.holdComplete = true;
                }
                // Execute and flag a motion cancel with deceleration and return to idle. Used primarily by probing cycle
                // to halt and cancel the remainder of the motion.
                if (rt_exec_state.bit.motionCancel) {
                    // MOTION_CANCEL only occurs during a CYCLE, but a HOLD and SAFETY_DOOR may been initiated beforehand
                    // to hold the CYCLE. Motion cancel is valid for a single planner block motion only, while jog cancel
                    // will handle and clear multiple planner block motions.
                    if (sys.state != State::Jog) {
                        sys.suspend.bit.motionCancel = true;  // NOTE: State is State::Cycle.
                    }
                    sys_rt_exec_state.bit.motionCancel = false;
                }
                // Execute a feed hold with deceleration, if required. Then, suspend system.
                if (rt_exec_state.bit.feedHold) {
                    // Block SAFETY_DOOR, JOG, and SLEEP states from changing to HOLD state.
                    if (!(sys.state == State::SafetyDoor || sys.state == State::Jog || sys.state == State::Sleep)) {
                        sys.state = State::Hold;
                    }
                    sys_rt_exec_state.bit.feedHold = false;
                }
                // Execute a safety door stop with a feed hold and disable spindle/coolant.
                // NOTE: Safety door differs from feed holds by stopping everything no matter state, disables powered
                // devices (spindle/coolant), and blocks resuming until switch is re-engaged.
                if (rt_exec_state.bit.safetyDoor) {
                    report_feedback_message(Message::SafetyDoorAjar);
                    // If jogging, block safety door methods until jog cancel is complete. Just flag that it happened.
                    if (!(sys.suspend.bit.jogCancel)) {
                        // Check if the safety re-opened during a restore parking motion only. Ignore if
                        // already retracting, parked or in sleep state.
                        if (sys.state == State::SafetyDoor) {
                            if (sys.suspend.bit.initiateRestore) {  // Actively restoring
#ifdef PARKING_ENABLE
                                // Set hold and reset appropriate control flags to restart parking sequence.
                                if (sys.step_control.executeSysMotion) {
                                    st_update_plan_block_parameters();  // Notify stepper module to recompute for hold deceleration.
                                    sys.step_control                  = {};
                                    sys.step_control.executeHold      = true;
                                    sys.step_control.executeSysMotion = true;
                                    sys.suspend.bit.holdComplete      = false;
                                }  // else NO_MOTION is active.
#endif
                                sys.suspend.bit.retractComplete = false;
                                sys.suspend.bit.initiateRestore = false;
                                sys.suspend.bit.restoreComplete = false;
                                sys.suspend.bit.restartRetract  = true;
                            }
                        }
                        if (sys.state != State::Sleep) {
                            sys.state = State::SafetyDoor;
                        }
                        sys_rt_exec_state.bit.safetyDoor = false;
                    }
                    // NOTE: This flag doesn't change when the door closes, unlike sys.state. Ensures any parking motions
                    // are executed if the door switch closes and the state returns to HOLD.
                    sys.suspend.bit.safetyDoorAjar = true;
                }
            }
            if (rt_exec_state.bit.sleep) {
                if (sys.state == State::Alarm) {
                    sys.suspend.bit.retractComplete = true;
                    sys.suspend.bit.holdComplete    = true;
                }
                sys.state                   = State::Sleep;
                sys_rt_exec_state.bit.sleep = false;
            }
        }
        // Execute a cycle start by starting the stepper interrupt to begin executing the blocks in queue.
        if (rt_exec_state.bit.cycleStart) {
            // Block if called at same time as the hold commands: feed hold, motion cancel, and safety door.
            // Ensures auto-cycle-start doesn't resume a hold without an explicit user-input.
            if (!(rt_exec_state.bit.feedHold || rt_exec_state.bit.motionCancel || rt_exec_state.bit.safetyDoor)) {
                // Resume door state when parking motion has retracted and door has been closed.
                if (sys.state == State::SafetyDoor && !(sys.suspend.bit.safetyDoorAjar)) {
                    if (sys.suspend.bit.restoreComplete) {
                        sys.state = State::Idle;  // Set to IDLE to immediately resume the cycle. 
                    } else if (sys.suspend.bit.retractComplete) {
                        // Flag to re-energize powered components and restore original position, if disabled by SAFETY_DOOR.
                        // NOTE: For a safety door to resume, the switch must be closed, as indicated by HOLD state, and
                        // the retraction execution is complete, which implies the initial feed hold is not active. To
                        // restore normal operation, the restore procedures must be initiated by the following flag. Once,
                        // they are complete, it will call CYCLE_START automatically to resume and exit the suspend.
                        sys.suspend.bit.initiateRestore = true;
                    }
                }
                // Cycle start only when IDLE or when a hold is complete and ready to resume.
                // ГЕЙТ ПО ПРОСЬБЕ ПОЛЬЗОВАТЕЛЯ: если прямо сейчас у какого-то клиента в буфере
                // лежит недопринятая строка (скорее всего хвост команды $MJ=..., ещё не
                // доехавший по сети/USB), резюм откладывается — cycleStart НЕ сбрасывается
                // (см. ниже, вне этого if), а suspend-цикл делает ещё один проход, где
                // manual_adjust_poll_clients() выше успеет её дочитать и выполнить. Так резюм
                // гарантированно срабатывает только после завершения ВСЕХ уже отправленных $MJ.
                if ((sys.state == State::Idle || (sys.state == State::Hold && sys.suspend.bit.holdComplete)) &&
                    !manual_adjust_input_pending() && !manual_adjust_command_active) {
                    manual_adjust_resume_delay_logged = false;
                    if (sys.state == State::Hold && sys.spindle_stop_ovr.value) {
                        sys.spindle_stop_ovr.bit.restoreCycle = true;  // Set to restore in suspend routine and cycle start after.
                    } else {
                        // ФИНАЛЬНЫЙ ФЛАШ ПЕРЕД РЕЗЮМЕ (найдено по тесту с окружностью из мелких
                        // сегментов: MPos после resume прыгал между двумя группами точек с разницей
                        // РОВНО в величину $MJ-сдвига — 61.763↔71.559, 61.194↔69.374, diff~10мм —
                        // прямой признак чередования СТАРЫХ (до сдвига) и НОВЫХ (после сдвига)
                        // сегментов в общем FIFO. st_flush_segment_buffer() в setup/restore_buffer()
                        // защищает от этого только ВНУТРИ самих $MJ-движений; между последним MJ и
                        // фактическим cycleStart здесь ничего не гарантировало чистоту очереди.
                        // Таймер степпера гарантированно остановлен (мы в Hold, st_go_idle() уже
                        // вызывался), поэтому здесь безопасно финально сбросить очередь и ISR-сегмент
                        // непосредственно перед тем, как ниже будут вызваны st_prep_buffer()/st_wake_up().
                        st_flush_segment_buffer();
                        manual_adjust_report_hidden_delta("before_resume");
                        st_debug_dump_prep("resume prep_state");
                        // ДИАГНОСТИКА RESUME: раньше в этой точке не логировалось вообще ничего,
                        // из-за чего в USB-логе не было следов между последним "$MJ done" и
                        // следующими событиями файла. Печатаем полное состояние ПРЯМО перед
                        // стартом цикла — блок в очереди, его оставшуюся дистанцию (mm), реальную
                        // машинную позицию (sys_position/mpos) и позицию парсера (gc_state.position),
                        // чтобы поймать источник видимого смещения на самом резюме.
                        {
                            plan_block_t* resume_block = plan_get_current_block();
                            float*        resume_mpos  = system_get_mpos();
                            manual_adjust_logf(
                                "[MSG:resume block=%s rem=%.3fmm sys_steps=(%ld,%ld,%ld) mpos=%.3f,%.3f,%.3f parser_pos=%.3f,%.3f,%.3f "
                                "motionCancel=%d]",
                                resume_block ? "yes" : "no",
                                resume_block ? resume_block->millimeters : 0.0f,
                                (long)sys_position[0],
                                (long)sys_position[1],
                                (long)sys_position[2],
                                resume_mpos[0],
                                resume_mpos[1],
                                resume_mpos[2],
                                gc_state.position[0],
                                gc_state.position[1],
                                gc_state.position[2],
                                (int)sys.suspend.bit.motionCancel);
                        }
                        // Start cycle only if queued motions exist in planner buffer and the motion is not canceled.
                        sys.step_control = {};  // Restore step control to normal operation
                        if (plan_get_current_block() && !sys.suspend.bit.motionCancel) {
                            sys.suspend.value = 0;  // Break suspend state.
                            sys.state         = State::Cycle;
                            st_prep_buffer();  // Initialize step segment buffer before beginning cycle.
                            st_wake_up();
                        } else {                    // Otherwise, do nothing. Set and resume IDLE state.
                            sys.suspend.value = 0;  // Break suspend state.
                            sys.state         = State::Idle; 
                        }
                    }
                } else if (!manual_adjust_resume_delay_logged) {
                    manual_adjust_resume_delay_logged = true;
                    manual_adjust_logf("[MSG:resume delayed pending=%d active=%d state=%d holdComplete=%d]",
                                       (int)manual_adjust_input_pending(),
                                       (int)manual_adjust_command_active,
                                       (int)sys.state,
                                       (int)sys.suspend.bit.holdComplete);
                }
            }
            // Если резюм отложен из-за незавершённого $MJ-ввода ИЛИ потому, что текущий
            // $MJ ещё физически выполняется (manual_adjust_command_active), НЕ сбрасываем
            // cycleStart — оставляем его выставленным, чтобы попытка повторилась на
            // следующем проходе suspend-цикла, когда и ввод, и само движение будут завершены.
            if (!manual_adjust_input_pending() && !manual_adjust_command_active) {
                sys_rt_exec_state.bit.cycleStart = false;
            }
        }
        if (cycle_stop) {
            // Reinitializes the cycle plan and stepper system after a feed hold for a resume. Called by
            // realtime command execution in the main program, ensuring that the planner re-plans safely.
            // NOTE: Bresenham algorithm variables are still maintained through both the planner and stepper
            // cycle reinitializations. The stepper path should continue exactly as if nothing has happened.
            // NOTE: cycle_stop is set by the stepper subsystem when a cycle or feed hold completes.
            if ((sys.state == State::Hold || sys.state == State::SafetyDoor || sys.state == State::Sleep) && !(sys.soft_limit) &&
                !(sys.suspend.bit.jogCancel)) {
                // Hold complete. Set to indicate ready to resume.  Remain in HOLD or DOOR states until user
                // has issued a resume command or reset.
                plan_cycle_reinitialize();
                if (sys.step_control.executeHold) {
                    sys.suspend.bit.holdComplete = true;
                }
                sys.step_control.executeHold      = false;
                sys.step_control.executeSysMotion = false;
            } else {
                // Motion complete. Includes CYCLE/JOG/HOMING states and jog cancel/motion cancel/soft limit events.
                // NOTE: Motion and jog cancel both immediately return to idle after the hold completes.
                if (sys.suspend.bit.jogCancel) {  // For jog cancel, flush buffers and sync positions.
                    sys.step_control = {};
                    plan_reset();
                    st_reset();
                    gc_sync_position();
                    plan_sync_position();
                }
                if (sys.suspend.bit.safetyDoorAjar) {  // Only occurs when safety door opens during jog.
                    sys.suspend.bit.jogCancel    = false;
                    sys.suspend.bit.holdComplete = true;
                    sys.state                    = State::SafetyDoor;
                } else {
                    sys.suspend.value = 0;
                    sys.state         = State::Idle;
                    // ФИКС зависания на M3/M4 (spindle->sync -> protocol_buffer_synchronize) после
                    // короткого/быстрого предшествующего перемещения (напр. "G0Z5" перед "M3 S..."):
                    // ISR ставит cycle_stop=true, когда буфер СЕГМЕНТОВ опустел (st_go_idle()), но
                    // это не гарантирует, что st_prep_buffer() успел дойти до конца ТЕКУЩЕГО блока
                    // планировщика и вызвать plan_discard_current_block() (штатно это делает именно
                    // st_prep_buffer(), а не ISR). Если st_prep_buffer() отстал (не успел на короткой
                    // операции из-за паузы в обслуживании main-loop), блок остаётся в очереди навечно:
                    // sys.state уже Idle, а Idle не входит в список состояний, для которых
                    // protocol_execute_realtime() продолжает звать st_prep_buffer() — то есть
                    // "довызвать" его уже никто не сможет. plan_get_current_block() после этого
                    // навсегда возвращает тот же "недоеденный" блок, и protocol_buffer_synchronize()
                    // (напр. из spindle->sync() на следующей команде M3/M4) виснет бесконечно в своём
                    // do-while, ожидая его завершения. ВАЖНО (см. discard_stale_planner_block()):
                    // "ISR решил, что степать больше нечего" означает только что буфер СЕГМЕНТОВ
                    // пуст — а не что весь блок физически пройден, если st_prep_buffer() не успел
                    // сгенерировать для него сегменты до конца. Поэтому снимаем с очереди только
                    // тихо, если реальной непройденной дистанции не осталось; иначе поднимаем тревогу.
                    // ПЕРЕД тревогой сначала пробуем бесшовно резюмировать тот же блок (см.
                    // try_resume_stale_block()) — большинство таких гонок транзиентны (главный
                    // подозреваемый триггер — кратковременная нагрузка WebUI/WiFi на main-loop),
                    // и повторная подготовка сегментов для того же module-static block state
                    // обычно позволяет продолжить движение без какой-либо видимой остановки.
                    // Motion-cancel is used by probing to intentionally stop the current block on contact.
                    // Do not auto-resume that block here, otherwise a valid probe hit can continue moving.
                    if (plan_get_current_block() && !sys.suspend.bit.motionCancel &&
                        gc_state.modal.motion != Motion::ProbeToward && gc_state.modal.motion != Motion::ProbeTowardNoError &&
                        gc_state.modal.motion != Motion::ProbeAway && gc_state.modal.motion != Motion::ProbeAwayNoError) {
                        if (!try_resume_stale_block("cycle_stop")) {
                            discard_stale_planner_block("cycle_stop");
                        }
                    }
                }
            }
            cycle_stop = false;
        }
    }
    // Execute overrides.
    if ((sys_rt_f_override != sys.f_override) || (sys_rt_r_override != sys.r_override)) {
        sys.f_override         = sys_rt_f_override;
        sys.r_override         = sys_rt_r_override;
        sys.report_ovr_counter = 0;  // Set to report change immediately
        plan_update_velocity_profile_parameters();
        plan_cycle_reinitialize();
    }

    // NOTE: Unlike motion overrides, spindle overrides do not require a planner reinitialization.
    if (sys_rt_s_override != sys.spindle_speed_ovr) {
        sys.step_control.updateSpindleRpm = true;
        sys.spindle_speed_ovr             = sys_rt_s_override;
        sys.report_ovr_counter            = 0;  // Set to report change immediately
        // If spinlde is on, tell it the rpm has been overridden
        if (gc_state.modal.spindle != SpindleState::Disable) {
            spindle->set_rpm(gc_state.spindle_speed);
        }
    }

    if (sys_rt_exec_accessory_override.bit.spindleOvrStop) {
        sys_rt_exec_accessory_override.bit.spindleOvrStop = false;
        // Spindle stop override allowed only while in HOLD state.
        // NOTE: Report counters are set in spindle_set_state() when spindle stop is executed.
        if (sys.state == State::Hold) {
            if (sys.spindle_stop_ovr.value == 0) {
                sys.spindle_stop_ovr.bit.initiate = true;
            } else if (sys.spindle_stop_ovr.bit.enabled) {
                sys.spindle_stop_ovr.bit.restore = true;
            }
        }
    }

    // NOTE: Since coolant state always performs a planner sync whenever it changes, the current
    // run state can be determined by checking the parser state.
    if (sys_rt_exec_accessory_override.bit.coolantFloodOvrToggle) {
        sys_rt_exec_accessory_override.bit.coolantFloodOvrToggle = false;
#ifdef COOLANT_FLOOD_PIN
        if (sys.state == State::Idle || sys.state == State::Cycle || sys.state == State::Hold) {
            gc_state.modal.coolant.Flood = !gc_state.modal.coolant.Flood;
            coolant_set_state(gc_state.modal.coolant);  // Report counter set in coolant_set_state().
        }
#endif
    }
    if (sys_rt_exec_accessory_override.bit.coolantMistOvrToggle) {
        sys_rt_exec_accessory_override.bit.coolantMistOvrToggle = false;
#ifdef COOLANT_MIST_PIN
        if (sys.state == State::Idle || sys.state == State::Cycle || sys.state == State::Hold) {
            gc_state.modal.coolant.Mist = !gc_state.modal.coolant.Mist;
            coolant_set_state(gc_state.modal.coolant);  // Report counter set in coolant_set_state().
        }
#endif
    }

#ifdef DEBUG
    if (sys_rt_exec_debug) {
        report_realtime_debug();
        sys_rt_exec_debug = false;
    }
#endif
    // Reload step segment buffer
    // ФИНАЛЬНЫЙ УРОВЕНЬ ТОЙ ЖЕ ПРОБЛЕМЫ (найдено по логам: та же диагональная убывающая
    // последовательность (64,64,64)→...→(0,-8,0) повторяется даже после flush в
    // st_parking_setup_buffer()/сброса exec_segment): пока пользователь просто ждёт между
    // командами $MJ (ничего не нажимая), ЭТОТ switch продолжает вызывать st_prep_buffer()
    // на каждой итерации цикла паузы (State::Hold). Раз pl_block==NULL и holdPartialBlock=1
    // (обычная ситуация — пауза застала станок посреди блока), st_prep_buffer() КАЖДЫЙ РАЗ
    // молча регенерирует НОВЫЕ сегменты прерванного блока (используя restored prep.steps_
    // remaining/dt_remainder/step_per_mm) в segment_buffer — хотя таймер ISR не запущен и
    // никто их не читает. Они просто копятся. Мой flush в setup_buffer() чистит буфер ТОЛЬКО
    // в момент старта СЛЕДУЮЩЕГО $MJ, но между этим моментом и предыдущим MJ (пока
    // пользователь ждёт) буфер снова успевает наполниться этими "фоновыми" сегментами того
    // же старого блока — отсюда идентичная убывающая последовательность при каждом тесте
    // (она детерминирована характеристиками одного и того же прерванного блока).
    // Правильная точка вызова: только когда ЛИБО обычный цикл активно крутится (не Hold),
    // ЛИБО именно ВНУТРИ системного ($MJ) движения (executeSysMotion=true — это нужно,
    // подтверждено: без этого длинные X10/X-10 обрывались на середине). Все ОСТАЛЬНОЕ время
    // Hold (простое ожидание следующей команды на паузе) НЕ должно трогать segment_buffer —
    // дозаправка недокрученного блока произойдёт штатно уже на самом Resume.
    switch (sys.state) {
        case State::Cycle:
        case State::SafetyDoor:
        case State::Homing:
        case State::Sleep:
        case State::Jog:
            st_prep_buffer();
            break;
        case State::Hold:
            if (sys.step_control.executeSysMotion || sys.step_control.executeHold) {
                st_prep_buffer();
            }
            break;
        default:
            break;
    }
}

// Handles Grbl system suspend procedures, such as feed hold, safety door, and parking motion.
// The system will enter this loop, create local variables for suspend tasks, and return to
// whatever function that invoked the suspend, such that Grbl resumes normal operation.
// This function is written in a way to promote custom parking motions. Simply use this as a
// template
static void protocol_exec_rt_suspend() {
#ifdef PARKING_ENABLE
    // Declare and initialize parking local variables
    float             restore_target[MAX_N_AXIS];
    float             retract_waypoint = PARKING_PULLOUT_INCREMENT;
    plan_line_data_t  plan_data;
    plan_line_data_t* pl_data = &plan_data;
    memset(pl_data, 0, sizeof(plan_line_data_t));
    pl_data->motion                = {};
    pl_data->motion.systemMotion   = 1;
    pl_data->motion.noFeedOverride = 1;
#    ifdef USE_LINE_NUMBERS
    pl_data->line_number = PARKING_MOTION_LINE_NUMBER;
#    endif
#endif
    plan_block_t* block = plan_get_current_block();
    CoolantState  restore_coolant;
    SpindleState  restore_spindle;
    float         restore_spindle_speed;
    if (block == NULL) {
        restore_coolant       = gc_state.modal.coolant;
        restore_spindle       = gc_state.modal.spindle;
        restore_spindle_speed = gc_state.spindle_speed;
    } else {
        restore_coolant       = block->coolant;
        restore_spindle       = block->spindle;
        restore_spindle_speed = block->spindle_speed;
    }
#ifdef DISABLE_LASER_DURING_HOLD
    if (spindle->inLaserMode()) {
        sys_rt_exec_accessory_override.bit.spindleOvrStop = true;
    }
#endif

    while (sys.suspend.value) {
        if (sys.abort) {
            return;
        }
        // Ручная коррекция при паузе (см. manual_adjust_jog() выше): читаем и
        // выполняем команды $MJ=..., пока задание стоит на паузе. Никакого
        // авто-возврата перед возобновлением здесь намеренно нет — смещение
        // должно молча остаться в физическом положении станка (см. комментарий
        // к manual_adjust_erase_motion_from_tracking()).
        manual_adjust_poll_clients();
        // if a jogCancel comes in and we have a jog "in-flight" (parsed and handed over to mc_line()),
        //  then we need to cancel it before it reaches the planner.  otherwise we may try to move way out of
        //  normal bounds, especially with senders that issue a series of jog commands before sending a cancel.
        if (sys.suspend.bit.jogCancel && sys_pl_data_inflight != NULL && ((plan_line_data_t*)sys_pl_data_inflight)->is_jog) {
            sys_pl_data_inflight = NULL;
        }
        // Block until initial hold is complete and the machine has stopped motion.
        if (sys.suspend.bit.holdComplete) {
            // Parking manager. Handles de/re-energizing, switch state checks, and parking motions for
            // the safety door and sleep states.
            if (sys.state == State::SafetyDoor || sys.state == State::Sleep) {
                // Handles retraction motions and de-energizing.
#ifdef PARKING_ENABLE
                float* parking_target = system_get_mpos();
#endif
                if (!sys.suspend.bit.retractComplete) {
                    // Ensure any prior spindle stop override is disabled at start of safety door routine.
                    sys.spindle_stop_ovr.value = 0;  // Disable override
#ifndef PARKING_ENABLE
                    spindle->set_state(SpindleState::Disable, 0);  // De-energize
                    coolant_off();
#else
                    // Get current position and store restore location and spindle retract waypoint.
                    if (!sys.suspend.bit.restartRetract) {
                        memcpy(restore_target, parking_target, sizeof(restore_target[0]) * number_axis->get());
                        retract_waypoint += restore_target[PARKING_AXIS];
                        retract_waypoint = MIN(retract_waypoint, PARKING_TARGET);
                    }
                    // Execute slow pull-out parking retract motion. Parking requires homing enabled, the
                    // current location not exceeding the parking target location, and laser mode disabled.
                    // NOTE: State is will remain DOOR, until the de-energizing and retract is complete.
                    if (can_park() && parking_target[PARKING_AXIS] < PARKING_TARGET) {
                        // Retract spindle by pullout distance. Ensure retraction motion moves away from
                        // the workpiece and waypoint motion doesn't exceed the parking target location.
                        if (parking_target[PARKING_AXIS] < retract_waypoint) {
                            parking_target[PARKING_AXIS] = retract_waypoint;
                            pl_data->feed_rate           = PARKING_PULLOUT_RATE;
                            pl_data->coolant             = restore_coolant;
                            pl_data->spindle             = restore_spindle;
                            pl_data->spindle_speed       = restore_spindle_speed;
                            mc_parking_motion(parking_target, pl_data);
                        }
                        // NOTE: Clear accessory state after retract and after an aborted restore motion.
                        pl_data->spindle               = SpindleState::Disable;
                        pl_data->coolant               = {};
                        pl_data->motion                = {};
                        pl_data->motion.systemMotion   = 1;
                        pl_data->motion.noFeedOverride = 1;
                        pl_data->spindle_speed         = 0.0;
                        spindle->set_state(pl_data->spindle, 0);  // De-energize
                        coolant_set_state(pl_data->coolant);
                        // Execute fast parking retract motion to parking target location.
                        if (parking_target[PARKING_AXIS] < PARKING_TARGET) {
                            parking_target[PARKING_AXIS] = PARKING_TARGET;
                            pl_data->feed_rate           = PARKING_RATE;
                            mc_parking_motion(parking_target, pl_data);
                        }
                    } else {
                        // Parking motion not possible. Just disable the spindle and coolant.
                        // NOTE: Laser mode does not start a parking motion to ensure the laser stops immediately.
                        spindle->set_state(SpindleState::Disable, 0);  // De-energize
                        coolant_off();
                    }
#endif
                    sys.suspend.bit.restartRetract  = false;
                    sys.suspend.bit.retractComplete = true;
                } else {
                    if (sys.state == State::Sleep) {
                        report_feedback_message(Message::SleepMode);
                        // Spindle and coolant should already be stopped, but do it again just to be sure.
                        spindle->set_state(SpindleState::Disable, 0);  // De-energize
                        coolant_off();
                        st_go_idle();  // Disable steppers
                        while (!(sys.abort)) {
                            protocol_exec_rt_system();  // Do nothing until reset.
                        }
                        return;  // Abort received. Return to re-initialize.
                    }
                    // Allows resuming from parking/safety door. Actively checks if safety door is closed and ready to resume.
                    if (sys.state == State::SafetyDoor) {
                        if (!(system_check_safety_door_ajar())) {
                            sys.suspend.bit.safetyDoorAjar = false;  // Reset door ajar flag to denote ready to resume.
                        }
                    }
                    // Handles parking restore and safety door resume.
                    if (sys.suspend.bit.initiateRestore) {
#ifdef PARKING_ENABLE
                        // Execute fast restore motion to the pull-out position. Parking requires homing enabled.
                        // NOTE: State is will remain DOOR, until the de-energizing and retract is complete.
                        if (can_park()) {
                            // Check to ensure the motion doesn't move below pull-out position.
                            if (parking_target[PARKING_AXIS] <= PARKING_TARGET) {
                                parking_target[PARKING_AXIS] = retract_waypoint;
                                pl_data->feed_rate           = PARKING_RATE;
                                mc_parking_motion(parking_target, pl_data);
                            }
                        }
#endif
                        // Delayed Tasks: Restart spindle and coolant, delay to power-up, then resume cycle.
                        if (gc_state.modal.spindle != SpindleState::Disable) {
                            // Block if safety door re-opened during prior restore actions.
                            if (!sys.suspend.bit.restartRetract) {
                                if (spindle->inLaserMode()) {
                                    // When in laser mode, ignore spindle spin-up delay. Set to turn on laser when cycle starts.
                                    sys.step_control.updateSpindleRpm = true;
                                } else {
                                    spindle->set_state(restore_spindle, (uint32_t)restore_spindle_speed);
                                    // restore delay is done in the spindle class
                                    //delay_sec(int32_t(1000.0 * spindle_delay_spinup->get()), DwellMode::SysSuspend);
                                }
                            }
                        }
                        if (gc_state.modal.coolant.Flood || gc_state.modal.coolant.Mist) {
                            // Block if safety door re-opened during prior restore actions.
                            if (!sys.suspend.bit.restartRetract) {
                                // NOTE: Laser mode will honor this delay. An exhaust system is often controlled by this pin.
                                coolant_set_state(restore_coolant);
                                delay_msec(int32_t(1000.0 * coolant_start_delay->get()), DwellMode::SysSuspend);
                            }
                        }
#ifdef PARKING_ENABLE
                        // Execute slow plunge motion from pull-out position to resume position.
                        if (can_park()) {
                            // Block if safety door re-opened during prior restore actions.
                            if (!sys.suspend.bit.restartRetract) {
                                // Regardless if the retract parking motion was a valid/safe motion or not, the
                                // restore parking motion should logically be valid, either by returning to the
                                // original position through valid machine space or by not moving at all.
                                pl_data->feed_rate     = PARKING_PULLOUT_RATE;
                                pl_data->spindle       = restore_spindle;
                                pl_data->coolant       = restore_coolant;
                                pl_data->spindle_speed = restore_spindle_speed;
                                mc_parking_motion(restore_target, pl_data);
                            }
                        }
#endif
                        if (!sys.suspend.bit.restartRetract) {
                            sys.suspend.bit.restoreComplete  = true;
                            sys_rt_exec_state.bit.cycleStart = true;  // Set to resume program.
                        }
                    }
                }
            } else {
                // Feed hold manager. Controls spindle stop override states.
                // NOTE: Hold ensured as completed by condition check at the beginning of suspend routine.
                if (sys.spindle_stop_ovr.value) {
                    // Handles beginning of spindle stop
                    if (sys.spindle_stop_ovr.bit.initiate) {
                        if (gc_state.modal.spindle != SpindleState::Disable) {
                            spindle->set_state(SpindleState::Disable, 0);  // De-energize
                            sys.spindle_stop_ovr.value       = 0;
                            sys.spindle_stop_ovr.bit.enabled = true;  // Set stop override state to enabled, if de-energized.
                        } else {
                            sys.spindle_stop_ovr.value = 0;  // Clear stop override state
                        }
                        // Handles restoring of spindle state
                    } else if (sys.spindle_stop_ovr.bit.restore || sys.spindle_stop_ovr.bit.restoreCycle) {
                        if (gc_state.modal.spindle != SpindleState::Disable) {
                            report_feedback_message(Message::SpindleRestore);
                            if (spindle->inLaserMode()) {
                                // When in laser mode, ignore spindle spin-up delay. Set to turn on laser when cycle starts.
                                sys.step_control.updateSpindleRpm = true;
                            } else {
                                spindle->set_state(restore_spindle, (uint32_t)restore_spindle_speed);
                            }
                        }
                        if (sys.spindle_stop_ovr.bit.restoreCycle) {
                            sys_rt_exec_state.bit.cycleStart = true;  // Set to resume program.
                        }
                        sys.spindle_stop_ovr.value = 0;  // Clear stop override state
                    }
                } else {
                    // Handles spindle state during hold. NOTE: Spindle speed overrides may be altered during hold state.
                    // NOTE: sys.step_control.updateSpindleRpm is automatically reset upon resume in step generator.
                    if (sys.step_control.updateSpindleRpm) {
                        spindle->set_state(restore_spindle, (uint32_t)restore_spindle_speed);
                        sys.step_control.updateSpindleRpm = false;
                    }
                }
            }
        }
        protocol_exec_rt_system();
    }
    // Пауза завершилась (любым способом) — следующая пауза должна начать захват
    // позиции для правки с чистого листа.
    manual_adjust_report_hidden_delta("suspend_exit");
    manual_adjust_end_session();
}
