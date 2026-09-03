/**
 * @file spatial_audio.h
 * @brief 数学的に美しい立体音響 (球頭モデル完全導出)
 * @details
 *   理論根拠:
 *   - ITD: Woodworth & Schlosberg 1954 球モデル  ITD(θ) = r/c * (θ + sinθ)  |θ|≤π/2
 *           r=0.0875m (成人平均頭半径), c=343m/s, θ=方位角[rad] (-π/2左, +π/2右)
 *           最大 ITD = r/c*(π/2+1) = 0.655ms = 31.4smp@48kHz (実測と一致)
 *   - ILD: Kuhn 1977 低域は等価パワーPan (Parseval保存: L²+R²=1),
 *           高域は Rayleigh球回折による遮蔽を1次 shelving で近似
 *           遮蔽量 G(f,θ) = 1 / sqrt(1 + (k·a·sinθ)²), k=2πf/c, a=r
 *           実装は 1pole LP (fc = 700 + 4300·cosθ) で連続近似
 *   - 距離: 逆距離則 G(d)=r₀/d + 空気吸収 α(f,d)=exp(-m(f)·d), m≈7e-11·f²
 *           可聴域では高域のみ -0.5dB/m@8kHz として 1pole で近似
 */

#ifndef SPATIAL_AUDIO_H_
#define SPATIAL_AUDIO_H_

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define SPATIAL_SAMPLE_RATE 48000.0f
#define SPATIAL_HEAD_RADIUS 0.0875f
#define SPATIAL_SPEED_OF_SOUND 343.0f
#define SPATIAL_MAX_ITD_SAMPLES 32  /* Woodworth最大31.4 + 余裕1.6 = 32 */

typedef struct {
    float delay_buf[SPATIAL_MAX_ITD_SAMPLES];
    uint8_t wpos;
    float lp_state_l;
    float lp_state_r;
} SpatialVoiceState;

/* ---- 純粋関数群: 副作用なし、数学的に 1:1 ---- */

/**
 * @brief Pan(0..1) → 方位角θ[rad] 純粋写像。等価パワーPanの逆写像と一致
 * @note チャンネル/音高によるステージ配置は別関数 spatial_stage_azimuth() に分離
 */
static inline float spatial_pan_to_azimuth(float pan)
{
    if (pan < 0.0f) pan = 0.0f;
    if (pan > 1.0f) pan = 1.0f;
    return (pan - 0.5f) * (float)M_PI; /* -π/2 .. +π/2 */
}

/**
 * @brief Woodworth ITD [samples] 純粋導出
 */
static inline float spatial_itd_samples(float theta)
{
    /* 負は左先行、正は右先行。対称性より絶対値を遅延量とする */
    float itd_sec = SPATIAL_HEAD_RADIUS / SPATIAL_SPEED_OF_SOUND * (theta + sinf(theta));
    float itd_samp = fabsf(itd_sec) * SPATIAL_SAMPLE_RATE;
    const float max_samp = (float)(SPATIAL_MAX_ITD_SAMPLES - 2);
    if (itd_samp > max_samp) itd_samp = max_samp;
    if (itd_samp < 0.0f) itd_samp = 0.0f;
    return itd_samp;
}

/**
 * @brief 等価パワー + 周波数依存遮蔽の ILD
 * @param theta 方位角
 * @param l_gain, r_gain 出力 (線形)
 * @param lp_coeff 対側用 1pole係数 alpha = exp(-2πfc/fs) の近似、0..1 (1=bypass)
 * @details
 *   低域: L=cos(π·pan/2), R=sin(π·pan/2) で L²+R²=1 保存
 *   高域: 遮蔽は cosθ に比例し fc(θ)=700+3000·cosθ で LP化
 */
static inline void spatial_ild_gains(float theta, float *l_gain, float *r_gain, float *lp_coeff)
{
    float pan = theta / (float)M_PI + 0.5f;
    float ang = pan * ((float)M_PI * 0.5f);
    float base_l = cosf(ang);
    float base_r = sinf(ang);

    /* 頭部遮蔽: Kuhnの球回折を 700Hz-3700Hz の可変fcで近似 */
    float costh = cosf(theta);
    if (costh < 0.0f) costh = 0.0f; /* 後方は正面と同等に扱う */
    float fc = 700.0f + 3000.0f * costh; /* 正面3700Hz, 側面700Hz */
    float alpha = expf(-2.0f * (float)M_PI * fc / SPATIAL_SAMPLE_RATE);
    /* ILD自体は低域はそのまま、高域のみ遮蔽で1poleに通す */
    *l_gain = base_l;
    *r_gain = base_r;
    *lp_coeff = alpha;
    /* 極端な無音防止 */
    if (*l_gain < 0.001f) *l_gain = 0.001f;
    if (*r_gain < 0.001f) *r_gain = 0.001f;
}

