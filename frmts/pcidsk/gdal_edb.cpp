/******************************************************************************
 *
 * Project:  PCIDSK Database File
 * Purpose:  External Database access interface implementation (EDBFile).
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2010, Frank Warmerdam <warmerdam@pobox.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_conv.h"
#include "cpl_multiproc.h"
#include "gdal_priv.h"
#include "pcidsk.h"

using PCIDSK::CHN_16S;
using PCIDSK::CHN_16U;
using PCIDSK::CHN_32R;
using PCIDSK::CHN_8U;
using PCIDSK::CHN_C16S;
using PCIDSK::CHN_UNKNOWN;
using PCIDSK::eChanType;
using PCIDSK::EDBFile;
using PCIDSK::ThrowPCIDSKException;

EDBFile *GDAL_EDBOpen(const std::string &osFilename,
                      const std::string &osAccess);

/************************************************************************/
/* ==================================================================== */
/*                            GDAL_EDBFile                              */
/* ==================================================================== */
/************************************************************************/

class GDAL_EDBFile final : public EDBFile
{
    GDALDataset *poDS;

  public:
    explicit GDAL_EDBFile(GDALDataset *poDSIn)
    {
        poDS = poDSIn;
    }

    ~GDAL_EDBFile()
    {
        if (poDS)
            GDAL_EDBFile::Close();
    }

    int Close() const override;
    int GetWidth() const override;
    int GetHeight() const override;
    int GetChannels() const override;
    int GetBlockWidth(int channel) const override;
    int GetBlockHeight(int channel) const override;
    eChanType GetType(int channel) const override;
    int ReadBlock(int channel, int block_index, void *buffer, int win_xoff,
                  int win_yoff, int win_xsize, int win_ysize) override;
    int WriteBlock(int channel, int block_index, void *buffer) override;
};

/************************************************************************/
/*                            GDAL_EDBOpen()                            */
/************************************************************************/

EDBFile *GDAL_EDBOpen(const std::string &osFilename,
                      const std::string &osAccess)

{
    GDALDataset *poDS = nullptr;

    if (osAccess == "r")
        poDS =
            GDALDataset::FromHandle(GDALOpen(osFilename.c_str(), GA_ReadOnly));
    else
        poDS = GDALDataset::FromHandle(GDALOpen(osFilename.c_str(), GA_Update));

    if (poDS == nullptr)
        ThrowPCIDSKException("%s", CPLGetLastErrorMsg());

    return new GDAL_EDBFile(poDS);
}

/************************************************************************/
/*                               Close()                                */
/************************************************************************/

int GDAL_EDBFile::Close() const

{
    if (poDS != nullptr)
    {
        delete poDS;
        const_cast<GDAL_EDBFile *>(this)->poDS = nullptr;
    }

    return 1;
}

/************************************************************************/
/*                              GetWidth()                              */
/************************************************************************/

int GDAL_EDBFile::GetWidth() const

{
    return poDS->GetRasterXSize();
}

/************************************************************************/
/*                             GetHeight()                              */
/************************************************************************/

int GDAL_EDBFile::GetHeight() const

{
    return poDS->GetRasterYSize();
}

/************************************************************************/
/*                            GetChannels()                             */
/************************************************************************/

int GDAL_EDBFile::GetChannels() const

{
    return poDS->GetRasterCount();
}

/************************************************************************/
/*                           GetBlockWidth()                            */
/************************************************************************/

int GDAL_EDBFile::GetBlockWidth(int nChannel) const

{
    int nWidth, nHeight;

    poDS->GetRasterBand(nChannel)->GetBlockSize(&nWidth, &nHeight);

    return nWidth;
}

/************************************************************************/
/*                           GetBlockHeight()                           */
/************************************************************************/

int GDAL_EDBFile::GetBlockHeight(int nChannel) const

{
    int nWidth, nHeight;

    poDS->GetRasterBand(nChannel)->GetBlockSize(&nWidth, &nHeight);

    return nHeight;
}

/************************************************************************/
/*                              GetType()                               */
/************************************************************************/

eChanType GDAL_EDBFile::GetType(int nChannel) const
{
    switch (poDS->GetRasterBand(nChannel)->GetRasterDataType())
    {
        case GDT_Byte:
            return CHN_8U;

        case GDT_Int16:
            return CHN_16S;

        case GDT_UInt16:
            return CHN_16U;

        case GDT_Float32:
            return CHN_32R;

        case GDT_CInt16:
            return CHN_C16S;

        default:
            return CHN_UNKNOWN;
    }
}

