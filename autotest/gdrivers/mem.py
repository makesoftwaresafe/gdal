#!/usr/bin/env pytest
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test MEM format driver.
# Author:   Frank Warmerdam <warmerdam@pobox.com>
#
###############################################################################
# Copyright (c) 2005, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2008-2012, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT
###############################################################################

import ctypes
import struct

import gdaltest
import pytest

from osgeo import gdal, osr


@pytest.fixture
@gdaltest.disable_exceptions()
def mem_native_memory():

    with gdal.quiet_errors():
        ds = gdal.Open("MEM:::")
    assert ds is None, "opening MEM dataset should have failed."
    for libname in ["msvcrt", "libc.so.6"]:
        try:
            crt = ctypes.CDLL(libname)
        except OSError:
            crt = None
        if crt is not None:
            break

    if crt is None:
        pytest.skip()

    malloc = crt.malloc
    malloc.argtypes = [ctypes.c_size_t]
    malloc.restype = ctypes.c_void_p

    free = crt.free
    free.argtypes = [ctypes.c_void_p]
    free.restype = None

    # allocate band data array.
    width = 50
    height = 3
    p = malloc(width * height * 4)
    if p is None:
        pytest.skip()

    return p, free, width, height


###############################################################################
# Create a MEM dataset, and set some data, then test it.


def test_mem_1():

    #######################################################
    # Setup dataset
    drv = gdal.GetDriverByName("MEM")
    ds = drv.Create("mem_1.mem", 50, 3)

    assert ds.GetProjection() == "", "projection wrong"

    assert ds.GetGeoTransform(can_return_null=True) is None, "geotransform wrong"

    raw_data = b"".join(struct.pack("f", v) for v in range(150))
    ds.WriteRaster(0, 0, 50, 3, raw_data, buf_type=gdal.GDT_Float32, band_list=[1])

    wkt = gdaltest.user_srs_to_wkt("EPSG:26711")
    ds.SetProjection(wkt)

    gt = (440720, 5, 0, 3751320, 0, -5)
    ds.SetGeoTransform(gt)

    band = ds.GetRasterBand(1)
    band.SetNoDataValue(-1.0)

    # Set GCPs()
    wkt_gcp = gdaltest.user_srs_to_wkt("EPSG:4326")
    gcps = [gdal.GCP(0, 1, 2, 3, 4)]
    ds.SetGCPs([], "")
    ds.SetGCPs(gcps, wkt_gcp)
    ds.SetGCPs([], "")
    ds.SetGCPs(gcps, wkt_gcp)
    ds.SetGCPs(gcps, wkt_gcp)

    #######################################################
    # Verify dataset.

    assert band.GetNoDataValue() == -1.0, "no data is wrong"

    assert ds.GetProjection() == wkt, "projection wrong"

    assert ds.GetGeoTransform() == gt, "geotransform wrong"

    assert band.Checksum() == 1531, "checksum wrong"

    assert ds.GetGCPCount() == 1, "GetGCPCount wrong"

    assert len(ds.GetGCPs()) == 1, "GetGCPs wrong"

    assert ds.GetGCPProjection() == wkt_gcp, "GetGCPProjection wrong"

    assert band.DeleteNoDataValue() == 0, "wrong return code"
    assert band.GetNoDataValue() is None, "got nodata value whereas none was expected"

    gdaltest.mem_ds = None


###############################################################################
# Open an in-memory array.


