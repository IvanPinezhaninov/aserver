#============================================================================
#
# Copyright (C) 2025 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
#
# This file is part of the aserver which can be found at
# https://github.com/IvanPinezhaninov/aserver/.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
# OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR
# THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
#============================================================================

cmake_minimum_required(VERSION 3.24)

project(aserver LANGUAGES CXX VERSION 1.0.0)

option(ASERVER_BUILD_EXAMPLES "Build examples" OFF)
option(ASERVER_BUILD_TESTS "Build tests" OFF)
option(ASERVER_USE_SYSTEM_BOOST "Use system Boost" ON)
option(ASERVER_USE_SYSTEM_OPENSSL "Use system OpenSSL" ON)

add_subdirectory(3rdparty)

if(ASERVER_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(ASERVER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

find_package(Boost COMPONENTS coroutine REQUIRED)
find_package(Threads REQUIRED)

set(HEADERS
  include/aserver/aserver.hpp
)

add_library(${PROJECT_NAME} INTERFACE
  ${HEADERS}
)

add_library("aserver::aserver" ALIAS ${PROJECT_NAME})

target_compile_definitions(${PROJECT_NAME} INTERFACE
  BOOST_ALLOW_DEPRECATED_HEADERS
  BOOST_ASIO_NO_DEPRECATED
)

if(WIN32)
  target_compile_definitions(${PROJECT_NAME} INTERFACE
    _WIN32_WINNT=0x0603
    BOOST_ALL_NO_LIB
  )
endif()

target_include_directories(${PROJECT_NAME} INTERFACE
  include
)

target_link_libraries(${PROJECT_NAME} INTERFACE
  Boost::boost
  Boost::coroutine
  Threads::Threads
)

set_target_properties(${PROJECT_NAME} PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
