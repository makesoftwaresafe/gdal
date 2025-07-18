/******************************************************************************
 *
 * Project:  ECRG TOC read Translator
 * Purpose:  Implementation of ECRGTOCDataset and ECRGTOCSubDataset.
 * Author:   Even Rouault, even.rouault at spatialys.com
 *
 ******************************************************************************
 * Copyright (c) 2011, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_minixml.h"
#include "cpl_string.h"
#include "gdal.h"
#include "gdal_frmts.h"
#include "gdal_pam.h"
#include "gdal_priv.h"
#include "gdal_proxy.h"
#include "ogr_srs_api.h"
#include "vrtdataset.h"
#include "nitfdrivercore.h"

/** Overview of used classes :
   - ECRGTOCDataset : lists the different subdatasets, listed in the .xml,
                      as subdatasets
   - ECRGTOCSubDataset : one of these subdatasets, implemented as a VRT, of
                         the relevant NITF tiles
*/

namespace
{
typedef struct
{
    const char *pszName;
    const char *pszPath;
    int nScale;
    int nZone;
} FrameDesc;
}  // namespace

/************************************************************************/
/* ==================================================================== */
/*                            ECRGTOCDataset                            */
/* ==================================================================== */
/************************************************************************/

class ECRGTOCDataset final : public GDALPamDataset
{
    OGRSpatialReference m_oSRS{};
    char **papszSubDatasets = nullptr;
    GDALGeoTransform m_gt{};
    char **papszFileList = nullptr;

    CPL_DISALLOW_COPY_ASSIGN(ECRGTOCDataset)

  public:
    ECRGTOCDataset()
    {
        m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        m_oSRS.SetFromUserInput(SRS_WKT_WGS84_LAT_LONG);
    }

    virtual ~ECRGTOCDataset()
    {
        CSLDestroy(papszSubDatasets);
        CSLDestroy(papszFileList);
    }

    virtual char **GetMetadata(const char *pszDomain = "") override;

    virtual char **GetFileList() override
    {
        return CSLDuplicate(papszFileList);
    }

    void AddSubDataset(const char *pszFilename, const char *pszProductTitle,
                       const char *pszDiscId, const char *pszScale);

    virtual CPLErr GetGeoTransform(GDALGeoTransform &gt) const override
    {
        gt = m_gt;
        return CE_None;
    }

    const OGRSpatialReference *GetSpatialRef() const override
    {
        return &m_oSRS;
    }

    static GDALDataset *Build(const char *pszTOCFilename, CPLXMLNode *psXML,
                              const std::string &osProduct,
                              const std::string &osDiscId,
                              const std::string &osScale,
                              const char *pszFilename);

    static GDALDataset *Open(GDALOpenInfo *poOpenInfo);
};

/************************************************************************/
/* ==================================================================== */
/*                            ECRGTOCSubDataset                          */
/* ==================================================================== */
/************************************************************************/

class ECRGTOCSubDataset final : public VRTDataset
{
    char **papszFileList = nullptr;
    CPL_DISALLOW_COPY_ASSIGN(ECRGTOCSubDataset)

  public:
    ECRGTOCSubDataset(int nXSize, int nYSize) : VRTDataset(nXSize, nYSize)
    {
        /* Don't try to write a VRT file */
        SetWritable(FALSE);

        /* The driver is set to VRT in VRTDataset constructor. */
        /* We have to set it to the expected value ! */
        poDriver = GDALDriver::FromHandle(GDALGetDriverByName("ECRGTOC"));
    }

    ~ECRGTOCSubDataset() override;

    virtual char **GetFileList() override
    {
        return CSLDuplicate(papszFileList);
    }

    static GDALDataset *
    Build(const char *pszProductTitle, const char *pszDiscId, int nScale,
          int nCountSubDataset, const char *pszTOCFilename,
          const std::vector<FrameDesc> &aosFrameDesc, double dfGlobalMinX,
          double dfGlobalMinY, double dfGlobalMaxX, double dfGlobalMaxY,
          double dfGlobalPixelXSize, double dfGlobalPixelYSize);
};

ECRGTOCSubDataset::~ECRGTOCSubDataset()
{
    CSLDestroy(papszFileList);
}

/************************************************************************/
/*                           LaunderString()                            */
/************************************************************************/

static CPLString LaunderString(const char *pszStr)
{
    CPLString osRet(pszStr);
    for (size_t i = 0; i < osRet.size(); i++)
    {
        if (osRet[i] == ':' || osRet[i] == ' ')
            osRet[i] = '_';
    }
    return osRet;
}

/************************************************************************/
/*                           AddSubDataset()                            */
/************************************************************************/

