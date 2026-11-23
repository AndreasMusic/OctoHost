#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

// --- 1. 2nd-Order Butterworth High-Pass Filter ---
class StereoHighPass
{
public:
    enum class Mode { Off = 0, Hz35 = 1, Hz80 = 2, Hz120 = 3 };

    void prepare(double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        reset();
        updateCoeffs(lastMode);
    }

    void reset()
    {
        for (int ch = 0; ch < 2; ++ch)
            z1[ch] = z2[ch] = 0.0;
    }

    void process(juce::AudioBuffer<float>& buf, int numSamples, Mode mode) noexcept
    {
        if (mode == Mode::Off)
        {
            if (lastMode != Mode::Off)
            {
                reset();
                lastMode = Mode::Off;
            }
            return;
        }

        if (mode != lastMode)
            updateCoeffs(mode);

        const int chCount = std::min(2, buf.getNumChannels());
        for (int ch = 0; ch < chCount; ++ch)
        {
            float* data = buf.getWritePointer(ch);
            double s1 = z1[ch], s2 = z2[ch];
            for (int i = 0; i < numSamples; ++i)
            {
                const double x = static_cast<double>(data[i]);
                const double y = b0 * x + s1;
                s1 = b1 * x - a1 * y + s2;
                s2 = b2 * x - a2 * y;
                data[i] = static_cast<float>(y);
            }
            z1[ch] = s1;
            z2[ch] = s2;
        }
    }

private:
    void updateCoeffs(Mode m) noexcept
    {
        lastMode = m;
        double fc = 80.0;
        if (m == Mode::Hz35)       fc = 35.0;
        else if (m == Mode::Hz80)  fc = 80.0;
        else if (m == Mode::Hz120) fc = 120.0;

        const double k = std::tan(juce::MathConstants<double>::pi * fc / sampleRate);
        const double norm = 1.0 / (1.0 + std::sqrt(2.0) * k + k * k);
        b0 = 1.0 * norm;
        b1 = -2.0 * b0;
        b2 = b0;
        a1 = 2.0 * (k * k - 1.0) * norm;
        a2 = (1.0 - std::sqrt(2.0) * k + k * k) * norm;
    }

    double sampleRate = 44100.0;
    Mode lastMode = Mode::Off;
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1[2] = { 0.0, 0.0 };
    double z2[2] = { 0.0, 0.0 };
};

// --- 2. Fast Guitar / Bass Input Noise Gate ---
class InputNoiseGate
{
public:
    enum class Mode { Off = 0, Tight = 1, Metal = 2 };

    void prepare(double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        env = 0.0f;
        gain = 1.0f;
        holdCounter = 0;
    }

    void process(juce::AudioBuffer<float>& buf, int numSamples, Mode mode) noexcept
    {
        if (mode == Mode::Off)
        {
            gain = 1.0f;
            return;
        }

        const float threshOpen  = (mode == Mode::Metal) ? 0.010f : 0.0025f;
        const float threshClose = threshOpen * 0.65f;
        const float attCoeff = std::exp(-1.0f / static_cast<float>(0.0015 * sampleRate));
        const float relCoeff = std::exp(-1.0f / static_cast<float>((mode == Mode::Metal ? 0.045 : 0.12) * sampleRate));
        const int holdSamples = static_cast<int>((mode == Mode::Metal ? 0.020 : 0.060) * sampleRate);

        float* ch0 = buf.getWritePointer(0);
        float* ch1 = buf.getNumChannels() > 1 ? buf.getWritePointer(1) : nullptr;

        for (int i = 0; i < numSamples; ++i)
        {
            float lvl = std::abs(ch0[i]);
            if (ch1 != nullptr)
                lvl = std::max(lvl, std::abs(ch1[i]));

            env = std::max(lvl, env * 0.995f);

            float target = 0.0f;
            if (env >= threshOpen)
            {
                holdCounter = holdSamples;
                target = 1.0f;
            }
            else if (holdCounter > 0)
            {
                --holdCounter;
                target = 1.0f;
            }
            else if (env > threshClose && gain > 0.5f)
            {
                target = 1.0f;
            }

            const float coeff = (target > gain) ? attCoeff : relCoeff;
            gain = target + coeff * (gain - target);

            ch0[i] *= gain;
            if (ch1 != nullptr)
                ch1[i] *= gain;
        }
    }

private:
    double sampleRate = 44100.0;
    float env = 0.0f;
    float gain = 1.0f;
    int holdCounter = 0;
};

