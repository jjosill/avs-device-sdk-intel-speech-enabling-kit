/*
 * Copyright 2017-2018 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *     http://aws.amazon.com/apache2.0/
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "SampleApp/ConnectionObserver.h"
#include "SampleApp/GuiRenderer.h"
#include "SampleApp/KeywordObserver.h"
#include "SampleApp/SampleApplication.h"
#include "SampleApp/PortAudioObserver.h"

#ifdef KWD_KITTAI
#include <KittAi/KittAiKeyWordDetector.h>
#elif KWD_SENSORY
#include <Sensory/SensoryKeywordDetector.h>


#ifdef KWD
#include <KWDProvider/KeywordDetectorProvider.h>
#endif


#elif KWD_HARDWARE
#if defined(ALSA_HW_CTRL)
#include <AlsaController/AlsaHardwareController.h>
#endif
#include <HardwareController/AbstractHardwareController.h>
#include <Hardware/HardwareKeywordDetector.h>

#endif

#ifdef ENABLE_ESP
#include <ESP/ESPDataProvider.h>
#else
#include <ESP/DummyESPDataProvider.h>
#endif

#include <AVSCommon/AVS/Initialization/AlexaClientSDKInit.h>
#include <AVSCommon/Utils/Configuration/ConfigurationNode.h>
#include <AVSCommon/Utils/DeviceInfo.h>
#include <AVSCommon/Utils/LibcurlUtils/HTTPContentFetcherFactory.h>
#include <AVSCommon/Utils/LibcurlUtils/HttpPut.h>
#include <AVSCommon/Utils/Logger/Logger.h>
#include <AVSCommon/Utils/Logger/LoggerSinkManager.h>
#include <AVSCommon/Utils/Network/InternetConnectionMonitor.h>
#include <Alerts/Storage/SQLiteAlertStorage.h>
#include <Audio/AudioFactory.h>
//#include <Bluetooth/SQLiteBluetoothStorage.h>
#include <CBLAuthDelegate/CBLAuthDelegate.h>
#include <CBLAuthDelegate/SQLiteCBLAuthDelegateStorage.h>
#include <CapabilitiesDelegate/CapabilitiesDelegate.h>
#include <MediaPlayer/MediaPlayer.h>
#include <Notifications/SQLiteNotificationsStorage.h>
#include <Settings/SQLiteSettingStorage.h>
#include <SQLiteStorage/SQLiteMiscStorage.h>
//#include <DCFDelegate/DCFDelegate.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <csignal>

namespace alexaClientSDK {
namespace sampleApp {

/// The sample rate of microphone audio data.
static const unsigned int SAMPLE_RATE_HZ = 16000;

/// The number of audio channels.
static const unsigned int NUM_CHANNELS = 1;

/// The size of each word within the stream.
static const size_t WORD_SIZE = 2;

/// The maximum number of readers of the stream.
static const size_t MAX_READERS = 10;

/// The amount of audio data to keep in the ring buffer.
static const std::chrono::seconds AMOUNT_OF_AUDIO_DATA_IN_BUFFER = std::chrono::seconds(15);

/// The size of the ring buffer.
static const size_t BUFFER_SIZE_IN_SAMPLES = (SAMPLE_RATE_HZ)*AMOUNT_OF_AUDIO_DATA_IN_BUFFER.count();

/// Key for the root node value containing configuration values for SampleApp.
static const std::string SAMPLE_APP_CONFIG_KEY("sampleApp");

/// Key for the @c firmwareVersion value under the @c SAMPLE_APP_CONFIG_KEY configuration node.
static const std::string FIRMWARE_VERSION_KEY("firmwareVersion");

/// Key for the @c endpoint value under the @c SAMPLE_APP_CONFIG_KEY configuration node.
static const std::string ENDPOINT_KEY("endpoint");

/// Key for setting if display cards are supported or not under the @c SAMPLE_APP_CONFIG_KEY configuration node.
static const std::string DISPLAY_CARD_KEY("displayCardsSupported");

/// Timeout for reset timer - timeout is currently 3 mintues
static const std::chrono::milliseconds RESET_TIMEOUT = std::chrono::milliseconds(3 * 60 * 1000);

using namespace capabilityAgents::externalMediaPlayer;

/// The @c m_playerToMediaPlayerMap Map of the adapter to their speaker-type and MediaPlayer creation methods.
std::unordered_map<std::string, SampleApplication::SpeakerTypeAndCreateFunc>
    SampleApplication::m_playerToMediaPlayerMap;

/// The singleton map from @c playerId to @c ExternalMediaAdapter creation functions.
std::unordered_map<std::string, ExternalMediaPlayer::AdapterCreateFunction> SampleApplication::m_adapterToCreateFuncMap;


#ifdef KWD_KITTAI
/// The sensitivity of the Kitt.ai engine.
static const double KITT_AI_SENSITIVITY = 0.6;

/// The audio amplifier level of the Kitt.ai engine.
static const float KITT_AI_AUDIO_GAIN = 2.0;

/// Whether Kitt.ai should apply front end audio processing.
static const bool KITT_AI_APPLY_FRONT_END_PROCESSING = true;
#endif

/// String to identify log entries originating from this file.
static const std::string TAG("SampleApplication");


/**
 * Create a LogEntry using this file's TAG and the specified event string.
 *
 * @param The event string for this @c LogEntry.
 */
