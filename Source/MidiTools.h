#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>
#include "DrumEngine.h"

struct MidiNote
{
    int pitch = 60;          // 0..127
    double startBeat = 0.0;  // Quarter-note beats from 0.0
    double lengthBeats = 0.5;
    uint8_t velocity = 100;  // 1..127
    int channel = 1;         // 1..16
};

struct ScaleDefinition
{
    const char* name;
    std::vector<int> intervals;
};

namespace MusicTheory
{
    inline const std::array<const char*, 12> noteNames {{
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    }};

    inline const std::vector<ScaleDefinition>& getScales()
    {
        static const std::vector<ScaleDefinition> scales {
            { "Natural Minor",     { 0, 2, 3, 5, 7, 8, 10 } },
            { "Minor Pentatonic",  { 0, 3, 5, 7, 10 } },
            { "Phrygian",          { 0, 1, 3, 5, 7, 8, 10 } },
            { "Phrygian Dominant", { 0, 1, 4, 5, 7, 8, 10 } },
            { "Dorian",            { 0, 2, 3, 5, 7, 9, 10 } },
            { "Harmonic Minor",    { 0, 2, 3, 5, 7, 8, 11 } },
            { "Blues",             { 0, 3, 5, 6, 7, 10 } },
            { "Major",             { 0, 2, 4, 5, 7, 9, 11 } },
            { "Chromatic",         { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } }
        };
        return scales;
    }

    inline bool isPitchInScale(int pitch, int rootPc, int scaleIdx)
    {
        const auto& scales = getScales();
        const auto& ivs = scales[static_cast<size_t>(juce::jlimit(0, static_cast<int>(scales.size()) - 1, scaleIdx))].intervals;
        const int rel = (((pitch - rootPc) % 12) + 12) % 12;
        return std::find(ivs.begin(), ivs.end(), rel) != ivs.end();
    }

    inline int snapPitchToScale(int pitch, int rootPc, int scaleIdx)
    {
        pitch = juce::jlimit(12, 115, pitch);
        if (isPitchInScale(pitch, rootPc, scaleIdx))
            return pitch;
        for (int delta = 1; delta <= 6; ++delta)
        {
            if (isPitchInScale(pitch - delta, rootPc, scaleIdx)) return pitch - delta;
            if (isPitchInScale(pitch + delta, rootPc, scaleIdx)) return pitch + delta;
        }
        return pitch;
    }

    inline std::vector<int> buildScalePitches(int rootPc, int scaleIdx, int minPitch, int maxPitch)
    {
        std::vector<int> out;
        for (int p = minPitch; p <= maxPitch; ++p)
            if (isPitchInScale(p, rootPc, scaleIdx))
                out.push_back(p);
        if (out.empty())
            out.push_back(juce::jlimit(minPitch, maxPitch, 48 + rootPc));
        return out;
    }

    inline juce::String midiPitchName(int pitch)
    {
        const int pc = ((pitch % 12) + 12) % 12;
        const int oct = (pitch / 12) - 1;
        return juce::String(noteNames[static_cast<size_t>(pc)]) + juce::String(oct);
    }
}

namespace MidiHelpers
{
    inline std::vector<std::pair<int64_t, juce::MidiMessage>> notesToTimedEvents(
        const std::vector<MidiNote>& notes, double bpm, double sampleRate)
    {
        const double spb = (sampleRate * 60.0) / std::max(30.0, bpm);
        std::vector<std::pair<int64_t, juce::MidiMessage>> events;
        events.reserve(notes.size() * 2);

        for (const auto& n : notes)
        {
            const int ch = juce::jlimit(1, 16, n.channel);
            const int pitch = juce::jlimit(0, 127, n.pitch);
            const juce::uint8 vel = static_cast<juce::uint8>(juce::jlimit(1, 127, static_cast<int>(n.velocity)));
            const int64_t onPos = std::max<int64_t>(0, static_cast<int64_t>(std::llround(n.startBeat * spb)));
            const int64_t offPos = std::max<int64_t>(onPos + 16,
                                                     static_cast<int64_t>(std::llround((n.startBeat + std::max(0.0625, n.lengthBeats)) * spb)));

            events.emplace_back(onPos, juce::MidiMessage::noteOn(ch, pitch, vel));
            events.emplace_back(offPos, juce::MidiMessage::noteOff(ch, pitch));
        }

        std::sort(events.begin(), events.end(),
                  [](const auto& a, const auto& b) {
                      if (a.first != b.first) return a.first < b.first;
                      // NoteOff before NoteOn at identical sample timestamps to prevent choked re-triggers
                      return a.second.isNoteOff() && b.second.isNoteOn();
                  });
        return events;
    }

