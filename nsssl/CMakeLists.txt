add_library(nsssl MODULE
    nsssl.c
    ${PROJECT_SOURCE_DIR}/include/nsmodinit.c
)
target_compile_definitions(nsssl PRIVATE NS_MODINIT=NsSSL_ModInit)
target_include_directories(nsssl
    SYSTEM PRIVATE
        ${CMAKE_BINARY_DIR}/deps/install/include
)
target_link_libraries(nsssl PRIVATE nsd OpenSSL::SSL OpenSSL::Crypto)
set_target_properties(nsssl PROPERTIES PREFIX "" SUFFIX ".so")
add_dependencies(nsssl tcl_ep gperftools_ep openssl_ep)

install(TARGETS nsssl LIBRARY DESTINATION bin)