#define LX(event) alexaClientSDK::avsCommon::utils::logger::LogEntry(TAG, event)

/// A set of all log levels.
static const std::set<alexaClientSDK::avsCommon::utils::logger::Level> allLevels = {
    alexaClientSDK::avsCommon::utils::logger::Level::DEBUG9,
    alexaClientSDK::avsCommon::utils::logger::Level::DEBUG8,
    alexaClientSDK::avsCommon::utils::logger::Level::DEBUG7,
    alexaClientSDK::avsCommon::utils::logger::Level::DEBUG6,
    alexaClientSDK::avsCommon::utils::logger::Level::DEBUG5,
    alexaClientSDK::avsCommon::utils::logger::Level::DEBUG4,
    alexaClientSDK::avsCommon::utils::logger::Level::DEBUG3,
    alexaClientSDK::avsCommon::utils::logger::Level::DEBUG2,
    alexaClientSDK::avsCommon::utils::logger::Level::DEBUG1,
    alexaClientSDK::avsCommon::utils::logger::Level::DEBUG0,
    alexaClientSDK::avsCommon::utils::logger::Level::INFO,
    alexaClientSDK::avsCommon::utils::logger::Level::WARN,
    alexaClientSDK::avsCommon::utils::logger::Level::ERROR,
    alexaClientSDK::avsCommon::utils::logger::Level::CRITICAL,
    alexaClientSDK::avsCommon::utils::logger::Level::NONE};

/**
 * Gets a log level consumable by the SDK based on the user input string for log level.
 *
 * @param userInputLogLevel The string to be parsed into a log level.
 * @return The log level. This will default to NONE if the input string is not properly parsable.
 */
static alexaClientSDK::avsCommon::utils::logger::Level getLogLevelFromUserInput(std::string userInputLogLevel) {
    std::transform(userInputLogLevel.begin(), userInputLogLevel.end(), userInputLogLevel.begin(), ::toupper);
    return alexaClientSDK::avsCommon::utils::logger::convertNameToLevel(userInputLogLevel);
}

/**
 * The interface used to display messages in the console.
 *
 * TODO: g_consolePrinter is a static/global because it is passed by reference to changeSinkLogger() below,
 * which keeps a reference to it for the lifetime of the logging system.  If the logging system is refactoroed to
 * use shared_ptrs (ACSDK-445), the ConsolePrinter can be instantiated as shared_ptr class member and passed to
 * LoggerSinkManager.
 *private:

 */
static alexaClientSDK::sampleApp::ConsolePrinter g_consolePrinter;
/**
 * Allows the process to ignore the SIGPIPE signal.
 * The SIGPIPE signal may be received when the application performs a write to a closed socket.
 * This is a case that arises in the use of certain networking libraries.
 *
 * @return true if the action for handling SIGPIPEs was correctly set to ignore, else false.
 */
static bool ignoreSigpipeSignals() {
#ifndef NO_SIGPIPE
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        return false;
    }
#endif
    return true;
}

//$ const std::string& pathToConfig,
std::unique_ptr<SampleApplication> SampleApplication::create(
    const std::vector<std::string>& configFiles,
    const std::string& pathToInputFolder,
    const std::string& logLevel,
    const std::string& hwName) {
    auto clientApplication = std::unique_ptr<SampleApplication>(new SampleApplication);
    if (!clientApplication->initialize(configFiles, pathToInputFolder, logLevel, hwName)) {
        ConsolePrinter::simplePrint("Failed to initialize SampleApplication");
        return nullptr;
    } else {
        ConsolePrinter::simplePrint("SampleApplication initialized successfully");
    }
    if (!ignoreSigpipeSignals()) {
        ConsolePrinter::simplePrint("Failed to set a signal handler for SIGPIPE");
        return nullptr;
    }

    return clientApplication;
}


SampleApplication::AdapterRegistration::AdapterRegistration(
    const std::string& playerId,
    ExternalMediaPlayer::AdapterCreateFunction createFunction) {
    if (m_adapterToCreateFuncMap.find(playerId) != m_adapterToCreateFuncMap.end()) {
        std::string errorStr = "WARNING:Adapter already exists for playerId " + playerId;
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint(errorStr);
    }

    m_adapterToCreateFuncMap[playerId] = createFunction;
}

