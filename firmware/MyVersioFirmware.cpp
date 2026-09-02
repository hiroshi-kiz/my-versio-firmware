#include "daisy_versio.h"
#include "daisysp.h"
#include <math.h>
#include <stdint.h>

using namespace daisy;
using namespace daisysp;

// =====================================================================
// ハードウェア呼称（Ruina Versioパネル準拠、実機確認済み）
//   ノブ:  A=Blend  B=Center  C=Phase  D=Fold  E=DOOM  F=Drive  G=8vize
//   トグル: T1=UND/X/OVR(SW_0)  T2=OFF/ON/TRK(SW_1)
//   Smoosh: 1つの物理パーツがボタンとジャックを兼ねる。
//           X-SW=ボタン部分(hw.tap / SwitchPressed)  X-IN=ジャック部分(hw.gate)
//   ジャック: INL/INR=Audio In  OUTL/OUTR=Audio Out
//   LED: L1=LED_0  L2=LED_1  L3=LED_2  L4=LED_3
//
// 役割分担:
//   X-IN  : ドラムのトリガー専用
//   X-SW  : タップテンポ入力
//   INL   : 外部クロック入力(オーディオ入力をゲートのように閾値検出して使う)
//   X-SWとINLはどちらも同じ tempo_period_sec を上書きする。
//   最後に信号があった方が優先される(明示的な排他制御はしない、最も単純な方式)。
// =====================================================================
constexpr int KNOB_A = DaisyVersio::KNOB_0; // Blend      -> OSC基本ピッチ
constexpr int KNOB_B = DaisyVersio::KNOB_6; // Center     -> DELAYグライドタイム
constexpr int KNOB_C = DaisyVersio::KNOB_4; // Phase      -> DELAYフィードバック
constexpr int KNOB_D = DaisyVersio::KNOB_2; // Fold       -> OSCピッチディケイ
constexpr int KNOB_E = DaisyVersio::KNOB_3; // DOOM       -> AMPディケイ
constexpr int KNOB_F = DaisyVersio::KNOB_5; // Drive      -> DELAYミックス
constexpr int KNOB_G = DaisyVersio::KNOB_1; // 8vize      -> OSCピッチEG量

constexpr int SW_T1 = DaisyVersio::SW_0; // UND/X/OVR -> DELAY分周比
constexpr int SW_T2 = DaisyVersio::SW_1; // OFF/ON/TRK -> OSC波形

constexpr size_t LED_L1 = 0;
constexpr size_t LED_L2 = 1;
constexpr size_t LED_L3 = 2;
constexpr size_t LED_L4 = 3;

// =====================================================================
// 起動時の初期テンポ。X-SW(タップテンポ)やINL(外部クロック)で
// 上書きされるまではこの値が使われる。
// =====================================================================
#define TEMPO_BPM 136.0f

// タップ/クロックとして受理する周期の範囲(この外側はノイズや誤操作とみなして無視)
constexpr float kMinTempoPeriodSec = 0.05f; // 1200 BPM相当
constexpr float kMaxTempoPeriodSec = 3.0f;  // 20 BPM相当

// INLのクロック検出しきい値(0〜1、Eurorackのゲート/トリガーを想定)
constexpr float kClockThreshold = 0.3f;

// ディレイバッファの最大長（サンプル数）。48kHz換算で1秒分確保しておけば
// BPM60の4分音符までカバーできる。SDRAM(64MB)に置くことで内蔵SRAMを圧迫しない。
constexpr size_t kMaxDelaySamples = 48000;

DaisyVersio hw;

Oscillator osc;
WhiteNoise noise;
Svf        clap_filter; // ハンドクラップのノイズをバンドパスで色付けする
AdEnv      pitch_env;
AdEnv      amp_env;

static DelayLine<float, kMaxDelaySamples> DSY_SDRAM_BSS delay_line_l;
static DelayLine<float, kMaxDelaySamples> DSY_SDRAM_BSS delay_line_r;

enum Waveform
{
    WAVE_MODE_SINE = 0,
    WAVE_MODE_SQUARE,
    WAVE_MODE_CLAP,
};