def test_mem_2(mem_native_memory):

    p, free, width, height = mem_native_memory
    float_p = ctypes.cast(p, ctypes.POINTER(ctypes.c_float))

    # build ds name.
    dsnames = [
        "MEM:::DATAPOINTER=0x%X,PIXELS=%d,LINES=%d,BANDS=1,DATATYPE=Float32,PIXELOFFSET=4,LINEOFFSET=%d,BANDOFFSET=0"
        % (p, width, height, width * 4),
        "MEM:::DATAPOINTER=0x%X,PIXELS=%d,LINES=%d,DATATYPE=Float32"
        % (p, width, height),
    ]

    for dsname in dsnames:

        for i in range(width * height):
            float_p[i] = 5.0

        with pytest.raises(
            Exception,
            match="Opening a MEM dataset with the MEM:::DATAPOINTER= syntax is no longer supported by default for security reasons",
        ):
            gdal.Open(dsname)

        with gdal.config_option("GDAL_MEM_ENABLE_OPEN", "YES"):
            dsro = gdal.Open(dsname)
        if dsro is None:
            free(p)
            pytest.fail("opening MEM dataset failed in read only mode.")

        chksum = dsro.GetRasterBand(1).Checksum()
        if chksum != 750:
            print(chksum)
            free(p)
            pytest.fail("checksum failed.")
        dsro = None

        with gdal.config_option("GDAL_MEM_ENABLE_OPEN", "YES"):
            dsup = gdal.Open(dsname, gdal.GA_Update)
        if dsup is None:
            free(p)
            pytest.fail("opening MEM dataset failed in update mode.")

        dsup.GetRasterBand(1).Fill(100.0)
        dsup.FlushCache()

        if float_p[0] != 100.0:
            print(float_p[0])
            free(p)
            pytest.fail("fill seems to have failed.")

        dsup = None


@pytest.mark.parametrize(
    "ds_definition, expected_sr",
    [
        (
            r"MEM:::DATAPOINTER=0x{datapointer:X},GEOTRANSFORM=-1e+06/1953.125/0/1e+06/0/-3906.25,PIXELS=50,LINES=3,SPATIALREFERENCE={proj_crs},DATATYPE=Float32",
            "Lambert",
        ),
        (
            r"MEM:::DATAPOINTER=0x{datapointer:X},GEOTRANSFORM=-1e+06/1953.125/0/1e+06/0/-3906.25,PIXELS=50,LINES=3,SPATIALREFERENCE=bogus,DATATYPE=Float32",
            "",
        ),
        (
            r'MEM:::DATAPOINTER=0x{datapointer:X},GEOTRANSFORM=-1e+06/1953.125/0/1e+06/0/-3906.25,PIXELS=50,LINES=3,SPATIALREFERENCE="{ll_crs}",DATATYPE=Float32',
            "GEOGCS",
        ),
    ],
)
def test_geotransform(ds_definition, expected_sr, mem_native_memory):
    """Test GEOTRANSFORM and SPATIALREFERENCE"""

    p, free, width, height = mem_native_memory

    ## more ds names, ensure GEOTRANSFORM and SPATIALREFERENCE get tested
    proj_crs = "+proj=laea +lon_0=147 +lat_0=-42"
    ll_crs = """GEOGCS[\\"WGS 84\\",DATUM[\\"WGS_1984\\",SPHEROID[\\"WGS 84\\",6378137,298.257223563,AUTHORITY[\\"EPSG\\",\\"7030\\"]],AUTHORITY[\\"EPSG\\",\\"6326\\"]],PRIMEM[\\"Greenwich\\",0,AUTHORITY[\\"EPSG\\",\\"8901\\"]],UNIT[\\"degree\\",0.0174532925199433,AUTHORITY[\\"EPSG\\",\\"9122\\"]],AXIS[\\"Latitude\\",NORTH],AXIS[\\"Longitude\\",EAST],AUTHORITY[\\"EPSG\\",\\"4326\\"]]"""

    with gdal.config_option("GDAL_MEM_ENABLE_OPEN", "YES"):
        dsro = gdal.Open(
            ds_definition.format(datapointer=p, proj_crs=proj_crs, ll_crs=ll_crs)
        )
    if dsro is None:
        free(p)
        pytest.fail("opening MEM dataset failed in read only mode.")

    assert dsro.GetGeoTransform() == (-1e06, 1953.125, 0, 1e06, 0, -3906.25)
    assert expected_sr in dsro.GetProjectionRef()
    dsro = None
    free(p)


