/**
 * @file joystick_shield.c
 * @brief JoyStick Shield ハードウェアドライバ実装 (30ms デバウンスフィルタ対応)
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <errno.h>

#if !defined(_WIN32) && !defined(_MSC_VER)
#include <unistd.h>
#endif

#ifdef __NuttX__
#include <nuttx/config.h>
#include <sys/ioctl.h>
#include <nuttx/analog/ioctl.h>
#include <nuttx/analog/adc.h>
#include <arch/chip/pin.h>
#include <arch/board/board.h>
#ifdef CONFIG_CXD56_ADC
#include <arch/chip/scu.h>
#include <arch/chip/adc.h>
#endif
#endif

#include "joystick_shield.h"

#ifdef __NuttX__
static int s_adc_fd_x = -1;
static int s_adc_fd_y = -1;
static bool s_adc_is_lpadc = false;
static float s_stick_x = 0.0f;
static float s_stick_y = 0.0f;
static float s_center_x = JOYSTICK_ADC_CENTER;
static float s_center_y = JOYSTICK_ADC_CENTER;

#ifndef ANIOC_CXD56_START
#define ANIOC_CXD56_START _ANIOC(AN_FIRST + AN_NCMDS + 0)
#endif
#ifndef SCUIOC_SETFIFOMODE
/* _SCUIOC(0x0012)=_IOC(0xa000,0x0012)=0xa0000012 . ヘッダ依存を排除した数値定義 */
#define SCUIOC_SETFIFOMODE 0xa0000012
#endif

/**
 * @brief ADC (A0/A1) を読み、デッドゾーン処理した -1.0〜+1.0 のスティック値を更新
 */
static void update_stick_from_adc(void)
{
    if (s_adc_fd_x < 0 && s_adc_fd_y < 0) return;

    bool got_x = false, got_y = false;
    float x = s_stick_x, y = s_stick_y;
    int32_t raw_x = 0, raw_y = 0;

    if (s_adc_is_lpadc) {
        /* Spresense LPADC (/dev/lpadc0, /dev/lpadc1) */
        if (s_adc_fd_x >= 0) {
            uint16_t sample_x = 0;
            ssize_t n = read(s_adc_fd_x, &sample_x, sizeof(sample_x));
            if (n >= (ssize_t)sizeof(uint16_t)) {
                raw_x = (int32_t)sample_x;
                x = ((float)raw_x - s_center_x) / JOYSTICK_ADC_FULLSCALE;
                got_x = true;
            }
        }
        if (s_adc_fd_y >= 0) {
            uint16_t sample_y = 0;
            ssize_t n = read(s_adc_fd_y, &sample_y, sizeof(sample_y));
            if (n >= (ssize_t)sizeof(uint16_t)) {
                raw_y = (int32_t)sample_y;
                y = ((float)raw_y - s_center_y) / JOYSTICK_ADC_FULLSCALE;
                got_y = true;
            }
        }
    } else {
        /* 汎用 /dev/adc0 */
        struct adc_msg_s msgs[16];
        ssize_t nread = read(s_adc_fd_x, msgs, sizeof(msgs));
        ioctl(s_adc_fd_x, ANIOC_TRIGGER, 0);

        if (nread > 0) {
            size_t count = (size_t)nread / sizeof(struct adc_msg_s);
            for (size_t i = 0; i < count; i++) {
                if (msgs[i].am_channel == JOYSTICK_ADC_CH_X && !got_x) {
                    raw_x = (int32_t)msgs[i].am_data;
                    x = ((float)raw_x - s_center_x) / JOYSTICK_ADC_FULLSCALE;
                    got_x = true;
                } else if (msgs[i].am_channel == JOYSTICK_ADC_CH_Y && !got_y) {
                    raw_y = (int32_t)msgs[i].am_data;
                    y = ((float)raw_y - s_center_y) / JOYSTICK_ADC_FULLSCALE;
                    got_y = true;
                }
            }
        }
    }

    /* 中立オートリキャリブレーション */
    const float band = JOYSTICK_ADC_FULLSCALE * 0.04f;
    static uint16_t neutral_frames = 0;
    if (got_x && got_y &&
        fabsf((float)raw_x - s_center_x) < band &&
        fabsf((float)raw_y - s_center_y) < band) {
        if (neutral_frames < 0xFFFFu) neutral_frames++;
        if (neutral_frames >= 940u) { /* ~10s @ 94Hz */
            if ((float)raw_x > s_center_x) s_center_x += 1.0f;
            else if ((float)raw_x < s_center_x) s_center_x -= 1.0f;
            if ((float)raw_y > s_center_y) s_center_y += 1.0f;
            else if ((float)raw_y < s_center_y) s_center_y -= 1.0f;
        }
    } else {
        neutral_frames = 0;
    }

    /* Y軸は上方向で音量アップ/高音となるよう反転 (ADC: 上=小さい値) */
    if (got_x) s_stick_x = x;
    if (got_y) s_stick_y = -y;
}

