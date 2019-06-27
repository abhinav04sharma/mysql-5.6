set(TBB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tbb)


include(${TBB_DIR}/cmake/TBBBuild.cmake)

set(tbb_build_MAKE_ARGS extra_inc=big_iron.inc)
tbb_build(TBB_ROOT ${TBB_DIR} CONFIG_DIR TBB_DIR MAKE_ARGS extra_inc=big_iron.inc)

include(ExternalProject)
ExternalProject_Add(libcityhash
        PREFIX ${CMAKE_CURRENT_BINARY_DIR}/cityhash
        CONFIGURE_COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/cityhash/configure --prefix=${CMAKE_CURRENT_BINARY_DIR}/cityhash
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cityhash/src
        BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/cityhash
        BUILD_COMMAND ${MAKE}
        INSTALL_DIR ${CMAKE_CURRENT_BINARY_DIR}/cityhash)