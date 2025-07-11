/******************************************************************************
 *
 * Project:  Vertical Datum Transformation
 * Purpose:  Implementation of NOAA .gtx vertical datum shift file format.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2010, Frank Warmerdam
 * Copyright (c) 2010-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_string.h"
#include "gdal_frmts.h"
#include "ogr_srs_api.h"
#include "rawdataset.h"

#include <algorithm>
#include <limits>

/**

NOAA .GTX Vertical Datum Grid Shift Format

All values are bigendian

Header
------

float64  latitude_of_origin
float64  longitude_of_origin (0-360)
float64  cell size (x?y?)
float64  cell size (x?y?)
int32    length in pixels
int32    width in pixels

Data
----

float32  * width in pixels * length in pixels

Values are an offset in meters between two vertical datums.

**/

/************************************************************************/
/* ==================================================================== */
/*                              GTXDataset                              */
/* ==================================================================== */
/************************************************************************/

class GTXDataset final : public RawDataset
{
    VSILFILE *fpImage = nullptr;  // image data file.

    OGRSpatialReference m_oSRS{};
    GDALGeoTransform m_gt{};

    CPL_DISALLOW_COPY_ASSIGN(GTXDataset)

    CPLErr Close() override;

  public:
    GTXDataset()
    {
        m_oSRS.SetFromUserInput(SRS_WKT_WGS84_LAT_LONG);
        m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    }

    ~GTXDataset() override;

    CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;
    CPLErr SetGeoTransform(const GDALGeoTransform &gt) override;

    const OGRSpatialReference *GetSpatialRef() const override
    {
        return &m_oSRS;
    }

    static GDALDataset *Open(GDALOpenInfo *);
    static int Identify(GDALOpenInfo *);
    static GDALDataset *Create(const char *pszFilename, int nXSize, int nYSize,
                               int nBands, GDALDataType eType,
                               char **papszOptions);
};

/************************************************************************/
/* ==================================================================== */
/*                           GTXRasterBand                              */
/* ==================================================================== */
/************************************************************************/

class GTXRasterBand final : public RawRasterBand
{
    CPL_DISALLOW_COPY_ASSIGN(GTXRasterBand)

  public:
    GTXRasterBand(GDALDataset *poDS, int nBand, VSILFILE *fpRaw,
                  vsi_l_offset nImgOffset, int nPixelOffset, int nLineOffset,
                  GDALDataType eDataType, int bNativeOrder);

    ~GTXRasterBand() override;

    double GetNoDataValue(int *pbSuccess = nullptr) override;
};

/************************************************************************/
/*                            GTXRasterBand()                           */
/************************************************************************/

GTXRasterBand::GTXRasterBand(GDALDataset *poDSIn, int nBandIn,
                             VSILFILE *fpRawIn, vsi_l_offset nImgOffsetIn,
                             int nPixelOffsetIn, int nLineOffsetIn,
                             GDALDataType eDataTypeIn, int bNativeOrderIn)
    : RawRasterBand(poDSIn, nBandIn, fpRawIn, nImgOffsetIn, nPixelOffsetIn,
                    nLineOffsetIn, eDataTypeIn, bNativeOrderIn,
                    RawRasterBand::OwnFP::NO)
{
}

/************************************************************************/
/*                           ~GTXRasterBand()                           */
/************************************************************************/

GTXRasterBand::~GTXRasterBand()
{
}

/************************************************************************/
/*                           GetNoDataValue()                           */
/************************************************************************/

double GTXRasterBand::GetNoDataValue(int *pbSuccess)
{
    if (pbSuccess)
        *pbSuccess = TRUE;
    int bSuccess = FALSE;
    double dfNoData = GDALPamRasterBand::GetNoDataValue(&bSuccess);
    if (bSuccess)
    {
        return dfNoData;
    }
    return -88.8888;
}

/************************************************************************/
/* ==================================================================== */
/*                              GTXDataset                              */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                            ~GTXDataset()                             */
/************************************************************************/

GTXDataset::~GTXDataset()

{
    GTXDataset::Close();
}

/************************************************************************/
/*                              Close()                                 */
/************************************************************************/

CPLErr GTXDataset::Close()
{
    CPLErr eErr = CE_None;
    if (nOpenFlags != OPEN_FLAGS_CLOSED)
    {
        if (GTXDataset::FlushCache(true) != CE_None)
            eErr = CE_Failure;

        if (fpImage)
        {
            if (VSIFCloseL(fpImage) != 0)
            {
                CPLError(CE_Failure, CPLE_FileIO, "I/O error");
                eErr = CE_Failure;
            }
        }

        if (GDALPamDataset::Close() != CE_None)
            eErr = CE_Failure;
    }
    return eErr;
}

