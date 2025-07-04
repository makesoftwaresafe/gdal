/******************************************************************************
 *
 * Project:  CEOS Translator
 * Purpose:  GDALDataset driver for CEOS translator.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1999, Frank Warmerdam
 * Copyright (c) 2009-2010, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ceosopen.h"
#include "gdal_frmts.h"
#include "gdal_pam.h"

/************************************************************************/
/* ==================================================================== */
/*                              CEOSDataset                             */
/* ==================================================================== */
/************************************************************************/

class CEOSRasterBand;

class CEOSDataset final : public GDALPamDataset
{
    friend class CEOSRasterBand;

    CEOSImage *psCEOS;

    CPL_DISALLOW_COPY_ASSIGN(CEOSDataset)

  public:
    CEOSDataset();
    ~CEOSDataset();
    static GDALDataset *Open(GDALOpenInfo *);
};

/************************************************************************/
/* ==================================================================== */
/*                            CEOSRasterBand                             */
/* ==================================================================== */
/************************************************************************/

class CEOSRasterBand final : public GDALPamRasterBand
{
    friend class CEOSDataset;

  public:
    CEOSRasterBand(CEOSDataset *, int);

    CPLErr IReadBlock(int, int, void *) override;
};

/************************************************************************/
/*                           CEOSRasterBand()                            */
/************************************************************************/

CEOSRasterBand::CEOSRasterBand(CEOSDataset *poDSIn, int nBandIn)

{
    poDS = poDSIn;
    nBand = nBandIn;

    eDataType = GDT_Byte;

    nBlockXSize = poDS->GetRasterXSize();
    nBlockYSize = 1;
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr CEOSRasterBand::IReadBlock(CPL_UNUSED int nBlockXOff, int nBlockYOff,
                                  void *pImage)
{
    CEOSDataset *poCEOS_DS = cpl::down_cast<CEOSDataset *>(poDS);

    CPLAssert(nBlockXOff == 0);

    return CEOSReadScanline(poCEOS_DS->psCEOS, nBand, nBlockYOff + 1, pImage);
}

/************************************************************************/
/* ==================================================================== */
/*                             CEOSDataset                              */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                            CEOSDataset()                             */
/************************************************************************/

CEOSDataset::CEOSDataset() : psCEOS(nullptr)
{
}

/************************************************************************/
/*                            ~CEOSDataset()                            */
/************************************************************************/

CEOSDataset::~CEOSDataset()

{
    FlushCache(true);
    if (psCEOS)
        CEOSClose(psCEOS);
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *CEOSDataset::Open(GDALOpenInfo *poOpenInfo)

{
    /* -------------------------------------------------------------------- */
    /*      Before trying CEOSOpen() we first verify that the first         */
    /*      record is in fact a CEOS file descriptor record.                */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->nHeaderBytes < 100)
        return nullptr;

    if (poOpenInfo->pabyHeader[4] != 0x3f ||
        poOpenInfo->pabyHeader[5] != 0xc0 ||
        poOpenInfo->pabyHeader[6] != 0x12 || poOpenInfo->pabyHeader[7] != 0x12)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Try opening the dataset.                                        */
    /* -------------------------------------------------------------------- */
    CEOSImage *psCEOS = CEOSOpen(poOpenInfo->pszFilename, "rb");
    if (psCEOS == nullptr)
        return nullptr;

    if (psCEOS->nBitsPerPixel != 8)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "The CEOS driver cannot handle nBitsPerPixel = %d",
                 psCEOS->nBitsPerPixel);
        CEOSClose(psCEOS);
        return nullptr;
    }

    if (!GDALCheckDatasetDimensions(psCEOS->nPixels, psCEOS->nBands) ||
        !GDALCheckBandCount(psCEOS->nBands, FALSE))
    {
        CEOSClose(psCEOS);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Confirm the requested access is supported.                      */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->eAccess == GA_Update)
    {
        CEOSClose(psCEOS);
        ReportUpdateNotSupportedByDriver("CEOS");
        return nullptr;
    }
    /* -------------------------------------------------------------------- */
    /*      Create a corresponding GDALDataset.                             */
    /* -------------------------------------------------------------------- */
    CEOSDataset *poDS = new CEOSDataset();

    poDS->psCEOS = psCEOS;

    /* -------------------------------------------------------------------- */
    /*      Capture some information from the file that is of interest.     */
    /* -------------------------------------------------------------------- */
    poDS->nRasterXSize = psCEOS->nPixels;
    poDS->nRasterYSize = psCEOS->nLines;

    /* -------------------------------------------------------------------- */
    /*      Create band information objects.                                */
    /* -------------------------------------------------------------------- */
    poDS->nBands = psCEOS->nBands;

    for (int i = 0; i < poDS->nBands; i++)
        poDS->SetBand(i + 1, new CEOSRasterBand(poDS, i + 1));

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->TryLoadXML();

    /* -------------------------------------------------------------------- */
    /*      Check for overviews.                                            */
    /* -------------------------------------------------------------------- */
    poDS->oOvManager.Initialize(poDS, poOpenInfo->pszFilename);

    return poDS;
}

/************************************************************************/
/*                          GDALRegister_GTiff()                        */
/************************************************************************/

void GDALRegister_CEOS()

{
    if (GDALGetDriverByName("CEOS") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("CEOS");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "CEOS Image");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/ceos.html");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    poDriver->pfnOpen = CEOSDataset::Open;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
