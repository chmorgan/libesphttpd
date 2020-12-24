include(${COMPONENT_DIR}/cmake/vars.cmake)

idf_component_register(
  SRCS ${libesphttpd_SRCS}
  INCLUDE_DIRS ${libesphttpd_INCLUDE_DIRS}
  PRIV_INCLUDE_DIRS ${libesphttpd_PRIV_INCLUDE_DIRS}
  REQUIRES ${libesphttpd_REQUIRES}
  PRIV_REQUIRES ${libesphttpd_PRIV_REQUIRES}
)
