#pragma once
#include <utility>
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

// Built-in 8-lane Rock/Metal Step Sequencer + Punchy Physical/Analog Drum Synth
// Outputs standard GM MIDI drum notes so it drives BOTH the internal kit AND any VST3/AU drum plugin!
class DrumEngine
{
public:
    static constexpr int numLanes = 8;
    static constexpr int maxSteps = 16;
    static constexpr int numPatterns = 4; // A, B, C, D

    struct LaneInfo
    {
        const char* name;
        int midiNote;
    };

    static constexpr std::array<LaneInfo, numLanes> lanes {{
        { "KICK",  36 },
        { "SNARE", 38 },
        { "C-HAT", 42 },
        { "O-HAT", 46 },
        { "HI TM", 48 },
        { "LO TM", 45 },
        { "CRASH", 49 },
        { "RIDE",  51 }
    }};

    // Step velocity levels: 0 = Off, 1 = Ghost (55), 2 = Normal (100), 3 = Accent (127)
    struct Pattern
    {
        std::array<std::array<uint8_t, maxSteps>, numLanes> grid {};
        int stepCount = 16; // Supports 1..16 (e.g., 16 = 4/4, 12 = 6/8 or 3/4, 14 = 7/8!)
    };

    DrumEngine()
    {
        loadPresetRock(0);
        loadPresetHeavyRiff(1);
        loadPresetHalfTime(2);
        loadPresetProg78(3);
    }

