#pragma once
#include <cstdint>
#include <algorithm>
#include <utility>
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include "AudioEngine.h"
#include "LookAndFeel.h"

class StereoMeter : public juce::Component
{
public:
    void setLevels(float left, float right, float rmsVal = -1.0f)
    {
        lvlL = std::max(left, lvlL * 0.82f);
        lvlR = std::max(right, lvlR * 0.82f);
        if (rmsVal >= 0.0f)
            rms = std::max(rmsVal, rms * 0.88f);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const float halfW = (b.getWidth() - 2.0f) * 0.5f;
        drawBar(g, { b.getX(), b.getY(), halfW, b.getHeight() }, lvlL, rms);
        drawBar(g, { b.getX() + halfW + 2.0f, b.getY(), halfW, b.getHeight() }, lvlR, rms);
    }

private:
    static void drawBar(juce::Graphics& g, juce::Rectangle<float> r, float lvl, float rmsVal)
    {
        g.setColour(juce::Colour(0xff0b0d12));
        g.fillRoundedRectangle(r, 1.5f);

        const float clamped = juce::jlimit(0.0f, 1.0f, lvl);
        const float h = r.getHeight() * clamped;
        if (h > 0.5f)
        {
            auto fill = r;
            fill = fill.removeFromBottom(h);
            auto col = OctoColours::accentTeal;
            if (clamped > 0.94f) col = OctoColours::accentRed;
            else if (clamped > 0.78f) col = OctoColours::accentAmber;
            g.setColour(col);
            g.fillRoundedRectangle(fill, 1.5f);
        }

        if (rmsVal > 0.01f)
        {
            const float rh = r.getHeight() * juce::jlimit(0.0f, 1.0f, rmsVal);
            g.setColour(juce::Colours::white.withAlpha(0.55f));
            g.fillRect(r.getX(), r.getBottom() - rh, r.getWidth(), 2.0f);
        }
    }

    float lvlL = 0.0f, lvlR = 0.0f, rms = 0.0f;
};

inline void showInsertSlotMenu(AudioEngine& engine,
                               InsertChain& chain,
                               int slotIdx,
                               const juce::String& ownerTitle,
                               juce::Component* target,
                               std::function<void()> onUpdated)
{
    juce::PopupMenu menu;
    const bool hasPlug = chain.hasPlugin(slotIdx);
    const bool bypassed = chain.isBypassed(slotIdx);

    menu.addSectionHeader(ownerTitle + " - Insert " + juce::String(slotIdx + 1));

    if (hasPlug)
    {
        menu.addItem(1, "Open Plugin UI");
        menu.addItem(2, bypassed ? "Enable (Unbypass)" : "Bypass Slot", true, bypassed);
        if (slotIdx > 0) menu.addItem(3, "Move Up");
        if (slotIdx < InsertChain::numSlots - 1) menu.addItem(4, "Move Down");
        menu.addItem(5, "Clear Slot");
        menu.addSeparator();
    }

    auto types = engine.knownPluginList.getTypes();
    if (types.isEmpty())
    {
        menu.addItem(99, "No plugins scanned - Click 'Plugins' in toolbar", false);
    }
    else
    {
        juce::PopupMenu instrMenu, fxMenu;
        for (int i = 0; i < types.size(); ++i)
        {
            const auto& d = types.getReference(i);
            const juce::String label = d.name + "  [" + d.pluginFormatName + "]";
            if (d.isInstrument) instrMenu.addItem(1000 + i, label);
            else                fxMenu.addItem(1000 + i, label);
        }
        if (instrMenu.getNumItems() > 0) menu.addSubMenu("Load Instrument (VSTi / AUi)", instrMenu);
        if (fxMenu.getNumItems() > 0)    menu.addSubMenu("Load Effect (VST3 / AU)", fxMenu);
    }

    menu.addSeparator();
    menu.addItem(50, "Copy Entire 4-Slot Chain");
    menu.addItem(51, "Paste 4-Slot Chain", engine.hasCopiedInsertChain());

    auto opts = target != nullptr ? juce::PopupMenu::Options().withTargetComponent(target)
                                  : juce::PopupMenu::Options();

    menu.showMenuAsync(opts, [&engine, &chain, slotIdx, ownerTitle, types, bypassed, cb = std::move(onUpdated)](int r) {
        if (r == 1)      chain.openWindow(slotIdx, ownerTitle);
        else if (r == 2) chain.setBypassed(slotIdx, !bypassed);
        else if (r == 3) chain.swapSlots(slotIdx, slotIdx - 1);
        else if (r == 4) chain.swapSlots(slotIdx, slotIdx + 1);
        else if (r == 5) chain.clearSlot(slotIdx);
        else if (r == 50) engine.copyInsertChain(chain);
        else if (r == 51) engine.pasteInsertChain(chain);
        else if (r >= 1000 && (r - 1000) < types.size())
        {
            engine.loadPluginIntoChain(&chain, slotIdx, types.getReference(r - 1000),
                                       [cb](const juce::String& err) {
                                           if (err.isNotEmpty())
                                               juce::AlertWindow::showMessageBoxAsync(
                                                   juce::MessageBoxIconType::WarningIcon,
                                                   "Plugin Load Error", err);
                                           if (cb) cb();
                                       });
            return;
        }
        if (cb) cb();
    });
}