SampleApplication::MediaPlayerRegistration::MediaPlayerRegistration(
    const std::string& playerId,
    avsCommon::sdkInterfaces::SpeakerInterface::Type speakerType,
    MediaPlayerCreateFunction createFunction) {
    if (m_playerToMediaPlayerMap.find(playerId) != m_playerToMediaPlayerMap.end()) {
        std::string errorStr = "WARNING:MediaPlayer already exists for playerId " + playerId;
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint(errorStr);
    }

    m_playerToMediaPlayerMap[playerId] =
        std::pair<avsCommon::sdkInterfaces::SpeakerInterface::Type, MediaPlayerCreateFunction>(
            speakerType, createFunction);
}

void SampleApplication::run() {
    m_userInputManager->run();
}

SampleApplication::~SampleApplication() {
    if (m_capabilitiesDelegate) {
        m_capabilitiesDelegate->shutdown();
    }
	/*if (m_dcfDelegate) {
        m_dcfDelegate->shutdown();
    } */

    // First clean up anything that depends on the the MediaPlayers.
    m_userInputManager.reset();
    m_externalMusicProviderMediaPlayersMap.clear();

    if (m_interactionManager) {
        m_interactionManager->shutdown();
    }

    // Now it's safe to shut down the MediaPlayers.
    for (auto& mediaPlayer : m_adapterMediaPlayers) {
        mediaPlayer->shutdown();
    }
    if (m_speakMediaPlayer) {
        m_speakMediaPlayer->shutdown();
    }
    if (m_audioMediaPlayer) {
        m_audioMediaPlayer->shutdown();
    }
    if (m_alertsMediaPlayer) {
        m_alertsMediaPlayer->shutdown();
    }
    if (m_notificationsMediaPlayer) {
        m_notificationsMediaPlayer->shutdown();
    }
	
	/*
    if (m_bluetoothMediaPlayer) {
        m_bluetoothMediaPlayer->shutdown();
    }
	*/
	
    if (m_ringtoneMediaPlayer) {
        m_ringtoneMediaPlayer->shutdown();
    }
}

bool SampleApplication::createMediaPlayersForAdapters(
    std::shared_ptr<avsCommon::utils::libcurlUtils::HTTPContentFetcherFactory> httpContentFetcherFactory,
    std::vector<std::shared_ptr<avsCommon::sdkInterfaces::SpeakerInterface>>& additionalSpeakers) {
    for (auto& entry : m_playerToMediaPlayerMap) {
        auto mediaPlayer =
            entry.second.second(httpContentFetcherFactory, entry.second.first, entry.first + "MediaPlayer");
        if (mediaPlayer) {
            m_externalMusicProviderMediaPlayersMap[entry.first] = mediaPlayer;
            additionalSpeakers.push_back(
                std::static_pointer_cast<alexaClientSDK::avsCommon::sdkInterfaces::SpeakerInterface>(mediaPlayer));
            m_adapterMediaPlayers.push_back(mediaPlayer);
        } else {
            ACSDK_CRITICAL(LX("Failed to create mediaPlayer").d("playerId", entry.first));
            std::string errorStr = "ERROR:Failed to create mediaPlayer for playerId " + entry.first;
            alexaClientSDK::sampleApp::ConsolePrinter::simplePrint(errorStr);
            return false;
        }
    }

    return true;
}


