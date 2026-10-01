cmake_minimum_required(VERSION 4.0)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # Keep the existing CLion project opened at src/ usable on its own.
    project(src LANGUAGES CXX)
    include(CTest)
endif ()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)


find_package(Qt5 5.12 REQUIRED COMPONENTS Core Widgets Multimedia Concurrent)

add_library(lmsc_core STATIC
        core/BeatmapDocument.cpp
        core/BeatmapDocument.h
        core/TimeMap.cpp
        core/TimeMap.h
        core/ProjectStore.cpp
        core/ProjectStore.h
        core/AudioService.cpp
        core/AudioService.h
        core/RhythmAnalyzer.cpp
        core/RhythmAnalyzer.h)
target_include_directories(lmsc_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(lmsc_core PUBLIC Qt5::Core Qt5::Multimedia)

add_executable(src WIN32
        app/main.cpp
        gui/MainWindow.cpp
        gui/MainWindow.h
        gui/MainWindow.ui
        gui/EditorViews.cpp
        gui/EditorViews.h)
target_include_directories(src PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(src
        lmsc_core
        Qt5::Widgets
        Qt5::Concurrent
)

get_filename_component(LMSC_REPOSITORY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(LMSC_AUDIO_TOOLS_DIR "${LMSC_REPOSITORY_DIR}/third_party/ffmpeg" CACHE PATH
        "Directory containing FFmpeg bin/, LICENSE.txt and README.md")
option(LMSC_DEPLOY_AUDIO_TOOLS "Deploy the bundled offline audio tools with the application" ON)

if (WIN32 AND NOT CMAKE_CROSSCOMPILING)
    # Resolve the deployment tool from the Qt package that was actually found.
    # CMAKE_PREFIX_PATH may be empty when CLion supplies Qt5_DIR instead.
    get_target_property(QT_QMAKE_EXECUTABLE Qt5::qmake IMPORTED_LOCATION)
    get_filename_component(QT_BIN_DIR "${QT_QMAKE_EXECUTABLE}" DIRECTORY)
    set(QT_WINDEPLOYQT_EXECUTABLE "${QT_BIN_DIR}/windeployqt.exe")
    if (NOT EXISTS "${QT_WINDEPLOYQT_EXECUTABLE}")
        message(FATAL_ERROR "Qt deployment tool not found: ${QT_WINDEPLOYQT_EXECUTABLE}")
    endif ()

    get_filename_component(COMPILER_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    add_custom_command(TARGET src POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E env
            --modify "PATH=path_list_prepend:${COMPILER_BIN_DIR}"
            --modify "PATH=path_list_prepend:${QT_BIN_DIR}"
            -- "${QT_WINDEPLOYQT_EXECUTABLE}"
            "$<IF:$<CONFIG:Debug>,--debug,--release>"
            --no-translations --compiler-runtime
            "$<TARGET_FILE:src>"
            COMMENT "Deploying Qt and compiler runtime dependencies"
            VERBATIM)

    # QAudioOutput discovers this plugin at runtime. Copy it explicitly because
    # scanning imported DLLs alone does not reliably identify audio backends.
    set(LMSC_WINDOWS_AUDIO_PLUGIN
            "${QT_BIN_DIR}/../plugins/audio/qtaudio_windows$<$<CONFIG:Debug>:d>.dll")
    add_custom_command(TARGET src POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:src>/audio"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${LMSC_WINDOWS_AUDIO_PLUGIN}" "$<TARGET_FILE_DIR:src>/audio"
            COMMENT "Deploying the Windows audio backend"
            VERBATIM)

    if (LMSC_DEPLOY_AUDIO_TOOLS)
        if (NOT EXISTS "${LMSC_AUDIO_TOOLS_DIR}/bin/ffmpeg.exe" OR
                NOT EXISTS "${LMSC_AUDIO_TOOLS_DIR}/bin/ffprobe.exe")
            message(FATAL_ERROR "Bundled FFmpeg tools are missing. Run scripts/setup-audio-tools.ps1 before building, or set LMSC_DEPLOY_AUDIO_TOOLS=OFF for a core-only build.")
        endif ()
        add_custom_command(TARGET src POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${LMSC_AUDIO_TOOLS_DIR}/bin" "$<TARGET_FILE_DIR:src>/tools/ffmpeg"
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${LMSC_AUDIO_TOOLS_DIR}/LICENSE.txt"
                "${LMSC_AUDIO_TOOLS_DIR}/README.md"
                "$<TARGET_FILE_DIR:src>/tools/ffmpeg"
                COMMENT "Deploying FFmpeg tools and their license information"
                VERBATIM)
    endif ()
endif ()

if (BUILD_TESTING)
    # Both entry points use these same tests; the paths are independent of cwd.
    add_executable(core_tests "${LMSC_REPOSITORY_DIR}/tests/core_tests.cpp")
    target_link_libraries(core_tests PRIVATE lmsc_core)
    set_target_properties(core_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    if (WIN32 AND NOT CMAKE_CROSSCOMPILING)
        set(LMSC_TEST_ENVIRONMENT "${CMAKE_COMMAND}" -E env
                --modify "PATH=path_list_prepend:${COMPILER_BIN_DIR}"
                --modify "PATH=path_list_prepend:${QT_BIN_DIR}"
                --modify "QT_PLUGIN_PATH=path_list_prepend:${QT_BIN_DIR}/../plugins"
                --modify "QT_FORCE_STDERR_LOGGING=set:1" --)
    else ()
        set(LMSC_TEST_ENVIRONMENT "${CMAKE_COMMAND}" -E env --)
    endif ()

    add_test(NAME core_editing COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:core_tests>")
    set_tests_properties(core_editing PROPERTIES TIMEOUT 60 LABELS "core"
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    file(GLOB LMSC_SAMPLE_PACKAGES "${LMSC_REPOSITORY_DIR}/samples/beatmaps/*.zip")
    if (LMSC_SAMPLE_PACKAGES)
        add_test(NAME core_samples COMMAND ${LMSC_TEST_ENVIRONMENT}
                "$<TARGET_FILE:core_tests>" "${LMSC_REPOSITORY_DIR}/samples/beatmaps")
        set_tests_properties(core_samples PROPERTIES TIMEOUT 180 LABELS "core;samples"
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    endif ()

    if (WIN32 AND EXISTS "${LMSC_AUDIO_TOOLS_DIR}/bin/ffmpeg.exe")
        find_program(LMSC_POWERSHELL_EXECUTABLE NAMES powershell pwsh REQUIRED)
        set(LMSC_AUDIO_FIXTURES_DIR "${CMAKE_CURRENT_BINARY_DIR}/tests/audio-fixtures")
        add_executable(audio_service_tests "${LMSC_REPOSITORY_DIR}/tests/AudioServiceTest.cpp")
        target_link_libraries(audio_service_tests PRIVATE lmsc_core)
        set_target_properties(audio_service_tests PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
        add_test(NAME audio_fixtures COMMAND "${LMSC_POWERSHELL_EXECUTABLE}"
                -NoProfile -ExecutionPolicy Bypass -File
                "${LMSC_REPOSITORY_DIR}/tests/GenerateAudioFixtures.ps1"
                -OutputDirectory "${LMSC_AUDIO_FIXTURES_DIR}"
                -ToolsDirectory "${LMSC_AUDIO_TOOLS_DIR}/bin")
        set_tests_properties(audio_fixtures PROPERTIES
                FIXTURES_SETUP audio_media TIMEOUT 60 LABELS "audio")
        add_test(NAME audio_service COMMAND ${LMSC_TEST_ENVIRONMENT}
                "$<TARGET_FILE:audio_service_tests>" "${LMSC_AUDIO_FIXTURES_DIR}"
                "${LMSC_AUDIO_TOOLS_DIR}/bin")
        set_tests_properties(audio_service PROPERTIES
                FIXTURES_REQUIRED audio_media TIMEOUT 90
                LABELS "audio;requires-audio-device"
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")

        find_package(Qt5 5.12 REQUIRED COMPONENTS Test)
        add_executable(main_window_tests
                "${LMSC_REPOSITORY_DIR}/tests/MainWindowTest.cpp"
                gui/MainWindow.cpp gui/MainWindow.h gui/MainWindow.ui
                gui/EditorViews.cpp gui/EditorViews.h)
        target_link_libraries(main_window_tests PRIVATE
                lmsc_core Qt5::Widgets Qt5::Concurrent Qt5::Test)
        target_compile_definitions(main_window_tests PRIVATE
                LMSC_REPOSITORY_DIR="${LMSC_REPOSITORY_DIR}"
                LMSC_AUDIO_TOOLS_DIR="${LMSC_AUDIO_TOOLS_DIR}"
                LMSC_AUDIO_FIXTURES_DIR="${LMSC_AUDIO_FIXTURES_DIR}")
        set_target_properties(main_window_tests PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
        add_test(NAME main_window_editor COMMAND ${LMSC_TEST_ENVIRONMENT}
                "$<TARGET_FILE:main_window_tests>")
        set_tests_properties(main_window_editor PROPERTIES
                FIXTURES_REQUIRED audio_media TIMEOUT 150 LABELS "gui;integration"
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    endif ()
endif ()