/************************************************************************/
/*                             ReadBlock()                              */
/************************************************************************/

int GDAL_EDBFile::ReadBlock(int channel, int block_index, void *buffer,
                            int win_xoff, int win_yoff, int win_xsize,
                            int win_ysize)

{
    GDALRasterBand *poBand = poDS->GetRasterBand(channel);

    if (GetType(channel) == CHN_UNKNOWN)
    {
        ThrowPCIDSKException("%s channel type not supported for PCIDSK access.",
                             GDALGetDataTypeName(poBand->GetRasterDataType()));
    }

    int nBlockXSize, nBlockYSize;
    poBand->GetBlockSize(&nBlockXSize, &nBlockYSize);

    const int nWidthInBlocks = DIV_ROUND_UP(poBand->GetXSize(), nBlockXSize);

    const int nBlockX = block_index % nWidthInBlocks;
    const int nBlockY = block_index / nWidthInBlocks;

    const int nPixelOffset =
        GDALGetDataTypeSizeBytes(poBand->GetRasterDataType());
    const int nLineOffset = win_xsize * nPixelOffset;

    /* -------------------------------------------------------------------- */
    /*      Are we reading a partial block at the edge of the database?     */
    /*      If so, ensure we don't read off the database.                   */
    /* -------------------------------------------------------------------- */
    if (nBlockX * nBlockXSize + win_xoff + win_xsize > poBand->GetXSize())
        win_xsize = poBand->GetXSize() - nBlockX * nBlockXSize - win_xoff;

    if (nBlockY * nBlockYSize + win_yoff + win_ysize > poBand->GetYSize())
        win_ysize = poBand->GetYSize() - nBlockY * nBlockYSize - win_yoff;

    const CPLErr eErr = poBand->RasterIO(
        GF_Read, nBlockX * nBlockXSize + win_xoff,
        nBlockY * nBlockYSize + win_yoff, win_xsize, win_ysize, buffer,
        win_xsize, win_ysize, poBand->GetRasterDataType(), nPixelOffset,
        nLineOffset, nullptr);

    if (eErr != CE_None)
    {
        ThrowPCIDSKException("%s", CPLGetLastErrorMsg());
    }

    return 1;
}

/************************************************************************/
/*                             WriteBlock()                             */
/************************************************************************/

int GDAL_EDBFile::WriteBlock(int channel, int block_index, void *buffer)

{
    GDALRasterBand *poBand = poDS->GetRasterBand(channel);

    if (GetType(channel) == CHN_UNKNOWN)
    {
        ThrowPCIDSKException("%s channel type not supported for PCIDSK access.",
                             GDALGetDataTypeName(poBand->GetRasterDataType()));
    }

    int nBlockXSize, nBlockYSize;
    poBand->GetBlockSize(&nBlockXSize, &nBlockYSize);

    const int nWidthInBlocks = DIV_ROUND_UP(poBand->GetXSize(), nBlockXSize);

    const int nBlockX = block_index % nWidthInBlocks;
    const int nBlockY = block_index / nWidthInBlocks;

    /* -------------------------------------------------------------------- */
    /*      Are we reading a partial block at the edge of the database?     */
    /*      If so, ensure we don't read off the database.                   */
    /* -------------------------------------------------------------------- */
    int nWinXSize, nWinYSize;

    if (nBlockX * nBlockXSize + nBlockXSize > poBand->GetXSize())
        nWinXSize = poBand->GetXSize() - nBlockX * nBlockXSize;
    else
        nWinXSize = nBlockXSize;

    if (nBlockY * nBlockYSize + nBlockYSize > poBand->GetYSize())
        nWinYSize = poBand->GetYSize() - nBlockY * nBlockYSize;
    else
        nWinYSize = nBlockYSize;

    const CPLErr eErr =
        poBand->RasterIO(GF_Write, nBlockX * nBlockXSize, nBlockY * nBlockYSize,
                         nWinXSize, nWinYSize, buffer, nWinXSize, nWinYSize,
                         poBand->GetRasterDataType(), 0, 0, nullptr);

    if (eErr != CE_None)
    {
        ThrowPCIDSKException("%s", CPLGetLastErrorMsg());
    }

    return 1;
}
