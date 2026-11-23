#pragma once
#include <cstdint>
#include <utility>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#include "AudioEngine.h"
#include "LookAndFeel.h"

class PianoRollUI : public juce::Component
{
public:
    explicit PianoRollUI(AudioEngine& eng) : engine(eng)
    {
        trackBox.onChange = [this] {
            const int idx = trackBox.getSelectedId() - 1;
            if (idx >= 0)
                setTrackIndex(idx);
        };
        addAndMakeVisible(trackBox);

        gridBox.addItem("1/4", 1);
        gridBox.addItem("1/8", 2);
        gridBox.addItem("1/16", 3);
        gridBox.addItem("1/8T", 4);
        gridBox.setSelectedId(3, juce::dontSendNotification);
        gridBox.onChange = [this] { repaint(); };
        addAndMakeVisible(gridBox);

        for (int i = 0; i < 12; ++i)
            rootBox.addItem(MusicTheory::noteNames[static_cast<size_t>(i)], i + 1);
        rootBox.setSelectedId(5, juce::dontSendNotification); // E default
        rootBox.onChange = [this] { repaint(); };
        addAndMakeVisible(rootBox);

        const auto& scales = MusicTheory::getScales();
        for (int i = 0; i < static_cast<int>(scales.size()); ++i)
            scaleBox.addItem(scales[static_cast<size_t>(i)].name, i + 1);
        scaleBox.setSelectedId(1, juce::dontSendNotification); // Natural Minor
        scaleBox.onChange = [this] { repaint(); };
        addAndMakeVisible(scaleBox);

        octDownBtn.setButtonText("Oct-");
        octDownBtn.onClick = [this] { lowestPitch = juce::jlimit(24, 72, lowestPitch - 12); repaint(); };
        addAndMakeVisible(octDownBtn);

        octUpBtn.setButtonText("Oct+");
        octUpBtn.onClick = [this] { lowestPitch = juce::jlimit(24, 72, lowestPitch + 12); repaint(); };
        addAndMakeVisible(octUpBtn);

        quantBtn.setButtonText("Quant");
        quantBtn.onClick = [this] { applyToNotes([this](auto& n) { MidiHelpers::quantizeNotes(n, getGridBeats()); }); };
        addAndMakeVisible(quantBtn);

        humanBtn.setButtonText("Human");
        humanBtn.onClick = [this] { applyToNotes([](auto& n) { MidiHelpers::humanizeNotes(n, 0.035, 14); }); };
        addAndMakeVisible(humanBtn);

        toolsBtn.setButtonText("Edit v");
        toolsBtn.onClick = [this] { showToolsMenu(); };
        addAndMakeVisible(toolsBtn);

        clearBtn.setButtonText("Clear");
        clearBtn.onClick = [this] { applyToNotes([](auto& n) { n.clear(); }); };
        addAndMakeVisible(clearBtn);

        syncTrackCombo();
    }

    void setTrackIndex(int idx)
    {
        selectedTrackIndex = idx;
        syncTrackCombo();
        reloadNotesFromTrack();
        repaint();
    }

    int getTrackIndex() const { return selectedTrackIndex; }

    void syncTrackCombo()
    {
        const int count = engine.getNumTracks();
        trackBox.clear(juce::dontSendNotification);
        int firstMidi = -1;
        for (int i = 0; i < count; ++i)
        {
            if (auto t = engine.getTrack(i))
            {
                const juce::String tag = (t->mode.load() == TrackMode::Midi ? "[M] " : "[A] ") + t->name;
                trackBox.addItem(tag, i + 1);
                if (firstMidi < 0 && t->mode.load() == TrackMode::Midi)
                    firstMidi = i;
            }
        }
        if (selectedTrackIndex < 0 || selectedTrackIndex >= count)
            selectedTrackIndex = (firstMidi >= 0) ? firstMidi : 0;
        trackBox.setSelectedId(selectedTrackIndex + 1, juce::dontSendNotification);
    }