    inline std::vector<MidiNote> timedEventsToNotes(
        const std::vector<std::pair<int64_t, juce::MidiMessage>>& events, double bpm, double sampleRate)
    {
        const double spb = (sampleRate * 60.0) / std::max(30.0, bpm);
        std::vector<MidiNote> notes;
        std::array<int, 128> openIdx;
        openIdx.fill(-1);

        for (const auto& kv : events)
        {
            const auto& msg = kv.second;
            const double beat = spb > 0.0 ? static_cast<double>(kv.first) / spb : 0.0;
            if (msg.isNoteOn() && msg.getVelocity() > 0)
            {
                const int p = msg.getNoteNumber();
                if (openIdx[static_cast<size_t>(p)] >= 0)
                {
                    auto& prev = notes[static_cast<size_t>(openIdx[static_cast<size_t>(p)])];
                    prev.lengthBeats = std::max(0.125, beat - prev.startBeat);
                }
                MidiNote n;
                n.pitch = p;
                n.startBeat = beat;
                n.lengthBeats = 0.25;
                n.velocity = msg.getVelocity();
                n.channel = msg.getChannel();
                openIdx[static_cast<size_t>(p)] = static_cast<int>(notes.size());
                notes.push_back(n);
            }
            else if (msg.isNoteOff() || (msg.isNoteOn() && msg.getVelocity() == 0))
            {
                const int p = msg.getNoteNumber();
                const int idx = openIdx[static_cast<size_t>(p)];
                if (idx >= 0 && idx < static_cast<int>(notes.size()))
                {
                    notes[static_cast<size_t>(idx)].lengthBeats = std::max(0.125, beat - notes[static_cast<size_t>(idx)].startBeat);
                    openIdx[static_cast<size_t>(p)] = -1;
                }
            }
        }
        return notes;
    }

    inline void quantizeNotes(std::vector<MidiNote>& notes, double gridBeats)
    {
        if (gridBeats <= 0.0) return;
        for (auto& n : notes)
            n.startBeat = std::max(0.0, std::round(n.startBeat / gridBeats) * gridBeats);
    }

    inline void humanizeNotes(std::vector<MidiNote>& notes, double maxTimingBeats, int maxVelDelta)
    {
        static std::mt19937 rng { std::random_device{}() };
        std::uniform_real_distribution<double> tDist(-maxTimingBeats, maxTimingBeats);
        std::uniform_int_distribution<int> vDist(-maxVelDelta, maxVelDelta);
        for (auto& n : notes)
        {
            n.startBeat = std::max(0.0, n.startBeat + tDist(rng));
            n.velocity = static_cast<uint8_t>(juce::jlimit(25, 127, static_cast<int>(n.velocity) + vDist(rng)));
        }
    }

    inline void transposeNotes(std::vector<MidiNote>& notes, int semitones)
    {
        for (auto& n : notes)
            n.pitch = juce::jlimit(12, 115, n.pitch + semitones);
    }

    inline void applyLegato(std::vector<MidiNote>& notes)
    {
        if (notes.size() < 2) return;
        std::sort(notes.begin(), notes.end(),
                  [](const MidiNote& a, const MidiNote& b) { return a.startBeat < b.startBeat; });
        for (size_t i = 0; i + 1 < notes.size(); ++i)
        {
            const double gap = notes[i + 1].startBeat - notes[i].startBeat;
            if (gap > 0.05)
                notes[i].lengthBeats = std::max(0.125, gap - 0.02);
        }
    }

    inline bool exportMidiFile(const juce::File& file, const std::vector<MidiNote>& notes, double bpm)
    {
        juce::MidiFile mf;
        const int tpq = 480;
        mf.setTicksPerQuarterNote(tpq);

        juce::MidiMessageSequence seq;
        seq.addEvent(juce::MidiMessage::tempoMetaEvent(static_cast<int>(60000000.0 / std::max(30.0, bpm))), 0.0);

        for (const auto& n : notes)
        {
            const double onTick = n.startBeat * tpq;
            const double offTick = (n.startBeat + std::max(0.0625, n.lengthBeats)) * tpq;
            seq.addEvent(juce::MidiMessage::noteOn(n.channel, n.pitch, n.velocity), onTick);
            seq.addEvent(juce::MidiMessage::noteOff(n.channel, n.pitch), offTick);
        }
        seq.updateMatchedPairs();
        mf.addTrack(seq);

        file.deleteFile();
        auto out = file.createOutputStream();
        if (out == nullptr) return false;
        return mf.writeTo(*out);
    }