void ECRGTOCDataset::AddSubDataset(const char *pszFilename,
                                   const char *pszProductTitle,
                                   const char *pszDiscId, const char *pszScale)

{
    char szName[80];
    const int nCount = CSLCount(papszSubDatasets) / 2;

    snprintf(szName, sizeof(szName), "SUBDATASET_%d_NAME", nCount + 1);
    papszSubDatasets = CSLSetNameValue(
        papszSubDatasets, szName,
        CPLSPrintf("ECRG_TOC_ENTRY:%s:%s:%s:%s",
                   LaunderString(pszProductTitle).c_str(),
                   LaunderString(pszDiscId).c_str(),
                   LaunderString(pszScale).c_str(), pszFilename));

    snprintf(szName, sizeof(szName), "SUBDATASET_%d_DESC", nCount + 1);
    papszSubDatasets =
        CSLSetNameValue(papszSubDatasets, szName,
                        CPLSPrintf("Product %s, disc %s, scale %s",
                                   pszProductTitle, pszDiscId, pszScale));
}

/************************************************************************/
/*                            GetMetadata()                             */
/************************************************************************/

char **ECRGTOCDataset::GetMetadata(const char *pszDomain)

{
    if (pszDomain != nullptr && EQUAL(pszDomain, "SUBDATASETS"))
        return papszSubDatasets;

    return GDALPamDataset::GetMetadata(pszDomain);
}

/************************************************************************/
/*                         GetScaleFromString()                         */
/************************************************************************/

static int GetScaleFromString(const char *pszScale)
{
    const char *pszPtr = strstr(pszScale, "1:");
    if (pszPtr)
        pszPtr = pszPtr + 2;
    else
        pszPtr = pszScale;

    int nScale = 0;
    char ch;
    while ((ch = *pszPtr) != '\0')
    {
        if (ch >= '0' && ch <= '9')
            nScale = nScale * 10 + ch - '0';
        else if (ch == ' ')
            ;
        else if (ch == 'k' || ch == 'K')
            return nScale * 1000;
        else if (ch == 'm' || ch == 'M')
            return nScale * 1000000;
        else
            return 0;
        pszPtr++;
    }
    return nScale;
}

/************************************************************************/
/*                            GetFromBase34()                           */
/************************************************************************/

static GIntBig GetFromBase34(const char *pszVal, int nMaxSize)
{
    GIntBig nFrameNumber = 0;
    for (int i = 0; i < nMaxSize; i++)
    {
        char ch = pszVal[i];
        if (ch == '\0')
            break;
        int chVal;
        if (ch >= 'A' && ch <= 'Z')
            ch += 'a' - 'A';
        /* i and o letters are excluded, */
        if (ch >= '0' && ch <= '9')
            chVal = ch - '0';
        else if (ch >= 'a' && ch <= 'h')
            chVal = ch - 'a' + 10;
        else if (ch >= 'j' && ch <= 'n')
            chVal = ch - 'a' + 10 - 1;
        else if (ch >= 'p' && ch <= 'z')
            chVal = ch - 'a' + 10 - 2;
        else
        {
            CPLDebug("ECRG", "Invalid base34 value : %s", pszVal);
            break;
        }
        nFrameNumber = nFrameNumber * 34 + chVal;
    }

    return nFrameNumber;
}

/************************************************************************/
/*                             GetExtent()                              */
/************************************************************************/

/* MIL-PRF-32283 - Table II. ECRG zone limits. */
/* starting with a fake zone 0 for convenience. */
constexpr int anZoneUpperLat[] = {0, 32, 48, 56, 64, 68, 72, 76, 80};

/* APPENDIX 70, TABLE III of MIL-A-89007 */
constexpr int anACst_ADRG[] = {369664, 302592, 245760, 199168,
                               163328, 137216, 110080, 82432};
constexpr int nBCst_ADRG = 400384;

// TODO: Why are these two functions done this way?
static int CEIL_ROUND(double a, double b)
{
    return static_cast<int>(ceil(a / b) * b);
}

static int NEAR_ROUND(double a, double b)
{
    return static_cast<int>(floor((a / b) + 0.5) * b);
}

constexpr int ECRG_PIXELS = 2304;