###############################################################################
# Test creating a MEM dataset with the "MEM:::" name


def test_mem_3():

    drv = gdal.GetDriverByName("MEM")
    ds = drv.Create("MEM:::", 1, 1, 1)
    assert ds is not None
    ds = None


###############################################################################
# Test creating a band interleaved multi-band MEM dataset


def test_mem_4():

    drv = gdal.GetDriverByName("MEM")

    ds = drv.Create("", 100, 100, 3)
    expected_cs = [0, 0, 0]
    for i in range(3):
        cs = ds.GetRasterBand(i + 1).Checksum()
        assert cs == expected_cs[i], "did not get expected checksum for band %d" % (
            i + 1
        )

    ds.GetRasterBand(1).Fill(255)
    expected_cs = [57182, 0, 0]
    for i in range(3):
        cs = ds.GetRasterBand(i + 1).Checksum()
        assert (
            cs == expected_cs[i]
        ), "did not get expected checksum for band %d after fill" % (i + 1)

    assert (
        ds.GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE") == "BAND"
    ), "did not get expected INTERLEAVE value"

    ds = None


###############################################################################
# Test creating a pixel interleaved multi-band MEM dataset


def test_mem_5():

    drv = gdal.GetDriverByName("MEM")

    ds = drv.Create("", 100, 100, 3, options=["INTERLEAVE=PIXEL"])
    expected_cs = [0, 0, 0]
    for i in range(3):
        cs = ds.GetRasterBand(i + 1).Checksum()
        assert cs == expected_cs[i], "did not get expected checksum for band %d" % (
            i + 1
        )

    ds.GetRasterBand(1).Fill(255)
    expected_cs = [57182, 0, 0]
    for i in range(3):
        cs = ds.GetRasterBand(i + 1).Checksum()
        assert (
            cs == expected_cs[i]
        ), "did not get expected checksum for band %d after fill" % (i + 1)

    assert (
        ds.GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE") == "PIXEL"
    ), "did not get expected INTERLEAVE value"

    ds = None


###############################################################################
# Test out-of-memory situations


@gdaltest.disable_exceptions()
def test_mem_6():

    if gdal.GetConfigOption("SKIP_MEM_INTENSIVE_TEST") is not None:
        pytest.skip()

    drv = gdal.GetDriverByName("MEM")

    # Multiplication overflow
    with gdal.quiet_errors():
        ds = drv.Create("", 1, 1, 0x7FFFFFFF, gdal.GDT_Float64)
    assert ds is None
    ds = None

    # Multiplication overflow
    with gdal.quiet_errors():
        ds = drv.Create("", 0x7FFFFFFF, 0x7FFFFFFF, 16)
    assert ds is None
    ds = None

    # Multiplication overflow
    with gdal.quiet_errors():
        ds = drv.Create("", 0x7FFFFFFF, 0x7FFFFFFF, 1, gdal.GDT_Float64)
    assert ds is None
    ds = None

    # Out of memory error
    with gdal.quiet_errors():
        ds = drv.Create("", 0x7FFFFFFF, 0x7FFFFFFF, 1, options=["INTERLEAVE=PIXEL"])
    assert ds is None
    ds = None

    # Out of memory error
    with gdal.quiet_errors():
        ds = drv.Create("", 0x7FFFFFFF, 0x7FFFFFFF, 1)
    assert ds is None
    ds = None

    # 32 bit overflow on 32-bit builds, or possible out of memory error
    ds = drv.Create("", 0x7FFFFFFF, 1, 0)
    with gdal.quiet_errors():
        ds.AddBand(gdal.GDT_Float64)

    # Will raise out of memory error in all cases
    ds = drv.Create("", 0x7FFFFFFF, 0x7FFFFFFF, 0)
    with gdal.quiet_errors():
        ret = ds.AddBand(gdal.GDT_Float64)
    assert ret != 0


