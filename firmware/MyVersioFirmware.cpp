#include "daisy_versio.h"
#include "daisysp.h"

using namespace daisy;
using namespace daisysp;

DaisyVersio hw;

// ここにDSP用のオブジェクトを追加していく
static Oscillator osc;

void AudioCallback(AudioHandle::InterleavingInputBuffer  in,
                    AudioHandle::InterleavingOutputBuffer out,
                    size_t                                size)
{
    for(size_t i = 0; i < size; i += 2)
    {
        float sig = osc.Process();

        out[i]     = sig; // L
        out[i + 1] = sig; // R
    }
}

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(4);

    osc.Init(hw.AudioSampleRate());
    osc.SetWaveform(Oscillator::WAVE_SIN);
    osc.SetFreq(220.f);
    osc.SetAmp(0.5f);

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    while(1)
    {
        hw.ProcessAnalogControls();

        // KNOB_0 で周波数、KNOB_1 で音量を制御する例
        float freq = 50.f + hw.GetKnobValue(DaisyVersio::KNOB_0) * 2000.f;
        float amp  = hw.GetKnobValue(DaisyVersio::KNOB_1);
        osc.SetFreq(freq);
        osc.SetAmp(amp);

        hw.UpdateLeds();
        System::Delay(1);
    }
}