static void GetExtent(const char *pszFrameName, int nScale, int nZone,
                      double &dfMinX, double &dfMaxX, double &dfMinY,
                      double &dfMaxY, double &dfPixelXSize,
                      double &dfPixelYSize)
{
    const int nAbsZone = abs(nZone);
#ifdef DEBUG
    assert(nAbsZone > 0 && nAbsZone <= 8);
#endif

    /************************************************************************/
    /*  Compute east-west constant                                          */
    /************************************************************************/
    /* MIL-PRF-89038 - 60.1.2 - East-west pixel constant. */
    const int nEW_ADRG =
        CEIL_ROUND(anACst_ADRG[nAbsZone - 1] * (1e6 / nScale), 512);
    const int nEW_CADRG = NEAR_ROUND(nEW_ADRG / (150. / 100.), 256);
    /* MIL-PRF-32283 - D.2.1.2 - East-west pixel constant. */
    const int nEW = nEW_CADRG / 256 * 384;

    /************************************************************************/
    /*  Compute number of longitudinal frames                               */
    /************************************************************************/
    /* MIL-PRF-32283 - D.2.1.7 - Longitudinal frames and subframes */
    const int nCols =
        static_cast<int>(ceil(static_cast<double>(nEW) / ECRG_PIXELS));

    /************************************************************************/
    /*  Compute north-south constant                                        */
    /************************************************************************/
    /* MIL-PRF-89038 - 60.1.1 -  North-south. pixel constant */
    const int nNS_ADRG = CEIL_ROUND(nBCst_ADRG * (1e6 / nScale), 512) / 4;
    const int nNS_CADRG = NEAR_ROUND(nNS_ADRG / (150. / 100.), 256);
    /* MIL-PRF-32283 - D.2.1.1 - North-south. pixel constant and Frame
     * Width/Height */
    const int nNS = nNS_CADRG / 256 * 384;

    /************************************************************************/
    /*  Compute number of latitudinal frames and latitude of top of zone    */
    /************************************************************************/
    dfPixelYSize = 90.0 / nNS;

    const double dfFrameLatHeight = dfPixelYSize * ECRG_PIXELS;

    /* MIL-PRF-32283 - D.2.1.5 - Equatorward and poleward zone extents. */
    int nUpperZoneFrames =
        static_cast<int>(ceil(anZoneUpperLat[nAbsZone] / dfFrameLatHeight));
    int nBottomZoneFrames = static_cast<int>(
        floor(anZoneUpperLat[nAbsZone - 1] / dfFrameLatHeight));
    const int nRows = nUpperZoneFrames - nBottomZoneFrames;

    /* Not sure to really understand D.2.1.5.a. Testing needed */
    if (nZone < 0)
    {
        nUpperZoneFrames = -nBottomZoneFrames;
        /*nBottomZoneFrames = nUpperZoneFrames - nRows;*/
    }

    const double dfUpperZoneTopLat = dfFrameLatHeight * nUpperZoneFrames;

    /************************************************************************/
    /*  Compute coordinates of the frame in the zone                        */
    /************************************************************************/

    /* Converts the first 10 characters into a number from base 34 */
    const GIntBig nFrameNumber = GetFromBase34(pszFrameName, 10);

    /*  MIL-PRF-32283 - A.2.6.1 */
    const GIntBig nY = nFrameNumber / nCols;
    const GIntBig nX = nFrameNumber % nCols;

    /************************************************************************/
    /*  Compute extent of the frame                                         */
    /************************************************************************/

    /* The nY is counted from the bottom of the zone... Pfff */
    dfMaxY = dfUpperZoneTopLat - (nRows - 1 - nY) * dfFrameLatHeight;
    dfMinY = dfMaxY - dfFrameLatHeight;

    dfPixelXSize = 360.0 / nEW;

    const double dfFrameLongWidth = dfPixelXSize * ECRG_PIXELS;
    dfMinX = -180.0 + nX * dfFrameLongWidth;
    dfMaxX = dfMinX + dfFrameLongWidth;

#ifdef DEBUG_VERBOSE
    CPLDebug("ECRG",
             "Frame %s : minx=%.16g, maxy=%.16g, maxx=%.16g, miny=%.16g",
             pszFrameName, dfMinX, dfMaxY, dfMaxX, dfMinY);
#endif
}

/************************************************************************/
/*                          ECRGTOCSource                               */
/************************************************************************/

class ECRGTOCSource final : public VRTSimpleSource
{
    int m_nRasterXSize = 0;
    int m_nRasterYSize = 0;
    double m_dfMinX = 0;
    double m_dfMaxY = 0;
    double m_dfPixelXSize = 0;
    double m_dfPixelYSize = 0;

    bool ValidateOpenedBand(GDALRasterBand *) const override;