/**
 * @brief 起動時オートキャリブレーション
 */
static void calibrate_stick_center(void)
{
    if (s_adc_fd_x < 0 && s_adc_fd_y < 0) return;

    float sum_x = 0.0f, sum_y = 0.0f;
    int valid_x = 0, valid_y = 0;

    for (int attempt = 0; attempt < 40 && (valid_x < 16 || valid_y < 16); attempt++) {
        if (s_adc_is_lpadc) {
            if (s_adc_fd_x >= 0 && valid_x < 64) {
                uint16_t sx = 0;
                if (read(s_adc_fd_x, &sx, sizeof(sx)) >= (ssize_t)sizeof(uint16_t)) {
                    sum_x += (float)sx;
                    valid_x++;
                }
            }
            if (s_adc_fd_y >= 0 && valid_y < 64) {
                uint16_t sy = 0;
                if (read(s_adc_fd_y, &sy, sizeof(sy)) >= (ssize_t)sizeof(uint16_t)) {
                    sum_y += (float)sy;
                    valid_y++;
                }
            }
        } else {
            ioctl(s_adc_fd_x, ANIOC_TRIGGER, 0);
            usleep(2000);
            struct adc_msg_s msgs[16];
            ssize_t nread = read(s_adc_fd_x, msgs, sizeof(msgs));
            if (nread > 0) {
                size_t count = (size_t)nread / sizeof(struct adc_msg_s);
                for (size_t i = 0; i < count; i++) {
                    if (msgs[i].am_channel == JOYSTICK_ADC_CH_X && valid_x < 64) {
                        sum_x += (float)msgs[i].am_data;
                        valid_x++;
                    } else if (msgs[i].am_channel == JOYSTICK_ADC_CH_Y && valid_y < 64) {
                        sum_y += (float)msgs[i].am_data;
                        valid_y++;
                    }
                }
            }
        }
        usleep(3000);
    }

    if (valid_x > 0) s_center_x = sum_x / (float)valid_x;
    if (valid_y > 0) s_center_y = sum_y / (float)valid_y;

    printf("[JOYSTICK] Stick center calibrated: X=%.0f Y=%.0f (samples: %d/%d)\n",
           s_center_x, s_center_y, valid_x, valid_y);
}

/**
 * @brief デッドゾーン処理とクランプ (-1.0 〜 +1.0)
 */
static float apply_deadzone(float v)
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    float dz = JOYSTICK_STICK_DEADZONE;
    if (v > dz)       return (v - dz) / (1.0f - dz);
    else if (v < -dz) return (v + dz) / (1.0f - dz);
    return 0.0f;
}
#endif

static uint8_t s_last_raw_btns = 0;
static uint8_t s_debounced_buttons = 0;
static uint8_t s_last_debounced_buttons = 0;
static uint8_t s_debounce_count = 0;
static uint8_t s_btn_pin_ok = 0;     /**< bit: GPIO 設定に成功したボタンのみ 1 */

