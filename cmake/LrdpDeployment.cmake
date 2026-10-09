if(TARGET lrdpd)
  target_sources(lrdpd PRIVATE src/server_options.cpp src/server_preflight.cpp)
  target_compile_definitions(lrdpd PRIVATE LRDP_VERSION="${PROJECT_VERSION}")
  if(BUILD_TESTING)
    add_test(NAME deployment_preflight COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/deployment_preflight.py $<TARGET_FILE:lrdpd>)
    set_tests_properties(deployment_preflight PROPERTIES TIMEOUT 45)
  endif()
endif()