  public:
    ECRGTOCSource(const char *pszFilename, int nBandIn, int nRasterXSize,
                  int nRasterYSize, double dfDstXOff, double dfDstYOff,
                  double dfDstXSize, double dfDstYSize, double dfMinX,
                  double dfMaxY, double dfPixelXSize, double dfPixelYSize)
        : m_nRasterXSize(nRasterXSize), m_nRasterYSize(nRasterYSize),
          m_dfMinX(dfMinX), m_dfMaxY(dfMaxY), m_dfPixelXSize(dfPixelXSize),
          m_dfPixelYSize(dfPixelYSize)
    {
        SetSrcBand(pszFilename, nBandIn);
        SetSrcWindow(0, 0, nRasterXSize, nRasterYSize);
        SetDstWindow(dfDstXOff, dfDstYOff, dfDstXSize, dfDstYSize);
    }
};

/************************************************************************/
/*                       ValidateOpenedBand()                           */
/************************************************************************/

#define WARN_CHECK_DS(x)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(x))                                                              \
        {                                                                      \
            CPLError(CE_Warning, CPLE_AppDefined,                              \
                     "For %s, assert '" #x "' failed",                         \
                     poSourceDS->GetDescription());                            \
            checkOK = false;                                                   \
        }                                                                      \
    } while (false)

bool ECRGTOCSource::ValidateOpenedBand(GDALRasterBand *poBand) const
{
    bool checkOK = true;
    auto poSourceDS = poBand->GetDataset();
    CPLAssert(poSourceDS);

    GDALGeoTransform l_gt;
    poSourceDS->GetGeoTransform(l_gt);
    WARN_CHECK_DS(fabs(l_gt[0] - m_dfMinX) < 1e-10);
    WARN_CHECK_DS(fabs(l_gt[3] - m_dfMaxY) < 1e-10);
    WARN_CHECK_DS(fabs(l_gt[1] - m_dfPixelXSize) < 1e-10);
    WARN_CHECK_DS(fabs(l_gt[5] - (-m_dfPixelYSize)) < 1e-10);
    WARN_CHECK_DS(l_gt[2] == 0 && l_gt[4] == 0);  // No rotation.
    WARN_CHECK_DS(poSourceDS->GetRasterCount() == 3);
    WARN_CHECK_DS(poSourceDS->GetRasterXSize() == m_nRasterXSize);
    WARN_CHECK_DS(poSourceDS->GetRasterYSize() == m_nRasterYSize);
    WARN_CHECK_DS(
        EQUAL(poSourceDS->GetProjectionRef(), SRS_WKT_WGS84_LAT_LONG));
    WARN_CHECK_DS(poSourceDS->GetRasterBand(1)->GetRasterDataType() ==
                  GDT_Byte);
    return checkOK;
}

/************************************************************************/
/*                           BuildFullName()                            */
/************************************************************************/

static std::string BuildFullName(const char *pszTOCFilename,
                                 const char *pszFramePath,
                                 const char *pszFrameName)
{
    char *pszPath = nullptr;
    if (pszFramePath[0] == '.' &&
        (pszFramePath[1] == '/' || pszFramePath[1] == '\\'))
        pszPath = CPLStrdup(pszFramePath + 2);
    else
        pszPath = CPLStrdup(pszFramePath);
    for (int i = 0; pszPath[i] != '\0'; i++)
    {
        if (pszPath[i] == '\\')
            pszPath[i] = '/';
    }
    const std::string osName =
        CPLFormFilenameSafe(pszPath, pszFrameName, nullptr);
    CPLFree(pszPath);
    pszPath = nullptr;
    std::string osTOCPath = CPLGetDirnameSafe(pszTOCFilename);
    const auto nPosFirstSlashInName = osName.find('/');
    if (nPosFirstSlashInName != std::string::npos)
    {
        if (osTOCPath.size() >= nPosFirstSlashInName + 1 &&
            (osTOCPath[osTOCPath.size() - (nPosFirstSlashInName + 1)] == '/' ||
             osTOCPath[osTOCPath.size() - (nPosFirstSlashInName + 1)] ==
                 '\\') &&
            strncmp(osTOCPath.c_str() + osTOCPath.size() - nPosFirstSlashInName,
                    osName.c_str(), nPosFirstSlashInName) == 0)
        {
            osTOCPath = CPLGetDirnameSafe(osTOCPath.c_str());
        }
    }
    return CPLProjectRelativeFilenameSafe(osTOCPath.c_str(), osName.c_str());
}

/************************************************************************/
/*                              Build()                                 */
/************************************************************************/

