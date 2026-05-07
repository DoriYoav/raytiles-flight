cmake_minimum_required(VERSION 4.2)
project(raytiles)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include")
list(APPEND CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include")

include(FetchContent)

if (NOT TARGET raylib)
    FetchContent_Declare(
            raylib
            GIT_REPOSITORY https://github.com/raysan5/raylib.git
            GIT_TAG 5.5
            GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(raylib)
endif ()

if (NOT TARGET httplib)
    FetchContent_Declare(
            httplib
            GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
            GIT_TAG v0.18.5
            GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(httplib)
endif ()

add_library(${PROJECT_NAME} STATIC
        raytiles.h
        src/raytiles.cpp
        src/downloader.hpp
        src/tilekey.hpp
)

target_include_directories(${PROJECT_NAME} PUBLIC include)
target_link_libraries(${PROJECT_NAME} PUBLIC raylib)
target_link_libraries(${PROJECT_NAME} PUBLIC httplib) # make private

if(APPLE)
    find_library(SECURITY_FRAMEWORK Security)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${SECURITY_FRAMEWORK})
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

add_executable(Sandbox sandbox/main.cpp)
target_link_libraries(Sandbox PRIVATE ${PROJECT_NAME})