bool SampleApplication::initialize(
    const std::vector<std::string>& configFiles,
    const std::string& pathToInputFolder,
    const std::string& logLevel,
    const std::string& hwName) {
    /*
     * Set up the SDK logging system to write to the SampleApp's ConsolePrinter.  Also adjust the logging level
     * if requested.
     */
    std::shared_ptr<alexaClientSDK::avsCommon::utils::logger::Logger> consolePrinter =
        std::make_shared<alexaClientSDK::sampleApp::ConsolePrinter>();

    if (!logLevel.empty()) {
        auto logLevelValue = getLogLevelFromUserInput(logLevel);
        if (alexaClientSDK::avsCommon::utils::logger::Level::UNKNOWN == logLevelValue) {
            alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Unknown log level input!");
            alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Possible log level options are: ");
            for (auto it = allLevels.begin(); it != allLevels.end(); ++it) {
                alexaClientSDK::sampleApp::ConsolePrinter::simplePrint(
                    alexaClientSDK::avsCommon::utils::logger::convertLevelToName(*it));
            }
            return false;
        }

        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint(
            "Running app with log level: " +
            alexaClientSDK::avsCommon::utils::logger::convertLevelToName(logLevelValue));
        consolePrinter->setLevel(logLevelValue);
    }
    alexaClientSDK::avsCommon::utils::logger::LoggerSinkManager::instance().initialize(consolePrinter);

    std::vector<std::shared_ptr<std::istream>> configJsonStreams;

    for (auto configFile : configFiles) {
        if (configFile.empty()) {
            alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Config filename is empty!");
            return false;
        }

        auto configInFile = std::shared_ptr<std::ifstream>(new std::ifstream(configFile));
        if (!configInFile->good()) {
            ACSDK_CRITICAL(LX("Failed to read config file").d("filename", configFile));
            alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to read config file " + configFile);
            return false;
        }

        configJsonStreams.push_back(configInFile);
    }

    if (!avsCommon::avs::initialization::AlexaClientSDKInit::initialize(configJsonStreams)) {
        ACSDK_CRITICAL(LX("Failed to initialize SDK!"));
        return false;
    }
	
	//$
	/*
     * This is a required step upon startup of the SDK before any modules are created. For that reason, it is being
     * called here, before creating the MediaPlayer, audio streams, DefaultClient, etc.
     
    std::ifstream configInfile(pathToConfig);
    if (!configInfile.good()) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to read config file!");
        return false;
    }
    if (!avsCommon::avs::initialization::AlexaClientSDKInit::initialize({&configInfile})) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to initialize SDK!");
        return false;
    }
	*/
    auto config = alexaClientSDK::avsCommon::utils::configuration::ConfigurationNode::getRoot();
    auto sampleAppConfig = config[SAMPLE_APP_CONFIG_KEY];

    auto httpContentFetcherFactory = std::make_shared<avsCommon::utils::libcurlUtils::HTTPContentFetcherFactory>();

    m_speakMediaPlayer = alexaClientSDK::mediaPlayer::MediaPlayer::create(
        httpContentFetcherFactory, avsCommon::sdkInterfaces::SpeakerInterface::Type::AVS_SYNCED, "SpeakMediaPlayer");
    if (!m_speakMediaPlayer) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to create media player for speech!");
        return false;
    }

    m_audioMediaPlayer = alexaClientSDK::mediaPlayer::MediaPlayer::create(
        httpContentFetcherFactory, avsCommon::sdkInterfaces::SpeakerInterface::Type::AVS_SYNCED, "AudioMediaPlayer");
    if (!m_audioMediaPlayer) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to create media player for content!");
        return false;
    }

    m_notificationsMediaPlayer = alexaClientSDK::mediaPlayer::MediaPlayer::create(
        httpContentFetcherFactory,
        avsCommon::sdkInterfaces::SpeakerInterface::Type::AVS_SYNCED,
        "NotificationsMediaPlayer");
    if (!m_notificationsMediaPlayer) {
        ACSDK_CRITICAL(LX("Failed to create media player for notifications!"));
        return false;
    }
	
	/*
    m_bluetoothMediaPlayer = alexaClientSDK::mediaPlayer::MediaPlayer::create(
        httpContentFetcherFactory,
        avsCommon::sdkInterfaces::SpeakerInterface::Type::AVS_SYNCED,
        "BluetoothMediaPlayer");

    if (!m_bluetoothMediaPlayer) {
        ACSDK_CRITICAL(LX("Failed to create media player for bluetooth!"));
    }
	*/

    m_ringtoneMediaPlayer = alexaClientSDK::mediaPlayer::MediaPlayer::create(
        httpContentFetcherFactory, avsCommon::sdkInterfaces::SpeakerInterface::Type::AVS_SYNCED, "RingtoneMediaPlayer");
    if (!m_ringtoneMediaPlayer) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to create media player for ringtones!");
        return false;
    }

    /*
     * The ALERTS speaker type will cause volume control to be independent and localized. By assigning this type,
     * Alerts volume/mute changes will not be in sync with AVS. No directives or events will be associated with volume
     * control.
     */
    m_alertsMediaPlayer = alexaClientSDK::mediaPlayer::MediaPlayer::create(
        httpContentFetcherFactory, avsCommon::sdkInterfaces::SpeakerInterface::Type::LOCAL, "AlertsMediaPlayer");
    if (!m_alertsMediaPlayer) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to create media player for alerts!");
        return false;
    }

    /*
     * Create Speaker interfaces to control the volume. For the SDK, the MediaPlayer happens to also provide
     * volume control functionality, but this does not have to be case.
     * Note the externalMusicProviderMediaPlayer is not added to the set of SpeakerInterfaces as there would be
     * more actions needed for these beyond setting the volume control on the MediaPlayer.
     */
    std::shared_ptr<alexaClientSDK::avsCommon::sdkInterfaces::SpeakerInterface> speakSpeaker =
        std::static_pointer_cast<alexaClientSDK::avsCommon::sdkInterfaces::SpeakerInterface>(m_speakMediaPlayer);
    std::shared_ptr<alexaClientSDK::avsCommon::sdkInterfaces::SpeakerInterface> audioSpeaker =
        std::static_pointer_cast<alexaClientSDK::avsCommon::sdkInterfaces::SpeakerInterface>(m_audioMediaPlayer);
    std::shared_ptr<alexaClientSDK::avsCommon::sdkInterfaces::SpeakerInterface> alertsSpeaker =
        std::static_pointer_cast<alexaClientSDK::avsCommon::sdkInterfaces::SpeakerInterface>(m_alertsMediaPlayer);
    std::shared_ptr<alexaClientSDK::avsCommon::sdkInterfaces::SpeakerInterface> notificationsSpeaker =
        std::static_pointer_cast<alexaClientSDK::avsCommon::sdkInterfaces::SpeakerInterface>(
            m_notificationsMediaPlayer);
	/*
    std::shared_ptr<alexaClientSDK::avsCommon::sdkInterfaces::SpeakerInterface> bluetoothSpeaker =
        std::static_pointer_cast<alexaClientSDK::avsCommon::sdkInterfaces::SpeakerInterface>(m_bluetoothMediaPlayer);
	*/
		
    std::shared_ptr<alexaClientSDK::avsCommon::sdkInterfaces::SpeakerInterface> ringtoneSpeaker =
        std::static_pointer_cast<alexaClientSDK::avsCommon::sdkInterfaces::SpeakerInterface>(m_ringtoneMediaPlayer);

    std::vector<std::shared_ptr<avsCommon::sdkInterfaces::SpeakerInterface>> additionalSpeakers;

    if (!createMediaPlayersForAdapters(httpContentFetcherFactory, additionalSpeakers)) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("ERROR: Could not create mediaPlayers for adapters");
        return false;
    }

    auto audioFactory = std::make_shared<alexaClientSDK::applicationUtilities::resources::audio::AudioFactory>();

    // Creating the alert storage object to be used for rendering and storing alerts.
    auto alertStorage =
        alexaClientSDK::capabilityAgents::alerts::storage::SQLiteAlertStorage::create(config, audioFactory->alerts());

    // Creating the message storage object to be used for storing message to be sent later.
    auto messageStorage = alexaClientSDK::certifiedSender::SQLiteMessageStorage::create(config);

    /*
     * Creating notifications storage object to be used for storing notification indicators.
     */
    auto notificationsStorage =
        alexaClientSDK::capabilityAgents::notifications::SQLiteNotificationsStorage::create(config);

    /*
     * Creating settings storage object to be used for storing <key, value> pairs of AVS Settings.
     */
    auto settingsStorage = alexaClientSDK::capabilityAgents::settings::SQLiteSettingStorage::create(config);

    // Creating the misc DB object to be used by various components.
    std::shared_ptr<alexaClientSDK::storage::sqliteStorage::SQLiteMiscStorage> miscStorage =
        alexaClientSDK::storage::sqliteStorage::SQLiteMiscStorage::create(config);

    // Create HTTP Put handler
    std::shared_ptr<avsCommon::utils::libcurlUtils::HttpPut> httpPut =
        avsCommon::utils::libcurlUtils::HttpPut::create();

    
    /*  Creating bluetooth storage object to be used for storing uuid to mac mappings for devices.
     
    auto bluetoothStorage = alexaClientSDK::capabilityAgents::bluetooth::SQLiteBluetoothStorage::create(config);
	*/


    /*
     * Creating the UI component that observes various components and prints to the console accordingly.
     */
    auto userInterfaceManager = std::make_shared<alexaClientSDK::sampleApp::UIManager>();

    /*
     * Creating customerDataManager which will be used by the registrationManager and all classes that extend
     * CustomerDataHandler
     */
    auto customerDataManager = std::make_shared<registrationManager::CustomerDataManager>();

    /*
     * Creating the deviceInfo object
     */
    std::shared_ptr<avsCommon::utils::DeviceInfo> deviceInfo = avsCommon::utils::DeviceInfo::create(config);
    if (!deviceInfo) {
        ACSDK_CRITICAL(LX("Creation of DeviceInfo failed!"));
        return false;
    }

    //$
    /*
     * Setting up a connection observer to wait for connection and authorization prior to accepting user input at
     * startup.
     */
    auto connectionObserver = std::make_shared<alexaClientSDK::sampleApp::ConnectionObserver>();

    /*
     * Creating the deviceInfo object
     * Creating the AuthDelegate - this component takes care of LWA and authorization of the client. At the moment,
     * this must be done and authorization must be achieved prior to making the call to connect().
     */
    auto authDelegateStorage = authorization::cblAuthDelegate::SQLiteCBLAuthDelegateStorage::create(config);
    std::shared_ptr<avsCommon::sdkInterfaces::AuthDelegateInterface> authDelegate =
        authorization::cblAuthDelegate::CBLAuthDelegate::create(
            config, customerDataManager, std::move(authDelegateStorage), userInterfaceManager, nullptr, deviceInfo);

    if (!authDelegate) {
        ACSDK_CRITICAL(LX("Creation of AuthDelegate failed!"));
        return false;
    }

    /*
     * Creating the CapabilitiesDelegate - This component provides the client with the ability to send messages to the
     * Capabilities API.
     */
    m_capabilitiesDelegate = alexaClientSDK::capabilitiesDelegate::CapabilitiesDelegate::create(
        authDelegate, miscStorage, httpPut, config, deviceInfo);

    if (!m_capabilitiesDelegate) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Creation of CapabilitiesDelegate failed!");
        return false;
    }

    authDelegate->addAuthObserver(userInterfaceManager);
    m_capabilitiesDelegate->addCapabilitiesObserver(userInterfaceManager);
	
	//$ old auth mec
	/*
	    std::shared_ptr<alexaClientSDK::authDelegate::AuthDelegate> authDelegate =
        alexaClientSDK::authDelegate::AuthDelegate::create();

    if (!authDelegate) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Creation of AuthDelegate failed!");
        return false;
    }

    authDelegate->addAuthObserver(connectionObserver);
	*/

    // INVALID_FIRMWARE_VERSION is passed to @c getInt() as a default in case FIRMWARE_VERSION_KEY is not found.
    int firmwareVersion = static_cast<int>(avsCommon::sdkInterfaces::softwareInfo::INVALID_FIRMWARE_VERSION);
    sampleAppConfig.getInt(FIRMWARE_VERSION_KEY, &firmwareVersion, firmwareVersion);

    /*
     * Check to see if displayCards is supported on the device. The default is supported unless specified otherwise in
     * the configuration.
     */
    bool displayCardsSupported;
    config[SAMPLE_APP_CONFIG_KEY].getBool(DISPLAY_CARD_KEY, &displayCardsSupported, true);
	
	 /*
     * Creating the InternetConnectionMonitor that will notify observers of internet connection status changes.
     */
	 
    auto internetConnectionMonitor =
        avsCommon::utils::network::InternetConnectionMonitor::create(httpContentFetcherFactory);
    if (!internetConnectionMonitor) {
        ACSDK_CRITICAL(LX("Failed to create InternetConnectionMonitor"));
        return false;
    }
		
	

    /*
     * Creating the DefaultClient - this component serves as an out-of-box default object that instantiates and "glues"
     * together all the modules.
     */
    std::shared_ptr<alexaClientSDK::defaultClient::DefaultClient> client =
        alexaClientSDK::defaultClient::DefaultClient::create(
            customerDataManager,
            m_externalMusicProviderMediaPlayersMap,
            m_adapterToCreateFuncMap,
            m_speakMediaPlayer,
            m_audioMediaPlayer,
            m_alertsMediaPlayer,
            m_notificationsMediaPlayer,

            m_ringtoneMediaPlayer,
            speakSpeaker,
            audioSpeaker,
            alertsSpeaker,
            notificationsSpeaker,
            ringtoneSpeaker,
            additionalSpeakers,
            audioFactory,
            authDelegate,
            std::move(alertStorage),
            std::move(messageStorage),
            std::move(notificationsStorage),
            std::move(settingsStorage),
            {userInterfaceManager},
            {userInterfaceManager},
            std::move(internetConnectionMonitor),
            displayCardsSupported,
            m_capabilitiesDelegate,
            firmwareVersion,
            true,
            nullptr);

    if (!client) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to create default SDK client!");
        return false;
    }


    /*
     * TODO: ACSDK-384 Remove the requirement of clients having to wait for authorization before making the connect()
     * call.
     */
    if (!connectionObserver->waitFor(
            alexaClientSDK::avsCommon::sdkInterfaces::AuthObserverInterface::State::REFRESHED)) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to authorize SDK client!");
        return false;
    }

	/* $ comment this part 
    std::string endpoint;
    sampleAppConfig.getString(ENDPOINT_KEY, &endpoint);

    client->connect(endpoint);

    if (!connectionObserver->waitFor(avsCommon::sdkInterfaces::ConnectionStatusObserverInterface::Status::CONNECTED)) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to connect to AVS!");
        return false;
    }
	*/ 
	
    // Add userInterfaceManager as observer of locale setting.
    client->addSettingObserver("locale", userInterfaceManager);
    
	// Send default settings set by the user to AVS.
    //$client->sendDefaultSettings();

    client->addSpeakerManagerObserver(userInterfaceManager);

    client->addNotificationsObserver(userInterfaceManager);

    //$ client->addAlertsObserver(userInterfaceManager);

    /*
     * Add GUI Renderer as an observer if display cards are supported.
     */
    if (displayCardsSupported) {
        auto guiRenderer = std::make_shared<GuiRenderer>();
        client->addTemplateRuntimeObserver(guiRenderer);
    }

    /*
     * Creating the buffer (Shared Data Stream) that will hold user audio data. This is the main input into the SDK.
     */
    size_t bufferSize = alexaClientSDK::avsCommon::avs::AudioInputStream::calculateBufferSize(
        BUFFER_SIZE_IN_SAMPLES, WORD_SIZE, MAX_READERS);
    auto buffer = std::make_shared<alexaClientSDK::avsCommon::avs::AudioInputStream::Buffer>(bufferSize);
    std::shared_ptr<alexaClientSDK::avsCommon::avs::AudioInputStream> sharedDataStream =
        alexaClientSDK::avsCommon::avs::AudioInputStream::create(buffer, WORD_SIZE, MAX_READERS);

    if (!sharedDataStream) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to create shared data stream!");
        return false;
    }

    alexaClientSDK::avsCommon::utils::AudioFormat compatibleAudioFormat;
    compatibleAudioFormat.sampleRateHz = SAMPLE_RATE_HZ;
    compatibleAudioFormat.sampleSizeInBits = WORD_SIZE * CHAR_BIT;
    compatibleAudioFormat.numChannels = NUM_CHANNELS;
    compatibleAudioFormat.endianness = alexaClientSDK::avsCommon::utils::AudioFormat::Endianness::LITTLE;
    compatibleAudioFormat.encoding = alexaClientSDK::avsCommon::utils::AudioFormat::Encoding::LPCM;

    /*
     * Creating each of the audio providers. An audio provider is a simple package of data consisting of the stream
     * of audio data, as well as metadata about the stream. For each of the three audio providers created here, the same
     * stream is used since this sample application will only have one microphone.
     */

    // Creating tap to talk audio provider
    bool tapAlwaysReadable = true;
    bool tapCanOverride = true;
    bool tapCanBeOverridden = true;

    alexaClientSDK::capabilityAgents::aip::AudioProvider tapToTalkAudioProvider(
        sharedDataStream,
        compatibleAudioFormat,
        alexaClientSDK::capabilityAgents::aip::ASRProfile::NEAR_FIELD,
        tapAlwaysReadable,
        tapCanOverride,
        tapCanBeOverridden);

    // Creating hold to talk audio provider
    bool holdAlwaysReadable = false;
    bool holdCanOverride = true;
    bool holdCanBeOverridden = false;

    alexaClientSDK::capabilityAgents::aip::AudioProvider holdToTalkAudioProvider(
        sharedDataStream,
        compatibleAudioFormat,
        alexaClientSDK::capabilityAgents::aip::ASRProfile::CLOSE_TALK,
        holdAlwaysReadable,
        holdCanOverride,
        holdCanBeOverridden);

    std::shared_ptr<alexaClientSDK::sampleApp::PortAudioMicrophoneWrapper> micWrapper =
        alexaClientSDK::sampleApp::PortAudioMicrophoneWrapper::create(buffer, sharedDataStream);
    if (!micWrapper) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to create PortAudioMicrophoneWrapper!");
        return false;
    }
