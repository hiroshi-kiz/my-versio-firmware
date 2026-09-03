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
//   X-SW  : Mod Type(ピッチモジュレーションの種類)を押すたびに循環切替
//           (KORG ER-1のOSCILLATORセクションと同じ操作感。ER-1本体でも
//            Mod Typeはノブではなくボタンで循環切替する仕様)
//   INL   : 外部クロック入力(オーディオ入力をゲートのように閾値検出して使う)
// =====================================================================
constexpr int KNOB_A = DaisyVersio::KNOB_0; // Blend      -> OSC基本ピッチ
constexpr int KNOB_B = DaisyVersio::KNOB_6; // Center     -> Mod Depth
constexpr int KNOB_C = DaisyVersio::KNOB_4; // Phase      -> DELAYフィードバック
constexpr int KNOB_D = DaisyVersio::KNOB_2; // Fold       -> Mod Speed(Envelope時はディケイ)
constexpr int KNOB_E = DaisyVersio::KNOB_3; // DOOM       -> AMPディケイ
constexpr int KNOB_F = DaisyVersio::KNOB_5; // Drive      -> DELAYミックス
// G(8vize)は未解決の問題により保留中: 実機で無反応。KNOB_0〜6の全パターン、
// および他ADC対応ピン全16本のスキャンでも反応するチャンネルが見つからず、
// 一方でNoise Engineering純正ファームウェア(2種)では正常動作を確認済み。
// ハードウェアは正常だが、コミュニティ版libDaisyの daisy_versio.h が
// 対応していない経路(マルチプレクサ等)で配線されている可能性が高い。

constexpr int SW_T1 = DaisyVersio::SW_0; // UND/X/OVR -> DELAY分周比
constexpr int SW_T2 = DaisyVersio::SW_1; // OFF/ON/TRK -> OSC波形

// KORG ER-1のOSCILLATOR Mod Typeを参考にした、ピッチモジュレーションの種類。
// ER-1本体と同じ6種類。X-SWを押すたびに順に切り替わる。
enum ModType
{
    MOD_SAW_DOWN = 0, // 周期的に下降するのこぎり波
    MOD_SQUARE,       // 2つの音高を交互に
    MOD_TRIANGLE,     // 周期的に上下する三角波
    MOD_SAMPLE_HOLD,  // 周期ごとにランダムな値へジャンプ
    MOD_NOISE,        // 連続的なノイズを加算(スネア向き、ER-1準拠)
    MOD_ENVELOPE,     // 単発のピッチディケイ(キック/タム向き、ER-1準拠)
    MOD_TYPE_LAST
};

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

// INLのクロック検出しきい値(ヒステリシス付き)。単一のしきい値だと、信号に
// 乗ったノイズ/リンギングで1回のパルスに対して何度も誤検出してしまうため、
// 「上がる時」と「下がる時」で別のしきい値を使うシュミットトリガー方式にする。
constexpr float kClockThresholdHigh = 0.35f;
constexpr float kClockThresholdLow  = 0.15f;

// ディレイバッファの最大長（サンプル数）。48kHz換算で1秒分確保しておけば
// BPM60の4分音符までカバーできる。SDRAM(64MB)に置くことで内蔵SRAMを圧迫しない。
constexpr size_t kMaxDelaySamples = 48000;

DaisyVersio hw;

Oscillator osc;
WhiteNoise noise;
// ハンドクラップ用ノイズ整形: レゾナンス無しのHP→LPを直列にかけ、
// 単一の共振バンドパスより耳当たりの柔らかい帯域制限ノイズを作る。
Svf        clap_hp;
Svf        clap_lp;
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
constexpr float kClapBurstOnSec       = 0.006f;
constexpr float kClapBurstOffsetsSec[3] = {0.f, 0.015f, 0.030f};
constexpr float kClapBurstEndSec      = 0.045f; // これ以降はテール(ゲート常時オープン)
constexpr float kClapGateSmoothSec    = 0.005f; // バーストの角を丸めてクリック感を減らす

// Clap時、A(本来はOSCピッチ)でクラップのトーン(帯域の中心)を可変にする
constexpr float kClapToneMinHz = 500.f;
constexpr float kClapToneMaxHz = 3000.f;

// ピンポンディレイの左右クロスフィード量。1.0で完全に左右交互(硬いピンポン)、
// 0.5で完全にモノラルへ収束。1.0未満にすると繰り返すたびに徐々にセンターへ
// 収束していく(＝エコーが減衰しながら左右の広がりが狭まる)。
constexpr float kDelayCrossFeed = 0.75f;

// DELAYタイム切替時のグライドタイム。以前はB(Center)が担当していたが、
// Mod Depthに転用したため固定値にした。
constexpr float kFixedGlideTimeSec = 0.08f;

float sample_rate;