// --- 3. Subtle Analog Console Saturation / Drive (Off / Warm Tape / Grit) ---
class AnalogDriveStage
{
public:
    enum class Mode { Off = 0, Warm = 1, Grit = 2 };

    static void process(juce::AudioBuffer<float>& buf, int numSamples, Mode mode) noexcept
    {
        if (mode == Mode::Off)
            return;

        const float drive = (mode == Mode::Grit) ? 2.8f : 1.45f;
        const float makeup = (mode == Mode::Grit) ? 0.62f : 0.84f;
        const int chCount = std::min(2, buf.getNumChannels());

        for (int ch = 0; ch < chCount; ++ch)
        {
            float* data = buf.getWritePointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                // Slight asymmetric harmonic warmth
                const float x = data[i] * drive;
                const float y = std::tanh(x + 0.04f * x * x);
                data[i] = y * makeup;
            }
        }
    }
};

// --- 4. Built-In Polyphonic Analog Synth (instant sound for Piano Roll / Melody Forge when no VSTi loaded) ---
class PolyFallbackSynth
{
public:
    enum class Patch { AnalogSub = 0, SawLead = 1, WarmPad = 2, PluckBass = 3 };

    void prepare(double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        for (auto& v : voices)
            v.active = false;
    }

    void render(juce::AudioBuffer<float>& outBuffer,
                const juce::MidiBuffer& midi,
                int numSamples,
                Patch patch) noexcept
    {
        auto it = midi.begin();
        const auto end = midi.end();

        float* left = outBuffer.getWritePointer(0);
        float* right = outBuffer.getNumChannels() > 1 ? outBuffer.getWritePointer(1) : left;

        const float attackRate  = static_cast<float>(1.0 / ((patch == Patch::WarmPad ? 0.12 : 0.006) * sampleRate));
        const float releaseRate = static_cast<float>(1.0 / ((patch == Patch::WarmPad ? 0.38 : (patch == Patch::PluckBass ? 0.09 : 0.18)) * sampleRate));
        const float decayRate   = static_cast<float>(1.0 / ((patch == Patch::PluckBass ? 0.22 : 0.45) * sampleRate));
        const float sustainLvl  = (patch == Patch::PluckBass) ? 0.28f : 0.72f;

        for (int i = 0; i < numSamples; ++i)
        {
            while (it != end && (*it).samplePosition <= i)
            {
                const auto msg = (*it).getMessage();
                if (msg.isNoteOn())
                    startVoice(msg.getNoteNumber(), msg.getFloatVelocity());
                else if (msg.isNoteOff())
                    releaseVoice(msg.getNoteNumber());
                else if (msg.isAllNotesOff() || msg.isAllSoundOff())
                    allNotesOff();
                ++it;
            }

            float sumL = 0.0f, sumR = 0.0f;
            for (auto& v : voices)
            {
                if (!v.active)
                    continue;

                // ADSR Envelope
                if (v.stage == 0) // Attack
                {
                    v.env += attackRate;
                    if (v.env >= 1.0f) { v.env = 1.0f; v.stage = 1; }
                }
                else if (v.stage == 1) // Decay
                {
                    v.env -= decayRate;
                    if (v.env <= sustainLvl) { v.env = sustainLvl; v.stage = 2; }
                }
                else if (v.stage == 3) // Release
                {
                    v.env -= releaseRate;
                    if (v.env <= 0.0005f) { v.active = false; continue; }
                }

                // Dual detuned oscillator
                const double dt1 = v.freq / sampleRate;
                const double dt2 = (v.freq * (patch == Patch::WarmPad ? 1.006 : 1.003)) / sampleRate;
                v.phase1 += dt1; if (v.phase1 >= 1.0) v.phase1 -= 1.0;
                v.phase2 += dt2; if (v.phase2 >= 1.0) v.phase2 -= 1.0;

                const float saw1 = static_cast<float>(2.0 * v.phase1 - 1.0);
                const float saw2 = static_cast<float>(2.0 * v.phase2 - 1.0);
                const float sub  = static_cast<float>(std::sin(juce::MathConstants<double>::twoPi * v.phase1));

                float oscL = 0.0f, oscR = 0.0f;
                if (patch == Patch::AnalogSub || patch == Patch::PluckBass)
                {
                    const float mix = sub * 0.62f + saw1 * 0.38f;
                    oscL = oscR = mix;
                }
                else
                {
                    oscL = saw1 * 0.55f + saw2 * 0.35f + sub * 0.25f;
                    oscR = saw2 * 0.55f + saw1 * 0.35f + sub * 0.25f;
                }

                // Simple resonant-warmth 1-pole low-pass modulated by envelope
                const float cutoff = (patch == Patch::WarmPad) ? (0.08f + 0.18f * v.env)
                                                               : (0.12f + 0.42f * v.env);
                v.lpL += cutoff * (oscL - v.lpL);
                v.lpR += cutoff * (oscR - v.lpR);

                const float amp = v.env * v.velocity * 0.26f;
                sumL += v.lpL * amp;
                sumR += v.lpR * amp;
            }

            left[i] += sumL;
            if (right != left)
                right[i] += sumR;
        }
    }

private:
    struct Voice
    {
        bool active = false;
        int note = 60;
        double freq = 261.63;
        double phase1 = 0.0;
        double phase2 = 0.0;
        float velocity = 0.8f;
        float env = 0.0f;
        int stage = 0; // 0=A, 1=D, 2=S, 3=R
        float lpL = 0.0f;
        float lpR = 0.0f;
    };