// ハンドクラップ用: 短いノイズバーストを3回連打した後、通常のAMPディケイ(E)で
// テールを鳴らす。オフセット/長さは固定値(808/909系クラップの模倣)。
constexpr float kClapBurstOnSec       = 0.004f;
constexpr float kClapBurstOffsetsSec[3] = {0.f, 0.012f, 0.024f};
constexpr float kClapBurstEndSec      = 0.036f; // これ以降はテール(ゲート常時オープン)
constexpr float kClapGateSmoothSec    = 0.002f; // バーストの角を丸めてクリック感を減らす

// ピンポンディレイの左右クロスフィード量。1.0で完全に左右交互(硬いピンポン)、
// 0.5で完全にモノラルへ収束。1.0未満にすると繰り返すたびに徐々にセンターへ
// 収束していく(＝エコーが減衰しながら左右の広がりが狭まる)。
constexpr float kDelayCrossFeed = 0.75f;

float sample_rate;

// X-SW(タップテンポ)とINL(外部クロック)の両方から書き換えられる、
// 現在のテンポ(4分音符の周期、秒)。最後に更新した方が使われる。
volatile float tempo_period_sec = 60.f / TEMPO_BPM;

// 制御レート(メインループ)で更新され、オーディオコールバックから参照される値
volatile int   waveform_mode     = WAVE_MODE_SINE;
volatile float base_freq         = 60.f;
volatile float pitch_depth_oct   = 2.f;
volatile float feedback_amount   = 0.5f;
volatile float delay_mix         = 0.5f;
volatile float delay_glide_coeff = 0.01f;
volatile float delay_time_target_samples = 0.f;
volatile float delay_led_brightness       = 0.f;

float delay_time_current_samples = 0.f;
float delay_led_phase            = 0.f; // 0〜1でディレイ1周期を表す（LED明滅用）

// INL外部クロック検出用(オーディオコールバック内でのみ使用)
float    inl_prev_sample        = 0.f;
uint32_t inl_samples_since_edge = 0;

float clap_gate_smoothed  = 0.f; // バーストゲートの角を丸めた後の値
float clap_gate_smooth_coeff = 0.f; // main()でsample_rate確定後に計算

// ハンドクラップのバースト位置計算用。メインループでのトリガー時に0へ
// リセットし、オーディオコールバックで毎サンプル加算する。
volatile uint32_t clap_elapsed_samples = 0xFFFFFFFF;

// 経過時間に応じて、クラップのノイズバーストが「鳴っている区間」かどうかを返す
float ClapBurstGate(uint32_t elapsed_samples)
{
    float t = elapsed_samples / sample_rate;
    for(int i = 0; i < 3; i++)
    {
        if(t >= kClapBurstOffsetsSec[i] && t < kClapBurstOffsetsSec[i] + kClapBurstOnSec)
            return 1.f;
    }
    return t >= kClapBurstEndSec ? 1.f : 0.f;
}

float ExpMap(float knob01, float min_v, float max_v)
{
    return min_v * powf(max_v / min_v, knob01);
}