class TrackStripUI : public juce::Component
{
public:
    TrackStripUI(AudioEngine& eng, std::shared_ptr<Track> trk, int index)
        : engine(eng), track(std::move(trk)), trackIndex(index)
    {
        headerBtn.onClick = [this] {
            if (onSelectTrack) onSelectTrack(trackIndex, false);
        };
        headerBtn.onRightClick = [this](const juce::MouseEvent&) { showTrackContextMenu(); };
        addAndMakeVisible(headerBtn);

        ioBtn.setTooltip("Left or Right-click: Assign Input / Output, Channel DSP, Duplicate, Remove");
        ioBtn.onClick = [this] { showTrackContextMenu(); };
        ioBtn.onRightClick = [this](const juce::MouseEvent&) { showTrackContextMenu(); };
        addAndMakeVisible(ioBtn);

        for (int s = 0; s < InsertChain::numSlots; ++s)
        {
            auto& btn = insertButtons[static_cast<size_t>(s)];
            btn.setTooltip("Left-click: Open UI / Load | Right-click: Insert Menu & Copy/Paste Chain");
            btn.onClick = [this, s] {
                if (track->inserts.hasPlugin(s))
                    track->inserts.openWindow(s, track->name);
                else
                    showInsertSlotMenu(engine, track->inserts, s, track->name,
                                       &insertButtons[static_cast<size_t>(s)], [this] { refresh(); });
            };
            btn.onRightClick = [this, s](const juce::MouseEvent&) {
                showInsertSlotMenu(engine, track->inserts, s, track->name,
                                   &insertButtons[static_cast<size_t>(s)], [this] { refresh(); });
            };
            addAndMakeVisible(btn);
        }

        setupSendKnob(sendAKnob, "FX A", OctoColours::accentBlue, true);
        setupSendKnob(sendBKnob, "FX B", OctoColours::accentPurp, false);

        armBtn.setButtonText("R");
        armBtn.setClickingTogglesState(true);
        armBtn.setColour(juce::TextButton::buttonOnColourId, OctoColours::accentRed);
        armBtn.onClick = [this] { track->armed.store(armBtn.getToggleState()); };
        armBtn.onRightClick = [this](const juce::MouseEvent&) { showTrackContextMenu(); };
        addAndMakeVisible(armBtn);

        monBtn.setButtonText("I");
        monBtn.setTooltip("Live Input Monitoring");
        monBtn.setClickingTogglesState(true);
        monBtn.setColour(juce::TextButton::buttonOnColourId, OctoColours::accentTeal);
        monBtn.onClick = [this] { track->inputMonitor.store(monBtn.getToggleState()); };
        monBtn.onRightClick = [this](const juce::MouseEvent&) { showTrackContextMenu(); };
        addAndMakeVisible(monBtn);

        muteBtn.setButtonText("M");
        muteBtn.setClickingTogglesState(true);
        muteBtn.setColour(juce::TextButton::buttonOnColourId, OctoColours::accentBlue);
        muteBtn.onClick = [this] { track->muted.store(muteBtn.getToggleState()); };
        muteBtn.onRightClick = [this](const juce::MouseEvent&) { showTrackContextMenu(); };
        addAndMakeVisible(muteBtn);

        soloBtn.setButtonText("S");
        soloBtn.setClickingTogglesState(true);
        soloBtn.setColour(juce::TextButton::buttonOnColourId, OctoColours::accentAmber);
        soloBtn.onClick = [this] { track->soloed.store(soloBtn.getToggleState()); };
        soloBtn.onRightClick = [this](const juce::MouseEvent&) { showTrackContextMenu(); };
        addAndMakeVisible(soloBtn);

        panKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        panKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        panKnob.setRange(-1.0, 1.0, 0.01);
        panKnob.setDoubleClickReturnValue(true, 0.0);
        panKnob.setColour(juce::Slider::rotarySliderFillColourId, OctoColours::accentTeal);
        panKnob.onValueChange = [this] { track->pan.store(static_cast<float>(panKnob.getValue())); };
        panKnob.onRightClick = [this](const juce::MouseEvent&) { showPanMenu(); };
        addAndMakeVisible(panKnob);

        volFader.setSliderStyle(juce::Slider::LinearVertical);
        volFader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        volFader.setRange(0.0, 1.5, 0.005);
        volFader.setDoubleClickReturnValue(true, 1.0);
        volFader.onValueChange = [this] { track->gain.store(static_cast<float>(volFader.getValue())); };
        volFader.onRightClick = [this](const juce::MouseEvent&) { showTrackContextMenu(); };
        addAndMakeVisible(volFader);

        addAndMakeVisible(meter);
        refresh();
    }

    void setSelected(bool sel) { isSelected = sel; repaint(); }
    int getTrackIndex() const { return trackIndex; }