// Creating wake word audio provider, if necessary
#ifdef KWD
    bool wakeAlwaysReadable = true;
#ifdef KWD_HARDWARE
    bool wakeCanOverride = true;
#else
    bool wakeCanOverride = false;
#endif
    bool wakeCanBeOverridden = true;

    alexaClientSDK::capabilityAgents::aip::AudioProvider wakeWordAudioProvider(
        sharedDataStream,
        compatibleAudioFormat,
        alexaClientSDK::capabilityAgents::aip::ASRProfile::NEAR_FIELD,
        wakeAlwaysReadable,
        wakeCanOverride,
        wakeCanBeOverridden);

#ifdef ENABLE_ESP
    // Creating ESP connector
    std::shared_ptr<esp::ESPDataProviderInterface> espProvider = esp::ESPDataProvider::create(wakeWordAudioProvider);
    std::shared_ptr<esp::ESPDataModifierInterface> espModifier = nullptr;
#else
    // Create dummy ESP connector
    auto dummyEspProvider = std::make_shared<esp::DummyESPDataProvider>();
    std::shared_ptr<esp::ESPDataProviderInterface> espProvider = dummyEspProvider;
    std::shared_ptr<esp::ESPDataModifierInterface> espModifier = dummyEspProvider;
#endif

    // This observer is notified any time a keyword is detected and notifies the DefaultClient to start recognizing.
    auto keywordObserver =
        std::make_shared<alexaClientSDK::sampleApp::KeywordObserver>(client, wakeWordAudioProvider, espProvider);

