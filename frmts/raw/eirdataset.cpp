/******************************************************************************
 *
 * Project:  Erdas EIR Raw Driver
 * Purpose:  Implementation of EIRDataset
 * Author:   Adam Milling, amilling@alumni.uwaterloo.ca
 *
 ******************************************************************************
 * Copyright (c) 1999, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2009-2010, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_string.h"
#include "gdal_frmts.h"
#include "ogr_spatialref.h"
#include "rawdataset.h"

#include <limits>

/************************************************************************/
/* ==================================================================== */
/*              EIRDataset                                              */
/* ==================================================================== */
/************************************************************************/

class EIRDataset final : public RawDataset
{
    friend class RawRasterBand;

    VSILFILE *fpImage = nullptr;  // image data file
    bool bGotTransform = false;
    GDALGeoTransform m_gt{};
    bool bHDRDirty = false;
    CPLStringList aosHDR{};
    char **papszExtraFiles = nullptr;

    void ResetKeyValue(const char *pszKey, const char *pszValue);
#ifdef unused
    const char *GetKeyValue(const char *pszKey, const char *pszDefault = "");
#endif

    CPL_DISALLOW_COPY_ASSIGN(EIRDataset)

    CPLErr Close() override;

  public:
    EIRDataset();
    ~EIRDataset() override;

    CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;

    char **GetFileList() override;

    static int Identify(GDALOpenInfo *);
    static GDALDataset *Open(GDALOpenInfo *);
};

/************************************************************************/
/* ==================================================================== */
/*                            EIRDataset                                */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                            EIRDataset()                             */
/************************************************************************/

EIRDataset::EIRDataset() = default;

/************************************************************************/
/*                            ~EIRDataset()                            */
/************************************************************************/

EIRDataset::~EIRDataset()

{
    EIRDataset::Close();
}

/************************************************************************/
/*                              Close()                                 */
/************************************************************************/

CPLErr EIRDataset::Close()
{
    CPLErr eErr = CE_None;
    if (nOpenFlags != OPEN_FLAGS_CLOSED)
    {
        if (EIRDataset::FlushCache(true) != CE_None)
            eErr = CE_Failure;

        if (nBands > 0 && GetAccess() == GA_Update)
        {
            RawRasterBand *poBand =
                cpl::down_cast<RawRasterBand *>(GetRasterBand(1));

            int bNoDataSet = FALSE;
            const double dfNoData = poBand->GetNoDataValue(&bNoDataSet);
            if (bNoDataSet)
            {
                ResetKeyValue("NODATA", CPLString().Printf("%.8g", dfNoData));
            }
        }

        if (fpImage)
        {
            if (VSIFCloseL(fpImage) != 0)
                eErr = CE_Failure;
        }

        CSLDestroy(papszExtraFiles);

        if (GDALPamDataset::Close() != CE_None)
            eErr = CE_Failure;
    }
    return eErr;
}

#ifdef unused
/************************************************************************/
/*                            GetKeyValue()                             */
/************************************************************************/

const char *EIRDataset::GetKeyValue(const char *pszKey, const char *pszDefault)

{
    const char *const *papszHDR = aosHDR.List();
    for (int i = 0; papszHDR[i] != nullptr; i++)
    {
        if (EQUALN(pszKey, papszHDR[i], strlen(pszKey)) &&
            isspace((unsigned char)papszHDR[i][strlen(pszKey)]))
        {
            const char *pszValue = papszHDR[i] + strlen(pszKey);
            while (isspace(static_cast<unsigned char>(*pszValue)))
                pszValue++;

            return pszValue;
        }
    }

    return pszDefault;
}
#endif

/************************************************************************/
/*                           ResetKeyValue()                            */
/*                                                                      */
/*      Replace or add the keyword with the indicated value in the      */
/*      papszHDR list.                                                  */
/************************************************************************/

void EIRDataset::ResetKeyValue(const char *pszKey, const char *pszValue)