    void refresh()
    {
        if (track == nullptr) return;

        headerBtn.setButtonText(track->name);
        headerBtn.setColour(juce::TextButton::buttonColourId, track->trackColour.withAlpha(isSelected ? 0.45f : 0.25f));

        ioBtn.setButtonText(track->getInputLabel() + " > " + track->getOutputLabel());
        ioBtn.setColour(juce::TextButton::buttonColourId, OctoColours::slotBg);

        for (int s = 0; s < InsertChain::numSlots; ++s)
        {
            auto& btn = insertButtons[static_cast<size_t>(s)];
            const bool loaded = track->inserts.hasPlugin(s);
            const bool byp = track->inserts.isBypassed(s);
            btn.setButtonText(loaded ? track->inserts.getSlotName(s) : ("+ Ins " + juce::String(s + 1)));
            btn.setColour(juce::TextButton::buttonColourId,
                          !loaded ? OctoColours::slotBg : (byp ? OctoColours::slotBypass : OctoColours::slotActive));
            btn.setColour(juce::TextButton::textColourOffId,
                          !loaded ? OctoColours::textDim.withAlpha(0.65f)
                                  : (byp ? OctoColours::accentAmber : OctoColours::textMain));
        }

        const bool isAudio = (track->mode.load() == TrackMode::Audio);
        monBtn.setEnabled(isAudio);

        armBtn.setToggleState(track->armed.load(), juce::dontSendNotification);
        monBtn.setToggleState(track->inputMonitor.load(), juce::dontSendNotification);
        muteBtn.setToggleState(track->muted.load(), juce::dontSendNotification);
        soloBtn.setToggleState(track->soloed.load(), juce::dontSendNotification);

        if (!sendAKnob.isMouseButtonDown()) sendAKnob.setValue(track->sendAGain.load(), juce::dontSendNotification);
        if (!sendBKnob.isMouseButtonDown()) sendBKnob.setValue(track->sendBGain.load(), juce::dontSendNotification);
        if (!panKnob.isMouseButtonDown())   panKnob.setValue(track->pan.load(), juce::dontSendNotification);
        if (!volFader.isMouseButtonDown())  volFader.setValue(track->gain.load(), juce::dontSendNotification);

        meter.setLevels(track->peakL.load(std::memory_order_relaxed),
                        track->peakR.load(std::memory_order_relaxed));
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced(1.5f);
        g.setColour(isSelected ? OctoColours::stripSel : OctoColours::stripBg);
        g.fillRoundedRectangle(b, 5.0f);

        g.setColour(track->trackColour);
        g.fillRoundedRectangle(b.getX() + 1.0f, b.getY() + 1.0f, b.getWidth() - 2.0f, 3.0f, 1.5f);

        g.setColour(isSelected ? track->trackColour.withAlpha(0.85f) : OctoColours::border.withAlpha(0.7f));
        g.drawRoundedRectangle(b, 5.0f, isSelected ? 1.4f : 1.0f);

        g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
        g.setColour(OctoColours::textDim);

        auto sendALabel = sendAKnob.getBounds().withY(sendAKnob.getBottom() - 2).withHeight(11);
        g.drawText(track->sendAPreFader.load() ? "A PRE" : "FX A", sendALabel, juce::Justification::centred);

        auto sendBLabel = sendBKnob.getBounds().withY(sendBKnob.getBottom() - 2).withHeight(11);
        g.drawText(track->sendBPreFader.load() ? "B PRE" : "FX B", sendBLabel, juce::Justification::centred);

        juce::String flags;
        if (track->phaseInvert.load()) flags << juce::String::charToString(0x00D8) << " ";
        const int hpf = track->hpfMode.load();
        if (hpf == 1) flags << "35Hz ";
        else if (hpf == 2) flags << "80Hz ";
        else if (hpf == 3) flags << "120Hz ";
        const int gate = track->gateMode.load();
        if (gate == 1) flags << "GT ";
        else if (gate == 2) flags << "MTLG ";
        const int drv = track->driveMode.load();
        if (drv == 1) flags << "WRM ";
        else if (drv == 2) flags << "GRT ";

        if (flags.isNotEmpty())
        {
            g.setColour(OctoColours::accentAmber);
            g.drawText(flags.trim(), dspBadgeArea, juce::Justification::centred);
        }
        else
        {
            g.setColour(OctoColours::textDim.withAlpha(0.45f));
            g.drawText(track->mode.load() == TrackMode::Audio ? "AUDIO" : "MIDI",
                       dspBadgeArea, juce::Justification::centred);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            showTrackContextMenu();
            return;
        }
        if (onSelectTrack)
            onSelectTrack(trackIndex, e.getNumberOfClicks() >= 2);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(5, 6);
        area.removeFromTop(3);

        headerBtn.setBounds(area.removeFromTop(21));
        area.removeFromTop(3);
        ioBtn.setBounds(area.removeFromTop(18));
        dspBadgeArea = area.removeFromTop(13);
        area.removeFromTop(2);

        for (auto& btn : insertButtons)
        {
            btn.setBounds(area.removeFromTop(19));
            area.removeFromTop(2);
        }
        area.removeFromTop(3);

        auto sendRow = area.removeFromTop(32);
        const int half = sendRow.getWidth() / 2;
        sendAKnob.setBounds(sendRow.removeFromLeft(half).reduced(2, 0));
        sendBKnob.setBounds(sendRow.reduced(2, 0));
        area.removeFromTop(10);

        auto flagsRow = area.removeFromTop(20);
        const int qw = flagsRow.getWidth() / 4;
        armBtn.setBounds(flagsRow.removeFromLeft(qw).reduced(1, 0));
        monBtn.setBounds(flagsRow.removeFromLeft(qw).reduced(1, 0));
        muteBtn.setBounds(flagsRow.removeFromLeft(qw).reduced(1, 0));
        soloBtn.setBounds(flagsRow.reduced(1, 0));
        area.removeFromTop(4);

        auto panRow = area.removeFromTop(26);
        panKnob.setBounds(panRow.withSizeKeepingCentre(26, 26));
        area.removeFromTop(3);

        auto faderRow = area;
        meter.setBounds(faderRow.removeFromRight(12).reduced(0, 4));
        faderRow.removeFromRight(4);
        volFader.setBounds(faderRow);
    }

    std::function<void(int trackIdx, bool openEditor)> onSelectTrack;

private:
    void setupSendKnob(ContextSlider& knob, const juce::String& tip, juce::Colour accent, bool isA)
    {
        knob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        knob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        knob.setRange(0.0, 1.0, 0.01);
        knob.setDoubleClickReturnValue(true, 0.0);
        knob.setTooltip(tip + " Send (Right-click for Pre/Post Fader)");
        knob.setColour(juce::Slider::rotarySliderFillColourId, accent);
        knob.onValueChange = [this, &knob, isA] {
            if (isA) track->sendAGain.store(static_cast<float>(knob.getValue()));
            else     track->sendBGain.store(static_cast<float>(knob.getValue()));
        };
        knob.onRightClick = [this, isA](const juce::MouseEvent&) { showSendMenu(isA); };
        addAndMakeVisible(knob);
    }

    void showSendMenu(bool isA)
    {
        const bool isPre = isA ? track->sendAPreFader.load() : track->sendBPreFader.load();
        juce::PopupMenu m;
        m.addSectionHeader(isA ? "Send to FX Bus A" : "Send to FX Bus B");
        m.addItem(1, "Post-Fader", true, !isPre);
        m.addItem(2, "Pre-Fader", true, isPre);
        m.addSeparator();
        m.addItem(3, "Set Unity (100%)");
        m.addItem(4, "Set 50%");
        m.addItem(5, "Reset (-inf)");

        m.showMenuAsync(juce::PopupMenu::Options(), [this, isA](int r) {
            if (r == 1)      (isA ? track->sendAPreFader : track->sendBPreFader).store(false);
            else if (r == 2) (isA ? track->sendAPreFader : track->sendBPreFader).store(true);
            else if (r == 3) (isA ? track->sendAGain : track->sendBGain).store(1.0f);
            else if (r == 4) (isA ? track->sendAGain : track->sendBGain).store(0.5f);
            else if (r == 5) (isA ? track->sendAGain : track->sendBGain).store(0.0f);
            refresh();
        });
    }