int joystick_shield_init(void)
{
#ifdef __NuttX__
    /* Spresense 拡張ボードの D2〜D8 ピンを GPIO 入力 (プルアップ有効) として設定
     * ※ Spresense の GPIO は 3.3V 駆動・5V 非耐圧。
     *   シールド側電圧セレクタは必ず 3.3V 側に設定すること。
     *   本ドライバは常に入力モードのみ使用し、ピンを HIGH 出力することはない。 */
    const uint32_t btn_pins[] = {
        JOYSTICK_PIN_BTN_A,
        JOYSTICK_PIN_BTN_B,
        JOYSTICK_PIN_BTN_C,
        JOYSTICK_PIN_BTN_D,
        JOYSTICK_PIN_BTN_E,
        JOYSTICK_PIN_BTN_F,
        JOYSTICK_PIN_BTN_STICK
    };
    const size_t num_pins = sizeof(btn_pins) / sizeof(btn_pins[0]);
    const uint8_t btn_masks[7] = {
        BTN_MASK_A, BTN_MASK_B, BTN_MASK_C, BTN_MASK_D,
        BTN_MASK_E, BTN_MASK_F, BTN_MASK_STICK
    };

    s_btn_pin_ok = 0;
    size_t ok_count = 0;
    for (size_t i = 0; i < num_pins; i++) {
        int ret = board_gpio_config(btn_pins[i], 0, true, false, PIN_PULLUP);
        if (ret < 0) {
            /* ピンが他ドライバに確保されている等の失敗。そのピンは以降
             * 読みに行かない (他ドライバの Low 出力が「押しっぱなし」と
             * 解釈され幽霊ボタン化するのを防ぐ) */
            printf("[JOYSTICK] Warning: GPIO config failed (pin=%u, ret=%d)\n",
                   (unsigned int)btn_pins[i], ret);
        } else {
            s_btn_pin_ok |= btn_masks[i];
            ok_count++;
        }
    }
    if (ok_count == 0) {
        printf("[JOYSTICK] Error: all button GPIO configs failed.\n");
        return -EIO;
    }

    /* 未使用GPIOのシンク設定 (弱プルダウン):
     * 拡張ボード上の浮動ピン D9-D13 (SPI4/PWM2) を GND に安定化して高周波アンテナ化・ノイズ混入を防止。
     * ※ CXD5247 CODEC 制御線 (SPI3_CS0_X 等)、I2S0/MCLK、UART2、SDIO、D2-D8、A0/A1 は絶対に触らない。 */
    {
        const struct {
            uint32_t pin;
            const char *name;
        } base_sink_pins[] = {
            { PIN_PWM2,      "D9/PWM2" },
#if !defined(CONFIG_CXD56_SPISD) || !defined(CONFIG_CXD56_SPISD_SPI_CH) || (CONFIG_CXD56_SPISD_SPI_CH != 4)
            { PIN_SPI4_CS_X, "D10/SPI4_CS" },
            { PIN_SPI4_MOSI, "D11/SPI4_MOSI" },
            { PIN_SPI4_MISO, "D12/SPI4_MISO" },
            { PIN_SPI4_SCK,  "D13/SPI4_SCK" },
#endif
        };
        for (size_t i = 0; i < sizeof(base_sink_pins)/sizeof(base_sink_pins[0]); i++) {
            board_gpio_config(base_sink_pins[i].pin, 0, true, false, PIN_PULLDOWN);
        }
    }

#ifdef CONFIG_CXD56_ADC
    cxd56_adcinitialize();
#endif

    s_adc_fd_x = open("/dev/lpadc0", O_RDONLY | O_NONBLOCK);
    s_adc_fd_y = open("/dev/lpadc1", O_RDONLY | O_NONBLOCK);
    if (s_adc_fd_x >= 0 && s_adc_fd_y >= 0) {
        s_adc_is_lpadc = true;
        ioctl(s_adc_fd_x, SCUIOC_SETFIFOMODE, 1);
        ioctl(s_adc_fd_x, ANIOC_CXD56_START, 0);
        ioctl(s_adc_fd_y, SCUIOC_SETFIFOMODE, 1);
        ioctl(s_adc_fd_y, ANIOC_CXD56_START, 0);
        printf("[JOYSTICK] Opened Spresense LPADC: /dev/lpadc0 (X) and /dev/lpadc1 (Y)\n");
    } else {
        /* フォールバック: /dev/adc0 */
        if (s_adc_fd_x >= 0) { close(s_adc_fd_x); s_adc_fd_x = -1; }
        if (s_adc_fd_y >= 0) { close(s_adc_fd_y); s_adc_fd_y = -1; }
        s_adc_fd_x = open(JOYSTICK_ADC_DEV_PATH, O_RDONLY | O_NONBLOCK);
        s_adc_is_lpadc = false;
        if (s_adc_fd_x >= 0) {
            printf("[JOYSTICK] Opened standard ADC: %s\n", JOYSTICK_ADC_DEV_PATH);
        } else {
            printf("[JOYSTICK] Warning: No ADC device available (/dev/lpadc0 or %s)\n",
                   JOYSTICK_ADC_DEV_PATH);
        }
    }

    calibrate_stick_center();
#endif

    s_last_raw_btns = 0;
    s_debounced_buttons = 0;
    s_last_debounced_buttons = 0;
    s_debounce_count = 0;
#ifdef __NuttX__
    s_stick_x = 0.0f;
    s_stick_y = 0.0f;
#endif

    printf("[JOYSTICK] Initialized JoyStick Shield driver.\n");
    return 0;
}