    void refresh()
    {
        if (dragMode == DragMode::None)
            reloadNotesFromTrack();
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
        auto keysArea = getKeysArea();
        auto velArea  = getVelocityArea();

        const int rootPc = rootBox.getSelectedId() - 1;
        const int scaleIdx = scaleBox.getSelectedId() - 1;
        const float rowH = gridArea.getHeight() / static_cast<float>(visiblePitchRows);
        const double totalBeats = getTotalVisibleBeats();
        const float pxPerBeat = gridArea.getWidth() / static_cast<float>(totalBeats);

        // 1. Piano Keyboard & Horizontal Pitch Lanes
        for (int r = 0; r < visiblePitchRows; ++r)
        {
            const int pitch = lowestPitch + (visiblePitchRows - 1 - r);
            const float y = gridArea.getY() + r * rowH;
            const int pc = pitch % 12;
            const bool isBlack = (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);
            const bool inScale = MusicTheory::isPitchInScale(pitch, rootPc, scaleIdx);
            const bool isRoot  = (pc == rootPc);

            // Key graphic
            g.setColour(isBlack ? juce::Colour(0xff13161d) : juce::Colour(0xff2c3240));
            g.fillRect(keysArea.getX(), y, keysArea.getWidth() - 1.0f, rowH - 0.5f);
            if (isRoot)
            {
                g.setColour(OctoColours::accentTeal.withAlpha(0.75f));
                g.fillRect(keysArea.getX(), y, 3.0f, rowH);
            }
            if (pc == 0 || isRoot)
            {
                g.setColour(isRoot ? OctoColours::accentTeal : OctoColours::textDim);
                g.setFont(juce::FontOptions(9.0f, juce::Font::bold));
                g.drawText(MusicTheory::midiPitchName(pitch),
                           static_cast<int>(keysArea.getX()) + 4, static_cast<int>(y),
                           static_cast<int>(keysArea.getWidth()) - 6, static_cast<int>(rowH),
                           juce::Justification::centredLeft);
            }

            // Lane background (scale highlight!)
            auto laneCol = isBlack ? juce::Colour(0xff12151c) : juce::Colour(0xff181c26);
            if (isRoot)
                laneCol = OctoColours::accentTeal.withAlpha(0.11f);
            else if (inScale)
                laneCol = laneCol.brighter(0.05f);

            g.setColour(laneCol);
            g.fillRect(gridArea.getX(), y, gridArea.getWidth(), rowH);
            g.setColour(OctoColours::border.withAlpha(pc == 0 ? 0.55f : 0.2f));
            g.drawHorizontalLine(static_cast<int>(y), gridArea.getX(), gridArea.getRight());
        }

        // 2. Vertical Beat / Sub-Grid Lines
        const double step = getGridBeats();
        const int numSteps = static_cast<int>(std::ceil(totalBeats / step));
        for (int s = 0; s <= numSteps; ++s)
        {
            const double beat = s * step;
            const float x = gridArea.getX() + static_cast<float>(beat) * pxPerBeat;
            const bool isBar = std::abs(std::fmod(beat, 4.0)) < 0.001;
            const bool isBeat = std::abs(std::fmod(beat, 1.0)) < 0.001;
            g.setColour(OctoColours::border.withAlpha(isBar ? 0.75f : (isBeat ? 0.35f : 0.14f)));
            g.drawVerticalLine(static_cast<int>(x), gridArea.getY(), velArea.getBottom());
        }

        // 3. Notes + Velocity Lane Bars
        g.setColour(OctoColours::slotBg);
        g.fillRect(velArea);
        g.setColour(OctoColours::border);
        g.drawHorizontalLine(static_cast<int>(velArea.getY()), velArea.getX(), velArea.getRight());

        auto track = engine.getTrack(selectedTrackIndex);
        const auto noteCol = (track != nullptr) ? track->trackColour : OctoColours::accentTeal;

        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto& n = notes[i];
            const float x = gridArea.getX() + static_cast<float>(n.startBeat) * pxPerBeat;
            const float w = std::max(6.0f, static_cast<float>(n.lengthBeats) * pxPerBeat);

            // Velocity bar
            const float vNorm = static_cast<float>(n.velocity) / 127.0f;
            const float vh = velArea.getHeight() * vNorm;
            g.setColour(noteCol.withAlpha(0.75f));
            g.fillRect(x, velArea.getBottom() - vh, std::min(w, 6.0f), vh);

            if (n.pitch < lowestPitch || n.pitch >= lowestPitch + visiblePitchRows)
                continue;

            const int row = (lowestPitch + visiblePitchRows - 1) - n.pitch;
            const float y = gridArea.getY() + row * rowH;
            auto nr = juce::Rectangle<float>(x + 0.5f, y + 1.0f, w - 1.0f, rowH - 2.0f);

            const bool isSel = (static_cast<int>(i) == activeNoteIdx);
            g.setColour(noteCol.withAlpha(0.5f + 0.45f * vNorm));
            g.fillRoundedRectangle(nr, 2.5f);
            g.setColour(isSel ? juce::Colours::white : noteCol.brighter(0.35f));
            g.drawRoundedRectangle(nr, 2.5f, isSel ? 1.5f : 0.9f);
        }

