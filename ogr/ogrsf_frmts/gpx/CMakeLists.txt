add_gdal_driver(TARGET ogr_GPX
                SOURCES ogr_gpx.h ogrgpxdatasource.cpp ogrgpxdriver.cpp ogrgpxlayer.cpp
  PLUGIN_CAPABLE
  NO_DEPS
  NO_CXX_WFLAGS_EFFCXX
  NO_WFLAG_OLD_STYLE_CAST
)
gdal_standard_includes(ogr_GPX)

if (GDAL_USE_EXPAT)
  target_compile_definitions(ogr_GPX PRIVATE -DHAVE_EXPAT=1)
  gdal_target_link_libraries(ogr_GPX PRIVATE ${EXPAT_TARGET})
endif ()
