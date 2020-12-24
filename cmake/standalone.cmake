include("${CMAKE_SOURCE_DIR}/cmake/vars.cmake")

add_library(libesphttpd STATIC ${libesphttpd_SRCS})
target_include_directories(libesphttpd PUBLIC ${libesphttpd_INCLUDE_DIRS})
target_include_directories(libesphttpd PRIVATE ${libesphttpd_PRIV_INCLUDE_DIRS})
#if(CONFIG_ESPHTTPD_USE_ESPFS)
#  add_dependencies(libesphttpd libespfs)
#endif()

get_cmake_property(CACHE_VARS CACHE_VARIABLES)
foreach(CACHE_VAR ${CACHE_VARS})
  if (CACHE_VAR MATCHES "^CONFIG_")
    target_compile_definitions(libesphttpd PRIVATE "-D${CACHE_VAR}=${${CACHE_VAR}}")
  endif()
endforeach()

add_subdirectory(libespfs)