###############################################################################
# Test AddBand()


def test_mem_7():

    drv = gdal.GetDriverByName("MEM")
    ds = drv.Create("MEM:::", 1, 1, 1)
    ds.AddBand(gdal.GDT_Byte, [])
    assert ds.RasterCount == 2
    ds = None


###############################################################################
# Test SetDefaultHistogram() / GetDefaultHistogram()


def test_mem_8():

    drv = gdal.GetDriverByName("MEM")
    ds = drv.Create("MEM:::", 1, 1, 1)
    ds.GetRasterBand(1).SetDefaultHistogram(0, 255, [])
    ds.GetRasterBand(1).SetDefaultHistogram(1, 2, [5, 6])
    ds.GetRasterBand(1).SetDefaultHistogram(1, 2, [3000000000, 4])
    hist = ds.GetRasterBand(1).GetDefaultHistogram(force=0)
    ds = None

    assert hist == (1.0, 2.0, 2, [3000000000, 4])


###############################################################################
# Test RasterIO()


def test_mem_9():

    # Test IRasterIO(GF_Read,)
    src_ds = gdal.Open("data/rgbsmall.tif")
    drv = gdal.GetDriverByName("MEM")

    for interleave in ["BAND", "PIXEL"]:
        out_ds = drv.CreateCopy("", src_ds, options=["INTERLEAVE=%s" % interleave])
        ref_data = src_ds.GetRasterBand(2).ReadRaster(20, 8, 4, 5)
        got_data = out_ds.GetRasterBand(2).ReadRaster(20, 8, 4, 5)
        if ref_data != got_data:
            import struct

            print(struct.unpack("B" * 4 * 5, ref_data))
            print(struct.unpack("B" * 4 * 5, got_data))
            pytest.fail(interleave)

        ref_data = src_ds.GetRasterBand(2).ReadRaster(
            20, 8, 4, 5, buf_pixel_space=3, buf_line_space=100
        )
        got_data = out_ds.GetRasterBand(2).ReadRaster(
            20, 8, 4, 5, buf_pixel_space=3, buf_line_space=100
        )
        assert ref_data == got_data, interleave

        ref_data = src_ds.ReadRaster(20, 8, 4, 5)
        got_data = out_ds.ReadRaster(20, 8, 4, 5)
        assert ref_data == got_data, interleave

        ref_data = src_ds.ReadRaster(20, 8, 4, 5, buf_pixel_space=3, buf_band_space=1)
        got_data = out_ds.ReadRaster(20, 8, 4, 5, buf_pixel_space=3, buf_band_space=1)
        assert ref_data == got_data, interleave

        out_ds.WriteRaster(20, 8, 4, 5, got_data, buf_pixel_space=3, buf_band_space=1)
        got_data = out_ds.ReadRaster(20, 8, 4, 5, buf_pixel_space=3, buf_band_space=1)
        assert ref_data == got_data, interleave

        ref_data = src_ds.ReadRaster(
            20, 8, 4, 5, buf_pixel_space=3, buf_line_space=100, buf_band_space=1
        )
        got_data = out_ds.ReadRaster(
            20, 8, 4, 5, buf_pixel_space=3, buf_line_space=100, buf_band_space=1
        )
        assert ref_data == got_data, interleave

        ref_data = src_ds.ReadRaster(
            20, 20, 4, 5, buf_type=gdal.GDT_Int32, buf_pixel_space=12, buf_band_space=4
        )
        got_data = out_ds.ReadRaster(
            20, 20, 4, 5, buf_type=gdal.GDT_Int32, buf_pixel_space=12, buf_band_space=4
        )
        assert ref_data == got_data, interleave
        out_ds.WriteRaster(
            20,
            20,
            4,
            5,
            got_data,
            buf_type=gdal.GDT_Int32,
            buf_pixel_space=12,
            buf_band_space=4,
        )
        got_data = out_ds.ReadRaster(
            20, 20, 4, 5, buf_type=gdal.GDT_Int32, buf_pixel_space=12, buf_band_space=4
        )
        assert ref_data == got_data, interleave

        # Test IReadBlock
        ref_data = src_ds.GetRasterBand(1).ReadRaster(0, 10, src_ds.RasterXSize, 1)
        # This is a bit nasty to have to do that. We should fix the core
        # to make that unnecessary
        out_ds.FlushCache()
        got_data = out_ds.GetRasterBand(1).ReadBlock(0, 10)
        assert ref_data == got_data, interleave

        # Test IRasterIO(GF_Write,)
        ref_data = src_ds.GetRasterBand(1).ReadRaster(2, 3, 4, 5)
        out_ds.GetRasterBand(1).WriteRaster(6, 7, 4, 5, ref_data)
        got_data = out_ds.GetRasterBand(1).ReadRaster(6, 7, 4, 5)
        assert ref_data == got_data

        # Test IRasterIO(GF_Write, change data type) + IWriteBlock() + IRasterIO(GF_Read, change data type)
        ref_data = src_ds.GetRasterBand(1).ReadRaster(
            10, 11, 4, 5, buf_type=gdal.GDT_Int32
        )
        out_ds.GetRasterBand(1).WriteRaster(
            10, 11, 4, 5, ref_data, buf_type=gdal.GDT_Int32
        )
        got_data = out_ds.GetRasterBand(1).ReadRaster(
            10, 11, 4, 5, buf_type=gdal.GDT_Int32
        )
        assert ref_data == got_data, interleave

        ref_data = src_ds.GetRasterBand(1).ReadRaster(10, 11, 4, 5)
        got_data = out_ds.GetRasterBand(1).ReadRaster(10, 11, 4, 5)
        assert ref_data == got_data, interleave

        # Test IRasterIO(GF_Write, resampling) + IWriteBlock() + IRasterIO(GF_Read, resampling)
        ref_data = src_ds.GetRasterBand(1).ReadRaster(10, 11, 4, 5)
        ref_data_zoomed = src_ds.GetRasterBand(1).ReadRaster(10, 11, 4, 5, 8, 10)
        out_ds.GetRasterBand(1).WriteRaster(10, 11, 8, 10, ref_data, 4, 5)
        got_data = out_ds.GetRasterBand(1).ReadRaster(10, 11, 8, 10)
        assert ref_data_zoomed == got_data, interleave

        got_data = out_ds.GetRasterBand(1).ReadRaster(10, 11, 8, 10, 4, 5)
        assert ref_data == got_data, interleave

    for interleave in ["BAND", "PIXEL"]:
        out_ds = drv.CreateCopy("", src_ds, options=["INTERLEAVE=%s" % interleave])
        for i in range(3):
            out_ds.GetRasterBand(i + 1).Fill(0)
        ref_data = src_ds.ReadRaster(
            0, 10, out_ds.RasterXSize, 5, buf_pixel_space=3, buf_band_space=1
        )
        out_ds.WriteRaster(
            0, 10, out_ds.RasterXSize, 5, ref_data, buf_pixel_space=3, buf_band_space=1
        )
        got_data = out_ds.ReadRaster(
            0, 10, out_ds.RasterXSize, 5, buf_pixel_space=3, buf_band_space=1
        )
        assert ref_data == got_data, interleave

    for interleave in ["BAND", "PIXEL"]:
        out_ds = drv.CreateCopy("", src_ds, options=["INTERLEAVE=%s" % interleave])
        for i in range(3):
            out_ds.GetRasterBand(i + 1).Fill(0)
        ref_data = src_ds.ReadRaster(4, 10, 15, 5, buf_pixel_space=3, buf_band_space=1)
        out_ds.WriteRaster(4, 10, 15, 5, ref_data, buf_pixel_space=3, buf_band_space=1)
        got_data = out_ds.ReadRaster(4, 10, 15, 5, buf_pixel_space=3, buf_band_space=1)
        assert ref_data == got_data, interleave


