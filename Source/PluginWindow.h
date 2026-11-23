#pragma once
#include <utility>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow(juce::AudioProcessor& processor,
                 const juce::String& title,
                 std::function<void()> onCloseCallback)
        : DocumentWindow(title,
                         juce::Colour(0xff171a22),
                         DocumentWindow::minimiseButton | DocumentWindow::closeButton),
          onClose(std::move(onCloseCallback))
    {
        setUsingNativeTitleBar(true);

        juce::AudioProcessorEditor* editor = processor.createEditorIfNeeded();
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor(processor);

        setContentOwned(editor, true);
        setResizable(editor->isResizable(), false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose();
    }

private:
    std::function<void()> onClose;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginWindow)
};
