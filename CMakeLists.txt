cmake_minimum_required(VERSION 3.21)
project(GroovyGrab LANGUAGES C CXX RC)

set(LZ4_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lz4")
set(TURBOJPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libjpeg-turbo/sdk")
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
set(IMGUI_SOURCES
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)

add_executable(GroovyGrab WIN32
    src/main.cpp
    src/app.cpp
    src/app_gui.cpp
    src/app_window.cpp
    src/format_menu.cpp
    src/preview_renderer.cpp
    src/directshow_capture.cpp
    src/audio_capture.cpp
    src/async_sender.cpp
    src/async_calibrate.cpp
    src/field_prep.cpp
    src/field_prep_avx2.cpp
    src/groovy_sender.cpp
    src/latency_probe.cpp
    src/rate_lock.cpp
    src/groovygrab.rc
    "${LZ4_DIR}/lz4.c"
    ${IMGUI_SOURCES}
)
target_compile_features(GroovyGrab PRIVATE cxx_std_20)
target_compile_definitions(GroovyGrab PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(GroovyGrab PRIVATE
    "${LZ4_DIR}" "${IMGUI_DIR}" "${IMGUI_DIR}/backends"
    "${TURBOJPEG_DIR}/include")
target_compile_options(GroovyGrab PRIVATE /O2 /W4 /permissive-)
target_link_options(GroovyGrab PRIVATE /ENTRY:wmainCRTStartup /MANIFEST:NO) # groovygrab.rc embeds it
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_FLAGS "/W3")
target_link_libraries(GroovyGrab PRIVATE
    strmiids ole32 oleaut32
    d3d11 dxgi d3dcompiler
    avrt user32 gdi32 ws2_32 winmm dwmapi cfgmgr32 comctl32
    "${TURBOJPEG_DIR}/lib/turbojpeg.lib"
)
# Checks that AVX2 and plain conversion give the same bytes. Takes a minute or two.
add_executable(field_prep_test tests/field_prep_test.cpp src/field_prep.cpp src/field_prep_avx2.cpp "${LZ4_DIR}/lz4.c")
target_compile_features(field_prep_test PRIVATE cxx_std_20)
target_compile_definitions(field_prep_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(field_prep_test PRIVATE src "${LZ4_DIR}")

add_custom_command(TARGET GroovyGrab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${TURBOJPEG_DIR}/bin/turbojpeg.dll"
        "$<TARGET_FILE_DIR:GroovyGrab>")
