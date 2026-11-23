#pragma once
#include <utility>
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include "PluginWindow.h"

// Reusable 4-slot VST3 / AU Insert Rack (used by all 8 Tracks, FX Bus A, FX Bus B, and Master)
class InsertChain
{
public:
    static constexpr int numSlots = 4;

    InsertChain() = default;
    ~InsertChain() { clearAll(); }

    void prepare(double sampleRate, int maxBlockSize)
    {
        currentSampleRate = sampleRate;
        currentBlockSize = maxBlockSize;
        const int allocBlock = std::max(maxBlockSize, 4096);

        const juce::SpinLock::ScopedLockType sl(lock);
        for (auto& s : slots)
        {
            if (s.plugin != nullptr)
            {
                const int ch = std::max({ 2,
                                          s.plugin->getTotalNumInputChannels(),
                                          s.plugin->getTotalNumOutputChannels() });
                s.workBuffer.setSize(ch, allocBlock, false, true, true);
                s.plugin->prepareToPlay(sampleRate, maxBlockSize);
            }
        }
    }

    void setSlot(int slotIdx,
                 std::unique_ptr<juce::AudioPluginInstance> newPlugin,
                 const juce::PluginDescription& desc,
                 double sampleRate,
                 int blockSize,
                 juce::AudioPlayHead* playHead)
    {
        if (slotIdx < 0 || slotIdx >= numSlots)
            return;

        closeWindow(slotIdx);

        const int allocBlock = std::max(blockSize, 4096);
        juce::AudioBuffer<float> newWork;
        juce::String newName = "---";
        bool isSynth = false;

        if (newPlugin != nullptr)
        {
            newPlugin->setPlayHead(playHead);
            newPlugin->enableAllBuses();
            newPlugin->prepareToPlay(sampleRate, blockSize);
            newName = newPlugin->getName();
            isSynth = desc.isInstrument || newPlugin->acceptsMidi();

            const int ch = std::max({ 2,
                                      newPlugin->getTotalNumInputChannels(),
                                      newPlugin->getTotalNumOutputChannels() });
            newWork.setSize(ch, allocBlock, false, true, true);
        }

        std::unique_ptr<juce::AudioPluginInstance> oldPlugin;
        {
            const juce::SpinLock::ScopedLockType sl(lock);
            auto& s = slots[static_cast<size_t>(slotIdx)];
            oldPlugin = std::move(s.plugin);
            s.plugin = std::move(newPlugin);
            s.description = desc;
            s.workBuffer = std::move(newWork);
            s.name = newName;
            s.bypassed.store(false);
            s.isInstrument.store(isSynth);
        }
    }

    void clearSlot(int slotIdx)
    {
        setSlot(slotIdx, nullptr, {}, currentSampleRate, currentBlockSize, nullptr);
    }

    void clearAll()
    {
        for (int i = 0; i < numSlots; ++i)
            clearSlot(i);
    }

    void swapSlots(int a, int b)
    {
        if (a < 0 || a >= numSlots || b < 0 || b >= numSlots || a == b)
            return;

        closeWindow(a);
        closeWindow(b);

        const juce::SpinLock::ScopedLockType sl(lock);
        auto& sa = slots[static_cast<size_t>(a)];
        auto& sb = slots[static_cast<size_t>(b)];
        std::swap(sa.plugin, sb.plugin);
        std::swap(sa.description, sb.description);
        std::swap(sa.workBuffer, sb.workBuffer);
        std::swap(sa.name, sb.name);
        const bool ba = sa.bypassed.load();
        sa.bypassed.store(sb.bypassed.load());
        sb.bypassed.store(ba);
        const bool ia = sa.isInstrument.load();
        sa.isInstrument.store(sb.isInstrument.load());
        sb.isInstrument.store(ia);
    }

    void setBypassed(int slotIdx, bool shouldBypass)
    {
        if (slotIdx >= 0 && slotIdx < numSlots)
            slots[static_cast<size_t>(slotIdx)].bypassed.store(shouldBypass);
    }

    bool isBypassed(int slotIdx) const
    {
        return (slotIdx >= 0 && slotIdx < numSlots)
                   ? slots[static_cast<size_t>(slotIdx)].bypassed.load()
                   : false;
    }

    bool hasPlugin(int slotIdx) const
    {
        if (slotIdx < 0 || slotIdx >= numSlots)
            return false;
        const juce::SpinLock::ScopedLockType sl(lock);
        return slots[static_cast<size_t>(slotIdx)].plugin != nullptr;
    }

    bool hasAnyInstrument() const noexcept
    {
        for (const auto& s : slots)
            if (s.isInstrument.load(std::memory_order_relaxed) && !s.bypassed.load(std::memory_order_relaxed))
                return true;
        return false;
    }

    juce::String getSlotName(int slotIdx) const
    {
        if (slotIdx < 0 || slotIdx >= numSlots)
            return "---";
        const juce::SpinLock::ScopedLockType sl(lock);
        return slots[static_cast<size_t>(slotIdx)].name;
    }

