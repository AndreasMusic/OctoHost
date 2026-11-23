#pragma once
#include <cstdint>
#include <juce_gui_basics/juce_gui_basics.h>
#include "AudioEngine.h"
#include "LookAndFeel.h"

class DrumSequencerUI : public juce::Component
{
public:
    explicit DrumSequencerUI(AudioEngine& eng) : engine(eng)
    {
        enableBtn.setButtonText("SEQ ON");
        enableBtn.setClickingTogglesState(true);
        enableBtn.setToggleState(engine.drumEngine.sequencerEnabled.load(), juce::dontSendNotification);
        enableBtn.setColour(juce::TextButton::buttonOnColourId, OctoColours::accentAmber.darker(0.15f));
        enableBtn.onClick = [this] {
            engine.drumEngine.sequencerEnabled.store(enableBtn.getToggleState());
        };
        addAndMakeVisible(enableBtn);

        static const char* patLabels[4] = { "A", "B", "C", "D" };
        for (int p = 0; p < DrumEngine::numPatterns; ++p)
        {
            auto& b = patButtons[static_cast<size_t>(p)];
            b.setButtonText(patLabels[p]);
            b.onClick = [this, p] {
                engine.drumEngine.activePattern.store(p);
                refresh();
            };
            addAndMakeVisible(b);
        }

        stepsBox.addItem("16 Steps (4/4)", 16);
        stepsBox.addItem("14 Steps (7/8)", 14);
        stepsBox.addItem("12 Steps (6/8)", 12);
        stepsBox.addItem("10 Steps (5/8)", 10);
        stepsBox.addItem("8 Steps (2/4)", 8);
        stepsBox.setSelectedId(16, juce::dontSendNotification);
        stepsBox.onChange = [this] {
            const int pat = engine.drumEngine.activePattern.load();
            const int id = stepsBox.getSelectedId();
            if (id >= 4)
                engine.drumEngine.setPatternStepCount(pat, id);
            repaint();
        };
        addAndMakeVisible(stepsBox);

        groovesBtn.setButtonText("Grooves v");
        groovesBtn.onClick = [this] { showGrooveMenu(); };
        addAndMakeVisible(groovesBtn);

        swingSlider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        swingSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        swingSlider.setRange(0.0, 0.35, 0.01);
        swingSlider.setValue(0.0);
        swingSlider.setTooltip("Swing / Groove Feel");
        swingSlider.setColour(juce::Slider::rotarySliderFillColourId, OctoColours::accentAmber);
        swingSlider.onValueChange = [this] {
            engine.drumEngine.swingAmount.store(static_cast<float>(swingSlider.getValue()));
        };
        addAndMakeVisible(swingSlider);

        swingLabel.setText("SWING", juce::dontSendNotification);
        swingLabel.setFont(juce::FontOptions(10.0f, juce::Font::bold));
        swingLabel.setColour(juce::Label::textColourId, OctoColours::textDim);
        swingLabel.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(swingLabel);

        commitBtn.setButtonText("Commit -> Trk 1");
        commitBtn.setColour(juce::TextButton::buttonColourId, OctoColours::accentTeal.darker(0.35f));
        commitBtn.onClick = [this] { showCommitMenu(); };
        addAndMakeVisible(commitBtn);

        clearBtn.setButtonText("Clear");
        clearBtn.onClick = [this] {
            engine.drumEngine.clearPattern(engine.drumEngine.activePattern.load());
            repaint();
        };
        addAndMakeVisible(clearBtn);
    }