    void prepare(double sr)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        for (auto& v : voices)
            v.active = false;
    }

    // --- Thread-Safe Grid Access ---
    uint8_t getStep(int pat, int lane, int step) const
    {
        const juce::SpinLock::ScopedLockType sl(patternLock);
        return patterns[static_cast<size_t>(juce::jlimit(0, numPatterns - 1, pat))]
                       .grid[static_cast<size_t>(juce::jlimit(0, numLanes - 1, lane))]
                            [static_cast<size_t>(juce::jlimit(0, maxSteps - 1, step))];
    }

    void setStep(int pat, int lane, int step, uint8_t val)
    {
        const juce::SpinLock::ScopedLockType sl(patternLock);
        patterns[static_cast<size_t>(juce::jlimit(0, numPatterns - 1, pat))]
                .grid[static_cast<size_t>(juce::jlimit(0, numLanes - 1, lane))]
                     [static_cast<size_t>(juce::jlimit(0, maxSteps - 1, step))] = val;
    }

    int getPatternStepCount(int pat) const
    {
        const juce::SpinLock::ScopedLockType sl(patternLock);
        return patterns[static_cast<size_t>(juce::jlimit(0, numPatterns - 1, pat))].stepCount;
    }

    void setPatternStepCount(int pat, int count)
    {
        const juce::SpinLock::ScopedLockType sl(patternLock);
        patterns[static_cast<size_t>(juce::jlimit(0, numPatterns - 1, pat))].stepCount = juce::jlimit(4, maxSteps, count);
    }

    void clearPattern(int pat)
    {
        const juce::SpinLock::ScopedLockType sl(patternLock);
        auto& p = patterns[static_cast<size_t>(juce::jlimit(0, numPatterns - 1, pat))];
        for (auto& row : p.grid)
            row.fill(0);
    }

    void clearLane(int pat, int lane)
    {
        const juce::SpinLock::ScopedLockType sl(patternLock);
        patterns[static_cast<size_t>(juce::jlimit(0, numPatterns - 1, pat))]
                .grid[static_cast<size_t>(juce::jlimit(0, numLanes - 1, lane))].fill(0);
    }

    void fillLaneEvery(int pat, int lane, int stride, uint8_t vel = 2)
    {
        const juce::SpinLock::ScopedLockType sl(patternLock);
        auto& row = patterns[static_cast<size_t>(juce::jlimit(0, numPatterns - 1, pat))]
                            .grid[static_cast<size_t>(juce::jlimit(0, numLanes - 1, lane))];
        row.fill(0);
        for (int s = 0; s < maxSteps; s += std::max(1, stride))
            row[static_cast<size_t>(s)] = vel;
    }

    // --- Instant Guitar/Bass Jam Groove Presets ---
    void loadPresetRock(int pat)
    {
        clearPattern(pat);
        setPatternStepCount(pat, 16);
        setRow(pat, 0, { 3,0,0,0, 0,0,2,0, 3,0,2,0, 0,0,0,0 }); // Kick
        setRow(pat, 1, { 0,0,0,0, 3,0,0,1, 0,0,0,0, 3,0,0,0 }); // Snare
        setRow(pat, 2, { 2,0,2,0, 2,0,2,0, 2,0,2,0, 2,0,2,0 }); // C-Hat
        setRow(pat, 6, { 2,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0 }); // Crash
    }

    void loadPresetHeavyRiff(int pat)
    {
        clearPattern(pat);
        setPatternStepCount(pat, 16);
        setRow(pat, 0, { 3,0,2,0, 0,0,3,2, 0,2,3,0, 0,0,2,2 }); // Driving double-kick
        setRow(pat, 1, { 0,0,0,0, 3,0,0,0, 0,0,0,0, 3,0,0,1 }); // Backbeat snare
        setRow(pat, 3, { 2,0,0,0, 2,0,0,0, 2,0,0,0, 2,0,0,0 }); // Washy Open Hat
        setRow(pat, 7, { 2,0,2,0, 2,0,2,0, 2,0,2,0, 2,0,2,0 }); // Ride
    }

    void loadPresetHalfTime(int pat)
    {
        clearPattern(pat);
        setPatternStepCount(pat, 16);
        setRow(pat, 0, { 3,0,0,2, 0,0,2,0, 0,0,3,0, 0,0,2,0 }); // Breakdown kick
        setRow(pat, 1, { 0,0,0,0, 0,0,0,0, 3,0,0,0, 0,1,0,0 }); // Half-time snare on 3
        setRow(pat, 6, { 3,0,0,0, 2,0,0,0, 2,0,0,0, 2,0,0,0 }); // Quarter crash wash
        setRow(pat, 5, { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,2,3 }); // Floor tom turnaround
    }

    void loadPresetProg78(int pat)
    {
        clearPattern(pat);
        setPatternStepCount(pat, 14); // 7/8 (14 sixteenth notes!)
        setRow(pat, 0, { 3,0,0,2, 0,0,3,0, 0,0,2,0, 2,0, 0,0 });
        setRow(pat, 1, { 0,0,0,0, 3,0,0,0, 1,0,3,0, 0,1, 0,0 });
        setRow(pat, 2, { 2,0,2,0, 2,0,2,0, 2,0,2,0, 2,0, 0,0 });
        setRow(pat, 4, { 0,0,0,0, 0,0,0,0, 0,0,0,0, 2,2, 0,0 });
    }

    void loadPresetPunk(int pat)
    {
        clearPattern(pat);
        setPatternStepCount(pat, 16);
        setRow(pat, 0, { 3,0,0,2, 0,2,3,0, 3,0,0,2, 0,2,0,0 });
        setRow(pat, 1, { 0,0,3,0, 0,0,3,0, 0,0,3,0, 0,0,3,0 });
        setRow(pat, 3, { 2,0,2,0, 2,0,2,0, 2,0,2,0, 2,0,2,0 });
    }

    void loadPresetBlues68(int pat)
    {
        clearPattern(pat);
        setPatternStepCount(pat, 12); // 6/8 feel
        setRow(pat, 0, { 3,0,0,0, 0,2, 0,0,2,0, 0,0, 0,0,0,0 });
        setRow(pat, 1, { 0,0,0,0, 0,0, 3,0,0,0, 0,1, 0,0,0,0 });
        setRow(pat, 7, { 2,0,1,2, 0,1, 2,0,1,2, 0,1, 0,0,0,0 });
    }

    void loadPresetFunk(int pat)
    {
        clearPattern(pat);
        setPatternStepCount(pat, 16);
        setRow(pat, 0, { 3,0,2,0, 0,0,0,2, 0,0,3,0, 0,0,0,0 });
        setRow(pat, 1, { 0,0,0,0, 3,0,0,1, 0,1,0,0, 3,0,0,1 });
        setRow(pat, 2, { 2,1,2,1, 2,1,2,1, 2,1,2,1, 2,1,0,1 });
        setRow(pat, 3, { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,2,0 });
    }

    // --- Generate MIDI Events for Current Audio Block (Audio Thread) ---
    void generateMidiForBlock(juce::MidiBuffer& outMidi,
                              int numSamples,
                              int64_t playheadSample,
                              double bpm,
                              bool isPlaying) noexcept
    {
        if (!sequencerEnabled.load(std::memory_order_relaxed) || !isPlaying || bpm <= 0.0)
        {
            currentPlayingStep.store(-1, std::memory_order_relaxed);
            return;
        }

        const juce::SpinLock::ScopedTryLockType sl(patternLock);
        if (!sl.isLocked())
            return;

        const int patIdx = juce::jlimit(0, numPatterns - 1, activePattern.load(std::memory_order_relaxed));
        const auto& pat = patterns[static_cast<size_t>(patIdx)];
        const int stepCount = std::max(1, pat.stepCount);
        const double samplesPer16th = (sampleRate * 60.0) / (bpm * 4.0);
        const double swing = juce::jlimit(0.0, 0.35, static_cast<double>(swingAmount.load(std::memory_order_relaxed)));

        const int64_t blockStart = playheadSample;
        const int64_t blockEnd = playheadSample + numSamples;

        const int64_t firstGlobalStep = std::max<int64_t>(0, static_cast<int64_t>(std::floor(blockStart / samplesPer16th)) - 1);
        const int64_t lastGlobalStep = static_cast<int64_t>(std::ceil(blockEnd / samplesPer16th)) + 1;

        for (int64_t gStep = firstGlobalStep; gStep <= lastGlobalStep; ++gStep)
        {
            double stepTime = static_cast<double>(gStep) * samplesPer16th;
            if ((gStep & 1) != 0)
                stepTime += swing * samplesPer16th;

            const int64_t triggerSample = static_cast<int64_t>(std::llround(stepTime));
            if (triggerSample >= blockStart && triggerSample < blockEnd)
            {
                const int offset = static_cast<int>(triggerSample - blockStart);
                const int localStep = static_cast<int>(gStep % stepCount);
                currentPlayingStep.store(localStep, std::memory_order_relaxed);

                for (int lane = 0; lane < numLanes; ++lane)
                {
                    if (laneMuted[static_cast<size_t>(lane)].load(std::memory_order_relaxed))
                        continue;

                    const uint8_t vLevel = pat.grid[static_cast<size_t>(lane)][static_cast<size_t>(localStep)];
                    if (vLevel > 0)
                    {
                        const juce::uint8 velocity = (vLevel == 1) ? 58 : (vLevel == 2 ? 102 : 127);
                        outMidi.addEvent(juce::MidiMessage::noteOn(10, lanes[static_cast<size_t>(lane)].midiNote, velocity),
                                         juce::jlimit(0, numSamples - 1, offset));
                    }
                }
            }
        }
    }

    // Returns active pattern's kick hit mask (16 steps) & step count for Bassline Pocket lock
    void getActiveKickMask(std::array<uint8_t, maxSteps>& outKick, int& outStepCount) const
    {
        const juce::SpinLock::ScopedLockType sl(patternLock);
        const int patIdx = juce::jlimit(0, numPatterns - 1, activePattern.load());
        outKick = patterns[static_cast<size_t>(patIdx)].grid[0];
        outStepCount = patterns[static_cast<size_t>(patIdx)].stepCount;
    }

    // --- Export Active Pattern to a Timed MIDI Event Sequence (Message Thread) ---
    void exportPatternMidi(std::vector<std::pair<int64_t, juce::MidiMessage>>& outEvents,
                           int numBars,
                           double bpm,
                           double sr) const
    {
        const juce::SpinLock::ScopedLockType sl(patternLock);
        const int patIdx = juce::jlimit(0, numPatterns - 1, activePattern.load());
        const auto& pat = patterns[static_cast<size_t>(patIdx)];
        const int stepCount = std::max(1, pat.stepCount);
        const double samplesPer16th = (sr * 60.0) / (bpm * 4.0);
        const double swing = juce::jlimit(0.0, 0.35, static_cast<double>(swingAmount.load()));
        const int totalSteps = stepCount * std::max(1, numBars);

        outEvents.clear();
        for (int gStep = 0; gStep < totalSteps; ++gStep)
        {
            double t = static_cast<double>(gStep) * samplesPer16th;
            if ((gStep & 1) != 0)
                t += swing * samplesPer16th;
            const int64_t pos = static_cast<int64_t>(std::llround(t));
            const int localStep = gStep % stepCount;

            for (int lane = 0; lane < numLanes; ++lane)
            {
                const uint8_t vLevel = pat.grid[static_cast<size_t>(lane)][static_cast<size_t>(localStep)];
                if (vLevel > 0)
                {
                    const juce::uint8 vel = (vLevel == 1) ? 58 : (vLevel == 2 ? 102 : 127);
                    const int note = lanes[static_cast<size_t>(lane)].midiNote;
                    outEvents.emplace_back(pos, juce::MidiMessage::noteOn(10, note, vel));
                    outEvents.emplace_back(pos + static_cast<int64_t>(samplesPer16th * 0.5),
                                           juce::MidiMessage::noteOff(10, note));
                }
            }
        }
    }

    // --- Render Internal Punchy Rock Drum Synth for MIDI Buffer (Audio Thread) ---
    void renderSynth(juce::AudioBuffer<float>& outBuffer, const juce::MidiBuffer& midi, int numSamples) noexcept
    {
        auto it = midi.begin();
        const auto end = midi.end();

        float* left = outBuffer.getWritePointer(0);
        float* right = outBuffer.getNumChannels() > 1 ? outBuffer.getWritePointer(1) : left;

        for (int i = 0; i < numSamples; ++i)
        {
            while (it != end && (*it).samplePosition <= i)
            {
                const auto msg = (*it).getMessage();
                if (msg.isNoteOn())
                    triggerVoice(msg.getNoteNumber(), msg.getFloatVelocity());
                ++it;
            }

            float sumL = 0.0f, sumR = 0.0f;
            for (int v = 0; v < numLanes; ++v)
            {
                if (!voices[static_cast<size_t>(v)].active)
                    continue;

                const float s = renderVoiceSample(v);
                sumL += s * voicePanL[static_cast<size_t>(v)];
                sumR += s * voicePanR[static_cast<size_t>(v)];
            }

            left[i] += sumL;
            if (right != left)
                right[i] += sumR;
        }
    }

    // --- XML Serialization ---
    std::unique_ptr<juce::XmlElement> toXml() const
    {
        const juce::SpinLock::ScopedLockType sl(patternLock);
        auto root = std::make_unique<juce::XmlElement>("DrumEngine");
        root->setAttribute("enabled", sequencerEnabled.load());
        root->setAttribute("activePattern", activePattern.load());
        root->setAttribute("swing", static_cast<double>(swingAmount.load()));

        for (int p = 0; p < numPatterns; ++p)
        {
            auto* pEl = root->createNewChildElement("Pattern");
            pEl->setAttribute("index", p);
            pEl->setAttribute("steps", patterns[static_cast<size_t>(p)].stepCount);
            for (int l = 0; l < numLanes; ++l)
            {
                juce::String row;
                for (int s = 0; s < maxSteps; ++s)
                    row << static_cast<int>(patterns[static_cast<size_t>(p)].grid[static_cast<size_t>(l)][static_cast<size_t>(s)]);
                pEl->setAttribute("L" + juce::String(l), row);
            }
        }
        return root;
    }

    void fromXml(const juce::XmlElement& xml)
    {
        const juce::SpinLock::ScopedLockType sl(patternLock);
        sequencerEnabled.store(xml.getBoolAttribute("enabled", true));
        activePattern.store(juce::jlimit(0, numPatterns - 1, xml.getIntAttribute("activePattern", 0)));
        swingAmount.store(static_cast<float>(xml.getDoubleAttribute("swing", 0.0)));

        for (auto* pEl : xml.getChildWithTagNameIterator("Pattern"))
        {
            const int p = juce::jlimit(0, numPatterns - 1, pEl->getIntAttribute("index", 0));
            patterns[static_cast<size_t>(p)].stepCount = juce::jlimit(4, maxSteps, pEl->getIntAttribute("steps", 16));
            for (int l = 0; l < numLanes; ++l)
            {
                const juce::String row = pEl->getStringAttribute("L" + juce::String(l));
                for (int s = 0; s < std::min(maxSteps, row.length()); ++s)
                    patterns[static_cast<size_t>(p)].grid[static_cast<size_t>(l)][static_cast<size_t>(s)] =
                        static_cast<uint8_t>(juce::jlimit(0, 3, static_cast<int>(row[s]) - static_cast<int>('0')));
            }
        }
    }

    std::atomic<bool> sequencerEnabled { true };
    std::atomic<int> activePattern { 0 };
    std::atomic<float> swingAmount { 0.0f };
    std::atomic<int> currentPlayingStep { -1 };
    std::array<std::atomic<bool>, numLanes> laneMuted {};

