cmake_minimum_required(VERSION 3.21)
project(tinySA-App VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# auto-detect a Qt installed under C:/Qt if no prefix was given
if(NOT DEFINED CMAKE_PREFIX_PATH)
    file(GLOB _qt_candidates "C:/Qt/6.*/msvc*_64")
    if(_qt_candidates)
        list(SORT _qt_candidates)
        list(GET _qt_candidates -1 CMAKE_PREFIX_PATH)
        message(STATUS "Using Qt at ${CMAKE_PREFIX_PATH}")
    endif()
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets SerialPort PrintSupport Test)
qt_standard_project_setup()

if(MSVC)
    add_compile_options(/utf-8 /W3)
    add_compile_definitions(NOMINMAX _USE_MATH_DEFINES)
endif()

# --- core: device protocol, data model, maths. No QtWidgets. ---
qt_add_library(tinysa_core STATIC
    src/core/units.cpp
    src/core/model.cpp
    src/core/protocol.cpp
    src/core/sweepconfig.cpp
    src/core/genconfig.cpp
    src/core/simdevice.cpp
    src/core/link.cpp
    src/core/linkgen.cpp
    src/core/linktrack.cpp
    src/core/tracking.cpp
    src/core/session.cpp
    src/core/appsettings.cpp
    src/core/traces.cpp
    src/core/csvio.cpp
    src/core/markers.cpp
    src/core/levelunit.cpp
)
target_include_directories(tinysa_core PUBLIC src/core)
target_link_libraries(tinysa_core PUBLIC Qt6::Core Qt6::Gui Qt6::SerialPort)
if(WIN32)
    target_link_libraries(tinysa_core PUBLIC winmm)  # timeBeginPeriod: 1 ms settle times while tracking
endif()

# --- DFU firmware upload: DfuSe protocol + Windows USB transports ---
add_library(tinysa_dfu STATIC
    src/dfu/dfufile.cpp
    src/dfu/dfuse.cpp
    src/dfu/dfuusb.cpp
    src/dfu/firmwareupdater.cpp
)
target_include_directories(tinysa_dfu PUBLIC src/dfu)
set_target_properties(tinysa_dfu PROPERTIES AUTOMOC ON)
target_link_libraries(tinysa_dfu PUBLIC Qt6::Core setupapi winusb)

# --- QCustomPlot 2.1.1 (GPL), vendored unmodified ---
qt_add_library(qcustomplot STATIC vendor/qcustomplot/qcustomplot.cpp)
target_include_directories(qcustomplot PUBLIC vendor/qcustomplot)
target_link_libraries(qcustomplot PUBLIC Qt6::Widgets Qt6::PrintSupport)
if(MSVC)
    target_compile_options(qcustomplot PRIVATE /wd4996)  # deprecated Qt APIs in vendored code
endif()

# --- the application; screens are Qt Designer .ui files in src/ui ---
qt_add_executable(tinySA-App WIN32
    src/main.cpp
    src/ui/autotest.cpp
    src/ui/mainwindow.cpp
    src/ui/mainwindow.ui
    src/ui/mainfiles.cpp
    src/ui/maintest.cpp
    src/ui/spectrumview.cpp
    src/ui/spectrummouse.cpp
    src/ui/spectrummarkers.cpp
    src/ui/markermodel.cpp
    src/ui/mainmarkers.cpp
    src/ui/maintools.cpp
    src/ui/maingenerator.cpp
    src/ui/maintracking.cpp
    src/ui/generatorpanel.cpp
    src/ui/generatorpanel.ui
    src/ui/digitedit.cpp
    src/ui/consolewindow.cpp
    src/ui/consolewindow.ui
    src/ui/batterywindow.cpp
    src/ui/batterywindow.ui
    src/ui/screenview.cpp
    src/ui/screenwindow.cpp
    src/ui/screenwindow.ui
    src/ui/settingsdialog.cpp
    src/ui/settingsdialog.ui
    src/ui/firmwaredialog.cpp
    src/ui/firmwaredialog.ui
    res/tinySA-App.rc
    translations/translations.qrc
)
target_include_directories(tinySA-App PRIVATE src/ui)
set_target_properties(tinySA-App PROPERTIES AUTOUIC_SEARCH_PATHS src/ui AUTORCC ON)
target_link_libraries(tinySA-App PRIVATE tinysa_core tinysa_dfu qcustomplot Qt6::Widgets Qt6::Test)

# version 2.0.0-<commits>-g<hash>[-dirty] like the firmware's git describe, for the About box,
# --version and the exe's version info; build.bat configures again on every build
execute_process(COMMAND git rev-list --count HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE APP_COMMITS OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git describe --always --dirty --exclude "*" WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE APP_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT APP_COMMITS)
    set(APP_COMMITS 0)
    set(APP_HASH unknown)
endif()
set(APP_VERSION "${PROJECT_VERSION}-${APP_COMMITS}-g${APP_HASH}")
message(STATUS "tinySA-App version ${APP_VERSION}")
string(REGEX MATCH "-dirty$" APP_DIRTY "${APP_HASH}")  # build.bat dist: tinySA-App-2.0.0-24[-dirty].zip
file(WRITE ${CMAKE_BINARY_DIR}/version.txt "${PROJECT_VERSION}-${APP_COMMITS}${APP_DIRTY}")
target_compile_definitions(tinySA-App PRIVATE APP_VERSION="${APP_VERSION}"
    APP_VERSION_NUMBERS=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},${APP_COMMITS})

# --- unit tests for core ---
enable_testing()
qt_add_executable(tst_core test/tst_core.cpp)
target_link_libraries(tst_core PRIVATE tinysa_core tinysa_dfu Qt6::Test)
add_test(NAME tst_core COMMAND tst_core)
