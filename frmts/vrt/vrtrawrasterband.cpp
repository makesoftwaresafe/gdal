/******************************************************************************
 *
 * Project:  Virtual GDAL Datasets
 * Purpose:  Implementation of VRTRawRasterBand
 * Author:   Frank Warmerdam <warmerdam@pobox.com>
 *
 ******************************************************************************
 * Copyright (c) 2004, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2007-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "rawdataset.h"
#include "vrtdataset.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_hash_set.h"
#include "cpl_minixml.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "gdal.h"
#include "gdal_priv.h"

/*! @cond Doxygen_Suppress */

/************************************************************************/
/* ==================================================================== */
/*                          VRTRawRasterBand                            */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                          VRTRawRasterBand()                          */
/************************************************************************/

VRTRawRasterBand::VRTRawRasterBand(GDALDataset *poDSIn, int nBandIn,
                                   GDALDataType eType)
    : m_poRawRaster(nullptr), m_pszSourceFilename(nullptr),
      m_bRelativeToVRT(FALSE)
{
    if (!VRTDataset::IsRawRasterBandEnabled())
    {
        // Safety belt. Not supposed to happen, hence CE_Fatal
        CPLError(CE_Fatal, CPLE_NotSupported,
                 "Crashing process: VRTRawRasterBand constructor called "
                 "whereas not authorized");
        return;
    }

    Initialize(poDSIn->GetRasterXSize(), poDSIn->GetRasterYSize());

    // Declared in GDALRasterBand.
    poDS = poDSIn;
    nBand = nBandIn;

    if (eType != GDT_Unknown)
        eDataType = eType;
}

/************************************************************************/
/*                         ~VRTRawRasterBand()                          */
/************************************************************************/

VRTRawRasterBand::~VRTRawRasterBand()

{
    FlushCache(true);
    ClearRawLink();
}

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr VRTRawRasterBand::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                                   int nXSize, int nYSize, void *pData,
                                   int nBufXSize, int nBufYSize,
                                   GDALDataType eBufType, GSpacing nPixelSpace,
                                   GSpacing nLineSpace,
                                   GDALRasterIOExtraArg *psExtraArg)
{
    if (m_poRawRaster == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "No raw raster band configured on VRTRawRasterBand.");
        return CE_Failure;
    }

    if (eRWFlag == GF_Write && eAccess == GA_ReadOnly)
    {
        CPLError(CE_Failure, CPLE_NoWriteAccess,
                 "Attempt to write to read only dataset in"
                 "VRTRawRasterBand::IRasterIO().");

        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Do we have overviews that would be appropriate to satisfy       */
    /*      this request?                                                   */
    /* -------------------------------------------------------------------- */
    if ((nBufXSize < nXSize || nBufYSize < nYSize) && GetOverviewCount() > 0)
    {
        if (OverviewRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData,
                             nBufXSize, nBufYSize, eBufType, nPixelSpace,
                             nLineSpace, psExtraArg) == CE_None)
            return CE_None;
    }

    m_poRawRaster->SetAccess(eAccess);

    return m_poRawRaster->RasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData,
                                   nBufXSize, nBufYSize, eBufType, nPixelSpace,
                                   nLineSpace, psExtraArg);
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr VRTRawRasterBand::IReadBlock(int nBlockXOff, int nBlockYOff,
                                    void *pImage)

{
    if (m_poRawRaster == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "No raw raster band configured on VRTRawRasterBand.");
        return CE_Failure;
    }

    return m_poRawRaster->ReadBlock(nBlockXOff, nBlockYOff, pImage);
}

/************************************************************************/
/*                            IWriteBlock()                             */
/************************************************************************/

CPLErr VRTRawRasterBand::IWriteBlock(int nBlockXOff, int nBlockYOff,
                                     void *pImage)

{
    if (m_poRawRaster == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "No raw raster band configured on VRTRawRasterBand.");
        return CE_Failure;
    }

    m_poRawRaster->SetAccess(eAccess);

    return m_poRawRaster->WriteBlock(nBlockXOff, nBlockYOff, pImage);
}