        // 4. Live Playhead Line
        const double curBeat = engine.getPlayheadSeconds() * (engine.bpm.load() / 60.0);
        if (curBeat >= 0.0 && curBeat <= totalBeats)
        {
            const float px = gridArea.getX() + static_cast<float>(curBeat) * pxPerBeat;
            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.drawLine(px, gridArea.getY(), px, velArea.getBottom(), 1.5f);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(8, 5);
        auto top = area.removeFromTop(24);

        trackBox.setBounds(top.removeFromLeft(118).reduced(1));
        top.removeFromLeft(4);
        gridBox.setBounds(top.removeFromLeft(58).reduced(1));
        top.removeFromLeft(4);
        rootBox.setBounds(top.removeFromLeft(48).reduced(1));
        scaleBox.setBounds(top.removeFromLeft(122).reduced(1));
        top.removeFromLeft(6);
        octDownBtn.setBounds(top.removeFromLeft(42).reduced(1));
        octUpBtn.setBounds(top.removeFromLeft(42).reduced(1));

        clearBtn.setBounds(top.removeFromRight(48).reduced(1));
        toolsBtn.setBounds(top.removeFromRight(58).reduced(1));
        humanBtn.setBounds(top.removeFromRight(56).reduced(1));
        quantBtn.setBounds(top.removeFromRight(52).reduced(1));
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        auto keysArea = getKeysArea();
        auto gridArea = getGridArea();
        auto velArea  = getVelocityArea();

        // Click piano keys to audition note!
        if (keysArea.contains(e.position))
        {
            const int pitch = yToPitch(e.position.y, gridArea);
            auditionPitch = pitch;
            if (auto t = engine.getTrack(selectedTrackIndex))
                t->pushAuditionNote(pitch, 108, true);
            return;
        }

        // Click velocity lane to paint velocities
        if (velArea.contains(e.position))
        {
            dragMode = DragMode::Velocity;
            editVelocityAt(e.position, gridArea, velArea);
            return;
        }

        if (!gridArea.contains(e.position))
            return;

        const double totalBeats = getTotalVisibleBeats();
        const float pxPerBeat = gridArea.getWidth() / static_cast<float>(totalBeats);
        const float rowH = gridArea.getHeight() / static_cast<float>(visiblePitchRows);

        // Hit-test existing note
        int hitIdx = -1;
        bool onRightEdge = false;
        for (int i = static_cast<int>(notes.size()) - 1; i >= 0; --i)
        {
            const auto& n = notes[static_cast<size_t>(i)];
            if (n.pitch < lowestPitch || n.pitch >= lowestPitch + visiblePitchRows) continue;
            const int row = (lowestPitch + visiblePitchRows - 1) - n.pitch;
            const float x = gridArea.getX() + static_cast<float>(n.startBeat) * pxPerBeat;
            const float w = std::max(8.0f, static_cast<float>(n.lengthBeats) * pxPerBeat);
            const float y = gridArea.getY() + row * rowH;
            juce::Rectangle<float> r(x, y, w, rowH);
            if (r.contains(e.position))
            {
                hitIdx = i;
                onRightEdge = (e.position.x >= r.getRight() - 6.0f);
                break;
            }
        }

        if (e.mods.isPopupMenu())
        {
            if (hitIdx >= 0)
            {
                notes.erase(notes.begin() + hitIdx);
                activeNoteIdx = -1;
                pushNotesToTrack();
                repaint();
            }
            return;
        }

        if (hitIdx >= 0)
        {
            if (e.getNumberOfClicks() >= 2)
            {
                notes.erase(notes.begin() + hitIdx);
                activeNoteIdx = -1;
                pushNotesToTrack();
                repaint();
                return;
            }

            activeNoteIdx = hitIdx;
            dragMode = onRightEdge ? DragMode::Resize : DragMode::Move;
            dragStartNoteBeat = notes[static_cast<size_t>(hitIdx)].startBeat;
            dragStartClickBeat = (e.position.x - gridArea.getX()) / pxPerBeat;

            if (auto t = engine.getTrack(selectedTrackIndex))
            {
                auditionPitch = notes[static_cast<size_t>(hitIdx)].pitch;
                t->pushAuditionNote(auditionPitch, notes[static_cast<size_t>(hitIdx)].velocity, true);
            }
            repaint();
            return;
        }

        // Empty cell -> create new note snapped to grid!
        const double step = getGridBeats();
        const double clickBeat = (e.position.x - gridArea.getX()) / pxPerBeat;
        const double snappedBeat = std::max(0.0, std::floor(clickBeat / step) * step);
        const int pitch = yToPitch(e.position.y, gridArea);

        MidiNote n;
        n.pitch = pitch;
        n.startBeat = snappedBeat;
        n.lengthBeats = step;
        n.velocity = 104;
        n.channel = 1;

        notes.push_back(n);
        activeNoteIdx = static_cast<int>(notes.size()) - 1;
        dragMode = DragMode::Resize;
        pushNotesToTrack();

        if (auto t = engine.getTrack(selectedTrackIndex))
        {
            auditionPitch = pitch;
            t->pushAuditionNote(pitch, n.velocity, true);
        }
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        auto gridArea = getGridArea();
        auto velArea  = getVelocityArea();

        if (dragMode == DragMode::Velocity)
        {
            editVelocityAt(e.position, gridArea, velArea);
            return;
        }

        if (activeNoteIdx < 0 || activeNoteIdx >= static_cast<int>(notes.size()))
            return;

        const double totalBeats = getTotalVisibleBeats();
        const float pxPerBeat = gridArea.getWidth() / static_cast<float>(totalBeats);
        const double step = getGridBeats();
        auto& n = notes[static_cast<size_t>(activeNoteIdx)];

        if (dragMode == DragMode::Move)
        {
            const double curBeat = (e.position.x - gridArea.getX()) / pxPerBeat;
            const double delta = curBeat - dragStartClickBeat;
            n.startBeat = std::max(0.0, std::round((dragStartNoteBeat + delta) / step) * step);
            const int newPitch = yToPitch(e.position.y, gridArea);
            if (newPitch != n.pitch)
            {
                if (auto t = engine.getTrack(selectedTrackIndex))
                {
                    if (auditionPitch >= 0) t->pushAuditionNote(auditionPitch, 0, false);
                    auditionPitch = newPitch;
                    t->pushAuditionNote(newPitch, n.velocity, true);
                }
                n.pitch = newPitch;
            }
            pushNotesToTrack();
            repaint();
        }
        else if (dragMode == DragMode::Resize)
        {
            const double endBeat = (e.position.x - gridArea.getX()) / pxPerBeat;
            const double snappedEnd = std::ceil(endBeat / step) * step;
            n.lengthBeats = std::max(step * 0.5, snappedEnd - n.startBeat);
            pushNotesToTrack();
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        dragMode = DragMode::None;
        if (auditionPitch >= 0)
        {
            if (auto t = engine.getTrack(selectedTrackIndex))
                t->pushAuditionNote(auditionPitch, 0, false);
            auditionPitch = -1;
        }
    }

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        if (std::abs(wheel.deltaY) > 0.01f)
        {
            lowestPitch = juce::jlimit(24, 76, lowestPitch + (wheel.deltaY > 0 ? 2 : -2));
            repaint();
        }
    }

private:
    enum class DragMode { None, Move, Resize, Velocity };

