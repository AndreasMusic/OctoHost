#pragma once
#include <cstdint>
#include <utility>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>
#include "ChannelStrip.h"

class AudioEngine : public juce::AudioIODeviceCallback,
                    public juce::MidiInputCallback,
                    public juce::AudioPlayHead,
                    public juce::ChangeListener
{
public:
    static constexpr int maxTracks = 24;
    static constexpr int maxDirectHwChannels = 32;

    AudioEngine()
    {
       #if JUCE_MAJOR_VERSION >= 9
        juce::addDefaultFormatsToManager(formatManager);
       #else
        formatManager.addDefaultFormats();
       #endif

        loadSavedPluginList();
        knownPluginList.addChangeListener(this);
        writerThread.startThread();

        // Start with the iconic 8-track Guitar/Bass/Drum template (fully dynamic!)
        for (int i = 0; i < 8; ++i)
            tracks.push_back(std::make_shared<Track>(nextTrackId++, i));

        deviceManager.initialiseWithDefaultDevices(8, 8);

        for (const auto& info : juce::MidiInput::getAvailableDevices())
            deviceManager.setMidiInputDeviceEnabled(info.identifier, true);

        deviceManager.addMidiInputDeviceCallback({}, this);
        deviceManager.addAudioCallback(this);
    }

    ~AudioEngine() override
    {
        stop(false);
        deviceManager.removeAudioCallback(this);
        deviceManager.removeMidiInputDeviceCallback({}, this);
        knownPluginList.removeChangeListener(this);
        savePluginList();

        masterInserts.clearAll();
        fxBusA.inserts.clearAll();
        fxBusB.inserts.clearAll();

        std::vector<std::shared_ptr<Track>> toDestroy;
        {
            const juce::SpinLock::ScopedLockType sl(trackListLock);
            toDestroy = std::move(tracks);
        }
        toDestroy.clear();

        writerThread.stopThread(2000);
    }

    // --- Dynamic Track Management (Thread-Safe, Message Thread Only) ---
    int getNumTracks() const
    {
        const juce::SpinLock::ScopedLockType sl(trackListLock);
        return static_cast<int>(tracks.size());
    }

    std::shared_ptr<Track> getTrack(int index) const
    {
        const juce::SpinLock::ScopedLockType sl(trackListLock);
        if (index < 0 || index >= static_cast<int>(tracks.size()))
            return nullptr;
        return tracks[static_cast<size_t>(index)];
    }

    int addTrack(TrackMode trackMode,
                 const juce::String& customName = {},
                 juce::Colour col = {},
                 float initialPan = 0.0f,
                 int initialHpf = 0,
                 PolyFallbackSynth::Patch synthPatch = PolyFallbackSynth::Patch::AnalogSub)
    {
        if (getNumTracks() >= maxTracks)
            return -1;

        const int id = nextTrackId++;
        auto t = std::make_shared<Track>(id, -1);
        t->mode.store(trackMode);
        if (customName.isNotEmpty())
            t->name = customName;
        else
            t->name = (trackMode == TrackMode::Midi ? "MIDI " : "AUD ") + juce::String(id);

        if (!col.isTransparent())
            t->trackColour = col;

        t->pan.store(initialPan);
        t->hpfMode.store(initialHpf);

        if (trackMode == TrackMode::Midi)
        {
            t->inputSourceType.store(static_cast<int>(InputSourceType::MidiAll));
            t->useFallbackPolySynth.store(true);
            t->fallbackSynthPatch.store(static_cast<int>(synthPatch));
        }
        else
        {
            t->inputSourceType.store(static_cast<int>(InputSourceType::MonoHW));
            t->inputChannel.store(0);
        }

        // Prepare BEFORE publishing to audio thread (zero allocation on audio thread!)
        t->prepare(currentSampleRate.load(), currentBlockSize.load());

        int newIdx = -1;
        {
            const juce::SpinLock::ScopedLockType sl(trackListLock);
            newIdx = static_cast<int>(tracks.size());
            tracks.push_back(std::move(t));
        }
        notifyTrackLayoutChanged();
        return newIdx;
    }

    void addGuitarDoublePair()
    {
        const int pairNum = std::max(2, (getNumTracks() / 2));
        addTrack(TrackMode::Audio, "GTR L" + juce::String(pairNum), juce::Colour(0xff3498db), -0.80f, 2);
        addTrack(TrackMode::Audio, "GTR R" + juce::String(pairNum), juce::Colour(0xff2980b9),  0.80f, 2);
    }

    int duplicateTrack(int sourceIndex)
    {
        auto src = getTrack(sourceIndex);
        if (src == nullptr || getNumTracks() >= maxTracks)
            return -1;

        const int id = nextTrackId++;
        auto dst = std::make_shared<Track>(id, -1);
        dst->name = src->name.substring(0, 10) + " 2";
        dst->trackColour = src->trackColour;
        dst->mode.store(src->mode.load());
        dst->inputSourceType.store(src->inputSourceType.load());
        dst->inputChannel.store(src->inputChannel.load());
        dst->outputDestType.store(src->outputDestType.load());
        dst->outputHwChannel.store(src->outputHwChannel.load());
        dst->inputTrimDb.store(src->inputTrimDb.load());
        dst->phaseInvert.store(src->phaseInvert.load());
        dst->hpfMode.store(src->hpfMode.load());
        dst->gateMode.store(src->gateMode.load());
        dst->driveMode.store(src->driveMode.load());
        dst->useInternalDrumSynth.store(src->useInternalDrumSynth.load());
        dst->useFallbackPolySynth.store(src->useFallbackPolySynth.load());
        dst->fallbackSynthPatch.store(src->fallbackSynthPatch.load());
        dst->sendAGain.store(src->sendAGain.load());
        dst->sendAPreFader.store(src->sendAPreFader.load());
        dst->sendBGain.store(src->sendBGain.load());
        dst->sendBPreFader.store(src->sendBPreFader.load());
        dst->gain.store(src->gain.load());
        dst->pan.store(src->pan.load());

        dst->prepare(currentSampleRate.load(), currentBlockSize.load());
        dst->copyClipsFrom(*src);

        if (auto insXml = src->inserts.toXml())
            dst->inserts.restoreFromXml(*insXml, formatManager, currentSampleRate.load(), currentBlockSize.load(), this);

        int newIdx = -1;
        {
            const juce::SpinLock::ScopedLockType sl(trackListLock);
            const size_t insertPos = std::min(tracks.size(), static_cast<size_t>(sourceIndex + 1));
            newIdx = static_cast<int>(insertPos);
            tracks.insert(tracks.begin() + static_cast<std::ptrdiff_t>(insertPos), std::move(dst));
        }
        notifyTrackLayoutChanged();
        return newIdx;
    }

    bool removeTrack(int index)
    {
        std::shared_ptr<Track> removed;
        {
            const juce::SpinLock::ScopedLockType sl(trackListLock);
            if (tracks.size() <= 1 || index < 0 || index >= static_cast<int>(tracks.size()))
                return false;
            removed = std::move(tracks[static_cast<size_t>(index)]);
            tracks.erase(tracks.begin() + index);
        }

        // Destroy removed track safely outside lock (flushes recording & closes plugin windows)
        if (removed != nullptr)
        {
            removed->stopAudioRecording();
            removed->inserts.clearAll();
            removed.reset();
        }
        notifyTrackLayoutChanged();
        return true;
    }

    // --- Copy / Paste 4-Slot Insert Chain ---
    void copyInsertChain(const InsertChain& src)
    {
        copiedInsertChainXml = src.toXml();
    }

    bool hasCopiedInsertChain() const { return copiedInsertChainXml != nullptr; }

    void pasteInsertChain(InsertChain& dst)
    {
        if (copiedInsertChainXml != nullptr)
            dst.restoreFromXml(*copiedInsertChainXml, formatManager,
                               currentSampleRate.load(), currentBlockSize.load(), this);
    }

    // --- Hardware Channel Queries ---
    juce::StringArray getHardwareInputNames() const
    {
        if (auto* dev = deviceManager.getCurrentAudioDevice())
        {
            auto names = dev->getInputChannelNames();
            if (!names.isEmpty()) return names;
        }
        return { "In 1", "In 2", "In 3", "In 4", "In 5", "In 6", "In 7", "In 8" };
    }

    juce::StringArray getHardwareOutputNames() const
    {
        if (auto* dev = deviceManager.getCurrentAudioDevice())
        {
            auto names = dev->getOutputChannelNames();
            if (!names.isEmpty()) return names;
        }
        return { "Out 1", "Out 2", "Out 3", "Out 4", "Out 5", "Out 6", "Out 7", "Out 8" };
    }

    // --- Transport, Loop, BPM & Count-In ---
    void setBpm(double newBpm)
    {
        newBpm = juce::jlimit(40.0, 240.0, newBpm);
        bpm.store(newBpm);
        const double sr = currentSampleRate.load();
        for (int i = 0; i < getNumTracks(); ++i)
            if (auto t = getTrack(i))
                t->resyncMidiToBpm(newBpm, sr);
    }

    void play()
    {
        countInRemainingSamples.store(0);
        isPlaying.store(true);
    }

    void pause()
    {
        if (isRecording.load() || countInRemainingSamples.load() > 0)
            stopRecordingInternal();
        isPlaying.store(false);
        pendingAllNotesOff.store(true);
    }

    void stop(bool rewindToLoopOrZero)
    {
        if (isRecording.load() || countInRemainingSamples.load() > 0)
            stopRecordingInternal();

        isPlaying.store(false);
        pendingAllNotesOff.store(true);

        if (rewindToLoopOrZero)
            playheadSample.store(loopEnabled.load() ? loopStartSample.load() : 0);
    }

    void toggleRecord()
    {
        if (isRecording.load() || countInRemainingSamples.load() > 0)
        {
            stopRecordingInternal();
            return;
        }

        const int64_t punchPos = playheadSample.load();
        const double sr = currentSampleRate.load();
        for (int i = 0; i < getNumTracks(); ++i)
            if (auto t = getTrack(i))
                t->startAudioRecording(writerThread, sr, punchPos);

        if (countInEnabled.load() && !isPlaying.load())
        {
            const double samplesPerBeat = (sr * 60.0) / std::max(30.0, bpm.load());
            countInTotalSamples.store(static_cast<int64_t>(std::llround(samplesPerBeat * 4.0)));
            countInRemainingSamples.store(countInTotalSamples.load());
            lastCountInBeat.store(-1);
            pendingRecordAfterCountIn.store(true);
            isPlaying.store(false);
        }
        else
        {
            isRecording.store(true);
            isPlaying.store(true);
        }
    }

    void setPlayheadSeconds(double seconds)
    {
        const int64_t target = static_cast<int64_t>(std::max(0.0, seconds) * currentSampleRate.load());
        playheadSample.store(target);
        pendingAllNotesOff.store(true);
    }

    double getPlayheadSeconds() const
    {
        const double sr = currentSampleRate.load();
        return sr > 0.0 ? static_cast<double>(playheadSample.load()) / sr : 0.0;
    }

    double getSamplesPerBar() const
    {
        return (currentSampleRate.load() * 60.0 / std::max(30.0, bpm.load())) * 4.0;
    }

    void setLoopBars(int startBarZeroBased, int lengthBars)
    {
        const double spb = getSamplesPerBar();
        const int64_t s = static_cast<int64_t>(std::max(0, startBarZeroBased) * spb);
        const int64_t e = static_cast<int64_t>((std::max(0, startBarZeroBased) + std::max(1, lengthBars)) * spb);
        loopStartSample.store(s);
        loopEndSample.store(e);
    }

    void drainAllTrackMidi()
    {
        const double curBpm = bpm.load();
        const double sr = currentSampleRate.load();
        for (int i = 0; i < getNumTracks(); ++i)
            if (auto t = getTrack(i))
                t->drainRecordedMidi(curBpm, sr);
    }

    // --- Async VST3 / AU Plugin Loading ---
    void loadPluginIntoChain(InsertChain* chain,
                             int slotIdx,
                             const juce::PluginDescription& desc,
                             std::function<void(const juce::String& error)> onComplete = nullptr)
    {
        if (chain == nullptr) return;
        const double sr = currentSampleRate.load();
        const int bs = currentBlockSize.load();

        formatManager.createPluginInstanceAsync(
            desc, sr, bs,
            [this, chain, slotIdx, desc, sr, bs, cb = std::move(onComplete)](
                std::unique_ptr<juce::AudioPluginInstance> instance,
                const juce::String& error)
            {
                if (instance != nullptr)
                    chain->setSlot(slotIdx, std::move(instance), desc, sr, bs, this);
                if (cb) cb(error);
            });
    }

    void commitDrumPatternToTrack(int trackIndex, int numBars)
    {
        auto t = getTrack(trackIndex);
        if (t == nullptr) return;

        std::vector<std::pair<int64_t, juce::MidiMessage>> events;
        drumEngine.exportPatternMidi(events, numBars, bpm.load(), currentSampleRate.load());
        t->mode.store(TrackMode::Midi);
        t->replaceMidiClip(events, true, bpm.load(), currentSampleRate.load());
        drumEngine.sequencerEnabled.store(false);
    }

    // --- Offline 24-bit Stereo WAV Mixdown & Full Multi-Track Stem Export ---
    bool exportMixdownWav(const juce::File& targetFile, int soloOnlyTrackIndex = -1)
    {
        stop(false);
        const double sr = currentSampleRate.load();
        int64_t totalSamples = 0;

        for (int i = 0; i < getNumTracks(); ++i)
            if (auto t = getTrack(i))
                totalSamples = std::max(totalSamples, t->getClipLengthSamples());

        if (loopEnabled.load() && loopEndSample.load() > loopStartSample.load())
            totalSamples = std::max(totalSamples, loopEndSample.load());

        if (totalSamples <= 0)
            totalSamples = static_cast<int64_t>(getSamplesPerBar() * 4.0);

        totalSamples += static_cast<int64_t>(sr * 2.0); // 2s FX tail

        targetFile.deleteFile();
        juce::WavAudioFormat wavFormat;
        auto stream = targetFile.createOutputStream();
        if (stream == nullptr) return false;

       #if JUCE_MAJOR_VERSION >= 9
        auto options = juce::AudioFormatWriterOptions{}
                           .withSampleRate(sr)
                           .withNumChannels(2)
                           .withBitsPerSample(24);
        auto writer = wavFormat.createWriterFor(stream, options);
       #else
        std::unique_ptr<juce::AudioFormatWriter> writer(
            wavFormat.createWriterFor(stream.release(), sr, 2, 24, {}, 0));
       #endif
        if (writer == nullptr) return false;

        // Save solo states if rendering a single stem
        std::vector<bool> savedSolos;
        if (soloOnlyTrackIndex >= 0)
        {
            for (int i = 0; i < getNumTracks(); ++i)
            {
                auto t = getTrack(i);
                savedSolos.push_back(t->soloed.load());
                t->soloed.store(i == soloOnlyTrackIndex);
            }
        }

        offlineRendering.store(true);
        juce::Thread::sleep(35);

        constexpr int blockSize = 512;
        juce::AudioBuffer<float> bounceOut(2, blockSize);
        const int64_t savedPos = playheadSample.load();
        const bool savedMetronome = metronomeEnabled.load();
        const bool savedDim = masterDim.load();
        metronomeEnabled.store(false);
        masterDim.store(false);

        int64_t pos = 0;
        while (pos < totalSamples)
        {
            const int n = static_cast<int>(std::min<int64_t>(blockSize, totalSamples - pos));
            bounceOut.clear();
            float* outPtrs[2] = { bounceOut.getWritePointer(0), bounceOut.getWritePointer(1) };
            renderBlock(nullptr, 0, outPtrs, 2, n, pos, true, false, false);
            writer->writeFromAudioSampleBuffer(bounceOut, 0, n);
            pos += n;
        }

        metronomeEnabled.store(savedMetronome);
        masterDim.store(savedDim);
        playheadSample.store(savedPos);

        if (soloOnlyTrackIndex >= 0)
        {
            for (int i = 0; i < static_cast<int>(savedSolos.size()); ++i)
                if (auto t = getTrack(i))
                    t->soloed.store(savedSolos[static_cast<size_t>(i)]);
        }

        offlineRendering.store(false);
        return true;
    }

    int exportAllStems(const juce::File& folder)
    {
        folder.createDirectory();
        int exported = 0;
        for (int i = 0; i < getNumTracks(); ++i)
        {
            auto t = getTrack(i);
            if (t == nullptr) continue;
            const bool isDrumSeqTrack = (static_cast<InputSourceType>(t->inputSourceType.load()) == InputSourceType::DrumSeq
                                         && drumEngine.sequencerEnabled.load());
            if (t->getClipLengthSamples() <= 0 && !isDrumSeqTrack)
                continue;

            const juce::String safeName = juce::File::createLegalFileName(
                juce::String(i + 1).paddedLeft('0', 2) + "_" + t->name + ".wav");
            if (exportMixdownWav(folder.getChildFile(safeName), i))
                ++exported;
        }
        return exported;
    }

    // --- Full Session Save / Load (.octo XML + stems folder) ---
    bool saveSession(const juce::File& file)
    {
        stop(false);
        auto audioDir = file.getSiblingFile(file.getFileNameWithoutExtension() + "_audio");
        audioDir.createDirectory();

        auto root = std::make_unique<juce::XmlElement>("OctoHostSession");
        root->setAttribute("version", 3);
        root->setAttribute("bpm", bpm.load());
        root->setAttribute("masterGain", static_cast<double>(masterGain.load()));
        root->setAttribute("monoCheck", masterMonoCheck.load());
        root->setAttribute("limiter", masterLimiterEnabled.load());
        root->setAttribute("loopEnabled", loopEnabled.load());
        root->setAttribute("loopStart", juce::String(loopStartSample.load()));
        root->setAttribute("loopEnd", juce::String(loopEndSample.load()));

        root->addChildElement(drumEngine.toXml().release());
        root->addChildElement(fxBusA.toXml().release());
        root->addChildElement(fxBusB.toXml().release());

        auto* masterEl = root->createNewChildElement("Master");
        masterEl->addChildElement(masterInserts.toXml().release());

        const int count = getNumTracks();
        for (int i = 0; i < count; ++i)
            if (auto t = getTrack(i))
                root->addChildElement(t->toXml(i, audioDir, currentSampleRate.load()).release());

        return root->writeTo(file);
    }

    bool loadSession(const juce::File& file)
    {
        stop(true);
        auto root = juce::XmlDocument::parse(file);
        if (root == nullptr || !root->hasTagName("OctoHostSession"))
            return false;

        auto audioDir = file.getSiblingFile(file.getFileNameWithoutExtension() + "_audio");
        const double sr = currentSampleRate.load();
        const int bs = currentBlockSize.load();
        const double loadedBpm = root->getDoubleAttribute("bpm", 120.0);

        bpm.store(loadedBpm);
        masterGain.store(static_cast<float>(root->getDoubleAttribute("masterGain", 0.85)));
        masterMonoCheck.store(root->getBoolAttribute("monoCheck", false));
        masterLimiterEnabled.store(root->getBoolAttribute("limiter", true));
        loopEnabled.store(root->getBoolAttribute("loopEnabled", true));
        loopStartSample.store(root->getStringAttribute("loopStart").getLargeIntValue());
        loopEndSample.store(std::max<int64_t>(static_cast<int64_t>(getSamplesPerBar()),
                                              root->getStringAttribute("loopEnd").getLargeIntValue()));

        if (auto* dXml = root->getChildByName("DrumEngine"))
            drumEngine.fromXml(*dXml);

        for (auto* bXml : root->getChildWithTagNameIterator("FXBus"))
        {
            const int idx = bXml->getIntAttribute("index", -1);
            if (idx == 0) fxBusA.fromXml(*bXml, formatManager, sr, bs, this);
            if (idx == 1) fxBusB.fromXml(*bXml, formatManager, sr, bs, this);
        }

        if (auto* mXml = root->getChildByName("Master"))
            if (auto* insXml = mXml->getChildByName("InsertChain"))
                masterInserts.restoreFromXml(*insXml, formatManager, sr, bs, this);

        std::vector<std::shared_ptr<Track>> loadedTracks;
        for (auto* tXml : root->getChildWithTagNameIterator("Track"))
        {
            auto t = std::make_shared<Track>(nextTrackId++, -1);
            t->prepare(sr, bs);
            t->fromXml(*tXml, audioDir, formatManager, loadedBpm, sr, bs, this);
            loadedTracks.push_back(std::move(t));
        }

        if (!loadedTracks.empty())
        {
            std::vector<std::shared_ptr<Track>> oldTracks;
            {
                const juce::SpinLock::ScopedLockType sl(trackListLock);
                oldTracks = std::move(tracks);
                tracks = std::move(loadedTracks);
            }
            oldTracks.clear();
            notifyTrackLayoutChanged();
        }
        return true;
    }

    juce::File getPluginDataDirectory() const
    {
        auto base = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
       #if JUCE_MAC
        auto dir = base.getChildFile("Application Support").getChildFile("OctoHost");
       #else
        auto dir = base.getChildFile("OctoHost");
       #endif
        dir.createDirectory();
        return dir;
    }

    // --- AudioIODeviceCallback ---
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        const double sr = device->getCurrentSampleRate();
        const int bs = device->getCurrentBufferSizeSamples();
        currentSampleRate.store(sr);
        currentBlockSize.store(bs);

        const int allocBlock = std::max(bs, 4096);
        masterBuffer.setSize(2, allocBlock, false, true, true);
        directHwOutBuffer.setSize(maxDirectHwChannels, allocBlock, false, true, true);
        midiCollector.reset(sr);
        drumEngine.prepare(sr);
        tuner.prepare(sr);
        metronome.prepare(sr);
        fxBusA.prepare(sr, bs);
        fxBusB.prepare(sr, bs);
        masterInserts.prepare(sr, bs);

        const juce::SpinLock::ScopedLockType sl(trackListLock);
        for (auto& t : tracks)
            t->prepare(sr, bs);

        if (loopEndSample.load() <= loopStartSample.load())
            setLoopBars(0, 4);
    }

    void audioDeviceStopped() override {}

    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
        juce::ScopedNoDenormals noDenormals;

        if (offlineRendering.load(std::memory_order_relaxed))
        {
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (outputChannelData[ch] != nullptr)
                    juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
            return;
        }

        const int n = std::min(numSamples, masterBuffer.getNumSamples());
        bool playing = isPlaying.load(std::memory_order_relaxed);
        bool recording = isRecording.load(std::memory_order_relaxed);
        int64_t pos = playheadSample.load(std::memory_order_relaxed);

        // 1-Bar Count-In countdown
        int64_t ciRem = countInRemainingSamples.load(std::memory_order_relaxed);
        bool inCountIn = false;
        if (ciRem > 0)
        {
            inCountIn = true;
            const int64_t total = countInTotalSamples.load(std::memory_order_relaxed);
            const int64_t elapsed = total - ciRem;
            const double spb = (currentSampleRate.load(std::memory_order_relaxed) * 60.0)
                               / std::max(30.0, bpm.load(std::memory_order_relaxed));
            const int beatNow = static_cast<int>(elapsed / spb);
            if (beatNow != lastCountInBeat.load(std::memory_order_relaxed))
            {
                lastCountInBeat.store(beatNow, std::memory_order_relaxed);
                metronome.trigger(beatNow == 0);
            }

            ciRem -= n;
            if (ciRem <= 0)
            {
                countInRemainingSamples.store(0, std::memory_order_relaxed);
                if (pendingRecordAfterCountIn.exchange(false, std::memory_order_relaxed))
                {
                    isRecording.store(true, std::memory_order_relaxed);
                    isPlaying.store(true, std::memory_order_relaxed);
                    recording = true;
                    playing = true;
                    inCountIn = false;
                }
            }
            else
            {
                countInRemainingSamples.store(ciRem, std::memory_order_relaxed);
            }
        }

        renderBlock(inputChannelData, numInputChannels, outputChannelData, numOutputChannels,
                    n, pos, playing, recording, inCountIn);

        if (playing)
        {
            int64_t nextPos = pos + n;
            if (loopEnabled.load(std::memory_order_relaxed))
            {
                const int64_t lStart = loopStartSample.load(std::memory_order_relaxed);
                const int64_t lEnd = loopEndSample.load(std::memory_order_relaxed);
                if (lEnd > lStart && nextPos >= lEnd)
                {
                    nextPos = lStart + (nextPos - lEnd);
                    pendingAllNotesOff.store(true, std::memory_order_relaxed);
                }
            }
            playheadSample.store(nextPos, std::memory_order_relaxed);
        }
    }

    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message) override
    {
        midiCollector.addMessageToQueue(message);
    }

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        const double sr = currentSampleRate.load(std::memory_order_relaxed);
        const int64_t samples = playheadSample.load(std::memory_order_relaxed);
        const double currentBpm = bpm.load(std::memory_order_relaxed);
        const double seconds = sr > 0.0 ? static_cast<double>(samples) / sr : 0.0;

        info.setBpm(currentBpm);
        info.setTimeSignature(TimeSignature { 4, 4 });
        info.setIsPlaying(isPlaying.load(std::memory_order_relaxed));
        info.setIsRecording(isRecording.load(std::memory_order_relaxed));
        info.setTimeInSamples(samples);
        info.setTimeInSeconds(seconds);
        info.setPpqPosition(seconds * (currentBpm / 60.0));
        return info;
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override { savePluginList(); }

    // Callback fired on message thread whenever tracks are added/removed/loaded
    std::function<void()> onTrackLayoutChanged;

    // --- Public Members ---
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList knownPluginList;
    juce::TimeSliceThread writerThread { "OctoHost Audio Writer" };
    juce::AudioDeviceManager deviceManager;

    FXBus fxBusA { "FX A", 0 };
    FXBus fxBusB { "FX B", 1 };
    InsertChain masterInserts;
    DrumEngine drumEngine;
    ChromaticTuner tuner;

    std::atomic<bool> isPlaying { false };
    std::atomic<bool> isRecording { false };
    std::atomic<int64_t> playheadSample { 0 };
    std::atomic<double> bpm { 120.0 };

    std::atomic<bool> loopEnabled { true };
    std::atomic<int64_t> loopStartSample { 0 };
    std::atomic<int64_t> loopEndSample { 0 };

    std::atomic<bool> metronomeEnabled { false };
    std::atomic<float> metronomeVolume { 0.65f };
    std::atomic<bool> countInEnabled { true };
    std::atomic<int64_t> countInRemainingSamples { 0 };

    std::atomic<float> masterGain { 0.85f };
    std::atomic<bool> masterMonoCheck { false };
    std::atomic<bool> masterLimiterEnabled { true };
    std::atomic<bool> masterDim { false };
    std::atomic<float> masterPeakL { 0.0f };
    std::atomic<float> masterPeakR { 0.0f };
    std::atomic<float> masterRms { 0.0f };
    std::atomic<bool> masterClipped { false };

    std::atomic<double> currentSampleRate { 44100.0 };
    std::atomic<int> currentBlockSize { 512 };

