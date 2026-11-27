cmake_minimum_required(VERSION 3.24)

project(CodexOrganizer VERSION 0.3.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(codex_organizer_core STATIC
    src/core.cpp
    src/windows.cpp
)
target_include_directories(codex_organizer_core PUBLIC include)
target_compile_definitions(codex_organizer_core PUBLIC
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)
target_link_libraries(codex_organizer_core PUBLIC
    dwmapi
    ole32
    oleaut32
    uiautomationcore
    user32
    winhttp
)

add_executable(CodexOrganizer WIN32
    src/main.cpp
    src/gui.cpp
    resources/codex-organizer.rc
)
target_include_directories(CodexOrganizer PRIVATE resources)
target_link_libraries(CodexOrganizer PRIVATE
    codex_organizer_core
    comctl32
    shell32
    uxtheme
)

add_executable(codex_organizer_tests
    tests/native_tests.cpp
)
target_link_libraries(codex_organizer_tests PRIVATE codex_organizer_core)
target_link_libraries(codex_organizer_tests PRIVATE ws2_32)

if(MSVC)
    foreach(target codex_organizer_core CodexOrganizer codex_organizer_tests)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /EHsc /sdl /guard:cf)
    endforeach()
    # The application manifest is embedded through the .rc file so the icon,
    # version resource, DPI settings, and Common Controls v6 remain one unit.
    target_link_options(CodexOrganizer PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT /MANIFEST:NO)
    # Preserve optimized Release behavior while emitting symbols for the
    # project-local per-test native function execution collector.
    target_compile_options(codex_organizer_core PRIVATE /Zi)
    target_compile_options(codex_organizer_tests PRIVATE /Zi)
    target_link_options(codex_organizer_tests PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT /DEBUG:FULL)
endif()

add_custom_command(TARGET CodexOrganizer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/config.json"
            "$<TARGET_FILE_DIR:CodexOrganizer>/config.json"
    VERBATIM
)

enable_testing()
add_test(NAME native-regressions COMMAND codex_organizer_tests)

install(TARGETS CodexOrganizer RUNTIME DESTINATION .)
install(FILES config.json DESTINATION .)
install(FILES
    resources/codex-organizer.ico
    resources/codex-organizer-icon.png
    DESTINATION resources
)

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT CodexOrganizer)
