#pragma once
#include <cstdint>
#include <utility>
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include "AudioEngine.h"
#include "LookAndFeel.h"

class AudioEditorUI : public juce::Component
{
public:
    explicit AudioEditorUI(AudioEngine& eng) : engine(eng)
    {
        trackBox.onChange = [this] {
            const int idx = trackBox.getSelectedId() - 1;
            if (idx >= 0)
                setTrackIndex(idx);
        };
        addAndMakeVisible(trackBox);

        undoBtn.setButtonText("Undo");
        undoBtn.onClick = [this] {
            if (auto t = engine.getTrack(selectedTrackIndex))
            {
                t->undoAudioEdit();
                resetSelection();
                repaint();
            }
        };
        addAndMakeVisible(undoBtn);

        trimBtn.setButtonText("Trim");
        trimBtn.onClick = [this] {
            if (auto t = engine.getTrack(selectedTrackIndex))
            {
                auto [s, e] = getSelectedSampleRange(*t);
                t->audioTrimRange(s, e);
                resetSelection();
                repaint();
            }
        };
        addAndMakeVisible(trimBtn);

        silenceBtn.setButtonText("Silence");
        silenceBtn.onClick = [this] {
            if (auto t = engine.getTrack(selectedTrackIndex))
            {
                auto [s, e] = getSelectedSampleRange(*t);
                t->audioSilenceRange(s, e);
                repaint();
            }
        };
        addAndMakeVisible(silenceBtn);

        normBtn.setButtonText("Norm -1dB");
        normBtn.onClick = [this] {
            if (auto t = engine.getTrack(selectedTrackIndex))
            {
                auto [s, e] = getSelectedSampleRange(*t);
                t->audioNormalize(s, e, -1.0f);
                repaint();
            }
        };
        addAndMakeVisible(normBtn);

        fadeInBtn.setButtonText("Fade In");
        fadeInBtn.onClick = [this] {
            if (auto t = engine.getTrack(selectedTrackIndex))
            {
                auto [s, e] = getSelectedSampleRange(*t);
                t->audioFadeIn(s, e);
                repaint();
            }
        };
        addAndMakeVisible(fadeInBtn);

        fadeOutBtn.setButtonText("Fade Out");
        fadeOutBtn.onClick = [this] {
            if (auto t = engine.getTrack(selectedTrackIndex))
            {
                auto [s, e] = getSelectedSampleRange(*t);
                t->audioFadeOut(s, e);
                repaint();
            }
        };
        addAndMakeVisible(fadeOutBtn);

        revBtn.setButtonText("Reverse");
        revBtn.onClick = [this] {
            if (auto t = engine.getTrack(selectedTrackIndex))
            {
                auto [s, e] = getSelectedSampleRange(*t);
                t->audioReverse(s, e);
                repaint();
            }
        };
        addAndMakeVisible(revBtn);

        moreBtn.setButtonText("More v");
        moreBtn.onClick = [this] { showMoreMenu(); };
        addAndMakeVisible(moreBtn);

        zoomSlider.setSliderStyle(juce::Slider::LinearHorizontal);
        zoomSlider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        zoomSlider.setRange(1.0, 12.0, 0.1);
        zoomSlider.setValue(1.0);
        zoomSlider.setTooltip("Waveform Zoom");
        zoomSlider.onValueChange = [this] { repaint(); };
        addAndMakeVisible(zoomSlider);

        syncTrackCombo();
    }

    void setTrackIndex(int idx)
    {
        selectedTrackIndex = idx;
        syncTrackCombo();
        resetSelection();
        repaint();
    }

