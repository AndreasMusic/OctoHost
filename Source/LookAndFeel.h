#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
#include <functional>

namespace OctoColours
{
    inline const juce::Colour bgDark      { 0xff0f1116 };
    inline const juce::Colour panelBg     { 0xff171a22 };
    inline const juce::Colour stripBg     { 0xff1c202a };
    inline const juce::Colour stripAlt    { 0xff212632 };
    inline const juce::Colour stripSel    { 0xff252c3b };
    inline const juce::Colour border      { 0xff2d3442 };
    inline const juce::Colour slotBg      { 0xff12151c };
    inline const juce::Colour slotActive  { 0xff1e3a4e };
    inline const juce::Colour slotBypass  { 0xff2e271f };
    inline const juce::Colour textMain    { 0xffe8ebf2 };
    inline const juce::Colour textDim     { 0xff8a94a8 };
    inline const juce::Colour accentTeal  { 0xff1abc9c };
    inline const juce::Colour accentAmber { 0xfff39c12 };
    inline const juce::Colour accentRed   { 0xffe74c3c };
    inline const juce::Colour accentBlue  { 0xff3498db };
    inline const juce::Colour accentPurp  { 0xff9b59b6 };
    inline const juce::Colour accentCyan  { 0xff00bcd4 };
}

class OctoLookAndFeel : public juce::LookAndFeel_V4
{
public:
    OctoLookAndFeel()
    {
        setColour(juce::ResizableWindow::backgroundColourId, OctoColours::bgDark);
        setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xff1a1e27));
        setColour(juce::PopupMenu::textColourId, OctoColours::textMain);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, OctoColours::accentTeal.withAlpha(0.32f));
        setColour(juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour(juce::TextButton::buttonColourId, juce::Colour(0xff262b36));
        setColour(juce::TextButton::textColourOffId, OctoColours::textMain);
        setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff1c202a));
        setColour(juce::ComboBox::outlineColourId, OctoColours::border);
        setColour(juce::ComboBox::textColourId, OctoColours::textMain);
        setColour(juce::Label::textColourId, OctoColours::textMain);
        setColour(juce::Slider::textBoxTextColourId, OctoColours::textDim);
        setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour(juce::ScrollBar::thumbColourId, juce::Colour(0xff3b4354));
    }

    void drawButtonBackground(juce::Graphics& g,
                              juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool isHighlighted,
                              bool isDown) override
    {
        auto b = button.getLocalBounds().toFloat().reduced(0.5f);
        auto col = backgroundColour;
        if (!button.isEnabled())
            col = col.withAlpha(0.38f);
        else if (isDown)
            col = col.brighter(0.18f);
        else if (isHighlighted)
            col = col.brighter(0.08f);

        g.setColour(col);
        g.fillRoundedRectangle(b, 3.0f);
        g.setColour(OctoColours::border.withAlpha(0.75f));
        g.drawRoundedRectangle(b, 3.0f, 1.0f);
    }

    void drawRotarySlider(juce::Graphics& g,
                          int x, int y, int width, int height,
                          float sliderPos,
                          float rotaryStartAngle,
                          float rotaryEndAngle,
                          juce::Slider& slider) override
    {
        const float radius = juce::jmin(width, height) * 0.42f;
        const float cx = x + width * 0.5f;
        const float cy = y + height * 0.5f;
        const float angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);

        juce::Path bgArc;
        bgArc.addCentredArc(cx, cy, radius, radius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
        g.setColour(juce::Colour(0xff101319));
        g.strokePath(bgArc, juce::PathStrokeType(2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        auto accent = slider.findColour(juce::Slider::rotarySliderFillColourId);
        if (accent.isTransparent())
            accent = OctoColours::accentTeal;

        const bool bipolar = (slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0);
        const float startArc = bipolar ? (rotaryStartAngle + 0.5f * (rotaryEndAngle - rotaryStartAngle))
                                       : rotaryStartAngle;
        juce::Path valArc;
        valArc.addCentredArc(cx, cy, radius, radius, 0.0f,
                             std::min(startArc, angle), std::max(startArc, angle), true);
        g.setColour(accent);
        g.strokePath(valArc, juce::PathStrokeType(2.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const float capR = radius * 0.72f;
        g.setColour(juce::Colour(0xff2a303d));
        g.fillEllipse(cx - capR, cy - capR, capR * 2.0f, capR * 2.0f);

        juce::Path ptr;
        ptr.addRectangle(-1.2f, -capR + 1.5f, 2.4f, capR * 0.65f);
        g.setColour(OctoColours::textMain);
        g.fillPath(ptr, juce::AffineTransform::rotation(angle).translated(cx, cy));
    }

    void drawLinearSlider(juce::Graphics& g,
                          int x, int y, int width, int height,
                          float sliderPos,
                          float minSliderPos,
                          float maxSliderPos,
                          juce::Slider::SliderStyle style,
                          juce::Slider& slider) override
    {
        if (style != juce::Slider::LinearVertical)
        {
            LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
            return;
        }

        const float cx = x + width * 0.5f;
        g.setColour(juce::Colour(0xff0b0d12));
        g.fillRoundedRectangle(cx - 2.5f, static_cast<float>(y), 5.0f, static_cast<float>(height), 2.0f);

        // Unity 0 dB tick mark (1.0 on 0..1.5 range)
        const double normUnity = slider.valueToProportionOfLength(1.0);
        const float unityY = y + height * static_cast<float>(1.0 - normUnity);
        g.setColour(OctoColours::textDim.withAlpha(0.55f));
        g.drawHorizontalLine(static_cast<int>(unityY), cx - 10.0f, cx + 10.0f);

        const float capW = std::min(22.0f, static_cast<float>(width) - 2.0f);
        const float capH = 14.0f;
        auto thumb = juce::Rectangle<float>(cx - capW * 0.5f, sliderPos - capH * 0.5f, capW, capH);
        g.setColour(juce::Colour(0xff3a4254));
        g.fillRoundedRectangle(thumb, 2.5f);
        g.setColour(OctoColours::border.brighter(0.25f));
        g.drawRoundedRectangle(thumb, 2.5f, 1.0f);
        g.setColour(OctoColours::textMain);
        g.drawHorizontalLine(static_cast<int>(sliderPos), thumb.getX() + 3.0f, thumb.getRight() - 3.0f);
    }
};

class ContextButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    std::function<void(const juce::MouseEvent&)> onRightClick;

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (onRightClick)
                onRightClick(e);
            return;
        }
        juce::TextButton::mouseDown(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        juce::TextButton::mouseUp(e);
    }
};

class ContextSlider : public juce::Slider
{
public:
    using juce::Slider::Slider;
    std::function<void(const juce::MouseEvent&)> onRightClick;

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (onRightClick)
                onRightClick(e);
            return;
        }
        juce::Slider::mouseDown(e);
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            return;
        juce::Slider::mouseUp(e);
    }
};
