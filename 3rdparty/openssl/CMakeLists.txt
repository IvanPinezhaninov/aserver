cmake_minimum_required(VERSION 3.5)

if(WIN32)
  set(CMAKE_REQUIRED_LIBRARIES Crypt32)
  find_library(CRYPT32_LIBRARY Crypt32)
  if(NOT CRYPT32_LIBRARY)
    message(FATAL_ERROR "Crypt32.lib not found!")
  endif()
endif()

include(FetchContent)

set(OPENSSL_VERSION 3.4.0)
set(CMAKE_CXX_STANDARD 17)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
  OpenSSL
  URL https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON
)

FetchContent_GetProperties(OpenSSL)

if(NOT openssl_POPULATED)
  FetchContent_Populate(OpenSSL)
endif()

file(MAKE_DIRECTORY "${openssl_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${openssl_BINARY_DIR}/lib")
file(TOUCH "${openssl_BINARY_DIR}/lib/libcrypto${CMAKE_STATIC_LIBRARY_SUFFIX}")
file(TOUCH "${openssl_BINARY_DIR}/lib/libssl${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(OPENSSL_ROOT_DIR ${openssl_BINARY_DIR} CACHE INTERNAL "OpenSSL root directory")
set(OPENSSL_INCLUDE_DIR "${openssl_BINARY_DIR}/include" CACHE INTERNAL "OpenSSL include directory")

set(OPENSSL_CONFIGURE_OPTIONS
  no-apps
  no-docs
  no-pinshared
  no-shared
  -static
  --prefix="${openssl_BINARY_DIR}"
  --openssldir="${openssl_BINARY_DIR}"
  --libdir=lib
)

set(OPENSSL_MAKEFILE "${openssl_SOURCE_DIR}/Makefile")

add_custom_command(
  OUTPUT ${OPENSSL_MAKEFILE}
  COMMAND perl ./Configure ${OPENSSL_CONFIGURE_OPTIONS}
  WORKING_DIRECTORY "${openssl_SOURCE_DIR}"
  COMMENT "OpenSSL configure"
)

set(CONFIG_TARGET openssl_config)
add_custom_target(${CONFIG_TARGET} DEPENDS ${OPENSSL_MAKEFILE})

set(OPENSSL_LIBRARIES
  "${openssl_SOURCE_DIR}/libssl${CMAKE_STATIC_LIBRARY_SUFFIX}"
  "${openssl_SOURCE_DIR}/libcrypto${CMAKE_STATIC_LIBRARY_SUFFIX}"
)

if(DEFINED ENV{MAKE})
  set(BUILD_TOOL $ENV{MAKE})
elseif(WIN32)
  set(BUILD_TOOL nmake)
else()
  set(BUILD_TOOL make)
endif()

add_custom_command(
  OUTPUT ${OPENSSL_LIBRARIES}
  COMMAND ${BUILD_TOOL}
  WORKING_DIRECTORY "${openssl_SOURCE_DIR}"
  DEPENDS ${CONFIG_TARGET}
  COMMENT "OpenSSL build"
)

set(BUILD_TARGET openssl_build)
add_custom_target(${BUILD_TARGET} DEPENDS ${OPENSSL_LIBRARIES})

set(OPENSSL_INCLUDES
  "${openssl_BINARY_DIR}/include/openssl/conf.h"
  "${openssl_BINARY_DIR}/include/openssl/ssl.h"
)

add_custom_command(
  OUTPUT ${OPENSSL_INCLUDES}
  COMMAND ${BUILD_TOOL} install
  WORKING_DIRECTORY "${openssl_SOURCE_DIR}"
  DEPENDS ${BUILD_TARGET}
  COMMENT "OpenSSL install"
)

set(INSTALL_TARGET openssl_install)
add_custom_target(${INSTALL_TARGET} DEPENDS ${OPENSSL_INCLUDES})

# CRYPTO
set(CRYPTO_TARGET openssl_crypto)

add_library(${CRYPTO_TARGET} STATIC IMPORTED GLOBAL)
add_library(OpenSSL::Crypto ALIAS ${CRYPTO_TARGET})

add_dependencies(${CRYPTO_TARGET} ${INSTALL_TARGET})

set_target_properties(${CRYPTO_TARGET} PROPERTIES
  IMPORTED_LOCATION "${openssl_BINARY_DIR}/lib/libcrypto${CMAKE_STATIC_LIBRARY_SUFFIX}"
  INTERFACE_INCLUDE_DIRECTORIES "${openssl_BINARY_DIR}/include"
)

target_link_libraries(${CRYPTO_TARGET} INTERFACE
  ${CMAKE_DL_LIBS}
)

if(WIN32)
  target_link_libraries(${CRYPTO_TARGET} INTERFACE
    ${CRYPT32_LIBRARY}
  )
endif()

# SSL
set(SSL_TARGET openssl_ssl)

add_library(${SSL_TARGET} STATIC IMPORTED GLOBAL)
add_library(OpenSSL::SSL ALIAS ${SSL_TARGET})

add_dependencies(${SSL_TARGET} ${CRYPTO_TARGET})

set_target_properties(${SSL_TARGET} PROPERTIES
  IMPORTED_LOCATION "${openssl_BINARY_DIR}/lib/libssl${CMAKE_STATIC_LIBRARY_SUFFIX}"
  INTERFACE_INCLUDE_DIRECTORIES "${openssl_BINARY_DIR}/include"
)

target_link_libraries(${SSL_TARGET} INTERFACE
  ${CRYPTO_TARGET}
)
