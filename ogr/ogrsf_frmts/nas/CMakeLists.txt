add_gdal_driver(
  TARGET ogr_NAS
  SOURCES nashandler.cpp
          nasreader.cpp
          nasreaderp.h
          ogr_nas.h
          ogrnasdatasource.cpp
          ogrnasdriver.cpp
          ogrnaslayer.cpp
  BUILTIN  # BUILTIN because of GML dependency
  NO_CXX_WFLAGS_EFFCXX
  NO_WFLAG_OLD_STYLE_CAST
)
gdal_standard_includes(ogr_NAS)
target_include_directories(ogr_NAS PRIVATE $<TARGET_PROPERTY:ogr_GML,SOURCE_DIR> $<TARGET_PROPERTY:gmlutils,SOURCE_DIR>)
if (GDAL_USE_EXPAT)
  target_compile_definitions(ogr_NAS PRIVATE -DHAVE_EXPAT)
  gdal_target_link_libraries(ogr_NAS PRIVATE ${EXPAT_TARGET})
endif ()

foreach (d ${XercesC_INCLUDE_DIRS})
  target_include_directories(ogr_NAS PRIVATE ${d} ${d}/xercesc)
endforeach ()
gdal_target_link_libraries(ogr_NAS PRIVATE XercesC::XercesC)
target_compile_definitions(ogr_NAS PRIVATE -DHAVE_XERCES)
