#include "daisy_versio.h"
#include "daisysp.h"
#include <math.h>

using namespace daisy;
using namespace daisysp;

// =====================================================================
// テンポ設定
// 外部クロックには依存せず、ここで固定する。変更したい場合はこの値を
// 書き換えてビルド・書き込みし直す（DELAYタイムはこのBPMの分周比で決まる）。
// =====================================================================
#define TEMPO_BPM 136.0f

// ディレイバッファの最大長（サンプル数）。48kHz換算で1秒分確保しておけば
// BPM60の4分音符までカバーできる。SDRAM(64MB)に置くことで内蔵SRAMを圧迫しない。
constexpr size_t kMaxDelaySamples = 48000;

DaisyVersio hw;

Oscillator osc;
WhiteNoise noise;
AdEnv      pitch_env;
AdEnv      amp_env;

static DelayLine<float, kMaxDelaySamples> DSY_SDRAM_BSS delay_line;

enum Waveform
{
    WAVE_MODE_SINE = 0,
    WAVE_MODE_SQUARE,
    WAVE_MODE_NOISE,
};

float sample_rate;
float quarter_note_sec;

// 制御レート(メインループ)で更新され、オーディオコールバックから参照される値
volatile bool  trigger_pending   = false;
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

float ExpMap(float knob01, float min_v, float max_v)
{
    return min_v * powf(max_v / min_v, knob01);
}

// SW_0(3ポジション)でディレイの分周比を選ぶ。切替時はターゲットのみ更新し、
// 実際の遅延時間はオーディオコールバック内でゆっくり追従(グライド)させる。
// これによりBBD/テープディレイ風に、切替の瞬間フィードバック音のピッチが動く。
float SubdivisionSeconds(int sw_pos)
{
    switch(sw_pos)
    {
        case Switch3::POS_UP: return quarter_note_sec; // 4分音符
        case Switch3::POS_DOWN: return quarter_note_sec * 0.25f; // 16分音符
        default: return quarter_note_sec * 0.5f; // 8分音符 (POS_CENTER)
    }
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                    AudioHandle::InterleavingOutputBuffer out,
                    size_t                                size)
{
    for(size_t i = 0; i < size; i += 2)
    {
        delay_time_current_samples
            += (delay_time_target_samples - delay_time_current_samples)
               * delay_glide_coeff;
        delay_line.SetDelay(delay_time_current_samples);

        // ディレイ1周期ごとに明滅させ、フィードバック(タップ)のタイミングを可視化する
        delay_led_phase += 1.f / delay_time_current_samples;
        if(delay_led_phase >= 1.f)
            delay_led_phase -= 1.f;
        delay_led_brightness = expf(-delay_led_phase * 8.f);

        float pitch_val = pitch_env.Process(); // 0(idle/末端) 〜 1(トリガー直後)
        float amp_val   = amp_env.Process();

        float osc_out;
        if(waveform_mode == WAVE_MODE_NOISE)
        {
            osc_out = noise.Process();
        }
        else
        {
            float freq = base_freq * powf(2.f, pitch_val * pitch_depth_oct);
            osc.SetFreq(freq);
            osc_out = osc.Process();
        }

        float dry = osc_out * amp_val;
        float wet = delay_line.Read();
        delay_line.Write(dry + wet * feedback_amount);

        float mixed = dry + wet * delay_mix;

        out[i]     = mixed;
        out[i + 1] = mixed;
    }
}

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(4);
    sample_rate      = hw.AudioSampleRate();
    quarter_note_sec = 60.f / TEMPO_BPM;

    osc.Init(sample_rate);
    osc.SetWaveform(Oscillator::WAVE_SIN);
    osc.SetAmp(1.f);
    noise.Init();

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

    delay_line.Init();
    delay_time_target_samples  = SubdivisionSeconds(Switch3::POS_CENTER) * sample_rate;
    delay_time_current_samples = delay_time_target_samples;

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    int  last_sw0          = -1;
    int  last_sw1          = -1;
    bool last_switch_state = false; // Smooshボタンの立ち上がりエッジ検出用

    while(1)
    {
        hw.ProcessAnalogControls();

        base_freq       = ExpMap(hw.GetKnobValue(DaisyVersio::KNOB_0), 30.f, 300.f);
        pitch_depth_oct = hw.GetKnobValue(DaisyVersio::KNOB_1) * 4.f;
        pitch_env.SetTime(ADENV_SEG_DECAY,
                           ExpMap(hw.GetKnobValue(DaisyVersio::KNOB_2), 0.001f, 0.3f));
        amp_env.SetTime(ADENV_SEG_DECAY,
                         ExpMap(hw.GetKnobValue(DaisyVersio::KNOB_3), 0.005f, 1.f));
        feedback_amount = hw.GetKnobValue(DaisyVersio::KNOB_4) * 0.92f;
        delay_mix       = hw.GetKnobValue(DaisyVersio::KNOB_5);

        float glide_time_sec = ExpMap(hw.GetKnobValue(DaisyVersio::KNOB_6), 0.005f, 0.5f);
        delay_glide_coeff    = 1.f - expf(-1.f / (glide_time_sec * sample_rate));

        int sw0 = hw.sw[DaisyVersio::SW_0].Read();
        if(sw0 != last_sw0)
        {
            delay_time_target_samples = SubdivisionSeconds(sw0) * sample_rate;
            last_sw0                  = sw0;
        }

        int sw1 = hw.sw[DaisyVersio::SW_1].Read();
        if(sw1 != last_sw1)
        {
            waveform_mode = (sw1 == Switch3::POS_UP)     ? WAVE_MODE_SINE
                            : (sw1 == Switch3::POS_DOWN)  ? WAVE_MODE_NOISE
                                                           : WAVE_MODE_SQUARE;
            osc.SetWaveform(waveform_mode == WAVE_MODE_SQUARE
                                 ? Oscillator::WAVE_POLYBLEP_SQUARE
                                 : Oscillator::WAVE_SIN);
            last_sw1 = sw1;
        }

        // SwitchPressed()は押している間ずっとtrueを返すレベル検出のため、
        // 立ち上がりエッジ(押した瞬間)だけを自前で検出する。
        bool switch_state  = hw.SwitchPressed();
        bool switch_rising = switch_state && !last_switch_state;
        last_switch_state  = switch_state;

        if(hw.gate.Trig() || switch_rising)
        {
            pitch_env.Trigger();
            amp_env.Trigger();
        }

        float hit = amp_env.GetValue();
        hw.SetLed(0, hit, hit * 0.3f, 0.f);
        hw.SetLed(1, waveform_mode == WAVE_MODE_SINE ? 1.f : 0.f,
                  waveform_mode == WAVE_MODE_SQUARE ? 1.f : 0.f,
                  waveform_mode == WAVE_MODE_NOISE ? 1.f : 0.f);
        hw.SetLed(2, sw0 == Switch3::POS_UP ? 1.f : 0.f,
                  sw0 == Switch3::POS_CENTER ? 1.f : 0.f,
                  sw0 == Switch3::POS_DOWN ? 1.f : 0.f);
        // LED_3: 色=フィードバック量(緑→赤)、明滅速度=ディレイタイム(分周比のテンポ)
        float fb_norm = feedback_amount / 0.92f;
        hw.SetLed(3,
                  fb_norm * delay_led_brightness,
                  (1.f - fb_norm) * delay_led_brightness,
                  0.f);
        hw.UpdateLeds();

        System::Delay(1);
    }
}
