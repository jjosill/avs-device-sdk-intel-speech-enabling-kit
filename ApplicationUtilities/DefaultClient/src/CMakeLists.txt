cmake_minimum_required(VERSION 3.1 FATAL_ERROR)

add_definitions("-DACSDK_LOG_MODULE=defaultClient")
add_library(DefaultClient SHARED
    DefaultClient.cpp)
target_include_directories(DefaultClient PUBLIC
    "${DefaultClient_SOURCE_DIR}/include" "${MRM_SOURCE_DIR}/include")


target_link_libraries(DefaultClient
    AVSCommon
    ACL
    ADSL
    AFML
    AIP
    Alerts
    Notifications
    PlaybackController
    SpeakerManager
    SpeechSynthesizer
    Settings
    TemplateRuntime
    AudioPlayer
    ExternalMediaPlayer
    AVSSystem
    ContextManager
    RegistrationManager)

if (COMMS)
    target_link_libraries(DefaultClient CallManager)
endif()

if (MRM)
    target_link_libraries(DefaultClient MRM MRMHandler)
endif()

# install target
asdk_install()
