#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "gnuradio::gnuradio-lora_sdr" for configuration "Release"
set_property(TARGET gnuradio::gnuradio-lora_sdr APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(gnuradio::gnuradio-lora_sdr PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libgnuradio-lora_sdr.so.ga8143cb"
  IMPORTED_SONAME_RELEASE "libgnuradio-lora_sdr.so.1.0.0git"
  )

list(APPEND _cmake_import_check_targets gnuradio::gnuradio-lora_sdr )
list(APPEND _cmake_import_check_files_for_gnuradio::gnuradio-lora_sdr "${_IMPORT_PREFIX}/lib/libgnuradio-lora_sdr.so.ga8143cb" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
