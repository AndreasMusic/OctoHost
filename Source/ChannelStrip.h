#pragma once
#include <cstdint>
#include <utility>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <memory>
#include <vector>
#include "DSPUtils.h"
#include "DrumEngine.h"
#include "InsertChain.h"
#include "MidiTools.h"

enum class TrackMode { Audio = 0, Midi = 1 };

enum class InputSourceType : int
{
    None = 0,
    MonoHW = 1,
    StereoHW = 2,
    MidiAll = 3,
    MidiChannel = 4,
    DrumSeq = 5
};

enum class OutputDestType : int
{
    Master = 0,
    FxBusA = 1,
    FxBusB = 2,
    DirectStereoHW = 3,
    DirectMonoHW = 4
};

struct TimedMidiEvent
{
    int64_t samplePos = 0;
    juce::MidiMessage message;
};

// --- Dedicated Stereo FX Send Bus (FX A / FX B) with 4 VST3/AU Inserts ---
class FXBus
{
public:
    FXBus(juce::String busName, int idx)
        : name(std::move(busName)), busIndex(idx) {}

    void prepare(double sampleRate, int maxBlockSize)
    {
        const int allocBlock = std::max(maxBlockSize, 4096);
        busBuffer.setSize(2, allocBlock, false, true, true);
        inserts.prepare(sampleRate, maxBlockSize);
    }

    void clearInput(int numSamples) noexcept
    {
        const int n = std::min(numSamples, busBuffer.getNumSamples());
        if (n > 0)
            busBuffer.clear(0, n);
    }

    void addInput(const juce::AudioBuffer<float>& src, int numSamples, float sendGain) noexcept
    {
        if (sendGain <= 0.0001f)
            return;
        const int n = std::min({ numSamples, busBuffer.getNumSamples(), src.getNumSamples() });
        if (n <= 0)
            return;
        busBuffer.addFrom(0, 0, src, 0, 0, n, sendGain);
        busBuffer.addFrom(1, 0, src, src.getNumChannels() > 1 ? 1 : 0, 0, n, sendGain);
    }

    void process(juce::AudioBuffer<float>& masterOut,
                 juce::AudioBuffer<float>& directHwOut,
                 int numSamples,
                 bool anyBusSoloed) noexcept
    {
        const int n = std::min(numSamples, busBuffer.getNumSamples());
        if (n <= 0)
            return;

        dummyMidi.clear();
        inserts.process(busBuffer, dummyMidi, n);

        const bool isMuted = muted.load(std::memory_order_relaxed);
        const bool isSoloed = soloed.load(std::memory_order_relaxed);
        const bool audible = !isMuted && (!anyBusSoloed || isSoloed);

        if (!audible)
        {
            peakL.store(0.0f, std::memory_order_relaxed);
            peakR.store(0.0f, std::memory_order_relaxed);
            return;
        }

        const float g = gain.load(std::memory_order_relaxed);
        const float p = (pan.load(std::memory_order_relaxed) + 1.0f) * 0.5f;
        const float lg = g * std::cos(p * juce::MathConstants<float>::halfPi);
        const float rg = g * std::sin(p * juce::MathConstants<float>::halfPi);

        peakL.store(busBuffer.getMagnitude(0, 0, n) * lg, std::memory_order_relaxed);
        peakR.store(busBuffer.getMagnitude(1, 0, n) * rg, std::memory_order_relaxed);

        const auto dest = static_cast<OutputDestType>(outputDestType.load(std::memory_order_relaxed));
        const int hwCh = outputHwChannel.load(std::memory_order_relaxed);

        if (dest == OutputDestType::DirectStereoHW && hwCh + 1 < directHwOut.getNumChannels())
        {
            directHwOut.addFrom(hwCh, 0, busBuffer, 0, 0, n, lg);
            directHwOut.addFrom(hwCh + 1, 0, busBuffer, 1, 0, n, rg);
        }
        else if (dest == OutputDestType::DirectMonoHW && hwCh < directHwOut.getNumChannels())
        {
            directHwOut.addFrom(hwCh, 0, busBuffer, 0, 0, n, lg * 0.5f);
            directHwOut.addFrom(hwCh, 0, busBuffer, 1, 0, n, rg * 0.5f);
        }
        else
        {
            masterOut.addFrom(0, 0, busBuffer, 0, 0, n, lg);
            masterOut.addFrom(1, 0, busBuffer, 1, 0, n, rg);
        }
    }

    juce::String getOutputLabel() const
    {
        const auto dest = static_cast<OutputDestType>(outputDestType.load());
        const int ch = outputHwChannel.load();
        if (dest == OutputDestType::DirectStereoHW)
            return "OUT " + juce::String(ch + 1) + "-" + juce::String(ch + 2);
        if (dest == OutputDestType::DirectMonoHW)
            return "OUT " + juce::String(ch + 1);
        return "MST";
    }

    std::unique_ptr<juce::XmlElement> toXml() const
    {
        auto el = std::make_unique<juce::XmlElement>("FXBus");
        el->setAttribute("index", busIndex);
        el->setAttribute("name", name);
        el->setAttribute("gain", static_cast<double>(gain.load()));
        el->setAttribute("pan", static_cast<double>(pan.load()));
        el->setAttribute("muted", muted.load());
        el->setAttribute("soloed", soloed.load());
        el->setAttribute("outType", outputDestType.load());
        el->setAttribute("outCh", outputHwChannel.load());
        el->addChildElement(inserts.toXml().release());
        return el;
    }

    void fromXml(const juce::XmlElement& el,
                 juce::AudioPluginFormatManager& fmtMgr,
                 double sr, int bs,
                 juce::AudioPlayHead* playHead)
    {
        name = el.getStringAttribute("name", name);
        gain.store(static_cast<float>(el.getDoubleAttribute("gain", 0.85)));
        pan.store(static_cast<float>(el.getDoubleAttribute("pan", 0.0)));
        muted.store(el.getBoolAttribute("muted", false));
        soloed.store(el.getBoolAttribute("soloed", false));
        outputDestType.store(el.getIntAttribute("outType", static_cast<int>(OutputDestType::Master)));
        outputHwChannel.store(el.getIntAttribute("outCh", 0));
        if (auto* insXml = el.getChildByName("InsertChain"))
            inserts.restoreFromXml(*insXml, fmtMgr, sr, bs, playHead);
    }

    juce::String name;
    const int busIndex;
    InsertChain inserts;

