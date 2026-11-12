#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

namespace mlm
{
/** Draws a QR code for a piece of text (dark modules on a white card with a quiet zone). */
class QrCodeComponent : public juce::Component
{
public:
    void setText (const juce::String& text);
    const juce::String& getText() const noexcept { return text; }

    /** Module matrix of the current code (true = dark). Exposed for tests. */
    int getSize() const noexcept { return size; }
    bool isDark (int x, int y) const noexcept { return modules[(size_t) (y * size + x)]; }

    void paint (juce::Graphics&) override;

private:
    juce::String text;
    int size = 0;
    std::vector<bool> modules;
};
}