/* Builds a ECRGTOCSubDataset from the set of files of the toc entry */
GDALDataset *ECRGTOCSubDataset::Build(
    const char *pszProductTitle, const char *pszDiscId, int nScale,
    int nCountSubDataset, const char *pszTOCFilename,
    const std::vector<FrameDesc> &aosFrameDesc, double dfGlobalMinX,
    double dfGlobalMinY, double dfGlobalMaxX, double dfGlobalMaxY,
    double dfGlobalPixelXSize, double dfGlobalPixelYSize)
{
    GDALDriver *poDriver = GetGDALDriverManager()->GetDriverByName("VRT");
    if (poDriver == nullptr)
        return nullptr;

    const int nSizeX = static_cast<int>(
        (dfGlobalMaxX - dfGlobalMinX) / dfGlobalPixelXSize + 0.5);
    const int nSizeY = static_cast<int>(
        (dfGlobalMaxY - dfGlobalMinY) / dfGlobalPixelYSize + 0.5);

    /* ------------------------------------ */
    /* Create the VRT with the overall size */
    /* ------------------------------------ */
    ECRGTOCSubDataset *poVirtualDS = new ECRGTOCSubDataset(nSizeX, nSizeY);

    poVirtualDS->SetProjection(SRS_WKT_WGS84_LAT_LONG);

    GDALGeoTransform gt{
        dfGlobalMinX,       dfGlobalPixelXSize, 0, dfGlobalMaxY, 0,
        -dfGlobalPixelYSize};
    poVirtualDS->SetGeoTransform(gt);

    for (int i = 0; i < 3; i++)
    {
        poVirtualDS->AddBand(GDT_Byte, nullptr);
        GDALRasterBand *poBand = poVirtualDS->GetRasterBand(i + 1);
        poBand->SetColorInterpretation(
            static_cast<GDALColorInterp>(GCI_RedBand + i));
    }

    poVirtualDS->SetDescription(pszTOCFilename);

    poVirtualDS->SetMetadataItem("PRODUCT_TITLE", pszProductTitle);
    poVirtualDS->SetMetadataItem("DISC_ID", pszDiscId);
    if (nScale != -1)
        poVirtualDS->SetMetadataItem("SCALE", CPLString().Printf("%d", nScale));

    /* -------------------------------------------------------------------- */
    /*      Check for overviews.                                            */
    /* -------------------------------------------------------------------- */

    poVirtualDS->oOvManager.Initialize(
        poVirtualDS,
        CPLString().Printf("%s.%d", pszTOCFilename, nCountSubDataset));

    poVirtualDS->papszFileList = poVirtualDS->GDALDataset::GetFileList();

    // Rather hacky... Force GDAL_FORCE_CACHING=NO so that the
    // GDALProxyPoolRasterBand do not use the GDALRasterBand::IRasterIO()
    // default implementation, which would rely on the block size of
    // GDALProxyPoolRasterBand, which we don't know...
    CPLConfigOptionSetter oSetter("GDAL_FORCE_CACHING", "NO", false);

    for (int i = 0; i < static_cast<int>(aosFrameDesc.size()); i++)
    {
        const std::string osName = BuildFullName(
            pszTOCFilename, aosFrameDesc[i].pszPath, aosFrameDesc[i].pszName);

        double dfMinX = 0.0;
        double dfMaxX = 0.0;
        double dfMinY = 0.0;
        double dfMaxY = 0.0;
        double dfPixelXSize = 0.0;
        double dfPixelYSize = 0.0;
        ::GetExtent(aosFrameDesc[i].pszName, aosFrameDesc[i].nScale,
                    aosFrameDesc[i].nZone, dfMinX, dfMaxX, dfMinY, dfMaxY,
                    dfPixelXSize, dfPixelYSize);

        const int nFrameXSize =
            static_cast<int>((dfMaxX - dfMinX) / dfPixelXSize + 0.5);
        const int nFrameYSize =
            static_cast<int>((dfMaxY - dfMinY) / dfPixelYSize + 0.5);

        poVirtualDS->papszFileList =
            CSLAddString(poVirtualDS->papszFileList, osName.c_str());

        for (int j = 0; j < 3; j++)
        {
            VRTSourcedRasterBand *poBand =
                cpl::down_cast<VRTSourcedRasterBand *>(
                    poVirtualDS->GetRasterBand(j + 1));
            /* Place the raster band at the right position in the VRT */
            auto poSource = new ECRGTOCSource(
                osName.c_str(), j + 1, nFrameXSize, nFrameYSize,
                static_cast<int>((dfMinX - dfGlobalMinX) / dfGlobalPixelXSize +
                                 0.5),
                static_cast<int>((dfGlobalMaxY - dfMaxY) / dfGlobalPixelYSize +
                                 0.5),
                static_cast<int>((dfMaxX - dfMinX) / dfGlobalPixelXSize + 0.5),
                static_cast<int>((dfMaxY - dfMinY) / dfGlobalPixelYSize + 0.5),
                dfMinX, dfMaxY, dfPixelXSize, dfPixelYSize);
            poBand->AddSource(poSource);
        }
    }

    poVirtualDS->SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");

    return poVirtualDS;
}

