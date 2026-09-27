cmake_minimum_required(VERSION 3.21)
project(P2PMessenger VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2 Network WebSockets)
if(NOT WIN32 AND NOT ANDROID)
    find_package(OpenSSL REQUIRED)
endif()

option(P2P_MESSENGER_WITH_DAEMON "Link the private Jami daemon engine" ON)
option(P2P_MESSENGER_BUILD_DAEMON_E2E "Build a local two-identity daemon integration test" OFF)
option(P2P_MESSENGER_BUILD_RELAY_TESTS "Build relay encryption tests" OFF)
set(P2P_MESSENGER_DAEMON_INCLUDE_DIR "" CACHE PATH "Directory containing jami/jami.h")
set(P2P_MESSENGER_DAEMON_LIBRARY "" CACHE FILEPATH "Private daemon library")

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(P2PMessenger
    src/main.cpp
    src/messenger_controller.cpp
    src/messenger_controller.h
    src/daemon_bridge.cpp
    src/daemon_bridge.h
    src/gateway_mapper.cpp
    src/gateway_mapper.h
    src/local_vault.cpp
    src/local_vault.h
    src/relay_crypto.cpp
    src/relay_crypto.h
    src/relay_client.cpp
    src/relay_client.h
    src/private_network_config.cpp
    src/private_network_config.h
)

