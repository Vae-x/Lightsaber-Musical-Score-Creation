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


# 图标独立使用与 C++ 编译器同一工具链的 windres，避免其他 MinGW 抢先被找到。
if (WIN32 AND MINGW)
    get_filename_component(LMSC_MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
endif ()
if (ANDROID)
    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../android")
    set(ANDROID_MIN_SDK_VERSION "24" CACHE STRING "Android minimum SDK")
    set(ANDROID_TARGET_SDK_VERSION "34" CACHE STRING "Android target SDK")
endif ()
find_package(Qt5 5.12 REQUIRED COMPONENTS Core Widgets Multimedia Concurrent Network)
if (ANDROID)
    find_package(Qt5 5.15 REQUIRED COMPONENTS AndroidExtras Svg)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif ()

add_library(lmsc_core STATIC
        core/WorkerThread.h
        core/AppInfo.h
        core/DiagnosticLog.cpp core/DiagnosticLog.h core/AiDiagnostics.h
        core/BeatmapDocument.cpp
        core/BeatmapDocument.h
        core/TimeMap.cpp
        core/TimeMap.h
        core/ProjectStore.cpp
        core/ProjectStore.h
        core/SongExporter.cpp core/SongExporter.h
        core/AudioService.cpp
        core/AudioService.h
        core/PcmAudioSnapshot.h
        core/RhythmAnalyzer.cpp
        core/RhythmAnalyzer.h
        core/AppSettings.cpp core/AppSettings.h
        core/AiRecognitionService.cpp core/AiRecognitionService.h
        core/AiTextTransport.cpp core/AiTextTransport.h
        core/CodexTextSession.cpp core/CodexTextSession.h
        core/AiGenerationService.cpp core/AiGenerationService.h
        core/LocalAiGenerationService.cpp core/LocalAiGenerationService.h
        core/InfernoSaberGenerationService.cpp core/InfernoSaberGenerationService.h
        core/LocalChartGenerator.cpp core/LocalChartGenerator.h
        core/SongArrangementPlan.cpp core/SongArrangementPlan.h
        core/HybridAiGenerationService.cpp core/HybridAiGenerationService.h
        core/RefinementTypes.cpp core/RefinementTypes.h
        core/AiRefinementService.cpp core/AiRefinementService.h
        core/MusicFeatureAnalyzer.cpp core/MusicFeatureAnalyzer.h
        core/BeatmapPlayabilityValidator.cpp core/BeatmapPlayabilityValidator.h
        core/ApiModelClient.cpp core/ApiModelClient.h
        core/WinHttpModelTransport.cpp core/WinHttpModelTransport.h
        core/CodexAccountClient.cpp core/CodexAccountClient.h
        core/WorkspacePaths.cpp core/WorkspacePaths.h
        core/MtpImportService.cpp core/MtpImportService.h
        core/MtpExportService.cpp core/MtpExportService.h
        core/MtpDeleteService.cpp core/MtpDeleteService.h)
target_include_directories(lmsc_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(lmsc_core PUBLIC Qt5::Core Qt5::Multimedia Qt5::Network Qt5::Concurrent)
if (ANDROID)
    target_sources(lmsc_core PRIVATE
            core/AndroidStorage.cpp core/AndroidStorage.h
            core/NativeAudioTool.cpp core/NativeAudioTool.h)
    target_link_libraries(lmsc_core PUBLIC Qt5::AndroidExtras)
endif ()
if (WIN32)
    target_link_libraries(lmsc_core PRIVATE crypt32 winhttp)
endif ()

# A console-only, local comparison harness; it never opens or writes a GUI project.
if (NOT ANDROID)
    add_executable(lmsc_generation_trial app/GenerationTrial.cpp)
    target_link_libraries(lmsc_generation_trial PRIVATE lmsc_core)
    set_target_properties(lmsc_generation_trial PROPERTIES OUTPUT_NAME "lmsc-generation-trial")
endif ()

set(LMSC_APP_SOURCES
        app/main.cpp
        gui/MainWindow.cpp
        gui/MainWindowAi.cpp
        gui/MainWindow.h
        gui/MainWindow.ui
        gui/SongImportDialog.cpp gui/SongImportDialog.h
        gui/SongExportDialog.cpp gui/SongExportDialog.h
        gui/EditorViews.cpp
        gui/EditorViews.h
        gui/SettingsDialog.cpp gui/SettingsDialog.h
        gui/SettingsPanel.cpp gui/SettingsPanel.h
        gui/AiRecognitionPage.cpp gui/AiRecognitionPage.h
        gui/TaskProgressView.cpp gui/TaskProgressView.h
        gui/EditorRefinementPanel.cpp gui/EditorRefinementPanel.h
        gui/EditorSessionController.cpp gui/EditorSessionController.h
        gui/NavigationSidebar.cpp gui/NavigationSidebar.h
        gui/ThemeManager.cpp gui/ThemeManager.h
        ../resources/app.qrc)
if (ANDROID)
    add_library(src SHARED ${LMSC_APP_SOURCES})
    # Qt5's Android CMake integration appends the ABI to the library suffix.
    set_target_properties(src PROPERTIES OUTPUT_NAME "LightsaberMusicalScoreCreation")
else ()
    add_executable(src WIN32 ${LMSC_APP_SOURCES})
endif ()
if (WIN32)
    if (MINGW)
        # MinGW 7 的 windres 会错误转义带空格的 -I 路径。此资源仅依赖系统头，独立编译无需继承 Qt 包含路径。
        set(LMSC_ICON_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/app-icon.obj")
        set(LMSC_RESOURCE_DEBUG_FLAG)
        if (CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(LMSC_RESOURCE_DEBUG_FLAG -D_DEBUG)
        endif ()
        add_custom_command(OUTPUT "${LMSC_ICON_OBJECT}"
                COMMAND "${LMSC_MINGW_BIN_DIR}/windres.exe" -O coff ${LMSC_RESOURCE_DEBUG_FLAG}
                -i "${CMAKE_CURRENT_SOURCE_DIR}/../resources/windows/app.rc" -o "${LMSC_ICON_OBJECT}"
                DEPENDS ../resources/windows/app.rc ../resources/icons/app.ico VERBATIM)
        set_source_files_properties("${LMSC_ICON_OBJECT}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
        target_sources(src PRIVATE "${LMSC_ICON_OBJECT}")
    else ()
        target_sources(src PRIVATE ../resources/windows/app.rc)
    endif ()
endif ()
target_include_directories(src PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(src
        lmsc_core
        Qt5::Widgets
        Qt5::Concurrent
)
if (ANDROID)
    target_link_libraries(src Qt5::Svg)
endif ()

option(LMSC_ANDROID_PLATFORM_TESTS "Build the synthetic Android platform test APK library" OFF)
if (ANDROID AND LMSC_ANDROID_PLATFORM_TESTS)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Test)
    add_library(lmsc_android_platform_tests SHARED
            ../tests/AndroidPlatformTestMain.cpp
            ../tests/AndroidAudioTest.cpp
            ../tests/AndroidStorageTest.cpp)
    target_link_libraries(lmsc_android_platform_tests PRIVATE lmsc_core Qt5::Widgets Qt5::Test)
    target_compile_definitions(lmsc_android_platform_tests PRIVATE LMSC_ANDROID_PLATFORM_TESTS)
    set_target_properties(lmsc_android_platform_tests PROPERTIES
            OUTPUT_NAME "LightsaberMusicalScoreCreationPlatformTests")
endif ()

get_filename_component(LMSC_REPOSITORY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set_property(TARGET src APPEND PROPERTY LINK_DEPENDS "${LMSC_REPOSITORY_DIR}/scripts/infernosaber_trial.py")
add_custom_command(TARGET src POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:src>/tools/infernosaber"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${LMSC_REPOSITORY_DIR}/scripts/infernosaber_trial.py" "$<TARGET_FILE_DIR:src>/tools/infernosaber"
        COMMENT "Deploying the offline InfernoSaber runner"
        VERBATIM)
set(LMSC_AUDIO_TOOLS_DIR "${LMSC_REPOSITORY_DIR}/third_party/ffmpeg" CACHE PATH
        "Directory containing FFmpeg bin/, LICENSE.txt and README.md")
option(LMSC_DEPLOY_AUDIO_TOOLS "Deploy the bundled offline audio tools with the application" ON)

if (WIN32 AND NOT CMAKE_CROSSCOMPILING)
    set(LMSC_WPD_HELPER "${CMAKE_CURRENT_BINARY_DIR}/tools/mtp/WpdTransfer.exe")
    set(LMSC_WPD_POWERSHELL "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe")
    add_custom_command(OUTPUT "${LMSC_WPD_HELPER}"
            COMMAND "${LMSC_WPD_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${LMSC_REPOSITORY_DIR}/scripts/build-wpd-helper.ps1"
            -BuildDirectory "${CMAKE_CURRENT_BINARY_DIR}/tools/mtp"
            DEPENDS "${LMSC_REPOSITORY_DIR}/scripts/build-wpd-helper.ps1"
            "${LMSC_REPOSITORY_DIR}/scripts/windows/WpdTransfer.cs"
            COMMENT "构建 64 位 WPD 传输辅助程序" VERBATIM)
    add_custom_target(lmsc_wpd_helper ALL DEPENDS "${LMSC_WPD_HELPER}")
    add_dependencies(src lmsc_wpd_helper)
    add_custom_command(TARGET src POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:src>/tools/mtp"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${LMSC_REPOSITORY_DIR}/scripts/windows/mtp-import.ps1" "$<TARGET_FILE_DIR:src>/tools/mtp"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${LMSC_REPOSITORY_DIR}/LICENSE" "$<TARGET_FILE_DIR:src>/LICENSE"
            COMMENT "部署头显导入工具和项目许可" VERBATIM)
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
    find_package(Qt5 5.12 REQUIRED COMPONENTS Test)
    add_executable(diagnostic_log_tests "${LMSC_REPOSITORY_DIR}/tests/DiagnosticLogTest.cpp")
    target_link_libraries(diagnostic_log_tests PRIVATE lmsc_core Qt5::Test)
    set_target_properties(diagnostic_log_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    find_package(Qt5 5.12 REQUIRED COMPONENTS Test)
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
    add_executable(generated_chart_tests "${LMSC_REPOSITORY_DIR}/tests/GeneratedChartTest.cpp")
    target_link_libraries(generated_chart_tests PRIVATE lmsc_core Qt5::Test)
    set_target_properties(generated_chart_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME generated_chart COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:generated_chart_tests>")
    set_tests_properties(generated_chart PROPERTIES TIMEOUT 60 LABELS "core;ai")
    add_executable(ai_text_transport_tests "${LMSC_REPOSITORY_DIR}/tests/AiTextTransportTest.cpp")
    target_link_libraries(ai_text_transport_tests PRIVATE lmsc_core Qt5::Test)
    set_target_properties(ai_text_transport_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME ai_text_transport COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:ai_text_transport_tests>")
    set_tests_properties(ai_text_transport PROPERTIES TIMEOUT 150 LABELS "ai;network;codex")
    add_executable(ai_generation_tests "${LMSC_REPOSITORY_DIR}/tests/AiGenerationTest.cpp")
    target_link_libraries(ai_generation_tests PRIVATE lmsc_core Qt5::Test)
    set_target_properties(ai_generation_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME ai_generation COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:ai_generation_tests>")
    set_tests_properties(ai_generation PROPERTIES TIMEOUT 180 LABELS "core;ai;audio")
    add_executable(local_generation_tests "${LMSC_REPOSITORY_DIR}/tests/LocalGenerationTest.cpp")
    target_link_libraries(local_generation_tests PRIVATE lmsc_core Qt5::Test)
    set_target_properties(local_generation_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME local_generation COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:local_generation_tests>")
    set_tests_properties(local_generation PROPERTIES TIMEOUT 180 LABELS "core;audio;generation")
    add_executable(infernosaber_generation_tests "${LMSC_REPOSITORY_DIR}/tests/InfernoSaberGenerationTest.cpp")
    target_link_libraries(infernosaber_generation_tests PRIVATE lmsc_core Qt5::Test)
    set_target_properties(infernosaber_generation_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME infernosaber_generation COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:infernosaber_generation_tests>")
    set_tests_properties(infernosaber_generation PROPERTIES TIMEOUT 180 LABELS "core;audio;generation;model")
    add_executable(generation_trial_tests "${LMSC_REPOSITORY_DIR}/tests/GenerationTrialTest.cpp")
    target_link_libraries(generation_trial_tests PRIVATE lmsc_core Qt5::Test)
    target_compile_definitions(generation_trial_tests PRIVATE
            LMSC_TRIAL_EXECUTABLE="$<TARGET_FILE:lmsc_generation_trial>"
            LMSC_TRIAL_TOOLS="${LMSC_AUDIO_TOOLS_DIR}/bin")
    add_dependencies(generation_trial_tests lmsc_generation_trial)
    set_target_properties(generation_trial_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME generation_trial COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:generation_trial_tests>")
    set_tests_properties(generation_trial PROPERTIES TIMEOUT 180 LABELS "core;audio;generation")
    foreach(LMSC_HYBRID_TEST IN ITEMS HybridGeneration AiRefinement RefinedChart)
        string(REGEX REPLACE "([a-z])([A-Z])" "\\1_\\2" LMSC_HYBRID_NAME "${LMSC_HYBRID_TEST}")
        string(TOLOWER "${LMSC_HYBRID_NAME}" LMSC_HYBRID_NAME)
        add_executable(${LMSC_HYBRID_NAME}_tests "${LMSC_REPOSITORY_DIR}/tests/${LMSC_HYBRID_TEST}Test.cpp")
        target_link_libraries(${LMSC_HYBRID_NAME}_tests PRIVATE lmsc_core Qt5::Test)
        set_target_properties(${LMSC_HYBRID_NAME}_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
        add_test(NAME ${LMSC_HYBRID_NAME} COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:${LMSC_HYBRID_NAME}_tests>")
        set_tests_properties(${LMSC_HYBRID_NAME} PROPERTIES TIMEOUT 180 LABELS "core;audio;ai;generation")
    endforeach()
    add_executable(editing_draft_tests "${LMSC_REPOSITORY_DIR}/tests/EditingDraftTest.cpp")
    target_link_libraries(editing_draft_tests PRIVATE lmsc_core Qt5::Test)
    set_target_properties(editing_draft_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME editing_draft COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:editing_draft_tests>")
    set_tests_properties(editing_draft PROPERTIES TIMEOUT 90 LABELS "core;editing")
    add_executable(editor_session_tests "${LMSC_REPOSITORY_DIR}/tests/EditorSessionControllerTest.cpp"
            gui/EditorSessionController.cpp gui/EditorSessionController.h)
    target_link_libraries(editor_session_tests PRIVATE lmsc_core Qt5::Widgets Qt5::Test)
    set_target_properties(editor_session_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME editor_session COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:editor_session_tests>")
    set_tests_properties(editor_session PROPERTIES TIMEOUT 90 LABELS "gui;editing")
    add_executable(editor_refinement_panel_tests "${LMSC_REPOSITORY_DIR}/tests/EditorRefinementPanelTest.cpp"
            gui/EditorRefinementPanel.cpp gui/EditorRefinementPanel.h
            gui/TaskProgressView.cpp gui/TaskProgressView.h
            gui/AiRecognitionPage.cpp gui/AiRecognitionPage.h gui/ThemeManager.cpp gui/ThemeManager.h
            ../resources/app.qrc)
    target_link_libraries(editor_refinement_panel_tests PRIVATE lmsc_core Qt5::Widgets Qt5::Test)
    set_target_properties(editor_refinement_panel_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME editor_refinement_panel COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:editor_refinement_panel_tests>")
    set_tests_properties(editor_refinement_panel PROPERTIES TIMEOUT 90 LABELS "gui;editing;ai")
    add_executable(ai_generation_page_tests
            "${LMSC_REPOSITORY_DIR}/tests/AiGenerationPageTest.cpp"
            gui/AiRecognitionPage.cpp gui/AiRecognitionPage.h
            gui/GenerationPreviewDialog.cpp gui/GenerationPreviewDialog.h
            gui/EditorViews.cpp gui/EditorViews.h
            gui/ThemeManager.cpp gui/ThemeManager.h)
    target_link_libraries(ai_generation_page_tests PRIVATE lmsc_core Qt5::Widgets Qt5::Test)
    target_sources(ai_generation_page_tests PRIVATE ../resources/app.qrc)
    set_target_properties(ai_generation_page_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME ai_generation_page COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:ai_generation_page_tests>")
    set_tests_properties(ai_generation_page PROPERTIES TIMEOUT 90 LABELS "gui;ai")
    add_executable(ai_settings_tests "${LMSC_REPOSITORY_DIR}/tests/AiSettingsTest.cpp")
    target_link_libraries(ai_settings_tests PRIVATE lmsc_core Qt5::Test)
    set_target_properties(ai_settings_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME ai_settings COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:ai_settings_tests>")
    set_tests_properties(ai_settings PROPERTIES TIMEOUT 60 LABELS "settings;network")
    add_executable(ai_recognition_tests
            "${LMSC_REPOSITORY_DIR}/tests/AiRecognitionTest.cpp"
            gui/AiRecognitionPage.cpp gui/AiRecognitionPage.h)
    target_link_libraries(ai_recognition_tests PRIVATE lmsc_core Qt5::Widgets Qt5::Test)
    set_target_properties(ai_recognition_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME ai_recognition COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:ai_recognition_tests>")
    set_tests_properties(ai_recognition PROPERTIES TIMEOUT 60 LABELS "gui;ai")
    add_executable(codex_account_tests "${LMSC_REPOSITORY_DIR}/tests/CodexAccountTest.cpp")
    target_link_libraries(codex_account_tests PRIVATE lmsc_core Qt5::Test)
    set_target_properties(codex_account_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME codex_account COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:codex_account_tests>")
    set_tests_properties(codex_account PROPERTIES TIMEOUT 90 LABELS "settings;codex")
    add_executable(settings_dialog_tests
            "${LMSC_REPOSITORY_DIR}/tests/SettingsDialogTest.cpp"
            gui/SettingsDialog.cpp gui/SettingsDialog.h
            gui/SettingsPanel.cpp gui/SettingsPanel.h
            gui/NavigationSidebar.cpp gui/NavigationSidebar.h
            gui/ThemeManager.cpp gui/ThemeManager.h
            gui/EditorViews.cpp gui/EditorViews.h)
    target_sources(settings_dialog_tests PRIVATE ../resources/app.qrc)
    target_link_libraries(settings_dialog_tests PRIVATE lmsc_core Qt5::Widgets Qt5::Test)
    set_target_properties(settings_dialog_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME settings_dialog COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:settings_dialog_tests>")
    set_tests_properties(settings_dialog PROPERTIES TIMEOUT 90 LABELS "settings;gui")
    add_executable(mtp_import_tests "${LMSC_REPOSITORY_DIR}/tests/MtpImportTest.cpp")
    target_link_libraries(mtp_import_tests PRIVATE lmsc_core)
    set_target_properties(mtp_import_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME mtp_import_service COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:mtp_import_tests>")
    set_tests_properties(mtp_import_service PROPERTIES TIMEOUT 60 LABELS "mtp"
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    if (WIN32 AND NOT CMAKE_CROSSCOMPILING)
        add_test(NAME mtp_category_scan COMMAND "${LMSC_WPD_POWERSHELL}"
                -NoProfile -NonInteractive -ExecutionPolicy Bypass -File
                "${LMSC_REPOSITORY_DIR}/tests/MtpScanTest.ps1"
                -SourcePath "${LMSC_REPOSITORY_DIR}/scripts/windows/mtp-import.ps1")
        set_tests_properties(mtp_category_scan PROPERTIES TIMEOUT 60 LABELS "mtp")
    endif ()
    add_executable(mtp_export_tests "${LMSC_REPOSITORY_DIR}/tests/MtpExportTest.cpp")
    target_link_libraries(mtp_export_tests PRIVATE lmsc_core)
    if (TARGET lmsc_wpd_helper)
        add_dependencies(mtp_export_tests lmsc_wpd_helper)
    endif ()
    set_target_properties(mtp_export_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME mtp_export_service COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:mtp_export_tests>")
    set_tests_properties(mtp_export_service PROPERTIES TIMEOUT 90 LABELS "mtp"
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_executable(mtp_delete_tests "${LMSC_REPOSITORY_DIR}/tests/MtpDeleteTest.cpp")
    target_link_libraries(mtp_delete_tests PRIVATE lmsc_core)
    if (TARGET lmsc_wpd_helper)
        add_dependencies(mtp_delete_tests lmsc_wpd_helper)
    endif ()
    set_target_properties(mtp_delete_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME mtp_delete_service COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:mtp_delete_tests>"
            --helper "${LMSC_WPD_HELPER}")
    set_tests_properties(mtp_delete_service PROPERTIES TIMEOUT 90 LABELS "mtp"
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_executable(workspace_paths_tests "${LMSC_REPOSITORY_DIR}/tests/WorkspacePathsTest.cpp")
    target_link_libraries(workspace_paths_tests PRIVATE lmsc_core)
    set_target_properties(workspace_paths_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    add_test(NAME workspace_paths COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:workspace_paths_tests>")
    set_tests_properties(workspace_paths PROPERTIES TIMEOUT 60 LABELS "core"
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
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

        add_executable(song_exporter_tests "${LMSC_REPOSITORY_DIR}/tests/SongExporterTest.cpp")
        target_link_libraries(song_exporter_tests PRIVATE lmsc_core)
        set_target_properties(song_exporter_tests PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
        add_test(NAME song_exporter COMMAND ${LMSC_TEST_ENVIRONMENT}
                "$<TARGET_FILE:song_exporter_tests>" "${LMSC_AUDIO_TOOLS_DIR}/bin")
        set_tests_properties(song_exporter PROPERTIES TIMEOUT 90 LABELS "core;audio"
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")

        find_package(Qt5 5.12 REQUIRED COMPONENTS Test)
        add_executable(main_window_tests
                "${LMSC_REPOSITORY_DIR}/tests/MainWindowTest.cpp"
                gui/MainWindow.cpp gui/MainWindow.h gui/MainWindow.ui
                gui/MainWindowAi.cpp
                gui/EditorViews.cpp gui/EditorViews.h)
        target_sources(main_window_tests PRIVATE
                gui/SongImportDialog.cpp gui/SongImportDialog.h
                gui/SongExportDialog.cpp gui/SongExportDialog.h
                gui/SettingsDialog.cpp gui/SettingsDialog.h
                gui/SettingsPanel.cpp gui/SettingsPanel.h
                gui/AiRecognitionPage.cpp gui/AiRecognitionPage.h
                gui/TaskProgressView.cpp gui/TaskProgressView.h
                gui/EditorRefinementPanel.cpp gui/EditorRefinementPanel.h
                gui/EditorSessionController.cpp gui/EditorSessionController.h
                gui/GenerationPreviewDialog.cpp gui/GenerationPreviewDialog.h
                gui/NavigationSidebar.cpp gui/NavigationSidebar.h
                gui/ThemeManager.cpp gui/ThemeManager.h)
        target_sources(main_window_tests PRIVATE ../resources/app.qrc)
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

foreach(LMSC_TASK_UI_TARGET ai_generation_page_tests ai_recognition_tests)
    if(TARGET ${LMSC_TASK_UI_TARGET})
        target_sources(${LMSC_TASK_UI_TARGET} PRIVATE gui/TaskProgressView.cpp gui/TaskProgressView.h)
    endif()
endforeach()
foreach(LMSC_LOG_UI_TARGET src ai_generation_page_tests ai_recognition_tests settings_dialog_tests main_window_tests editor_refinement_panel_tests)
    if(TARGET ${LMSC_LOG_UI_TARGET})
        target_sources(${LMSC_LOG_UI_TARGET} PRIVATE gui/DiagnosticLogDialog.cpp gui/DiagnosticLogDialog.h)
    endif()
endforeach()
if(TARGET diagnostic_log_tests)
    add_test(NAME diagnostic_log COMMAND ${LMSC_TEST_ENVIRONMENT} "$<TARGET_FILE:diagnostic_log_tests>")
    set_tests_properties(diagnostic_log PROPERTIES TIMEOUT 60 LABELS "core;diagnostics")
endif()
