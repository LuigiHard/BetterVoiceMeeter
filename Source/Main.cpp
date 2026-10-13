#include <JuceHeader.h>
#include "Engine.h"
#include "UI/MainComponent.h"

class BetterVoiceMeeterApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String& commandLine) override
    {
        logger.reset (juce::FileLogger::createDefaultAppLogger ("BetterVoiceMeeter", "BetterVoiceMeeter.log",
                                                                "BetterVoiceMeeter log"));
        juce::Logger::setCurrentLogger (logger.get());
        engine = std::make_unique<Engine>();
        theme::apply (theme::styleFromName (engine->uiTheme));
        window = std::make_unique<MainWindow> (*engine);
        handleCommandLine (commandLine);
    }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (window != nullptr) window->toFront (true);
        handleCommandLine (commandLine);
    }

    // BetterVoiceMeeter.exe --sing <songId> [--seek <seconds>] [--play]
    void handleCommandLine (const juce::String& commandLine)
    {
        auto args = juce::StringArray::fromTokens (commandLine, true);
        if (const int t = args.indexOf ("--theme"); t >= 0 && t + 1 < args.size() && engine != nullptr && engine->onThemeChanged)
        {
            engine->uiTheme = args[t + 1].unquoted();
            engine->onThemeChanged();
        }
        if (args.contains ("--apps") && window != nullptr)
            if (auto* mc = dynamic_cast<MainComponent*> (window->getContentComponent()))
                mc->toggleAppMixer();
        const int i = args.indexOf ("--sing");
        if (i < 0 || i + 1 >= args.size() || window == nullptr) return;
        const int s = args.indexOf ("--seek");
        const double seek = s >= 0 && s + 1 < args.size() ? args[s + 1].getDoubleValue() : 0.0;
        if (auto* mc = dynamic_cast<MainComponent*> (window->getContentComponent()))
            mc->openSong (args[i + 1].unquoted(), seek, args.contains ("--play"));
    }

    void shutdown() override
    {
        window.reset();
        engine.reset(); // saves settings
        juce::Logger::setCurrentLogger (nullptr);
    }

    void systemRequestedQuit() override { quit(); }

private:
    class MainWindow : public juce::DocumentWindow
    {
    public:
        explicit MainWindow (Engine& e)
            : DocumentWindow ("BetterVoiceMeeter", theme::bg, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (e), true);
            e.onThemeChanged = [this, &e]
            {
                theme::apply (theme::styleFromName (e.uiTheme));
                e.saveState();
                setBackgroundColour (theme::bg);
                const auto bounds = getBounds();
                setContentOwned (new MainComponent (e), false); // rebuild every component with the new palette
                setBounds (bounds);
            };
            setResizable (true, false);
            setResizeLimits (900, 640, 6000, 3000);
            if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            {
                auto area = display->userBounds.toNearestInt();
                centreWithSize (juce::jmin (getWidth(), area.getWidth() - 40), juce::jmin (getHeight(), area.getHeight() - 40));
            }
            setVisible (true);
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    std::unique_ptr<juce::FileLogger> logger;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (BetterVoiceMeeterApp)