// X-SW(タップテンポ)とINL(外部クロック)の両方から書き換えられる、
// 現在のテンポ(4分音符の周期、秒)。最後に更新した方が使われる。
volatile float tempo_period_sec = 60.f / TEMPO_BPM;

// 制御レート(メインループ)で更新され、オーディオコールバックから参照される値
volatile int   waveform_mode     = WAVE_MODE_SINE;
volatile int   mod_type          = MOD_ENVELOPE;
volatile float base_freq         = 60.f;
volatile float mod_depth_oct     = 0.f; // B: -3〜+3オクターブ(バイポーラ、中央=無効果)
volatile float mod_speed_hz      = 2.f; // D: LFOレート(Envelope時はディケイタイムとして解釈)
volatile float feedback_amount   = 0.5f;
volatile float delay_mix         = 0.5f;
volatile float delay_glide_coeff = 0.01f;
volatile float delay_time_target_samples = 0.f;
volatile float delay_led_brightness       = 0.f;

float delay_time_current_samples = 0.f;
float delay_led_phase            = 0.f; // 0〜1でディレイ1周期を表す（LED明滅用）

// ピッチモジュレーション用
WhiteNoise      mod_noise;             // Sample&Hold/Noiseタイプ用の乱数源
volatile float  mod_lfo_phase = 0.f;   // LFO位相(0〜1)。トリガーごとに0へリセットする
float           mod_sh_value  = 0.f;   // Sample&Holdで保持中の値(オーディオコールバック内でのみ使用)

// Clapのゲイン補正。HP->LPを直列に通すと音量が下がるため、聴感を揃えるために増幅する。
constexpr float kClapGain = 3.0f;

// INL外部クロック検出用(オーディオコールバック内でのみ使用)
bool     inl_gate_high          = false; // シュミットトリガーの現在の状態
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
        case Switch3::POS_DOWN: return period * 0.5f; // 8分音符
        default: return period * 0.75f; // 付点8分音符 (POS_CENTER)
    }
}

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                    AudioHandle::InterleavingOutputBuffer out,
                    size_t                                size)
{
    for(size_t i = 0; i < size; i += 2)
    {
        // INL: オーディオ入力(L)を外部クロックとして扱い、しきい値超えの
        // 立ち上がりエッジ間隔をテンポとして採用する。ノイズ/リンギングによる
        // 多重検出を防ぐため、ヒステリシス(シュミットトリガー)を使う。
        inl_samples_since_edge++;
        float inl_sample = in[i];
        if(!inl_gate_high && inl_sample > kClockThresholdHigh)
        {
            inl_gate_high = true;
            float interval_sec = inl_samples_since_edge / sample_rate;
            if(interval_sec > kMinTempoPeriodSec && interval_sec < kMaxTempoPeriodSec)
                tempo_period_sec = interval_sec;
            inl_samples_since_edge = 0;
        }
        else if(inl_gate_high && inl_sample < kClockThresholdLow)
        {
            inl_gate_high = false;
        }

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
            clap_hp.Process(noise.Process());
            clap_lp.Process(clap_hp.High());
            float gate_target = ClapBurstGate(clap_elapsed_samples);
            clap_gate_smoothed += (gate_target - clap_gate_smoothed) * clap_gate_smooth_coeff;
            osc_out = clap_lp.Low() * clap_gate_smoothed * kClapGain;
        }
        else
        {
            float mod_val; // -1〜+1のバイポーラ形状(Envelopeのみ0〜1)
            if(mod_type == MOD_ENVELOPE)
            {
                mod_val = pitch_val; // 0(idle/末端)〜1(トリガー直後)
            }
            else
            {
                mod_lfo_phase += mod_speed_hz / sample_rate;
                bool wrapped = false;
                if(mod_lfo_phase >= 1.f)
                {
                    mod_lfo_phase -= 1.f;
                    wrapped = true;
                }

                switch(mod_type)
                {
                    case MOD_SAW_DOWN:
                        mod_val = 1.f - 2.f * mod_lfo_phase;
                        break;
                    case MOD_SQUARE:
                        mod_val = (mod_lfo_phase < 0.5f) ? 1.f : -1.f;
                        break;
                    case MOD_TRIANGLE:
                        mod_val = 4.f * fabsf(mod_lfo_phase - 0.5f) - 1.f;
                        break;
                    case MOD_SAMPLE_HOLD:
                        if(wrapped)
                            mod_sh_value = mod_noise.Process();
                        mod_val = mod_sh_value;
                        break;
                    case MOD_NOISE:
                    default:
                        mod_val = mod_noise.Process();
                        break;
                }
            }

            float freq = base_freq * powf(2.f, mod_val * mod_depth_oct);
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

        // フィードバックを高くした際に信号が1.0を超えて割れるのを防ぐ、
        // ゆるいソフトクリップ(通常の音量では実質素通し)
        out[i]     = tanhf(dry + wet_l * delay_mix);
        out[i + 1] = tanhf(dry + wet_r * delay_mix);
    }
}