#if defined(KWD_KITTAI)
    m_keywordDetector = alexaClientSDK::kwd::KittAiKeyWordDetector::create(
        sharedDataStream,
        compatibleAudioFormat,
        {keywordObserver},
        std::unordered_set<
            std::shared_ptr<alexaClientSDK::avsCommon::sdkInterfaces::KeyWordDetectorStateObserverInterface>>(),
        pathToInputFolder + "/common.res",
        {{pathToInputFolder + "/alexa.umdl", "ALEXA", KITT_AI_SENSITIVITY}},
        KITT_AI_AUDIO_GAIN,
        KITT_AI_APPLY_FRONT_END_PROCESSING);
    if (!m_keywordDetector) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to create KittAiKeyWordDetector!");
        return false;
    }
#elif defined(KWD_SENSORY)
    m_keywordDetector = kwd::SensoryKeywordDetector::create(
        sharedDataStream,
        compatibleAudioFormat,
        {keywordObserver},
        std::unordered_set<
            std::shared_ptr<alexaClientSDK::avsCommon::sdkInterfaces::KeyWordDetectorStateObserverInterface>>(),
        pathToInputFolder + "/spot-alexa-rpi-31000.snsr");
    if (!m_keywordDetector) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to create SensoryKeyWordDetector!");
        return false;
    }