    void showPanMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader("Track Pan");
        m.addItem(1, "Hard Left (-100%)");
        m.addItem(2, "Guitar Left (-75%)");
        m.addItem(3, "Center (0%)");
        m.addItem(4, "Guitar Right (+75%)");
        m.addItem(5, "Hard Right (+100%)");
        m.showMenuAsync(juce::PopupMenu::Options(), [this](int r) {
            if (r == 1) track->pan.store(-1.0f);
            else if (r == 2) track->pan.store(-0.75f);
            else if (r == 3) track->pan.store(0.0f);
            else if (r == 4) track->pan.store(0.75f);
            else if (r == 5) track->pan.store(1.0f);
            refresh();
        });
    }

    void showTrackContextMenu()
    {
        juce::PopupMenu menu;
        const bool isAudio = (track->mode.load() == TrackMode::Audio);
        menu.addSectionHeader(track->name + "  (" + (isAudio ? "Audio" : "MIDI") + ")");

        menu.addItem(50, isAudio ? "Open in Audio Editor" : "Open in Piano Roll");
        menu.addSeparator();

        // --- INPUT ROUTING ---
        juce::PopupMenu inMenu;
        const auto curInType = static_cast<InputSourceType>(track->inputSourceType.load());
        const int curInCh = track->inputChannel.load();
        const auto hwIns = engine.getHardwareInputNames();

        juce::PopupMenu monoInMenu, stereoInMenu, midiInMenu;
        for (int i = 0; i < hwIns.size(); ++i)
            monoInMenu.addItem(100 + i, "Mono " + juce::String(i + 1) + " (" + hwIns[i] + ")",
                               true, curInType == InputSourceType::MonoHW && curInCh == i);
        for (int i = 0; i + 1 < hwIns.size(); i += 2)
            stereoInMenu.addItem(200 + i, "Stereo " + juce::String(i + 1) + "-" + juce::String(i + 2),
                                 true, curInType == InputSourceType::StereoHW && curInCh == i);
        midiInMenu.addItem(300, "All MIDI Channels", true, curInType == InputSourceType::MidiAll);
        for (int ch = 1; ch <= 16; ++ch)
            midiInMenu.addItem(300 + ch, "MIDI Channel " + juce::String(ch),
                               true, curInType == InputSourceType::MidiChannel && curInCh == ch);

        inMenu.addSubMenu("Hardware Mono Input", monoInMenu);
        if (stereoInMenu.getNumItems() > 0) inMenu.addSubMenu("Hardware Stereo Pair", stereoInMenu);
        inMenu.addSeparator();
        inMenu.addSubMenu("MIDI Input", midiInMenu);
        inMenu.addItem(350, "Built-in Drum Sequencer", true, curInType == InputSourceType::DrumSeq);
        inMenu.addItem(351, "No Input", true, curInType == InputSourceType::None);
        menu.addSubMenu("Assign Input", inMenu);

        // --- OUTPUT ROUTING ---
        juce::PopupMenu outMenu;
        const auto curOutType = static_cast<OutputDestType>(track->outputDestType.load());
        const int curOutCh = track->outputHwChannel.load();
        const auto hwOuts = engine.getHardwareOutputNames();

        outMenu.addItem(400, "Master Bus", true, curOutType == OutputDestType::Master);
        outMenu.addItem(401, "FX Send Bus A (Direct)", true, curOutType == OutputDestType::FxBusA);
        outMenu.addItem(402, "FX Send Bus B (Direct)", true, curOutType == OutputDestType::FxBusB);
        outMenu.addSeparator();

        juce::PopupMenu dirStereo, dirMono;
        for (int i = 0; i + 1 < hwOuts.size(); i += 2)
            dirStereo.addItem(500 + i, "Hardware Out " + juce::String(i + 1) + "-" + juce::String(i + 2),
                              true, curOutType == OutputDestType::DirectStereoHW && curOutCh == i);
        for (int i = 0; i < hwOuts.size(); ++i)
            dirMono.addItem(600 + i, "Hardware Mono Out " + juce::String(i + 1) + " (" + hwOuts[i] + ")",
                            true, curOutType == OutputDestType::DirectMonoHW && curOutCh == i);
        if (dirStereo.getNumItems() > 0) outMenu.addSubMenu("Direct Hardware Stereo Out", dirStereo);
        outMenu.addSubMenu("Direct Hardware Mono Out (Re-Amp)", dirMono);
        menu.addSubMenu("Assign Output", outMenu);

        menu.addSeparator();

        // --- CHANNEL INPUT DSP ---
        juce::PopupMenu dspMenu;
        dspMenu.addItem(700, juce::String("Phase Invert (") + juce::String::charToString(0x00D8) + " Polarity Flip)", true, track->phaseInvert.load());
        dspMenu.addSeparator();
        const int curHpf = track->hpfMode.load();
        dspMenu.addItem(710, "HPF: Off", true, curHpf == 0);
        dspMenu.addItem(711, "HPF: 35 Hz (Bass Sub Tightener)", true, curHpf == 1);
        dspMenu.addItem(712, "HPF: 80 Hz (Guitar Rhythm Clean)", true, curHpf == 2);
        dspMenu.addItem(713, "HPF: 120 Hz (Guitar Lead / Vox)", true, curHpf == 3);
        dspMenu.addSeparator();
        const int curGate = track->gateMode.load();
        dspMenu.addItem(720, "Noise Gate: Off", true, curGate == 0);
        dspMenu.addItem(721, "Noise Gate: Tight (Low-Gain / Bass)", true, curGate == 1);
        dspMenu.addItem(722, "Noise Gate: Metal (High-Gain Clamp)", true, curGate == 2);
        dspMenu.addSeparator();
        const int curDrv = track->driveMode.load();
        dspMenu.addItem(740, "Console Drive: Off", true, curDrv == 0);
        dspMenu.addItem(741, "Console Drive: Warm Tape", true, curDrv == 1);
        dspMenu.addItem(742, "Console Drive: Analog Grit", true, curDrv == 2);
        dspMenu.addSeparator();
        const float trim = track->inputTrimDb.load();
        dspMenu.addItem(730, "Input Trim: -6 dB", true, std::abs(trim + 6.0f) < 0.1f);
        dspMenu.addItem(731, "Input Trim: 0 dB", true, std::abs(trim) < 0.1f);
        dspMenu.addItem(732, "Input Trim: +6 dB", true, std::abs(trim - 6.0f) < 0.1f);
        dspMenu.addItem(733, "Input Trim: +12 dB (Passive Pickup Boost)", true, std::abs(trim - 12.0f) < 0.1f);
        menu.addSubMenu("Channel Strip DSP (Phase / HPF / Gate / Drive)", dspMenu);

        // --- TRACK MODE & BUILT-IN SYNTH ---
        juce::PopupMenu modeMenu;
        modeMenu.addItem(800, "Audio Track", true, isAudio);
        modeMenu.addItem(801, "MIDI Instrument Track", true, !isAudio);
        modeMenu.addSeparator();
        modeMenu.addItem(802, "Built-in Rock Drum Synth (no VSTi)", true, track->useInternalDrumSynth.load());
        modeMenu.addItem(803, "Built-in Poly Synth: Analog Sub", true, !track->useInternalDrumSynth.load() && track->fallbackSynthPatch.load() == 0);
        modeMenu.addItem(804, "Built-in Poly Synth: Saw Lead", true, !track->useInternalDrumSynth.load() && track->fallbackSynthPatch.load() == 1);
        modeMenu.addItem(805, "Built-in Poly Synth: Warm Pad", true, !track->useInternalDrumSynth.load() && track->fallbackSynthPatch.load() == 2);
        modeMenu.addItem(806, "Built-in Poly Synth: Pluck Bass", true, !track->useInternalDrumSynth.load() && track->fallbackSynthPatch.load() == 3);
        menu.addSubMenu("Track Type & Internal Synth", modeMenu);

        // --- INSERT CHAIN & CLIPBOARD ---
        menu.addItem(850, "Copy Track 4-Insert Chain");
        menu.addItem(851, "Paste 4-Insert Chain", engine.hasCopiedInsertChain());

        // --- COLOUR & RENAME ---
        menu.addItem(810, "Rename Track...");
        juce::PopupMenu colMenu;
        static const std::pair<const char*, uint32_t> palette[7] = {
            { "Teal (Bass)", 0xff1abc9c }, { "Blue (Guitar)", 0xff3498db },
            { "Orange (Drums)", 0xffe67e22 }, { "Crimson (Lead)", 0xffe74c3c },
            { "Emerald (Clean)", 0xff2ecc71 }, { "Purple (Synth)", 0xff9b59b6 },
            { "Gold (Vox/FX)", 0xfff1c40f }
        };
        for (int c = 0; c < 7; ++c)
            colMenu.addItem(820 + c, palette[c].first);
        menu.addSubMenu("Track Colour", colMenu);

        menu.addSeparator();
        menu.addItem(900, "Import WAV Clip...");
        menu.addItem(901, "Export Track Audio WAV...", track->getClipLengthSamples() > 0 && isAudio);
        menu.addItem(904, "Export Track MIDI (.mid)...", !isAudio && track->getClipLengthSamples() > 0);
        menu.addItem(902, "Clear Recorded Clip");
        menu.addItem(903, "Clear All 4 Inserts");
        menu.addSeparator();
        menu.addItem(950, "Duplicate Track", engine.getNumTracks() < AudioEngine::maxTracks);
        menu.addItem(951, "Remove Track", engine.getNumTracks() > 1);

        juce::Component::SafePointer<TrackStripUI> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options(), [safeThis](int r) { if (safeThis != nullptr) safeThis->handleTrackMenuResult(r); });
    }

    void handleTrackMenuResult(int r)
    {
        if (r == 50)
        {
            if (onSelectTrack) onSelectTrack(trackIndex, true);
            return;
        }
        else if (r >= 100 && r < 200)
        {
            track->mode.store(TrackMode::Audio);
            track->inputSourceType.store(static_cast<int>(InputSourceType::MonoHW));
            track->inputChannel.store(r - 100);
        }
        else if (r >= 200 && r < 300)
        {
            track->mode.store(TrackMode::Audio);
            track->inputSourceType.store(static_cast<int>(InputSourceType::StereoHW));
            track->inputChannel.store(r - 200);
        }
        else if (r == 300)
        {
            track->mode.store(TrackMode::Midi);
            track->inputSourceType.store(static_cast<int>(InputSourceType::MidiAll));
        }
        else if (r > 300 && r <= 316)
        {
            track->mode.store(TrackMode::Midi);
            track->inputSourceType.store(static_cast<int>(InputSourceType::MidiChannel));
            track->inputChannel.store(r - 300);
        }
        else if (r == 350)
        {
            track->mode.store(TrackMode::Midi);
            track->inputSourceType.store(static_cast<int>(InputSourceType::DrumSeq));
        }
        else if (r == 351) track->inputSourceType.store(static_cast<int>(InputSourceType::None));
        else if (r == 400) track->outputDestType.store(static_cast<int>(OutputDestType::Master));
        else if (r == 401) track->outputDestType.store(static_cast<int>(OutputDestType::FxBusA));
        else if (r == 402) track->outputDestType.store(static_cast<int>(OutputDestType::FxBusB));
        else if (r >= 500 && r < 600)
        {
            track->outputDestType.store(static_cast<int>(OutputDestType::DirectStereoHW));
            track->outputHwChannel.store(r - 500);
        }
        else if (r >= 600 && r < 700)
        {
            track->outputDestType.store(static_cast<int>(OutputDestType::DirectMonoHW));
            track->outputHwChannel.store(r - 600);
        }
        else if (r == 700) track->phaseInvert.store(!track->phaseInvert.load());
        else if (r >= 710 && r <= 713) track->hpfMode.store(r - 710);
        else if (r >= 720 && r <= 722) track->gateMode.store(r - 720);
        else if (r >= 740 && r <= 742) track->driveMode.store(r - 740);
        else if (r == 730) track->inputTrimDb.store(-6.0f);
        else if (r == 731) track->inputTrimDb.store(0.0f);
        else if (r == 732) track->inputTrimDb.store(6.0f);
        else if (r == 733) track->inputTrimDb.store(12.0f);
        else if (r == 800) track->mode.store(TrackMode::Audio);
        else if (r == 801) track->mode.store(TrackMode::Midi);
        else if (r == 802)
        {
            track->mode.store(TrackMode::Midi);
            track->useInternalDrumSynth.store(!track->useInternalDrumSynth.load());
        }
        else if (r >= 803 && r <= 806)
        {
            track->mode.store(TrackMode::Midi);
            track->useInternalDrumSynth.store(false);
            track->useFallbackPolySynth.store(true);
            track->fallbackSynthPatch.store(r - 803);
        }
        else if (r == 850) engine.copyInsertChain(track->inserts);
        else if (r == 851) engine.pasteInsertChain(track->inserts);
        else if (r == 810) promptRename();
        else if (r >= 820 && r < 827)
        {
            static const uint32_t cols[7] = { 0xff1abc9c, 0xff3498db, 0xffe67e22, 0xffe74c3c, 0xff2ecc71, 0xff9b59b6, 0xfff1c40f };
            track->trackColour = juce::Colour(cols[r - 820]);
        }
        else if (r == 900)
        {
            fileChooser = std::make_unique<juce::FileChooser>("Import WAV into " + track->name, juce::File(), "*.wav");
            fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                     [this](const juce::FileChooser& fc) {
                                         auto f = fc.getResult();
                                         if (f.existsAsFile()) track->importWavFile(f);
                                         refresh();
                                     });
        }
        else if (r == 901)
        {
            fileChooser = std::make_unique<juce::FileChooser>(
                "Export " + track->name + " WAV",
                juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile(track->name + ".wav"),
                "*.wav");
            fileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                     [this](const juce::FileChooser& fc) {
                                         auto f = fc.getResult();
                                         if (f != juce::File()) track->exportClipWav(f, engine.currentSampleRate.load());
                                     });
        }
        else if (r == 904)
        {
            auto notes = track->getMidiNotes(engine.bpm.load(), engine.currentSampleRate.load());
            fileChooser = std::make_unique<juce::FileChooser>(
                "Export " + track->name + " MIDI",
                juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile(track->name + ".mid"),
                "*.mid");
            fileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                     [this, notes](const juce::FileChooser& fc) {
                                         auto f = fc.getResult();
                                         if (f != juce::File()) MidiHelpers::exportMidiFile(f, notes, engine.bpm.load());
                                     });
        }
        else if (r == 902) track->clearClip();
        else if (r == 903) track->inserts.clearAll();
        else if (r == 950)
        {
            auto* engPtr = &engine;
            const int idx = trackIndex;
            juce::MessageManager::callAsync([engPtr, idx] { engPtr->duplicateTrack(idx); });
            return;
        }
        else if (r == 951)
        {
            auto* engPtr = &engine;
            const int idx = trackIndex;
            juce::MessageManager::callAsync([engPtr, idx] { engPtr->removeTrack(idx); });
            return;
        }

        refresh();
    }

    void promptRename()
    {
        auto* aw = new juce::AlertWindow("Rename Track", "Enter new name for " + track->name + ":",
                                         juce::MessageBoxIconType::NoIcon);
        aw->addTextEditor("name", track->name);
        aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        auto trkPtr = track;
        juce::Component::SafePointer<TrackStripUI> safeThis(this);
        aw->enterModalState(true, juce::ModalCallbackFunction::create([safeThis, trkPtr, aw](int res) {
            if (res == 1 && trkPtr != nullptr)
            {
                const auto txt = aw->getTextEditorContents("name").trim();
                if (txt.isNotEmpty())
                    trkPtr->name = txt.substring(0, 14);
                if (safeThis != nullptr)
                    safeThis->refresh();
            }
        }), true);
    }

    AudioEngine& engine;
    std::shared_ptr<Track> track;
    int trackIndex;
    bool isSelected = false;

    ContextButton headerBtn, ioBtn;
    std::array<ContextButton, InsertChain::numSlots> insertButtons;
    ContextSlider sendAKnob, sendBKnob, panKnob, volFader;
    ContextButton armBtn, monBtn, muteBtn, soloBtn;
    StereoMeter meter;
    juce::Rectangle<int> dspBadgeArea;
    std::unique_ptr<juce::FileChooser> fileChooser;
};

