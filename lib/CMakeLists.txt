cmake_minimum_required(VERSION 3.4.1)
if(WIN32)
  SET(DCMAKE_TOOLCHAIN_FILE "H:/ffmpeg/source/vcpkg/scripts/buildsystems/vcpkg.cmake")
  include("H:/ffmpeg/source/vcpkg/scripts/buildsystems/vcpkg.cmake")
endif()
include_directories( playfair llhttp )

aux_source_directory(. play_src)
set(DIR_SRCS ${play_src})

add_library( airplay
        STATIC
        ${DIR_SRCS}
        )

find_library( LIBPLIST NAMES plist plist-2.0 )

if(WIN32)
target_link_libraries( airplay
          playfair
          llhttp
          ${LIBPLIST} )
else()
target_link_libraries( airplay
        pthread
          playfair
          llhttp
          ${LIBPLIST} )
endif()

if( UNIX OR WIN32 AND NOT APPLE )
  find_package(OpenSSL REQUIRED)
  target_compile_definitions(airplay PUBLIC OPENSSL_API_COMPAT=0x10101000L)
  target_link_libraries( airplay OpenSSL::Crypto )
  if( NOT WIN32)
    target_link_libraries( airplay dns_sd )
  else()
    find_package(pthread REQUIRED)
    target_link_libraries( airplay PThreads4W::PThreads4W )
    target_link_libraries( airplay "${CMAKE_CURRENT_SOURCE_DIR}/Bonjour/BonjourSDK/Lib/Win32/dnssd.lib")
	  target_link_libraries( airplay Ws2_32)
  endif()
else()
  include_directories( /usr/local/opt/openssl@1.1/include/ )
  target_link_libraries( airplay /usr/local/opt/openssl@1.1/lib/libcrypto.a )
endif()
        