    juce::Rectangle<float> getKeysArea() const
    {
        auto a = getLocalBounds().reduced(8, 5);
        a.removeFromTop(28);
        a.removeFromBottom(32);
        return a.removeFromLeft(44).toFloat();
    }

    juce::Rectangle<float> getGridArea() const
    {
        auto a = getLocalBounds().reduced(8, 5);
        a.removeFromTop(28);
        a.removeFromBottom(32);
        a.removeFromLeft(44);
        return a.toFloat();
    }

    juce::Rectangle<float> getVelocityArea() const
    {
        auto a = getLocalBounds().reduced(8, 5);
        a.removeFromLeft(44);
        return a.removeFromBottom(30).toFloat();
    }

    double getGridBeats() const
    {
        switch (gridBox.getSelectedId())
        {
            case 1: return 1.0;
            case 2: return 0.5;
            case 4: return 1.0 / 3.0;
            case 3:
            default: return 0.25;
        }
    }

    double getTotalVisibleBeats() const
    {
        double maxB = 16.0; // 4 bars default
        for (const auto& n : notes)
            maxB = std::max(maxB, std::ceil((n.startBeat + n.lengthBeats) / 4.0) * 4.0);
        return std::min(64.0, maxB);
    }

    int yToPitch(float y, juce::Rectangle<float> gridArea) const
    {
        const float rowH = gridArea.getHeight() / static_cast<float>(visiblePitchRows);
        const int row = juce::jlimit(0, visiblePitchRows - 1, static_cast<int>((y - gridArea.getY()) / rowH));
        return lowestPitch + (visiblePitchRows - 1 - row);
    }