private:
    struct Voice
    {
        bool active = false;
        double age = 0.0;
        double phase = 0.0;
        double phase2 = 0.0;
        float velocity = 1.0f;
        float lpState = 0.0f;
        float hpState = 0.0f;
        float prevNoise = 0.0f;
    };

    void setRow(int pat, int lane, std::initializer_list<uint8_t> vals)
    {
        const juce::SpinLock::ScopedLockType sl(patternLock);
        auto& row = patterns[static_cast<size_t>(pat)].grid[static_cast<size_t>(lane)];
        int i = 0;
        for (uint8_t v : vals)
            if (i < maxSteps)
                row[static_cast<size_t>(i++)] = v;
    }

    float nextNoise() noexcept
    {
        rngState = rngState * 1664525u + 1013904223u;
        return (static_cast<float>(rngState >> 9) / 4194304.0f) - 1.0f;
    }

    void triggerVoice(int midiNote, float vel) noexcept
    {
        int lane = -1;
        for (int i = 0; i < numLanes; ++i)
            if (lanes[static_cast<size_t>(i)].midiNote == midiNote)
                lane = i;

        if (lane < 0)
            return;

        // Closed hat chokes Open hat
        if (lane == 2)
            voices[3].active = false;

        auto& v = voices[static_cast<size_t>(lane)];
        v.active = true;
        v.age = 0.0;
        v.phase = 0.0;
        v.phase2 = 0.0;
        v.velocity = juce::jlimit(0.15f, 1.0f, vel);
        v.lpState = 0.0f;
        v.hpState = 0.0f;
    }

    float renderVoiceSample(int lane) noexcept
    {
        auto& v = voices[static_cast<size_t>(lane)];
        const double t = v.age;
        v.age += 1.0 / sampleRate;

        const float n = nextNoise();
        float out = 0.0f;

        switch (lane)
        {
            case 0: // KICK: deep punchy sub + beater click
            {
                if (t > 0.42) { v.active = false; return 0.0f; }
                const double freq = 46.0 + 135.0 * std::exp(-t * 36.0);
                v.phase += juce::MathConstants<double>::twoPi * freq / sampleRate;
                const float body = static_cast<float>(std::sin(v.phase)) * static_cast<float>(std::exp(-t * 9.5));
                const float click = n * static_cast<float>(std::exp(-t * 280.0)) * 0.45f;
                out = std::tanh((body * 1.15f + click) * 1.35f) * 0.85f;
                break;
            }
            case 1: // SNARE: resonant shell + crisp snare wires
            {
                if (t > 0.34) { v.active = false; return 0.0f; }
                const double freq = 145.0 + 85.0 * std::exp(-t * 45.0);
                v.phase += juce::MathConstants<double>::twoPi * freq / sampleRate;
                const float body = static_cast<float>(std::sin(v.phase)) * static_cast<float>(std::exp(-t * 18.0)) * 0.55f;
                // Band-shaped snare wire noise
                v.hpState = 0.72f * (v.hpState + n - v.prevNoise);
                v.prevNoise = n;
                const float wires = v.hpState * static_cast<float>(std::exp(-t * 14.0)) * 0.72f;
                out = std::tanh((body + wires) * 1.3f) * 0.75f;
                break;
            }
            case 2: // CLOSED HAT
            {
                if (t > 0.09) { v.active = false; return 0.0f; }
                v.hpState = 0.86f * (v.hpState + n - v.prevNoise);
                v.prevNoise = n;
                v.phase += juce::MathConstants<double>::twoPi * 6350.0 / sampleRate;
                const float metal = (std::sin(v.phase) > 0.0 ? 0.25f : -0.25f);
                out = (v.hpState * 0.75f + metal * 0.25f) * static_cast<float>(std::exp(-t * 48.0)) * 0.42f;
                break;
            }
            case 3: // OPEN HAT
            {
                if (t > 0.45) { v.active = false; return 0.0f; }
                v.hpState = 0.85f * (v.hpState + n - v.prevNoise);
                v.prevNoise = n;
                v.phase += juce::MathConstants<double>::twoPi * 5900.0 / sampleRate;
                const float metal = (std::sin(v.phase) > 0.0 ? 0.22f : -0.22f);
                out = (v.hpState * 0.8f + metal * 0.2f) * static_cast<float>(std::exp(-t * 8.5)) * 0.44f;
                break;
            }
            case 4: // HIGH TOM
            {
                if (t > 0.40) { v.active = false; return 0.0f; }
                const double freq = 128.0 + 72.0 * std::exp(-t * 24.0);
                v.phase += juce::MathConstants<double>::twoPi * freq / sampleRate;
                out = static_cast<float>(std::sin(v.phase)) * static_cast<float>(std::exp(-t * 9.0)) * 0.65f;
                break;
            }
            case 5: // LOW / FLOOR TOM
            {
                if (t > 0.50) { v.active = false; return 0.0f; }
                const double freq = 76.0 + 54.0 * std::exp(-t * 20.0);
                v.phase += juce::MathConstants<double>::twoPi * freq / sampleRate;
                out = static_cast<float>(std::sin(v.phase)) * static_cast<float>(std::exp(-t * 7.2)) * 0.72f;
                break;
            }
            case 6: // CRASH
            {
                if (t > 1.25) { v.active = false; return 0.0f; }
                v.hpState = 0.82f * (v.hpState + n - v.prevNoise);
                v.prevNoise = n;
                v.phase += juce::MathConstants<double>::twoPi * 3840.0 / sampleRate;
                v.phase2 += juce::MathConstants<double>::twoPi * 5410.0 / sampleRate;
                const float shimmer = static_cast<float>(std::sin(v.phase) * std::sin(v.phase2)) * 0.3f;
                out = (v.hpState * 0.8f + shimmer) * static_cast<float>(std::exp(-t * 3.1)) * 0.46f;
                break;
            }
            case 7: // RIDE
            {
                if (t > 0.75) { v.active = false; return 0.0f; }
                v.hpState = 0.88f * (v.hpState + n - v.prevNoise);
                v.prevNoise = n;
                v.phase += juce::MathConstants<double>::twoPi * 2950.0 / sampleRate;
                v.phase2 += juce::MathConstants<double>::twoPi * 4420.0 / sampleRate;
                const float bell = static_cast<float>(std::sin(v.phase) + 0.5 * std::sin(v.phase2))
                                   * static_cast<float>(std::exp(-t * 7.5)) * 0.25f;
                out = (v.hpState * 0.55f * static_cast<float>(std::exp(-t * 5.2)) + bell) * 0.42f;
                break;
            }
        }

        return out * v.velocity;
    }

    double sampleRate = 44100.0;
    mutable juce::SpinLock patternLock;
    std::array<Pattern, numPatterns> patterns {};
    std::array<Voice, numLanes> voices {};
    uint32_t rngState = 0x12345678u;

    // Natural drummer stereo spread: Kick/Snare center, Hats slightly L, Toms L->R, Crash L, Ride R
    static constexpr std::array<float, numLanes> voicePanL { 0.85f, 0.85f, 0.92f, 0.90f, 0.88f, 0.52f, 0.92f, 0.50f };
    static constexpr std::array<float, numLanes> voicePanR { 0.85f, 0.85f, 0.58f, 0.62f, 0.60f, 0.92f, 0.55f, 0.94f };
};
