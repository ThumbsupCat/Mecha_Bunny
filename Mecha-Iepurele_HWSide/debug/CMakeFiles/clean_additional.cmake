# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "debug")
  file(REMOVE_RECURSE
  "Mecha-Iepurele_HWSide.bin"
  "clean_files-NOTFOUND"
  )
endif()
