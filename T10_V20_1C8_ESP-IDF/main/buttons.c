#include "buttons.h"
#include <stdint.h>
#include "driver/gpio.h"

// Actual button-to-GPIO wiring on this T10 V2.0 unit (found empirically; the
// repo's Arduino header lists 36/37/39, which is wrong for this revision).
static const int s_pins[3] = { 35, 34, 39 };

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
}

int buttons_level(int idx)
{
    if (idx < 0 || idx > 2) return 0;
    return gpio_get_level(s_pins[idx]) == 0 ? 1 : 0;   // active-low
}
