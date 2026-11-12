#include "PluginEditor.h"
#include "net/NetUtil.h"

namespace
{
    const juce::Colour bg      { 0xff0f1117 };
    const juce::Colour panel   { 0xff171b24 };
    const juce::Colour edge    { 0xff262c3b };
    const juce::Colour text    { 0xffe6e9f2 };
    const juce::Colour dim     { 0xff8b93a7 };
    const juce::Colour accent  { 0xffff4d6d };
    const juce::Colour accent2 { 0xffffb84d };
    const juce::Colour ok      { 0xff36e2b4 };

    juce::String pt (const char* s) { return juce::String::fromUTF8 (s); }

    void card (juce::Graphics& g, juce::Rectangle<int> r)
    {
        g.setColour (panel);
        g.fillRoundedRectangle (r.toFloat(), 14.0f);
        g.setColour (edge);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 14.0f, 1.0f);
    }
}

//==============================================================================
MLiveMixAudioProcessorEditor::LookAndFeel::LookAndFeel()
{
    setColour (juce::TextButton::buttonColourId, panel);
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, bg);
    setColour (juce::Label::textColourId, text);
}

void MLiveMixAudioProcessorEditor::LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                                                       const juce::Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    auto fill = b.getToggleState() ? accent : panel.brighter (0.05f);
    if (over) fill = fill.brighter (0.08f);
    if (down) fill = fill.darker (0.1f);
    g.setColour (fill);
    g.fillRoundedRectangle (r, 10.0f);
    g.setColour (b.getToggleState() ? accent : edge.brighter (0.2f));
    g.drawRoundedRectangle (r, 10.0f, 1.0f);
}

juce::Font MLiveMixAudioProcessorEditor::LookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    return juce::Font (juce::FontOptions (13.0f, juce::Font::bold));
}

//==============================================================================
MLiveMixAudioProcessorEditor::MLiveMixAudioProcessorEditor (MLiveMixAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (qr);

    urlLabel.setJustificationType (juce::Justification::centred);
    urlLabel.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
    addAndMakeVisible (urlLabel);

    statusLabel.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    addAndMakeVisible (statusLabel);

    openHereButton.setButtonText (pt ("ABRIR NESTE COMPUTADOR"));
    openHereButton.onClick = [this]
    {
        if (remoteMode)
        {
            const auto url = processor.getRemoteShareUrl();
            if (url.isNotEmpty())
                juce::URL (url).launchInDefaultBrowser();
            return;
        }
        const int port = processor.getServer().getPort();
        if (port > 0)
            juce::URL ("http://localhost:" + juce::String (port) + "/").launchInDefaultBrowser();
    };

    copyButton.setButtonText ("COPIAR LINK");
    copyButton.onClick = [this]
    {
        juce::SystemClipboard::copyTextToClipboard (currentUrl());
        copyButton.setButtonText ("COPIADO!");
        juce::Timer::callAfterDelay (1500, [safe = juce::Component::SafePointer<juce::TextButton> (&copyButton)]
        {
            if (safe != nullptr) safe->setButtonText ("COPIAR LINK");
        });
    };

    switchNetworkButton.setButtonText ("TROCAR REDE");
    switchNetworkButton.onClick = [this]
    {
        if (addresses.size() > 1)
        {
            addressIndex = (addressIndex + 1) % addresses.size();
            processor.preferredAddress = addresses[addressIndex];
            updateStatus();
        }
    };

    retryButton.setButtonText ("TENTAR DE NOVO");
    retryButton.onClick = [this] { processor.ensureServerRunning(); refreshAddresses(); updateStatus(); };

    streamButton.setClickingTogglesState (true);
    streamButton.setToggleState (processor.isStreaming(), juce::dontSendNotification);
    streamButton.onClick = [this]
    {
        processor.setStreaming (streamButton.getToggleState());
        updateStatus();
    };

    // Tabs: local network / internet
    localTab.setButtonText ("NA REDE (WI-FI)");
    remoteTab.setButtonText ("REMOTO (INTERNET)");
    localTab.onClick  = [this] { remoteMode = false; updateStatus(); resized(); repaint(); };
    remoteTab.onClick = [this] { remoteMode = true;  updateStatus(); resized(); repaint(); };

    remoteStartButton.setClickingTogglesState (true);
    remoteStartButton.setToggleState (processor.getRemote().isOn(), juce::dontSendNotification);
    remoteStartButton.onClick = [this]
    {
        if (remoteStartButton.getToggleState())
            processor.startRemote();
        else
            processor.stopRemote();
        updateStatus();
    };

    newLinkButton.setButtonText ("NOVO LINK");
    newLinkButton.onClick = [this]
    {
        if (! confirmNewLink)
        {
            // First click asks for confirmation: the old link will stop working
            confirmNewLink = true;
            newLinkButton.setButtonText ("CONFIRMAR NOVO LINK?");
            juce::Timer::callAfterDelay (4000, [safe = juce::Component::SafePointer<MLiveMixAudioProcessorEditor> (this)]
            {
                if (safe != nullptr && safe->confirmNewLink)
                {
                    safe->confirmNewLink = false;
                    safe->newLinkButton.setButtonText ("NOVO LINK");
                }
            });
            return;
        }
        confirmNewLink = false;
        newLinkButton.setButtonText ("NOVO LINK");
        processor.newRemoteLink();
        updateStatus();
    };

    for (auto* b : { &openHereButton, &copyButton, &switchNetworkButton, &retryButton, &streamButton,
                     &localTab, &remoteTab, &remoteStartButton, &newLinkButton })
        addAndMakeVisible (b);

    processor.ensureServerRunning();
    refreshAddresses();

    setSize (480, 810);
    updateStatus();
    startTimerHz (15);
}

