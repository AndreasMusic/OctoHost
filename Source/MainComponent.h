#pragma once
#include <cstdint>
#include <utility>
#include <functional>
#include <juce_gui_extra/juce_gui_extra.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include "AudioEditorUI.h"
#include "AudioEngine.h"
#include "DrumSequencerUI.h"
#include "LookAndFeel.h"
#include "MelodyGeneratorUI.h"
#include "MixerStripUI.h"
#include "PianoRollUI.h"

class TunerBarComponent : public juce::Component
{
public:
    explicit TunerBarComponent(AudioEngine& eng) : engine(eng)
    {
        muteBtn.setButtonText("Silent Tune");
        muteBtn.setClickingTogglesState(true);
        muteBtn.setColour(juce::TextButton::buttonOnColourId, OctoColours::accentRed);
        muteBtn.onClick = [this] { engine.tuner.muteWhileTuning.store(muteBtn.getToggleState()); };
        addAndMakeVisible(muteBtn);
    }

    void updateReading(const ChromaticTuner::Reading& r) { reading = r; repaint(); }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(OctoColours::panelBg);
        g.fillRoundedRectangle(b, 5.0f);
        g.setColour(OctoColours::border);
        g.drawRoundedRectangle(b.reduced(0.5f), 5.0f, 1.0f);

        auto area = getLocalBounds().reduced(10, 4);
        area.removeFromRight(96);

        auto noteBox = area.removeFromLeft(78);
        const bool inTune = reading.valid && std::abs(reading.cents) <= 4.0f;
        g.setFont(juce::FontOptions(22.0f, juce::Font::bold));
        g.setColour(reading.valid ? (inTune ? OctoColours::accentTeal : OctoColours::textMain) : OctoColours::textDim);
        g.drawText(reading.valid ? reading.noteName : "--", noteBox, juce::Justification::centredLeft);

        auto hzBox = area.removeFromLeft(90);
        g.setFont(juce::FontOptions(12.0f));
        g.setColour(OctoColours::textDim);
        g.drawText(reading.valid ? juce::String(reading.frequencyHz, 1) + " Hz" : "Play note...",
                   hzBox, juce::Justification::centredLeft);

        auto meterArea = area.reduced(12, 8).toFloat();
        g.setColour(OctoColours::slotBg);
        g.fillRoundedRectangle(meterArea, 4.0f);

        const float cx = meterArea.getCentreX();
        g.setColour(OctoColours::accentTeal.withAlpha(0.6f));
        g.drawVerticalLine(static_cast<int>(cx), meterArea.getY(), meterArea.getBottom());

        if (reading.valid)
        {
            const float norm = juce::jlimit(-1.0f, 1.0f, reading.cents / 50.0f);
            const float px = cx + norm * (meterArea.getWidth() * 0.46f);
            g.setColour(inTune ? OctoColours::accentTeal : OctoColours::accentAmber);
            g.fillRoundedRectangle(px - 4.0f, meterArea.getY() + 2.0f, 8.0f, meterArea.getHeight() - 4.0f, 3.0f);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(8, 5);
        muteBtn.setBounds(r.removeFromRight(88));
    }

private:
    AudioEngine& engine;
    ChromaticTuner::Reading reading;
    juce::TextButton muteBtn;
};

class TimelineOverview : public juce::Component
{
public:
    explicit TimelineOverview(AudioEngine& eng) : engine(eng) {}

    std::function<void(int trackIdx)> onDoubleClickTrackLane;