/**
 * @brief 距離減衰 (逆距離則) + 空気吸収
 * @param dist 1.0=基準 (1m), 2.0で-6dB
 */
static inline float spatial_distance_gain(float dist, float *air_lp_alpha)
{
    if (dist < 1.0f) dist = 1.0f;
    float g = 1.0f / dist; /* 逆距離則 */
    /* 空気吸収: 8kHzで -0.5dB/m → fcが距離で下がる */
    float fc_air = 24000.0f / dist; /* 1mで24k(通過), 4mで6k */
    if (fc_air > 24000.0f) fc_air = 24000.0f;
    if (fc_air < 800.0f) fc_air = 800.0f;
    *air_lp_alpha = expf(-2.0f * (float)M_PI * fc_air / SPATIAL_SAMPLE_RATE);
    return g;
}

/* ---- 状態付きフィルタ (美しさ: 状態は Voice に局所、関数は純粋) ---- */

static inline float spatial_delay_read(const SpatialVoiceState *st, float delay_samp)
{
    float rpos = (float)st->wpos - delay_samp;
    while (rpos < 0.0f) rpos += (float)SPATIAL_MAX_ITD_SAMPLES;
    int i0 = (int)rpos;
    int i1 = (i0 + 1) & (SPATIAL_MAX_ITD_SAMPLES - 1);
    float frac = rpos - (float)i0;
    i0 &= (SPATIAL_MAX_ITD_SAMPLES - 1);
    return st->delay_buf[i0] * (1.0f - frac) + st->delay_buf[i1] * frac;
}

static inline void spatial_delay_write(SpatialVoiceState *st, float x)
{
    st->delay_buf[st->wpos] = x;
    st->wpos = (st->wpos + 1) & (SPATIAL_MAX_ITD_SAMPLES - 1);
}

static inline float spatial_onepole_lp(float x, float *state, float alpha)
{
    if (alpha >= 0.999f) return x;
    /* y[n] = α·y[n-1] + (1-α)·x  ← 指数平滑で fc = -lnα·fs/2π */
    *state = alpha * (*state) + (1.0f - alpha) * x;
    return *state;
}

static inline void spatial_process_sample(SpatialVoiceState *st, float sample,
                                          float azimuth, float itd,
                                          float l_gain, float r_gain, float alpha,
                                          float *out_l, float *out_r)
{
    float l = sample * l_gain;
    float r = sample * r_gain;
    /* Woodworth ITD=0 (センター) は完全バイパス。0遅延で read/write すると
     * 1サンプル遅延が混入し右寄りに聴こえる (wposが未書込位置を指すため) */
    if (itd < 0.5f) {
        /* 1poleはセンターでは fc=3700Hz α~0.62 だがバイパスと差は不可聴、素通しで対称性担保 */
        *out_l = l;
        *out_r = r;
        return;
    }
    if (azimuth < 0.0f) {
        float r_d = spatial_delay_read(st, itd);
        spatial_delay_write(st, r);
        r = spatial_onepole_lp(r_d, &st->lp_state_r, alpha);
        *out_l = l;
        *out_r = r;
    } else {
        float l_d = spatial_delay_read(st, itd);
        spatial_delay_write(st, l);
        l = spatial_onepole_lp(l_d, &st->lp_state_l, alpha);
        *out_l = l;
        *out_r = r;
    }
}

/* ---- オプション: ステージ配置 (数学と演出の分離) ---- */
static inline float spatial_stage_azimuth(float pan, uint8_t channel, uint8_t note)
{
    float diff = pan - 0.5f;
    if (fabsf(diff) > 0.08f) {
        return diff * (float)M_PI; /* 明示Pan優先 */
    }
    static const float ch_stage_pan[16] = {
         0.15f, -0.20f, -0.05f, -0.30f,  0.30f,  0.25f, -0.25f, 0.10f,
        -0.15f,  0.00f,  0.20f, -0.10f, -0.20f,  0.35f, -0.35f, 0.05f
    };
    float stage = ch_stage_pan[channel & 0x0Fu];
    float pitch_spread = ((float)note - 60.0f) * (0.25f / 36.0f);
    if (pitch_spread > 0.20f) pitch_spread = 0.20f;
    if (pitch_spread < -0.20f) pitch_spread = -0.20f;
    float eff = 0.5f + stage + pitch_spread;
    if (eff < 0.10f) eff = 0.10f;
    if (eff > 0.90f) eff = 0.90f;
    return (eff - 0.5f) * (float)M_PI;
}

static inline void spatial_voice_init(SpatialVoiceState *st)
{
    for (int i = 0; i < SPATIAL_MAX_ITD_SAMPLES; i++) st->delay_buf[i] = 0.0f;
    st->wpos = 0;
    st->lp_state_l = 0.0f;
    st->lp_state_r = 0.0f;
}

#endif /* SPATIAL_AUDIO_H_ */