class FXBusStripUI : public juce::Component
{
public:
    FXBusStripUI(AudioEngine& eng, FXBus& b, juce::Colour accentCol)
        : engine(eng), bus(b), accent(accentCol)
    {
        headerBtn.setButtonText(bus.name);
        headerBtn.onClick = [this] { showBusMenu(); };
        headerBtn.onRightClick = [this](const juce::MouseEvent&) { showBusMenu(); };
        addAndMakeVisible(headerBtn);

        outBtn.onClick = [this] { showBusMenu(); };
        outBtn.onRightClick = [this](const juce::MouseEvent&) { showBusMenu(); };
        addAndMakeVisible(outBtn);

        for (int s = 0; s < InsertChain::numSlots; ++s)
        {
            auto& btn = insertButtons[static_cast<size_t>(s)];
            btn.onClick = [this, s] {
                if (bus.inserts.hasPlugin(s))
                    bus.inserts.openWindow(s, bus.name);
                else
                    showInsertSlotMenu(engine, bus.inserts, s, bus.name,
                                       &insertButtons[static_cast<size_t>(s)], [this] { refresh(); });
            };
            btn.onRightClick = [this, s](const juce::MouseEvent&) {
                showInsertSlotMenu(engine, bus.inserts, s, bus.name,
                                   &insertButtons[static_cast<size_t>(s)], [this] { refresh(); });
            };
            addAndMakeVisible(btn);
        }

        muteBtn.setButtonText("M");
        muteBtn.setClickingTogglesState(true);
        muteBtn.setColour(juce::TextButton::buttonOnColourId, OctoColours::accentBlue);
        muteBtn.onClick = [this] { bus.muted.store(muteBtn.getToggleState()); };
        addAndMakeVisible(muteBtn);

        soloBtn.setButtonText("S");
        soloBtn.setClickingTogglesState(true);
        soloBtn.setColour(juce::TextButton::buttonOnColourId, OctoColours::accentAmber);
        soloBtn.onClick = [this] { bus.soloed.store(soloBtn.getToggleState()); };
        addAndMakeVisible(soloBtn);

        panKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        panKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        panKnob.setRange(-1.0, 1.0, 0.01);
        panKnob.setDoubleClickReturnValue(true, 0.0);
        panKnob.setColour(juce::Slider::rotarySliderFillColourId, accent);
        panKnob.onValueChange = [this] { bus.pan.store(static_cast<float>(panKnob.getValue())); };
        addAndMakeVisible(panKnob);

        volFader.setSliderStyle(juce::Slider::LinearVertical);
        volFader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        volFader.setRange(0.0, 1.5, 0.005);
        volFader.setDoubleClickReturnValue(true, 1.0);
        volFader.onValueChange = [this] { bus.gain.store(static_cast<float>(volFader.getValue())); };
        addAndMakeVisible(volFader);

        addAndMakeVisible(meter);
        refresh();
    }