    void paint(juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        g.setColour(OctoColours::slotBg);
        g.fillRoundedRectangle(area, 6.0f);

        const double totalSec = getDisplayDurationSeconds();
        const double sr = engine.currentSampleRate.load();
        const double secPerBar = (60.0 / std::max(30.0, engine.bpm.load())) * 4.0;

        auto ruler = area.removeFromTop(16.0f);
        g.setColour(OctoColours::panelBg);
        g.fillRoundedRectangle(ruler, 4.0f);

        const int numBars = static_cast<int>(std::ceil(totalSec / secPerBar));
        g.setFont(juce::FontOptions(10.0f));
        for (int bar = 0; bar <= numBars; ++bar)
        {
            const float x = area.getX() + static_cast<float>((bar * secPerBar / totalSec) * area.getWidth());
            g.setColour(OctoColours::border.withAlpha(bar % 4 == 0 ? 0.65f : 0.25f));
            g.drawVerticalLine(static_cast<int>(x), ruler.getY(), area.getBottom());

            if (bar % 2 == 0)
            {
                g.setColour(OctoColours::textDim);
                g.drawText(juce::String(bar + 1), static_cast<int>(x) + 3, static_cast<int>(ruler.getY()),
                           26, 15, juce::Justification::centredLeft);
            }
        }

        if (sr > 0.0 && engine.loopEndSample.load() > engine.loopStartSample.load())
        {
            const double ls = static_cast<double>(engine.loopStartSample.load()) / sr;
            const double le = static_cast<double>(engine.loopEndSample.load()) / sr;
            const float x1 = area.getX() + static_cast<float>((ls / totalSec) * area.getWidth());
            const float x2 = area.getX() + static_cast<float>((le / totalSec) * area.getWidth());
            const bool loopOn = engine.loopEnabled.load();
            auto loopCol = loopOn ? OctoColours::accentTeal : OctoColours::textDim;
            g.setColour(loopCol.withAlpha(loopOn ? 0.55f : 0.2f));
            g.fillRoundedRectangle(x1, ruler.getY() + 2.0f, std::max(4.0f, x2 - x1), ruler.getHeight() - 4.0f, 3.0f);
            g.setColour(loopCol.withAlpha(loopOn ? 0.07f : 0.02f));
            g.fillRect(x1, area.getY(), std::max(4.0f, x2 - x1), area.getHeight());
        }

        const int trackCount = std::max(1, engine.getNumTracks());
        const float laneH = area.getHeight() / static_cast<float>(trackCount);
        for (int i = 0; i < trackCount; ++i)
        {
            auto track = engine.getTrack(i);
            if (track == nullptr) continue;
            const float y = area.getY() + i * laneH;

            g.setColour(OctoColours::border.withAlpha(0.32f));
            g.drawHorizontalLine(static_cast<int>(y), area.getX(), area.getRight());

            g.setFont(juce::FontOptions(std::min(9.5f, std::max(7.0f, laneH - 2.0f)), juce::Font::bold));
            g.setColour(track->trackColour.withAlpha(0.75f));
            g.drawText(track->name, static_cast<int>(area.getX()) + 4, static_cast<int>(y),
                       58, static_cast<int>(laneH), juce::Justification::centredLeft);

            const double clipSec = sr > 0.0 ? static_cast<double>(track->getClipLengthSamples()) / sr : 0.0;
            if (clipSec <= 0.0) continue;

            const float w = std::min(area.getWidth() - 4.0f,
                                     static_cast<float>((clipSec / totalSec) * area.getWidth()));
            auto clipRect = juce::Rectangle<float>(area.getX() + 1.0f, y + 1.0f, w, std::max(2.0f, laneH - 2.0f));

            g.setColour(track->trackColour.withAlpha(0.24f));
            g.fillRoundedRectangle(clipRect, 2.0f);
            g.setColour(track->trackColour.withAlpha(0.65f));
            g.drawRoundedRectangle(clipRect, 2.0f, 0.8f);

            if (track->mode.load() == TrackMode::Audio)
            {
                std::vector<float> peaks;
                track->getWaveformPreview(peaks);
                if (!peaks.empty())
                {
                    g.setColour(track->trackColour.brighter(0.25f));
                    const float midY = clipRect.getCentreY();
                    const float halfH = clipRect.getHeight() * 0.45f;
                    const float dx = clipRect.getWidth() / static_cast<float>(peaks.size());
                    for (size_t k = 0; k < peaks.size(); ++k)
                    {
                        const float px = clipRect.getX() + k * dx;
                        const float ph = std::max(1.0f, peaks[k] * halfH);
                        g.drawVerticalLine(static_cast<int>(px), midY - ph, midY + ph);
                    }
                }
            }
            else
            {
                std::vector<int64_t> notePositions;
                track->getMidiPreviewPositions(notePositions);
                g.setColour(track->trackColour.brighter(0.35f));
                for (int64_t sp : notePositions)
                {
                    const float nx = area.getX() + static_cast<float>(((static_cast<double>(sp) / sr) / totalSec) * area.getWidth());
                    if (nx <= clipRect.getRight())
                        g.fillRect(nx, clipRect.getY() + 1.0f, 2.0f, std::max(1.0f, clipRect.getHeight() - 2.0f));
                }
            }
        }

        const double posSec = engine.getPlayheadSeconds();
        const float px = area.getX() + static_cast<float>((posSec / totalSec) * area.getWidth());
        g.setColour(engine.isRecording.load() ? OctoColours::accentRed : juce::Colours::white);
        g.drawLine(px, getLocalBounds().toFloat().getY(), px, area.getBottom(), 1.8f);

        g.setColour(OctoColours::border);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 6.0f, 1.0f);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            showTimelineMenu(e.position.x);
            return;
        }
        if (e.getNumberOfClicks() >= 2 && e.position.y > 16.0f)
        {
            const int count = std::max(1, engine.getNumTracks());
            const float laneH = (getHeight() - 16.0f) / static_cast<float>(count);
            const int idx = juce::jlimit(0, count - 1, static_cast<int>((e.position.y - 16.0f) / laneH));
            if (onDoubleClickTrackLane)
                onDoubleClickTrackLane(idx);
            return;
        }
        seekToX(e.position.x);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!e.mods.isPopupMenu())
            seekToX(e.position.x);
    }