###############################################################################
# Test BuildOverviews()


@gdaltest.disable_exceptions()
def test_mem_10():

    # Error case: building overview on a 0 band dataset
    ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 0)
    with gdal.quiet_errors():
        ds.BuildOverviews("NEAR", [2])

    # Requesting overviews when they are not
    ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    assert ds.GetRasterBand(1).GetOverviewCount() == 0
    assert ds.GetRasterBand(1).GetOverview(-1) is None
    assert ds.GetRasterBand(1).GetOverview(0) is None

    # Single band case
    ds = gdal.GetDriverByName("MEM").CreateCopy("", gdal.Open("data/byte.tif"))
    for _ in range(2):
        ret = ds.BuildOverviews("NEAR", [2])
        assert ret == 0
        assert ds.GetRasterBand(1).GetOverviewCount() == 1
        cs = ds.GetRasterBand(1).GetOverview(0).Checksum()
        assert cs == 1087

    ret = ds.BuildOverviews("NEAR", [4])
    assert ret == 0
    assert ds.GetRasterBand(1).GetOverviewCount() == 2
    cs = ds.GetRasterBand(1).GetOverview(0).Checksum()
    assert cs == 1087
    cs = ds.GetRasterBand(1).GetOverview(1).Checksum()
    assert cs == 328

    ret = ds.BuildOverviews("NEAR", [2, 4])
    assert ret == 0
    assert ds.GetRasterBand(1).GetOverviewCount() == 2
    cs = ds.GetRasterBand(1).GetOverview(0).Checksum()
    assert cs == 1087
    cs = ds.GetRasterBand(1).GetOverview(1).Checksum()
    assert cs == 328

    # Test that average in one or several steps give the same result
    ds.GetRasterBand(1).GetOverview(0).Fill(0)
    ds.GetRasterBand(1).GetOverview(1).Fill(0)

    ret = ds.BuildOverviews("AVERAGE", [2, 4])
    assert ret == 0
    assert ds.GetRasterBand(1).GetOverviewCount() == 2
    cs = ds.GetRasterBand(1).GetOverview(0).Checksum()
    assert cs == 1152
    cs = ds.GetRasterBand(1).GetOverview(1).Checksum()
    assert cs == 240

    ds.GetRasterBand(1).GetOverview(0).Fill(0)
    ds.GetRasterBand(1).GetOverview(1).Fill(0)

    ret = ds.BuildOverviews("AVERAGE", [2])
    ret = ds.BuildOverviews("AVERAGE", [4])
    assert ret == 0
    assert ds.GetRasterBand(1).GetOverviewCount() == 2
    cs = ds.GetRasterBand(1).GetOverview(0).Checksum()
    assert cs == 1152
    cs = ds.GetRasterBand(1).GetOverview(1).Checksum()
    assert cs == 240

    ds = None

    # Multiple band case
    ds = gdal.GetDriverByName("MEM").CreateCopy("", gdal.Open("data/rgbsmall.tif"))
    ret = ds.BuildOverviews("NEAR", [2])
    assert ret == 0
    cs = ds.GetRasterBand(1).GetOverview(0).Checksum()
    assert cs == 5057
    cs = ds.GetRasterBand(2).GetOverview(0).Checksum()
    assert cs == 5304
    cs = ds.GetRasterBand(3).GetOverview(0).Checksum()
    assert cs == 5304
    ds = None

    # Clean overviews
    ds = gdal.GetDriverByName("MEM").CreateCopy("", gdal.Open("data/byte.tif"))
    ret = ds.BuildOverviews("NEAR", [2])
    assert ret == 0
    ret = ds.BuildOverviews("NONE", [])
    assert ret == 0
    assert ds.GetRasterBand(1).GetOverviewCount() == 0
    ds = None


