cmake_minimum_required(VERSION 3.22)
project(ble_glucose_reader_linux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)

set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH FALSE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SIGCPP REQUIRED IMPORTED_TARGET sigc++-3.0)

set(ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SITE OFF CACHE BOOL "" FORCE)

set(DBUS_CXX_ORG "dbus-cxx")
set(DBUS_CXX_REPO "dbus-cxx.git")
set(DBUS_CXX_URL "https://github.com/${DBUS_CXX_ORG}/${DBUS_CXX_REPO}")

include(FetchContent)
FetchContent_Declare(
        dbus_cxx
        GIT_REPOSITORY "${DBUS_CXX_URL}"
        GIT_TAG        2.6.2
        GIT_SHALLOW    YES
)
FetchContent_MakeAvailable(dbus_cxx)

FetchContent_Declare(
        argh
        GIT_REPOSITORY https://github.com/adishavit/argh.git
        GIT_TAG        master
)

# 3. Disable argh's own tests and examples so they don't clutter your build
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# 4. Download and make the 'argh' target available
FetchContent_MakeAvailable(argh)

add_executable(read_glucose_meter_ble main.cpp)

#set_target_properties(read_glucose_meter_ble PROPERTIES
#        CXX_SCAN_FOR_MODULES OFF
#)

target_include_directories(read_glucose_meter_ble PRIVATE
#        "${dbus_cxx_SOURCE_DIR}/.."
        "${dbus_cxx_SOURCE_DIR}"
#        "${dbus_cxx_BINARY_DIR}/.."
        "${dbus_cxx_BINARY_DIR}"
)

target_include_directories(read_glucose_meter_ble PRIVATE "${argh_INCLUDE_DIR}")
target_link_libraries(read_glucose_meter_ble PRIVATE dbus-cxx argh)
target_link_libraries(read_glucose_meter_ble PRIVATE argh)

install(TARGETS read_glucose_meter_ble RUNTIME DESTINATION .)
install(TARGETS dbus-cxx LIBRARY DESTINATION .)