/************************************************************************/
/*                             Build()                                  */
/************************************************************************/

GDALDataset *ECRGTOCDataset::Build(const char *pszTOCFilename,
                                   CPLXMLNode *psXML,
                                   const std::string &osProduct,
                                   const std::string &osDiscId,
                                   const std::string &osScale,
                                   const char *pszOpenInfoFilename)
{
    CPLXMLNode *psTOC = CPLGetXMLNode(psXML, "=Table_of_Contents");
    if (psTOC == nullptr)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Cannot find Table_of_Contents element");
        return nullptr;
    }

    double dfGlobalMinX = 0.0;
    double dfGlobalMinY = 0.0;
    double dfGlobalMaxX = 0.0;
    double dfGlobalMaxY = 0.0;
    double dfGlobalPixelXSize = 0.0;
    double dfGlobalPixelYSize = 0.0;
    bool bGlobalExtentValid = false;

    ECRGTOCDataset *poDS = new ECRGTOCDataset();
    int nSubDatasets = 0;

    int bLookForSubDataset = !osProduct.empty() && !osDiscId.empty();

    int nCountSubDataset = 0;

    poDS->SetDescription(pszOpenInfoFilename);
    poDS->papszFileList = poDS->GDALDataset::GetFileList();

    for (CPLXMLNode *psIter1 = psTOC->psChild; psIter1 != nullptr;
         psIter1 = psIter1->psNext)
    {
        if (!(psIter1->eType == CXT_Element && psIter1->pszValue != nullptr &&
              strcmp(psIter1->pszValue, "product") == 0))
            continue;

        const char *pszProductTitle =
            CPLGetXMLValue(psIter1, "product_title", nullptr);
        if (pszProductTitle == nullptr)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Cannot find product_title attribute");
            continue;
        }

        if (bLookForSubDataset &&
            strcmp(LaunderString(pszProductTitle), osProduct.c_str()) != 0)
            continue;

        for (CPLXMLNode *psIter2 = psIter1->psChild; psIter2 != nullptr;
             psIter2 = psIter2->psNext)
        {
            if (!(psIter2->eType == CXT_Element &&
                  psIter2->pszValue != nullptr &&
                  strcmp(psIter2->pszValue, "disc") == 0))
                continue;

            const char *pszDiscId = CPLGetXMLValue(psIter2, "id", nullptr);
            if (pszDiscId == nullptr)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Cannot find id attribute");
                continue;
            }

            if (bLookForSubDataset &&
                strcmp(LaunderString(pszDiscId), osDiscId.c_str()) != 0)
                continue;

            CPLXMLNode *psFrameList = CPLGetXMLNode(psIter2, "frame_list");
            if (psFrameList == nullptr)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Cannot find frame_list element");
                continue;
            }

            for (CPLXMLNode *psIter3 = psFrameList->psChild; psIter3 != nullptr;
                 psIter3 = psIter3->psNext)
            {
                if (!(psIter3->eType == CXT_Element &&
                      psIter3->pszValue != nullptr &&
                      strcmp(psIter3->pszValue, "scale") == 0))
                    continue;

                const char *pszSize = CPLGetXMLValue(psIter3, "size", nullptr);
                if (pszSize == nullptr)
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Cannot find size attribute");
                    continue;
                }

                int nScale = GetScaleFromString(pszSize);
                if (nScale <= 0)
                {
                    CPLError(CE_Warning, CPLE_AppDefined, "Invalid scale %s",
                             pszSize);
                    continue;
                }

                if (bLookForSubDataset)
                {
                    if (!osScale.empty())
                    {
                        if (strcmp(LaunderString(pszSize), osScale.c_str()) !=
                            0)
                        {
                            continue;
                        }
                    }
                    else
                    {
                        int nCountScales = 0;
                        for (CPLXMLNode *psIter4 = psFrameList->psChild;
                             psIter4 != nullptr; psIter4 = psIter4->psNext)
                        {
                            if (!(psIter4->eType == CXT_Element &&
                                  psIter4->pszValue != nullptr &&
                                  strcmp(psIter4->pszValue, "scale") == 0))
                                continue;
                            nCountScales++;
                        }
                        if (nCountScales > 1)
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "Scale should be mentioned in "
                                     "subdatasets syntax since this disk "
                                     "contains several scales");
                            delete poDS;
                            return nullptr;
                        }
                    }
                }

                nCountSubDataset++;

                std::vector<FrameDesc> aosFrameDesc;
                int nValidFrames = 0;

                for (CPLXMLNode *psIter4 = psIter3->psChild; psIter4 != nullptr;
                     psIter4 = psIter4->psNext)
                {
                    if (!(psIter4->eType == CXT_Element &&
                          psIter4->pszValue != nullptr &&
                          strcmp(psIter4->pszValue, "frame") == 0))
                        continue;

                    const char *pszFrameName =
                        CPLGetXMLValue(psIter4, "name", nullptr);
                    if (pszFrameName == nullptr)
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "Cannot find name element");
                        continue;
                    }

                    if (strlen(pszFrameName) != 18)
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "Invalid value for name element : %s",
                                 pszFrameName);
                        continue;
                    }

                    const char *pszFramePath =
                        CPLGetXMLValue(psIter4, "frame_path", nullptr);
                    if (pszFramePath == nullptr)
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "Cannot find frame_path element");
                        continue;
                    }

                    const char *pszFrameZone =
                        CPLGetXMLValue(psIter4, "frame_zone", nullptr);
                    if (pszFrameZone == nullptr)
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "Cannot find frame_zone element");
                        continue;
                    }
                    if (strlen(pszFrameZone) != 1)
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "Invalid value for frame_zone element : %s",
                                 pszFrameZone);
                        continue;
                    }
                    char chZone = pszFrameZone[0];
                    int nZone = 0;
                    if (chZone >= '1' && chZone <= '9')
                        nZone = chZone - '0';
                    else if (chZone >= 'a' && chZone <= 'h')
                        nZone = -(chZone - 'a' + 1);
                    else if (chZone >= 'A' && chZone <= 'H')
                        nZone = -(chZone - 'A' + 1);
                    else if (chZone == 'j' || chZone == 'J')
                        nZone = -9;
                    else
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "Invalid value for frame_zone element : %s",
                                 pszFrameZone);
                        continue;
                    }
                    if (nZone == 9 || nZone == -9)
                    {
                        CPLError(
                            CE_Warning, CPLE_AppDefined,
                            "Polar zones unhandled by current implementation");
                        continue;
                    }

                    double dfMinX = 0.0;
                    double dfMaxX = 0.0;
                    double dfMinY = 0.0;
                    double dfMaxY = 0.0;
                    double dfPixelXSize = 0.0;
                    double dfPixelYSize = 0.0;
                    ::GetExtent(pszFrameName, nScale, nZone, dfMinX, dfMaxX,
                                dfMinY, dfMaxY, dfPixelXSize, dfPixelYSize);

                    nValidFrames++;

                    const std::string osFullName = BuildFullName(
                        pszTOCFilename, pszFramePath, pszFrameName);
                    poDS->papszFileList =
                        CSLAddString(poDS->papszFileList, osFullName.c_str());

                    if (!bGlobalExtentValid)
                    {
                        dfGlobalMinX = dfMinX;
                        dfGlobalMinY = dfMinY;
                        dfGlobalMaxX = dfMaxX;
                        dfGlobalMaxY = dfMaxY;
                        dfGlobalPixelXSize = dfPixelXSize;
                        dfGlobalPixelYSize = dfPixelYSize;
                        bGlobalExtentValid = true;
                    }
                    else
                    {
                        if (dfMinX < dfGlobalMinX)
                            dfGlobalMinX = dfMinX;
                        if (dfMinY < dfGlobalMinY)
                            dfGlobalMinY = dfMinY;
                        if (dfMaxX > dfGlobalMaxX)
                            dfGlobalMaxX = dfMaxX;
                        if (dfMaxY > dfGlobalMaxY)
                            dfGlobalMaxY = dfMaxY;
                        if (dfPixelXSize < dfGlobalPixelXSize)
                            dfGlobalPixelXSize = dfPixelXSize;
                        if (dfPixelYSize < dfGlobalPixelYSize)
                            dfGlobalPixelYSize = dfPixelYSize;
                    }

                    nValidFrames++;

                    if (bLookForSubDataset)
                    {
                        FrameDesc frameDesc;
                        frameDesc.pszName = pszFrameName;
                        frameDesc.pszPath = pszFramePath;
                        frameDesc.nScale = nScale;
                        frameDesc.nZone = nZone;
                        aosFrameDesc.push_back(frameDesc);
                    }
                }

                if (bLookForSubDataset)
                {
                    delete poDS;
                    if (nValidFrames == 0)
                        return nullptr;
                    return ECRGTOCSubDataset::Build(
                        pszProductTitle, pszDiscId, nScale, nCountSubDataset,
                        pszTOCFilename, aosFrameDesc, dfGlobalMinX,
                        dfGlobalMinY, dfGlobalMaxX, dfGlobalMaxY,
                        dfGlobalPixelXSize, dfGlobalPixelYSize);
                }

                if (nValidFrames)
                {
                    poDS->AddSubDataset(pszOpenInfoFilename, pszProductTitle,
                                        pszDiscId, pszSize);
                    nSubDatasets++;
                }
            }
        }
    }

    if (!bGlobalExtentValid)
    {
        delete poDS;
        return nullptr;
    }

    if (nSubDatasets == 1)
    {
        const char *pszSubDatasetName = CSLFetchNameValue(
            poDS->GetMetadata("SUBDATASETS"), "SUBDATASET_1_NAME");
        GDALOpenInfo oOpenInfo(pszSubDatasetName, GA_ReadOnly);
        delete poDS;
        GDALDataset *poRetDS = Open(&oOpenInfo);
        if (poRetDS)
            poRetDS->SetDescription(pszOpenInfoFilename);
        return poRetDS;
    }

    poDS->m_gt[0] = dfGlobalMinX;
    poDS->m_gt[1] = dfGlobalPixelXSize;
    poDS->m_gt[2] = 0.0;
    poDS->m_gt[3] = dfGlobalMaxY;
    poDS->m_gt[4] = 0.0;
    poDS->m_gt[5] = -dfGlobalPixelYSize;

    poDS->nRasterXSize = static_cast<int>(0.5 + (dfGlobalMaxX - dfGlobalMinX) /
                                                    dfGlobalPixelXSize);
    poDS->nRasterYSize = static_cast<int>(0.5 + (dfGlobalMaxY - dfGlobalMinY) /
                                                    dfGlobalPixelYSize);

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->TryLoadXML();

    return poDS;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *ECRGTOCDataset::Open(GDALOpenInfo *poOpenInfo)

