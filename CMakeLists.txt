cmake_minimum_required(VERSION 3.25)
project(Jieying VERSION 0.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

option(JIEYING_BUILD_TESTS "Build the headless UI tests" ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Network Svg)

if(MSVC)
    add_compile_options(/utf-8)
    set(JIEYING_WARNINGS /W4) # our code only; third-party sources keep their own level
endif()

# Everything but main(), so tests can link the same code.
qt_add_library(jieying_core STATIC
    src/annotations.cpp src/annotations.h
    src/app.cpp src/app.h
    src/audiocapture.cpp src/audiocapture.h
    src/autostart.cpp src/autostart.h
    src/autoscroller.cpp src/autoscroller.h
    src/beautify.cpp src/beautify.h
    src/beautifydialog.cpp src/beautifydialog.h
    src/capturehistory.cpp src/capturehistory.h
    src/capturesession.cpp src/capturesession.h
    src/capturetoolbar.cpp src/capturetoolbar.h
    src/collagedialog.cpp src/collagedialog.h
    src/config.cpp src/config.h
    src/frameencoder.h src/gifencoder.cpp src/mp4encoder.cpp
    src/globalhotkeys.cpp src/globalhotkeys.h
    src/historydialog.cpp src/historydialog.h
    src/icons.h
    src/hotkeymanager.cpp src/hotkeymanager.h
    src/imagecollage.cpp src/imagecollage.h
    src/imageio.cpp src/imageio.h
    src/inputoverlay.cpp src/inputoverlay.h
    src/livepindialog.cpp src/livepindialog.h
    src/ocr.cpp src/ocr.h
    src/ocrwindow.cpp src/ocrwindow.h
    src/pinboard.cpp src/pinboard.h
    src/pinwindow.cpp src/pinwindow.h
    src/recorder.cpp src/recorder.h
    src/regionframe.cpp src/regionframe.h
    src/screengrab.cpp src/screengrab.h
    src/scrollcapture.cpp src/scrollcapture.h
    src/scrollstitcher.cpp src/scrollstitcher.h
    src/wgcgrabber.cpp src/wgcgrabber.h
    src/selectionoverlay.cpp src/selectionoverlay.h
    src/selfcheck.cpp src/selfcheck.h
    src/sensitive.cpp src/sensitive.h
    src/settingsdialog.cpp src/settingsdialog.h
    src/tableocr.cpp src/tableocr.h
    src/tableocrdialog.cpp src/tableocrdialog.h
    src/translator.cpp src/translator.h
    src/windowfinder.cpp src/windowfinder.h
)
target_include_directories(jieying_core PUBLIC src)
# windowsapp: WinRT (Windows.Media.Ocr) used through the SDK's C++/WinRT headers.
target_link_libraries(jieying_core PUBLIC Qt6::Widgets Qt6::Concurrent Qt6::Network Qt6::Svg crypt32 dwmapi windowsapp
                                        mfplat mfreadwrite mfuuid propsys d3d11 dxgi)
target_compile_definitions(jieying_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(jieying_core PRIVATE ${JIEYING_WARNINGS})

# QR code / barcode reading: ZXing-cpp from tools/fetch_deps.ps1, built as a static library.
set(JIEYING_ZXING_DIR "${CMAKE_SOURCE_DIR}/third_party/zxing-cpp")
if(EXISTS "${JIEYING_ZXING_DIR}/CMakeLists.txt")
    set(ZXING_READERS ON CACHE BOOL "" FORCE)
    set(ZXING_WRITERS NEW CACHE STRING "" FORCE) # tests generate codes to read back
    set(ZXING_C_API OFF CACHE BOOL "" FORCE)
    set(ZXING_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory("${JIEYING_ZXING_DIR}" zxing EXCLUDE_FROM_ALL)
    set_target_properties(ZXing PROPERTIES AUTOMOC OFF) # its optional ZXingQt.h is not ours to moc
    target_sources(jieying_core PRIVATE src/barcodes.cpp src/barcodes.h)
    target_link_libraries(jieying_core PUBLIC ZXing::ZXing)
    target_compile_definitions(jieying_core PUBLIC JIEYING_HAVE_ZXING)
    message(STATUS "QR codes (ZXing-cpp): enabled")
else()
    message(STATUS "QR codes (ZXing-cpp): disabled (run tools/fetch_deps.ps1 to enable)")
endif()
# App icon (assets/jieying.ico, from tools/make_icon.py) for the tray and windows.
qt_add_resources(jieying_core "appicon" PREFIX "/" BASE assets FILES assets/jieying.ico)
file(GLOB JIEYING_TOOL_ICONS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/icons/*.svg")
qt_add_resources(jieying_core "toolicons" PREFIX "/icons" BASE assets/icons FILES ${JIEYING_TOOL_ICONS})

# PaddleOCR engine: enabled when tools/fetch_deps.ps1 has fetched ONNX Runtime and the models.
set(JIEYING_ORT_DIR "${CMAKE_SOURCE_DIR}/third_party/onnxruntime")
set(JIEYING_OCR_MODELS "${CMAKE_SOURCE_DIR}/third_party/ocr-models")
if(EXISTS "${JIEYING_ORT_DIR}/lib/onnxruntime.lib" AND EXISTS "${JIEYING_OCR_MODELS}/rec.onnx")
    target_sources(jieying_core PRIVATE src/paddleocr.cpp src/paddleocr.h)
    target_compile_definitions(jieying_core PUBLIC JIEYING_HAVE_PADDLEOCR)
    target_include_directories(jieying_core PRIVATE "${JIEYING_ORT_DIR}/include")
    # Delay-loaded: the app still starts (falling back to Windows OCR) if the DLL is missing.
    target_link_libraries(jieying_core PUBLIC "${JIEYING_ORT_DIR}/lib/onnxruntime.lib" delayimp)
    target_link_options(jieying_core PUBLIC "/DELAYLOAD:onnxruntime.dll")
    add_custom_target(jieying_ocr_runtime ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/ocr-models"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${JIEYING_ORT_DIR}/lib/onnxruntime.dll" "${CMAKE_BINARY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${JIEYING_OCR_MODELS}/det.onnx" "${JIEYING_OCR_MODELS}/rec.onnx"
                "${JIEYING_OCR_MODELS}/dict.txt" "${CMAKE_BINARY_DIR}/ocr-models"
        COMMENT "Copying ONNX Runtime and PaddleOCR models")
    message(STATUS "PaddleOCR: enabled")
else()
    message(STATUS "PaddleOCR: disabled (run tools/fetch_deps.ps1 to enable)")
endif()

# Exe icon and version info (Explorer properties, installer).
configure_file(src/jieying.rc.in "${CMAKE_BINARY_DIR}/jieying.rc" @ONLY)
qt_add_executable(jieying WIN32 src/main.cpp "${CMAKE_BINARY_DIR}/jieying.rc")
target_link_libraries(jieying PRIVATE jieying_core Qt6::Network)
target_compile_options(jieying PRIVATE ${JIEYING_WARNINGS})
target_compile_definitions(jieying PRIVATE JIEYING_VERSION="${PROJECT_VERSION}")

# Copy the Qt runtime next to the exe so it starts without Qt on PATH.
get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
add_custom_command(TARGET jieying POST_BUILD
    COMMAND "${_qt_bin}/windeployqt.exe" --translations zh_CN --no-compiler-runtime --no-opengl-sw
            --no-system-d3d-compiler --no-system-dxc-compiler --verbose 0 "$<TARGET_FILE:jieying>"
    VERBATIM)

if(JIEYING_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    # Runs on Qt's offscreen platform: never touches the real desktop, mouse or keyboard.
    # ocr_eval measures OCR accuracy on demand; run_tests.bat does not run it.
    foreach(_test overlay_test ocr_test m4_test settings_test historydialog_test collage_test tableocr_test livepin_test ocr_eval)
        qt_add_executable(${_test} tests/${_test}.cpp)
        target_link_libraries(${_test} PRIVATE jieying_core Qt6::Test)
        target_compile_options(${_test} PRIVATE ${JIEYING_WARNINGS})
        # windeployqt only copies the windows platform plugin; offscreen comes from the Qt install.
        target_compile_definitions(${_test} PRIVATE JIEYING_QT_PLUGIN_DIR="${_qt_bin}/../plugins")
    endforeach()
endif()
