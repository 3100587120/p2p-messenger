cmake_minimum_required(VERSION 3.21)
project(P2PMessenger VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick)
if(NOT WIN32)
    find_package(OpenSSL REQUIRED)
endif()

option(P2P_MESSENGER_WITH_DAEMON "Link the private Jami daemon engine" OFF)
set(P2P_MESSENGER_DAEMON_INCLUDE_DIR "" CACHE PATH "Directory containing jami/jami.h")
set(P2P_MESSENGER_DAEMON_LIBRARY "" CACHE FILEPATH "Private daemon library")

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(P2PMessenger
    src/main.cpp
    src/messenger_controller.cpp
    src/messenger_controller.h
    src/daemon_bridge.cpp
    src/daemon_bridge.h
    src/local_vault.cpp
    src/local_vault.h
    src/private_network_config.cpp
    src/private_network_config.h
)

qt_add_qml_module(P2PMessenger
    URI P2PMessenger
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

target_link_libraries(P2PMessenger PRIVATE Qt6::Quick)

if(NOT WIN32)
    target_link_libraries(P2PMessenger PRIVATE OpenSSL::Crypto)
endif()

if(WIN32)
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
endif()

set_target_properties(P2PMessenger PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

include(GNUInstallDirs)
install(TARGETS P2PMessenger
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
