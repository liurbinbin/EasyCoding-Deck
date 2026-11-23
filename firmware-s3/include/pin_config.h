#pragma once

namespace tkeyboard::pins {
constexpr int i2c_scl = 2, i2c_sda = 3, pmic_irq = 21;
constexpr int key1 = 10, key2 = 14, key3 = 13, key4 = 12, key5 = 0;
constexpr int lcd_cs[] = {48, 38, 7, 42, 11};
constexpr int lcd_clk = 41, lcd_mosi = 40, lcd_dc = 39, lcd_rst = 46, lcd_backlight = 1;
constexpr int encoder_a = 4, encoder_b = 5, ws2812 = 47;
constexpr int i2s_mclk = 16, i2s_bclk = 17, i2s_ws = 18, i2s_din = 15, i2s_dout = 8;
constexpr int es7210_int = 9, pa_enable = 45, aux_detect = 6;
constexpr int qwiic_tx = 43, qwiic_rx = 44;
}