#elif defined(KWD_HARDWARE)
    std::shared_ptr<kwd::AbstractHardwareController> controller = nullptr;

#if defined(ALSA_HW_CTRL)
    std::shared_ptr<kwd::AlsaHardwareController> alsaCtrl = kwd::AlsaHardwareController::create(hwName, "Alexa");
    // Add controller to audio input observers
    client->addAudioInputProcessorObserver(alsaCtrl);
    controller = alsaCtrl;
#endif

    auto paObserver = PortAudioObserver::create(micWrapper);
    if(!paObserver) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint(
                "Failed to create PortAudioObserver!");
        return false;
    }

    // Add the PortAudioObserver to the AudioInputProcessor observers
    client->addAudioInputProcessorObserver(paObserver);

    m_keywordDetector = kwd::HardwareKeywordDetector::create(
        sharedDataStream, 
        compatibleAudioFormat, 
        controller, 
        {paObserver, keywordObserver},
        std::unordered_set<std::shared_ptr<
            alexaClientSDK::avsCommon::sdkInterfaces::KeyWordDetectorStateObserverInterface>>());
    if(!m_keywordDetector) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to create HardwareKeywordDetector!");
        return false;
    }

    client->addAlexaDialogStateObserver(paObserver);
#ifdef ALSA_HW_CTRL
    client->addAlexaDialogStateObserver(alsaCtrl);
