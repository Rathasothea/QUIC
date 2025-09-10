cmake_minimum_required(VERSION 3.16)
project(NewMSQuicProject VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Platform-specific libraries
if(WIN32)
    add_definitions(-DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00)
    set(PLATFORM_LIBS ws2_32 mswsock)
else()
    set(PLATFORM_LIBS pthread dl)
endif()

# Find OpenSSL - CRITICAL for QUIC TLS support
find_package(OpenSSL REQUIRED)
if(OpenSSL_FOUND)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
    message(STATUS "OpenSSL libraries: ${OPENSSL_LIBRARIES}")
    message(STATUS "OpenSSL include: ${OPENSSL_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "OpenSSL not found! Install with: sudo apt install libssl-dev")
endif()

# Find MsQuic
if(WIN32)
    find_path(MSQUIC_INCLUDE_DIR 
        NAMES msquic.h
        PATHS
            "C:/Program Files/Microsoft/msquic/include"
            "C:/msquic/src/inc"
            "${CMAKE_SOURCE_DIR}/third_party/msquic/src/inc"
            ENV MSQUIC_INCLUDE_DIR
    )
    
    find_library(MSQUIC_LIBRARY
        NAMES msquic
        PATHS
            "C:/Program Files/Microsoft/msquic/lib"
            "C:/msquic/artifacts/bin/windows/x64_Release_openssl"
            "${CMAKE_SOURCE_DIR}/third_party/msquic/artifacts/bin/windows/x64_Release_openssl"
            ENV MSQUIC_LIB_DIR
    )
else()
    find_path(MSQUIC_INCLUDE_DIR
        NAMES msquic.h
        PATHS
            /usr/include
            /usr/local/include
            /opt/msquic/include
            "${CMAKE_SOURCE_DIR}/third_party/msquic/src/inc"
            ENV MSQUIC_INCLUDE_DIR
    )
    
    find_library(MSQUIC_LIBRARY
        NAMES msquic
        PATHS
            /usr/lib
            /usr/local/lib
            /usr/lib/x86_64-linux-gnu
            /opt/msquic/lib
            "${CMAKE_SOURCE_DIR}/third_party/msquic/artifacts/bin/linux/x64_Release_openssl"
            ENV MSQUIC_LIB_DIR
    )
endif()

if(NOT MSQUIC_INCLUDE_DIR)
    message(FATAL_ERROR "MsQuic headers not found! Please install MsQuic or set MSQUIC_INCLUDE_DIR")
endif()

if(NOT MSQUIC_LIBRARY)
    message(WARNING "MsQuic library not found in common paths. Will try to link with -lmsquic")
    set(MSQUIC_LIBRARY msquic)
endif()

message(STATUS "MsQuic include directory: ${MSQUIC_INCLUDE_DIR}")
message(STATUS "MsQuic library: ${MSQUIC_LIBRARY}")

# Include directories
include_directories(${MSQUIC_INCLUDE_DIR})
include_directories(${OPENSSL_INCLUDE_DIR})

# Server executable
add_executable(quic_server
    src/server.c
)

target_link_libraries(quic_server 
    ${MSQUIC_LIBRARY}
    OpenSSL::SSL
    OpenSSL::Crypto
    ${PLATFORM_LIBS}
)

# Client executable
add_executable(quic_client
    src/client.c
)

target_link_libraries(quic_client
    ${MSQUIC_LIBRARY}
    OpenSSL::SSL
    OpenSSL::Crypto
    ${PLATFORM_LIBS}
)

# Compiler options
if(MSVC)
    target_compile_options(quic_server PRIVATE /W4 /WX-)
    target_compile_options(quic_client PRIVATE /W4 /WX-)
else()
    target_compile_options(quic_server PRIVATE -Wall -Wextra -O2)
    target_compile_options(quic_client PRIVATE -Wall -Wextra -O2)
    
    # Add runtime library path for Linux
    set_target_properties(quic_server PROPERTIES
        INSTALL_RPATH_USE_LINK_PATH TRUE
        BUILD_WITH_INSTALL_RPATH TRUE
    )
    set_target_properties(quic_client PROPERTIES
        INSTALL_RPATH_USE_LINK_PATH TRUE
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()

# Debug configuration
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(quic_server PRIVATE DEBUG=1)
    target_compile_definitions(quic_client PRIVATE DEBUG=1)
    if(NOT MSVC)
        target_compile_options(quic_server PRIVATE -g)
        target_compile_options(quic_client PRIVATE -g)
    endif()
endif()

# Installation
install(TARGETS quic_server quic_client
    RUNTIME DESTINATION bin
)

# Custom targets
add_custom_target(run-server
    COMMAND quic_server
    DEPENDS quic_server
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    COMMENT "Running QUIC server"
)

add_custom_target(run-client
    COMMAND quic_client
    DEPENDS quic_client
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    COMMENT "Running QUIC client"
)

# Status messages
message(STATUS "")
message(STATUS "=== Build Configuration ===")
message(STATUS "Project: ${PROJECT_NAME}")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "")
message(STATUS "=== Dependencies ===")
message(STATUS "OpenSSL: ${OPENSSL_VERSION} (${OPENSSL_LIBRARIES})")
message(STATUS "MsQuic: ${MSQUIC_LIBRARY}")
message(STATUS "")
message(STATUS "=== Build Targets ===")
message(STATUS "  make                 - Build all executables")
message(STATUS "  make quic_server     - Build server only")
message(STATUS "  make quic_client     - Build client only")
message(STATUS "  make run-server      - Build and run server")
message(STATUS "  make run-client      - Build and run client")
message(STATUS "")
message(STATUS "Executables will be in: ${CMAKE_BINARY_DIR}/bin/")