/************************************************************************/
/*                             SetRawLink()                             */
/************************************************************************/

CPLErr VRTRawRasterBand::SetRawLink(const char *pszFilename,
                                    const char *pszVRTPath,
                                    int bRelativeToVRTIn,
                                    vsi_l_offset nImageOffset, int nPixelOffset,
                                    int nLineOffset, const char *pszByteOrder)

{
    ClearRawLink();

    static_cast<VRTDataset *>(poDS)->SetNeedsFlush();

    /* -------------------------------------------------------------------- */
    /*      Prepare filename.                                               */
    /* -------------------------------------------------------------------- */
    if (pszFilename == nullptr)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Missing <SourceFilename> element in VRTRasterBand.");
        return CE_Failure;
    }

    const std::string osExpandedFilename =
        (pszVRTPath && bRelativeToVRTIn)
            ? CPLProjectRelativeFilenameSafe(pszVRTPath, pszFilename)
            : pszFilename;

    const char *pszAllowedPaths =
        CPLGetConfigOption("GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE", nullptr);
    if (pszAllowedPaths == nullptr ||
        EQUAL(pszAllowedPaths, "SIBLING_OR_CHILD_OF_VRT_PATH"))
    {
        const char *pszErrorMsgPart =
            pszAllowedPaths
                ? "GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE=SIBLING_OR_CHILD_OF_"
                  "VRT_PATH"
                : "the GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE configuration "
                  "option is not set (and thus defaults to "
                  "SIBLING_OR_CHILD_OF_VRT_PATH. Consult "
                  "https://gdal.org/drivers/raster/"
                  "vrt.html#vrtrawrasterband_restricted_access for more "
                  "details)";
        if (!bRelativeToVRTIn)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "'%s' is invalid because the relativeToVRT flag is not "
                     "set and %s",
                     pszFilename, pszErrorMsgPart);
            return CE_Failure;
        }
        if (!CPLIsFilenameRelative(pszFilename))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "'%s' is invalid because it is not relative to the VRT "
                     "path and %s",
                     pszFilename, pszErrorMsgPart);
            return CE_Failure;
        }
        if (strstr(pszFilename, "../") || strstr(pszFilename, "..\\"))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "'%s' is invalid because it may not be a sibling or "
                     "child of the VRT path and %s",
                     pszFilename, pszErrorMsgPart);
            return CE_Failure;
        }
    }
    else if (EQUAL(pszAllowedPaths, "ALL"))
    {
        // ok
    }
    else if (EQUAL(pszAllowedPaths, "ONLY_REMOTE"))
    {
        if (VSIIsLocal(pszFilename))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "'%s' is a local file, whereas "
                     "GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE=ONLY_REMOTE is set",
                     pszFilename);
            return CE_Failure;
        }
    }
    else
    {
        if (strstr(pszFilename, "../") || strstr(pszFilename, "..\\"))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "'%s' is invalid because the presence of ../ in it may "
                     "escape from the allowed path(s)",
                     pszFilename);
            return CE_Failure;
        }
#ifdef _WIN32
        constexpr const char *pszSep = ";";
#else
        constexpr const char *pszSep = ":";