    void refresh()
    {
        headerBtn.setButtonText(bus.name);
        headerBtn.setColour(juce::TextButton::buttonColourId, accent.withAlpha(0.32f));
        outBtn.setButtonText("OUT > " + bus.getOutputLabel());
        outBtn.setColour(juce::TextButton::buttonColourId, OctoColours::slotBg);

        for (int s = 0; s < InsertChain::numSlots; ++s)
        {
            auto& btn = insertButtons[static_cast<size_t>(s)];
            const bool loaded = bus.inserts.hasPlugin(s);
            const bool byp = bus.inserts.isBypassed(s);
            btn.setButtonText(loaded ? bus.inserts.getSlotName(s) : ("+ FX " + juce::String(s + 1)));
            btn.setColour(juce::TextButton::buttonColourId,
                          !loaded ? OctoColours::slotBg : (byp ? OctoColours::slotBypass : OctoColours::slotActive));
            btn.setColour(juce::TextButton::textColourOffId,
                          !loaded ? OctoColours::textDim.withAlpha(0.65f)
                                  : (byp ? OctoColours::accentAmber : OctoColours::textMain));
        }

        muteBtn.setToggleState(bus.muted.load(), juce::dontSendNotification);
        soloBtn.setToggleState(bus.soloed.load(), juce::dontSendNotification);
        if (!panKnob.isMouseButtonDown()) panKnob.setValue(bus.pan.load(), juce::dontSendNotification);
        if (!volFader.isMouseButtonDown()) volFader.setValue(bus.gain.load(), juce::dontSendNotification);

        meter.setLevels(bus.peakL.load(std::memory_order_relaxed),
                        bus.peakR.load(std::memory_order_relaxed));
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced(1.5f);
        g.setColour(OctoColours::stripAlt);
        g.fillRoundedRectangle(b, 5.0f);
        g.setColour(accent);
        g.fillRoundedRectangle(b.getX() + 1.0f, b.getY() + 1.0f, b.getWidth() - 2.0f, 3.0f, 1.5f);
        g.setColour(OctoColours::border);
        g.drawRoundedRectangle(b, 5.0f, 1.0f);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            showBusMenu();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(5, 6);
        area.removeFromTop(3);
        headerBtn.setBounds(area.removeFromTop(21));
        area.removeFromTop(3);
        outBtn.setBounds(area.removeFromTop(18));
        area.removeFromTop(15);

        for (auto& btn : insertButtons)
        {
            btn.setBounds(area.removeFromTop(19));
            area.removeFromTop(2);
        }
        area.removeFromTop(45);

        auto flags = area.removeFromTop(20);
        muteBtn.setBounds(flags.removeFromLeft(flags.getWidth() / 2).reduced(1, 0));
        soloBtn.setBounds(flags.reduced(1, 0));
        area.removeFromTop(4);

        auto panRow = area.removeFromTop(26);
        panKnob.setBounds(panRow.withSizeKeepingCentre(26, 26));
        area.removeFromTop(3);

        auto faderRow = area;
        meter.setBounds(faderRow.removeFromRight(12).reduced(0, 4));
        faderRow.removeFromRight(4);
        volFader.setBounds(faderRow);
    }

private:
    void showBusMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader(bus.name + " Output & Inserts");
        const auto curOut = static_cast<OutputDestType>(bus.outputDestType.load());
        const int curCh = bus.outputHwChannel.load();
        const auto hwOuts = engine.getHardwareOutputNames();

