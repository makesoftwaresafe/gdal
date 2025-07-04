add_gdal_driver(
  TARGET gdal_MBTiles
  SOURCES mbtilesdataset.cpp
  BUILTIN
  NO_CXX_WFLAGS_EFFCXX
  NO_WFLAG_OLD_STYLE_CAST
)
gdal_standard_includes(gdal_MBTiles)
target_include_directories(gdal_MBTiles PRIVATE ${GDAL_VECTOR_FORMAT_SOURCE_DIR}/mvt)
if (GDAL_USE_ZLIB_INTERNAL)
  gdal_add_vendored_lib(gdal_MBTiles libz)
endif ()
if (GDAL_USE_JSONC_INTERNAL)
  gdal_add_vendored_lib(gdal_MBTiles libjson)
endif ()

if (GDAL_USE_GEOS)
  target_compile_definitions(gdal_MBTiles PRIVATE -DHAVE_GEOS=1)
  gdal_target_link_libraries(gdal_MBTiles PRIVATE ${GEOS_TARGET})
endif ()

target_compile_definitions(gdal_MBTiles PRIVATE -DHAVE_SQLITE)
gdal_target_link_libraries(gdal_MBTiles PRIVATE SQLite::SQLite3)

if (NOT GDAL_USE_ZLIB_INTERNAL)
  gdal_target_link_libraries(gdal_MBTiles PRIVATE ZLIB::ZLIB)
endif ()
if (NOT GDAL_USE_JSONC_INTERNAL)
  gdal_target_link_libraries(gdal_MBTiles PRIVATE ${JSONC_TARGET})
endif ()
target_include_directories(
  gdal_MBTiles PRIVATE ${GDAL_VECTOR_FORMAT_SOURCE_DIR}/gpkg
                       ${GDAL_VECTOR_FORMAT_SOURCE_DIR}/sqlite)