MLiveMixAudioProcessorEditor::~MLiveMixAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void MLiveMixAudioProcessorEditor::refreshAddresses()
{
    addresses = mlm::net::getLanAddresses();
    const int saved = addresses.indexOf (processor.preferredAddress);
    addressIndex = saved >= 0 ? saved : 0;
}

juce::String MLiveMixAudioProcessorEditor::getListenUrl() const
{
    const int port = processor.getServer().getPort();
    if (port <= 0)
        return {};
    const auto host = addresses.isEmpty() ? juce::String ("localhost") : addresses[addressIndex];
    return "http://" + host + ":" + juce::String (port) + "/";
}

juce::String MLiveMixAudioProcessorEditor::currentUrl()
{
    return remoteMode ? processor.getRemoteShareUrl() : getListenUrl();
}

void MLiveMixAudioProcessorEditor::updateStatus()
{
    localTab.setToggleState (! remoteMode, juce::dontSendNotification);
    remoteTab.setToggleState (remoteMode, juce::dontSendNotification);

    for (auto* b : { &switchNetworkButton, &retryButton, &streamButton })
        b->setVisible (! remoteMode);
    for (auto* b : { &remoteStartButton, &newLinkButton })
        b->setVisible (remoteMode);
    openHereButton.setButtonText (remoteMode ? pt ("ABRIR NO NAVEGADOR") : pt ("ABRIR NESTE COMPUTADOR"));

    if (remoteMode)
    {
        auto& remote = processor.getRemote();
        const bool available = processor.isRemoteAvailable();
        const auto url = available ? processor.getRemoteShareUrl() : juce::String();

        qr.setText (url);
        urlLabel.setText (url.isNotEmpty() ? url.fromFirstOccurrenceOf ("://", false, false) : pt ("—"),
                          juce::dontSendNotification);
        urlLabel.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        urlLabel.setMinimumHorizontalScale (0.6f);

        remoteStartButton.setEnabled (available);
        newLinkButton.setEnabled (available);
        copyButton.setEnabled (available);
        openHereButton.setEnabled (available);
        remoteStartButton.setToggleState (remote.isOn(), juce::dontSendNotification);
        remoteStartButton.setButtonText (remote.isOn() ? pt ("PARAR TRANSMISSÃO") : pt ("INICIAR TRANSMISSÃO"));

        juce::String status;
        using S = mlm::RemoteLink::State;
        if (! available)
        {
            status = pt ("Servidor remoto ainda não configurado nesta versão");
            statusColour = accent2;
        }
        else switch (remote.getState())
        {
            case S::off:
                status = pt ("Desligado — clique em INICIAR TRANSMISSÃO");
                statusColour = dim.brighter (0.3f);
                break;
            case S::connecting:
                status = pt ("Conectando ao servidor…");
                statusColour = accent2;
                break;
            case S::live:
            {
                const int n = remote.getListeners();
                status = n == 0 ? pt ("No ar — aguardando ouvintes")
                                : (n == 1 ? pt ("Ao vivo pela internet — 1 ouvinte")
                                          : pt ("Ao vivo pela internet — ") + juce::String (n) + " ouvintes");
                statusColour = n > 0 ? ok : dim.brighter (0.3f);
                break;
            }
            case S::retrying:
                status = pt ("Sem conexão — tentando de novo. ") + remote.getLastError();
                statusColour = accent;
                break;
            case S::unavailable:
            default:
                status = remote.getLastError();
                statusColour = accent;
                break;
        }
        statusLabel.setText (status, juce::dontSendNotification);
        statusLabel.setColour (juce::Label::textColourId, statusColour);
        repaint (statusDot.expanded (2));
        return;
    }

    copyButton.setEnabled (true);
    openHereButton.setEnabled (true);
    urlLabel.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));

    auto& server = processor.getServer();
    const auto url = getListenUrl();

    qr.setText (url);
    urlLabel.setText (url.isNotEmpty() ? url.fromFirstOccurrenceOf ("http://", false, false).trimCharactersAtEnd ("/")
                                       : pt ("—"),
                      juce::dontSendNotification);

    switchNetworkButton.setVisible (addresses.size() > 1);
    retryButton.setVisible (! server.isRunning());
    streamButton.setButtonText (processor.isStreaming() ? "TRANSMITINDO" : pt ("TRANSMISSÃO PAUSADA"));
    streamButton.setToggleState (processor.isStreaming(), juce::dontSendNotification);

    juce::String status;
    if (! server.isRunning())
    {
        status = pt ("Servidor desligado. ") + server.getLastError();
        statusColour = accent;
    }
    else if (addresses.isEmpty())
    {
        status = pt ("Sem rede Wi-Fi/cabo: só funciona neste computador");
        statusColour = accent2;
    }
    else if (! processor.isStreaming())
    {
        status = pt ("Transmissão pausada");
        statusColour = accent2;
    }
    else
    {
        const int n = server.getNumListeners();
        status = n == 0 ? pt ("Pronto — nenhum ouvinte conectado")
                        : (n == 1 ? pt ("Ao vivo — 1 ouvinte") : pt ("Ao vivo — ") + juce::String (n) + " ouvintes");
        statusColour = n > 0 ? ok : dim.brighter (0.3f);
    }
    statusLabel.setText (status, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, statusColour);
    repaint (statusDot.expanded (2));
}