{
    if (strlen(pszValue) > 65)
    {
        CPLAssert(strlen(pszValue) <= 65);
        return;
    }

    char szNewLine[82] = {'\0'};
    snprintf(szNewLine, sizeof(szNewLine), "%-15s%s", pszKey, pszValue);

    char **papszHDR = aosHDR.List();
    for (int i = aosHDR.size() - 1; i >= 0; i--)
    {
        if (EQUALN(papszHDR[i], szNewLine, strlen(pszKey) + 1))
        {
            if (strcmp(papszHDR[i], szNewLine) != 0)
            {
                CPLFree(papszHDR[i]);
                papszHDR[i] = CPLStrdup(szNewLine);
                bHDRDirty = true;
            }
            return;
        }
    }

    bHDRDirty = true;
    aosHDR.AddString(szNewLine);
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr EIRDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    if (bGotTransform)
    {
        gt = m_gt;
        return CE_None;
    }

    return GDALPamDataset::GetGeoTransform(gt);
}

/************************************************************************/
/*                            GetFileList()                             */
/************************************************************************/

char **EIRDataset::GetFileList()

{
    // Main data file, etc.
    char **papszFileList = GDALPamDataset::GetFileList();

    // Header file.
    papszFileList = CSLInsertStrings(papszFileList, -1, papszExtraFiles);

    return papszFileList;
}

/************************************************************************/
/*                              Identify()                              */
/************************************************************************/

int EIRDataset::Identify(GDALOpenInfo *poOpenInfo)

