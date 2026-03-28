add_library(nsssl MODULE
    nsssl.c
    ${PROJECT_SOURCE_DIR}/include/nsmodinit.c
)
target_compile_definitions(nsssl PRIVATE NS_MODINIT=NsSSL_ModInit)
target_include_directories(nsssl
    SYSTEM PRIVATE
        ${DEPS_INSTALL_DIR}/include
)
target_link_libraries(nsssl PRIVATE nsd OpenSSL::SSL OpenSSL::Crypto)
if(UNIX AND NOT APPLE)
    target_link_libraries(nsssl PRIVATE ${CMAKE_DL_LIBS})
endif()
set_target_properties(nsssl PROPERTIES PREFIX "" SUFFIX ".so")
add_dependencies(nsssl tcl_ep gperftools_ep)
if(NOT NS_USE_SYSTEM_OPENSSL)
    add_dependencies(nsssl openssl_ep)
endif()

install(TARGETS nsssl LIBRARY DESTINATION bin)
