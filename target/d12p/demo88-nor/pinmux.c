/*
 * Copyright (c) 2025-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors: huahui.mai@artinchip.com
 */

#include <aic_core.h>
#include <aic_hal.h>
#include "board.h"
#include <aic_utils.h>

struct aic_pinmux aic_pinmux_config[] = {
#ifdef AIC_USING_CIR
    /* cir */
    {3, PIN_PULL_DIS, 3, "PA.0"},
    {3, PIN_PULL_UP, 3, "PA.1"},
#endif
#ifdef AIC_USING_UART0
    /* uart1 */
    {5, PIN_PULL_DIS, 3, "PA.0"},
    {5, PIN_PULL_UP, 3, "PA.1"},
#endif
#ifdef AIC_USING_UART1
    /* uart1 */
    {5, PIN_PULL_DIS, 3, "PA.2"},
    {5, PIN_PULL_UP, 3, "PA.3"},
#endif
#ifdef AIC_USING_CANFD0
    /* can0 */
    {4, PIN_PULL_DIS, 3, "PA.4"},
    {4, PIN_PULL_DIS, 3, "PA.5"},
#endif
#ifdef AIC_USING_I2C0
    {4, PIN_PULL_DIS, 3, "PA.6"},  // SCK
    {4, PIN_PULL_DIS, 3, "PA.7"},  // SDA
#endif
#ifdef AIC_USING_I2C1
    {4, PIN_PULL_DIS, 3, "PD.2"},  // SCK
    {4, PIN_PULL_DIS, 3, "PD.3"},  // SDA
#endif
#ifdef AIC_USING_RTP
    {2, PIN_PULL_DIS, 3, "PA.8"},
    {2, PIN_PULL_DIS, 3, "PA.9"},
    {2, PIN_PULL_DIS, 3, "PA.10"},
    {2, PIN_PULL_DIS, 3, "PA.11"},
#endif
#ifdef AIC_EPD_P118F04
    /*
     * J14 -> P118F04 4-wire SPI. PE.6 is not connected.
     * PE.5 CSB, PE.7 SDA, PE.8 SCL, PE.9 BUSY_N, PE.10 DC, PE.11 RST_N.
     */
    {1, PIN_PULL_UP,   3, "PE.5"},
    {1, PIN_PULL_DIS,  3, "PE.6"},
    {1, PIN_PULL_UP,   3, "PE.7"},
    {1, PIN_PULL_DIS,  3, "PE.8"},
    {1, PIN_PULL_UP,   3, "PE.9"},
    {1, PIN_PULL_UP,   3, "PE.10"},
    {1, PIN_PULL_UP,   3, "PE.11"},
#elif defined(AIC_USING_QSPI1)
    /*
     * SPI1 / QSPI1 on PE (J25 DVP connector remapped to LCD):
     *   PE5  SPI1_CS   -> LCD_CS
     *   PE6  SPI1_MISO -> panel D0
     *   PE7  SPI1_MOSI -> panel D1
     *   PE8  SPI1_CLK  -> SPI_CLK
     *   PE9  SPI1_HOLD -> panel D3
     *   PE10 SPI1_WP   -> panel D2
     *   PE11 GPIO      -> LCD_RESET
     * Func 6 = SPI1 on PE (DVP=3, EPWM=7).
     * PD.26 MMOSIE is GPIO 1-wire (not SPI1/SPI5 MOSI).
     */
    {6, PIN_PULL_UP,  3, "PE.5"},
    {6, PIN_PULL_DIS, 3, "PE.6"},
    {6, PIN_PULL_DIS, 3, "PE.7"},
    {6, PIN_PULL_DIS, 3, "PE.8"},
    {6, PIN_PULL_DIS, 3, "PE.9"},
    {6, PIN_PULL_DIS, 3, "PE.10"},
#endif
#ifdef AIC_USING_SDMC0
    {2, PIN_PULL_UP, 7, "PB.6"},
    {2, PIN_PULL_UP, 7, "PB.7"},
    {2, PIN_PULL_UP, 7, "PB.8"},
    {2, PIN_PULL_UP, 7, "PB.9"},
    {2, PIN_PULL_UP, 7, "PB.10"},
    {2, PIN_PULL_UP, 7, "PB.11"},
#endif
#ifdef AIC_USING_QSPI0
#ifndef AIC_SYSCFG_SIP_FLASH_ENABLE
    /* qspi0 */
    {2, PIN_PULL_UP, 3, "PB.0"},
    {2, PIN_PULL_UP, 3, "PB.1"},
    {2, PIN_PULL_UP, 3, "PB.2"},
    {2, PIN_PULL_UP, 3, "PB.3"},
    {2, PIN_PULL_UP, 3, "PB.4"},
    {2, PIN_PULL_UP, 3, "PB.5"},
#else
    {8, PIN_PULL_UP, 3, "PB.12"},
    {8, PIN_PULL_UP, 3, "PB.13"},
    {8, PIN_PULL_UP, 3, "PB.14"},
    {8, PIN_PULL_UP, 3, "PB.15"},
    {8, PIN_PULL_UP, 3, "PB.16"},
    {8, PIN_PULL_UP, 3, "PB.17"},
#endif
#endif
#ifdef AIC_USING_SDMC1
    {2, PIN_PULL_UP, 7, "PC.0"},
    {2, PIN_PULL_UP, 7, "PC.1"},
    {2, PIN_PULL_UP, 7, "PC.2"},
    {2, PIN_PULL_UP, 7, "PC.3"},
    {2, PIN_PULL_UP, 7, "PC.4"},
    {2, PIN_PULL_UP, 7, "PC.5"},
#endif
#ifdef AIC_USING_QSPI2
    {3, PIN_PULL_UP, 7, "PC.6"},
    {3, PIN_PULL_UP, 7, "PC.7"},
    {3, PIN_PULL_UP, 7, "PC.8"},
    {3, PIN_PULL_UP, 7, "PC.9"},
    {3, PIN_PULL_UP, 7, "PC.10"},
    {3, PIN_PULL_UP, 7, "PC.11"},
#endif
#ifdef AIC_USING_I2S0
    {4, PIN_PULL_DIS, 3, "PD.11"},
    {4, PIN_PULL_DIS, 3, "PD.12"},
    {4, PIN_PULL_DIS, 3, "PD.13"},
    {4, PIN_PULL_DIS, 3, "PD.14"},
    {4, PIN_PULL_DIS, 3, "PD.15"},
#endif
#ifdef AIC_PRGB_24BIT
    {2, PIN_PULL_DIS, 3, "PD.0"},
    {2, PIN_PULL_DIS, 3, "PD.1"},
    {2, PIN_PULL_DIS, 3, "PD.2"},
    {2, PIN_PULL_DIS, 3, "PD.3"},
    {2, PIN_PULL_DIS, 3, "PD.4"},
    {2, PIN_PULL_DIS, 3, "PD.5"},
    {2, PIN_PULL_DIS, 3, "PD.6"},
    {2, PIN_PULL_DIS, 3, "PD.7"},
    {2, PIN_PULL_DIS, 3, "PD.8"},
    {2, PIN_PULL_DIS, 3, "PD.9"},
    {2, PIN_PULL_DIS, 3, "PD.10"},
    {2, PIN_PULL_DIS, 3, "PD.11"},
    {2, PIN_PULL_DIS, 3, "PD.12"},
    {2, PIN_PULL_DIS, 3, "PD.13"},
    {2, PIN_PULL_DIS, 3, "PD.14"},
    {2, PIN_PULL_DIS, 3, "PD.15"},
    {2, PIN_PULL_DIS, 3, "PD.16"},
    {2, PIN_PULL_DIS, 3, "PD.17"},
    {2, PIN_PULL_DIS, 3, "PD.18"},
    {2, PIN_PULL_DIS, 3, "PD.19"},
    {2, PIN_PULL_DIS, 3, "PD.20"},
    {2, PIN_PULL_DIS, 3, "PD.21"},
    {2, PIN_PULL_DIS, 3, "PD.22"},
    {2, PIN_PULL_DIS, 3, "PD.23"},
    {2, PIN_PULL_DIS, 3, "PD.24"},
    {2, PIN_PULL_DIS, 3, "PD.25"},
    {2, PIN_PULL_DIS, 3, "PD.26"},
    {2, PIN_PULL_DIS, 3, "PD.27"},
#endif
#ifdef AIC_PRGB_18BIT_LD
    {2, PIN_PULL_DIS, 3, "PD.6"},
    {2, PIN_PULL_DIS, 3, "PD.7"},
    {2, PIN_PULL_DIS, 3, "PD.8"},
    {2, PIN_PULL_DIS, 3, "PD.9"},
    {2, PIN_PULL_DIS, 3, "PD.10"},
    {2, PIN_PULL_DIS, 3, "PD.11"},
    {2, PIN_PULL_DIS, 3, "PD.12"},
    {2, PIN_PULL_DIS, 3, "PD.13"},
    {2, PIN_PULL_DIS, 3, "PD.14"},
    {2, PIN_PULL_DIS, 3, "PD.15"},
    {2, PIN_PULL_DIS, 3, "PD.16"},
    {2, PIN_PULL_DIS, 3, "PD.17"},
    {2, PIN_PULL_DIS, 3, "PD.18"},
    {2, PIN_PULL_DIS, 3, "PD.19"},
    {2, PIN_PULL_DIS, 3, "PD.20"},
    {2, PIN_PULL_DIS, 3, "PD.21"},
    {2, PIN_PULL_DIS, 3, "PD.22"},
    {2, PIN_PULL_DIS, 3, "PD.23"},
    {2, PIN_PULL_DIS, 3, "PD.24"},
    {2, PIN_PULL_DIS, 3, "PD.25"},
    {2, PIN_PULL_DIS, 3, "PD.26"},
    {2, PIN_PULL_DIS, 3, "PD.27"},
#endif
#ifdef AIC_DISP_MIPI_DSI
    {4, PIN_PULL_DIS, 3, "PD.18"},
    {4, PIN_PULL_DIS, 3, "PD.19"},
    {4, PIN_PULL_DIS, 3, "PD.20"},
    {4, PIN_PULL_DIS, 3, "PD.21"},
    {4, PIN_PULL_DIS, 3, "PD.22"},
    {4, PIN_PULL_DIS, 3, "PD.23"},
    {4, PIN_PULL_DIS, 3, "PD.24"},
    {4, PIN_PULL_DIS, 3, "PD.25"},
    {4, PIN_PULL_DIS, 3, "PD.26"},
    {4, PIN_PULL_DIS, 3, "PD.27"},
#endif
#if defined(AIC_DISP_MIPI_DBI) && !defined(LV_USE_SPI_REPLACE_MIPI_DBI)
    /* Legacy DBI/DE QSPI on PD (not used when SPI1 replace is enabled). */
    {2, PIN_PULL_DIS, 3, "PD.20"},
    {2, PIN_PULL_DIS, 3, "PD.21"},
    {2, PIN_PULL_DIS, 3, "PD.22"},
    {2, PIN_PULL_DIS, 3, "PD.23"},
    {2, PIN_PULL_DIS, 3, "PD.24"},
    {1, PIN_PULL_UP,  3, "PD.25"},
    {2, PIN_PULL_UP,  3, "PD.26"},
    {2, PIN_PULL_DIS, 3, "PD.27"},
#endif
#ifdef AIC_PANEL_ENABLE_GPIO
    /* Active-high BL: pull-down + output-low in aic_board_pinmux_init(). */
    {1, PIN_PULL_DOWN, 3, AIC_PANEL_ENABLE_GPIO},
#endif
#ifdef AIC_USING_CTP
    {1, PIN_PULL_DIS, 3, AIC_TOUCH_PANEL_RST_PIN},
    {1, PIN_PULL_DIS, 3, AIC_TOUCH_PANEL_INT_PIN},
#endif
#ifdef AIC_USING_DVP
    {3, PIN_PULL_DIS, 3, "PE.0"},
    {3, PIN_PULL_DIS, 3, "PE.1"},
    {3, PIN_PULL_DIS, 3, "PE.2"},
    {3, PIN_PULL_DIS, 3, "PE.3"},
#ifndef AIC_USING_QSPI1
    /* PE4-PE10 shared with SPI1 LCD; skip when QSPI1 owns them. */
    {3, PIN_PULL_DIS, 3, "PE.4"},
    {3, PIN_PULL_DIS, 3, "PE.5"},
    {3, PIN_PULL_DIS, 3, "PE.6"},
    {3, PIN_PULL_DIS, 3, "PE.7"},
    {3, PIN_PULL_DIS, 3, "PE.8"},
    {3, PIN_PULL_DIS, 3, "PE.9"},
    {3, PIN_PULL_DIS, 3, "PE.10"},
#endif
#endif
#ifdef AIC_USING_XPWM3
    {3, PIN_PULL_DIS, 3, "PE.12"},
#endif
#ifdef AIC_USING_XPWM4
    {3, PIN_PULL_DIS, 3, "PE.13"},
#endif
#ifdef AIC_USING_XPWM6
    {3, PIN_PULL_DIS, 3, "PE.17"},
#endif
#ifdef AIC_USING_XPWM7
    {3, PIN_PULL_DIS, 3, "PE.18"},
#endif
#ifdef AIC_USING_CLK_OUT0
    {4, PIN_PULL_DIS, 3, "PE.11"},
#endif
#if defined(AIC_PANEL_DBI_ZC_T1D8BP) && defined(AIC_USING_QSPI1) && \
    !defined(AIC_EPD_P118F04)
    /* GPIO 1-wire. Idle low, same as before the flash change. */
    {1, PIN_PULL_DOWN, 3, "PD.26"},
    /* Last: LCD_RST stays GPIO. Do not leave PE.11 as CLK_OUT. */
    {1, PIN_PULL_DOWN, 3, "PE.11"},
#endif
#ifdef AIC_EPD_P118F04
    /* KEY1..KEY5: external 10K pull-downs, pressed = high. */
    {1, PIN_PULL_DIS, 3, "PD.23"},
    {1, PIN_PULL_DIS, 3, "PD.24"},
    {1, PIN_PULL_DIS, 3, "PD.25"},
    {1, PIN_PULL_DIS, 3, "PD.26"},
    {1, PIN_PULL_DIS, 3, "PD.27"},
#endif
};