    void refresh()
    {
        const int pat = engine.drumEngine.activePattern.load();
        enableBtn.setToggleState(engine.drumEngine.sequencerEnabled.load(), juce::dontSendNotification);

        for (int p = 0; p < DrumEngine::numPatterns; ++p)
        {
            patButtons[static_cast<size_t>(p)].setColour(
                juce::TextButton::buttonColourId,
                (p == pat) ? OctoColours::accentAmber.darker(0.15f) : juce::Colour(0xff282d38));
        }

        const int curSteps = engine.drumEngine.getPatternStepCount(pat);
        if (stepsBox.getSelectedId() != curSteps)
            stepsBox.setSelectedId(curSteps, juce::dontSendNotification);

        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(OctoColours::panelBg);
        g.fillRoundedRectangle(b, 6.0f);
        g.setColour(OctoColours::border);
        g.drawRoundedRectangle(b.reduced(0.5f), 6.0f, 1.0f);

        auto gridArea = getGridArea();
        const int pat = engine.drumEngine.activePattern.load();
        const int stepCount = engine.drumEngine.getPatternStepCount(pat);
        const int playingStep = engine.drumEngine.currentPlayingStep.load();

        const float labelW = 58.0f;
        const float cellW = (gridArea.getWidth() - labelW) / static_cast<float>(DrumEngine::maxSteps);
        const float rowH = gridArea.getHeight() / static_cast<float>(DrumEngine::numLanes);

        for (int lane = 0; lane < DrumEngine::numLanes; ++lane)
        {
            const float y = gridArea.getY() + lane * rowH;
            const bool muted = engine.drumEngine.laneMuted[static_cast<size_t>(lane)].load();

            // Lane label
            g.setColour(muted ? OctoColours::textDim.withAlpha(0.35f) : OctoColours::textMain);
            g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
            g.drawText(DrumEngine::lanes[static_cast<size_t>(lane)].name,
                       static_cast<int>(gridArea.getX()), static_cast<int>(y),
                       static_cast<int>(labelW - 4.0f), static_cast<int>(rowH),
                       juce::Justification::centredRight);

            for (int step = 0; step < DrumEngine::maxSteps; ++step)
            {
                const float x = gridArea.getX() + labelW + step * cellW;
                auto cell = juce::Rectangle<float>(x + 1.5f, y + 1.5f, cellW - 3.0f, rowH - 3.0f);

                const bool activeRegion = (step < stepCount);
                const uint8_t vel = engine.drumEngine.getStep(pat, lane, step);
                const bool isBeatStart = (step % 4 == 0);

                if (!activeRegion)
                {
                    g.setColour(juce::Colour(0xff0d0f14));
                }
                else if (vel == 0)
                {
                    g.setColour(isBeatStart ? juce::Colour(0xff262c38) : juce::Colour(0xff1b1f29));
                }
                else if (vel == 1) // Ghost
                {
                    g.setColour(OctoColours::accentAmber.withAlpha(0.38f));
                }
                else if (vel == 2) // Normal
                {
                    g.setColour(OctoColours::accentAmber.withAlpha(0.78f));
                }
                else // Accent
                {
                    g.setColour(OctoColours::accentRed);
                }

                if (muted && vel > 0)
                    g.setColour(OctoColours::textDim.withAlpha(0.35f));

                g.fillRoundedRectangle(cell, 3.0f);

                if (step == playingStep && activeRegion)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.85f));
                    g.drawRoundedRectangle(cell, 3.0f, 1.5f);
                }
            }
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8, 6);
        auto top = area.removeFromTop(26);

        enableBtn.setBounds(top.removeFromLeft(66).reduced(1));
        top.removeFromLeft(6);
        for (auto& b : patButtons)
            b.setBounds(top.removeFromLeft(28).reduced(1));
        top.removeFromLeft(8);
        stepsBox.setBounds(top.removeFromLeft(128).reduced(1));
        top.removeFromLeft(6);
        groovesBtn.setBounds(top.removeFromLeft(86).reduced(1));
        top.removeFromLeft(10);
        swingLabel.setBounds(top.removeFromLeft(40));
        swingSlider.setBounds(top.removeFromLeft(28));

        clearBtn.setBounds(top.removeFromRight(56).reduced(1));
        top.removeFromRight(4);
        commitBtn.setBounds(top.removeFromRight(116).reduced(1));
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        auto gridArea = getGridArea();
        if (!gridArea.contains(e.position))
            return;

        const float labelW = 58.0f;
        const float cellW = (gridArea.getWidth() - labelW) / static_cast<float>(DrumEngine::maxSteps);
        const float rowH = gridArea.getHeight() / static_cast<float>(DrumEngine::numLanes);

        const int lane = juce::jlimit(0, DrumEngine::numLanes - 1,
                                      static_cast<int>((e.position.y - gridArea.getY()) / rowH));
        const int pat = engine.drumEngine.activePattern.load();

        // Right-click anywhere on a lane opens Lane Quick-Fill / Mute Menu
        if (e.mods.isPopupMenu())
        {
            showLaneContextMenu(pat, lane);
            return;
        }

        // Left-click on lane label toggles Mute
        if (e.position.x < gridArea.getX() + labelW)
        {
            auto& m = engine.drumEngine.laneMuted[static_cast<size_t>(lane)];
            m.store(!m.load());
            repaint();
            return;
        }

        const int step = static_cast<int>((e.position.x - (gridArea.getX() + labelW)) / cellW);
        if (step < 0 || step >= DrumEngine::maxSteps)
            return;

        const uint8_t cur = engine.drumEngine.getStep(pat, lane, step);
        // Shift-click cycles velocity (Off -> Normal -> Accent -> Ghost -> Off); normal click toggles Off <-> Normal (or Accent if Alt)
        uint8_t next = 0;
        if (e.mods.isShiftDown())
            next = static_cast<uint8_t>((cur + 1) % 4);
        else if (e.mods.isAltDown())
            next = (cur == 3) ? 0 : 3;
        else
            next = (cur == 0) ? 2 : (cur == 2 ? 3 : (cur == 3 ? 1 : 0));

        engine.drumEngine.setStep(pat, lane, step, next);
        repaint();
    }