private:
    double getDisplayDurationSeconds() const
    {
        const double secPerBar = (60.0 / std::max(30.0, engine.bpm.load())) * 4.0;
        double maxSec = secPerBar * 16.0;
        const double sr = engine.currentSampleRate.load();
        if (sr > 0.0)
        {
            const int count = engine.getNumTracks();
            for (int i = 0; i < count; ++i)
                if (auto t = engine.getTrack(i))
                    maxSec = std::max(maxSec, static_cast<double>(t->getClipLengthSamples()) / sr + secPerBar * 2.0);
            maxSec = std::max(maxSec, static_cast<double>(engine.loopEndSample.load()) / sr + secPerBar * 2.0);
            maxSec = std::max(maxSec, engine.getPlayheadSeconds() + secPerBar * 2.0);
        }
        return maxSec;
    }

    int barAtX(float x) const
    {
        const double ratio = juce::jlimit(0.0, 1.0, static_cast<double>(x) / std::max(1, getWidth()));
        const double sec = ratio * getDisplayDurationSeconds();
        const double secPerBar = (60.0 / std::max(30.0, engine.bpm.load())) * 4.0;
        return std::max(0, static_cast<int>(std::floor(sec / secPerBar)));
    }

    void seekToX(float x)
    {
        const double ratio = juce::jlimit(0.0, 1.0, static_cast<double>(x) / std::max(1, getWidth()));
        engine.setPlayheadSeconds(ratio * getDisplayDurationSeconds());
        repaint();
    }

    void showTimelineMenu(float clickX)
    {
        const int clickedBar = barAtX(clickX);
        juce::PopupMenu m;
        m.addSectionHeader("Timeline & Loop (Bar " + juce::String(clickedBar + 1) + ")");
        m.addItem(1, engine.loopEnabled.load() ? "Disable Loop" : "Enable Loop", true, engine.loopEnabled.load());
        m.addSeparator();
        m.addItem(2, "Set Loop Start Here (Bar " + juce::String(clickedBar + 1) + ")");
        m.addItem(3, "Set Loop End Here (End of Bar " + juce::String(clickedBar + 1) + ")");
        m.addSeparator();
        m.addItem(10, "Quick 2-Bar Loop (Bars 1-2)");
        m.addItem(11, "Quick 4-Bar Loop (Bars 1-4)");
        m.addItem(12, "Quick 8-Bar Loop (Bars 1-8)");
        m.addItem(13, "Quick 16-Bar Loop (Bars 1-16)");
        m.addSeparator();
        m.addItem(99, "Clear All Recorded Clips");

        m.showMenuAsync(juce::PopupMenu::Options(), [this, clickedBar](int r) {
            const double spb = engine.getSamplesPerBar();
            if (r == 1) engine.loopEnabled.store(!engine.loopEnabled.load());
            else if (r == 2)
            {
                const int64_t ns = static_cast<int64_t>(clickedBar * spb);
                engine.loopStartSample.store(ns);
                if (engine.loopEndSample.load() <= ns)
                    engine.loopEndSample.store(ns + static_cast<int64_t>(spb * 4.0));
                engine.loopEnabled.store(true);
            }
            else if (r == 3)
            {
                const int64_t ne = static_cast<int64_t>((clickedBar + 1) * spb);
                if (ne > engine.loopStartSample.load())
                    engine.loopEndSample.store(ne);
                engine.loopEnabled.store(true);
            }
            else if (r == 10) { engine.setLoopBars(0, 2);  engine.loopEnabled.store(true); }
            else if (r == 11) { engine.setLoopBars(0, 4);  engine.loopEnabled.store(true); }
            else if (r == 12) { engine.setLoopBars(0, 8);  engine.loopEnabled.store(true); }
            else if (r == 13) { engine.setLoopBars(0, 16); engine.loopEnabled.store(true); }
            else if (r == 99)
            {
                for (int i = 0; i < engine.getNumTracks(); ++i)
                    if (auto t = engine.getTrack(i)) t->clearClip();
            }
            repaint();
        });
    }

    AudioEngine& engine;
};

