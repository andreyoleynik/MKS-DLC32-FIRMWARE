#include "MKS_TS35.h"

TFT_eSPI tft = TFT_eSPI(); 
static bool ts35_beep_use_ledc = false;

void tft_LCD_Fill() {
    tft.fillRect(0, 0,480, 320, TFT_COLOR_RED);
}

void tft_TS35_init() {
    tft.begin();
    tft.setRotation(1);
    // tft.fillScreen(tft.color565(0X1A, 0X1A, 0X1A));
    tft.initDMA();
    delay_ms(100);
}   

void ts35_beep_init() { 
    #ifdef USE_BEEP_LEDC
        if(BEEPER < I2S_OUT_PIN_BASE) {
            ledcSetup(BEEP_LEDC_CHANNEL, 5000, 8);
            ledcAttachPin(BEEPER, BEEP_LEDC_CHANNEL);
            ledcWrite(BEEP_LEDC_CHANNEL, 0);
            ts35_beep_use_ledc = true;
        } else {
            pinMode(BEEPER, OUTPUT);
            digitalWrite(BEEPER, LOW);
            ts35_beep_use_ledc = false;
        }
    #else
        pinMode(BEEPER, OUTPUT);
        digitalWrite(BEEPER, LOW);
        ts35_beep_use_ledc = false;
    #endif
}

void ts35_beep_on(void) 
{
    if(!beep_status->get()) {
        return;
    }

    if(ts35_beep_use_ledc) {
        ledcWrite(BEEP_LEDC_CHANNEL, 50);
    } else {
        digitalWrite(BEEPER, HIGH);
    }
}

void ts35_beep_off(void) 
{
    if(ts35_beep_use_ledc) {
        ledcWrite(BEEP_LEDC_CHANNEL, 0);
    } else {
        digitalWrite(BEEPER, LOW);
    }
}

uint32_t ts35_beep_off_time = 0;

void ts35_beep_handler(void) {

    if(ts35_beep_off_time > 0 && millis() > ts35_beep_off_time)
    {
        ts35_beep_off_time = 0;
        ts35_beep_off();
    }
}

void ts35_beep_on(uint16_t time_ms) {

    ts35_beep_off_time = millis() + uint32_t(time_ms);

    ts35_beep_on();
}


#define LED_RESOLUTION      10
#define LED_CHANNEL         14
void bsp_led_init(void) { 
#ifdef LED_PIN
    ledcSetup(LED_CHANNEL, 1000, LED_RESOLUTION); // 设置通道14 ,分辨率为10（2^10 = 1024）
    ledcAttachPin(LED_PIN, LED_CHANNEL);  // 将通道与对应的引脚连接
#endif
}

void bsp_set_duty(uint32_t duty) {
#ifdef LED_PIN
    ledcWrite(LED_CHANNEL, duty);
#endif
}

// 0-255
void bsp_led_pwm_speed(uint8_t speed) {
    uint32_t duty = 1024;
    duty = (duty * (255 - speed)) / 255;
    bsp_set_duty(duty);
}


uint32_t led_count = 0;;
uint8_t led_dir_flag = 1;
void bsp_led_run(void) {

    if(led_dir_flag) {
        led_count = led_count + 15;
        if(led_count >= 255) {
            led_dir_flag = 0;
        }
    }else{
        led_count = led_count - 15;
        if(led_count == 0) {
            led_dir_flag = 1;
        }
    }
    bsp_led_pwm_speed(led_count);
}










