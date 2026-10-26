# Link only: constructing these objects is never executed by this check.
# Catch mismatched headers/libraries in cached dependencies before compiling
# the application. In particular, OpenSSL 3 headers can leak into an Intel
# build that links the bundled OpenSSL 1.1 archives.
include(CMakePushCheckState)
include(CheckCXXSourceCompiles)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES CURL::libcurl LibDataChannel::LibDataChannel
    OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
# Recheck when dependencies have been rebuilt in the same install directory.
unset(ORCA_BUNDLED_TLS_LINKS CACHE)
check_cxx_source_compiles("
    #include <curl/curl.h>
    #include <rtc/peerconnection.hpp>
    int main() {
        auto info = curl_version_info(CURLVERSION_NOW);
        rtc::PeerConnection peer;
        return info ? 0 : 1;
    }
" ORCA_BUNDLED_TLS_LINKS)
cmake_pop_check_state()
if(NOT ORCA_BUNDLED_TLS_LINKS)
    message(FATAL_ERROR
        "The bundled curl, camera transport and OpenSSL libraries do not link together. "
        "Rebuild the macOS dependencies with the selected SDK before building Porca.")
endif()