###############################################################################
# Test CreateMaskBand()


def test_mem_11():

    # Error case: building overview on a 0 band dataset
    ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 0)
    assert ds.CreateMaskBand(gdal.GMF_PER_DATASET) != 0

    # Per dataset mask on single band dataset
    ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    assert ds.CreateMaskBand(gdal.GMF_PER_DATASET) == 0
    assert ds.GetRasterBand(1).GetMaskFlags() == gdal.GMF_PER_DATASET
    assert not ds.GetRasterBand(1).IsMaskBand()
    mask = ds.GetRasterBand(1).GetMaskBand()
    assert mask.IsMaskBand()
    cs = mask.Checksum()
    assert cs == 0
    mask.Fill(255)
    cs = mask.Checksum()
    assert cs == 3

    # Check that the per dataset mask is shared by all bands
    ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 2)
    assert ds.CreateMaskBand(gdal.GMF_PER_DATASET) == 0
    mask1 = ds.GetRasterBand(1).GetMaskBand()
    mask1.Fill(255)
    mask2 = ds.GetRasterBand(2).GetMaskBand()
    cs = mask2.Checksum()
    assert cs == 3

    # Same but call it on band 2
    ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 2)
    assert ds.GetRasterBand(2).CreateMaskBand(gdal.GMF_PER_DATASET) == 0
    mask2 = ds.GetRasterBand(2).GetMaskBand()
    mask2.Fill(255)
    mask1 = ds.GetRasterBand(1).GetMaskBand()
    cs = mask1.Checksum()
    assert cs == 3

    # Per band masks
    ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 2)
    assert ds.GetRasterBand(1).CreateMaskBand(0) == 0
    assert ds.GetRasterBand(2).CreateMaskBand(0) == 0
    mask1 = ds.GetRasterBand(1).GetMaskBand()
    mask2 = ds.GetRasterBand(2).GetMaskBand()
    mask2.Fill(255)
    cs1 = mask1.Checksum()
    cs2 = mask2.Checksum()
    assert cs1 == 0 and cs2 == 3