    void startVoice(int note, float vel) noexcept
    {
        Voice* target = nullptr;
        // Reuse same note or free slot
        for (auto& v : voices)
            if (v.active && v.note == note) { target = &v; break; }
        if (target == nullptr)
        {
            for (auto& v : voices)
                if (!v.active) { target = &v; break; }
        }
        if (target == nullptr)
            target = &voices[nextSteal++ % voices.size()];

        target->active = true;
        target->note = note;
        target->freq = 440.0 * std::pow(2.0, (note - 69) / 12.0);
        target->velocity = juce::jlimit(0.1f, 1.0f, vel);
        target->stage = 0;
        target->env = std::min(target->env, 0.1f);
    }

    void releaseVoice(int note) noexcept
    {
        for (auto& v : voices)
            if (v.active && v.note == note)
                v.stage = 3;
    }

    void allNotesOff() noexcept
    {
        for (auto& v : voices)
            if (v.active)
                v.stage = 3;
    }

    double sampleRate = 44100.0;
    std::array<Voice, 12> voices {};
    size_t nextSteal = 0;
};

// --- 5. Lock-Free Chromatic Bass/Guitar YIN Tuner (down to 27 Hz / Low A0) ---
class ChromaticTuner
{
public:
    struct Reading
    {
        bool valid = false;
        float frequencyHz = 0.0f;
        float cents = 0.0f;
        juce::String noteName { "--" };
    };

    void prepare(double sr)
    {
        sampleRate.store(sr > 0.0 ? sr : 44100.0);
        fifo.reset();
        analysisBuffer.fill(0.0f);
        writeIdx = 0;
    }

    void pushSamples(const float* monoData, int numSamples) noexcept
    {
        if (!active.load(std::memory_order_relaxed) || monoData == nullptr || numSamples <= 0)
            return;

        int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
        fifo.prepareToWrite(numSamples, s1, n1, s2, n2);
        if (n1 > 0) std::copy_n(monoData, n1, ring.begin() + s1);
        if (n2 > 0) std::copy_n(monoData + n1, n2, ring.begin() + s2);
        fifo.finishedWrite(n1 + n2);
    }

    Reading analyse()
    {
        const int ready = fifo.getNumReady();
        if (ready > 0)
        {
            int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
            fifo.prepareToRead(ready, s1, n1, s2, n2);
            for (int i = 0; i < n1; ++i)
            {
                analysisBuffer[writeIdx] = ring[static_cast<size_t>(s1 + i)];
                writeIdx = (writeIdx + 1) % windowSize;
            }
            for (int i = 0; i < n2; ++i)
            {
                analysisBuffer[writeIdx] = ring[static_cast<size_t>(s2 + i)];
                writeIdx = (writeIdx + 1) % windowSize;
            }
            fifo.finishedRead(n1 + n2);
        }

        Reading out;
        if (!active.load())
            return out;

        std::array<float, windowSize> buf {};
        for (int i = 0; i < windowSize; ++i)
            buf[static_cast<size_t>(i)] = analysisBuffer[(writeIdx + static_cast<size_t>(i)) % windowSize];

        double energy = 0.0;
        for (float v : buf)
            energy += static_cast<double>(v) * v;
        if (std::sqrt(energy / windowSize) < 0.004)
            return out;

        const double sr = sampleRate.load() * 0.5;
        constexpr int N = windowSize / 2;
        std::array<float, N> dec {};
        for (int i = 0; i < N; ++i)
            dec[static_cast<size_t>(i)] = 0.5f * (buf[static_cast<size_t>(2 * i)] + buf[static_cast<size_t>(2 * i + 1)]);

        const int minLag = std::max(8, static_cast<int>(sr / 1100.0));
        const int maxLag = std::min(N / 2 - 2, static_cast<int>(sr / 27.0));
        if (maxLag <= minLag)
            return out;

        std::vector<float> cmnd(static_cast<size_t>(maxLag + 1), 1.0f);
        const int halfN = N / 2;
        float runningSum = 0.0f;
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            float sum = 0.0f;
            for (int j = 0; j < halfN; ++j)
            {
                const float d = dec[static_cast<size_t>(j)] - dec[static_cast<size_t>(j + tau)];
                sum += d * d;
            }
            runningSum += sum;
            cmnd[static_cast<size_t>(tau)] = (runningSum > 1e-9f) ? (sum * tau / runningSum) : 1.0f;
        }

