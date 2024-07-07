#============================================================================
#
# Copyright (C) 2024 Ivan Pinezhaninov <ivan.pinezhaninov@gmail.com>
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

include(FetchContent)

set(BOOST_VERSION 1.86.0)
set(BOOST_INCLUDE_LIBRARIES asio coroutine signals2 system)
set(CMAKE_CXX_STANDARD 17)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
  Boost
  URL https://github.com/boostorg/boost/releases/download/boost-${BOOST_VERSION}/boost-${BOOST_VERSION}-cmake.tar.xz
  USES_TERMINAL_DOWNLOAD ON
  DOWNLOAD_EXTRACT_TIMESTAMP ON
  OVERRIDE_FIND_PACKAGE
)

FetchContent_MakeAvailable(Boost)

set(TARGET BOOST_TARGET)

add_library(${TARGET} INTERFACE)

target_link_libraries(${TARGET} INTERFACE
  Boost::asio
  Boost::headers
  Boost::signals2
  Boost::system
)

add_library(Boost::boost ALIAS ${TARGET})