###############################################################################
# Test CreateMaskBand() and overviews.


def test_mem_12():

    # Test on per-band mask
    ds = gdal.GetDriverByName("MEM").Create("", 10, 10, 2)
    ds.GetRasterBand(1).CreateMaskBand(0)
    ds.GetRasterBand(1).GetMaskBand().Fill(127)
    ds.BuildOverviews("NEAR", [2])
    cs = ds.GetRasterBand(1).GetOverview(0).GetMaskBand().Checksum()
    assert cs == 267

    # Default mask
    cs = ds.GetRasterBand(2).GetOverview(0).GetMaskBand().Checksum()
    assert cs == 283

    # Test on per-dataset mask
    ds = gdal.GetDriverByName("MEM").Create("", 10, 10, 2)
    ds.CreateMaskBand(gdal.GMF_PER_DATASET)
    ds.GetRasterBand(1).GetMaskBand().Fill(127)
    ds.BuildOverviews("NEAR", [2])
    cs = ds.GetRasterBand(1).GetOverview(0).GetMaskBand().Checksum()
    assert cs == 267
    cs2 = ds.GetRasterBand(2).GetOverview(0).GetMaskBand().Checksum()
    assert cs2 == cs


###############################################################################
# Check RAT support


def test_mem_rat():

    ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    ds.GetRasterBand(1).SetDefaultRAT(gdal.RasterAttributeTable())
    assert ds.GetRasterBand(1).GetDefaultRAT() is not None
    ds.GetRasterBand(1).SetDefaultRAT(None)
    assert ds.GetRasterBand(1).GetDefaultRAT() is None


###############################################################################
# Check CategoryNames support


def test_mem_categorynames():

    ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    ds.GetRasterBand(1).SetCategoryNames(["foo"])
    assert ds.GetRasterBand(1).GetCategoryNames() == ["foo"]
    ds.GetRasterBand(1).SetCategoryNames([])
    assert ds.GetRasterBand(1).GetCategoryNames() is None