void MLiveMixAudioProcessorEditor::timerCallback()
{
    auto fall = [] (float current, float peak) { return juce::jmax (peak, current * 0.85f); };
    meterL = fall (meterL, processor.peakLeft.exchange (0.0f));
    meterR = fall (meterR, processor.peakRight.exchange (0.0f));
    repaint (meterArea);

    if (++slowCounter >= 8) // ~2x per second
    {
        slowCounter = 0;
        if (addresses.isEmpty())
            refreshAddresses();
        updateStatus();
    }
}

//==============================================================================
void MLiveMixAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (bg);

    auto r = getLocalBounds().reduced (20);
    auto header = r.removeFromTop (40);
    const juce::Font brand (juce::FontOptions (26.0f, juce::Font::bold));
    g.setFont (brand);
    const int liveW = (int) std::ceil (juce::GlyphArrangement::getStringWidth (brand, "MLive"));
    g.setColour (text);
    g.drawText ("MLive", header.removeFromLeft (liveW), juce::Justification::centredLeft);
    g.setColour (accent);
    g.drawText ("MIX", header.removeFromLeft (60), juce::Justification::centredLeft);
    g.setColour (dim);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.drawText (pt ("ouça sua mix no celular · v") + JucePlugin_VersionString, header, juce::Justification::centredRight);

    // Status dot
    g.setColour (statusColour);
    g.fillEllipse (statusDot.toFloat());

    // Meters
    card (g, meterArea);
    auto m = meterArea.reduced (14, 12);
    g.setColour (dim);
    g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    g.drawText (pt ("SAÍDA DO MASTER"), m.removeFromTop (14), juce::Justification::centredLeft);
    m.removeFromTop (6);
    for (float v : { meterL, meterR })
    {
        auto bar = m.removeFromTop (8).toFloat();
        m.removeFromTop (6);
        g.setColour (bg);
        g.fillRoundedRectangle (bar, 4.0f);
        const float db = juce::Decibels::gainToDecibels (v, -60.0f);
        const float w = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
        juce::ColourGradient grad (ok, bar.getX(), 0, accent, bar.getRight(), 0, false);
        grad.addColour (0.75, accent2);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * w), 4.0f);
    }

    // Steps
    card (g, stepsArea);
    auto s = stepsArea.reduced (16, 12);
    g.setFont (juce::Font (juce::FontOptions (13.0f)));
    const char* localSteps[] = {
        "1.  Celular na mesma rede Wi-Fi deste computador",
        "2.  Aponte a câmera para o QR Code",
        "3.  Toque em OUVIR na página que abrir",
    };
    const char* remoteSteps[] = {
        "1.  Clique em INICIAR TRANSMISSÃO",
        "2.  COPIAR LINK e mande por WhatsApp (ou QR Code)",
        "3.  A pessoa abre o link e toca em OUVIR",
    };
    for (auto* line : remoteMode ? remoteSteps : localSteps)
    {
        g.setColour (text);
        g.drawText (pt (line), s.removeFromTop (20), juce::Justification::centredLeft);
    }
    s.removeFromTop (4);
    g.setColour (dim);
    g.setFont (juce::Font (juce::FontOptions (11.5f)));
    g.drawFittedText (remoteMode ? pt ("O link é fixo neste projeto: quem tiver o link ouve de qualquer lugar. "
                                       "NOVO LINK faz o link antigo parar de funcionar.")
                                 : pt ("Se o Mac perguntar, permita conexões de entrada para a sua DAW. "
                                       "O som da DAW não é alterado: o plugin só envia uma cópia."),
                      s, juce::Justification::topLeft, 3, 1.0f); // wrap, never squash
}

