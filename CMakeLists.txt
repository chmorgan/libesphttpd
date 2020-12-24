cmake_minimum_required(VERSION 3.5)

if(ESP_PLATFORM)
  include("${COMPONENT_DIR}/cmake/idf.cmake")
else()
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/standalone.cmake")
endif()