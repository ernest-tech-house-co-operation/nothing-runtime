# SetupNlohmannJson.cmake
# Vendored single-header at third_party/nlohmann/json.hpp

set(NLOHMANN_JSON_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third_party")
if(NOT EXISTS "${NLOHMANN_JSON_INCLUDE_DIRS}/nlohmann/json.hpp")
    message(FATAL_ERROR
        "nlohmann/json.hpp is missing. Expected at third_party/nlohmann/json.hpp.\n"
        "Download with:\n"
        "  curl -fsSL -o third_party/nlohmann/json.hpp \\\n"
        "    https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp")
endif()
message(STATUS "nlohmann/json: vendored single-header")