{
    if (poOpenInfo->nHeaderBytes < 100)
        return FALSE;

    if (strstr(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
               "IMAGINE_RAW_FILE") == nullptr)
        return FALSE;

    return TRUE;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *EIRDataset::Open(GDALOpenInfo *poOpenInfo)

{
    if (!Identify(poOpenInfo) || poOpenInfo->fpL == nullptr)
        return nullptr;

    /* header example and description

    IMAGINE_RAW_FILE // must be on first line, by itself
    WIDTH 581        // number of columns in the image
    HEIGHT 695       // number of rows in the image
    NUM_LAYERS 3     // number of spectral bands in the image; default 1
    PIXEL_FILES raw8_3n_ui_sanjack.bl // raster file
                                      // default: same name with no extension
    FORMAT BIL       // BIL BIP BSQ; default BIL
    DATATYPE U8      // U1 U2 U4 U8 U16 U32 S16 S32 F32 F64; default U8
    BYTE_ORDER       // LSB MSB; required for U16 U32 S16 S32 F32 F64
    DATA_OFFSET      // start of image data in raster file; default 0 bytes
    END_RAW_FILE     // end RAW file - stop reading

    For a true color image with three bands (R, G, B) stored using 8 bits
    for each pixel in each band, DATA_TYPE equals U8 and NUM_LAYERS equals
    3 for a total of 24 bits per pixel.

    Note that the current version of ERDAS Raw Raster Reader/Writer does
    not support the LAYER_SKIP_BYTES, RECORD_SKIP_BYTES, TILE_WIDTH and
    TILE_HEIGHT directives. Since the reader does not read the PIXEL_FILES
    directive, the reader always assumes that the raw binary file is the
    dataset, and the name of this file is the name of the header without the
    extension. Currently, the reader does not support multiple raw binary
    files in one dataset or a single file with both the header and the raw
    binary data at the same time.
    */

    int nRows = -1;
    int nCols = -1;
    int nBands = 1;
    int nSkipBytes = 0;
    int nLineCount = 0;
    GDALDataType eDataType = GDT_Byte;
    int nBits = 8;
    char chByteOrder = 'M';
    char szLayout[10] = "BIL";
    CPLStringList aosHDR;

    // default raster file: same name with no extension
    const CPLString osPath = CPLGetPathSafe(poOpenInfo->pszFilename);
    const CPLString osName = CPLGetBasenameSafe(poOpenInfo->pszFilename);
    CPLString osRasterFilename = CPLFormCIFilenameSafe(osPath, osName, "");

    // parse the header file
    const char *pszLine = nullptr;
    VSIRewindL(poOpenInfo->fpL);
    while ((pszLine = CPLReadLineL(poOpenInfo->fpL)) != nullptr)
    {
        nLineCount++;

        if ((nLineCount == 1) && !EQUAL(pszLine, "IMAGINE_RAW_FILE"))
        {
            return nullptr;
        }

        if ((nLineCount > 50) || EQUAL(pszLine, "END_RAW_FILE"))
        {
            break;
        }

        if (strlen(pszLine) > 1000)
            break;

        aosHDR.AddString(pszLine);

        const CPLStringList aosTokens(
            CSLTokenizeStringComplex(pszLine, " \t", TRUE, FALSE));
        if (aosTokens.size() < 2)
        {
            continue;
        }

        if (EQUAL(aosTokens[0], "WIDTH"))
        {
            nCols = atoi(aosTokens[1]);
        }
        else if (EQUAL(aosTokens[0], "HEIGHT"))
        {
            nRows = atoi(aosTokens[1]);
        }
        else if (EQUAL(aosTokens[0], "NUM_LAYERS"))
        {
            nBands = atoi(aosTokens[1]);
        }
        else if (EQUAL(aosTokens[0], "PIXEL_FILES"))
        {
            osRasterFilename = CPLFormCIFilenameSafe(osPath, aosTokens[1], "");
        }
        else if (EQUAL(aosTokens[0], "FORMAT"))
        {
            snprintf(szLayout, sizeof(szLayout), "%s", aosTokens[1]);
        }
        else if (EQUAL(aosTokens[0], "DATATYPE") ||
                 EQUAL(aosTokens[0], "DATA_TYPE"))
        {
            if (EQUAL(aosTokens[1], "U1") || EQUAL(aosTokens[1], "U2") ||
                EQUAL(aosTokens[1], "U4") || EQUAL(aosTokens[1], "U8"))
            {
                nBits = 8;
                eDataType = GDT_Byte;
            }
            else if (EQUAL(aosTokens[1], "U16"))
            {
                nBits = 16;
                eDataType = GDT_UInt16;
            }
            else if (EQUAL(aosTokens[1], "U32"))
            {
                nBits = 32;
                eDataType = GDT_UInt32;
            }
            else if (EQUAL(aosTokens[1], "S16"))
            {
                nBits = 16;
                eDataType = GDT_Int16;
            }
            else if (EQUAL(aosTokens[1], "S32"))
            {
                nBits = 32;
                eDataType = GDT_Int32;
            }
            else if (EQUAL(aosTokens[1], "F32"))
            {
                nBits = 32;
                eDataType = GDT_Float32;
            }
            else if (EQUAL(aosTokens[1], "F64"))
            {
                nBits = 64;
                eDataType = GDT_Float64;
            }
            else
            {
                CPLError(CE_Failure, CPLE_NotSupported,
                         "EIR driver does not support DATATYPE %s.",
                         aosTokens[1]);
                return nullptr;
            }
        }
        else if (EQUAL(aosTokens[0], "BYTE_ORDER"))
        {
            // M for MSB, L for LSB
            chByteOrder = static_cast<char>(
                toupper(static_cast<unsigned char>(aosTokens[1][0])));
        }
        else if (EQUAL(aosTokens[0], "DATA_OFFSET"))
        {
            nSkipBytes = atoi(aosTokens[1]);  // TBD: is this mapping right?
            if (nSkipBytes < 0)
            {
                return nullptr;
            }
        }
    }
    CPL_IGNORE_RET_VAL(nBits);

    /* -------------------------------------------------------------------- */
    /*      Did we get the required keywords?  If not we return with        */
    /*      this never having been considered to be a match. This isn't     */
    /*      an error!                                                       */
    /* -------------------------------------------------------------------- */
    if (nRows <= 0 || nCols <= 0 || nBands <= 0)
    {
        return nullptr;
    }

    if (!GDALCheckDatasetDimensions(nCols, nRows) ||
        !GDALCheckBandCount(nBands, FALSE))
    {
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Confirm the requested access is supported.                      */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->eAccess == GA_Update)
    {
        ReportUpdateNotSupportedByDriver("EIR");
        return nullptr;
    }
    /* -------------------------------------------------------------------- */
    /*      Create a corresponding GDALDataset.                             */
    /* -------------------------------------------------------------------- */
    auto poDS = std::make_unique<EIRDataset>();

    /* -------------------------------------------------------------------- */
    /*      Capture some information from the file that is of interest.     */
    /* -------------------------------------------------------------------- */
    poDS->nRasterXSize = nCols;
    poDS->nRasterYSize = nRows;
    poDS->aosHDR = std::move(aosHDR);

    /* -------------------------------------------------------------------- */
    /*      Open target binary file.                                        */
    /* -------------------------------------------------------------------- */
    poDS->fpImage = VSIFOpenL(osRasterFilename.c_str(), "rb");
    if (poDS->fpImage == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "Failed to open %s: %s",
                 osRasterFilename.c_str(), VSIStrerror(errno));
        return nullptr;
    }
    poDS->papszExtraFiles =
        CSLAddString(poDS->papszExtraFiles, osRasterFilename);

    poDS->eAccess = poOpenInfo->eAccess;

    /* -------------------------------------------------------------------- */
    /*      Compute the line offset.                                        */
    /* -------------------------------------------------------------------- */
    const int nItemSize = GDALGetDataTypeSizeBytes(eDataType);
    int nPixelOffset = 0;
    int nLineOffset = 0;
    vsi_l_offset nBandOffset = 0;

    if (EQUAL(szLayout, "BIP"))
    {
        nPixelOffset = nItemSize * nBands;
        if (nPixelOffset > INT_MAX / nCols)
        {
            return nullptr;
        }
        nLineOffset = nPixelOffset * nCols;
        nBandOffset = static_cast<vsi_l_offset>(nItemSize);
    }
    else if (EQUAL(szLayout, "BSQ"))
    {
        nPixelOffset = nItemSize;
        if (nPixelOffset > INT_MAX / nCols)
        {
            return nullptr;
        }
        nLineOffset = nPixelOffset * nCols;
        nBandOffset = static_cast<vsi_l_offset>(nLineOffset) * nRows;
    }
    else /* assume BIL */
    {
        nPixelOffset = nItemSize;
        if (nItemSize > INT_MAX / nBands ||
            nItemSize * nBands > INT_MAX / nCols)
        {
            return nullptr;
        }
        nLineOffset = nItemSize * nBands * nCols;
        nBandOffset = static_cast<vsi_l_offset>(nItemSize) * nCols;
    }

    if (poDS->nBands > 1)
    {
        if (nBandOffset >
                std::numeric_limits<vsi_l_offset>::max() / (poDS->nBands - 1) ||
            static_cast<vsi_l_offset>(nSkipBytes) >
                std::numeric_limits<vsi_l_offset>::max() -
                    nBandOffset * (poDS->nBands - 1))
        {
            return nullptr;
        }
    }

    if (!RAWDatasetCheckMemoryUsage(
            poDS->nRasterXSize, poDS->nRasterYSize, nBands, nItemSize,
            nPixelOffset, nLineOffset, nSkipBytes, nBandOffset, poDS->fpImage))
    {
        return nullptr;
    }

    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->PamInitialize();

    /* -------------------------------------------------------------------- */
    /*      Create band information objects.                                */
    /* -------------------------------------------------------------------- */
    for (int i = 0; i < nBands; i++)
    {
        auto poBand = RawRasterBand::Create(
            poDS.get(), i + 1, poDS->fpImage, nSkipBytes + nBandOffset * i,
            nPixelOffset, nLineOffset, eDataType,
            chByteOrder == 'I' || chByteOrder == 'L'
                ? RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN
                : RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN,
            RawRasterBand::OwnFP::NO);
        if (!poBand)
            return nullptr;
        poDS->SetBand(i + 1, std::move(poBand));
    }

    /* -------------------------------------------------------------------- */
    /*      look for a worldfile                                            */
    /* -------------------------------------------------------------------- */

    if (!poDS->bGotTransform)
        poDS->bGotTransform = CPL_TO_BOOL(GDALReadWorldFile(
            poOpenInfo->pszFilename, nullptr, poDS->m_gt.data()));

    if (!poDS->bGotTransform)
        poDS->bGotTransform = CPL_TO_BOOL(GDALReadWorldFile(
            poOpenInfo->pszFilename, "wld", poDS->m_gt.data()));

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->TryLoadXML();

    /* -------------------------------------------------------------------- */
    /*      Check for overviews.                                            */
    /* -------------------------------------------------------------------- */
    poDS->oOvManager.Initialize(poDS.get(), poOpenInfo->pszFilename);

    return poDS.release();
}

/************************************************************************/
/*                         GDALRegister_EIR()                           */
/************************************************************************/

void GDALRegister_EIR()

{
    if (GDALGetDriverByName("EIR") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("EIR");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "Erdas Imagine Raw");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/eir.html");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    poDriver->pfnOpen = EIRDataset::Open;
    poDriver->pfnIdentify = EIRDataset::Identify;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