    std::atomic<float> gain { 0.85f };
    std::atomic<float> pan { 0.0f };
    std::atomic<bool> muted { false };
    std::atomic<bool> soloed { false };
    std::atomic<int> outputDestType { static_cast<int>(OutputDestType::Master) };
    std::atomic<int> outputHwChannel { 0 };
    std::atomic<float> peakL { 0.0f };
    std::atomic<float> peakR { 0.0f };

private:
    juce::AudioBuffer<float> busBuffer;
    juce::MidiBuffer dummyMidi;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FXBus)
};

// --- Full Powerhouse Channel Strip Track ---
class Track
{
public:
    Track(int uniqueId, int initialTemplateIndex = -1)
        : trackId(uniqueId)
    {
        static const char* defaultNames[8] = {
            "DRUMS", "BASS", "GTR L", "GTR R", "LEAD", "CLEAN", "SYNTH", "FX/VOX"
        };
        static const uint32_t defaultColours[8] = {
            0xffe67e22, 0xff1abc9c, 0xff3498db, 0xff2980b9,
            0xffe74c3c, 0xff2ecc71, 0xff9b59b6, 0xfff1c40f
        };

        if (initialTemplateIndex >= 0 && initialTemplateIndex < 8)
        {
            name = defaultNames[initialTemplateIndex];
            trackColour = juce::Colour(defaultColours[initialTemplateIndex]);

            if (initialTemplateIndex == 0)
            {
                mode.store(TrackMode::Midi);
                inputSourceType.store(static_cast<int>(InputSourceType::DrumSeq));
                useInternalDrumSynth.store(true);
                gain.store(0.88f);
            }
            else if (initialTemplateIndex == 6)
            {
                mode.store(TrackMode::Midi);
                inputSourceType.store(static_cast<int>(InputSourceType::MidiAll));
                useFallbackPolySynth.store(true);
                fallbackSynthPatch.store(static_cast<int>(PolyFallbackSynth::Patch::SawLead));
                gain.store(0.78f);
            }
            else
            {
                mode.store(TrackMode::Audio);
                inputSourceType.store(static_cast<int>(InputSourceType::MonoHW));
                inputChannel.store(0);
                gain.store(0.82f);

                if (initialTemplateIndex == 1)
                    hpfMode.store(static_cast<int>(StereoHighPass::Mode::Hz35));
                else if (initialTemplateIndex == 2)
                {
                    pan.store(-0.75f);
                    hpfMode.store(static_cast<int>(StereoHighPass::Mode::Hz80));
                }
                else if (initialTemplateIndex == 3)
                {
                    pan.store(0.75f);
                    hpfMode.store(static_cast<int>(StereoHighPass::Mode::Hz80));
                }
            }
        }
        else
        {
            name = "TRK " + juce::String(uniqueId);
            trackColour = juce::Colour(defaultColours[static_cast<size_t>(uniqueId % 8)]);
            mode.store(TrackMode::Audio);
            inputSourceType.store(static_cast<int>(InputSourceType::MonoHW));
            inputChannel.store(0);
            gain.store(0.82f);
        }
    }

    ~Track()
    {
        inserts.clearAll();
    }

    void prepare(double sampleRate, int maxBlockSize)
    {
        currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
        const int allocBlock = std::max(maxBlockSize, 4096);
        trackBuffer.setSize(2, allocBlock, false, true, true);
        hpf.prepare(currentSampleRate);
        gate.prepare(currentSampleRate);
        polySynth.prepare(currentSampleRate);
        inserts.prepare(currentSampleRate, maxBlockSize);
    }

    // --- Live Piano-Roll / Melody Audition Queue (Lock-Free) ---
    void pushAuditionNote(int pitch, uint8_t velocity, bool isNoteOn) noexcept
    {
        int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
        auditionFifo.prepareToWrite(1, s1, n1, s2, n2);
        if (n1 > 0)
        {
            auditionRing[static_cast<size_t>(s1)] = isNoteOn
                ? juce::MidiMessage::noteOn(1, juce::jlimit(0, 127, pitch), velocity)
                : juce::MidiMessage::noteOff(1, juce::jlimit(0, 127, pitch));
            auditionFifo.finishedWrite(1);
        }
    }