#endif
        bool bOK = false;
        const CPLStringList aosPaths(
            CSLTokenizeString2(pszAllowedPaths, pszSep, 0));
        for (const char *pszPath : aosPaths)
        {
            if (CPLIsFilenameRelative(pszPath))
            {
                CPLError(
                    CE_Failure, CPLE_AppDefined,
                    "Invalid value for GDAL_VRT_RAWRASTERBAND_ALLOWED_SOURCE. "
                    "'%s' is not an absolute path",
                    pszPath);
                return CE_Failure;
            }
            if (STARTS_WITH(osExpandedFilename.c_str(), pszPath))
            {
                bOK = true;
                break;
            }
        }
        if (!bOK)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "'%s' is invalid because it is not contained in one of "
                     "the allowed path(s)",
                     pszFilename);
            return CE_Failure;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Try and open the file.  We always use the large file API.       */
    /* -------------------------------------------------------------------- */
    CPLPushErrorHandler(CPLQuietErrorHandler);
    FILE *fp = CPLOpenShared(osExpandedFilename.c_str(), "rb+", TRUE);

    if (fp == nullptr)
        fp = CPLOpenShared(osExpandedFilename.c_str(), "rb", TRUE);

    if (fp == nullptr &&
        static_cast<VRTDataset *>(poDS)->GetAccess() == GA_Update)
    {
        fp = CPLOpenShared(osExpandedFilename.c_str(), "wb+", TRUE);
    }
    CPLPopErrorHandler();
    CPLErrorReset();

    if (fp == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "Unable to open %s.%s",
                 osExpandedFilename.c_str(), VSIStrerror(errno));

        return CE_Failure;
    }

    if (!RAWDatasetCheckMemoryUsage(
            nRasterXSize, nRasterYSize, 1,
            GDALGetDataTypeSizeBytes(GetRasterDataType()), nPixelOffset,
            nLineOffset, nImageOffset, 0, reinterpret_cast<VSILFILE *>(fp)))
    {
        CPLCloseShared(fp);
        return CE_Failure;
    }

    m_pszSourceFilename = CPLStrdup(pszFilename);
    m_bRelativeToVRT = bRelativeToVRTIn;

    /* -------------------------------------------------------------------- */
    /*      Work out if we are in native mode or not.                       */
    /* -------------------------------------------------------------------- */
    RawRasterBand::ByteOrder eByteOrder =
#if CPL_IS_LSB
        RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN;
#else
        RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN;
#endif

    if (pszByteOrder != nullptr)
    {
        if (EQUAL(pszByteOrder, "LSB"))
            eByteOrder = RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN;
        else if (EQUAL(pszByteOrder, "MSB"))
            eByteOrder = RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN;
        else if (EQUAL(pszByteOrder, "VAX"))
            eByteOrder = RawRasterBand::ByteOrder::ORDER_VAX;
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Illegal ByteOrder value '%s', should be LSB, MSB or VAX.",
                     pszByteOrder);
            CPLCloseShared(fp);
            return CE_Failure;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Create a corresponding RawRasterBand.                           */
    /* -------------------------------------------------------------------- */
    m_poRawRaster =
        RawRasterBand::Create(reinterpret_cast<VSILFILE *>(fp), nImageOffset,
                              nPixelOffset, nLineOffset, GetRasterDataType(),
                              eByteOrder, GetXSize(), GetYSize(),
                              RawRasterBand::OwnFP::NO)
            .release();
    if (!m_poRawRaster)
    {
        CPLCloseShared(fp);
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Reset block size to match the raw raster.                       */
    /* -------------------------------------------------------------------- */
    m_poRawRaster->GetBlockSize(&nBlockXSize, &nBlockYSize);

    return CE_None;
}

/************************************************************************/
/*                            ClearRawLink()                            */
/************************************************************************/

void VRTRawRasterBand::ClearRawLink()

{
    if (m_poRawRaster != nullptr)
    {
        VSILFILE *fp = m_poRawRaster->GetFPL();
        delete m_poRawRaster;
        m_poRawRaster = nullptr;
        // We close the file after deleting the raster band
        // since data can be flushed in the destructor.
        if (fp != nullptr)
        {
            CPLCloseShared(reinterpret_cast<FILE *>(fp));
        }
    }
    CPLFree(m_pszSourceFilename);
    m_pszSourceFilename = nullptr;
}

/************************************************************************/
/*                            GetVirtualMemAuto()                       */
/************************************************************************/

CPLVirtualMem *VRTRawRasterBand::GetVirtualMemAuto(GDALRWFlag eRWFlag,
                                                   int *pnPixelSpace,
                                                   GIntBig *pnLineSpace,
                                                   char **papszOptions)

