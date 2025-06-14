add_gdal_driver(
  TARGET ogr_VFK
  SOURCES ogr_vfk.h
          vfkreader.h
          vfkreaderp.h
          ogrvfkdatasource.cpp
          ogrvfkdriver.cpp
          ogrvfklayer.cpp
          vfkdatablock.cpp
          vfkdatablocksqlite.cpp
          vfkproperty.cpp
          vfkpropertydefn.cpp
          vfkreader.cpp
          vfkreadersqlite.cpp
          vfkfeature.cpp
          vfkfeaturesqlite.cpp
  CORE_SOURCES
          ogrvfkdrivercore.cpp
  PLUGIN_CAPABLE
  NO_SHARED_SYMBOL_WITH_CORE
  NO_CXX_WFLAGS_EFFCXX
  NO_WFLAG_OLD_STYLE_CAST
)
if(NOT TARGET ogr_VFK)
    return()
endif()

gdal_standard_includes(ogr_VFK)
if (GDAL_USE_SQLITE3)
  target_include_directories(ogr_VFK PRIVATE ${SQLite3_INCLUDE_DIR})
  gdal_target_link_libraries(ogr_VFK PRIVATE SQLite::SQLite3)
  target_compile_definitions(ogr_VFK PRIVATE -DHAVE_SQLITE)
endif ()
if (GDAL_USE_GEOS)
  target_compile_definitions(ogr_VFK PRIVATE -DHAVE_GEOS=1)
  gdal_target_link_libraries(ogr_VFK PRIVATE ${GEOS_TARGET})
endif ()