class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    enum class EditorTab { Hidden = 0, Drums = 1, PianoRoll = 2, AudioEdit = 3, MelodyForge = 4 };

    MainComponent()
        : tunerBar(engine),
          timeline(engine),
          drumUI(engine),
          pianoRollUI(engine),
          audioEditorUI(engine),
          melodyForgeUI(engine),
          fxBusAUI(engine, engine.fxBusA, OctoColours::accentBlue),
          fxBusBUI(engine, engine.fxBusB, OctoColours::accentPurp),
          masterUI(engine)
    {
        setLookAndFeel(&lookAndFeel);
        setWantsKeyboardFocus(true);

        engine.onTrackLayoutChanged = [this] { rebuildMixerStrips(); };

        // Transport
        rewindBtn.setButtonText("|<");
        rewindBtn.onClick = [this] { engine.setPlayheadSeconds(0.0); };
        addAndMakeVisible(rewindBtn);

        playBtn.setButtonText("Play");
        playBtn.onClick = [this] {
            if (engine.isPlaying.load()) engine.pause();
            else                         engine.play();
        };
        addAndMakeVisible(playBtn);

        stopBtn.setButtonText("Stop");
        stopBtn.onClick = [this] { engine.stop(true); };
        addAndMakeVisible(stopBtn);

        recBtn.setButtonText("Rec");
        recBtn.onClick = [this] { engine.toggleRecord(); };
        addAndMakeVisible(recBtn);

        loopBtn.setButtonText("Loop");
        loopBtn.setClickingTogglesState(true);
        loopBtn.setToggleState(engine.loopEnabled.load(), juce::dontSendNotification);
        loopBtn.setColour(juce::TextButton::buttonOnColourId, OctoColours::accentTeal.darker(0.15f));
        loopBtn.onClick = [this] { engine.loopEnabled.store(loopBtn.getToggleState()); };
        addAndMakeVisible(loopBtn);

        timeLabel.setFont(juce::FontOptions(13.5f, juce::Font::bold));
        timeLabel.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(timeLabel);

        bpmSlider.setRange(40.0, 240.0, 1.0);
        bpmSlider.setValue(120.0);
        bpmSlider.setTextValueSuffix(" BPM");
        bpmSlider.setSliderStyle(juce::Slider::IncDecButtons);
        bpmSlider.onValueChange = [this] { engine.setBpm(bpmSlider.getValue()); };
        addAndMakeVisible(bpmSlider);

        tapBtn.setButtonText("Tap");
        tapBtn.onClick = [this] { handleTapTempo(); };
        addAndMakeVisible(tapBtn);

        clickBtn.setButtonText("Click");
        clickBtn.setTooltip("Left-click: Metronome | Right-click: 1-Bar Count-In & Click Level");
        clickBtn.setClickingTogglesState(true);
        clickBtn.setColour(juce::TextButton::buttonOnColourId, OctoColours::accentAmber.darker(0.15f));
        clickBtn.onClick = [this] { engine.metronomeEnabled.store(clickBtn.getToggleState()); };
        clickBtn.onRightClick = [this](const juce::MouseEvent&) { showClickMenu(); };
        addAndMakeVisible(clickBtn);

        // Dynamic Track Add Button
        addTrackBtn.setButtonText("+ Track");
        addTrackBtn.setColour(juce::TextButton::buttonColourId, OctoColours::accentTeal.darker(0.32f));
        addTrackBtn.onClick = [this] { showAddTrackMenu(); };
        addAndMakeVisible(addTrackBtn);

        tunerBtn.setButtonText("Tuner");
        tunerBtn.setClickingTogglesState(true);
        tunerBtn.setColour(juce::TextButton::buttonOnColourId, OctoColours::accentTeal.darker(0.2f));
        tunerBtn.onClick = [this] {
            showTunerBar = tunerBtn.getToggleState();
            engine.tuner.active.store(showTunerBar);
            tunerBar.setVisible(showTunerBar);
            resized();
        };
        addAndMakeVisible(tunerBtn);

        sessionBtn.setButtonText("File v");
        sessionBtn.onClick = [this] { showSessionMenu(); };
        addAndMakeVisible(sessionBtn);

        pluginsBtn.setButtonText("Plugins");
        pluginsBtn.onClick = [this] { openPluginScannerDialog(); };
        addAndMakeVisible(pluginsBtn);

        settingsBtn.setButtonText("Audio/MIDI");
        settingsBtn.onClick = [this] { openAudioSettingsDialog(); };
        addAndMakeVisible(settingsBtn);

        // Streamlined Studio Editor Deck Tab Bar
        setupTabButton(tabDrumsBtn,  "1: DRUMS",        EditorTab::Drums,       OctoColours::accentAmber);
        setupTabButton(tabPianoBtn,  "2: PIANO ROLL",   EditorTab::PianoRoll,   OctoColours::accentPurp);
        setupTabButton(tabAudioBtn,  "3: AUDIO EDITOR", EditorTab::AudioEdit,   OctoColours::accentTeal);
        setupTabButton(tabMelodyBtn, "4: MELODY FORGE", EditorTab::MelodyForge, OctoColours::accentCyan);
        setupTabButton(tabHideBtn,   "Hide Deck",       EditorTab::Hidden,      OctoColours::border);

        addChildComponent(tunerBar);
        addAndMakeVisible(timeline);
        timeline.onDoubleClickTrackLane = [this](int idx) { selectTrack(idx, true); };

        addChildComponent(drumUI);
        addChildComponent(pianoRollUI);
        addChildComponent(audioEditorUI);
        addChildComponent(melodyForgeUI);
        melodyForgeUI.onOpenInPianoRoll = [this](int idx) {
            pianoRollUI.setTrackIndex(idx);
            setActiveTab(EditorTab::PianoRoll);
        };

        // Scrollable Track Mixer Viewport + Permanently Docked FX A / FX B / Master
        tracksViewport.setViewedComponent(&tracksContainer, false);
        tracksViewport.setScrollBarsShown(false, true);
        addAndMakeVisible(tracksViewport);

        addAndMakeVisible(fxBusAUI);
        addAndMakeVisible(fxBusBUI);
        addAndMakeVisible(masterUI);

        rebuildMixerStrips();
        setActiveTab(EditorTab::Drums);
        setSize(1380, 860);
        startTimerHz(30);
    }

    ~MainComponent() override
    {
        stopTimer();
        engine.onTrackLayoutChanged = nullptr;
        setLookAndFeel(nullptr);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::spaceKey)
        {
            if (engine.isPlaying.load()) engine.pause();
            else                         engine.play();
            return true;
        }
        if (key == juce::KeyPress::returnKey)
        {
            engine.setPlayheadSeconds(0.0);
            return true;
        }
        const auto ch = juce::CharacterFunctions::toLowerCase(key.getTextCharacter());
        if (ch == 'r') { engine.toggleRecord(); return true; }
        if (ch == 'l') { loopBtn.triggerClick(); return true; }
        if (ch == 'm') { clickBtn.triggerClick(); return true; }
        if (ch == 't') { tunerBtn.triggerClick(); return true; }
        if (ch == '1') { setActiveTab(EditorTab::Drums); return true; }
        if (ch == '2') { setActiveTab(EditorTab::PianoRoll); return true; }
        if (ch == '3') { setActiveTab(EditorTab::AudioEdit); return true; }
        if (ch == '4') { setActiveTab(EditorTab::MelodyForge); return true; }
        if (ch == 'd')
        {
            setActiveTab(activeTab == EditorTab::Hidden ? EditorTab::Drums : EditorTab::Hidden);
            return true;
        }
        return false;
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(OctoColours::bgDark);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8);

        // Top Transport Bar
        auto top = area.removeFromTop(32);
        rewindBtn.setBounds(top.removeFromLeft(32).reduced(1));
        playBtn.setBounds(top.removeFromLeft(50).reduced(1));
        stopBtn.setBounds(top.removeFromLeft(44).reduced(1));
        recBtn.setBounds(top.removeFromLeft(42).reduced(1));
        loopBtn.setBounds(top.removeFromLeft(46).reduced(1));
        top.removeFromLeft(6);
        timeLabel.setBounds(top.removeFromLeft(152).reduced(1));
        bpmSlider.setBounds(top.removeFromLeft(108).reduced(1));
        tapBtn.setBounds(top.removeFromLeft(38).reduced(1));
        clickBtn.setBounds(top.removeFromLeft(48).reduced(1));
        top.removeFromLeft(8);
        addTrackBtn.setBounds(top.removeFromLeft(66).reduced(1));

        settingsBtn.setBounds(top.removeFromRight(80).reduced(1));
        pluginsBtn.setBounds(top.removeFromRight(62).reduced(1));
        sessionBtn.setBounds(top.removeFromRight(56).reduced(1));
        top.removeFromRight(6);
        tunerBtn.setBounds(top.removeFromRight(54).reduced(1));

        area.removeFromTop(5);

        if (showTunerBar)
        {
            tunerBar.setBounds(area.removeFromTop(36));
            area.removeFromTop(5);
        }

        // Timeline Overview
        timeline.setBounds(area.removeFromTop(98));
        area.removeFromTop(5);

        // Studio Editor Deck Tab Strip
        auto tabRow = area.removeFromTop(24);
        tabDrumsBtn.setBounds(tabRow.removeFromLeft(110).reduced(1));
        tabPianoBtn.setBounds(tabRow.removeFromLeft(124).reduced(1));
        tabAudioBtn.setBounds(tabRow.removeFromLeft(128).reduced(1));
        tabMelodyBtn.setBounds(tabRow.removeFromLeft(132).reduced(1));
        tabHideBtn.setBounds(tabRow.removeFromRight(80).reduced(1));
        area.removeFromTop(4);

        // Active Editor Deck View
        if (activeTab != EditorTab::Hidden)
        {
            const int deckH = (activeTab == EditorTab::PianoRoll) ? 236 : 196;
            auto deckArea = area.removeFromTop(deckH);
            drumUI.setBounds(deckArea);
            pianoRollUI.setBounds(deckArea);
            audioEditorUI.setBounds(deckArea);
            melodyForgeUI.setBounds(deckArea);
            area.removeFromTop(6);
        }

        // Bottom Console: Scrollable Tracks on Left + Docked FX A, FX B, Master on Right
        constexpr int busStripW = 112;
        constexpr int masterStripW = 122;
        constexpr int gapW = 8;

        masterUI.setBounds(area.removeFromRight(masterStripW));
        fxBusBUI.setBounds(area.removeFromRight(busStripW));
        fxBusAUI.setBounds(area.removeFromRight(busStripW));
        area.removeFromRight(gapW);

        tracksViewport.setBounds(area);
        layoutTrackStripsInViewport();
    }