    // --- Audio Recording ---
    void startAudioRecording(juce::TimeSliceThread& writerThread,
                             double sampleRate,
                             int64_t startSample)
    {
        if (!armed.load() || mode.load() != TrackMode::Audio)
            return;

        recordStartSample = startSample;
        tempRecFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
                          .getNonexistentChildFile("octohost_id" + juce::String(trackId), ".wav");

        juce::WavAudioFormat wavFormat;
        auto stream = tempRecFile.createOutputStream();
        if (stream == nullptr)
            return;

       #if JUCE_MAJOR_VERSION >= 9
        auto options = juce::AudioFormatWriterOptions{}
                           .withSampleRate(sampleRate)
                           .withNumChannels(2)
                           .withBitsPerSample(24);
        auto writer = wavFormat.createWriterFor(stream, options);
       #else
        std::unique_ptr<juce::AudioFormatWriter> writer(
            wavFormat.createWriterFor(stream.release(), sampleRate, 2, 24, {}, 0));
       #endif

        if (writer != nullptr)
        {
            auto threaded = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
                writer.release(), writerThread, 32768);
            const juce::SpinLock::ScopedLockType sl(writerLock);
            activeWriter = std::move(threaded);
        }
    }

    void stopAudioRecording()
    {
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> finishedWriter;
        {
            const juce::SpinLock::ScopedLockType sl(writerLock);
            finishedWriter = std::move(activeWriter);
        }

        if (finishedWriter == nullptr)
            return;

        finishedWriter.reset();

        juce::WavAudioFormat wavFormat;
        if (auto inStream = tempRecFile.createInputStream())
        {
            if (auto reader = std::unique_ptr<juce::AudioFormatReader>(
                    wavFormat.createReaderFor(inStream.release(), true)))
            {
                const int recLen = static_cast<int>(reader->lengthInSamples);
                if (recLen > 0)
                {
                    pushAudioUndo();
                    juce::AudioBuffer<float> recBuf(2, recLen);
                    reader->read(&recBuf, 0, recLen, 0, true, true);

                    const int startPos = static_cast<int>(std::max<int64_t>(0, recordStartSample));
                    const int newLen = std::max(audioClip.getNumSamples(), startPos + recLen);

                    juce::AudioBuffer<float> merged(2, newLen);
                    merged.clear();

                    if (audioClip.getNumSamples() > 0)
                    {
                        for (int ch = 0; ch < 2; ++ch)
                            merged.copyFrom(ch, 0, audioClip, ch, 0, audioClip.getNumSamples());
                    }

                    for (int ch = 0; ch < 2; ++ch)
                        merged.copyFrom(ch, startPos, recBuf, ch, 0, recLen);

                    {
                        const juce::SpinLock::ScopedLockType sl(audioLock);
                        audioClip = std::move(merged);
                        audioLengthSamples.store(audioClip.getNumSamples());
                    }
                    rebuildWaveformCache();
                }
            }
        }

        tempRecFile.deleteFile();
    }

    // --- Undoable Audio Editor Operations (Message Thread) ---
    bool canUndoAudio() const { return !audioUndoStack.empty(); }

    void undoAudioEdit()
    {
        if (audioUndoStack.empty())
            return;
        auto prev = std::move(audioUndoStack.back());
        audioUndoStack.pop_back();
        {
            const juce::SpinLock::ScopedLockType sl(audioLock);
            audioClip = std::move(prev);
            audioLengthSamples.store(audioClip.getNumSamples());
        }
        rebuildWaveformCache();
    }

    void audioTrimRange(int startSample, int endSample)
    {
        juce::AudioBuffer<float> copy = copyAudioClip();
        const int total = copy.getNumSamples();
        if (total <= 0) return;
        startSample = juce::jlimit(0, total - 1, startSample);
        endSample = juce::jlimit(startSample + 1, total, endSample);
        const int newLen = endSample - startSample;

        pushAudioUndo();
        juce::AudioBuffer<float> trimmed(2, newLen);
        for (int ch = 0; ch < 2; ++ch)
            trimmed.copyFrom(ch, 0, copy, ch, startSample, newLen);
        replaceAudioClip(std::move(trimmed));
    }

    void audioSilenceRange(int startSample, int endSample)
    {
        juce::AudioBuffer<float> copy = copyAudioClip();
        const int total = copy.getNumSamples();
        if (total <= 0) return;
        startSample = juce::jlimit(0, total - 1, startSample);
        endSample = juce::jlimit(startSample + 1, total, endSample);

        pushAudioUndo();
        copy.clear(startSample, endSample - startSample);
        replaceAudioClip(std::move(copy));
    }

    void audioFadeIn(int startSample, int endSample)
    {
        juce::AudioBuffer<float> copy = copyAudioClip();
        const int total = copy.getNumSamples();
        if (total <= 0) return;
        startSample = juce::jlimit(0, total - 1, startSample);
        endSample = juce::jlimit(startSample + 1, total, endSample);
        const int len = endSample - startSample;

        pushAudioUndo();
        for (int ch = 0; ch < copy.getNumChannels(); ++ch)
        {
            float* d = copy.getWritePointer(ch);
            for (int i = 0; i < len; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(std::max(1, len - 1));
                const float curve = 0.5f * (1.0f - std::cos(juce::MathConstants<float>::pi * t));
                d[startSample + i] *= curve;
            }
        }
        replaceAudioClip(std::move(copy));
    }

    void audioFadeOut(int startSample, int endSample)
    {
        juce::AudioBuffer<float> copy = copyAudioClip();
        const int total = copy.getNumSamples();
        if (total <= 0) return;
        startSample = juce::jlimit(0, total - 1, startSample);
        endSample = juce::jlimit(startSample + 1, total, endSample);
        const int len = endSample - startSample;

        pushAudioUndo();
        for (int ch = 0; ch < copy.getNumChannels(); ++ch)
        {
            float* d = copy.getWritePointer(ch);
            for (int i = 0; i < len; ++i)
            {
                const float t = static_cast<float>(i) / static_cast<float>(std::max(1, len - 1));
                const float curve = 0.5f * (1.0f + std::cos(juce::MathConstants<float>::pi * t));
                d[startSample + i] *= curve;
            }
        }
        replaceAudioClip(std::move(copy));
    }

    void audioNormalize(int startSample, int endSample, float targetDb = -1.0f)
    {
        juce::AudioBuffer<float> copy = copyAudioClip();
        const int total = copy.getNumSamples();
        if (total <= 0) return;
        startSample = juce::jlimit(0, total - 1, startSample);
        endSample = juce::jlimit(startSample + 1, total, endSample);
        const int len = endSample - startSample;

        const float peak = copy.getMagnitude(startSample, len);
        if (peak < 0.00001f) return;

        pushAudioUndo();
        const float targetLin = std::pow(10.0f, targetDb / 20.0f);
        copy.applyGain(startSample, len, targetLin / peak);
        replaceAudioClip(std::move(copy));
    }

    void audioApplyGainDb(int startSample, int endSample, float gainDb)
    {
        juce::AudioBuffer<float> copy = copyAudioClip();
        const int total = copy.getNumSamples();
        if (total <= 0) return;
        startSample = juce::jlimit(0, total - 1, startSample);
        endSample = juce::jlimit(startSample + 1, total, endSample);

        pushAudioUndo();
        copy.applyGain(startSample, endSample - startSample, std::pow(10.0f, gainDb / 20.0f));
        replaceAudioClip(std::move(copy));
    }

    void audioReverse(int startSample, int endSample)
    {
        juce::AudioBuffer<float> copy = copyAudioClip();
        const int total = copy.getNumSamples();
        if (total <= 0) return;
        startSample = juce::jlimit(0, total - 1, startSample);
        endSample = juce::jlimit(startSample + 1, total, endSample);

        pushAudioUndo();
        for (int ch = 0; ch < copy.getNumChannels(); ++ch)
            std::reverse(copy.getWritePointer(ch) + startSample, copy.getWritePointer(ch) + endSample);
        replaceAudioClip(std::move(copy));
    }

    void audioAutoTrimSilence(float thresh = 0.004f)
    {
        juce::AudioBuffer<float> copy = copyAudioClip();
        const int total = copy.getNumSamples();
        if (total <= 0) return;

        int first = 0, last = total - 1;
        const float* c0 = copy.getReadPointer(0);
        const float* c1 = copy.getNumChannels() > 1 ? copy.getReadPointer(1) : c0;

        while (first < total && std::max(std::abs(c0[first]), std::abs(c1[first])) < thresh) ++first;
        while (last > first && std::max(std::abs(c0[last]), std::abs(c1[last])) < thresh) --last;

        if (first >= last) return;
        audioTrimRange(std::max(0, first - 256), std::min(total, last + 512));
    }

    void audioNudgeSamples(int deltaSamples)
    {
        juce::AudioBuffer<float> copy = copyAudioClip();
        const int total = copy.getNumSamples();
        if (total <= 0 || deltaSamples == 0) return;

        pushAudioUndo();
        if (deltaSamples > 0)
        {
            juce::AudioBuffer<float> shifted(2, total + deltaSamples);
            shifted.clear();
            for (int ch = 0; ch < 2; ++ch)
                shifted.copyFrom(ch, deltaSamples, copy, ch, 0, total);
            replaceAudioClip(std::move(shifted));
        }
        else
        {
            const int cut = std::min(total - 1, -deltaSamples);
            const int newLen = std::max(1, total - cut);
            juce::AudioBuffer<float> shifted(2, newLen);
            for (int ch = 0; ch < 2; ++ch)
                shifted.copyFrom(ch, 0, copy, ch, cut, newLen);
            replaceAudioClip(std::move(shifted));
        }
    }

    void getWaveformPreviewRange(int startSample, int endSample, int numBuckets,
                                 std::vector<float>& outMin, std::vector<float>& outMax) const
    {
        outMin.clear();
        outMax.clear();
        if (numBuckets <= 0) return;

        const juce::SpinLock::ScopedLockType sl(audioLock);
        const int total = audioClip.getNumSamples();
        if (total <= 0) return;

        startSample = juce::jlimit(0, total - 1, startSample);
        endSample = juce::jlimit(startSample + 1, total, endSample);
        const int span = endSample - startSample;

        outMin.resize(static_cast<size_t>(numBuckets), 0.0f);
        outMax.resize(static_cast<size_t>(numBuckets), 0.0f);

        const float* c0 = audioClip.getReadPointer(0);
        const float* c1 = audioClip.getNumChannels() > 1 ? audioClip.getReadPointer(1) : c0;

        for (int b = 0; b < numBuckets; ++b)
        {
            const int s = startSample + static_cast<int>((static_cast<int64_t>(b) * span) / numBuckets);
            const int e = startSample + std::max(b + 1, static_cast<int>((static_cast<int64_t>(b + 1) * span) / numBuckets));
            float mn = 0.0f, mx = 0.0f;
            for (int i = s; i < std::min(e, endSample); ++i)
            {
                const float v = 0.5f * (c0[i] + c1[i]);
                mn = std::min(mn, v);
                mx = std::max(mx, v);
            }
            outMin[static_cast<size_t>(b)] = mn;
            outMax[static_cast<size_t>(b)] = mx;
        }
    }

    // --- Import / Export WAV ---
    bool importWavFile(const juce::File& file)
    {
        juce::WavAudioFormat wavFormat;
        auto inStream = file.createInputStream();
        if (inStream == nullptr) return false;

        std::unique_ptr<juce::AudioFormatReader> reader(
            wavFormat.createReaderFor(inStream.release(), true));
        if (reader == nullptr || reader->lengthInSamples <= 0) return false;

        pushAudioUndo();
        const int len = static_cast<int>(std::min<int64_t>(reader->lengthInSamples, 48000 * 600));
        juce::AudioBuffer<float> loaded(2, len);
        reader->read(&loaded, 0, len, 0, true, true);

        mode.store(TrackMode::Audio);
        replaceAudioClip(std::move(loaded));
        return true;
    }

    bool exportClipWav(const juce::File& file, double sampleRate, int startSample = 0, int endSample = -1) const
    {
        juce::AudioBuffer<float> copy = copyAudioClip();
        const int total = copy.getNumSamples();
        if (total <= 0) return false;

        startSample = juce::jlimit(0, total - 1, startSample);
        endSample = (endSample < 0) ? total : juce::jlimit(startSample + 1, total, endSample);

        file.deleteFile();
        juce::WavAudioFormat wavFormat;
        auto stream = file.createOutputStream();
        if (stream == nullptr) return false;

       #if JUCE_MAJOR_VERSION >= 9
        auto options = juce::AudioFormatWriterOptions{}
                           .withSampleRate(sampleRate)
                           .withNumChannels(2)
                           .withBitsPerSample(24);
        auto writer = wavFormat.createWriterFor(stream, options);
       #else
        std::unique_ptr<juce::AudioFormatWriter> writer(
            wavFormat.createWriterFor(stream.release(), sampleRate, 2, 24, {}, 0));
       #endif

        if (writer == nullptr) return false;
        return writer->writeFromAudioSampleBuffer(copy, startSample, endSample - startSample);
    }

    // --- MIDI Note Clip & Piano Roll Sync ---
    std::vector<MidiNote> getMidiNotes(double bpm, double sr) const
    {
        if (!midiNotesCache.empty())
            return midiNotesCache;

        std::vector<std::pair<int64_t, juce::MidiMessage>> raw;
        {
            const juce::SpinLock::ScopedLockType sl(midiLock);
            raw.reserve(playbackMidi.size());
            for (const auto& ev : playbackMidi)
                raw.emplace_back(ev.samplePos, ev.message);
        }
        return MidiHelpers::timedEventsToNotes(raw, bpm, sr);
    }

    void setMidiNotes(const std::vector<MidiNote>& notes, double bpm, double sr)
    {
        midiNotesCache = notes;
        auto timed = MidiHelpers::notesToTimedEvents(notes, bpm, sr);
        replaceMidiClip(timed, false);
    }

    void resyncMidiToBpm(double bpm, double sr)
    {
        if (!midiNotesCache.empty())
        {
            auto timed = MidiHelpers::notesToTimedEvents(midiNotesCache, bpm, sr);
            replaceMidiClip(timed, false);
        }
    }

    void drainRecordedMidi(double bpm, double sr)
    {
        const int numReady = midiFifo.getNumReady();
        if (numReady == 0)
            return;

        int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
        midiFifo.prepareToRead(numReady, s1, n1, s2, n2);
        for (int i = 0; i < n1; ++i)
            midiEventsStaging.push_back(midiRingBuffer[static_cast<size_t>(s1 + i)]);
        for (int i = 0; i < n2; ++i)
            midiEventsStaging.push_back(midiRingBuffer[static_cast<size_t>(s2 + i)]);
        midiFifo.finishedRead(n1 + n2);

        commitStagingMidi();

        std::vector<std::pair<int64_t, juce::MidiMessage>> raw;
        raw.reserve(midiEventsStaging.size());
        for (const auto& ev : midiEventsStaging)
            raw.emplace_back(ev.samplePos, ev.message);
        midiNotesCache = MidiHelpers::timedEventsToNotes(raw, bpm, sr);
    }

    void replaceMidiClip(const std::vector<std::pair<int64_t, juce::MidiMessage>>& events, bool rebuildNoteCache = true, double bpm = 120.0, double sr = 44100.0)
    {
        midiEventsStaging.clear();
        midiEventsStaging.reserve(events.size());
        for (const auto& kv : events)
            midiEventsStaging.push_back({ kv.first, kv.second });
        commitStagingMidi();
        if (rebuildNoteCache)
            midiNotesCache = MidiHelpers::timedEventsToNotes(events, bpm, sr);
    }

    void clearClip()
    {
        pushAudioUndo();
        {
            const juce::SpinLock::ScopedLockType sl(audioLock);
            audioClip.setSize(2, 0);
            audioLengthSamples.store(0);
        }
        {
            const juce::SpinLock::ScopedLockType sl(waveLock);
            waveformPeaks.clear();
        }
        midiEventsStaging.clear();
        midiNotesCache.clear();
        {
            const juce::SpinLock::ScopedLockType sl(midiLock);
            playbackMidi.clear();
            midiLengthSamples.store(0);
        }
    }

    void copyClipsFrom(const Track& other)
    {
        auto audioCopy = other.copyAudioClip();
        replaceAudioClip(std::move(audioCopy));
        midiNotesCache = other.midiNotesCache;
        {
            const juce::SpinLock::ScopedLockType sl(other.midiLock);
            midiEventsStaging = other.playbackMidi;
        }
        commitStagingMidi();
    }

    // --- UI Helpers ---
    void getWaveformPreview(std::vector<float>& outPeaks) const
    {
        const juce::SpinLock::ScopedLockType sl(waveLock);
        outPeaks = waveformPeaks;
    }

    void getMidiPreviewPositions(std::vector<int64_t>& outSamplePositions) const
    {
        outSamplePositions.clear();
        const juce::SpinLock::ScopedLockType sl(midiLock);
        for (const auto& ev : playbackMidi)
            if (ev.message.isNoteOn())
                outSamplePositions.push_back(ev.samplePos);
    }

    juce::String getInputLabel() const
    {
        const auto t = static_cast<InputSourceType>(inputSourceType.load());
        const int ch = inputChannel.load();
        switch (t)
        {
            case InputSourceType::MonoHW:      return "IN " + juce::String(ch + 1);
            case InputSourceType::StereoHW:    return "IN " + juce::String(ch + 1) + "-" + juce::String(ch + 2);
            case InputSourceType::MidiAll:     return "MIDI ALL";
            case InputSourceType::MidiChannel: return "MIDI CH" + juce::String(ch);
            case InputSourceType::DrumSeq:     return "DRUM SEQ";
            case InputSourceType::None:        return "NO IN";
        }
        return "---";
    }

    juce::String getOutputLabel() const
    {
        const auto d = static_cast<OutputDestType>(outputDestType.load());
        const int ch = outputHwChannel.load();
        switch (d)
        {
            case OutputDestType::Master:         return "MST";
            case OutputDestType::FxBusA:         return "FX A";
            case OutputDestType::FxBusB:         return "FX B";
            case OutputDestType::DirectStereoHW: return "OUT " + juce::String(ch + 1) + "-" + juce::String(ch + 2);
            case OutputDestType::DirectMonoHW:   return "OUT " + juce::String(ch + 1);
        }
        return "MST";
    }

    // --- Real-Time Audio Thread Processing ---
    void process(const float* const* inputData,
                 int numInputChannels,
                 juce::AudioBuffer<float>& masterOut,
                 FXBus& fxBusA,
                 FXBus& fxBusB,
                 juce::AudioBuffer<float>& directHwOut,
                 const juce::MidiBuffer& liveMidi,
                 const juce::MidiBuffer& drumSeqMidi,
                 DrumEngine& drumEngine,
                 ChromaticTuner& tuner,
                 bool tapTunerFromThisTrack,
                 int numSamples,
                 int64_t playhead,
                 bool isPlaying,
                 bool isRecording,
                 bool anyTrackSoloed,
                 bool sendAllNotesOff) noexcept
    {
        const int n = std::min(numSamples, trackBuffer.getNumSamples());
        if (n <= 0) return;

        trackBuffer.clear(0, n);
        trackMidi.clear();

        const bool isArmed = armed.load(std::memory_order_relaxed);
        const TrackMode trackMode = mode.load(std::memory_order_relaxed);
        const auto inType = static_cast<InputSourceType>(inputSourceType.load(std::memory_order_relaxed));
        const int inCh = inputChannel.load(std::memory_order_relaxed);

        if (trackMode == TrackMode::Audio)
        {
            if (isPlaying)
            {
                const juce::SpinLock::ScopedTryLockType sl(audioLock);
                if (sl.isLocked() && audioClip.getNumSamples() > 0 && playhead < audioClip.getNumSamples())
                {
                    const int available = static_cast<int>(
                        std::min<int64_t>(n, audioClip.getNumSamples() - playhead));
                    for (int ch = 0; ch < 2; ++ch)
                        trackBuffer.copyFrom(ch, 0, audioClip, ch, static_cast<int>(playhead), available);
                }
            }

            if (inputData != nullptr && numInputChannels > 0
                && (inType == InputSourceType::MonoHW || inType == InputSourceType::StereoHW))
            {
                const int c0 = juce::jlimit(0, numInputChannels - 1, inCh);
                const int c1 = (inType == InputSourceType::StereoHW && c0 + 1 < numInputChannels) ? (c0 + 1) : c0;
                const float* src0 = inputData[c0];
                const float* src1 = inputData[c1];

                if (src0 != nullptr)
                {
                    if (tapTunerFromThisTrack)
                        tuner.pushSamples(src0, n);

                    if (isArmed && isRecording)
                    {
                        const float* stereoRec[2] = { src0, src1 != nullptr ? src1 : src0 };
                        const juce::SpinLock::ScopedTryLockType sl(writerLock);
                        if (sl.isLocked() && activeWriter != nullptr)
                            activeWriter->write(stereoRec, n);
                    }

                    const bool muteForTuner = tapTunerFromThisTrack
                                              && tuner.active.load(std::memory_order_relaxed)
                                              && tuner.muteWhileTuning.load(std::memory_order_relaxed);

                    if (isArmed && inputMonitor.load(std::memory_order_relaxed) && !muteForTuner)
                    {
                        trackBuffer.addFrom(0, 0, src0, n);
                        trackBuffer.addFrom(1, 0, src1 != nullptr ? src1 : src0, n);
                    }
                }
            }

            // Channel Strip Input DSP: Trim -> Phase -> HPF -> Noise Gate -> Analog Drive
            const float trimLin = std::pow(10.0f, inputTrimDb.load(std::memory_order_relaxed) / 20.0f);
            const float pol = phaseInvert.load(std::memory_order_relaxed) ? -1.0f : 1.0f;
            if (std::abs(trimLin * pol - 1.0f) > 0.0001f)
                trackBuffer.applyGain(0, n, trimLin * pol);

            hpf.process(trackBuffer, n, static_cast<StereoHighPass::Mode>(hpfMode.load(std::memory_order_relaxed)));
            gate.process(trackBuffer, n, static_cast<InputNoiseGate::Mode>(gateMode.load(std::memory_order_relaxed)));
            AnalogDriveStage::process(trackBuffer, n, static_cast<AnalogDriveStage::Mode>(driveMode.load(std::memory_order_relaxed)));
        }
        else // TrackMode::Midi
        {
            if (isPlaying)
            {
                const juce::SpinLock::ScopedTryLockType sl(midiLock);
                if (sl.isLocked() && !playbackMidi.empty())
                {
                    auto it = std::lower_bound(
                        playbackMidi.begin(), playbackMidi.end(), playhead,
                        [](const TimedMidiEvent& ev, int64_t pos) { return ev.samplePos < pos; });

                    const int64_t blockEnd = playhead + n;
                    while (it != playbackMidi.end() && it->samplePos < blockEnd)
                    {
                        const int offset = static_cast<int>(it->samplePos - playhead);
                        trackMidi.addEvent(it->message, juce::jlimit(0, n - 1, offset));
                        ++it;
                    }
                }
            }

            if (inType == InputSourceType::DrumSeq)
            {
                for (const auto meta : drumSeqMidi)
                    trackMidi.addEvent(meta.getMessage(), juce::jlimit(0, n - 1, meta.samplePosition));
            }

            if (isArmed && (inType == InputSourceType::MidiAll
                            || inType == InputSourceType::MidiChannel
                            || inType == InputSourceType::DrumSeq))
            {
                for (const auto meta : liveMidi)
                {
                    const auto msg = meta.getMessage();
                    if (inType == InputSourceType::MidiChannel && msg.getChannel() != inCh)
                        continue;

                    const int offset = juce::jlimit(0, n - 1, meta.samplePosition);
                    trackMidi.addEvent(msg, offset);

                    if (isRecording && msg.getRawDataSize() <= 8)
                        pushRecordedMidi(playhead + offset, msg);
                }
            }

            // Drain Piano Roll / Audition queue
            const int audReady = auditionFifo.getNumReady();
            if (audReady > 0)
            {
                int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
                auditionFifo.prepareToRead(audReady, s1, n1, s2, n2);
                for (int i = 0; i < n1; ++i) trackMidi.addEvent(auditionRing[static_cast<size_t>(s1 + i)], 0);
                for (int i = 0; i < n2; ++i) trackMidi.addEvent(auditionRing[static_cast<size_t>(s2 + i)], 0);
                auditionFifo.finishedRead(n1 + n2);
            }

            if (sendAllNotesOff)
            {
                for (int ch = 1; ch <= 16; ++ch)
                    trackMidi.addEvent(juce::MidiMessage::allNotesOff(ch), 0);
            }

            // Built-in synths automatically step aside if user loads any VSTi/AUi instrument!
            if (!inserts.hasAnyInstrument())
            {
                if (useInternalDrumSynth.load(std::memory_order_relaxed))
                    drumEngine.renderSynth(trackBuffer, trackMidi, n);
                else if (useFallbackPolySynth.load(std::memory_order_relaxed))
                    polySynth.render(trackBuffer, trackMidi, n,
                                     static_cast<PolyFallbackSynth::Patch>(fallbackSynthPatch.load(std::memory_order_relaxed)));
            }
        }

        // 4 VST3/AU Inserts
        inserts.process(trackBuffer, trackMidi, n);

        const bool isMuted = muted.load(std::memory_order_relaxed);
        const bool isSoloed = soloed.load(std::memory_order_relaxed);
        const bool audible = !isMuted && (!anyTrackSoloed || isSoloed);

        if (!audible)
        {
            peakL.store(0.0f, std::memory_order_relaxed);
            peakR.store(0.0f, std::memory_order_relaxed);
            return;
        }

        const float sA = sendAGain.load(std::memory_order_relaxed);
        const float sB = sendBGain.load(std::memory_order_relaxed);
        const bool preA = sendAPreFader.load(std::memory_order_relaxed);
        const bool preB = sendBPreFader.load(std::memory_order_relaxed);

        if (preA && sA > 0.0001f) fxBusA.addInput(trackBuffer, n, sA);
        if (preB && sB > 0.0001f) fxBusB.addInput(trackBuffer, n, sB);

        const float g = gain.load(std::memory_order_relaxed);
        const float p = (pan.load(std::memory_order_relaxed) + 1.0f) * 0.5f;
        const float lg = g * std::cos(p * juce::MathConstants<float>::halfPi);
        const float rg = g * std::sin(p * juce::MathConstants<float>::halfPi);

        trackBuffer.applyGain(0, 0, n, lg);
        trackBuffer.applyGain(1, 0, n, rg);

        peakL.store(trackBuffer.getMagnitude(0, 0, n), std::memory_order_relaxed);
        peakR.store(trackBuffer.getMagnitude(1, 0, n), std::memory_order_relaxed);

        if (!preA && sA > 0.0001f) fxBusA.addInput(trackBuffer, n, sA);
        if (!preB && sB > 0.0001f) fxBusB.addInput(trackBuffer, n, sB);

        const auto dest = static_cast<OutputDestType>(outputDestType.load(std::memory_order_relaxed));
        const int hwCh = outputHwChannel.load(std::memory_order_relaxed);

        switch (dest)
        {
            case OutputDestType::FxBusA:
                fxBusA.addInput(trackBuffer, n, 1.0f);
                break;
            case OutputDestType::FxBusB:
                fxBusB.addInput(trackBuffer, n, 1.0f);
                break;
            case OutputDestType::DirectStereoHW:
                if (hwCh + 1 < directHwOut.getNumChannels())
                {
                    directHwOut.addFrom(hwCh, 0, trackBuffer, 0, 0, n);
                    directHwOut.addFrom(hwCh + 1, 0, trackBuffer, 1, 0, n);
                }
                break;
            case OutputDestType::DirectMonoHW:
                if (hwCh < directHwOut.getNumChannels())
                {
                    directHwOut.addFrom(hwCh, 0, trackBuffer, 0, 0, n, 0.5f);
                    directHwOut.addFrom(hwCh, 0, trackBuffer, 1, 0, n, 0.5f);
                }
                break;
            case OutputDestType::Master:
            default:
                masterOut.addFrom(0, 0, trackBuffer, 0, 0, n);
                masterOut.addFrom(1, 0, trackBuffer, 1, 0, n);
                break;
        }
    }

    int64_t getClipLengthSamples() const
    {
        return mode.load() == TrackMode::Audio ? audioLengthSamples.load()
                                               : midiLengthSamples.load();
    }

    // --- XML Serialization ---
    std::unique_ptr<juce::XmlElement> toXml(int orderIndex, const juce::File& audioDir, double sampleRate) const
    {
        auto el = std::make_unique<juce::XmlElement>("Track");
        el->setAttribute("index", orderIndex);
        el->setAttribute("id", trackId);
        el->setAttribute("name", name);
        el->setAttribute("colour", trackColour.toString());
        el->setAttribute("mode", static_cast<int>(mode.load()));
        el->setAttribute("inType", inputSourceType.load());
        el->setAttribute("inCh", inputChannel.load());
        el->setAttribute("outType", outputDestType.load());
        el->setAttribute("outCh", outputHwChannel.load());
        el->setAttribute("trimDb", static_cast<double>(inputTrimDb.load()));
        el->setAttribute("phase", phaseInvert.load());
        el->setAttribute("hpf", hpfMode.load());
        el->setAttribute("gate", gateMode.load());
        el->setAttribute("drive", driveMode.load());
        el->setAttribute("drumSynth", useInternalDrumSynth.load());
        el->setAttribute("polySynth", useFallbackPolySynth.load());
        el->setAttribute("polyPatch", fallbackSynthPatch.load());
        el->setAttribute("sendA", static_cast<double>(sendAGain.load()));
        el->setAttribute("sendAPre", sendAPreFader.load());
        el->setAttribute("sendB", static_cast<double>(sendBGain.load()));
        el->setAttribute("sendBPre", sendBPreFader.load());
        el->setAttribute("gain", static_cast<double>(gain.load()));
        el->setAttribute("pan", static_cast<double>(pan.load()));
        el->setAttribute("muted", muted.load());
        el->setAttribute("soloed", soloed.load());

        el->addChildElement(inserts.toXml().release());

        if (audioLengthSamples.load() > 0)
        {
            const juce::String wavName = "track_" + juce::String(orderIndex + 1) + "_" + juce::String(trackId) + ".wav";
            if (exportClipWav(audioDir.getChildFile(wavName), sampleRate))
                el->setAttribute("audioFile", wavName);
        }

        if (!midiNotesCache.empty())
        {
            auto* notesEl = el->createNewChildElement("MidiNotes");
            for (const auto& n : midiNotesCache)
            {
                auto* ne = notesEl->createNewChildElement("N");
                ne->setAttribute("p", n.pitch);
                ne->setAttribute("s", n.startBeat);
                ne->setAttribute("l", n.lengthBeats);
                ne->setAttribute("v", static_cast<int>(n.velocity));
                ne->setAttribute("c", n.channel);
            }
        }
        return el;
    }

    void fromXml(const juce::XmlElement& el,
                 const juce::File& audioDir,
                 juce::AudioPluginFormatManager& fmtMgr,
                 double bpm, double sr, int bs,
                 juce::AudioPlayHead* playHead)
    {
        clearClip();
        name = el.getStringAttribute("name", name);
        const auto colStr = el.getStringAttribute("colour");
        if (colStr.isNotEmpty())
            trackColour = juce::Colour::fromString(colStr);
        mode.store(static_cast<TrackMode>(el.getIntAttribute("mode", 0)));
        inputSourceType.store(el.getIntAttribute("inType", static_cast<int>(InputSourceType::MonoHW)));
        inputChannel.store(el.getIntAttribute("inCh", 0));
        outputDestType.store(el.getIntAttribute("outType", static_cast<int>(OutputDestType::Master)));
        outputHwChannel.store(el.getIntAttribute("outCh", 0));
        inputTrimDb.store(static_cast<float>(el.getDoubleAttribute("trimDb", 0.0)));
        phaseInvert.store(el.getBoolAttribute("phase", false));
        hpfMode.store(el.getIntAttribute("hpf", 0));
        gateMode.store(el.getIntAttribute("gate", 0));
        driveMode.store(el.getIntAttribute("drive", 0));
        useInternalDrumSynth.store(el.getBoolAttribute("drumSynth", false));
        useFallbackPolySynth.store(el.getBoolAttribute("polySynth", true));
        fallbackSynthPatch.store(el.getIntAttribute("polyPatch", 0));
        sendAGain.store(static_cast<float>(el.getDoubleAttribute("sendA", 0.0)));
        sendAPreFader.store(el.getBoolAttribute("sendAPre", false));
        sendBGain.store(static_cast<float>(el.getDoubleAttribute("sendB", 0.0)));
        sendBPreFader.store(el.getBoolAttribute("sendBPre", false));
        gain.store(static_cast<float>(el.getDoubleAttribute("gain", 0.8)));
        pan.store(static_cast<float>(el.getDoubleAttribute("pan", 0.0)));
        muted.store(el.getBoolAttribute("muted", false));
        soloed.store(el.getBoolAttribute("soloed", false));

        if (auto* insXml = el.getChildByName("InsertChain"))
            inserts.restoreFromXml(*insXml, fmtMgr, sr, bs, playHead);

        const auto wavName = el.getStringAttribute("audioFile");
        if (wavName.isNotEmpty())
            importWavFile(audioDir.getChildFile(wavName));

        if (auto* notesEl = el.getChildByName("MidiNotes"))
        {
            std::vector<MidiNote> loaded;
            for (auto* ne : notesEl->getChildWithTagNameIterator("N"))
            {
                MidiNote n;
                n.pitch = ne->getIntAttribute("p", 60);
                n.startBeat = ne->getDoubleAttribute("s", 0.0);
                n.lengthBeats = ne->getDoubleAttribute("l", 0.5);
                n.velocity = static_cast<uint8_t>(juce::jlimit(1, 127, ne->getIntAttribute("v", 100)));
                n.channel = ne->getIntAttribute("c", 1);
                loaded.push_back(n);
            }
            setMidiNotes(loaded, bpm, sr);
        }
    }

    const int trackId;
    juce::String name;
    juce::Colour trackColour;
    InsertChain inserts;

    std::atomic<TrackMode> mode { TrackMode::Audio };
    std::atomic<int> inputSourceType { static_cast<int>(InputSourceType::MonoHW) };
    std::atomic<int> inputChannel { 0 };
    std::atomic<int> outputDestType { static_cast<int>(OutputDestType::Master) };
    std::atomic<int> outputHwChannel { 0 };

    std::atomic<float> inputTrimDb { 0.0f };
    std::atomic<bool> phaseInvert { false };
    std::atomic<int> hpfMode { static_cast<int>(StereoHighPass::Mode::Off) };
    std::atomic<int> gateMode { static_cast<int>(InputNoiseGate::Mode::Off) };
    std::atomic<int> driveMode { static_cast<int>(AnalogDriveStage::Mode::Off) };
    std::atomic<bool> useInternalDrumSynth { false };
    std::atomic<bool> useFallbackPolySynth { true };
    std::atomic<int> fallbackSynthPatch { static_cast<int>(PolyFallbackSynth::Patch::AnalogSub) };

    std::atomic<float> sendAGain { 0.0f };
    std::atomic<bool> sendAPreFader { false };
    std::atomic<float> sendBGain { 0.0f };
    std::atomic<bool> sendBPreFader { false };

    std::atomic<bool> armed { false };
    std::atomic<bool> inputMonitor { false };
    std::atomic<bool> muted { false };
    std::atomic<bool> soloed { false };
    std::atomic<float> gain { 0.8f };
    std::atomic<float> pan { 0.0f };
    std::atomic<float> peakL { 0.0f };
    std::atomic<float> peakR { 0.0f };