    void openWindow(int slotIdx, const juce::String& ownerTitle)
    {
        if (slotIdx < 0 || slotIdx >= numSlots)
            return;

        auto& win = windows[static_cast<size_t>(slotIdx)];
        if (win != nullptr)
        {
            win->toFront(true);
            return;
        }

        juce::AudioPluginInstance* raw = nullptr;
        juce::String title;
        {
            const juce::SpinLock::ScopedLockType sl(lock);
            auto& s = slots[static_cast<size_t>(slotIdx)];
            raw = s.plugin.get();
            title = ownerTitle + " [Ins " + juce::String(slotIdx + 1) + "] - " + s.name;
        }

        if (raw != nullptr)
        {
            win = std::make_unique<PluginWindow>(
                *raw, title, [this, slotIdx] { windows[static_cast<size_t>(slotIdx)].reset(); });
        }
    }

    void closeWindow(int slotIdx)
    {
        if (slotIdx >= 0 && slotIdx < numSlots)
            windows[static_cast<size_t>(slotIdx)].reset();
    }

    // Real-Time Audio Thread Processing
    void process(juce::AudioBuffer<float>& stereoBuffer, juce::MidiBuffer& midi, int numSamples) noexcept
    {
        const juce::SpinLock::ScopedTryLockType sl(lock);
        if (!sl.isLocked())
            return;

        for (auto& s : slots)
        {
            if (s.plugin == nullptr || s.bypassed.load(std::memory_order_relaxed))
                continue;
            if (s.workBuffer.getNumSamples() < numSamples)
                continue;

            const int workCh = s.workBuffer.getNumChannels();
            s.workBuffer.clear(0, numSamples);

            const int copyIn = std::min(2, workCh);
            for (int ch = 0; ch < copyIn; ++ch)
                s.workBuffer.copyFrom(ch, 0, stereoBuffer, ch, 0, numSamples);

            juce::AudioBuffer<float> slice(s.workBuffer.getArrayOfWritePointers(), workCh, numSamples);
            s.plugin->processBlock(slice, midi);

            if (s.plugin->getTotalNumOutputChannels() == 1)
            {
                stereoBuffer.copyFrom(0, 0, slice, 0, 0, numSamples);
                stereoBuffer.copyFrom(1, 0, slice, 0, 0, numSamples);
            }
            else if (workCh >= 2)
            {
                stereoBuffer.copyFrom(0, 0, slice, 0, 0, numSamples);
                stereoBuffer.copyFrom(1, 0, slice, 1, 0, numSamples);
            }
        }
    }

    // --- XML Serialization (including Base64 plugin state) ---
    std::unique_ptr<juce::XmlElement> toXml() const
    {
        auto root = std::make_unique<juce::XmlElement>("InsertChain");
        const juce::SpinLock::ScopedLockType sl(lock);

        for (int i = 0; i < numSlots; ++i)
        {
            const auto& s = slots[static_cast<size_t>(i)];
            if (s.plugin == nullptr)
                continue;

            auto* slotEl = root->createNewChildElement("Slot");
            slotEl->setAttribute("index", i);
            slotEl->setAttribute("bypassed", s.bypassed.load());
            if (auto descXml = s.description.createXml())
                slotEl->addChildElement(descXml.release());

            juce::MemoryBlock state;
            s.plugin->getStateInformation(state);
            slotEl->setAttribute("state", state.toBase64Encoding());
        }
        return root;
    }

    void restoreFromXml(const juce::XmlElement& xml,
                        juce::AudioPluginFormatManager& fmtMgr,
                        double sr,
                        int bs,
                        juce::AudioPlayHead* playHead)
    {
        clearAll();
        for (auto* slotEl : xml.getChildWithTagNameIterator("Slot"))
        {
            const int idx = slotEl->getIntAttribute("index", -1);
            const bool bypass = slotEl->getBoolAttribute("bypassed", false);
            const juce::String stateB64 = slotEl->getStringAttribute("state");
            auto* descXml = slotEl->getChildByName("PLUGIN");
            if (idx < 0 || idx >= numSlots || descXml == nullptr)
                continue;

            juce::PluginDescription desc;
            if (!desc.loadFromXml(*descXml))
                continue;

            juce::String err;
            if (auto inst = fmtMgr.createPluginInstance(desc, sr, bs, err))
            {
                if (stateB64.isNotEmpty())
                {
                    juce::MemoryBlock mb;
                    if (mb.fromBase64Encoding(stateB64))
                        inst->setStateInformation(mb.getData(), static_cast<int>(mb.getSize()));
                }
                setSlot(idx, std::move(inst), desc, sr, bs, playHead);
                setBypassed(idx, bypass);
            }
        }
    }

private:
    struct Slot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        juce::PluginDescription description;
        juce::AudioBuffer<float> workBuffer;
        juce::String name { "---" };
        std::atomic<bool> bypassed { false };
        std::atomic<bool> isInstrument { false };
    };

    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    mutable juce::SpinLock lock;
    std::array<Slot, numSlots> slots;
    std::array<std::unique_ptr<PluginWindow>, numSlots> windows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InsertChain)
};
