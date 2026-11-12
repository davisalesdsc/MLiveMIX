#include "QrCodeComponent.h"
#include "qrcodegen/qrcodegen.hpp"

namespace mlm
{
void QrCodeComponent::setText (const juce::String& newText)
{
    if (newText == text && size > 0)
        return;

    text = newText;
    modules.clear();
    size = 0;

    if (text.isNotEmpty())
    {
        const auto qr = qrcodegen::QrCode::encodeText (text.toRawUTF8(), qrcodegen::QrCode::Ecc::MEDIUM);
        size = qr.getSize();
        modules.resize ((size_t) (size * size));
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                modules[(size_t) (y * size + x)] = qr.getModule (x, y);
    }
    repaint();
}

void QrCodeComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (juce::Colours::white);
    g.fillRoundedRectangle (bounds, 14.0f);

    if (size <= 0)
        return;

    const int quiet = 3;
    const int total = size + 2 * quiet;
    const float side = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const float cell = std::floor (side / (float) total);
    const float codeSide = cell * (float) total;
    const float x0 = bounds.getCentreX() - codeSide * 0.5f + cell * (float) quiet;
    const float y0 = bounds.getCentreY() - codeSide * 0.5f + cell * (float) quiet;

    g.setColour (juce::Colour (0xff0f1117));
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if (modules[(size_t) (y * size + x)])
                g.fillRect (x0 + (float) x * cell, y0 + (float) y * cell, cell, cell);
}
}
