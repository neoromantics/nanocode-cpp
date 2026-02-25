cmake_minimum_required(VERSION 3.20)
project(nanocode-cpp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Find OpenSSL for HTTPS
find_package(OpenSSL REQUIRED)

# Find Boost
find_package(Boost CONFIG REQUIRED COMPONENTS json)

include(FetchContent)
FetchContent_Declare(
  replxx
  GIT_REPOSITORY https://github.com/AmokHuginnsson/replxx.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(replxx)

# Add our source files
file(GLOB SOURCES "src/*.cpp")

add_executable(nanocode ${SOURCES})

target_include_directories(nanocode PRIVATE src)

# Link libraries
target_link_libraries(nanocode
    PRIVATE
    Boost::json
    OpenSSL::SSL
    OpenSSL::Crypto
    replxx
)

# Optional: macOS CoreFoundation / Security frameworks for Boost.Asio SSL sometimes needed,
# though OpenSSL is usually sufficient.
if(APPLE)
    find_library(COREFoundation CoreFoundation)
    find_library(SECURITY Security)
    target_link_libraries(nanocode PRIVATE ${COREFoundation} ${SECURITY})
endif()