/************************************************************************/
/*                              Identify()                              */
/************************************************************************/

int GTXDataset::Identify(GDALOpenInfo *poOpenInfo)

{
    if (poOpenInfo->nHeaderBytes < 40)
        return FALSE;

    if (!poOpenInfo->IsExtensionEqualToCI("gtx"))
        return FALSE;

    return TRUE;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *GTXDataset::Open(GDALOpenInfo *poOpenInfo)

{
    if (!Identify(poOpenInfo) || poOpenInfo->fpL == nullptr)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Create a corresponding GDALDataset.                             */
    /* -------------------------------------------------------------------- */
    auto poDS = std::make_unique<GTXDataset>();

    poDS->eAccess = poOpenInfo->eAccess;
    std::swap(poDS->fpImage, poOpenInfo->fpL);

    /* -------------------------------------------------------------------- */
    /*      Read the header.                                                */
    /* -------------------------------------------------------------------- */
    double gt[6] = {0};
    CPL_IGNORE_RET_VAL(VSIFReadL(&gt[3], 8, 1, poDS->fpImage));
    CPL_IGNORE_RET_VAL(VSIFReadL(&gt[0], 8, 1, poDS->fpImage));
    CPL_IGNORE_RET_VAL(VSIFReadL(&gt[5], 8, 1, poDS->fpImage));
    CPL_IGNORE_RET_VAL(VSIFReadL(&gt[1], 8, 1, poDS->fpImage));

    CPL_IGNORE_RET_VAL(VSIFReadL(&(poDS->nRasterYSize), 4, 1, poDS->fpImage));
    CPL_IGNORE_RET_VAL(VSIFReadL(&(poDS->nRasterXSize), 4, 1, poDS->fpImage));

    CPL_MSBPTR32(&(poDS->nRasterYSize));
    CPL_MSBPTR32(&(poDS->nRasterXSize));

    CPL_MSBPTR64(&gt[0]);
    CPL_MSBPTR64(&gt[1]);
    CPL_MSBPTR64(&gt[3]);
    CPL_MSBPTR64(&gt[5]);

    poDS->m_gt = GDALGeoTransform(gt);
    poDS->m_gt[3] +=
        poDS->m_gt[5] * (static_cast<double>(poDS->nRasterYSize) - 1);

    poDS->m_gt[0] -= poDS->m_gt[1] * 0.5;
    poDS->m_gt[3] += poDS->m_gt[5] * 0.5;

    poDS->m_gt[5] *= -1;

    if (CPLFetchBool(poOpenInfo->papszOpenOptions,
                     "SHIFT_ORIGIN_IN_MINUS_180_PLUS_180", false))
    {
        if (poDS->m_gt[0] < -180.0 - poDS->m_gt[1])
            poDS->m_gt[0] += 360.0;
        else if (poDS->m_gt[0] > 180.0)
            poDS->m_gt[0] -= 360.0;
    }

    if (!GDALCheckDatasetDimensions(poDS->nRasterXSize, poDS->nRasterYSize) ||
        static_cast<vsi_l_offset>(poDS->nRasterXSize) * poDS->nRasterYSize >
            std::numeric_limits<vsi_l_offset>::max() / sizeof(double))
    {
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Guess the data type. Since October 1, 2009, it should be        */
    /*      Float32. Before it was double.                                  */
    /* -------------------------------------------------------------------- */
    CPL_IGNORE_RET_VAL(VSIFSeekL(poDS->fpImage, 0, SEEK_END));
    const vsi_l_offset nSize = VSIFTellL(poDS->fpImage);

    GDALDataType eDT = GDT_Float32;
    if (nSize - 40 == sizeof(double) *
                          static_cast<vsi_l_offset>(poDS->nRasterXSize) *
                          poDS->nRasterYSize)
        eDT = GDT_Float64;
    const int nDTSize = GDALGetDataTypeSizeBytes(eDT);
    if (nDTSize <= 0 || poDS->nRasterXSize > INT_MAX / nDTSize)
    {
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Create band information object.                                 */
    /* -------------------------------------------------------------------- */
    auto poBand = std::make_unique<GTXRasterBand>(
        poDS.get(), 1, poDS->fpImage,
        static_cast<vsi_l_offset>(poDS->nRasterYSize - 1) * poDS->nRasterXSize *
                nDTSize +
            40,
        nDTSize, poDS->nRasterXSize * -nDTSize, eDT, !CPL_IS_LSB);
    if (!poBand->IsValid())
        return nullptr;
    poDS->SetBand(1, std::move(poBand));

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->TryLoadXML();

    /* -------------------------------------------------------------------- */
    /*      Check for overviews.                                            */
    /* -------------------------------------------------------------------- */
    poDS->oOvManager.Initialize(poDS.get(), poOpenInfo->pszFilename);

    return poDS.release();
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr GTXDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    gt = m_gt;
    return CE_None;
}

/************************************************************************/
/*                          SetGeoTransform()                           */
/************************************************************************/

CPLErr GTXDataset::SetGeoTransform(const GDALGeoTransform &gt)
{
    if (gt[2] != 0.0 || gt[4] != 0.0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Attempt to write skewed or rotated geotransform to gtx.");
        return CE_Failure;
    }

    m_gt = gt;

    const double dfXOrigin = m_gt[0] + 0.5 * m_gt[1];
    const double dfYOrigin = m_gt[3] + (nRasterYSize - 0.5) * m_gt[5];
    const double dfWidth = m_gt[1];
    const double dfHeight = -m_gt[5];

    unsigned char header[32] = {'\0'};
    memcpy(header + 0, &dfYOrigin, 8);
    CPL_MSBPTR64(header + 0);

    memcpy(header + 8, &dfXOrigin, 8);
    CPL_MSBPTR64(header + 8);

    memcpy(header + 16, &dfHeight, 8);
    CPL_MSBPTR64(header + 16);

    memcpy(header + 24, &dfWidth, 8);
    CPL_MSBPTR64(header + 24);

    if (VSIFSeekL(fpImage, SEEK_SET, 0) != 0 ||
        VSIFWriteL(header, 32, 1, fpImage) != 1)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Attempt to write geotransform header to GTX failed.");
        return CE_Failure;
    }

    return CE_None;
}

/************************************************************************/
/*                               Create()                               */
/************************************************************************/

GDALDataset *GTXDataset::Create(const char *pszFilename, int nXSize, int nYSize,
                                int /* nBands */, GDALDataType eType,
                                char ** /* papszOptions */)
{
    if (eType != GDT_Float32)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Attempt to create gtx file with unsupported data type '%s'.",
                 GDALGetDataTypeName(eType));
        return nullptr;
    }

    if (!EQUAL(CPLGetExtensionSafe(pszFilename).c_str(), "gtx"))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Attempt to create gtx file with extension other than gtx.");
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Try to create the file.                                         */
    /* -------------------------------------------------------------------- */
    VSILFILE *fp = VSIFOpenL(pszFilename, "wb");
    if (fp == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Attempt to create file `%s' failed.\n", pszFilename);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Write out the header with stub georeferencing.                  */
    /* -------------------------------------------------------------------- */

    unsigned char header[40] = {'\0'};
    double dfYOrigin = 0.0;
    memcpy(header + 0, &dfYOrigin, 8);
    CPL_MSBPTR64(header + 0);

    double dfXOrigin = 0.0;
    memcpy(header + 8, &dfXOrigin, 8);
    CPL_MSBPTR64(header + 8);

    double dfYSize = 0.01;
    memcpy(header + 16, &dfYSize, 8);
    CPL_MSBPTR64(header + 16);

    double dfXSize = 0.01;
    memcpy(header + 24, &dfXSize, 8);
    CPL_MSBPTR64(header + 24);

    GInt32 nYSize32 = nYSize;
    memcpy(header + 32, &nYSize32, 4);
    CPL_MSBPTR32(header + 32);

    GInt32 nXSize32 = nXSize;
    memcpy(header + 36, &nXSize32, 4);
    CPL_MSBPTR32(header + 36);

    CPL_IGNORE_RET_VAL(VSIFWriteL(header, 40, 1, fp));
    CPL_IGNORE_RET_VAL(VSIFCloseL(fp));

    return GDALDataset::FromHandle(GDALOpen(pszFilename, GA_Update));
}

/************************************************************************/
/*                          GDALRegister_GTX()                          */
/************************************************************************/

void GDALRegister_GTX()

{
    if (GDALGetDriverByName("GTX") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("GTX");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "NOAA Vertical Datum .GTX");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSION, "gtx");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");
    // poDriver->SetMetadataItem( GDAL_DMD_HELPTOPIC,
    //                            "frmt_various.html#GTX" );
    poDriver->SetMetadataItem(
        GDAL_DMD_OPENOPTIONLIST,
        "<OpenOptionList>"
        "   <Option name='SHIFT_ORIGIN_IN_MINUS_180_PLUS_180' type='boolean' "
        "description='Whether to apply a +/-360 deg shift to the longitude of "
        "the top left corner so that it is in the [-180,180] range' "
        "default='NO'/>"
        "</OpenOptionList>");

    poDriver->SetMetadataItem(GDAL_DMD_CREATIONDATATYPES, "Float32");

    poDriver->pfnOpen = GTXDataset::Open;
    poDriver->pfnIdentify = GTXDataset::Identify;
    poDriver->pfnCreate = GTXDataset::Create;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