int joystick_shield_poll(JoystickState *state)
{
    if (!state) return -EINVAL;

    memset(state, 0, sizeof(JoystickState));
    uint8_t raw_btns = 0;

#ifdef __NuttX__
    /* Active-LOW 読み取り (押下時に 0 -> 反転して 1)。
     * 設定に失敗したピンは読みに行かず「解放」扱い */
    if ((s_btn_pin_ok & BTN_MASK_A) && !board_gpio_read(JOYSTICK_PIN_BTN_A))         raw_btns |= BTN_MASK_A;
    if ((s_btn_pin_ok & BTN_MASK_B) && !board_gpio_read(JOYSTICK_PIN_BTN_B))         raw_btns |= BTN_MASK_B;
    if ((s_btn_pin_ok & BTN_MASK_C) && !board_gpio_read(JOYSTICK_PIN_BTN_C))         raw_btns |= BTN_MASK_C;
    if ((s_btn_pin_ok & BTN_MASK_D) && !board_gpio_read(JOYSTICK_PIN_BTN_D))         raw_btns |= BTN_MASK_D;
    if ((s_btn_pin_ok & BTN_MASK_E) && !board_gpio_read(JOYSTICK_PIN_BTN_E))         raw_btns |= BTN_MASK_E;
    if ((s_btn_pin_ok & BTN_MASK_F) && !board_gpio_read(JOYSTICK_PIN_BTN_F))         raw_btns |= BTN_MASK_F;
    if ((s_btn_pin_ok & BTN_MASK_STICK) && !board_gpio_read(JOYSTICK_PIN_BTN_STICK)) raw_btns |= BTN_MASK_STICK;

    update_stick_from_adc();
    state->stick_x = apply_deadzone(s_stick_x);
    state->stick_y = apply_deadzone(s_stick_y);
#else
    state->stick_x = 0.0f;
    state->stick_y = 0.0f;
#endif

    /* ソフトウェアデバウンス (チャタリング防止フィルタ)
     * カウンタは安定継続中に無限加算すると uint8 ラップするため飽和させる */
    if (raw_btns == s_last_raw_btns) {
        if (s_debounce_count < 200u) s_debounce_count++;
        if (s_debounce_count >= 2) { /* 2サンプル連続一致で確定 */
            s_debounced_buttons = raw_btns;
        }
    } else {
        s_debounce_count = 0;
        s_last_raw_btns = raw_btns;
    }

    /* 個別ボタンフラグの設定 */
    state->btn_a     = (s_debounced_buttons & BTN_MASK_A) != 0;
    state->btn_b     = (s_debounced_buttons & BTN_MASK_B) != 0;
    state->btn_c     = (s_debounced_buttons & BTN_MASK_C) != 0;
    state->btn_d     = (s_debounced_buttons & BTN_MASK_D) != 0;
    state->btn_e     = (s_debounced_buttons & BTN_MASK_E) != 0;
    state->btn_f     = (s_debounced_buttons & BTN_MASK_F) != 0;
    state->btn_stick = (s_debounced_buttons & BTN_MASK_STICK) != 0;

    state->current_buttons  = s_debounced_buttons;
    state->pressed_buttons  = (s_debounced_buttons ^ s_last_debounced_buttons) & s_debounced_buttons;
    state->released_buttons = (s_debounced_buttons ^ s_last_debounced_buttons) & s_last_debounced_buttons;

    s_last_debounced_buttons = s_debounced_buttons;
    return 0;
}

void joystick_shield_deinit(void)
{
#ifdef __NuttX__
    if (s_adc_fd_x >= 0) {
        close(s_adc_fd_x);
        s_adc_fd_x = -1;
    }
    if (s_adc_fd_y >= 0) {
        close(s_adc_fd_y);
        s_adc_fd_y = -1;
    }
#endif
}