// T1(3ポジション、SW_0)でディレイの分周比を選ぶ。切替時はターゲットのみ更新し、
// 実際の遅延時間はオーディオコールバック内でゆっくり追従(グライド)させる。
// これによりBBD/テープディレイ風に、切替の瞬間フィードバック音のピッチが動く。
float SubdivisionSeconds(int sw_pos)
{
    float period = tempo_period_sec;
    switch(sw_pos)
    {
        case Switch3::POS_UP: return period; // 4分音符
        case Switch3::POS_DOWN: return period * 0.25f; // 16分音符
        default: return period * 0.5f; // 8分音符 (POS_CENTER)
    }
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                    AudioHandle::InterleavingOutputBuffer out,
                    size_t                                size)
{
    for(size_t i = 0; i < size; i += 2)
    {
        // INL: オーディオ入力(L)を外部クロックとして扱い、しきい値超えの
        // 立ち上がりエッジ間隔をテンポとして採用する。
        inl_samples_since_edge++;
        float inl_sample = in[i];
        if(inl_sample > kClockThreshold && inl_prev_sample <= kClockThreshold)
        {
            float interval_sec = inl_samples_since_edge / sample_rate;
            if(interval_sec > kMinTempoPeriodSec && interval_sec < kMaxTempoPeriodSec)
                tempo_period_sec = interval_sec;
            inl_samples_since_edge = 0;
        }
        inl_prev_sample = inl_sample;

        delay_time_current_samples
            += (delay_time_target_samples - delay_time_current_samples)
               * delay_glide_coeff;
        delay_line_l.SetDelay(delay_time_current_samples);
        delay_line_r.SetDelay(delay_time_current_samples);

        // ディレイ1周期ごとに明滅させ、フィードバック(タップ)のタイミングを可視化する
        delay_led_phase += 1.f / delay_time_current_samples;
        if(delay_led_phase >= 1.f)
            delay_led_phase -= 1.f;
        delay_led_brightness = expf(-delay_led_phase * 8.f);

        float pitch_val = pitch_env.Process(); // 0(idle/末端) 〜 1(トリガー直後)
        float amp_val   = amp_env.Process();

        float osc_out;
        if(waveform_mode == WAVE_MODE_CLAP)
        {
            clap_filter.Process(noise.Process());
            float gate_target = ClapBurstGate(clap_elapsed_samples);
            clap_gate_smoothed += (gate_target - clap_gate_smoothed) * clap_gate_smooth_coeff;
            osc_out = clap_filter.Band() * clap_gate_smoothed;
        }
        else
        {
            float freq = base_freq * powf(2.f, pitch_val * pitch_depth_oct);
            osc.SetFreq(freq);
            osc_out = osc.Process();
        }
        clap_elapsed_samples++;

        float dry = osc_out * amp_val;

        // ピンポンディレイ: kDelayCrossFeed分だけ左右をクロスフィードする。
        // 1.0未満にすると、繰り返すたびに左右差が少しずつ縮み、
        // エコーが減衰しながら中央へ収束していく。
        float wet_l = delay_line_l.Read();
        float wet_r = delay_line_r.Read();
        float feed_l = wet_l * (1.f - kDelayCrossFeed) + wet_r * kDelayCrossFeed;
        float feed_r = wet_r * (1.f - kDelayCrossFeed) + wet_l * kDelayCrossFeed;
        delay_line_l.Write(dry + feed_l * feedback_amount);
        delay_line_r.Write(feed_r * feedback_amount);

        out[i]     = dry + wet_l * delay_mix;
        out[i + 1] = dry + wet_r * delay_mix;
    }
}

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(4);
    sample_rate = hw.AudioSampleRate();

    osc.Init(sample_rate);
    osc.SetWaveform(Oscillator::WAVE_SIN);
    osc.SetAmp(1.f);
    noise.Init();

    clap_filter.Init(sample_rate);
    clap_filter.SetFreq(1000.f);
    clap_filter.SetRes(0.15f);
    clap_filter.SetDrive(0.f);

    clap_gate_smooth_coeff = 1.f - expf(-1.f / (kClapGateSmoothSec * sample_rate));

    pitch_env.Init(sample_rate);
    pitch_env.SetMin(0.f);
    pitch_env.SetMax(1.f);
    pitch_env.SetTime(ADENV_SEG_ATTACK, 0.001f);
    pitch_env.SetTime(ADENV_SEG_DECAY, 0.1f);

    amp_env.Init(sample_rate);
    amp_env.SetMin(0.f);
    amp_env.SetMax(1.f);
    amp_env.SetTime(ADENV_SEG_ATTACK, 0.001f);
    amp_env.SetTime(ADENV_SEG_DECAY, 0.2f);

    delay_line_l.Init();
    delay_line_r.Init();
    delay_time_target_samples  = SubdivisionSeconds(Switch3::POS_CENTER) * sample_rate;
    delay_time_current_samples = delay_time_target_samples;

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    int      last_sw1          = -1;
    bool     last_switch_state = false; // X-SWの立ち上がりエッジ検出用
    uint32_t last_tap_time_ms  = 0;

    while(1)
    {
        hw.ProcessAnalogControls();
        // hw.UpdateExample()を使わない場合、tap(Smooshボタン)のデバウンス処理は
        // 誰も呼ばないため、明示的に呼び出す必要がある。
        hw.tap.Debounce();

        base_freq       = ExpMap(hw.GetKnobValue(KNOB_A), 30.f, 300.f);   // A: OSC基本ピッチ
        pitch_depth_oct = hw.GetKnobValue(KNOB_G) * 4.f;                  // G: OSCピッチEG量
        pitch_env.SetTime(ADENV_SEG_DECAY,
                           ExpMap(hw.GetKnobValue(KNOB_D), 0.001f, 0.3f)); // D: OSCピッチディケイ
        amp_env.SetTime(ADENV_SEG_DECAY,
                         ExpMap(hw.GetKnobValue(KNOB_E), 0.005f, 1.f));    // E: AMPディケイ
        feedback_amount = hw.GetKnobValue(KNOB_C) * 0.985f;                // C: DELAYフィードバック(上限ギリギリ)
        delay_mix       = hw.GetKnobValue(KNOB_F);                        // F: DELAYミックス

        float glide_time_sec = ExpMap(hw.GetKnobValue(KNOB_B), 0.005f, 0.5f); // B: DELAYグライドタイム
        delay_glide_coeff    = 1.f - expf(-1.f / (glide_time_sec * sample_rate));

        int sw0 = hw.sw[SW_T1].Read(); // T1: DELAY分周比
        // tempo_period_secがX-SW/INLから継続的に更新されるため、
        // T1が変化していなくても毎ループ計算し直す(コストは軽微)。
        delay_time_target_samples = SubdivisionSeconds(sw0) * sample_rate;

        int sw1 = hw.sw[SW_T2].Read(); // T2: OSC波形
        if(sw1 != last_sw1)
        {
            waveform_mode = (sw1 == Switch3::POS_UP)     ? WAVE_MODE_SINE
                            : (sw1 == Switch3::POS_DOWN)  ? WAVE_MODE_CLAP
                                                           : WAVE_MODE_SQUARE;
            osc.SetWaveform(waveform_mode == WAVE_MODE_SQUARE
                                 ? Oscillator::WAVE_POLYBLEP_SQUARE
                                 : Oscillator::WAVE_SIN);
            last_sw1 = sw1;
        }

        // X-SW(Smooshボタン)はSwitchPressed()で押している間ずっとtrueを返す
        // レベル検出のため、立ち上がりエッジ(押した瞬間)だけを自前で検出する。
        bool switch_state  = hw.SwitchPressed();
        bool switch_rising = switch_state && !last_switch_state;
        last_switch_state  = switch_state;

        if(switch_rising) // X-SW: タップテンポ
        {
            uint32_t now_ms   = System::GetNow();
            uint32_t interval = now_ms - last_tap_time_ms;
            float    interval_sec = interval / 1000.f;
            if(interval_sec > kMinTempoPeriodSec && interval_sec < kMaxTempoPeriodSec)
                tempo_period_sec = interval_sec;
            last_tap_time_ms = now_ms;
        }

        if(hw.gate.Trig()) // X-IN: ドラムのトリガー
        {
            pitch_env.Trigger();
            amp_env.Trigger();
            clap_elapsed_samples = 0;
        }

        float hit = amp_env.GetValue();
        hw.SetLed(LED_L1, hit, hit * 0.3f, 0.f); // L1: AMPエンベロープの発音インジケーター
        hw.SetLed(LED_L2, waveform_mode == WAVE_MODE_SINE ? 1.f : 0.f,   // L2: OSC波形(T2)
                  waveform_mode == WAVE_MODE_SQUARE ? 1.f : 0.f,
                  waveform_mode == WAVE_MODE_CLAP ? 1.f : 0.f);
        hw.SetLed(LED_L3, sw0 == Switch3::POS_UP ? 1.f : 0.f,            // L3: DELAY分周比(T1)
                  sw0 == Switch3::POS_CENTER ? 1.f : 0.f,
                  sw0 == Switch3::POS_DOWN ? 1.f : 0.f);
        // L4: 色=フィードバック量(緑→赤)、明滅速度=ディレイタイム(分周比のテンポ)
        float fb_norm = feedback_amount / 0.985f;
        hw.SetLed(LED_L4,
                  fb_norm * delay_led_brightness,
                  (1.f - fb_norm) * delay_led_brightness,
                  0.f);
        hw.UpdateLeds();

        System::Delay(1);
    }
}
