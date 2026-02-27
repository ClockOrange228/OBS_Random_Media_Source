cmake_minimum_required(VERSION 3.28...3.30)
include(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake"
  NO_POLICY_SCOPE
)

project(OBS_Random_Media_Source VERSION 1.0.0)

option(
  ENABLE_FRONTEND_API
  "Use obs-frontend-api for UI functionality"
  ON
)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    PRIVATE OBS::obs-frontend-api
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.cpp)

set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

set_target_properties_plugin(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES OUTPUT_NAME "OBS_Random_Media_Source"
)
