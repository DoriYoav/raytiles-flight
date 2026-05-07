cmake_minimum_required(VERSION 4.2)
project(raytiles)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

if (NOT TARGET raylib)
    # set(SUPPORT_FILEFORMAT_JPG ON CACHE BOOL "" FORCE)
    add_compile_definitions(SUPPORT_FILEFORMAT_JPG)
    FetchContent_Declare(
            raylib
            GIT_REPOSITORY https://github.com/raysan5/raylib.git
            GIT_TAG 6.0
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

add_library(${PROJECT_NAME}
        raytiles.h
        src/raytiles.cpp
        src/downloader.hpp
        src/tilekey.hpp
)

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${PROJECT_NAME} PUBLIC raylib)
target_link_libraries(${PROJECT_NAME} PRIVATE httplib)

if (APPLE)
    find_library(SECURITY_FRAMEWORK Security)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${SECURITY_FRAMEWORK})
endif ()

find_package(OpenSSL REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(${PROJECT_NAME} PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

# sandbox example application
add_executable(Sandbox sandbox/main.cpp)
target_link_libraries(Sandbox PRIVATE ${PROJECT_NAME})