        m.addItem(1, "Master Bus", true, curOut == OutputDestType::Master);
        for (int i = 0; i + 1 < hwOuts.size(); i += 2)
            m.addItem(100 + i, "Direct Hardware Out " + juce::String(i + 1) + "-" + juce::String(i + 2),
                      true, curOut == OutputDestType::DirectStereoHW && curCh == i);
        m.addSeparator();
        m.addItem(50, "Copy Bus 4-Insert Chain");
        m.addItem(51, "Paste 4-Insert Chain", engine.hasCopiedInsertChain());
        m.addItem(99, "Clear All 4 Bus Inserts");

        m.showMenuAsync(juce::PopupMenu::Options(), [this](int r) {
            if (r == 1) bus.outputDestType.store(static_cast<int>(OutputDestType::Master));
            else if (r >= 100)
            {
                bus.outputDestType.store(static_cast<int>(OutputDestType::DirectStereoHW));
                bus.outputHwChannel.store(r - 100);
            }
            else if (r == 50) engine.copyInsertChain(bus.inserts);
            else if (r == 51) engine.pasteInsertChain(bus.inserts);
            else if (r == 99) bus.inserts.clearAll();
            refresh();
        });
    }

    AudioEngine& engine;
    FXBus& bus;
    juce::Colour accent;
    ContextButton headerBtn, outBtn;
    std::array<ContextButton, InsertChain::numSlots> insertButtons;
    ContextButton muteBtn, soloBtn;
    ContextSlider panKnob, volFader;
    StereoMeter meter;
};

class MasterStripUI : public juce::Component
{
public:
    explicit MasterStripUI(AudioEngine& eng) : engine(eng)
    {
        headerLabel.setText("MASTER", juce::dontSendNotification);
        headerLabel.setFont(juce::FontOptions(12.0f, juce::Font::bold));
        headerLabel.setJustificationType(juce::Justification::centred);
        addAndMakeVisible(headerLabel);

        clipBtn.setButtonText("CLIP");
        clipBtn.setTooltip("Master Clip Indicator (Click to reset)");
        clipBtn.onClick = [this] { engine.masterClipped.store(false); refresh(); };
        addAndMakeVisible(clipBtn);

        for (int s = 0; s < InsertChain::numSlots; ++s)
        {
            auto& btn = insertButtons[static_cast<size_t>(s)];
            btn.onClick = [this, s] {
                if (engine.masterInserts.hasPlugin(s))
                    engine.masterInserts.openWindow(s, "MASTER");
                else
                    showInsertSlotMenu(engine, engine.masterInserts, s, "MASTER",
                                       &insertButtons[static_cast<size_t>(s)], [this] { refresh(); });
            };
            btn.onRightClick = [this, s](const juce::MouseEvent&) {
                showInsertSlotMenu(engine, engine.masterInserts, s, "MASTER",
                                   &insertButtons[static_cast<size_t>(s)], [this] { refresh(); });
            };
            addAndMakeVisible(btn);
        }

        monoBtn.setButtonText("MONO");
        monoBtn.setTooltip("Sum Master to Mono (phase check for bass & double-tracked guitars)");
        monoBtn.setClickingTogglesState(true);
        monoBtn.setColour(juce::TextButton::buttonOnColourId, OctoColours::accentAmber);
        monoBtn.onClick = [this] { engine.masterMonoCheck.store(monoBtn.getToggleState()); };
        addAndMakeVisible(monoBtn);

        limBtn.setButtonText("LIM");
        limBtn.setTooltip("Master Bus Safety Soft-Limiter");
        limBtn.setClickingTogglesState(true);
        limBtn.setColour(juce::TextButton::buttonOnColourId, OctoColours::accentTeal);
        limBtn.onClick = [this] { engine.masterLimiterEnabled.store(limBtn.getToggleState()); };
        addAndMakeVisible(limBtn);

        dimBtn.setButtonText("DIM");
        dimBtn.setTooltip("Dim Monitor Output (-12 dB)");
        dimBtn.setClickingTogglesState(true);
        dimBtn.setColour(juce::TextButton::buttonOnColourId, OctoColours::accentBlue);
        dimBtn.onClick = [this] { engine.masterDim.store(dimBtn.getToggleState()); };
        addAndMakeVisible(dimBtn);

        volFader.setSliderStyle(juce::Slider::LinearVertical);
        volFader.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        volFader.setRange(0.0, 1.5, 0.005);
        volFader.setDoubleClickReturnValue(true, 1.0);
        volFader.onValueChange = [this] { engine.masterGain.store(static_cast<float>(volFader.getValue())); };
        addAndMakeVisible(volFader);

        addAndMakeVisible(meter);
        refresh();
    }

