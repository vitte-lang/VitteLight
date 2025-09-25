cmake_minimum_required(VERSION 3.16)

project(vittelight C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(GNUInstallDirs)

set(VL_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# Récupération automatique des sources
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS ${VL_ROOT}/core/*.c)

# Exclure certains fichiers utilisés comme main()
list(REMOVE_ITEM CORE_SOURCES ${VL_ROOT}/core/code.c)

add_library(vittelight STATIC
  ${CORE_SOURCES}
)

target_include_directories(vittelight PUBLIC
  ${VL_ROOT}
  ${VL_ROOT}/core
  ${VL_ROOT}/includes
  ${VL_ROOT}/compiler
)

find_package(Threads REQUIRED)
target_link_libraries(vittelight PUBLIC Threads::Threads)

if(NOT WIN32)
  target_link_libraries(vittelight PUBLIC m)
endif()

if(UNIX AND NOT APPLE)
  target_link_libraries(vittelight PUBLIC dl)
endif()

# Exécutables
add_executable(vitlc compiler/vitlc.c)
target_link_libraries(vitlc PRIVATE vittelight)

add_executable(vitte-cli core/code.c)
target_link_libraries(vitte-cli PRIVATE vittelight)

# Installation
install(TARGETS vittelight vitlc vitte-cli
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
# --- pkg-config (.pc) ---
# Génère vittelight.pc depuis le template .in
configure_file(
  ${CMAKE_SOURCE_DIR}/vittelight.pc.in
  ${CMAKE_BINARY_DIR}/vittelight.pc
  @ONLY
)

# Installe le .pc dans lib/pkgconfig
install(
  FILES ${CMAKE_BINARY_DIR}/vittelight.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
# --- Tests ---
enable_testing()

add_executable(vl_test tests/test.c)
target_link_libraries(vl_test PRIVATE vittelight)

add_test(NAME run_vl_test COMMAND vl_test)

