# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/rct4/esp/esp-idf/components/bootloader/subproject"
  "/home/rct4/esp/projects/wifi_tester/build/bootloader"
  "/home/rct4/esp/projects/wifi_tester/build/bootloader-prefix"
  "/home/rct4/esp/projects/wifi_tester/build/bootloader-prefix/tmp"
  "/home/rct4/esp/projects/wifi_tester/build/bootloader-prefix/src/bootloader-stamp"
  "/home/rct4/esp/projects/wifi_tester/build/bootloader-prefix/src"
  "/home/rct4/esp/projects/wifi_tester/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/rct4/esp/projects/wifi_tester/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/rct4/esp/projects/wifi_tester/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