private:
    juce::AudioBuffer<float> copyAudioClip() const
    {
        const juce::SpinLock::ScopedLockType sl(audioLock);
        juce::AudioBuffer<float> copy;
        if (audioClip.getNumSamples() > 0)
            copy.makeCopyOf(audioClip);
        return copy;
    }

    void replaceAudioClip(juce::AudioBuffer<float>&& newClip)
    {
        {
            const juce::SpinLock::ScopedLockType sl(audioLock);
            audioClip = std::move(newClip);
            audioLengthSamples.store(audioClip.getNumSamples());
        }
        rebuildWaveformCache();
    }

    void pushAudioUndo()
    {
        auto copy = copyAudioClip();
        if (copy.getNumSamples() <= 0) return;
        audioUndoStack.push_back(std::move(copy));
        if (audioUndoStack.size() > 8)
            audioUndoStack.pop_front();
    }

    void pushRecordedMidi(int64_t samplePos, const juce::MidiMessage& msg) noexcept
    {
        int s1 = 0, n1 = 0, s2 = 0, n2 = 0;
        midiFifo.prepareToWrite(1, s1, n1, s2, n2);
        if (n1 > 0)
        {
            midiRingBuffer[static_cast<size_t>(s1)] = { samplePos, msg };
            midiFifo.finishedWrite(1);
        }
    }

    void commitStagingMidi()
    {
        std::sort(midiEventsStaging.begin(), midiEventsStaging.end(),
                  [](const TimedMidiEvent& a, const TimedMidiEvent& b) {
                      return a.samplePos < b.samplePos;
                  });
        auto copy = midiEventsStaging;
        const int64_t maxSample = copy.empty() ? 0 : copy.back().samplePos;
        {
            const juce::SpinLock::ScopedLockType sl(midiLock);
            playbackMidi = std::move(copy);
            midiLengthSamples.store(maxSample);
        }
    }

    void rebuildWaveformCache()
    {
        std::vector<float> peaks;
        {
            const juce::SpinLock::ScopedLockType sl(audioLock);
            const int total = audioClip.getNumSamples();
            if (total > 0)
            {
                constexpr int buckets = 360;
                peaks.resize(buckets, 0.0f);
                const float* c0 = audioClip.getReadPointer(0);
                const float* c1 = audioClip.getNumChannels() > 1 ? audioClip.getReadPointer(1) : c0;
                for (int b = 0; b < buckets; ++b)
                {
                    const int start = static_cast<int>((static_cast<int64_t>(b) * total) / buckets);
                    const int end = std::max(start + 1, static_cast<int>((static_cast<int64_t>(b + 1) * total) / buckets));
                    float pk = 0.0f;
                    for (int i = start; i < std::min(end, total); ++i)
                        pk = std::max(pk, std::max(std::abs(c0[i]), std::abs(c1[i])));
                    peaks[static_cast<size_t>(b)] = pk;
                }
            }
        }
        const juce::SpinLock::ScopedLockType sl(waveLock);
        waveformPeaks = std::move(peaks);
    }

    double currentSampleRate = 44100.0;
    juce::AudioBuffer<float> trackBuffer;
    juce::MidiBuffer trackMidi;
    StereoHighPass hpf;
    InputNoiseGate gate;
    PolyFallbackSynth polySynth;

    // Audio clip + undo stack + waveform preview + threaded writer
    mutable juce::SpinLock audioLock;
    juce::AudioBuffer<float> audioClip;
    std::atomic<int64_t> audioLengthSamples { 0 };
    std::deque<juce::AudioBuffer<float>> audioUndoStack;
    mutable juce::SpinLock waveLock;
    std::vector<float> waveformPeaks;

    juce::SpinLock writerLock;
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> activeWriter;
    juce::File tempRecFile;
    int64_t recordStartSample = 0;

    // MIDI clip + lock-free FIFO + Piano Roll note cache + Audition queue
    static constexpr int midiFifoCapacity = 8192;
    juce::AbstractFifo midiFifo { midiFifoCapacity };
    std::array<TimedMidiEvent, midiFifoCapacity> midiRingBuffer;
    std::vector<TimedMidiEvent> midiEventsStaging;
    std::vector<MidiNote> midiNotesCache;
    mutable juce::SpinLock midiLock;
    std::vector<TimedMidiEvent> playbackMidi;
    std::atomic<int64_t> midiLengthSamples { 0 };

    static constexpr int auditionCapacity = 256;
    juce::AbstractFifo auditionFifo { auditionCapacity };
    std::array<juce::MidiMessage, auditionCapacity> auditionRing;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Track)
};
