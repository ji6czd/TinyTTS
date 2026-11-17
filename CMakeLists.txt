cmake_minimum_required(VERSION 3.15)
project(TinyTTS C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(tinytts src/tts.c)

if(MSVC)
  target_compile_options(tinytts PRIVATE /W3)
  target_compile_definitions(tinytts PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(tinytts PRIVATE -Wall -Wextra)
  if(NOT WIN32)
    target_link_libraries(tinytts PRIVATE m)
  endif()
endif()

if(WIN32)
  target_link_libraries(tinytts PRIVATE shell32)
endif()

install(TARGETS tinytts RUNTIME DESTINATION bin)
