# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Single source of truth for the running firmware's version -- read at
# runtime via esp_app_get_description(), compared against versioninfo.json
# by modules/coredy_ota.c. Bump this (and re-run ota/make_release.sh) to
# cut a release; nothing else needs to change.
set(PROJECT_VER "1.3.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(coredy_bridge)