// KNOB_0〜6のADCサンプリング時間を、libDaisyのデフォルト(8.5サイクル、かなり
// 高速)より長めにしておく。G調査の過程で導入したが、クロストーク自体の
// 解決策ではなかった。動作を変える理由はないのでそのまま残す。
void InitKnobAdc()
{
    using namespace daisy::seed;
    Pin pins[DaisyVersio::KNOB_LAST] = {D21, D22, D28, D23, D16, D17, D19};

    AdcChannelConfig adc_cfg[DaisyVersio::KNOB_LAST];
    for(int i = 0; i < DaisyVersio::KNOB_LAST; i++)
        adc_cfg[i].InitSingle(pins[i], AdcChannelConfig::SPEED_64CYCLES_5);

    hw.seed.adc.Init(adc_cfg, DaisyVersio::KNOB_LAST);
    for(int i = 0; i < DaisyVersio::KNOB_LAST; i++)
        hw.knobs[i].Init(hw.seed.adc.GetPtr(i), hw.AudioCallbackRate(), true);
}

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(4);
    InitKnobAdc();
    sample_rate = hw.AudioSampleRate();

    osc.Init(sample_rate);
    osc.SetWaveform(Oscillator::WAVE_SIN);
    osc.SetAmp(1.f);
    noise.Init();
    mod_noise.Init();

    clap_hp.Init(sample_rate);
    clap_hp.SetRes(0.f);
    clap_hp.SetDrive(0.f);
    clap_lp.Init(sample_rate);
    clap_lp.SetRes(0.f);
    clap_lp.SetDrive(0.f);

    clap_gate_smooth_coeff = 1.f - expf(-1.f / (kClapGateSmoothSec * sample_rate));
    delay_glide_coeff      = 1.f - expf(-1.f / (kFixedGlideTimeSec * sample_rate));

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
    // 直線的な減衰だと余韻が感じられないため、指数的な減衰カーブにする
    // (立ち上がりは0.001秒と短いため、この設定による影響は無視できる)。
    // -40は急すぎて設定時間の序盤でほぼ無音になってしまったため、緩やかな値にした。
    amp_env.SetCurve(-8.f);

    delay_line_l.Init();
    delay_line_r.Init();
    delay_time_target_samples  = SubdivisionSeconds(Switch3::POS_CENTER) * sample_rate;
    delay_time_current_samples = delay_time_target_samples;

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    int  last_sw1          = -1;
    bool last_switch_state = false; // X-SWの立ち上がりエッジ検出用

    while(1)
    {
        hw.ProcessAnalogControls();
        // hw.UpdateExample()を使わない場合、tap(Smooshボタン)のデバウンス処理は
        // 誰も呼ばないため、明示的に呼び出す必要がある。
        hw.tap.Debounce();

        float knob_a    = hw.GetKnobValue(KNOB_A);
        base_freq       = ExpMap(knob_a, 30.f, 300.f);                   // A: OSC基本ピッチ
        // Clap時はAでバンドの中心(トーン/ピッチ感)を可変にする
        float clap_center = ExpMap(knob_a, kClapToneMinHz, kClapToneMaxHz);
        clap_hp.SetFreq(clap_center * 0.6f);
        clap_lp.SetFreq(clap_center * 1.8f);
        mod_depth_oct = (hw.GetKnobValue(KNOB_B) - 0.5f) * 3.f; // B: Mod Depth(-1.5〜+1.5oct, 中央=無効果)

        float knob_d = hw.GetKnobValue(KNOB_D);
        if(mod_type == MOD_ENVELOPE)
            pitch_env.SetTime(ADENV_SEG_DECAY, ExpMap(knob_d, 0.001f, 0.3f)); // D: ピッチディケイ
        else
            mod_speed_hz = ExpMap(knob_d, 0.1f, 50.f); // D: Mod Speed(LFOレート)

        amp_env.SetTime(ADENV_SEG_DECAY,
                         ExpMap(hw.GetKnobValue(KNOB_E), 0.005f, 1.f));    // E: AMPディケイ
        feedback_amount = hw.GetKnobValue(KNOB_C) * 0.985f;                // C: DELAYフィードバック(上限ギリギリ)
        delay_mix       = hw.GetKnobValue(KNOB_F);                        // F: DELAYミックス

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

        if(switch_rising) // X-SW: Mod Typeを次に進める
            mod_type = (mod_type + 1) % MOD_TYPE_LAST;

        if(hw.gate.Trig()) // X-IN: ドラムのトリガー
        {
            pitch_env.Trigger();
            amp_env.Trigger();
            clap_elapsed_samples = 0;
            // Saw Down/Square/Triangle/S&Hはフリーランだと叩くたびに位相がずれて
            // ランダムに聞こえてしまうため、トリガーごとに0へ揃えてリズムを安定させる。
            mod_lfo_phase = 0.f;
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
