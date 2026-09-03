/**
 * @file joystick_shield.h
 * @brief Arduino JoyStick Shield (Spresense 拡張ボード接続用) 入力ドライバ
 * @details 2軸アナログジョイスティック(A0, A1) および 7つの押しボタン(D2-D8)の入力処理
 */

#ifndef JOYSTICK_SHIELD_H_
#define JOYSTICK_SHIELD_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* JoyStick Shield 標準ピンアサイン (Spresense 拡張ボード Arduino互換ピン)   */
/* ========================================================================= */
#define JOYSTICK_PIN_AXIS_X      (0)   /* A0: ジョイスティック X軸 (左右) */
#define JOYSTICK_PIN_AXIS_Y      (1)   /* A1: ジョイスティック Y軸 (上下) */
#define JOYSTICK_ADC_DEV_PATH    "/dev/adc0"

/* ADC チャンネル割当と正規化パラメータ (Spresense 16bit LPADC 想定) */
#define JOYSTICK_ADC_CH_X        (0)
#define JOYSTICK_ADC_CH_Y        (1)
#define JOYSTICK_ADC_CENTER      (32768.0f)
#define JOYSTICK_ADC_FULLSCALE   (30000.0f)
#define JOYSTICK_STICK_DEADZONE  (0.20f)

#ifdef __NuttX__
#include <arch/chip/pin.h>
#define JOYSTICK_PIN_BTN_A       PIN_HIF_IRQ_OUT   /* D2: ボタン A (UP / 上) */
#define JOYSTICK_PIN_BTN_B       PIN_PWM3          /* D3: ボタン B (RIGHT / 右) */
#define JOYSTICK_PIN_BTN_C       PIN_SPI2_MOSI     /* D4: ボタン C (DOWN / 下) */
#define JOYSTICK_PIN_BTN_D       PIN_PWM1          /* D5: ボタン D (LEFT / 左) */
#define JOYSTICK_PIN_BTN_E       PIN_PWM0          /* D6: ボタン E (小型ボタンスイッチ 1) */
#define JOYSTICK_PIN_BTN_F       PIN_SPI3_CS1_X    /* D7: ボタン F (小型ボタンスイッチ 2) */
#define JOYSTICK_PIN_BTN_STICK   PIN_SPI2_MISO     /* D8: ジョイスティック押し込みボタン (K) */
#else
#define JOYSTICK_PIN_BTN_A       (2)   /* D2: ボタン A (UP / 上) */
#define JOYSTICK_PIN_BTN_B       (3)   /* D3: ボタン B (RIGHT / 右) */
#define JOYSTICK_PIN_BTN_C       (4)   /* D4: ボタン C (DOWN / 下) */
#define JOYSTICK_PIN_BTN_D       (5)   /* D5: ボタン D (LEFT / 左) */
#define JOYSTICK_PIN_BTN_E       (6)   /* D6: ボタン E (小型ボタンスイッチ 1) */
#define JOYSTICK_PIN_BTN_F       (7)   /* D7: ボタン F (小型ボタンスイッチ 2) */
#define JOYSTICK_PIN_BTN_STICK   (8)   /* D8: ジョイスティック押し込みボタン (K) */
#endif

/**
 * @brief ボタンビットマスク定義
 */
typedef enum {
    BTN_NONE       = 0,
    BTN_MASK_A     = (1 << 0), /**< ボタン A (D2) */
    BTN_MASK_B     = (1 << 1), /**< ボタン B (D3) */
    BTN_MASK_C     = (1 << 2), /**< ボタン C (D4) */
    BTN_MASK_D     = (1 << 3), /**< ボタン D (D5) */
    BTN_MASK_E     = (1 << 4), /**< ボタン E (D6) */
    BTN_MASK_F     = (1 << 5), /**< ボタン F (D7) */
    BTN_MASK_STICK = (1 << 6)  /**< ジョイスティック押し込み (D8) */
} JoystickButtonMask;

/**
 * @brief ジョイスティック＆ボタンの入力状態構造体
 */
typedef struct {
    /* アナログスティック (-1.0f 〜 +1.0f, センターが0.0f) */
    float stick_x;
    float stick_y;

    /* ボタン生データ（押下時 true） */
    bool btn_a;
    bool btn_b;
    bool btn_c;
    bool btn_d;
    bool btn_e;
    bool btn_f;
    bool btn_stick;

    /* イベントフラグ (前回のpollからの変化) */
    uint8_t pressed_buttons;   /**< 新たに押されたボタン (ビットマスク) */
    uint8_t released_buttons;  /**< 離されたボタン (ビットマスク) */
    uint8_t current_buttons;   /**< 現在押されているボタン (ビットマスク) */
} JoystickState;

/**
 * @brief ジョイスティックシールドドライバの初期化
 * @return 成功時は 0, 失敗時は負のエラーコード
 */
int joystick_shield_init(void);

/**
 * @brief 入力状態のポーリング更新
 * @param state 入力状態を格納する構造体ポインタ
 * @return 成功時は 0
 */
int joystick_shield_poll(JoystickState *state);

/**
 * @brief ドライバの解放
 */
void joystick_shield_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* JOYSTICK_SHIELD_H_ */
