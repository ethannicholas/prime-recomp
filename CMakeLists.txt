cmake_minimum_required(VERSION 3.20)
project(prime C CXX)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Everything that is not specific to Metroid Prime lives in gcn-recomp (a submodule): the
# recompiler, the GameCube runtime, the renderer and the frontends. See its README.
add_subdirectory(gcn-recomp)

gcn_add_game(
  NAME prime
  TITLE "Metroid Prime"
  DISC_ID GM8E01
  DOL_SHA1 39a2f928159ad46491e9b1ef0a72613d91e0cc40
  SYMBOLS ${CMAKE_SOURCE_DIR}/analysis/symbols.txt
  TABLES ${CMAKE_SOURCE_DIR}/recomp
  FPS 60
  SOURCES ${CMAKE_SOURCE_DIR}/src/heap_check.cpp ${CMAKE_SOURCE_DIR}/src/vr_prime.cpp
  # The headset app and its harness, on an Android build: see build-android.ps1 and
  # package-apk.ps1.
  ANDROID_PACKAGE com.example.prime)
