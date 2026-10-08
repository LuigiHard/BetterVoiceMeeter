#include <JuceHeader.h>
#include "Engine.h"
#include "UI/MainComponent.h"

class BetterVoiceMeeterApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise (const juce::String&) override
    {
        logger.reset (juce::FileLogger::createDefaultAppLogger ("BetterVoiceMeeter", "BetterVoiceMeeter.log",
                                                                "BetterVoiceMeeter log"));
        juce::Logger::setCurrentLogger (logger.get());
        engine = std::make_unique<Engine>();
        window = std::make_unique<MainWindow> (*engine);
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
            : DocumentWindow ("BetterVoiceMeeter", juce::Colour (0xff1b1e24), DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setContentOwned (new MainComponent (e), true);
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
