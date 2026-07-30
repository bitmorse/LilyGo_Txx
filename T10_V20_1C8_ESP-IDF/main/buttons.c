#include "buttons.h"
#include <stdint.h>
#include "driver/gpio.h"

// Actual button-to-GPIO wiring on this T10 V2.0 unit (found empirically; the
// repo's Arduino header lists 36/37/39, which is wrong for this revision).
static const int s_pins[3] = { 35, 34, 39 };
static int s_last[3] = { 1, 1, 1 };   // released = high

void buttons_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < 3; i++) mask |= (1ULL << s_pins[i]);

    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,     // GPIO34-39 have no internal pulls
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    for (int i = 0; i < 3; i++) s_last[i] = gpio_get_level(s_pins[i]);
}

int buttons_poll(void)
{
    int pressed = 0;
    for (int i = 0; i < 3; i++) {
        int lvl = gpio_get_level(s_pins[i]);
        if (lvl == 0 && s_last[i] == 1) {      // high -> low edge = new press
            pressed |= (1 << i);
        }
        s_last[i] = lvl;
    }
    return pressed;
}

int buttons_level(int idx)
{
    if (idx < 0 || idx > 2) return 0;
    return gpio_get_level(s_pins[idx]) == 0 ? 1 : 0;   // active-low
}