{
    // check the pointer to RawRasterBand
    if (m_poRawRaster == nullptr)
    {
        // use the super class method
        return VRTRasterBand::GetVirtualMemAuto(eRWFlag, pnPixelSpace,
                                                pnLineSpace, papszOptions);
    }
    // if available, use the RawRasterBand method (use mmap if available)
    return m_poRawRaster->GetVirtualMemAuto(eRWFlag, pnPixelSpace, pnLineSpace,
                                            papszOptions);
}

/************************************************************************/
/*                              XMLInit()                               */
/************************************************************************/

CPLErr VRTRawRasterBand::XMLInit(const CPLXMLNode *psTree,
                                 const char *pszVRTPath,
                                 VRTMapSharedResources &oMapSharedSources)

{
    const CPLErr eErr =
        VRTRasterBand::XMLInit(psTree, pszVRTPath, oMapSharedSources);
    if (eErr != CE_None)
        return eErr;

    /* -------------------------------------------------------------------- */
    /*      Validate a bit.                                                 */
    /* -------------------------------------------------------------------- */
    if (psTree == nullptr || psTree->eType != CXT_Element ||
        !EQUAL(psTree->pszValue, "VRTRasterBand") ||
        !EQUAL(CPLGetXMLValue(psTree, "subClass", ""), "VRTRawRasterBand"))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Invalid node passed to VRTRawRasterBand::XMLInit().");
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Prepare filename.                                               */
    /* -------------------------------------------------------------------- */
    const char *pszFilename = CPLGetXMLValue(psTree, "SourceFilename", nullptr);

    if (pszFilename == nullptr)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Missing <SourceFilename> element in VRTRasterBand.");
        return CE_Failure;
    }

    const bool l_bRelativeToVRT = CPLTestBool(
        CPLGetXMLValue(psTree, "SourceFilename.relativeToVRT", "1"));

    /* -------------------------------------------------------------------- */
    /*      Collect layout information.                                     */
    /* -------------------------------------------------------------------- */
    int nWordDataSize = GDALGetDataTypeSizeBytes(GetRasterDataType());

    const char *pszImageOffset = CPLGetXMLValue(psTree, "ImageOffset", "0");
    const vsi_l_offset nImageOffset = CPLScanUIntBig(
        pszImageOffset, static_cast<int>(strlen(pszImageOffset)));

    int nPixelOffset = nWordDataSize;
    const char *pszPixelOffset = CPLGetXMLValue(psTree, "PixelOffset", nullptr);
    if (pszPixelOffset != nullptr)
    {
        nPixelOffset = atoi(pszPixelOffset);
    }
    if (nPixelOffset <= 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Invalid value for <PixelOffset> element : %d", nPixelOffset);
        return CE_Failure;
    }

    int nLineOffset = 0;
    const char *pszLineOffset = CPLGetXMLValue(psTree, "LineOffset", nullptr);
    if (pszLineOffset == nullptr)
    {
        if (nPixelOffset > INT_MAX / GetXSize())
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Int overflow");
            return CE_Failure;
        }
        nLineOffset = nPixelOffset * GetXSize();
    }
    else
        nLineOffset = atoi(pszLineOffset);

    const char *pszByteOrder = CPLGetXMLValue(psTree, "ByteOrder", nullptr);

    /* -------------------------------------------------------------------- */
    /*      Open the file, and setup the raw layer access to the data.      */
    /* -------------------------------------------------------------------- */
    return SetRawLink(pszFilename, pszVRTPath, l_bRelativeToVRT, nImageOffset,
                      nPixelOffset, nLineOffset, pszByteOrder);
}

/************************************************************************/
/*                           SerializeToXML()                           */
/************************************************************************/

CPLXMLNode *VRTRawRasterBand::SerializeToXML(const char *pszVRTPath,
                                             bool &bHasWarnedAboutRAMUsage,
                                             size_t &nAccRAMUsage)