    void refresh()
    {
        const bool clipped = engine.masterClipped.load(std::memory_order_relaxed);
        clipBtn.setColour(juce::TextButton::buttonColourId,
                          clipped ? OctoColours::accentRed : OctoColours::slotBg);
        clipBtn.setColour(juce::TextButton::textColourOffId,
                          clipped ? juce::Colours::white : OctoColours::textDim.withAlpha(0.5f));

        for (int s = 0; s < InsertChain::numSlots; ++s)
        {
            auto& btn = insertButtons[static_cast<size_t>(s)];
            const bool loaded = engine.masterInserts.hasPlugin(s);
            const bool byp = engine.masterInserts.isBypassed(s);
            btn.setButtonText(loaded ? engine.masterInserts.getSlotName(s) : ("+ Mst " + juce::String(s + 1)));
            btn.setColour(juce::TextButton::buttonColourId,
                          !loaded ? OctoColours::slotBg : (byp ? OctoColours::slotBypass : OctoColours::slotActive));
            btn.setColour(juce::TextButton::textColourOffId,
                          !loaded ? OctoColours::textDim.withAlpha(0.65f)
                                  : (byp ? OctoColours::accentAmber : OctoColours::textMain));
        }

        monoBtn.setToggleState(engine.masterMonoCheck.load(), juce::dontSendNotification);
        limBtn.setToggleState(engine.masterLimiterEnabled.load(), juce::dontSendNotification);
        dimBtn.setToggleState(engine.masterDim.load(), juce::dontSendNotification);
        if (!volFader.isMouseButtonDown())
            volFader.setValue(engine.masterGain.load(), juce::dontSendNotification);

        meter.setLevels(engine.masterPeakL.load(std::memory_order_relaxed),
                        engine.masterPeakR.load(std::memory_order_relaxed),
                        engine.masterRms.load(std::memory_order_relaxed));
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced(1.5f);
        g.setColour(juce::Colour(0xff232836));
        g.fillRoundedRectangle(b, 5.0f);
        g.setColour(OctoColours::accentRed);
        g.fillRoundedRectangle(b.getX() + 1.0f, b.getY() + 1.0f, b.getWidth() - 2.0f, 3.0f, 1.5f);
        g.setColour(OctoColours::border.brighter(0.2f));
        g.drawRoundedRectangle(b, 5.0f, 1.0f);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addSectionHeader("Master Bus");
            m.addItem(1, "Toggle Mono Check", true, engine.masterMonoCheck.load());
            m.addItem(2, "Toggle Safety Limiter", true, engine.masterLimiterEnabled.load());
            m.addItem(3, "Toggle Dim (-12 dB)", true, engine.masterDim.load());
            m.addSeparator();
            m.addItem(4, "Copy Master 4-Insert Chain");
            m.addItem(5, "Paste 4-Insert Chain", engine.hasCopiedInsertChain());
            m.addItem(6, "Clear All 4 Master Inserts");
            m.showMenuAsync(juce::PopupMenu::Options(), [this](int r) {
                if (r == 1) engine.masterMonoCheck.store(!engine.masterMonoCheck.load());
                else if (r == 2) engine.masterLimiterEnabled.store(!engine.masterLimiterEnabled.load());
                else if (r == 3) engine.masterDim.store(!engine.masterDim.load());
                else if (r == 4) engine.copyInsertChain(engine.masterInserts);
                else if (r == 5) engine.pasteInsertChain(engine.masterInserts);
                else if (r == 6) engine.masterInserts.clearAll();
                refresh();
            });
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(5, 6);
        area.removeFromTop(3);
        headerLabel.setBounds(area.removeFromTop(21));
        area.removeFromTop(3);
        clipBtn.setBounds(area.removeFromTop(18));
        area.removeFromTop(15);

        for (auto& btn : insertButtons)
        {
            btn.setBounds(area.removeFromTop(19));
            area.removeFromTop(2);
        }
        area.removeFromTop(45);

        auto flags = area.removeFromTop(20);
        const int w3 = flags.getWidth() / 3;
        monoBtn.setBounds(flags.removeFromLeft(w3).reduced(1, 0));
        limBtn.setBounds(flags.removeFromLeft(w3).reduced(1, 0));
        dimBtn.setBounds(flags.reduced(1, 0));
        area.removeFromTop(33);

        auto faderRow = area;
        meter.setBounds(faderRow.removeFromRight(16).reduced(0, 4));
        faderRow.removeFromRight(4);
        volFader.setBounds(faderRow);
    }

private:
    AudioEngine& engine;
    juce::Label headerLabel;
    juce::TextButton clipBtn;
    std::array<ContextButton, InsertChain::numSlots> insertButtons;
    ContextButton monoBtn, limBtn, dimBtn;
    ContextSlider volFader;
    StereoMeter meter;
};