###############################################################################
# Check ColorTable support


def test_mem_colortable():

    ds = gdal.GetDriverByName("MEM").Create("", 1, 1)
    ct = gdal.ColorTable()
    ct.SetColorEntry(0, (255, 255, 255, 255))
    ds.GetRasterBand(1).SetColorTable(ct)
    assert ds.GetRasterBand(1).GetColorTable().GetCount() == 1
    ds.GetRasterBand(1).SetColorTable(None)
    assert ds.GetRasterBand(1).GetColorTable() is None


###############################################################################
# Test dataset RasterIO with non nearest resampling


def test_mem_dataset_rasterio_non_nearest_resampling_source_with_ovr():

    ds = gdal.GetDriverByName("MEM").Create("", 10, 10, 3)
    ds.GetRasterBand(1).Fill(255)
    ds.BuildOverviews("NONE", [2])
    ds.GetRasterBand(1).GetOverview(0).Fill(10)

    got_data = ds.ReadRaster(0, 0, 10, 10, 5, 5)
    got_data = struct.unpack("B" * 5 * 5 * 3, got_data)
    assert got_data[0] == 10

    got_data = ds.ReadRaster(0, 0, 10, 10, 5, 5, resample_alg=gdal.GRIORA_Cubic)
    got_data = struct.unpack("B" * 5 * 5 * 3, got_data)
    assert got_data[0] == 10


###############################################################################
# Test Int64 nodata


def test_mem_nodata_int64():

    ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 1, gdal.GDT_Int64)
    val = -(1 << 63)
    assert ds.GetRasterBand(1).SetNoDataValue(val) == gdal.CE_None
    assert ds.GetRasterBand(1).GetNoDataValue() == val


###############################################################################
# Test UInt64 nodata


def test_mem_nodata_uint64():

    ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 1, gdal.GDT_UInt64)
    val = (1 << 64) - 1
    assert ds.GetRasterBand(1).SetNoDataValue(val) == gdal.CE_None
    assert ds.GetRasterBand(1).GetNoDataValue() == val


###############################################################################
# Check IsMaskBand() on an alpha band


def test_mem_alpha_ismaskband():

    ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 2)
    ds.GetRasterBand(2).SetColorInterpretation(gdal.GCI_AlphaBand)
    assert not ds.GetRasterBand(1).IsMaskBand()
    assert ds.GetRasterBand(2).IsMaskBand()


###############################################################################
# Check robustness to GDT_Unknown


def test_mem_gdt_unknown():

    with pytest.raises(Exception, match="Illegal GDT_Unknown/GDT_TypeCount argument"):
        gdal.GetDriverByName("MEM").Create("", 1, 1, 1, gdal.GDT_Unknown)

    with gdal.GetDriverByName("MEM").Create("", 1, 1, 0, gdal.GDT_Unknown) as ds:
        with pytest.raises(
            Exception, match="Illegal GDT_Unknown/GDT_TypeCount argument"
        ):
            ds.AddBand(gdal.GDT_Unknown)


###############################################################################
# Test Dataset.GetExtent()


def test_mem_extent():

    ds = gdal.GetDriverByName("MEM").Create("", 2, 10)
    assert ds.GetExtent() is None
    assert ds.GetExtentWGS84LongLat() is None

    srs = osr.SpatialReference(epsg=3857)
    ds.SetSpatialRef(srs)
    ds.SetGeoTransform([1000, 1, 0, 2000, 0, -2])

    assert ds.TestCapability(gdal.GDsCFastGetExtent)
    assert ds.GetExtent() == (1000, 1002, 1980, 2000)
    assert ds.TestCapability(gdal.GDsCFastGetExtentWGS84LongLat)
    assert ds.GetExtentWGS84LongLat() == pytest.approx(
        (
            0.008983152841195215,
            0.009001119146877604,
            0.017786642339884726,
            0.01796630538796444,
        )
    )