    void editVelocityAt(juce::Point<float> pos, juce::Rectangle<float> gridArea, juce::Rectangle<float> velArea)
    {
        const double totalBeats = getTotalVisibleBeats();
        const float pxPerBeat = gridArea.getWidth() / static_cast<float>(totalBeats);
        const double clickBeat = (pos.x - gridArea.getX()) / pxPerBeat;
        const float normY = 1.0f - juce::jlimit(0.05f, 1.0f, (pos.y - velArea.getY()) / std::max(1.0f, velArea.getHeight()));
        const uint8_t newVel = static_cast<uint8_t>(juce::jlimit(10, 127, static_cast<int>(std::round(normY * 127.0f))));

        bool changed = false;
        for (auto& n : notes)
        {
            if (std::abs(n.startBeat - clickBeat) < 0.35)
            {
                n.velocity = newVel;
                changed = true;
            }
        }
        if (changed)
        {
            pushNotesToTrack();
            repaint();
        }
    }

    void reloadNotesFromTrack()
    {
        if (auto t = engine.getTrack(selectedTrackIndex))
            notes = t->getMidiNotes(engine.bpm.load(), engine.currentSampleRate.load());
        else
            notes.clear();
    }

    void pushNotesToTrack()
    {
        if (auto t = engine.getTrack(selectedTrackIndex))
        {
            t->mode.store(TrackMode::Midi);
            t->setMidiNotes(notes, engine.bpm.load(), engine.currentSampleRate.load());
        }
    }

    void applyToNotes(std::function<void(std::vector<MidiNote>&)> fn)
    {
        reloadNotesFromTrack();
        fn(notes);
        pushNotesToTrack();
        repaint();
    }