qt_add_qml_module(P2PMessenger
    URI P2PMessenger
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

target_link_libraries(P2PMessenger PRIVATE Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::WebSockets)

if(NOT WIN32 AND NOT ANDROID)
    target_link_libraries(P2PMessenger PRIVATE OpenSSL::Crypto)
endif()

if(WIN32)
    enable_language(RC)
    target_sources(P2PMessenger PRIVATE windows/app_icon.rc)
    target_link_libraries(P2PMessenger PRIVATE crypt32 bcrypt)
endif()

if(APPLE)
    target_sources(P2PMessenger PRIVATE src/local_vault_apple.mm)
    target_link_libraries(P2PMessenger PRIVATE "-framework Security")
endif()

if(ANDROID)
    set_property(TARGET P2PMessenger APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
        ${CMAKE_CURRENT_SOURCE_DIR}/android)
endif()

if(P2P_MESSENGER_WITH_DAEMON)
    if(NOT P2P_MESSENGER_DAEMON_INCLUDE_DIR OR NOT P2P_MESSENGER_DAEMON_LIBRARY)
        message(FATAL_ERROR "Set P2P_MESSENGER_DAEMON_INCLUDE_DIR and P2P_MESSENGER_DAEMON_LIBRARY when enabling the daemon engine")
    endif()
    target_compile_definitions(P2PMessenger PRIVATE P2P_MESSENGER_WITH_DAEMON=1)
    target_include_directories(P2PMessenger PRIVATE ${P2P_MESSENGER_DAEMON_INCLUDE_DIR})
    target_link_libraries(P2PMessenger PRIVATE ${P2P_MESSENGER_DAEMON_LIBRARY})
    if(ANDROID)
        get_filename_component(_daemon_root "${P2P_MESSENGER_DAEMON_INCLUDE_DIR}" DIRECTORY)
        target_include_directories(P2PMessenger PRIVATE
            "${_daemon_root}/contrib/aarch64-linux-android/include")
        set_property(TARGET P2PMessenger APPEND PROPERTY QT_ANDROID_EXTRA_LIBS
            "${P2P_MESSENGER_DAEMON_LIBRARY}")
        target_link_libraries(P2PMessenger PRIVATE
            "${_daemon_root}/contrib/aarch64-linux-android/lib/libcrypto.a")
    endif()
    if(WIN32)
        get_filename_component(_daemon_root "${P2P_MESSENGER_DAEMON_INCLUDE_DIR}" DIRECTORY)
        set(_contrib "${_daemon_root}/contrib")
        target_include_directories(P2PMessenger PRIVATE
            "${_contrib}/msvc/include"
            "${_contrib}/build/openssl/include")
        target_link_directories(P2PMessenger PRIVATE "${_contrib}/msvc/lib/x64")
        target_link_libraries(P2PMessenger PRIVATE
            "${_contrib}/build/ffmpeg/Build/win32/x64/bin/avcodec.lib"
            "${_contrib}/build/ffmpeg/Build/win32/x64/bin/avdevice.lib"
            "${_contrib}/build/ffmpeg/Build/win32/x64/bin/avfilter.lib"
            "${_contrib}/build/ffmpeg/Build/win32/x64/bin/avformat.lib"
            "${_contrib}/build/ffmpeg/Build/win32/x64/bin/avutil.lib"
            "${_contrib}/build/ffmpeg/Build/win32/x64/bin/swresample.lib"
            "${_contrib}/build/ffmpeg/Build/win32/x64/bin/swscale.lib"
            "${_contrib}/msvc/lib/x64/libgnutls.lib"
            "${_contrib}/msvc/lib/x64/lib_json.lib"
            "${_contrib}/build/opendht/build/Release/libopendht.lib"
            "${_contrib}/build/argon2/vs2015/Argon2Ref/vs2015/build/Argon2Ref.lib"
            "${_contrib}/msvc/lib/x64/secp256k1.lib"
            "${_contrib}/build/yaml-cpp/msvc/Release/libyaml-cppmd.lib"
            "${_contrib}/msvc/lib/x64/libupnp.lib"
            "${_contrib}/build/natpmp/msvc/Release/natpmp.lib"
            "${_contrib}/msvc/lib/x64/archive_static.lib"
            "${_contrib}/build/pjproject/pjsip/lib/pjsip-core-x86_64-x64-vc15-Release.lib"
            "${_contrib}/build/pjproject/pjsip/lib/pjsip-simple-x86_64-x64-vc15-Release.lib"
            "${_contrib}/build/pjproject/pjsip/lib/pjsua2-lib-x86_64-x64-vc15-Release.lib"
            "${_contrib}/build/pjproject/pjsip/lib/pjsua-lib-x86_64-x64-vc15-Release.lib"
            "${_contrib}/build/pjproject/pjsip/lib/pjsip-ua-x86_64-x64-vc15-Release.lib"
            "${_contrib}/build/pjproject/pjmedia/lib/pjmedia-x86_64-x64-vc15-Release.lib"
            "${_contrib}/build/pjproject/pjlib-util/lib/pjlib-util-x86_64-x64-vc15-Release.lib"
            "${_contrib}/build/pjproject/pjlib/lib/pjlib-x86_64-x64-vc15-Release.lib"
            "${_contrib}/build/pjproject/pjnath/lib/pjnath-x86_64-x64-vc15-Release.lib"
            "${_contrib}/build/fmt/msvc/Release/fmt.lib"
            "${_contrib}/build/http_parser/x64/Release/http-parser.lib"
            "${_contrib}/build/openssl/libcrypto.lib"
            "${_contrib}/build/openssl/libssl.lib"
            "${_contrib}/build/speexdsp/lib/libspeexdsp.lib"
            "${_contrib}/build/webrtc-audio-processing/build/Release/webrtc-audio-processing.lib"
            "${_contrib}/build/portaudio/build/Release/portaudio_static_x64.lib"
            "${_contrib}/build/libgit2/msvc/Release/git2.lib"
            ws2_32 advapi32 strmiids
        )
        target_link_options(P2PMessenger PRIVATE /LTCG /ignore:4006)
    endif()
endif()

set_target_properties(P2PMessenger PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

include(GNUInstallDirs)
install(TARGETS P2PMessenger
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if(P2P_MESSENGER_BUILD_DAEMON_E2E)
    if(NOT WIN32 OR NOT P2P_MESSENGER_WITH_DAEMON)
        message(FATAL_ERROR "The daemon end-to-end test currently requires the Windows daemon build")
    endif()
    add_executable(P2PMessengerDaemonE2E tests/daemon_e2e.cpp)
    target_include_directories(P2PMessengerDaemonE2E PRIVATE
        ${P2P_MESSENGER_DAEMON_INCLUDE_DIR} "${_contrib}/msvc/include")
    target_link_directories(P2PMessengerDaemonE2E PRIVATE "${_contrib}/msvc/lib/x64")
    get_target_property(_p2p_messenger_links P2PMessenger LINK_LIBRARIES)
    target_link_libraries(P2PMessengerDaemonE2E PRIVATE ${_p2p_messenger_links})
    target_link_options(P2PMessengerDaemonE2E PRIVATE /LTCG /ignore:4006)

    add_executable(P2PMessengerGatewayTest
        tests/gateway_mapper_test.cpp
        src/gateway_mapper.cpp
        src/gateway_mapper.h)
    target_compile_definitions(P2PMessengerGatewayTest PRIVATE P2P_MESSENGER_GATEWAY_TESTING=1)
    target_include_directories(P2PMessengerGatewayTest PRIVATE src)
    target_link_libraries(P2PMessengerGatewayTest PRIVATE Qt6::Network)
endif()

if(P2P_MESSENGER_BUILD_RELAY_TESTS AND WIN32)
    add_executable(P2PMessengerRelayCryptoTest
        tests/relay_crypto_test.cpp
        src/relay_crypto.cpp src/relay_crypto.h
        src/local_vault.cpp src/local_vault.h)
    target_include_directories(P2PMessengerRelayCryptoTest PRIVATE
        src "${_contrib}/build/openssl/include")
    target_link_libraries(P2PMessengerRelayCryptoTest PRIVATE
        Qt6::Core crypt32 bcrypt "${_contrib}/build/openssl/libcrypto.lib")

    add_executable(P2PMessengerRelayClientTest
        tests/relay_client_test.cpp
        src/relay_client.cpp src/relay_client.h
        src/relay_crypto.cpp src/relay_crypto.h
        src/local_vault.cpp src/local_vault.h)
    target_include_directories(P2PMessengerRelayClientTest PRIVATE
        src "${_contrib}/build/openssl/include")
    target_link_libraries(P2PMessengerRelayClientTest PRIVATE
        Qt6::Core Qt6::Network Qt6::WebSockets crypt32 bcrypt
        "${_contrib}/build/openssl/libcrypto.lib")
endif()