#endif

#endif

/*
#ifdef KWD_HARDWARE
    bool startPaStream = false;
#else
    bool startPaStream = true;
#endif
*/

    // If wake word is enabled, then creating the interaction manager with a wake word audio provider.
   m_interactionManager = std::make_shared<alexaClientSDK::sampleApp::InteractionManager>(
        client,
        micWrapper,
        userInterfaceManager,
        holdToTalkAudioProvider,
        tapToTalkAudioProvider,
        wakeWordAudioProvider,
        espProvider,
        espModifier);

#else
    // If wake word is not enabled, then creating the interaction manager without a wake word audio provider.
    m_interactionManager = std::make_shared<alexaClientSDK::sampleApp::InteractionManager>(
        client, micWrapper, userInterfaceManager, holdToTalkAudioProvider, tapToTalkAudioProvider);

#endif

#ifdef KWD_HARDWARE
    // Stopping the audio stream, which is started by the interaction manager by
    // default. The hardware KWD needs this to be muted initially.
    micWrapper->stopStreamingMicrophoneData();
#endif


    client->addAlexaDialogStateObserver(m_interactionManager);


    // Creating the input observer.
    m_userInputManager = alexaClientSDK::sampleApp::UserInputManager::create(m_interactionManager);
    if (!m_userInputManager) {
        alexaClientSDK::sampleApp::ConsolePrinter::simplePrint("Failed to create UserInputManager!");
        return false;
    }

    authDelegate->addAuthObserver(m_userInputManager);
    m_capabilitiesDelegate->addCapabilitiesObserver(m_userInputManager);
    m_capabilitiesDelegate->addCapabilitiesObserver(client);

    // Connect once configuration is all set.
    std::string endpoint;
    sampleAppConfig.getString(ENDPOINT_KEY, &endpoint);

    client->connect(m_capabilitiesDelegate, endpoint);

    // Send default settings set by the user to AVS.
    client->sendDefaultSettings();

    return true;
}

}  // namespace sampleApp
}  // namespace alexaClientSDK