private:
    juce::Rectangle<float> getGridArea() const
    {
        auto area = getLocalBounds().reduced(8, 6);
        area.removeFromTop(32);
        return area.toFloat();
    }

    void showLaneContextMenu(int pat, int lane)
    {
        juce::PopupMenu m;
        const bool isMuted = engine.drumEngine.laneMuted[static_cast<size_t>(lane)].load();
        m.addSectionHeader(juce::String(DrumEngine::lanes[static_cast<size_t>(lane)].name)
                           + " (MIDI " + juce::String(DrumEngine::lanes[static_cast<size_t>(lane)].midiNote) + ")");
        m.addItem(1, isMuted ? "Unmute Lane" : "Mute Lane");
        m.addSeparator();
        m.addItem(2, "Fill Quarter Notes (Every 4)");
        m.addItem(3, "Fill Eighth Notes (Every 2)");
        m.addItem(4, "Fill Sixteenth Notes (All)");
        m.addItem(5, "Clear Lane");

        m.showMenuAsync(juce::PopupMenu::Options(), [this, pat, lane](int r) {
            if (r == 1)
            {
                auto& lm = engine.drumEngine.laneMuted[static_cast<size_t>(lane)];
                lm.store(!lm.load());
            }
            else if (r == 2) engine.drumEngine.fillLaneEvery(pat, lane, 4, 2);
            else if (r == 3) engine.drumEngine.fillLaneEvery(pat, lane, 2, 2);
            else if (r == 4) engine.drumEngine.fillLaneEvery(pat, lane, 1, 2);
            else if (r == 5) engine.drumEngine.clearLane(pat, lane);
            repaint();
        });
    }

    void showGrooveMenu()
    {
        const int pat = engine.drumEngine.activePattern.load();
        juce::PopupMenu m;
        m.addSectionHeader("Load Groove into Pattern");
        m.addItem(1, "Classic Rock (4/4)");
        m.addItem(2, "Heavy Riff / Double-Kick (4/4)");
        m.addItem(3, "Half-Time Breakdown (4/4)");
        m.addItem(4, "Progressive Odd Groove (7/8 - 14 Steps)");
        m.addItem(5, "Fast Punk / Skate (4/4)");
        m.addItem(6, "Blues / Slow Rock Shuffle (6/8 - 12 Steps)");
        m.addItem(7, "Funk Pocket (4/4)");

        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(groovesBtn), [this, pat](int r) {
            if (r == 1) engine.drumEngine.loadPresetRock(pat);
            else if (r == 2) engine.drumEngine.loadPresetHeavyRiff(pat);
            else if (r == 3) engine.drumEngine.loadPresetHalfTime(pat);
            else if (r == 4) engine.drumEngine.loadPresetProg78(pat);
            else if (r == 5) engine.drumEngine.loadPresetPunk(pat);
            else if (r == 6) engine.drumEngine.loadPresetBlues68(pat);
            else if (r == 7) engine.drumEngine.loadPresetFunk(pat);
            refresh();
        });
    }

    void showCommitMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader("Write Active Pattern to Track 1 MIDI Clip");
        m.addItem(4, "Commit 4 Bars to Track 1");
        m.addItem(8, "Commit 8 Bars to Track 1");
        m.addItem(16, "Commit 16 Bars to Track 1");
        m.addItem(32, "Commit 32 Bars to Track 1");

        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(commitBtn), [this](int bars) {
            if (bars > 0)
            {
                engine.commitDrumPatternToTrack(0, bars);
                refresh();
            }
        });
    }

    AudioEngine& engine;
    juce::TextButton enableBtn, groovesBtn, commitBtn, clearBtn;
    std::array<juce::TextButton, DrumEngine::numPatterns> patButtons;
    juce::ComboBox stepsBox;
    juce::Slider swingSlider;
    juce::Label swingLabel;
};