private:
    void setupTabButton(juce::TextButton& b, const juce::String& text, EditorTab tab, juce::Colour accent)
    {
        b.setButtonText(text);
        b.onClick = [this, tab] { setActiveTab(tab); };
        b.getProperties().set("accent", static_cast<int64_t>(accent.getARGB()));
        addAndMakeVisible(b);
    }

    void setActiveTab(EditorTab tab)
    {
        activeTab = tab;
        drumUI.setVisible(tab == EditorTab::Drums);
        pianoRollUI.setVisible(tab == EditorTab::PianoRoll);
        audioEditorUI.setVisible(tab == EditorTab::AudioEdit);
        melodyForgeUI.setVisible(tab == EditorTab::MelodyForge);

        auto styleTab = [this](juce::TextButton& b, EditorTab t, juce::Colour accent) {
            const bool active = (activeTab == t);
            b.setColour(juce::TextButton::buttonColourId,
                        active ? accent.darker(0.25f) : OctoColours::panelBg);
            b.setColour(juce::TextButton::textColourOffId,
                        active ? juce::Colours::white : OctoColours::textDim);
        };
        styleTab(tabDrumsBtn,  EditorTab::Drums,       OctoColours::accentAmber);
        styleTab(tabPianoBtn,  EditorTab::PianoRoll,   OctoColours::accentPurp);
        styleTab(tabAudioBtn,  EditorTab::AudioEdit,   OctoColours::accentTeal);
        styleTab(tabMelodyBtn, EditorTab::MelodyForge, OctoColours::accentCyan);
        styleTab(tabHideBtn,   EditorTab::Hidden,      OctoColours::border);

        resized();
    }

    void selectTrack(int idx, bool openMatchingEditor)
    {
        selectedTrackIndex = juce::jlimit(0, std::max(0, engine.getNumTracks() - 1), idx);
        for (auto& s : trackStrips)
            s->setSelected(s->getTrackIndex() == selectedTrackIndex);

        if (auto t = engine.getTrack(selectedTrackIndex))
        {
            if (t->mode.load() == TrackMode::Midi)
            {
                pianoRollUI.setTrackIndex(selectedTrackIndex);
                if (openMatchingEditor)
                    setActiveTab(static_cast<InputSourceType>(t->inputSourceType.load()) == InputSourceType::DrumSeq
                                     ? EditorTab::Drums : EditorTab::PianoRoll);
            }
            else
            {
                audioEditorUI.setTrackIndex(selectedTrackIndex);
                if (openMatchingEditor)
                    setActiveTab(EditorTab::AudioEdit);
            }
        }
    }

    void rebuildMixerStrips()
    {
        trackStrips.clear();
        tracksContainer.removeAllChildren();

        const int count = engine.getNumTracks();
        selectedTrackIndex = juce::jlimit(0, std::max(0, count - 1), selectedTrackIndex);

        for (int i = 0; i < count; ++i)
        {
            if (auto t = engine.getTrack(i))
            {
                auto strip = std::make_unique<TrackStripUI>(engine, t, i);
                strip->setSelected(i == selectedTrackIndex);
                strip->onSelectTrack = [this](int idx, bool openEd) { selectTrack(idx, openEd); };
                tracksContainer.addAndMakeVisible(*strip);
                trackStrips.push_back(std::move(strip));
            }
        }

        pianoRollUI.syncTrackCombo();
        audioEditorUI.syncTrackCombo();
        melodyForgeUI.syncTrackCombo();
        layoutTrackStripsInViewport();
    }

    void layoutTrackStripsInViewport()
    {
        const int count = std::max(1, static_cast<int>(trackStrips.size()));
        const int vpW = std::max(200, tracksViewport.getWidth());
        const int vpH = std::max(180, tracksViewport.getHeight() - (tracksViewport.isHorizontalScrollBarShown() ? 10 : 0));
        const int stripW = std::max(114, vpW / count);
        const int totalW = stripW * count;

        tracksContainer.setSize(totalW, vpH);
        for (int i = 0; i < static_cast<int>(trackStrips.size()); ++i)
            trackStrips[static_cast<size_t>(i)]->setBounds(i * stripW, 0, stripW, vpH);
    }

    void showAddTrackMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader("Add New Track (" + juce::String(engine.getNumTracks()) + "/" + juce::String(AudioEngine::maxTracks) + ")");
        m.addItem(1, "Add Audio Track (Guitar / Bass / Mic)");
        m.addItem(2, "Add MIDI Instrument Track (Synth / Drums)");
        m.addSeparator();
        m.addItem(3, "Add Double-Tracked Guitar Pair (L -80% / R +80%)");
        m.addItem(4, "Add Bass Track (35 Hz HPF)");
        m.addItem(5, "Add Synth Lead / Pad Track (MIDI)");

        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(addTrackBtn), [this](int r) {
            int newIdx = -1;
            if (r == 1) newIdx = engine.addTrack(TrackMode::Audio, {}, OctoColours::accentBlue);
            else if (r == 2) newIdx = engine.addTrack(TrackMode::Midi, {}, OctoColours::accentPurp, 0.0f, 0, PolyFallbackSynth::Patch::SawLead);
            else if (r == 3) { engine.addGuitarDoublePair(); newIdx = engine.getNumTracks() - 1; }
            else if (r == 4) newIdx = engine.addTrack(TrackMode::Audio, "BASS 2", OctoColours::accentTeal, 0.0f, 1);
            else if (r == 5) newIdx = engine.addTrack(TrackMode::Midi, "PAD", OctoColours::accentPurp, 0.0f, 0, PolyFallbackSynth::Patch::WarmPad);

            if (newIdx >= 0)
                selectTrack(newIdx, false);
        });
    }

    void timerCallback() override
    {
        engine.drainAllTrackMidi();

        const bool playing = engine.isPlaying.load();
        const bool recording = engine.isRecording.load();
        const bool countingIn = engine.countInRemainingSamples.load() > 0;

        playBtn.setButtonText(playing ? "Pause" : "Play");
        recBtn.setColour(juce::TextButton::buttonColourId,
                         recording ? OctoColours::accentRed
                                   : (countingIn ? OctoColours::accentAmber : juce::Colour(0xff262b36)));
        loopBtn.setToggleState(engine.loopEnabled.load(), juce::dontSendNotification);

        const double sec = engine.getPlayheadSeconds();
        const int mins = static_cast<int>(sec) / 60;
        const double remSec = sec - (mins * 60);
        const double totalBeats = sec * (engine.bpm.load() / 60.0);
        const int bar = static_cast<int>(totalBeats / 4.0) + 1;
        const int beat = (static_cast<int>(totalBeats) % 4) + 1;

        if (countingIn)
            timeLabel.setText("COUNT-IN...", juce::dontSendNotification);
        else
            timeLabel.setText(juce::String::formatted("%02d:%04.1f | %d.%d", mins, remSec, bar, beat),
                              juce::dontSendNotification);

        if (showTunerBar)
            tunerBar.updateReading(engine.tuner.analyse());

        timeline.repaint();
        if (activeTab == EditorTab::Drums)      drumUI.refresh();
        else if (activeTab == EditorTab::PianoRoll) pianoRollUI.refresh();
        else if (activeTab == EditorTab::AudioEdit) audioEditorUI.refresh();

        for (auto& s : trackStrips)
            s->refresh();
        fxBusAUI.refresh();
        fxBusBUI.refresh();
        masterUI.refresh();
    }

    void handleTapTempo()
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (lastTapMs > 0.0 && (now - lastTapMs) < 2000.0)
        {
            const double instBpm = 60000.0 / (now - lastTapMs);
            if (instBpm >= 40.0 && instBpm <= 240.0)
            {
                tapAvgBpm = (tapAvgBpm <= 0.0) ? instBpm : (tapAvgBpm * 0.5 + instBpm * 0.5);
                bpmSlider.setValue(std::round(tapAvgBpm));
            }
        }
        else
        {
            tapAvgBpm = 0.0;
        }
        lastTapMs = now;
    }

    void showClickMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader("Metronome & Count-In");
        m.addItem(1, "1-Bar Count-In Before Record", true, engine.countInEnabled.load());
        m.addSeparator();
        const float v = engine.metronomeVolume.load();
        m.addItem(2, "Click Level: Soft (35%)", true, v < 0.45f);
        m.addItem(3, "Click Level: Medium (65%)", true, v >= 0.45f && v < 0.8f);
        m.addItem(4, "Click Level: Loud (95%)", true, v >= 0.8f);

        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(clickBtn), [this](int r) {
            if (r == 1) engine.countInEnabled.store(!engine.countInEnabled.load());
            else if (r == 2) engine.metronomeVolume.store(0.35f);
            else if (r == 3) engine.metronomeVolume.store(0.65f);
            else if (r == 4) engine.metronomeVolume.store(0.95f);
        });
    }

    void showSessionMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader("Session & Mastering Export");
        m.addItem(1, "Save Session (.octo)...");
        m.addItem(2, "Load Session (.octo)...");
        m.addSeparator();
        m.addItem(3, "Bounce Stereo Mixdown (24-bit WAV)...");
        m.addItem(4, "Export All Active Track Stems (24-bit WAV Folder)...");

        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(sessionBtn), [this](int r) {
            if (r == 1)
            {
                fileChooser = std::make_unique<juce::FileChooser>(
                    "Save OctoHost Session",
                    juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("JamSession.octo"),
                    "*.octo");
                fileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                         [this](const juce::FileChooser& fc) {
                                             auto f = fc.getResult();
                                             if (f != juce::File()) engine.saveSession(f);
                                         });
            }
            else if (r == 2)
            {
                fileChooser = std::make_unique<juce::FileChooser>(
                    "Load OctoHost Session",
                    juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
                    "*.octo");
                fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                         [this](const juce::FileChooser& fc) {
                                             auto f = fc.getResult();
                                             if (f.existsAsFile())
                                             {
                                                 engine.loadSession(f);
                                                 bpmSlider.setValue(engine.bpm.load(), juce::dontSendNotification);
                                             }
                                         });
            }
            else if (r == 3)
            {
                fileChooser = std::make_unique<juce::FileChooser>(
                    "Bounce Stereo Mixdown WAV",
                    juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile("OctoHost_Mix.wav"),
                    "*.wav");
                fileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                         [this](const juce::FileChooser& fc) {
                                             auto f = fc.getResult();
                                             if (f != juce::File())
                                             {
                                                 const bool ok = engine.exportMixdownWav(f);
                                                 juce::AlertWindow::showMessageBoxAsync(
                                                     ok ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                                                     "WAV Mixdown",
                                                     ok ? ("Exported 24-bit stereo WAV to:\n" + f.getFullPathName())
                                                        : "Failed to write WAV file.");
                                             }
                                         });
            }
            else if (r == 4)
            {
                fileChooser = std::make_unique<juce::FileChooser>(
                    "Select Folder for Multi-Track Stems",
                    juce::File::getSpecialLocation(juce::File::userDesktopDirectory));
                fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                         [this](const juce::FileChooser& fc) {
                                             auto dir = fc.getResult();
                                             if (dir != juce::File())
                                             {
                                                 const int n = engine.exportAllStems(dir);
                                                 juce::AlertWindow::showMessageBoxAsync(
                                                     juce::MessageBoxIconType::InfoIcon,
                                                     "Stem Export Complete",
                                                     "Exported " + juce::String(n) + " 24-bit stereo WAV stems to:\n" + dir.getFullPathName());
                                             }
                                         });
            }
        });
    }

    void openPluginScannerDialog()
    {
        auto deadMansPedal = engine.getPluginDataDirectory().getChildFile("dead_mans_pedal.txt");
        auto* listComp = new juce::PluginListComponent(
            engine.formatManager, engine.knownPluginList, deadMansPedal, nullptr, true);
        listComp->setSize(740, 520);

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(listComp);
        opts.dialogTitle = "VST3 / AU Plugin Manager";
        opts.dialogBackgroundColour = OctoColours::panelBg;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = true;
        opts.resizable = true;
        opts.launchAsync();
    }

    void openAudioSettingsDialog()
    {
        auto* selector = new juce::AudioDeviceSelectorComponent(
            engine.deviceManager, 0, 32, 0, 32, true, false, true, false);
        selector->setSize(540, 460);

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(selector);
        opts.dialogTitle = "Audio & MIDI Device Settings";
        opts.dialogBackgroundColour = OctoColours::panelBg;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = true;
        opts.resizable = true;
        opts.launchAsync();
    }

    OctoLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { this, 450 };
    AudioEngine engine;

    TunerBarComponent tunerBar;
    TimelineOverview timeline;
    DrumSequencerUI drumUI;
    PianoRollUI pianoRollUI;
    AudioEditorUI audioEditorUI;
    MelodyGeneratorUI melodyForgeUI;

    juce::Viewport tracksViewport;
    juce::Component tracksContainer;
    std::vector<std::unique_ptr<TrackStripUI>> trackStrips;
    FXBusStripUI fxBusAUI, fxBusBUI;
    MasterStripUI masterUI;

    juce::TextButton rewindBtn, playBtn, stopBtn, recBtn, loopBtn, tapBtn, addTrackBtn;
    ContextButton clickBtn;
    juce::TextButton tunerBtn, sessionBtn, pluginsBtn, settingsBtn;
    juce::TextButton tabDrumsBtn, tabPianoBtn, tabAudioBtn, tabMelodyBtn, tabHideBtn;
    juce::Label timeLabel;
    juce::Slider bpmSlider;

    EditorTab activeTab = EditorTab::Drums;
    int selectedTrackIndex = 0;
    bool showTunerBar = false;
    double lastTapMs = 0.0;
    double tapAvgBpm = 0.0;
    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