private:
    void notifyTrackLayoutChanged()
    {
        if (onTrackLayoutChanged)
            onTrackLayoutChanged();
    }

    void renderBlock(const float* const* inputChannelData,
                     int numInputChannels,
                     float* const* outputChannelData,
                     int numOutputChannels,
                     int n,
                     int64_t pos,
                     bool playing,
                     bool recording,
                     bool inCountIn) noexcept
    {
        masterBuffer.clear(0, n);
        directHwOutBuffer.clear(0, n);
        fxBusA.clearInput(n);
        fxBusB.clearInput(n);

        liveMidiBuffer.clear();
        if (!offlineRendering.load(std::memory_order_relaxed))
            midiCollector.removeNextBlockOfMessages(liveMidiBuffer, n);

        drumSeqMidiBuffer.clear();
        drumEngine.generateMidiForBlock(drumSeqMidiBuffer, n, pos, bpm.load(std::memory_order_relaxed), playing);

        const bool allNotesOff = pendingAllNotesOff.exchange(false, std::memory_order_relaxed);

        {
            const juce::SpinLock::ScopedTryLockType sl(trackListLock);
            if (sl.isLocked())
            {
                bool anyTrackSolo = false;
                int tunerTrackIdx = -1;
                int firstAudioIdx = -1;

                for (size_t i = 0; i < tracks.size(); ++i)
                {
                    const auto& t = *tracks[i];
                    if (t.soloed.load(std::memory_order_relaxed))
                        anyTrackSolo = true;
                    if (t.mode.load(std::memory_order_relaxed) == TrackMode::Audio)
                    {
                        if (firstAudioIdx < 0) firstAudioIdx = static_cast<int>(i);
                        if (tunerTrackIdx < 0 && t.armed.load(std::memory_order_relaxed))
                            tunerTrackIdx = static_cast<int>(i);
                    }
                }
                if (tunerTrackIdx < 0)
                    tunerTrackIdx = firstAudioIdx;

                for (size_t i = 0; i < tracks.size(); ++i)
                {
                    tracks[i]->process(
                        inputChannelData, numInputChannels,
                        masterBuffer, fxBusA, fxBusB, directHwOutBuffer,
                        liveMidiBuffer, drumSeqMidiBuffer, drumEngine, tuner,
                        (static_cast<int>(i) == tunerTrackIdx),
                        n, pos, playing, recording, anyTrackSolo, allNotesOff);
                }
            }
        }

        const bool anyBusSolo = fxBusA.soloed.load(std::memory_order_relaxed)
                                || fxBusB.soloed.load(std::memory_order_relaxed);
        fxBusA.process(masterBuffer, directHwOutBuffer, n, anyBusSolo);
        fxBusB.process(masterBuffer, directHwOutBuffer, n, anyBusSolo);

        masterDummyMidi.clear();
        masterInserts.process(masterBuffer, masterDummyMidi, n);

        const float mg = masterGain.load(std::memory_order_relaxed)
                         * (masterDim.load(std::memory_order_relaxed) ? 0.251f : 1.0f); // -12 dB Dim
        const bool mono = masterMonoCheck.load(std::memory_order_relaxed);
        const bool lim = masterLimiterEnabled.load(std::memory_order_relaxed);
        const bool clickOn = metronomeEnabled.load(std::memory_order_relaxed) || inCountIn;
        const float clickVol = metronomeVolume.load(std::memory_order_relaxed);
        const double spb = (currentSampleRate.load(std::memory_order_relaxed) * 60.0)
                           / std::max(30.0, bpm.load(std::memory_order_relaxed));

        float* mL = masterBuffer.getWritePointer(0);
        float* mR = masterBuffer.getWritePointer(1);
        float pkL = 0.0f, pkR = 0.0f;
        double sumSq = 0.0;

        for (int i = 0; i < n; ++i)
        {
            if (playing && metronomeEnabled.load(std::memory_order_relaxed))
            {
                const int64_t sampleIdx = pos + i;
                const int64_t curBeat = static_cast<int64_t>(std::floor(sampleIdx / spb));
                const int64_t prevBeat = sampleIdx > 0 ? static_cast<int64_t>(std::floor((sampleIdx - 1) / spb)) : -1;
                if (curBeat != prevBeat)
                    metronome.trigger((curBeat % 4) == 0);
            }

            float l = mL[i] * mg;
            float r = mR[i] * mg;

            if (mono)
            {
                const float mid = 0.5f * (l + r);
                l = r = mid;
            }
            if (lim)
            {
                l = softLimitSample(l);
                r = softLimitSample(r);
            }
            if (clickOn)
            {
                const float c = metronome.nextSample(clickVol);
                l += c;
                r += c;
            }

            mL[i] = l;
            mR[i] = r;
            pkL = std::max(pkL, std::abs(l));
            pkR = std::max(pkR, std::abs(r));
            sumSq += 0.5 * (static_cast<double>(l) * l + static_cast<double>(r) * r);
        }

        masterPeakL.store(pkL, std::memory_order_relaxed);
        masterPeakR.store(pkR, std::memory_order_relaxed);
        masterRms.store(static_cast<float>(std::sqrt(sumSq / std::max(1, n))), std::memory_order_relaxed);
        if (pkL >= 0.99f || pkR >= 0.99f)
            masterClipped.store(true, std::memory_order_relaxed);

        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (outputChannelData[ch] == nullptr) continue;

            if (ch < maxDirectHwChannels)
                juce::FloatVectorOperations::copy(outputChannelData[ch], directHwOutBuffer.getReadPointer(ch), n);
            else
                juce::FloatVectorOperations::clear(outputChannelData[ch], n);

            if (ch < 2)
                juce::FloatVectorOperations::add(outputChannelData[ch], masterBuffer.getReadPointer(ch), n);
        }
    }

    void stopRecordingInternal()
    {
        countInRemainingSamples.store(0);
        pendingRecordAfterCountIn.store(false);
        isRecording.store(false);
        const double curBpm = bpm.load();
        const double sr = currentSampleRate.load();
        for (int i = 0; i < getNumTracks(); ++i)
        {
            if (auto t = getTrack(i))
            {
                t->stopAudioRecording();
                t->drainRecordedMidi(curBpm, sr);
            }
        }
    }

    void savePluginList()
    {
        if (auto xml = knownPluginList.createXml())
            xml->writeTo(getPluginDataDirectory().getChildFile("plugins.xml"));
    }

    void loadSavedPluginList()
    {
        auto file = getPluginDataDirectory().getChildFile("plugins.xml");
        if (auto xml = juce::XmlDocument::parse(file))
            knownPluginList.recreateFromXml(*xml);
    }

    mutable juce::SpinLock trackListLock;
    std::vector<std::shared_ptr<Track>> tracks;
    int nextTrackId = 1;
    std::unique_ptr<juce::XmlElement> copiedInsertChainXml;

    juce::MidiMessageCollector midiCollector;
    juce::AudioBuffer<float> masterBuffer;
    juce::AudioBuffer<float> directHwOutBuffer;
    juce::MidiBuffer liveMidiBuffer;
    juce::MidiBuffer drumSeqMidiBuffer;
    juce::MidiBuffer masterDummyMidi;
    MetronomeClick metronome;

    std::atomic<bool> pendingAllNotesOff { false };
    std::atomic<bool> offlineRendering { false };
    std::atomic<bool> pendingRecordAfterCountIn { false };
    std::atomic<int64_t> countInTotalSamples { 0 };
    std::atomic<int> lastCountInBeat { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
