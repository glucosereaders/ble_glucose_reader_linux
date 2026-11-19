cmake_minimum_required(VERSION 3.18.4)
project(read_glucose_meter_linux_ble_dbus_cxx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SIGCPP REQUIRED IMPORTED_TARGET sigc++-3.0)

#set(ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
#set(ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
#set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
#set(BUILD_SITE OFF CACHE BOOL "" FORCE)

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

add_executable(read_glucose_meter_linux_ble_dbus_cxx main.cpp)

set_target_properties(read_glucose_meter_linux_ble_dbus_cxx PROPERTIES
        CXX_SCAN_FOR_MODULES OFF
)

target_include_directories(read_glucose_meter_linux_ble_dbus_cxx PRIVATE
        "${dbus_cxx_SOURCE_DIR}/.."
        "${dbus_cxx_SOURCE_DIR}"
        "${dbus_cxx_BINARY_DIR}/.."
        "${dbus_cxx_BINARY_DIR}"
)

target_link_libraries(read_glucose_meter_linux_ble_dbus_cxx PRIVATE dbus-cxx)