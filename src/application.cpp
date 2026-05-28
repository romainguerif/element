// SPDX-FileCopyrightText: Copyright (C) Kushview, LLC.
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include "crashdiagnostics.hpp"
#include <element/application.hpp>
#include <element/services.hpp>
#include <element/version.hpp>
#include <element/context.hpp>
#include <element/devices.hpp>
#include <element/plugins.hpp>
#include <element/settings.hpp>
#include <element/ui.hpp>
#include <element/ui/commands.hpp>
#include <element/ui/commands.hpp>
#include <element/ui/content.hpp>
#include <element/ui/updater.hpp>

#include "engine/internalformat.hpp"
#include "engine/midiengine.hpp"
#include "scripting.hpp"
#include <element/datapath.hpp>
#include "services/sessionservice.hpp"
#include "log.hpp"
#include "messages.hpp"
#include "auth.hpp"
#include "utils.hpp"

/** Define to force the application to behave as if running for the first time.
    When enabled (set to 1), the application will:
    - Skip checking for existing settings files
    - Initialize default plugin scan paths
    - Perform other first-run setup tasks
    
    This is useful for testing first-run behavior without clearing user settings.
*/
#ifndef ELEMENT_FIRST_RUN
#define ELEMENT_FIRST_RUN 0
#endif

using namespace juce;

namespace element {

static std::atomic<int> sCanShutdown { 0 };

class Startup : public ActionBroadcaster
{
public:
    Startup (Context& w)
        : world (w), isFirstRun (false) {}
    ~Startup() {}

    void launchApplication()
    {
        DataPath::initializeDefaultLocation();

        [[maybe_unused]] Settings& settings (world.settings());
#if ELEMENT_FIRST_RUN
        isFirstRun = true;
#else
        isFirstRun = ! settings.getUserSettings()->getFile().existsAsFile();
#endif
        setupLogging();
        setupKeyMappings();
        setupAudioEngine();
        setupPlugins();
        setupMidiEngine();
        setupScripting();
        setupRepos();

        sendActionMessage ("finishedLaunching");
    }

private:
    friend class Application;
    Context& world;
    bool isFirstRun;

    void setupAudioEngine()
    {
        auto& settings = world.settings();
        DeviceManager& devices (world.devices());
        for (const auto& tp : devices.getAvailableDeviceTypes())
            tp->scanForDevices();

        AudioEnginePtr engine = world.audio();
        engine->applySettings (settings);

        auto* props = settings.getUserSettings();

        String error = "No device found at startup";
        if (auto dxml = props->getXmlValue ("devices"))
        {
            error = devices.initialise (DeviceManager::maxAudioChannels,
                                        DeviceManager::maxAudioChannels,
                                        dxml.get(),
                                        true,
                                        "*",
                                        nullptr);
            if (error.isNotEmpty())
            {
                auto setup = devices.getAudioDeviceSetup();
                error = devices.setAudioDeviceSetup (setup, true);
            }
        }

        if (error.isNotEmpty())
        {
#if JUCE_WINDOWS
            devices.setCurrentAudioDeviceType ("Windows Audio (Low Latency Mode)", true);
#endif
            error = devices.initialiseWithDefaultDevices (DeviceManager::maxAudioChannels,
                                                          DeviceManager::maxAudioChannels);
        }

        if (error.isNotEmpty())
        {
            Logger::writeToLog (error);
        }
    }

    void setupMidiEngine()
    {
#if ! JUCE_LINUX
        auto& midi = world.midi();
        midi.applySettings (world.settings());
#endif
    }

    void setupKeyMappings()
    {
        auto* const props = world.settings().getUserSettings();
        auto* const keymp = world.services().find<GuiService>()->commands().getKeyMappings();
        if (props && keymp)
        {
            std::unique_ptr<XmlElement> xml;
            xml = props->getXmlValue (Settings::keymappingsKey);
            if (xml != nullptr)
                keymp->restoreFromXml (*xml);
            xml = nullptr;
        }
    }

    void setupPlugins()
    {
        auto& settings (world.settings());
        auto& plugins (world.plugins());
        plugins.restoreUserPlugins (settings);
        plugins.setPropertiesFile (settings.getUserSettings());

        if (isFirstRun)
        {
            auto props = settings.getUserSettings();
            for (const auto& formatName : Util::compiledAudioPluginFormats())
                props->setValue (Settings::lastPluginScanPathPrefix + formatName,
                                 plugins.defaultSearchPath (formatName).toString());
        }

        plugins.scanInternalPlugins();
        plugins.searchUnverifiedPlugins();
    }