    inline bool importMidiFile(const juce::File& file, std::vector<MidiNote>& outNotes)
    {
        auto in = file.createInputStream();
        if (in == nullptr) return false;

        juce::MidiFile mf;
        if (!mf.readFrom(*in)) return false;

        const short timeFmt = mf.getTimeFormat();
        const double tpq = (timeFmt > 0) ? static_cast<double>(timeFmt) : 480.0;
        outNotes.clear();

        for (int t = 0; t < mf.getNumTracks(); ++t)
        {
            const auto* seq = mf.getTrack(t);
            if (seq == nullptr) continue;
            for (int i = 0; i < seq->getNumEvents(); ++i)
            {
                const auto* ev = seq->getEventPointer(i);
                if (ev->message.isNoteOn() && ev->message.getVelocity() > 0)
                {
                    MidiNote n;
                    n.pitch = ev->message.getNoteNumber();
                    n.startBeat = ev->message.getTimeStamp() / tpq;
                    n.velocity = ev->message.getVelocity();
                    n.channel = ev->message.getChannel();
                    if (ev->noteOffObject != nullptr)
                        n.lengthBeats = std::max(0.125, (ev->noteOffObject->message.getTimeStamp() - ev->message.getTimeStamp()) / tpq);
                    else
                        n.lengthBeats = 0.25;
                    outNotes.push_back(n);
                }
            }
        }
        return !outNotes.empty();
    }
}

// --- Smart Riff, Bassline & Auto-Melody Forge ---
class MidiAutoCreator
{
public:
    enum class Style
    {
        BasslinePocket = 0,
        ProgMetalRiff  = 1,
        LeadMelody     = 2,
        PowerChords    = 3,
        AtmosphericPad = 4,
        SynthArp       = 5
    };

    struct Params
    {
        Style style = Style::BasslinePocket;
        int rootPc = 4;        // E (guitar/bass standard!)
        int scaleIdx = 0;      // Natural Minor
        int octave = 2;        // E2=40 (bass), E3=52 (guitar), E4=64 (lead)
        int numBars = 4;       // 2, 4, 8
        float density = 0.65f; // 0.2..1.0
        float syncopation = 0.45f;
        bool lockToDrumKick = true;
    };