{

    /* -------------------------------------------------------------------- */
    /*      We can't set the layout if there is no open rawband.            */
    /* -------------------------------------------------------------------- */
    if (m_poRawRaster == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "VRTRawRasterBand::SerializeToXML() fails because "
                 "m_poRawRaster is NULL.");
        return nullptr;
    }

    CPLXMLNode *psTree = VRTRasterBand::SerializeToXML(
        pszVRTPath, bHasWarnedAboutRAMUsage, nAccRAMUsage);

    /* -------------------------------------------------------------------- */
    /*      Set subclass.                                                   */
    /* -------------------------------------------------------------------- */
    CPLCreateXMLNode(CPLCreateXMLNode(psTree, CXT_Attribute, "subClass"),
                     CXT_Text, "VRTRawRasterBand");

    /* -------------------------------------------------------------------- */
    /*      Setup the filename with relative flag.                          */
    /* -------------------------------------------------------------------- */
    CPLXMLNode *psNode = CPLCreateXMLElementAndValue(psTree, "SourceFilename",
                                                     m_pszSourceFilename);

    CPLCreateXMLNode(CPLCreateXMLNode(psNode, CXT_Attribute, "relativeToVRT"),
                     CXT_Text, m_bRelativeToVRT ? "1" : "0");

    /* -------------------------------------------------------------------- */
    /*      Set other layout information.                                   */
    /* -------------------------------------------------------------------- */

    CPLCreateXMLElementAndValue(
        psTree, "ImageOffset",
        CPLSPrintf(CPL_FRMT_GUIB, m_poRawRaster->GetImgOffset()));

    CPLCreateXMLElementAndValue(
        psTree, "PixelOffset",
        CPLSPrintf("%d", m_poRawRaster->GetPixelOffset()));

    CPLCreateXMLElementAndValue(
        psTree, "LineOffset", CPLSPrintf("%d", m_poRawRaster->GetLineOffset()));

    switch (m_poRawRaster->GetByteOrder())
    {
        case RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN:
            CPLCreateXMLElementAndValue(psTree, "ByteOrder", "LSB");
            break;
        case RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN:
            CPLCreateXMLElementAndValue(psTree, "ByteOrder", "MSB");
            break;
        case RawRasterBand::ByteOrder::ORDER_VAX:
            CPLCreateXMLElementAndValue(psTree, "ByteOrder", "VAX");
            break;
    }

    return psTree;
}

/************************************************************************/
/*                             GetFileList()                            */
/************************************************************************/

void VRTRawRasterBand::GetFileList(char ***ppapszFileList, int *pnSize,
                                   int *pnMaxSize, CPLHashSet *hSetFiles)
{
    if (m_pszSourceFilename == nullptr)
        return;

    /* -------------------------------------------------------------------- */
    /*      Is it already in the list ?                                     */
    /* -------------------------------------------------------------------- */
    CPLString osSourceFilename;
    if (m_bRelativeToVRT && strlen(poDS->GetDescription()) > 0)
        osSourceFilename = CPLFormFilenameSafe(
            CPLGetDirnameSafe(poDS->GetDescription()).c_str(),
            m_pszSourceFilename, nullptr);
    else
        osSourceFilename = m_pszSourceFilename;

    if (CPLHashSetLookup(hSetFiles, osSourceFilename) != nullptr)
        return;

    /* -------------------------------------------------------------------- */
    /*      Grow array if necessary                                         */
    /* -------------------------------------------------------------------- */
    if (*pnSize + 1 >= *pnMaxSize)
    {
        *pnMaxSize = 2 + 2 * (*pnMaxSize);
        *ppapszFileList = static_cast<char **>(
            CPLRealloc(*ppapszFileList, sizeof(char *) * (*pnMaxSize)));
    }

    /* -------------------------------------------------------------------- */
    /*      Add the string to the list                                      */
    /* -------------------------------------------------------------------- */
    (*ppapszFileList)[*pnSize] = CPLStrdup(osSourceFilename);
    (*ppapszFileList)[(*pnSize + 1)] = nullptr;
    CPLHashSetInsert(hSetFiles, (*ppapszFileList)[*pnSize]);

    (*pnSize)++;

    VRTRasterBand::GetFileList(ppapszFileList, pnSize, pnMaxSize, hSetFiles);
}

/*! @endcond */
