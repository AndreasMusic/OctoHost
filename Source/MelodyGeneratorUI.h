#pragma once
#include <algorithm>
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "AudioEngine.h"
#include "LookAndFeel.h"

class MelodyGeneratorUI : public juce::Component
{
public:
    explicit MelodyGeneratorUI(AudioEngine& eng) : engine(eng)
    {
        styleBox.addItem("Bassline Pocket (Kick-Locked)", 1);
        styleBox.addItem("Prog / Metal Guitar Riff", 2);
        styleBox.addItem("Lead Melody / Vocal Hook", 3);
        styleBox.addItem("Power Chord Progression", 4);
        styleBox.addItem("Atmospheric Dark Pad Chords", 5);
        styleBox.addItem("Driving 16th Synth Arp", 6);
        styleBox.setSelectedId(1, juce::dontSendNotification);
        styleBox.onChange = [this] {
            // Smart default octave per style
            const int id = styleBox.getSelectedId();
            if (id == 1)      octaveBox.setSelectedId(2, juce::dontSendNotification); // Oct 2 (Bass E2)
            else if (id == 2 || id == 4) octaveBox.setSelectedId(3, juce::dontSendNotification); // Oct 3 (Gtr E3)
            else              octaveBox.setSelectedId(4, juce::dontSendNotification); // Oct 4 (Lead/Pad)
        };
        addAndMakeVisible(styleBox);

        for (int i = 0; i < 12; ++i)
            rootBox.addItem("Key: " + juce::String(MusicTheory::noteNames[static_cast<size_t>(i)]), i + 1);
        rootBox.setSelectedId(5, juce::dontSendNotification); // E standard default
        addAndMakeVisible(rootBox);

        const auto& scales = MusicTheory::getScales();
        for (int i = 0; i < static_cast<int>(scales.size()); ++i)
            scaleBox.addItem(scales[static_cast<size_t>(i)].name, i + 1);
        scaleBox.setSelectedId(1, juce::dontSendNotification);
        addAndMakeVisible(scaleBox);

        for (int o = 1; o <= 5; ++o)
            octaveBox.addItem("Oct " + juce::String(o), o);
        octaveBox.setSelectedId(2, juce::dontSendNotification);
        addAndMakeVisible(octaveBox);

        barsBox.addItem("2 Bars", 2);
        barsBox.addItem("4 Bars", 4);
        barsBox.addItem("8 Bars", 8);
        barsBox.setSelectedId(4, juce::dontSendNotification);
        addAndMakeVisible(barsBox);

        patchBox.addItem("Synth: Analog Sub", 1);
        patchBox.addItem("Synth: Saw Lead", 2);
        patchBox.addItem("Synth: Warm Pad", 3);
        patchBox.addItem("Synth: Pluck Bass", 4);
        patchBox.setSelectedId(4, juce::dontSendNotification);
        patchBox.onChange = [this] {
            if (auto t = engine.getTrack(targetTrackBox.getSelectedId() - 1))
                t->fallbackSynthPatch.store(patchBox.getSelectedId() - 1);
        };
        addAndMakeVisible(patchBox);

        addAndMakeVisible(targetTrackBox);

        lockKickBtn.setButtonText("Lock to Drum Kick");
        lockKickBtn.setClickingTogglesState(true);
        lockKickBtn.setToggleState(true, juce::dontSendNotification);
        lockKickBtn.setColour(juce::TextButton::buttonOnColourId, OctoColours::accentAmber.darker(0.2f));
        addAndMakeVisible(lockKickBtn);

        densitySlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        densitySlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 58, 15);
        densitySlider.setRange(0.2, 1.0, 0.05);
        densitySlider.setValue(0.65);
        densitySlider.setColour(juce::Slider::rotarySliderFillColourId, OctoColours::accentCyan);
        addAndMakeVisible(densitySlider);

        syncSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        syncSlider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 58, 15);
        syncSlider.setRange(0.0, 1.0, 0.05);
        syncSlider.setValue(0.45);
        syncSlider.setColour(juce::Slider::rotarySliderFillColourId, OctoColours::accentPurp);
        addAndMakeVisible(syncSlider);

        densLabel.setText("DENSITY", juce::dontSendNotification);
        densLabel.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        densLabel.setColour(juce::Label::textColourId, OctoColours::textDim);
        densLabel.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(densLabel);

        syncLabel.setText("SYNCOPATION", juce::dontSendNotification);
        syncLabel.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        syncLabel.setColour(juce::Label::textColourId, OctoColours::textDim);
        syncLabel.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(syncLabel);

        generateBtn.setButtonText("GENERATE RIFF / MELODY");
        generateBtn.setColour(juce::TextButton::buttonColourId, OctoColours::accentTeal.darker(0.2f));
        generateBtn.onClick = [this] { runGenerate(); };
        addAndMakeVisible(generateBtn);

        mutateBtn.setButtonText("Mutate Existing");
        mutateBtn.setColour(juce::TextButton::buttonColourId, OctoColours::accentPurp.darker(0.25f));
        mutateBtn.onClick = [this] { runMutate(); };
        addAndMakeVisible(mutateBtn);

        openPianoRollBtn.setButtonText("Inspect in Piano Roll ->");
        openPianoRollBtn.onClick = [this] {
            if (onOpenInPianoRoll)
                onOpenInPianoRoll(targetTrackBox.getSelectedId() - 1);
        };
        addAndMakeVisible(openPianoRollBtn);

        statusLabel.setText("Generates musical basslines, riffs, chords, and melodies directly onto any MIDI track.",
                            juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, OctoColours::textDim);
        statusLabel.setFont(juce::FontOptions(11.0f));
        addAndMakeVisible(statusLabel);

        syncTrackCombo();
    }

    void syncTrackCombo()
    {
        const int prevId = targetTrackBox.getSelectedId();
        targetTrackBox.clear(juce::dontSendNotification);
        const int count = engine.getNumTracks();
        int preferredId = 1;
        for (int i = 0; i < count; ++i)
        {
            if (auto t = engine.getTrack(i))
            {
                targetTrackBox.addItem("Target: " + t->name, i + 1);
                if (t->mode.load() == TrackMode::Midi && !t->useInternalDrumSynth.load())
                    preferredId = i + 1;
            }
        }
        if (prevId >= 1 && prevId <= count)
            targetTrackBox.setSelectedId(prevId, juce::dontSendNotification);
        else
            targetTrackBox.setSelectedId(preferredId, juce::dontSendNotification);
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(OctoColours::panelBg);
        g.fillRoundedRectangle(b, 6.0f);
        g.setColour(OctoColours::border);
        g.drawRoundedRectangle(b.reduced(0.5f), 6.0f, 1.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(10, 8);
        auto row1 = area.removeFromTop(26);
        targetTrackBox.setBounds(row1.removeFromLeft(160).reduced(2));
        styleBox.setBounds(row1.removeFromLeft(225).reduced(2));
        rootBox.setBounds(row1.removeFromLeft(90).reduced(2));
        scaleBox.setBounds(row1.removeFromLeft(155).reduced(2));
        octaveBox.setBounds(row1.removeFromLeft(78).reduced(2));
        barsBox.setBounds(row1.removeFromLeft(78).reduced(2));
        patchBox.setBounds(row1.removeFromLeft(150).reduced(2));

        area.removeFromTop(8);
        auto mid = area.removeFromTop(86);

        auto knob1 = mid.removeFromLeft(88);
        densLabel.setBounds(knob1.removeFromTop(14));
        densitySlider.setBounds(knob1);

        mid.removeFromLeft(10);
        auto knob2 = mid.removeFromLeft(94);
        syncLabel.setBounds(knob2.removeFromTop(14));
        syncSlider.setBounds(knob2);

        mid.removeFromLeft(18);
        auto controlsCol = mid.removeFromLeft(170);
        lockKickBtn.setBounds(controlsCol.removeFromTop(30).reduced(2));
        controlsCol.removeFromTop(6);
        openPianoRollBtn.setBounds(controlsCol.removeFromTop(30).reduced(2));

        mid.removeFromLeft(16);
        auto actionCol = mid;
        generateBtn.setBounds(actionCol.removeFromTop(40).reduced(2));
        actionCol.removeFromTop(6);
        mutateBtn.setBounds(actionCol.removeFromTop(32).reduced(2));

        area.removeFromTop(6);
        statusLabel.setBounds(area.removeFromTop(20));
    }

    std::function<void(int trackIndex)> onOpenInPianoRoll;

private:
    void runGenerate()
    {
        const int idx = targetTrackBox.getSelectedId() - 1;
        auto t = engine.getTrack(idx);
        if (t == nullptr) return;

        MidiAutoCreator::Params p;
        p.style = static_cast<MidiAutoCreator::Style>(juce::jlimit(0, 5, styleBox.getSelectedId() - 1));
        p.rootPc = rootBox.getSelectedId() - 1;
        p.scaleIdx = scaleBox.getSelectedId() - 1;
        p.octave = octaveBox.getSelectedId();
        p.numBars = std::max(2, barsBox.getSelectedId());
        p.density = static_cast<float>(densitySlider.getValue());
        p.syncopation = static_cast<float>(syncSlider.getValue());
        p.lockToDrumKick = lockKickBtn.getToggleState();

        auto generated = MidiAutoCreator::generate(p, engine.drumEngine);

        t->mode.store(TrackMode::Midi);
        t->useInternalDrumSynth.store(false);
        t->useFallbackPolySynth.store(true);
        t->fallbackSynthPatch.store(patchBox.getSelectedId() - 1);
        t->setMidiNotes(generated, engine.bpm.load(), engine.currentSampleRate.load());

        // Set loop length to match generated idea if playing a jam loop
        engine.setLoopBars(0, p.numBars);

        statusLabel.setText("Generated " + juce::String(generated.size()) + " notes in "
                            + MusicTheory::noteNames[static_cast<size_t>(p.rootPc)] + " "
                            + MusicTheory::getScales()[static_cast<size_t>(p.scaleIdx)].name
                            + " -> " + t->name + " (" + juce::String(p.numBars) + " bars). Press Space to hear!",
                            juce::dontSendNotification);
    }

    void runMutate()
    {
        const int idx = targetTrackBox.getSelectedId() - 1;
        auto t = engine.getTrack(idx);
        if (t == nullptr) return;

        auto notes = t->getMidiNotes(engine.bpm.load(), engine.currentSampleRate.load());
        if (notes.empty())
        {
            runGenerate();
            return;
        }

        MidiAutoCreator::mutate(notes, rootBox.getSelectedId() - 1, scaleBox.getSelectedId() - 1);
        t->setMidiNotes(notes, engine.bpm.load(), engine.currentSampleRate.load());
        statusLabel.setText("Mutated " + juce::String(notes.size()) + " notes on " + t->name + " within selected scale.",
                            juce::dontSendNotification);
    }

    AudioEngine& engine;
    juce::ComboBox targetTrackBox, styleBox, rootBox, scaleBox, octaveBox, barsBox, patchBox;
    juce::TextButton lockKickBtn, generateBtn, mutateBtn, openPianoRollBtn;
    juce::Slider densitySlider, syncSlider;
    juce::Label densLabel, syncLabel, statusLabel;
};