    static std::vector<MidiNote> generate(const Params& p, const DrumEngine& drumEngine)
    {
        static std::mt19937 rng { std::random_device{}() };
        std::uniform_real_distribution<float> u01(0.0f, 1.0f);

        const int baseRoot = juce::jlimit(24, 72, (p.octave + 1) * 12 + p.rootPc);
        const auto scale = MusicTheory::buildScalePitches(p.rootPc, p.scaleIdx, baseRoot, baseRoot + 24);
        const auto getScaleDeg = [&](int deg) -> int {
            if (scale.empty()) return baseRoot;
            const int idx = juce::jlimit(0, static_cast<int>(scale.size()) - 1, deg);
            return scale[static_cast<size_t>(idx)];
        };

        // Choose a strong 4-bar chord root degree progression (0-based in scale)
        static const std::array<std::array<int, 4>, 5> progs {{
            { 0, 5, 2, 6 }, // i - VI - III - VII (epic rock/metal)
            { 0, 3, 5, 4 }, // i - iv - VI - v
            { 0, 0, 5, 6 }, // i - i - VI - VII (driving riff pedal)
            { 0, 1, 0, 6 }, // i - bII - i - VII (dark Phrygian / Tool-ish)
            { 0, 2, 3, 5 }  // i - III - iv - VI
        }};
        const auto& prog = progs[static_cast<size_t>(rng() % progs.size())];

        std::array<uint8_t, DrumEngine::maxSteps> kickMask {};
        int kickSteps = 16;
        drumEngine.getActiveKickMask(kickMask, kickSteps);

        std::vector<MidiNote> out;

        if (p.style == Style::BasslinePocket)
        {
            for (int bar = 0; bar < p.numBars; ++bar)
            {
                const int barDeg = prog[static_cast<size_t>(bar % 4)];
                const int barRoot = getScaleDeg(barDeg);
                const int barFifth = MusicTheory::snapPitchToScale(barRoot + 7, p.rootPc, p.scaleIdx);
                const int barOct = std::min(84, barRoot + 12);

                for (int step = 0; step < 16; ++step)
                {
                    const bool isKick = (kickMask[static_cast<size_t>(step % std::max(1, kickSteps))] > 0);
                    bool trigger = false;

                    if (p.lockToDrumKick && isKick)
                        trigger = true;
                    else if (step == 0)
                        trigger = true;
                    else if (step % 2 == 0 && u01(rng) < p.density * 0.55f)
                        trigger = true;
                    else if (step % 2 == 1 && u01(rng) < p.syncopation * p.density * 0.45f)
                        trigger = true;

                    if (!trigger) continue;

                    MidiNote n;
                    n.startBeat = bar * 4.0 + step * 0.25;
                    n.lengthBeats = (step % 4 == 0) ? 0.45 : 0.22;
                    n.velocity = static_cast<uint8_t>(isKick ? 116 : (84 + (rng() % 28)));

                    // Groove note selection: root anchor, fifth, octave pop, turnaround pickup on step 14-15
                    if (step == 0)
                        n.pitch = barRoot;
                    else if (step >= 14)
                        n.pitch = getScaleDeg(barDeg + (u01(rng) < 0.5f ? 1 : 6));
                    else
                    {
                        const float r = u01(rng);
                        if (r < 0.52f)      n.pitch = barRoot;
                        else if (r < 0.76f) n.pitch = barFifth;
                        else if (r < 0.90f) n.pitch = barOct;
                        else                n.pitch = getScaleDeg(barDeg + static_cast<int>(rng() % 4));
                    }
                    out.push_back(n);
                }
            }
        }
        else if (p.style == Style::ProgMetalRiff)
        {
            // Build a memorable 2-bar motif and repeat with variation
            std::vector<MidiNote> motif;
            int curDeg = 0;
            for (int step = 0; step < 32; ++step)
            {
                const bool kickHit = kickMask[static_cast<size_t>(step % std::max(1, kickSteps))] > 0;
                const bool hit = (step == 0) || (p.lockToDrumKick && kickHit)
                                 || (u01(rng) < p.density * ((step % 2 == 0) ? 0.65f : (0.4f + p.syncopation * 0.4f)));
                if (!hit) continue;

                MidiNote n;
                n.startBeat = step * 0.25;
                // Pedal tone back to root frequently like a real guitar riff
                if (u01(rng) < 0.48f)
                {
                    n.pitch = baseRoot;
                    n.lengthBeats = 0.22;
                    n.velocity = static_cast<uint8_t>(105 + (rng() % 20));
                }
                else
                {
                    const int jump = static_cast<int>(rng() % 5) - 2;
                    curDeg = juce::jlimit(1, std::min(7, static_cast<int>(scale.size()) - 1), curDeg + jump);
                    n.pitch = getScaleDeg(curDeg);
                    n.lengthBeats = (u01(rng) < 0.35f) ? 0.48 : 0.24;
                    n.velocity = 122;
                }
                motif.push_back(n);
            }

            for (int rep = 0; rep < std::max(1, p.numBars / 2); ++rep)
            {
                for (auto n : motif)
                {
                    n.startBeat += rep * 8.0;
                    if (rep % 2 == 1 && n.startBeat >= (rep * 8.0 + 6.0) && u01(rng) < 0.5f)
                        n.pitch = MusicTheory::snapPitchToScale(n.pitch + 3, p.rootPc, p.scaleIdx);
                    out.push_back(n);
                }
            }
        }
        else if (p.style == Style::LeadMelody)
        {
            // Call-and-response contour melody
            int deg = 4;
            double beat = 0.0;
            const double totalBeats = p.numBars * 4.0;
            while (beat < totalBeats - 0.1)
            {
                const int bar = static_cast<int>(beat / 4.0);
                const bool phraseEnd = (std::fmod(beat, 8.0) >= 6.5);

                if (phraseEnd && u01(rng) < 0.45f)
                {
                    beat += 0.5;
                    continue; // Breathing space
                }

                static const double durations[4] = { 0.25, 0.5, 0.75, 1.0 };
                double dur = durations[static_cast<size_t>(rng() % 4)];
                if (p.density > 0.75f && u01(rng) < 0.5f) dur = 0.25;
                if (p.density < 0.45f && u01(rng) < 0.5f) dur = 1.0;
                dur = std::min(dur, totalBeats - beat);

                if (phraseEnd)
                    deg = (bar % 2 == 0) ? 4 : 0; // Resolve cleanly to 5th or Root
                else
                {
                    const int stepMove = (u01(rng) < 0.72f) ? ((rng() % 3) - 1) : ((rng() % 5) - 2);
                    deg = juce::jlimit(0, static_cast<int>(scale.size()) - 1, deg + stepMove);
                }

                MidiNote n;
                n.pitch = getScaleDeg(deg);
                n.startBeat = beat;
                n.lengthBeats = dur * 0.92;
                n.velocity = static_cast<uint8_t>(85 + (rng() % 38));
                out.push_back(n);

                beat += dur;
                if (u01(rng) < (1.0f - p.density) * 0.4f)
                    beat += 0.25;
            }
        }
        else if (p.style == Style::PowerChords)
        {
            for (int bar = 0; bar < p.numBars; ++bar)
            {
                const int root = getScaleDeg(prog[static_cast<size_t>(bar % 4)]);
                const int fifth = root + 7;
                const int oct = root + 12;

                for (int step = 0; step < 16; step += 2)
                {
                    const bool isKick = kickMask[static_cast<size_t>(step % std::max(1, kickSteps))] > 0;
                    const bool hit = (step == 0) || (p.lockToDrumKick && isKick) || (u01(rng) < p.density * 0.55f);
                    if (!hit) continue;

                    const double pos = bar * 4.0 + step * 0.25;
                    const double len = (step == 0 || step == 8) ? 0.9 : 0.42;
                    const uint8_t vel = static_cast<uint8_t>(step == 0 ? 124 : (95 + (rng() % 24)));
                    for (int pitch : { root, fifth, oct })
                        out.push_back({ juce::jlimit(24, 96, pitch), pos, len, vel, 1 });
                }
            }
        }
        else if (p.style == Style::AtmosphericPad)
        {
            for (int bar = 0; bar < p.numBars; ++bar)
            {
                const int deg = prog[static_cast<size_t>(bar % 4)];
                const double pos = bar * 4.0;
                // Rich triad + 7th/9th voicing
                for (int offset : { 0, 2, 4, 6 })
                {
                    MidiNote n;
                    n.pitch = getScaleDeg(deg + offset);
                    n.startBeat = pos;
                    n.lengthBeats = 3.92;
                    n.velocity = static_cast<uint8_t>(74 + (rng() % 18));
                    out.push_back(n);
                }
            }
        }
        else // SynthArp
        {
            const double stepDur = (p.density > 0.6f) ? 0.25 : 0.5;
            const int stepsPerBar = static_cast<int>(std::round(4.0 / stepDur));
            static const int arpOrder[8] = { 0, 2, 4, 7, 4, 2, 5, 4 };

            for (int bar = 0; bar < p.numBars; ++bar)
            {
                const int deg = prog[static_cast<size_t>(bar % 4)];
                for (int s = 0; s < stepsPerBar; ++s)
                {
                    if (u01(rng) > (0.55f + p.density * 0.45f)) continue;
                    MidiNote n;
                    n.pitch = getScaleDeg(deg + arpOrder[static_cast<size_t>(s % 8)]);
                    n.startBeat = bar * 4.0 + s * stepDur;
                    n.lengthBeats = stepDur * 0.82;
                    n.velocity = static_cast<uint8_t>((s % 4 == 0) ? 116 : (82 + (rng() % 26)));
                    out.push_back(n);
                }
            }
        }

        return out;
    }

    static void mutate(std::vector<MidiNote>& notes, int rootPc, int scaleIdx)
    {
        if (notes.empty()) return;
        static std::mt19937 rng { std::random_device{}() };
        std::uniform_real_distribution<float> u01(0.0f, 1.0f);

        for (auto& n : notes)
        {
            const float r = u01(rng);
            if (r < 0.20f)
            {
                const int delta = (rng() % 2 == 0) ? 2 : -2;
                n.pitch = MusicTheory::snapPitchToScale(n.pitch + delta, rootPc, scaleIdx);
            }
            else if (r < 0.28f)
            {
                n.pitch = juce::jlimit(24, 96, n.pitch + ((rng() % 2 == 0) ? 12 : -12));
            }
            n.velocity = static_cast<uint8_t>(juce::jlimit(45, 127, static_cast<int>(n.velocity) + static_cast<int>(rng() % 21) - 10));
        }
    }
};