    void setupScripting()
    {
        auto& scripts = world.scripting();
        ignoreUnused (scripts);
    }

    void setupRepos()
    {
        if (! isFirstRun)
            return;
#if ELEMENT_UPDATER

#endif
    }

    void setupLogging()
    {
        Logger::setCurrentLogger (&world.logger());
    }
};

//=============================================================================
#if JUCE_LINUX
Application::MidiSettingsApply::MidiSettingsApply (Context& c)
    : context (c)
{
    juce::WeakReference<Application::MidiSettingsApply> safeThis (this);
    connection = juce::MidiDeviceListConnection::make ([safeThis] {
        if (auto self = safeThis.get())
        {
            self->connection.reset();
            self->context.midi().applySettings (
                self->context.settings());
        }
    });
}
#endif

Application::Application()
{
}

Application::~Application() {}

const String Application::getApplicationName() { return "Element"; }

const String Application::getApplicationVersion() { return ELEMENT_VERSION_STRING; }

bool Application::moreThanOneInstanceAllowed() { return true; }

void Application::initialise (const String& commandLine)
{
    // Install a flushing file logger BEFORE anything else so any crash that
    // happens during startup, audio init or plugin-window construction leaves
    // breadcrumbs on disk. The file lives at:
    //   ~/Library/Logs/Element/element.log     (macOS)
    //   ~/.config/Element/Logs/element.log     (Linux)
    //   %APPDATA%/Element/Logs/element.log     (Windows)
    fileLogger.reset (juce::FileLogger::createDefaultAppLogger (
        "Element",                  // app subdir under platform log directory
        "element.log",              // file name
        "==== Element session start ====",
        128 * 1024));               // rotate at 128 KB
    juce::Logger::setCurrentLogger (fileLogger.get());
    juce::Logger::writeToLog ("[startup] Application::initialise cmdLine=" + commandLine);

    // Install POSIX signal handlers that fsync a stack trace to a dedicated
    // crash file. This survives the kind of immediate process death that
    // JUCE's FileLogger can't capture (its FileOutputStreams are buffered
    // and a SIGSEGV in mid-paint will drop the latest entries).
    diagnostics::installCrashDiagnostics (
        juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
            .getChildFile ("Logs/Element/crash.log"));
   #if JUCE_MAC
    // Cocoa / VST3-on-AppKit plugin editors throw Objective-C exceptions
    // that aren't POSIX signals, so install those handlers too.
    diagnostics::installMacCrashHandlers();
   #endif
    diagnostics::breadcrumb ("startup", ("commandLine=" + commandLine).toRawUTF8());

    world = std::make_unique<Context> (RunMode::Standalone, commandLine);
    if (maybeLaunchScannerWorker (commandLine))
        return;

    if (sendCommandLineToPreexistingInstance())
    {
        quit();
        return;
    }

    initializeModulePath();
    printCopyNotice();

#if JUCE_MAC
    registerURLSchemeHandler();
#endif

    launchApplication();

    // Handle URL scheme if passed on command line
    if (commandLine.startsWith ("element://"))
        handleURLSchemeCallback (commandLine);
}

void Application::actionListenerCallback (const String& message)
{
    if (message == "finishedLaunching")
        finishLaunching();
}

bool Application::canShutdown()
{
    if (! MessageManager::getInstance()->isThisTheMessageThread())
    {
        auto result = MessageManager::getInstance()->callSync (Application::canShutdown);
        return result.has_value() ? *result : true;
    }

    if (auto app = dynamic_cast<Application*> (getInstance()))
    {
        auto& services = app->world->services();
        auto ssvc = services.find<SessionService>();
        return ! ssvc->hasSessionChanged();
    }

    return true;
}

void Application::shutdown()
{
    if (! world)
        return;

#if JUCE_MAC
    unregisterURLSchemeHandler();
#endif

#if JUCE_LINUX
    applyMidiSettings.reset();
#endif
    workers.clearQuick (true);
    auto& srvs = world->services();
    srvs.saveSettings();

    if (authStartupThread && authStartupThread->isThreadRunning())
        authStartupThread->stopThread (3000);
    authStartupThread.reset();

    auto& devices (world->devices());
    devices.closeAudioDevice();

    auto engine (world->audio());
    auto& plugins (world->plugins());
    auto& settings (world->settings());
    auto& midi (world->midi());
    auto* props = settings.getUserSettings();
    plugins.setPropertiesFile (nullptr); // must be done before Settings is deleted

    srvs.deactivate();
    srvs.shutdown();
    sCanShutdown.store (1, std::memory_order_release);

    plugins.saveUserPlugins (settings);
    midi.writeSettings (settings);

    if (auto el = world->devices().createStateXml())
        props->setValue (Settings::devicesKey, el.get());

    engine = nullptr;
    Logger::writeToLog ("[shutdown] Application::shutdown end");
    Logger::setCurrentLogger (nullptr);
    fileLogger.reset();
    world->setEngine (nullptr);
    world = nullptr;
}

void Application::systemRequestedQuit()
{
    if (! world)
    {
        Application::quit();
        return;
    }

    auto* sc = world->services().find<SessionService>();

    if (world->settings().askToSaveSession())
    {
        // - 0 if the third button was pressed ('cancel')
        // - 1 if the first button was pressed ('yes')
        // - 2 if the middle button was pressed ('no')
        const int res = ! sc->hasSessionChanged() ? 2
                                                  : AlertWindow::showYesNoCancelBox (AlertWindow::NoIcon, "Save Session", "This session may have changes. Would you like to save before exiting?");

        if (res == 1)
            sc->saveSession();
        if (res != 0)
            Application::quit();
    }
    else
    {
        if (sc->getSessionFile().existsAsFile())
        {
            sc->saveSession (false, false, false);
        }
        else
        {
            if (AlertWindow::showOkCancelBox (AlertWindow::NoIcon, "Save Session", "This session has not been saved to disk yet.\nWould you like to before exiting?", "Yes", "No"))
            {
                sc->saveSession();
            }
        }

        Application::quit();
    }
}

// Show a small "what should I open?" menu when Element starts with no
// session loaded — replaces the previous behaviour of silently reloading
// the last session. Lists the most recent .els files, plus an entry to
// pick a file from disk and one to dismiss with an empty session.
static void showStartupSessionPicker (Context& world)
{
    auto& services = world.services();
    auto* sc       = services.find<SessionService>();
    auto* gui      = services.find<GuiService>();
    if (sc == nullptr || gui == nullptr)
        return;

    // If a CLI file already loaded a session, don't get in the way.
    if (sc->getSessionFile().existsAsFile())
        return;

    juce::PopupMenu menu;
    menu.addSectionHeader ("Open a Session");

    auto& recents = gui->recentFiles();
    const int numRecents = juce::jmin (10, recents.getNumFiles());
    juce::Array<juce::File> recentFiles;
    for (int i = 0; i < numRecents; ++i)
    {
        auto f = recents.getFile (i);
        if (f.existsAsFile())
        {
            recentFiles.add (f);
            menu.addItem (i + 1, f.getFileNameWithoutExtension(),
                          true, false, juce::Image());
        }
    }
    if (recentFiles.isEmpty())
        menu.addItem (-1, "(no recent sessions)", false);

    menu.addSeparator();
    menu.addItem (101, "Open File…");
    menu.addItem (102, "New Empty Session");

    auto options = juce::PopupMenu::Options()
                       .withStandardItemHeight (24)
                       .withMinimumWidth (260);

    menu.showMenuAsync (options,
        [&world, recentFiles] (int result) {
            auto& services = world.services();
            auto* sc = services.find<SessionService>();
            if (sc == nullptr)
                return;

            if (result >= 1 && result <= recentFiles.size())
            {
                sc->openFile (recentFiles[result - 1]);
            }
            else if (result == 101)
            {
                auto chooser = std::make_shared<juce::FileChooser> (
                    "Open Session", juce::File(), "*.els");
                chooser->launchAsync (
                    juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles,
                    [chooser, sc] (const juce::FileChooser& fc) {
                        auto f = fc.getResult();
                        if (f.existsAsFile())
                            sc->openFile (f);
                    });
            }
            // result == 102 or no selection: keep the empty default session.
        });
}

void Application::maybeOpenCommandLineFile (const String& commandLine)
{
    if (auto* sc = world->services().find<SessionService>())
    {
        const auto path = commandLine.unquoted().trim();
        const File sessionFile = File::isAbsolutePath (path)
                                     ? File (path)
                                     : File::getCurrentWorkingDirectory().getChildFile (path);
        if (sessionFile.existsAsFile())
        {
            const File file (path);
            if (file.hasFileExtension ("els"))
                sc->openFile (file);
            else if (file.hasFileExtension ("elg"))
                sc->importGraph (file);
        }
    }
}

void Application::anotherInstanceStarted (const String& commandLine)
{
    if (! world)
        return;

    // Handle custom URL scheme callbacks (e.g., auth)
    if (commandLine.startsWith ("element://"))
    {
        handleURLSchemeCallback (commandLine);
        return;
    }

    maybeOpenCommandLineFile (commandLine);
}

void Application::handleURLSchemeCallback (const String& urlString)
{
    const URL url (urlString);
    if (url.getDomain() == "auth" && url.getSubPath() == "callback")
    {
        if (auto* gui = world->services().find<GuiService>())
            if (auto* mainWindow = gui->getMainWindow())
                mainWindow->toFront (true);

        auth::handleCallback (urlString, world->settings());
    }
}

void Application::suspended() {}

void Application::resumed()
{
    auto& devices (world->devices());
    devices.restartLastAudioDevice();
}

void Application::finishLaunching()
{
    if (nullptr == startup)
        return;

    if (world->settings().scanForPluginsOnStartup())
        world->plugins().scanAudioPlugins();

    const bool isFirstRun = startup->isFirstRun;
    startup.reset();

    auto& ui = *world->services().find<UI>();
    if (auto cf = createContentFactory())
        ui.setContentFactory (std::move (cf));

    world->services().run();

    // Attempt to restore auth state from a stored refresh token in the background.
    authStartupThread = std::make_unique<AuthStartupThread> (*world);
    authStartupThread->startThread (Thread::Priority::low);

    if (! isFirstRun && world->settings().checkForUpdates())
        startTimer (10 * 1000);

    maybeOpenCommandLineFile (getCommandLineParameters());

    // Post the startup picker to the next message tick so the main
    // window is on screen first — feels more natural than the menu
    // popping up before any UI is visible.
    juce::MessageManager::callAsync ([this] {
        if (world != nullptr)
            showStartupSessionPicker (*world);
    });
}

void Application::AuthStartupThread::run()
{
    auth::maybeRefreshOnStartup (ctx.settings());

    // Re-apply the channel preference to the updater from the message thread:
    // the startup refresh may have fetched a new signed appcast URL.
    juce::MessageManager::callAsync ([&ctx = ctx]() {
        if (auto* gui = ctx.services().find<GuiService>())
            gui->applyStoredChannelToUpdater();
    });
}

void Application::printCopyNotice()
{
    String appName = Util::appName();
    appName << " v" << getApplicationVersion() << " (GPL v3)";
    Logger::writeToLog (appName);
    Logger::writeToLog (String ("Copyright (c) 2017-%YEAR% Kushview, LLC.  All rights reserved.\n")
                            .replace ("%YEAR%", String (Time::getCurrentTime().getYear())));
}

bool Application::maybeLaunchScannerWorker (const String& commandLine)
{
    workers.clearQuick (true);
    workers.add (world->plugins().createAudioPluginScannerWorker());
    StringArray processIds = { EL_PLUGIN_SCANNER_PROCESS_ID };
    for (auto* worker : workers)
    {
        for (const auto& pid : processIds)
        {
            if (worker->initialiseFromCommandLine (commandLine, pid, 20 * 1000))
            {
#if JUCE_MAC
                Process::setDockIconVisible (false);
#endif
                return true;
            }
        }
    }

    return false;
}

void Application::launchApplication()
{
    if (startup != nullptr)
        return;

    startup = std::make_unique<Startup> (*world);
    startup->addActionListener (this);
    startup->launchApplication();
#if JUCE_LINUX
    applyMidiSettings = std::make_unique<MidiSettingsApply> (*world);
#endif
}

void Application::initializeModulePath()
{
    const File path (File::getSpecialLocation (File::invokedExecutableFile));
    File modDir = path.getParentDirectory().getParentDirectory().getChildFile ("lib/element").getFullPathName();
#if JUCE_DEBUG
    if (! modDir.exists())
    {
        modDir = path.getParentDirectory().getParentDirectory().getChildFile ("modules");
    }
#endif

#if JUCE_WINDOWS
    String putEnv = "ELEMENT_MODULE_PATH=";
    putEnv << modDir.getFullPathName();
    putenv (putEnv.toRawUTF8());
#else
    setenv ("ELEMENT_MODULE_PATH", modDir.getFullPathName().toRawUTF8(), 1);
#endif
}

void Application::timerCallback()
{
    world->services().find<UI>()->checkUpdates (true);
    stopTimer();
}

} // namespace element
