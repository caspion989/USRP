cmake_minimum_required(VERSION 3.12)
project(usrp_to_grc CXX)

set(CMAKE_CXX_STANDARD 17)

# UHD -- hardcoded paths because this install is missing UHDConfig.cmake
set(UHD_ROOT "C:/Program Files/UHD")
add_library(UHD::uhd STATIC IMPORTED)
set_target_properties(UHD::uhd PROPERTIES
    IMPORTED_LOCATION "${UHD_ROOT}/lib/uhd.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${UHD_ROOT}/include;C:/Users/2406h"
)

set(BOOST_LIB_DIR "C:/Users/2406h/radioconda/pkgs/libboost-1.86.0-hb0986bb_3/Library/lib")
add_executable(usrp_to_grc main.cpp)
target_link_libraries(usrp_to_grc PUBLIC UHD::uhd Ws2_32)
target_link_directories(usrp_to_grc PUBLIC "${BOOST_LIB_DIR}")