    void showToolsMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader("Piano Roll MIDI Operations");
        m.addItem(1, "Transpose +1 Octave (+12)");
        m.addItem(2, "Transpose -1 Octave (-12)");
        m.addItem(3, "Transpose +1 Semitone");
        m.addItem(4, "Transpose -1 Semitone");
        m.addSeparator();
        m.addItem(5, "Snap All Notes to Selected Scale");
        m.addItem(6, "Legato (Connect Note Lengths)");
        m.addItem(7, "Double Clip (Repeat to 2x Length)");
        m.addSeparator();
        m.addItem(10, "Import Standard MIDI File (.mid)...");
        m.addItem(11, "Export Standard MIDI File (.mid)...");

        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(toolsBtn), [this](int r) {
            if (r == 1) applyToNotes([](auto& n) { MidiHelpers::transposeNotes(n, 12); });
            else if (r == 2) applyToNotes([](auto& n) { MidiHelpers::transposeNotes(n, -12); });
            else if (r == 3) applyToNotes([](auto& n) { MidiHelpers::transposeNotes(n, 1); });
            else if (r == 4) applyToNotes([](auto& n) { MidiHelpers::transposeNotes(n, -1); });
            else if (r == 5)
            {
                const int rootPc = rootBox.getSelectedId() - 1;
                const int scaleIdx = scaleBox.getSelectedId() - 1;
                applyToNotes([rootPc, scaleIdx](auto& ns) {
                    for (auto& n : ns)
                        n.pitch = MusicTheory::snapPitchToScale(n.pitch, rootPc, scaleIdx);
                });
            }
            else if (r == 6) applyToNotes([](auto& n) { MidiHelpers::applyLegato(n); });
            else if (r == 7)
            {
                applyToNotes([](auto& ns) {
                    if (ns.empty()) return;
                    double maxEnd = 0.0;
                    for (const auto& n : ns) maxEnd = std::max(maxEnd, n.startBeat + n.lengthBeats);
                    const double span = std::max(4.0, std::ceil(maxEnd / 4.0) * 4.0);
                    const size_t orig = ns.size();
                    for (size_t i = 0; i < orig; ++i)
                    {
                        auto dup = ns[i];
                        dup.startBeat += span;
                        ns.push_back(dup);
                    }
                });
            }
            else if (r == 10)
            {
                fileChooser = std::make_unique<juce::FileChooser>("Import MIDI File", juce::File(), "*.mid;*.midi");
                fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                         [this](const juce::FileChooser& fc) {
                                             auto f = fc.getResult();
                                             std::vector<MidiNote> imported;
                                             if (f.existsAsFile() && MidiHelpers::importMidiFile(f, imported))
                                             {
                                                 notes = std::move(imported);
                                                 pushNotesToTrack();
                                                 repaint();
                                             }
                                         });
            }
            else if (r == 11)
            {
                auto t = engine.getTrack(selectedTrackIndex);
                const juce::String defName = (t != nullptr ? t->name : "Track") + ".mid";
                fileChooser = std::make_unique<juce::FileChooser>(
                    "Export MIDI File",
                    juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile(defName),
                    "*.mid");
                fileChooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                                         [this](const juce::FileChooser& fc) {
                                             auto f = fc.getResult();
                                             if (f != juce::File())
                                                 MidiHelpers::exportMidiFile(f, notes, engine.bpm.load());
                                         });
            }
        });
    }

    AudioEngine& engine;
    int selectedTrackIndex = 0;
    int lowestPitch = 36; // C2 (covers bass & guitar cleanly)
    static constexpr int visiblePitchRows = 24;

    std::vector<MidiNote> notes;
    int activeNoteIdx = -1;
    DragMode dragMode = DragMode::None;
    double dragStartNoteBeat = 0.0;
    double dragStartClickBeat = 0.0;
    int auditionPitch = -1;

    juce::ComboBox trackBox, gridBox, rootBox, scaleBox;
    juce::TextButton octDownBtn, octUpBtn, quantBtn, humanBtn, toolsBtn, clearBtn;
    std::unique_ptr<juce::FileChooser> fileChooser;
};
