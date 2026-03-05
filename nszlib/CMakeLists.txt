if(NOT NS_WITH_ZLIB)
    return()
endif()

find_package(ZLIB REQUIRED)

add_library(nszlib MODULE
    nszlib.c
    ${PROJECT_SOURCE_DIR}/include/nsmodinit.c
)
target_compile_definitions(nszlib PRIVATE NS_MODINIT=NsZlibModInit)
target_include_directories(nszlib
    SYSTEM PRIVATE
        ${CMAKE_BINARY_DIR}/deps/install/include
        ${ZLIB_INCLUDE_DIRS}
)
target_link_libraries(nszlib PRIVATE nsd ZLIB::ZLIB)
set_target_properties(nszlib PROPERTIES PREFIX "" SUFFIX ".so")
add_dependencies(nszlib tcl_ep gperftools_ep)

install(TARGETS nszlib LIBRARY DESTINATION bin)