        int bestTau = -1;
        for (int tau = minLag; tau < maxLag; ++tau)
        {
            if (cmnd[static_cast<size_t>(tau)] < 0.14f)
            {
                while (tau + 1 < maxLag && cmnd[static_cast<size_t>(tau + 1)] < cmnd[static_cast<size_t>(tau)])
                    ++tau;
                bestTau = tau;
                break;
            }
        }
        if (bestTau < 0)
            return out;

        float refinedTau = static_cast<float>(bestTau);
        if (bestTau > 1 && bestTau < maxLag)
        {
            const float s0 = cmnd[static_cast<size_t>(bestTau - 1)];
            const float s1 = cmnd[static_cast<size_t>(bestTau)];
            const float s2 = cmnd[static_cast<size_t>(bestTau + 1)];
            const float denom = 2.0f * (2.0f * s1 - s2 - s0);
            if (std::abs(denom) > 1e-6f)
                refinedTau += (s2 - s0) / denom;
        }

        const float freq = static_cast<float>(sr / refinedTau);
        if (freq < 27.0f || freq > 1200.0f)
            return out;

        const float midiExact = 69.0f + 12.0f * std::log2(freq / 440.0f);
        const int midiNote = static_cast<int>(std::round(midiExact));
        const float cents = (midiExact - static_cast<float>(midiNote)) * 100.0f;

        static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        out.valid = true;
        out.frequencyHz = freq;
        out.cents = cents;
        out.noteName = juce::String(names[((midiNote % 12) + 12) % 12]) + juce::String((midiNote / 12) - 1);
        return out;
    }

    std::atomic<bool> active { false };
    std::atomic<bool> muteWhileTuning { false };

private:
    static constexpr int fifoSize = 8192;
    static constexpr int windowSize = 4096;

    std::atomic<double> sampleRate { 44100.0 };
    juce::AbstractFifo fifo { fifoSize };
    std::array<float, fifoSize> ring {};
    std::array<float, windowSize> analysisBuffer {};
    size_t writeIdx = 0;
};

// --- 6. Sample-Accurate Metronome Click ---
class MetronomeClick
{
public:
    void prepare(double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        samplesRemaining = 0;
        phase = 0.0;
    }

    void trigger(bool accent) noexcept
    {
        freq = accent ? 1600.0 : 1050.0;
        totalSamples = static_cast<int>(0.025 * sampleRate);
        samplesRemaining = totalSamples;
        phase = 0.0;
        amp = accent ? 0.55f : 0.35f;
    }

    float nextSample(float vol) noexcept
    {
        if (samplesRemaining <= 0)
            return 0.0f;

        const float env = static_cast<float>(samplesRemaining) / static_cast<float>(std::max(1, totalSamples));
        const float s = static_cast<float>(std::sin(phase)) * env * env * amp * vol;
        phase += juce::MathConstants<double>::twoPi * freq / sampleRate;
        --samplesRemaining;
        return s;
    }

private:
    double sampleRate = 44100.0;
    double freq = 1050.0;
    double phase = 0.0;
    int totalSamples = 1;
    int samplesRemaining = 0;
    float amp = 0.4f;
};

inline float softLimitSample(float x) noexcept
{
    const float ax = std::abs(x);
    if (ax <= 0.85f)
        return x;
    const float excess = ax - 0.85f;
    const float shaped = 0.85f + 0.145f * std::tanh(excess / 0.145f);
    return x >= 0.0f ? shaped : -shaped;
}