    void syncTrackCombo()
    {
        const int count = engine.getNumTracks();
        trackBox.clear(juce::dontSendNotification);
        int firstAudio = -1;
        for (int i = 0; i < count; ++i)
        {
            if (auto t = engine.getTrack(i))
            {
                const juce::String tag = (t->mode.load() == TrackMode::Audio ? "[A] " : "[M] ") + t->name;
                trackBox.addItem(tag, i + 1);
                if (firstAudio < 0 && t->mode.load() == TrackMode::Audio)
                    firstAudio = i;
            }
        }
        if (selectedTrackIndex < 0 || selectedTrackIndex >= count)
            selectedTrackIndex = (firstAudio >= 0) ? firstAudio : 0;
        trackBox.setSelectedId(selectedTrackIndex + 1, juce::dontSendNotification);
    }

    void refresh()
    {
        if (auto t = engine.getTrack(selectedTrackIndex))
            undoBtn.setEnabled(t->canUndoAudio());
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(OctoColours::panelBg);
        g.fillRoundedRectangle(b, 6.0f);
        g.setColour(OctoColours::border);
        g.drawRoundedRectangle(b.reduced(0.5f), 6.0f, 1.0f);

        auto waveArea = getWaveArea();
        g.setColour(OctoColours::slotBg);
        g.fillRoundedRectangle(waveArea, 4.0f);

        auto track = engine.getTrack(selectedTrackIndex);
        if (track == nullptr || track->mode.load() != TrackMode::Audio || track->getClipLengthSamples() <= 0)
        {
            g.setColour(OctoColours::textDim);
            g.setFont(juce::FontOptions(13.0f));
            g.drawText("No audio clip on selected track. Record bass/guitar or click 'More v' -> Import WAV.",
                       waveArea.toNearestInt(), juce::Justification::centred);
            return;
        }

        const int totalSamples = static_cast<int>(track->getClipLengthSamples());
        auto [viewStart, viewEnd] = getVisibleSampleWindow(totalSamples);
        const int viewSpan = std::max(1, viewEnd - viewStart);

        // Center zero-crossing line
        const float midY = waveArea.getCentreY();
        g.setColour(OctoColours::border.withAlpha(0.55f));
        g.drawHorizontalLine(static_cast<int>(midY), waveArea.getX(), waveArea.getRight());

        // Draw high-resolution min/max waveform
        const int buckets = std::max(64, static_cast<int>(waveArea.getWidth()));
        std::vector<float> mins, maxs;
        track->getWaveformPreviewRange(viewStart, viewEnd, buckets, mins, maxs);

        const float halfH = waveArea.getHeight() * 0.46f;
        g.setColour(track->trackColour.brighter(0.2f));
        for (int i = 0; i < static_cast<int>(mins.size()); ++i)
        {
            const float x = waveArea.getX() + static_cast<float>(i);
            const float yTop = midY - maxs[static_cast<size_t>(i)] * halfH;
            const float yBot = midY - mins[static_cast<size_t>(i)] * halfH;
            g.drawVerticalLine(static_cast<int>(x), std::min(yTop, midY - 0.5f), std::max(yBot, midY + 0.5f));
        }

        // Draw Selection Overlay
        if (hasSelection && selEndRatio > selStartRatio + 0.002)
        {
            const float sx = waveArea.getX() + static_cast<float>(selStartRatio) * waveArea.getWidth();
            const float ex = waveArea.getX() + static_cast<float>(selEndRatio) * waveArea.getWidth();
            g.setColour(OctoColours::accentAmber.withAlpha(0.22f));
            g.fillRect(sx, waveArea.getY(), ex - sx, waveArea.getHeight());
            g.setColour(OctoColours::accentAmber);
            g.drawVerticalLine(static_cast<int>(sx), waveArea.getY(), waveArea.getBottom());
            g.drawVerticalLine(static_cast<int>(ex), waveArea.getY(), waveArea.getBottom());
        }

        // Playhead inside visible window
        const int64_t ph = engine.playheadSample.load();
        if (ph >= viewStart && ph <= viewEnd)
        {
            const float px = waveArea.getX()
                + static_cast<float>(static_cast<double>(ph - viewStart) / viewSpan) * waveArea.getWidth();
            g.setColour(juce::Colours::white);
            g.drawLine(px, waveArea.getY(), px, waveArea.getBottom(), 1.6f);
        }

        // Footer info readout
        const double sr = std::max(1.0, engine.currentSampleRate.load());
        auto [ss, ee] = getSelectedSampleRange(*track);
        const juce::String info = juce::String::formatted(
            "Length: %.2fs  |  Selection: %.2fs - %.2fs (%.2fs)  |  Drag to select region, Double-click to clear selection",
            totalSamples / sr, ss / sr, ee / sr, (ee - ss) / sr);
        g.setColour(OctoColours::textDim);
        g.setFont(juce::FontOptions(10.0f));
        g.drawText(info, static_cast<int>(waveArea.getX()) + 6, static_cast<int>(waveArea.getBottom()) - 15,
                   static_cast<int>(waveArea.getWidth()) - 12, 14, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8, 5);
        auto top = area.removeFromTop(24);

        trackBox.setBounds(top.removeFromLeft(118).reduced(1));
        top.removeFromLeft(4);
        undoBtn.setBounds(top.removeFromLeft(48).reduced(1));
        trimBtn.setBounds(top.removeFromLeft(46).reduced(1));
        silenceBtn.setBounds(top.removeFromLeft(56).reduced(1));
        normBtn.setBounds(top.removeFromLeft(72).reduced(1));
        fadeInBtn.setBounds(top.removeFromLeft(58).reduced(1));
        fadeOutBtn.setBounds(top.removeFromLeft(64).reduced(1));
        revBtn.setBounds(top.removeFromLeft(60).reduced(1));
        moreBtn.setBounds(top.removeFromLeft(56).reduced(1));

        zoomSlider.setBounds(top.removeFromRight(96).reduced(1));
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        auto waveArea = getWaveArea();
        if (!waveArea.contains(e.position)) return;

        if (e.getNumberOfClicks() >= 2)
        {
            resetSelection();
            repaint();
            return;
        }

        const double r = juce::jlimit(0.0, 1.0, static_cast<double>(e.position.x - waveArea.getX()) / std::max(1.0f, waveArea.getWidth()));
        dragAnchorRatio = r;
        selStartRatio = selEndRatio = r;
        hasSelection = true;

        // Also cue playhead to click position
        if (auto t = engine.getTrack(selectedTrackIndex))
        {
            const int total = static_cast<int>(t->getClipLengthSamples());
            if (total > 0)
            {
                auto [vs, ve] = getVisibleSampleWindow(total);
                engine.playheadSample.store(vs + static_cast<int64_t>(r * (ve - vs)));
            }
        }
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        auto waveArea = getWaveArea();
        const double r = juce::jlimit(0.0, 1.0, static_cast<double>(e.position.x - waveArea.getX()) / std::max(1.0f, waveArea.getWidth()));
        selStartRatio = std::min(dragAnchorRatio, r);
        selEndRatio   = std::max(dragAnchorRatio, r);
        hasSelection = true;
        repaint();
    }

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        if (zoomSlider.getValue() > 1.01)
        {
            panOffset = juce::jlimit(0.0, 1.0, panOffset - wheel.deltaX * 0.2 - wheel.deltaY * 0.15);
            repaint();
        }
    }

