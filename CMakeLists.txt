cmake_minimum_required(VERSION 2.8.4)
project(mytest)

add_compile_options( -std=c++11
        -Wall -Wextra -Wno-unused-parameter -Wno-implicit-fallthrough
        -m64 -g -Wno-unknown-warning-option -fPIC
        -DHAVE_UINT16_T -DHAVE_UINT32_T -DHAVE_UINT64_T -DHAVE_UINT8_T
        -DHAVE_STDINT_H -DHAVE_INTTYPES_H -DHAVE_NETINET_IN_H -DCPU_CISC
        -DMS_LITTLE_ENDIAN -D_POSIX_C_SOURCE=200112 -D_GNU_SOURCE -D_LARGEFILE_SOURCE
        -D_FILE_OFFSET_BITS=64 -DHAVE_INT16_T -DHAVE_INT32_T -DHAVE_INT8_T)


file(GLOB SOURCE ./src/*.cpp ./src/*/*.cpp ./src/*/*/*.cpp)

list(REMOVE_ITEM SOURCE main.cpp)

include_directories(include)
include_directories(devps/libuv/include)
include_directories(devps/libsrtp/srtp/include)
include_directories(devps/openssl/openssl/include)
include_directories(devps/usrsctp/usrsctp/usrsctplib)
include_directories(devps/usrsctp/usrsctp/usrsctplib/netinet)
include_directories(devps/netstring/netstring-c)
link_directories(lib)
add_library(mediasoup ${SOURCE})

add_executable(my_test src/main.cpp )


target_link_libraries(my_test mediasoup netstring srtp openssl usrsctp uv pthread dl)