cmake_minimum_required(VERSION 3.21)
project(P2PMessenger VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(P2PMessenger
    src/main.cpp
    src/messenger_controller.cpp
    src/messenger_controller.h
)

qt_add_qml_module(P2PMessenger
    URI P2PMessenger
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

target_link_libraries(P2PMessenger PRIVATE Qt6::Quick)

set_target_properties(P2PMessenger PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

include(GNUInstallDirs)
install(TARGETS P2PMessenger
    BUNDLE DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
