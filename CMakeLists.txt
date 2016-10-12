cmake_minimum_required(VERSION 2.8.9)

project(raplcap)
set(PROJECT_VERSION 0.1.0)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")

include_directories(${PROJECT_SOURCE_DIR}/inc)


# Install

install(FILES inc/raplcap.h DESTINATION "${CMAKE_INSTALL_PREFIX}/include")
install(DIRECTORY ${CMAKE_BINARY_DIR}/pkgconfig/ DESTINATION lib/pkgconfig)

# Uninstall

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
  @ONLY
)

add_custom_target(uninstall COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)

# Subdirectories

find_package(libmsr)
if(libmsr_FOUND)
  add_subdirectory(msr)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(POWERCAP powercap)
if(POWERCAP_FOUND)
  add_subdirectory(sysfs)
endif()

add_subdirectory(rapl-configure)