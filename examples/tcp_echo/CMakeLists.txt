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

cmake_minimum_required(VERSION 3.5)

project(tcp_echo LANGUAGES CXX VERSION 1.0.0)

find_package(Boost COMPONENTS log REQUIRED)

set(SOURCES
  src/main.cpp
)

add_executable(${PROJECT_NAME}
  ${SOURCES}
)

set_target_properties(${PROJECT_NAME} PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

target_link_libraries(${PROJECT_NAME} PRIVATE
  aserver::aserver
)