{
    if (!ECRGTOCDriverIdentify(poOpenInfo))
        return nullptr;

    const char *pszFilename = poOpenInfo->pszFilename;
    CPLString osFilename;
    CPLString osProduct, osDiscId, osScale;

    if (STARTS_WITH_CI(pszFilename, "ECRG_TOC_ENTRY:"))
    {
        pszFilename += strlen("ECRG_TOC_ENTRY:");

        /* PRODUCT:DISK:SCALE:FILENAME (or PRODUCT:DISK:FILENAME historically)
         */
        /* with FILENAME potentially C:\BLA... */
        char **papszTokens = CSLTokenizeString2(pszFilename, ":", 0);
        int nTokens = CSLCount(papszTokens);
        if (nTokens != 3 && nTokens != 4 && nTokens != 5)
        {
            CSLDestroy(papszTokens);
            return nullptr;
        }

        osProduct = papszTokens[0];
        osDiscId = papszTokens[1];

        if (nTokens == 3)
            osFilename = papszTokens[2];
        else if (nTokens == 4)
        {
            if (strlen(papszTokens[2]) == 1 &&
                (papszTokens[3][0] == '\\' || papszTokens[3][0] == '/'))
            {
                osFilename = papszTokens[2];
                osFilename += ":";
                osFilename += papszTokens[3];
            }
            else
            {
                osScale = papszTokens[2];
                osFilename = papszTokens[3];
            }
        }
        else if (nTokens == 5 && strlen(papszTokens[3]) == 1 &&
                 (papszTokens[4][0] == '\\' || papszTokens[4][0] == '/'))
        {
            osScale = papszTokens[2];
            osFilename = papszTokens[3];
            osFilename += ":";
            osFilename += papszTokens[4];
        }
        else
        {
            CSLDestroy(papszTokens);
            return nullptr;
        }

        CSLDestroy(papszTokens);
        pszFilename = osFilename.c_str();
    }

    /* -------------------------------------------------------------------- */
    /*      Parse the XML file                                              */
    /* -------------------------------------------------------------------- */
    CPLXMLNode *psXML = CPLParseXMLFile(pszFilename);
    if (psXML == nullptr)
    {
        return nullptr;
    }

    GDALDataset *poDS = Build(pszFilename, psXML, osProduct, osDiscId, osScale,
                              poOpenInfo->pszFilename);
    CPLDestroyXMLNode(psXML);

    if (poDS && poOpenInfo->eAccess == GA_Update)
    {
        ReportUpdateNotSupportedByDriver("ECRGTOC");
        delete poDS;
        return nullptr;
    }

    return poDS;
}

/************************************************************************/
/*                         GDALRegister_ECRGTOC()                       */
/************************************************************************/

void GDALRegister_ECRGTOC()

{
    if (GDALGetDriverByName(ECRGTOC_DRIVER_NAME) != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();
    ECRGTOCDriverSetCommonMetadata(poDriver);

    poDriver->pfnOpen = ECRGTOCDataset::Open;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