uint32_t aic_pinmux_config_size = ARRAY_SIZE(aic_pinmux_config);

static void demo88_gpio_drive_low(const char *name)
{
    long pin;
    unsigned int g, p;

    pin = hal_gpio_name2pin(name);
    if (pin < 0)
        return;

    g = GPIO_GROUP(pin);
    p = GPIO_GROUP_PIN(pin);
    hal_gpio_set_func(g, p, 1);
    hal_gpio_set_bias_pull(g, p, PIN_PULL_DOWN);
    /* Level first, then output, so the data register cannot glitch high. */
    hal_gpio_clr_output(g, p);
    hal_gpio_direction_output(g, p);
    hal_gpio_clr_output(g, p);
}

static void demo88_lcd_hold_dark(void)
{
#ifdef AIC_PANEL_ENABLE_GPIO
#ifndef AIC_PANEL_ENABLE_GPIO_LOW
    demo88_gpio_drive_low(AIC_PANEL_ENABLE_GPIO);
#endif
#endif
#if !defined(AIC_EPD_P118F04) && \
    (defined(AIC_USING_QSPI1) || defined(AIC_PANEL_DBI_ZC_T1D8BP))
    demo88_gpio_drive_low("PE.11");
#elif !defined(AIC_EPD_P118F04) && defined(AIC_PANEL_ENABLE_GPIO) && \
      !defined(AIC_USING_DVP)
    /* SPL has no QSPI1; still hold J25 LCD_RST so last GRAM stays blank. */
    demo88_gpio_drive_low("PE.11");
#endif
}

void aic_board_pinmux_init(void)
{
    uint32_t i;
    long pin;
    unsigned int g, p;

    demo88_lcd_hold_dark();

    for (i = 0; i < aic_pinmux_config_size; i++) {
        pin = hal_gpio_name2pin(aic_pinmux_config[i].name);
        if (pin < 0)
            continue;
        g = GPIO_GROUP(pin);
        p = GPIO_GROUP_PIN(pin);
        hal_gpio_set_func(g, p, aic_pinmux_config[i].func);
        hal_gpio_set_bias_pull(g, p, aic_pinmux_config[i].bias);
        hal_gpio_set_drive_strength(g, p, aic_pinmux_config[i].drive);
    }

    demo88_lcd_hold_dark();

    /*
     * J7 ARGB is paused. PD.8 is left at reset so it does not drive
     * the level shifter. Q4 gate is biased to 3.3V, PD.8 is the source,
     * and J7 pin2 is the drain.
     */
}
