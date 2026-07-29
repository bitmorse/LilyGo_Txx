#include "sound.h"

#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SPK_GPIO      25            // SPEAKER_OUT on the T10 V2.0
#define SPK_MODE      LEDC_LOW_SPEED_MODE
#define SPK_TIMER     LEDC_TIMER_0
#define SPK_CHANNEL   LEDC_CHANNEL_0
#define SPK_RES       LEDC_TIMER_10_BIT   // duty range 0..1023
#define SPK_DUTY_ON   400                 // ~40%: audible but not harsh

// Note frequencies (Hz).
enum {
    R = 0,                          // rest
    C5 = 523, D5 = 587, E5 = 659, F5 = 698, G5 = 784, A5 = 880, B5 = 988,
    C6 = 1047, D6 = 1175, E6 = 1319, G6 = 1568,
};

void sound_init(void)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = SPK_MODE,
        .duty_resolution = SPK_RES,
        .timer_num       = SPK_TIMER,
        .freq_hz         = 1000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tcfg);

    ledc_channel_config_t ccfg = {
        .gpio_num   = SPK_GPIO,
        .speed_mode = SPK_MODE,
        .channel    = SPK_CHANNEL,
        .timer_sel  = SPK_TIMER,
        .duty       = 0,            // start silent
        .hpoint     = 0,
    };
    ledc_channel_config(&ccfg);
}

void sound_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (freq_hz == 0) {
        ledc_set_duty(SPK_MODE, SPK_CHANNEL, 0);
        ledc_update_duty(SPK_MODE, SPK_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        return;
    }
    ledc_set_freq(SPK_MODE, SPK_TIMER, freq_hz);
    ledc_set_duty(SPK_MODE, SPK_CHANNEL, SPK_DUTY_ON);
    ledc_update_duty(SPK_MODE, SPK_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    // Brief gap so consecutive notes are distinct.
    ledc_set_duty(SPK_MODE, SPK_CHANNEL, 0);
    ledc_update_duty(SPK_MODE, SPK_CHANNEL);
    vTaskDelay(pdMS_TO_TICKS(18));
}

void sound_play_boot_melody(void)
{
    // A cheery little ascending flourish.
    static const uint16_t notes[] = { C5, E5, G5, C6, G5, C6, E6 };
    static const uint16_t durs[]  = { 110, 110, 110, 180, 110, 110, 300 };
    const int n = sizeof(notes) / sizeof(notes[0]);

    for (int i = 0; i < n; i++) {
        sound_tone(notes[i], durs[i]);
    }
    // Make sure the speaker is left silent.
    sound_tone(R, 1);
}