private:
    juce::Rectangle<float> getWaveArea() const
    {
        auto a = getLocalBounds().reduced(8, 5);
        a.removeFromTop(28);
        return a.toFloat();
    }

    std::pair<int, int> getVisibleSampleWindow(int totalSamples) const
    {
        const double zoom = std::max(1.0, zoomSlider.getValue());
        const int winLen = std::max(64, static_cast<int>(totalSamples / zoom));
        const int maxStart = std::max(0, totalSamples - winLen);
        const int start = static_cast<int>(panOffset * maxStart);
        return { start, std::min(totalSamples, start + winLen) };
    }

    std::pair<int, int> getSelectedSampleRange(const Track& t) const
    {
        const int total = static_cast<int>(t.getClipLengthSamples());
        if (total <= 0) return { 0, 0 };
        if (!hasSelection || selEndRatio <= selStartRatio + 0.002)
            return { 0, total };
        auto [vs, ve] = getVisibleSampleWindow(total);
        const int span = ve - vs;
        const int s = vs + static_cast<int>(selStartRatio * span);
        const int e = vs + static_cast<int>(selEndRatio * span);
        return { juce::jlimit(0, total - 1, s), juce::jlimit(s + 1, total, e) };
    }

    void resetSelection()
    {
        hasSelection = false;
        selStartRatio = 0.0;
        selEndRatio = 1.0;
    }

    void showMoreMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader("Pro Audio Clip Operations");
        m.addItem(1, "Gain +3 dB (Selection / Full)");
        m.addItem(2, "Gain -3 dB (Selection / Full)");
        m.addItem(3, "Auto-Trim Leading & Trailing Silence");
        m.addSeparator();
        m.addItem(4, "Nudge Clip Earlier (-10 ms)");
        m.addItem(5, "Nudge Clip Later (+10 ms)");
        m.addItem(6, "Nudge Clip Earlier (-50 ms)");
        m.addItem(7, "Nudge Clip Later (+50 ms)");
        m.addSeparator();
        m.addItem(10, "Import WAV Clip...");
        m.addItem(11, "Export Selection / Clip as 24-bit WAV...");

        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(moreBtn), [this](int r) {
            auto t = engine.getTrack(selectedTrackIndex);
            if (t == nullptr) return;
            const double sr = engine.currentSampleRate.load();
            const auto selRange = getSelectedSampleRange(*t);
            const int s = selRange.first;
            const int e = selRange.second;

            if (r == 1) t->audioApplyGainDb(s, e, 3.0f);
            else if (r == 2) t->audioApplyGainDb(s, e, -3.0f);
            else if (r == 3) { t->audioAutoTrimSilence(); resetSelection(); }
            else if (r == 4) t->audioNudgeSamples(-static_cast<int>(0.010 * sr));
            else if (r == 5) t->audioNudgeSamples( static_cast<int>(0.010 * sr));
            else if (r == 6) t->audioNudgeSamples(-static_cast<int>(0.050 * sr));
            else if (r == 7) t->audioNudgeSamples( static_cast<int>(0.050 * sr));
            else if (r == 10)
            {
                fileChooser = std::make_unique<juce::FileChooser>("Import WAV Clip", juce::File(), "*.wav");
                fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                         [this, t](const juce::FileChooser& fc) {
                                             auto f = fc.getResult();
                                             if (f.existsAsFile()) { t->importWavFile(f); resetSelection(); repaint(); }
                                         });
            }
            else if (r == 11)
            {
                fileChooser = std::make_unique<juce::FileChooser>(
                    "Export Audio Selection WAV",
                    juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile(t->name + "_edit.wav"),
                    "*.wav");
                fileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                         [t, sr, s, e](const juce::FileChooser& fc) {
                                             auto f = fc.getResult();
                                             if (f != juce::File()) t->exportClipWav(f, sr, s, e);
                                         });
            }
            repaint();
        });
    }

    AudioEngine& engine;
    int selectedTrackIndex = 1;
    bool hasSelection = false;
    double dragAnchorRatio = 0.0;
    double selStartRatio = 0.0;
    double selEndRatio = 1.0;
    double panOffset = 0.0;

    juce::ComboBox trackBox;
    juce::TextButton undoBtn, trimBtn, silenceBtn, normBtn, fadeInBtn, fadeOutBtn, revBtn, moreBtn;
    juce::Slider zoomSlider;
    std::unique_ptr<juce::FileChooser> fileChooser;
};