void MLiveMixAudioProcessorEditor::resized()
{
    auto r = getLocalBounds().reduced (20);
    r.removeFromTop (48);

    auto tabs = r.removeFromTop (34);
    const int tabW = (tabs.getWidth() - 8) / 2;
    localTab.setBounds (tabs.removeFromLeft (tabW));
    tabs.removeFromLeft (8);
    remoteTab.setBounds (tabs);
    r.removeFromTop (12);

    auto statusRow = r.removeFromTop (24);
    statusDot = statusRow.removeFromLeft (14).withSizeKeepingCentre (10, 10);
    statusRow.removeFromLeft (6);
    statusLabel.setBounds (statusRow);
    r.removeFromTop (10);

    const int qrSide = 280;
    qrCard = r.removeFromTop (qrSide).withSizeKeepingCentre (qrSide, qrSide);
    qr.setBounds (qrCard);
    r.removeFromTop (8);
    urlLabel.setBounds (r.removeFromTop (26));
    r.removeFromTop (10);

    auto buttons = r.removeFromTop (36);
    const int w = (buttons.getWidth() - 8) / 2;
    copyButton.setBounds (buttons.removeFromLeft (w));
    buttons.removeFromLeft (8);
    openHereButton.setBounds (buttons);
    r.removeFromTop (8);

    auto second = r.removeFromTop (36);
    const auto secondLeft = second.removeFromLeft (w);
    second.removeFromLeft (8);
    streamButton.setBounds (secondLeft);
    switchNetworkButton.setBounds (second);
    retryButton.setBounds (second);
    remoteStartButton.setBounds (secondLeft);
    newLinkButton.setBounds (second);
    r.removeFromTop (12);

    meterArea = r.removeFromTop (64);
    r.removeFromTop (12);
    stepsArea = r;
}
