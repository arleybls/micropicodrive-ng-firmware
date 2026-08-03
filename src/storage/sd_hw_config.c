// Hardware configuration for the onboard microSD slot (no-OS-FatFS library).
// Adapted from the UIExt sandbox sd_hw_config.c (sandbox commit 057b1ae) for
// the MicroPicoDrive board netlist:
//   MISO -> GP16   CS -> GP17   SCK -> GP18   MOSI -> GP19   (SPI0)
// No card-detect line. No external pull-ups on the slot — the library enables
// the Pico's internal pull-up on MISO by default (my_spi.c); leave
// no_miso_gpio_pull_up unset.
#include "hw_config.h"

static spi_t spi = {
    .hw_inst = spi0,
    .sck_gpio = 18,
    .mosi_gpio = 19,
    .miso_gpio = 16,
    .baud_rate = 12500000,   // conservative start; raise after hardware soak
};

static sd_spi_if_t spi_if = {
    .spi = &spi,
    .ss_gpio = 17,
};

static sd_card_t sd_card = {
    .type = SD_IF_SPI,
    .spi_if_p = &spi_if,
};

size_t sd_get_num(void) { return 1; }

sd_card_t *sd_get_by_num(size_t num) {
    return (num == 0) ? &sd_card : NULL;
}
