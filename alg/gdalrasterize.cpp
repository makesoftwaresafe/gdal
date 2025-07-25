/******************************************************************************
 *
 * Project:  GDAL
 * Purpose:  Vector rasterization.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2005, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "gdal_alg.h"
#include "gdal_alg_priv.h"

#include <climits>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cfloat>
#include <limits>
#include <vector>
#include <algorithm>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_progress.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "gdal.h"
#include "gdal_priv.h"
#include "gdal_priv_templates.hpp"
#include "ogr_api.h"
#include "ogr_core.h"
#include "ogr_feature.h"
#include "ogr_geometry.h"
#include "ogr_spatialref.h"
#include "ogrsf_frmts.h"

template <typename T> static inline T SaturatedAddSigned(T a, T b)
{
    if (a > 0 && b > 0 && a > std::numeric_limits<T>::max() - b)
    {
        return std::numeric_limits<T>::max();
    }
    else if (a < 0 && b < 0 && a < std::numeric_limits<T>::min() - b)
    {
        return std::numeric_limits<T>::min();
    }
    else
    {
        return a + b;
    }
}

/************************************************************************/
/*                              MakeKey()                               */
/************************************************************************/

inline uint64_t MakeKey(int y, int x)
{
    return (static_cast<uint64_t>(y) << 32) | static_cast<uint64_t>(x);
}

/************************************************************************/
/*                        gvBurnScanlineBasic()                         */
/************************************************************************/
template <typename T>
static inline void gvBurnScanlineBasic(GDALRasterizeInfo *psInfo, int nY,
                                       int nXStart, int nXEnd, double dfVariant)

{
    for (int iBand = 0; iBand < psInfo->nBands; iBand++)
    {
        const double burnValue =
            (psInfo->burnValues.double_values[iBand] +
             ((psInfo->eBurnValueSource == GBV_UserBurnValue) ? 0 : dfVariant));

        unsigned char *pabyInsert =
            psInfo->pabyChunkBuf + iBand * psInfo->nBandSpace +
            nY * psInfo->nLineSpace + nXStart * psInfo->nPixelSpace;
        if (psInfo->eMergeAlg == GRMA_Add)
        {
            if (psInfo->poSetVisitedPoints)
            {
                CPLAssert(!psInfo->bFillSetVisitedPoints);
                uint64_t nKey = MakeKey(nY, nXStart);
                auto &oSetVisitedPoints = *(psInfo->poSetVisitedPoints);
                for (int nX = nXStart; nX <= nXEnd; ++nX)
                {
                    if (oSetVisitedPoints.find(nKey) == oSetVisitedPoints.end())
                    {
                        double dfVal = static_cast<double>(
                                           *reinterpret_cast<T *>(pabyInsert)) +
                                       burnValue;
                        GDALCopyWord(dfVal, *reinterpret_cast<T *>(pabyInsert));
                    }
                    pabyInsert += psInfo->nPixelSpace;
                    ++nKey;
                }
            }
            else
            {
                for (int nX = nXStart; nX <= nXEnd; ++nX)
                {
                    double dfVal = static_cast<double>(
                                       *reinterpret_cast<T *>(pabyInsert)) +
                                   burnValue;
                    GDALCopyWord(dfVal, *reinterpret_cast<T *>(pabyInsert));
                    pabyInsert += psInfo->nPixelSpace;
                }
            }
        }
        else
        {
            T nVal;
            GDALCopyWord(burnValue, nVal);
            for (int nX = nXStart; nX <= nXEnd; ++nX)
            {
                *reinterpret_cast<T *>(pabyInsert) = nVal;
                pabyInsert += psInfo->nPixelSpace;
            }
        }
    }
}

static inline void gvBurnScanlineInt64UserBurnValue(GDALRasterizeInfo *psInfo,
                                                    int nY, int nXStart,
                                                    int nXEnd)

{
    for (int iBand = 0; iBand < psInfo->nBands; iBand++)
    {
        const std::int64_t burnValue = psInfo->burnValues.int64_values[iBand];

        unsigned char *pabyInsert =
            psInfo->pabyChunkBuf + iBand * psInfo->nBandSpace +
            nY * psInfo->nLineSpace + nXStart * psInfo->nPixelSpace;
        if (psInfo->eMergeAlg == GRMA_Add)
        {
            if (psInfo->poSetVisitedPoints)
            {
                CPLAssert(!psInfo->bFillSetVisitedPoints);
                uint64_t nKey = MakeKey(nY, nXStart);
                auto &oSetVisitedPoints = *(psInfo->poSetVisitedPoints);
                for (int nX = nXStart; nX <= nXEnd; ++nX)
                {
                    if (oSetVisitedPoints.find(nKey) == oSetVisitedPoints.end())
                    {
                        *reinterpret_cast<std::int64_t *>(pabyInsert) =
                            SaturatedAddSigned(
                                *reinterpret_cast<std::int64_t *>(pabyInsert),
                                burnValue);
                    }
                    pabyInsert += psInfo->nPixelSpace;
                    ++nKey;
                }
            }
            else
            {
                for (int nX = nXStart; nX <= nXEnd; ++nX)
                {
                    *reinterpret_cast<std::int64_t *>(pabyInsert) =
                        SaturatedAddSigned(
                            *reinterpret_cast<std::int64_t *>(pabyInsert),
                            burnValue);
                    pabyInsert += psInfo->nPixelSpace;
                }
            }
        }
        else
        {
            for (int nX = nXStart; nX <= nXEnd; ++nX)
            {
                *reinterpret_cast<std::int64_t *>(pabyInsert) = burnValue;
                pabyInsert += psInfo->nPixelSpace;
            }
        }
    }
}

/************************************************************************/
/*                           gvBurnScanline()                           */
/************************************************************************/
static void gvBurnScanline(GDALRasterizeInfo *psInfo, int nY, int nXStart,
                           int nXEnd, double dfVariant)

{
    if (nXStart > nXEnd)
        return;

    CPLAssert(nY >= 0 && nY < psInfo->nYSize);
    CPLAssert(nXStart < psInfo->nXSize);
    CPLAssert(nXEnd >= 0);

    if (nXStart < 0)
        nXStart = 0;
    if (nXEnd >= psInfo->nXSize)
        nXEnd = psInfo->nXSize - 1;

    if (psInfo->eBurnValueType == GDT_Int64)
    {
        if (psInfo->eType == GDT_Int64 &&
            psInfo->eBurnValueSource == GBV_UserBurnValue)
        {
            gvBurnScanlineInt64UserBurnValue(psInfo, nY, nXStart, nXEnd);
        }
        else
        {
            CPLAssert(false);
        }
        return;
    }

    switch (psInfo->eType)
    {
        case GDT_Byte:
            gvBurnScanlineBasic<GByte>(psInfo, nY, nXStart, nXEnd, dfVariant);
            break;
        case GDT_Int8:
            gvBurnScanlineBasic<GInt8>(psInfo, nY, nXStart, nXEnd, dfVariant);
            break;
        case GDT_Int16:
            gvBurnScanlineBasic<GInt16>(psInfo, nY, nXStart, nXEnd, dfVariant);
            break;
        case GDT_UInt16:
            gvBurnScanlineBasic<GUInt16>(psInfo, nY, nXStart, nXEnd, dfVariant);
            break;
        case GDT_Int32:
            gvBurnScanlineBasic<GInt32>(psInfo, nY, nXStart, nXEnd, dfVariant);
            break;
        case GDT_UInt32:
            gvBurnScanlineBasic<GUInt32>(psInfo, nY, nXStart, nXEnd, dfVariant);
            break;
        case GDT_Int64:
            gvBurnScanlineBasic<std::int64_t>(psInfo, nY, nXStart, nXEnd,
                                              dfVariant);
            break;
        case GDT_UInt64:
            gvBurnScanlineBasic<std::uint64_t>(psInfo, nY, nXStart, nXEnd,
                                               dfVariant);
            break;
        case GDT_Float16:
            gvBurnScanlineBasic<GFloat16>(psInfo, nY, nXStart, nXEnd,
                                          dfVariant);
            break;
        case GDT_Float32:
            gvBurnScanlineBasic<float>(psInfo, nY, nXStart, nXEnd, dfVariant);
            break;
        case GDT_Float64:
            gvBurnScanlineBasic<double>(psInfo, nY, nXStart, nXEnd, dfVariant);
            break;
        case GDT_CInt16:
        case GDT_CInt32:
        case GDT_CFloat16:
        case GDT_CFloat32:
        case GDT_CFloat64:
        case GDT_Unknown:
        case GDT_TypeCount:
            CPLAssert(false);
            break;
    }
}

/************************************************************************/
/*                        gvBurnPointBasic()                            */
/************************************************************************/
template <typename T>
static inline void gvBurnPointBasic(GDALRasterizeInfo *psInfo, int nY, int nX,
                                    double dfVariant)

{
    for (int iBand = 0; iBand < psInfo->nBands; iBand++)
    {
        double burnValue =
            (psInfo->burnValues.double_values[iBand] +
             ((psInfo->eBurnValueSource == GBV_UserBurnValue) ? 0 : dfVariant));
        unsigned char *pbyInsert =
            psInfo->pabyChunkBuf + iBand * psInfo->nBandSpace +
            nY * psInfo->nLineSpace + nX * psInfo->nPixelSpace;

        T *pbyPixel = reinterpret_cast<T *>(pbyInsert);
        if (psInfo->eMergeAlg == GRMA_Add)
            burnValue += static_cast<double>(*pbyPixel);
        GDALCopyWord(burnValue, *pbyPixel);
    }
}

static inline void gvBurnPointInt64UserBurnValue(GDALRasterizeInfo *psInfo,
                                                 int nY, int nX)

{
    for (int iBand = 0; iBand < psInfo->nBands; iBand++)
    {
        std::int64_t burnValue = psInfo->burnValues.int64_values[iBand];
        unsigned char *pbyInsert =
            psInfo->pabyChunkBuf + iBand * psInfo->nBandSpace +
            nY * psInfo->nLineSpace + nX * psInfo->nPixelSpace;

        std::int64_t *pbyPixel = reinterpret_cast<std::int64_t *>(pbyInsert);
        if (psInfo->eMergeAlg == GRMA_Add)
        {
            burnValue = SaturatedAddSigned(burnValue, *pbyPixel);
        }
        *pbyPixel = burnValue;
    }
}

/************************************************************************/
/*                            gvBurnPoint()                             */
/************************************************************************/
static void gvBurnPoint(GDALRasterizeInfo *psInfo, int nY, int nX,
                        double dfVariant)

{

    CPLAssert(nY >= 0 && nY < psInfo->nYSize);
    CPLAssert(nX >= 0 && nX < psInfo->nXSize);

    if (psInfo->poSetVisitedPoints)
    {
        const uint64_t nKey = MakeKey(nY, nX);
        if (psInfo->poSetVisitedPoints->find(nKey) ==
            psInfo->poSetVisitedPoints->end())
        {
            if (psInfo->bFillSetVisitedPoints)
                psInfo->poSetVisitedPoints->insert(nKey);
        }
        else
        {
            return;
        }
    }

    if (psInfo->eBurnValueType == GDT_Int64)
    {
        if (psInfo->eType == GDT_Int64 &&
            psInfo->eBurnValueSource == GBV_UserBurnValue)
        {
            gvBurnPointInt64UserBurnValue(psInfo, nY, nX);
        }
        else
        {
            CPLAssert(false);
        }
        return;
    }

    switch (psInfo->eType)
    {
        case GDT_Byte:
            gvBurnPointBasic<GByte>(psInfo, nY, nX, dfVariant);
            break;
        case GDT_Int8:
            gvBurnPointBasic<GInt8>(psInfo, nY, nX, dfVariant);
            break;
        case GDT_Int16:
            gvBurnPointBasic<GInt16>(psInfo, nY, nX, dfVariant);
            break;
        case GDT_UInt16:
            gvBurnPointBasic<GUInt16>(psInfo, nY, nX, dfVariant);
            break;
        case GDT_Int32:
            gvBurnPointBasic<GInt32>(psInfo, nY, nX, dfVariant);
            break;
        case GDT_UInt32:
            gvBurnPointBasic<GUInt32>(psInfo, nY, nX, dfVariant);
            break;
        case GDT_Int64:
            gvBurnPointBasic<std::int64_t>(psInfo, nY, nX, dfVariant);
            break;
        case GDT_UInt64:
            gvBurnPointBasic<std::uint64_t>(psInfo, nY, nX, dfVariant);
            break;
        case GDT_Float16:
            gvBurnPointBasic<GFloat16>(psInfo, nY, nX, dfVariant);
            break;
        case GDT_Float32:
            gvBurnPointBasic<float>(psInfo, nY, nX, dfVariant);
            break;
        case GDT_Float64:
            gvBurnPointBasic<double>(psInfo, nY, nX, dfVariant);
            break;
        case GDT_CInt16:
        case GDT_CInt32:
        case GDT_CFloat16:
        case GDT_CFloat32:
        case GDT_CFloat64:
        case GDT_Unknown:
        case GDT_TypeCount:
            CPLAssert(false);
    }
}

/************************************************************************/
/*                    GDALCollectRingsFromGeometry()                    */
/************************************************************************/

static void GDALCollectRingsFromGeometry(const OGRGeometry *poShape,
                                         std::vector<double> &aPointX,
                                         std::vector<double> &aPointY,
                                         std::vector<double> &aPointVariant,
                                         std::vector<int> &aPartSize,
                                         GDALBurnValueSrc eBurnValueSrc)

{
    if (poShape == nullptr || poShape->IsEmpty())
        return;

    const OGRwkbGeometryType eFlatType = wkbFlatten(poShape->getGeometryType());

    if (eFlatType == wkbPoint)
    {
        const auto poPoint = poShape->toPoint();

        aPointX.push_back(poPoint->getX());
        aPointY.push_back(poPoint->getY());
        aPartSize.push_back(1);
        if (eBurnValueSrc != GBV_UserBurnValue)
        {
            // TODO(schwehr): Why not have the option for M r18164?
            // switch( eBurnValueSrc )
            // {
            // case GBV_Z:*/
            aPointVariant.push_back(poPoint->getZ());
            // break;
            // case GBV_M:
            //    aPointVariant.reserve( nNewCount );
            //    aPointVariant.push_back( poPoint->getM() );
        }
    }
    else if (EQUAL(poShape->getGeometryName(), "LINEARRING"))
    {
        const auto poRing = poShape->toLinearRing();
        const int nCount = poRing->getNumPoints();
        const size_t nNewCount = aPointX.size() + static_cast<size_t>(nCount);

        aPointX.reserve(nNewCount);
        aPointY.reserve(nNewCount);
        if (eBurnValueSrc != GBV_UserBurnValue)
            aPointVariant.reserve(nNewCount);
        if (poRing->isClockwise())
        {
            for (int i = 0; i < nCount; i++)
            {
                aPointX.push_back(poRing->getX(i));
                aPointY.push_back(poRing->getY(i));
                if (eBurnValueSrc != GBV_UserBurnValue)
                {
                    /*switch( eBurnValueSrc )
                    {
                    case GBV_Z:*/
                    aPointVariant.push_back(poRing->getZ(i));
                    /*break;
                case GBV_M:
                    aPointVariant.push_back( poRing->getM(i) );
                }*/
                }
            }
        }
        else
        {
            for (int i = nCount - 1; i >= 0; i--)
            {
                aPointX.push_back(poRing->getX(i));
                aPointY.push_back(poRing->getY(i));
                if (eBurnValueSrc != GBV_UserBurnValue)
                {
                    /*switch( eBurnValueSrc )
                    {
                    case GBV_Z:*/
                    aPointVariant.push_back(poRing->getZ(i));
                    /*break;
                case GBV_M:
                    aPointVariant.push_back( poRing->getM(i) );
                }*/
                }
            }
        }
        aPartSize.push_back(nCount);
    }
    else if (eFlatType == wkbLineString)
    {
        const auto poLine = poShape->toLineString();
        const int nCount = poLine->getNumPoints();
        const size_t nNewCount = aPointX.size() + static_cast<size_t>(nCount);

        aPointX.reserve(nNewCount);
        aPointY.reserve(nNewCount);
        if (eBurnValueSrc != GBV_UserBurnValue)
            aPointVariant.reserve(nNewCount);
        for (int i = nCount - 1; i >= 0; i--)
        {
            aPointX.push_back(poLine->getX(i));
            aPointY.push_back(poLine->getY(i));
            if (eBurnValueSrc != GBV_UserBurnValue)
            {
                /*switch( eBurnValueSrc )
                {
                    case GBV_Z:*/
                aPointVariant.push_back(poLine->getZ(i));
                /*break;
            case GBV_M:
                aPointVariant.push_back( poLine->getM(i) );
        }*/
            }
        }
        aPartSize.push_back(nCount);
    }
    else if (eFlatType == wkbPolygon)
    {
        const auto poPolygon = poShape->toPolygon();

        GDALCollectRingsFromGeometry(poPolygon->getExteriorRing(), aPointX,
                                     aPointY, aPointVariant, aPartSize,
                                     eBurnValueSrc);

        for (int i = 0; i < poPolygon->getNumInteriorRings(); i++)
            GDALCollectRingsFromGeometry(poPolygon->getInteriorRing(i), aPointX,
                                         aPointY, aPointVariant, aPartSize,
                                         eBurnValueSrc);
    }
    else if (eFlatType == wkbMultiPoint || eFlatType == wkbMultiLineString ||
             eFlatType == wkbMultiPolygon || eFlatType == wkbGeometryCollection)
    {
        const auto poGC = poShape->toGeometryCollection();
        for (int i = 0; i < poGC->getNumGeometries(); i++)
            GDALCollectRingsFromGeometry(poGC->getGeometryRef(i), aPointX,
                                         aPointY, aPointVariant, aPartSize,
                                         eBurnValueSrc);
    }
    else
    {
        CPLDebug("GDAL", "Rasterizer ignoring non-polygonal geometry.");
    }
}

/************************************************************************
 *                       gv_rasterize_one_shape()
 *
 * @param pabyChunkBuf buffer to which values will be burned
 * @param nXOff chunk column offset from left edge of raster
 * @param nYOff chunk scanline offset from top of raster
 * @param nXSize number of columns in chunk
 * @param nYSize number of rows in chunk
 * @param nBands number of bands in chunk
 * @param eType data type of pabyChunkBuf
 * @param nPixelSpace number of bytes between adjacent pixels in chunk
 *                    (0 to calculate automatically)
 * @param nLineSpace number of bytes between adjacent scanlines in chunk
 *                   (0 to calculate automatically)
 * @param nBandSpace number of bytes between adjacent bands in chunk
 *                   (0 to calculate automatically)
 * @param bAllTouched burn value to all touched pixels?
 * @param poShape geometry to rasterize, in original coordinates
 * @param eBurnValueType type of value to be burned (must be Float64 or Int64)
 * @param padfBurnValues array of nBands values to burn (Float64), or nullptr
 * @param panBurnValues array of nBands values to burn (Int64), or nullptr
 * @param eBurnValueSrc whether to burn values from padfBurnValues /
 *                      panBurnValues, or from the Z or M values of poShape
 * @param eMergeAlg whether the burn value should replace or be added to the
 *                  existing values
 * @param pfnTransformer transformer from CRS of geometry to pixel/line
 *                       coordinates of raster
 * @param pTransformArg arguments to pass to pfnTransformer
 ************************************************************************/
static void gv_rasterize_one_shape(
    unsigned char *pabyChunkBuf, int nXOff, int nYOff, int nXSize, int nYSize,
    int nBands, GDALDataType eType, int nPixelSpace, GSpacing nLineSpace,
    GSpacing nBandSpace, int bAllTouched, const OGRGeometry *poShape,
    GDALDataType eBurnValueType, const double *padfBurnValues,
    const int64_t *panBurnValues, GDALBurnValueSrc eBurnValueSrc,
    GDALRasterMergeAlg eMergeAlg, GDALTransformerFunc pfnTransformer,
    void *pTransformArg)

{
    if (poShape == nullptr || poShape->IsEmpty())
        return;
    const auto eGeomType = wkbFlatten(poShape->getGeometryType());

    if ((eGeomType == wkbMultiLineString || eGeomType == wkbMultiPolygon ||
         eGeomType == wkbGeometryCollection) &&
        eMergeAlg == GRMA_Replace)
    {
        // Speed optimization: in replace mode, we can rasterize each part of
        // a geometry collection separately.
        const auto poGC = poShape->toGeometryCollection();
        for (const auto poPart : *poGC)
        {
            gv_rasterize_one_shape(
                pabyChunkBuf, nXOff, nYOff, nXSize, nYSize, nBands, eType,
                nPixelSpace, nLineSpace, nBandSpace, bAllTouched, poPart,
                eBurnValueType, padfBurnValues, panBurnValues, eBurnValueSrc,
                eMergeAlg, pfnTransformer, pTransformArg);
        }
        return;
    }

    if (nPixelSpace == 0)
    {
        nPixelSpace = GDALGetDataTypeSizeBytes(eType);
    }
    if (nLineSpace == 0)
    {
        nLineSpace = static_cast<GSpacing>(nXSize) * nPixelSpace;
    }
    if (nBandSpace == 0)
    {
        nBandSpace = nYSize * nLineSpace;
    }

    GDALRasterizeInfo sInfo;
    sInfo.nXSize = nXSize;
    sInfo.nYSize = nYSize;
    sInfo.nBands = nBands;
    sInfo.pabyChunkBuf = pabyChunkBuf;
    sInfo.eType = eType;
    sInfo.nPixelSpace = nPixelSpace;
    sInfo.nLineSpace = nLineSpace;
    sInfo.nBandSpace = nBandSpace;
    sInfo.eBurnValueType = eBurnValueType;
    if (eBurnValueType == GDT_Float64)
        sInfo.burnValues.double_values = padfBurnValues;
    else if (eBurnValueType == GDT_Int64)
        sInfo.burnValues.int64_values = panBurnValues;
    else
    {
        CPLAssert(false);
    }
    sInfo.eBurnValueSource = eBurnValueSrc;
    sInfo.eMergeAlg = eMergeAlg;
    sInfo.bFillSetVisitedPoints = false;
    sInfo.poSetVisitedPoints = nullptr;

    /* -------------------------------------------------------------------- */
    /*      Transform polygon geometries into a set of rings and a part     */
    /*      size list.                                                      */
    /* -------------------------------------------------------------------- */
    std::vector<double>
        aPointX;  // coordinate X values from all rings/components
    std::vector<double>
        aPointY;  // coordinate Y values from all rings/components
    std::vector<double> aPointVariant;  // coordinate Z values
    std::vector<int> aPartSize;  // number of X/Y/(Z) values associated with
                                 // each ring/component

    GDALCollectRingsFromGeometry(poShape, aPointX, aPointY, aPointVariant,
                                 aPartSize, eBurnValueSrc);

    /* -------------------------------------------------------------------- */
    /*      Transform points if needed.                                     */
    /* -------------------------------------------------------------------- */
    if (pfnTransformer != nullptr)
    {
        int *panSuccess =
            static_cast<int *>(CPLCalloc(sizeof(int), aPointX.size()));

        // TODO: We need to add all appropriate error checking at some point.
        pfnTransformer(pTransformArg, FALSE, static_cast<int>(aPointX.size()),
                       aPointX.data(), aPointY.data(), nullptr, panSuccess);
        CPLFree(panSuccess);
    }

    /* -------------------------------------------------------------------- */
    /*      Shift to account for the buffer offset of this buffer.          */
    /* -------------------------------------------------------------------- */
    for (unsigned int i = 0; i < aPointX.size(); i++)
        aPointX[i] -= nXOff;
    for (unsigned int i = 0; i < aPointY.size(); i++)
        aPointY[i] -= nYOff;

    /* -------------------------------------------------------------------- */
    /*      Perform the rasterization.                                      */
    /*      According to the C++ Standard/23.2.4, elements of a vector are  */
    /*      stored in continuous memory block.                              */
    /* -------------------------------------------------------------------- */

    switch (eGeomType)
    {
        case wkbPoint:
        case wkbMultiPoint:
            GDALdllImagePoint(
                sInfo.nXSize, nYSize, static_cast<int>(aPartSize.size()),
                aPartSize.data(), aPointX.data(), aPointY.data(),
                (eBurnValueSrc == GBV_UserBurnValue) ? nullptr
                                                     : aPointVariant.data(),
                gvBurnPoint, &sInfo);
            break;
        case wkbLineString:
        case wkbMultiLineString:
        {
            if (eMergeAlg == GRMA_Add)
            {
                sInfo.bFillSetVisitedPoints = true;
                sInfo.poSetVisitedPoints = new std::set<uint64_t>();
            }
            if (bAllTouched)
                GDALdllImageLineAllTouched(
                    sInfo.nXSize, nYSize, static_cast<int>(aPartSize.size()),
                    aPartSize.data(), aPointX.data(), aPointY.data(),
                    (eBurnValueSrc == GBV_UserBurnValue) ? nullptr
                                                         : aPointVariant.data(),
                    gvBurnPoint, &sInfo, eMergeAlg == GRMA_Add, false);
            else
                GDALdllImageLine(
                    sInfo.nXSize, nYSize, static_cast<int>(aPartSize.size()),
                    aPartSize.data(), aPointX.data(), aPointY.data(),
                    (eBurnValueSrc == GBV_UserBurnValue) ? nullptr
                                                         : aPointVariant.data(),
                    gvBurnPoint, &sInfo);
        }
        break;

        default:
        {
            if (eMergeAlg == GRMA_Add)
            {
                sInfo.bFillSetVisitedPoints = true;
                sInfo.poSetVisitedPoints = new std::set<uint64_t>();
            }
            if (bAllTouched)
            {
                // Reverting the variants to the first value because the
                // polygon is filled using the variant from the first point of
                // the first segment. Should be removed when the code to full
                // polygons more appropriately is added.
                if (eBurnValueSrc == GBV_UserBurnValue)
                {
                    GDALdllImageLineAllTouched(
                        sInfo.nXSize, nYSize,
                        static_cast<int>(aPartSize.size()), aPartSize.data(),
                        aPointX.data(), aPointY.data(), nullptr, gvBurnPoint,
                        &sInfo, eMergeAlg == GRMA_Add, true);
                }
                else
                {
                    for (unsigned int i = 0, n = 0;
                         i < static_cast<unsigned int>(aPartSize.size()); i++)
                    {
                        for (int j = 0; j < aPartSize[i]; j++)
                            aPointVariant[n++] = aPointVariant[0];
                    }

                    GDALdllImageLineAllTouched(
                        sInfo.nXSize, nYSize,
                        static_cast<int>(aPartSize.size()), aPartSize.data(),
                        aPointX.data(), aPointY.data(), aPointVariant.data(),
                        gvBurnPoint, &sInfo, eMergeAlg == GRMA_Add, true);
                }
            }
            sInfo.bFillSetVisitedPoints = false;
            GDALdllImageFilledPolygon(
                sInfo.nXSize, nYSize, static_cast<int>(aPartSize.size()),
                aPartSize.data(), aPointX.data(), aPointY.data(),
                (eBurnValueSrc == GBV_UserBurnValue) ? nullptr
                                                     : aPointVariant.data(),
                gvBurnScanline, &sInfo, eMergeAlg == GRMA_Add);
        }
        break;
    }

    delete sInfo.poSetVisitedPoints;
}

/************************************************************************/
/*                        GDALRasterizeOptions()                        */
/*                                                                      */
/*      Recognise a few rasterize options used by all three entry       */
/*      points.                                                         */
/************************************************************************/

static CPLErr GDALRasterizeOptions(CSLConstList papszOptions, int *pbAllTouched,
                                   GDALBurnValueSrc *peBurnValueSource,
                                   GDALRasterMergeAlg *peMergeAlg,
                                   GDALRasterizeOptim *peOptim)
{
    *pbAllTouched = CPLFetchBool(papszOptions, "ALL_TOUCHED", false);

    const char *pszOpt = CSLFetchNameValue(papszOptions, "BURN_VALUE_FROM");
    *peBurnValueSource = GBV_UserBurnValue;
    if (pszOpt)
    {
        if (EQUAL(pszOpt, "Z"))
        {
            *peBurnValueSource = GBV_Z;
        }
        // else if( EQUAL(pszOpt, "M"))
        //     eBurnValueSource = GBV_M;
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unrecognized value '%s' for BURN_VALUE_FROM.", pszOpt);
            return CE_Failure;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      MERGE_ALG=[REPLACE]/ADD                                         */
    /* -------------------------------------------------------------------- */
    *peMergeAlg = GRMA_Replace;
    pszOpt = CSLFetchNameValue(papszOptions, "MERGE_ALG");
    if (pszOpt)
    {
        if (EQUAL(pszOpt, "ADD"))
        {
            *peMergeAlg = GRMA_Add;
        }
        else if (EQUAL(pszOpt, "REPLACE"))
        {
            *peMergeAlg = GRMA_Replace;
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unrecognized value '%s' for MERGE_ALG.", pszOpt);
            return CE_Failure;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      OPTIM=[AUTO]/RASTER/VECTOR                                      */
    /* -------------------------------------------------------------------- */
    pszOpt = CSLFetchNameValue(papszOptions, "OPTIM");
    if (pszOpt)
    {
        if (peOptim)
        {
            *peOptim = GRO_Auto;
            if (EQUAL(pszOpt, "RASTER"))
            {
                *peOptim = GRO_Raster;
            }
            else if (EQUAL(pszOpt, "VECTOR"))
            {
                *peOptim = GRO_Vector;
            }
            else if (EQUAL(pszOpt, "AUTO"))
            {
                *peOptim = GRO_Auto;
            }
            else
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Unrecognized value '%s' for OPTIM.", pszOpt);
                return CE_Failure;
            }
        }
        else
        {
            CPLError(CE_Warning, CPLE_NotSupported,
                     "Option OPTIM is not supported by this function");
        }
    }

    return CE_None;
}

/************************************************************************/
/*                      GDALRasterizeGeometries()                       */
/************************************************************************/

static CPLErr GDALRasterizeGeometriesInternal(
    GDALDatasetH hDS, int nBandCount, const int *panBandList, int nGeomCount,
    const OGRGeometryH *pahGeometries, GDALTransformerFunc pfnTransformer,
    void *pTransformArg, GDALDataType eBurnValueType,
    const double *padfGeomBurnValues, const int64_t *panGeomBurnValues,
    CSLConstList papszOptions, GDALProgressFunc pfnProgress,
    void *pProgressArg);

/**
 * Burn geometries into raster.
 *
 * Rasterize a list of geometric objects into a raster dataset.  The
 * geometries are passed as an array of OGRGeometryH handlers.
 *
 * If the geometries are in the georeferenced coordinates of the raster
 * dataset, then the pfnTransform may be passed in NULL and one will be
 * derived internally from the geotransform of the dataset.  The transform
 * needs to transform the geometry locations into pixel/line coordinates
 * on the raster dataset.
 *
 * The output raster may be of any GDAL supported datatype. An explicit list
 * of burn values for each geometry for each band must be passed in.
 *
 * The papszOption list of options currently only supports one option. The
 * "ALL_TOUCHED" option may be enabled by setting it to "TRUE".
 *
 * @param hDS output data, must be opened in update mode.
 * @param nBandCount the number of bands to be updated.
 * @param panBandList the list of bands to be updated.
 * @param nGeomCount the number of geometries being passed in pahGeometries.
 * @param pahGeometries the array of geometries to burn in.
 * @param pfnTransformer transformation to apply to geometries to put into
 * pixel/line coordinates on raster.  If NULL a geotransform based one will
 * be created internally.
 * @param pTransformArg callback data for transformer.
 * @param padfGeomBurnValues the array of values to burn into the raster.
 * There should be nBandCount values for each geometry.
 * @param papszOptions special options controlling rasterization
 * <ul>
 * <li>"ALL_TOUCHED": May be set to TRUE to set all pixels touched
 * by the line or polygons, not just those whose center is within the polygon
 * (behavior is unspecified when the polygon is just touching the pixel center)
 * or that are selected by Brezenham's line algorithm.  Defaults to FALSE.</li>
 * <li>"BURN_VALUE_FROM": May be set to "Z" to use the Z values of the
 * geometries. dfBurnValue is added to this before burning.
 * Defaults to GDALBurnValueSrc.GBV_UserBurnValue in which case just the
 * dfBurnValue is burned. This is implemented only for points and lines for
 * now. The M value may be supported in the future.</li>
 * <li>"MERGE_ALG": May be REPLACE (the default) or ADD.  REPLACE results in
 * overwriting of value, while ADD adds the new value to the existing raster,
 * suitable for heatmaps for instance.</li>
 * <li>"CHUNKYSIZE": The height in lines of the chunk to operate on.
 * The larger the chunk size the less times we need to make a pass through all
 * the shapes. If it is not set or set to zero the default chunk size will be
 * used. Default size will be estimated based on the GDAL cache buffer size
 * using formula: cache_size_bytes/scanline_size_bytes, so the chunk will
 * not exceed the cache. Not used in OPTIM=RASTER mode.</li>
 * <li>"OPTIM": May be set to "AUTO", "RASTER", "VECTOR". Force the algorithm
 * used (results are identical). The raster mode is used in most cases and
 * optimise read/write operations. The vector mode is useful with a decent
 * amount of input features and optimize the CPU use. That mode has to be used
 * with tiled images to be efficient. The auto mode (the default) will chose
 * the algorithm based on input and output properties.
 * </li>
 * </ul>
 * @param pfnProgress the progress function to report completion.
 * @param pProgressArg callback data for progress function.
 *
 * @return CE_None on success or CE_Failure on error.
 *
 * <strong>Example</strong><br>
 * GDALRasterizeGeometries rasterize output to MEM Dataset :<br>
 * @code
 *     int nBufXSize      = 1024;
 *     int nBufYSize      = 1024;
 *     int nBandCount     = 1;
 *     GDALDataType eType = GDT_Byte;
 *     int nDataTypeSize  = GDALGetDataTypeSizeBytes(eType);
 *
 *     void* pData = CPLCalloc( nBufXSize*nBufYSize*nBandCount, nDataTypeSize );
 *     char memdsetpath[1024];
 *     sprintf(memdsetpath,"MEM:::DATAPOINTER=0x%p,PIXELS=%d,LINES=%d,"
 *             "BANDS=%d,DATATYPE=%s,PIXELOFFSET=%d,LINEOFFSET=%d",
 *             pData,nBufXSize,nBufYSize,nBandCount,GDALGetDataTypeName(eType),
 *             nBandCount*nDataTypeSize, nBufXSize*nBandCount*nDataTypeSize );
 *
 *      // Open Memory Dataset
 *      GDALDatasetH hMemDset = GDALOpen(memdsetpath, GA_Update);
 *      // or create it as follows
 *      // GDALDriverH hMemDriver = GDALGetDriverByName("MEM");
 *      // GDALDatasetH hMemDset = GDALCreate(hMemDriver, "", nBufXSize,
 *                                      nBufYSize, nBandCount, eType, NULL);
 *
 *      double adfGeoTransform[6];
 *      // Assign GeoTransform parameters,Omitted here.
 *
 *      GDALSetGeoTransform(hMemDset,adfGeoTransform);
 *      GDALSetProjection(hMemDset,pszProjection); // Can not
 *
 *      // Do something ...
 *      // Need an array of OGRGeometry objects,The assumption here is pahGeoms
 *
 *      int bandList[3] = { 1, 2, 3};
 *      std::vector<double> geomBurnValue(nGeomCount*nBandCount,255.0);
 *      CPLErr err = GDALRasterizeGeometries(
 *          hMemDset, nBandCount, bandList, nGeomCount, pahGeoms, pfnTransformer,
 *          pTransformArg, geomBurnValue.data(), papszOptions,
 *          pfnProgress, pProgressArg);
 *      if( err != CE_None )
 *      {
 *          // Do something ...
 *      }
 *      GDALClose(hMemDset);
 *      CPLFree(pData);
 *@endcode
 */

CPLErr GDALRasterizeGeometries(
    GDALDatasetH hDS, int nBandCount, const int *panBandList, int nGeomCount,
    const OGRGeometryH *pahGeometries, GDALTransformerFunc pfnTransformer,
    void *pTransformArg, const double *padfGeomBurnValues,
    CSLConstList papszOptions, GDALProgressFunc pfnProgress, void *pProgressArg)

{
    VALIDATE_POINTER1(hDS, "GDALRasterizeGeometries", CE_Failure);

    return GDALRasterizeGeometriesInternal(
        hDS, nBandCount, panBandList, nGeomCount, pahGeometries, pfnTransformer,
        pTransformArg, GDT_Float64, padfGeomBurnValues, nullptr, papszOptions,
        pfnProgress, pProgressArg);
}

/**
 * Burn geometries into raster.
 *
 * Same as GDALRasterizeGeometries(), except that the burn values array is
 * of type Int64. And the datatype of the output raster *must* be GDT_Int64.
 *
 * @since GDAL 3.5
 */
CPLErr GDALRasterizeGeometriesInt64(
    GDALDatasetH hDS, int nBandCount, const int *panBandList, int nGeomCount,
    const OGRGeometryH *pahGeometries, GDALTransformerFunc pfnTransformer,
    void *pTransformArg, const int64_t *panGeomBurnValues,
    CSLConstList papszOptions, GDALProgressFunc pfnProgress, void *pProgressArg)

{
    VALIDATE_POINTER1(hDS, "GDALRasterizeGeometriesInt64", CE_Failure);

    return GDALRasterizeGeometriesInternal(
        hDS, nBandCount, panBandList, nGeomCount, pahGeometries, pfnTransformer,
        pTransformArg, GDT_Int64, nullptr, panGeomBurnValues, papszOptions,
        pfnProgress, pProgressArg);
}

static CPLErr GDALRasterizeGeometriesInternal(
    GDALDatasetH hDS, int nBandCount, const int *panBandList, int nGeomCount,
    const OGRGeometryH *pahGeometries, GDALTransformerFunc pfnTransformer,
    void *pTransformArg, GDALDataType eBurnValueType,
    const double *padfGeomBurnValues, const int64_t *panGeomBurnValues,
    CSLConstList papszOptions, GDALProgressFunc pfnProgress, void *pProgressArg)

{
    if (pfnProgress == nullptr)
        pfnProgress = GDALDummyProgress;

    GDALDataset *poDS = GDALDataset::FromHandle(hDS);
    /* -------------------------------------------------------------------- */
    /*      Do some rudimentary arg checking.                               */
    /* -------------------------------------------------------------------- */
    if (nBandCount == 0 || nGeomCount == 0)
    {
        pfnProgress(1.0, "", pProgressArg);
        return CE_None;
    }

    if (eBurnValueType == GDT_Int64)
    {
        for (int i = 0; i < nBandCount; i++)
        {
            GDALRasterBand *poBand = poDS->GetRasterBand(panBandList[i]);
            if (poBand == nullptr)
                return CE_Failure;
            if (poBand->GetRasterDataType() != GDT_Int64)
            {
                CPLError(CE_Failure, CPLE_NotSupported,
                         "GDALRasterizeGeometriesInt64() only supported on "
                         "Int64 raster");
                return CE_Failure;
            }
        }
    }

    // Prototype band.
    GDALRasterBand *poBand = poDS->GetRasterBand(panBandList[0]);
    if (poBand == nullptr)
        return CE_Failure;

    /* -------------------------------------------------------------------- */
    /*      Options                                                         */
    /* -------------------------------------------------------------------- */
    int bAllTouched = FALSE;
    GDALBurnValueSrc eBurnValueSource = GBV_UserBurnValue;
    GDALRasterMergeAlg eMergeAlg = GRMA_Replace;
    GDALRasterizeOptim eOptim = GRO_Auto;
    if (GDALRasterizeOptions(papszOptions, &bAllTouched, &eBurnValueSource,
                             &eMergeAlg, &eOptim) == CE_Failure)
    {
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      If we have no transformer, assume the geometries are in file    */
    /*      georeferenced coordinates, and create a transformer to          */
    /*      convert that to pixel/line coordinates.                         */
    /*                                                                      */
    /*      We really just need to apply an affine transform, but for       */
    /*      simplicity we use the more general GenImgProjTransformer.       */
    /* -------------------------------------------------------------------- */
    bool bNeedToFreeTransformer = false;

    if (pfnTransformer == nullptr)
    {
        bNeedToFreeTransformer = true;

        char **papszTransformerOptions = nullptr;
        GDALGeoTransform gt;
        if (poDS->GetGeoTransform(gt) != CE_None && poDS->GetGCPCount() == 0 &&
            poDS->GetMetadata("RPC") == nullptr)
        {
            papszTransformerOptions = CSLSetNameValue(
                papszTransformerOptions, "DST_METHOD", "NO_GEOTRANSFORM");
        }

        pTransformArg = GDALCreateGenImgProjTransformer2(
            nullptr, hDS, papszTransformerOptions);
        CSLDestroy(papszTransformerOptions);

        pfnTransformer = GDALGenImgProjTransform;
        if (pTransformArg == nullptr)
        {
            return CE_Failure;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Choice of optimisation in auto mode. Use vector optim :         */
    /*      1) if output is tiled                                           */
    /*      2) if large number of features is present (>10000)              */
    /*      3) if the nb of pixels > 50 * nb of features (not-too-small ft) */
    /* -------------------------------------------------------------------- */
    int nXBlockSize, nYBlockSize;
    poBand->GetBlockSize(&nXBlockSize, &nYBlockSize);

    if (eOptim == GRO_Auto)
    {
        eOptim = GRO_Raster;
        // TODO make more tests with various inputs/outputs to adjust the
        // parameters
        if (nYBlockSize > 1 && nGeomCount > 10000 &&
            (poBand->GetXSize() * static_cast<long long>(poBand->GetYSize()) /
                 nGeomCount >
             50))
        {
            eOptim = GRO_Vector;
            CPLDebug("GDAL", "The vector optim has been chosen automatically");
        }
    }

    /* -------------------------------------------------------------------- */
    /*      The original algorithm                                          */
    /*      Optimized for raster writing                                    */
    /*      (optimal on a small number of large vectors)                    */
    /* -------------------------------------------------------------------- */
    unsigned char *pabyChunkBuf;
    CPLErr eErr = CE_None;
    if (eOptim == GRO_Raster)
    {
        /* --------------------------------------------------------------------
         */
        /*      Establish a chunksize to operate on.  The larger the chunk */
        /*      size the less times we need to make a pass through all the */
        /*      shapes. */
        /* --------------------------------------------------------------------
         */
        const GDALDataType eType =
            GDALGetNonComplexDataType(poBand->GetRasterDataType());

        const uint64_t nScanlineBytes = static_cast<uint64_t>(nBandCount) *
                                        poDS->GetRasterXSize() *
                                        GDALGetDataTypeSizeBytes(eType);

#if SIZEOF_VOIDP < 8
        // Only on 32-bit systems and in pathological cases
        if (nScanlineBytes > std::numeric_limits<size_t>::max())
        {
            CPLError(CE_Failure, CPLE_OutOfMemory, "Too big raster");
            if (bNeedToFreeTransformer)
                GDALDestroyTransformer(pTransformArg);
            return CE_Failure;
        }
#endif

        int nYChunkSize =
            atoi(CSLFetchNameValueDef(papszOptions, "CHUNKYSIZE", "0"));
        if (nYChunkSize <= 0)
        {
            const GIntBig nYChunkSize64 = GDALGetCacheMax64() / nScanlineBytes;
            const int knIntMax = std::numeric_limits<int>::max();
            nYChunkSize = nYChunkSize64 > knIntMax
                              ? knIntMax
                              : static_cast<int>(nYChunkSize64);
        }

        if (nYChunkSize < 1)
            nYChunkSize = 1;
        if (nYChunkSize > poDS->GetRasterYSize())
            nYChunkSize = poDS->GetRasterYSize();

        CPLDebug("GDAL", "Rasterizer operating on %d swaths of %d scanlines.",
                 DIV_ROUND_UP(poDS->GetRasterYSize(), nYChunkSize),
                 nYChunkSize);

        pabyChunkBuf = static_cast<unsigned char *>(VSI_MALLOC2_VERBOSE(
            nYChunkSize, static_cast<size_t>(nScanlineBytes)));
        if (pabyChunkBuf == nullptr)
        {
            if (bNeedToFreeTransformer)
                GDALDestroyTransformer(pTransformArg);
            return CE_Failure;
        }

        /* ====================================================================
         */
        /*      Loop over image in designated chunks. */
        /* ====================================================================
         */
        pfnProgress(0.0, nullptr, pProgressArg);

        for (int iY = 0; iY < poDS->GetRasterYSize() && eErr == CE_None;
             iY += nYChunkSize)
        {
            int nThisYChunkSize = nYChunkSize;
            if (nThisYChunkSize + iY > poDS->GetRasterYSize())
                nThisYChunkSize = poDS->GetRasterYSize() - iY;

            eErr = poDS->RasterIO(
                GF_Read, 0, iY, poDS->GetRasterXSize(), nThisYChunkSize,
                pabyChunkBuf, poDS->GetRasterXSize(), nThisYChunkSize, eType,
                nBandCount, panBandList, 0, 0, 0, nullptr);
            if (eErr != CE_None)
                break;

            for (int iShape = 0; iShape < nGeomCount; iShape++)
            {
                gv_rasterize_one_shape(
                    pabyChunkBuf, 0, iY, poDS->GetRasterXSize(),
                    nThisYChunkSize, nBandCount, eType, 0, 0, 0, bAllTouched,
                    OGRGeometry::FromHandle(pahGeometries[iShape]),
                    eBurnValueType,
                    padfGeomBurnValues
                        ? padfGeomBurnValues +
                              static_cast<size_t>(iShape) * nBandCount
                        : nullptr,
                    panGeomBurnValues
                        ? panGeomBurnValues +
                              static_cast<size_t>(iShape) * nBandCount
                        : nullptr,
                    eBurnValueSource, eMergeAlg, pfnTransformer, pTransformArg);
            }

            eErr = poDS->RasterIO(
                GF_Write, 0, iY, poDS->GetRasterXSize(), nThisYChunkSize,
                pabyChunkBuf, poDS->GetRasterXSize(), nThisYChunkSize, eType,
                nBandCount, panBandList, 0, 0, 0, nullptr);

            if (!pfnProgress((iY + nThisYChunkSize) /
                                 static_cast<double>(poDS->GetRasterYSize()),
                             "", pProgressArg))
            {
                CPLError(CE_Failure, CPLE_UserInterrupt, "User terminated");
                eErr = CE_Failure;
            }
        }
    }
    /* -------------------------------------------------------------------- */
    /*      The new algorithm                                               */
    /*      Optimized to minimize the vector computation                    */
    /*      (optimal on a large number of vectors & tiled raster)           */
    /* -------------------------------------------------------------------- */
    else
    {
        /* --------------------------------------------------------------------
         */
        /*      Establish a chunksize to operate on.  Its size is defined by */
        /*      the block size of the output file. */
        /* --------------------------------------------------------------------
         */
        const int nXBlocks = DIV_ROUND_UP(poBand->GetXSize(), nXBlockSize);
        const int nYBlocks = DIV_ROUND_UP(poBand->GetYSize(), nYBlockSize);

        const GDALDataType eType =
            poBand->GetRasterDataType() == GDT_Byte ? GDT_Byte : GDT_Float64;

        const int nPixelSize = nBandCount * GDALGetDataTypeSizeBytes(eType);

        // rem: optimized for square blocks
        const GIntBig nbMaxBlocks64 =
            GDALGetCacheMax64() / nPixelSize / nYBlockSize / nXBlockSize;
        const int knIntMax = std::numeric_limits<int>::max();
        const int nbMaxBlocks = static_cast<int>(
            std::min(static_cast<GIntBig>(knIntMax / nPixelSize / nYBlockSize /
                                          nXBlockSize),
                     nbMaxBlocks64));
        const int nbBlocksX = std::max(
            1,
            std::min(static_cast<int>(sqrt(static_cast<double>(nbMaxBlocks))),
                     nXBlocks));
        const int nbBlocksY =
            std::max(1, std::min(nbMaxBlocks / nbBlocksX, nYBlocks));

        const uint64_t nChunkSize = static_cast<uint64_t>(nXBlockSize) *
                                    nbBlocksX * nYBlockSize * nbBlocksY;

#if SIZEOF_VOIDP < 8
        // Only on 32-bit systems and in pathological cases
        if (nChunkSize > std::numeric_limits<size_t>::max())
        {
            CPLError(CE_Failure, CPLE_OutOfMemory, "Too big raster");
            if (bNeedToFreeTransformer)
                GDALDestroyTransformer(pTransformArg);
            return CE_Failure;
        }
#endif

        pabyChunkBuf = static_cast<unsigned char *>(
            VSI_MALLOC2_VERBOSE(nPixelSize, static_cast<size_t>(nChunkSize)));
        if (pabyChunkBuf == nullptr)
        {
            if (bNeedToFreeTransformer)
                GDALDestroyTransformer(pTransformArg);
            return CE_Failure;
        }

        OGREnvelope sRasterEnvelope;
        sRasterEnvelope.MinX = 0;
        sRasterEnvelope.MinY = 0;
        sRasterEnvelope.MaxX = poDS->GetRasterXSize();
        sRasterEnvelope.MaxY = poDS->GetRasterYSize();

        /* --------------------------------------------------------------------
         */
        /*      loop over the vectorial geometries */
        /* --------------------------------------------------------------------
         */
        pfnProgress(0.0, nullptr, pProgressArg);
        for (int iShape = 0; iShape < nGeomCount; iShape++)
        {

            const OGRGeometry *poGeometry =
                OGRGeometry::FromHandle(pahGeometries[iShape]);
            if (poGeometry == nullptr || poGeometry->IsEmpty())
                continue;
            /* ------------------------------------------------------------ */
            /*      get the envelope of the geometry and transform it to    */
            /*      pixels coordinates.                                     */
            /* ------------------------------------------------------------ */
            OGREnvelope sGeomEnvelope;
            poGeometry->getEnvelope(&sGeomEnvelope);
            if (pfnTransformer != nullptr)
            {
                int anSuccessTransform[2] = {0};
                double apCorners[4];
                apCorners[0] = sGeomEnvelope.MinX;
                apCorners[1] = sGeomEnvelope.MaxX;
                apCorners[2] = sGeomEnvelope.MinY;
                apCorners[3] = sGeomEnvelope.MaxY;

                if (!pfnTransformer(pTransformArg, FALSE, 2, &(apCorners[0]),
                                    &(apCorners[2]), nullptr,
                                    anSuccessTransform) ||
                    !anSuccessTransform[0] || !anSuccessTransform[1])
                {
                    continue;
                }
                sGeomEnvelope.MinX = std::min(apCorners[0], apCorners[1]);
                sGeomEnvelope.MaxX = std::max(apCorners[0], apCorners[1]);
                sGeomEnvelope.MinY = std::min(apCorners[2], apCorners[3]);
                sGeomEnvelope.MaxY = std::max(apCorners[2], apCorners[3]);
            }
            if (!sGeomEnvelope.Intersects(sRasterEnvelope))
                continue;
            sGeomEnvelope.Intersect(sRasterEnvelope);
            CPLAssert(sGeomEnvelope.MinX >= 0 &&
                      sGeomEnvelope.MinX <= poDS->GetRasterXSize());
            CPLAssert(sGeomEnvelope.MinY >= 0 &&
                      sGeomEnvelope.MinY <= poDS->GetRasterYSize());
            CPLAssert(sGeomEnvelope.MaxX >= 0 &&
                      sGeomEnvelope.MaxX <= poDS->GetRasterXSize());
            CPLAssert(sGeomEnvelope.MaxY >= 0 &&
                      sGeomEnvelope.MaxY <= poDS->GetRasterYSize());
            const int minBlockX = int(sGeomEnvelope.MinX) / nXBlockSize;
            const int minBlockY = int(sGeomEnvelope.MinY) / nYBlockSize;
            const int maxBlockX = int(sGeomEnvelope.MaxX + 1) / nXBlockSize;
            const int maxBlockY = int(sGeomEnvelope.MaxY + 1) / nYBlockSize;

            /* ------------------------------------------------------------ */
            /*      loop over the blocks concerned by the geometry          */
            /*      (by packs of nbBlocksX x nbBlocksY)                     */
            /* ------------------------------------------------------------ */

            for (int xB = minBlockX; xB <= maxBlockX; xB += nbBlocksX)
            {
                for (int yB = minBlockY; yB <= maxBlockY; yB += nbBlocksY)
                {

                    /* --------------------------------------------------------------------
                     */
                    /*      ensure to stay in the image */
                    /* --------------------------------------------------------------------
                     */
                    int remSBX = std::min(maxBlockX - xB + 1, nbBlocksX);
                    int remSBY = std::min(maxBlockY - yB + 1, nbBlocksY);
                    int nThisXChunkSize = nXBlockSize * remSBX;
                    int nThisYChunkSize = nYBlockSize * remSBY;
                    if (xB * nXBlockSize + nThisXChunkSize >
                        poDS->GetRasterXSize())
                        nThisXChunkSize =
                            poDS->GetRasterXSize() - xB * nXBlockSize;
                    if (yB * nYBlockSize + nThisYChunkSize >
                        poDS->GetRasterYSize())
                        nThisYChunkSize =
                            poDS->GetRasterYSize() - yB * nYBlockSize;

                    /* --------------------------------------------------------------------
                     */
                    /*      read image / process buffer / write buffer */
                    /* --------------------------------------------------------------------
                     */
                    eErr = poDS->RasterIO(
                        GF_Read, xB * nXBlockSize, yB * nYBlockSize,
                        nThisXChunkSize, nThisYChunkSize, pabyChunkBuf,
                        nThisXChunkSize, nThisYChunkSize, eType, nBandCount,
                        panBandList, 0, 0, 0, nullptr);
                    if (eErr != CE_None)
                        break;

                    gv_rasterize_one_shape(
                        pabyChunkBuf, xB * nXBlockSize, yB * nYBlockSize,
                        nThisXChunkSize, nThisYChunkSize, nBandCount, eType, 0,
                        0, 0, bAllTouched,
                        OGRGeometry::FromHandle(pahGeometries[iShape]),
                        eBurnValueType,
                        padfGeomBurnValues
                            ? padfGeomBurnValues +
                                  static_cast<size_t>(iShape) * nBandCount
                            : nullptr,
                        panGeomBurnValues
                            ? panGeomBurnValues +
                                  static_cast<size_t>(iShape) * nBandCount
                            : nullptr,
                        eBurnValueSource, eMergeAlg, pfnTransformer,
                        pTransformArg);

                    eErr = poDS->RasterIO(
                        GF_Write, xB * nXBlockSize, yB * nYBlockSize,
                        nThisXChunkSize, nThisYChunkSize, pabyChunkBuf,
                        nThisXChunkSize, nThisYChunkSize, eType, nBandCount,
                        panBandList, 0, 0, 0, nullptr);
                    if (eErr != CE_None)
                        break;
                }
            }

            if (!pfnProgress(iShape / static_cast<double>(nGeomCount), "",
                             pProgressArg))
            {
                CPLError(CE_Failure, CPLE_UserInterrupt, "User terminated");
                eErr = CE_Failure;
            }
        }

        if (!pfnProgress(1., "", pProgressArg))
        {
            CPLError(CE_Failure, CPLE_UserInterrupt, "User terminated");
            eErr = CE_Failure;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      cleanup                                                         */
    /* -------------------------------------------------------------------- */
    VSIFree(pabyChunkBuf);

    if (bNeedToFreeTransformer)
        GDALDestroyTransformer(pTransformArg);

    return eErr;
}

/************************************************************************/
/*                        GDALRasterizeLayers()                         */
/************************************************************************/

/**
 * Burn geometries from the specified list of layers into raster.
 *
 * Rasterize all the geometric objects from a list of layers into a raster
 * dataset.  The layers are passed as an array of OGRLayerH handlers.
 *
 * If the geometries are in the georeferenced coordinates of the raster
 * dataset, then the pfnTransform may be passed in NULL and one will be
 * derived internally from the geotransform of the dataset.  The transform
 * needs to transform the geometry locations into pixel/line coordinates
 * on the raster dataset.
 *
 * The output raster may be of any GDAL supported datatype. An explicit list
 * of burn values for each layer for each band must be passed in.
 *
 * @param hDS output data, must be opened in update mode.
 * @param nBandCount the number of bands to be updated.
 * @param panBandList the list of bands to be updated.
 * @param nLayerCount the number of layers being passed in pahLayers array.
 * @param pahLayers the array of layers to burn in.
 * @param pfnTransformer transformation to apply to geometries to put into
 * pixel/line coordinates on raster.  If NULL a geotransform based one will
 * be created internally.
 * @param pTransformArg callback data for transformer.
 * @param padfLayerBurnValues the array of values to burn into the raster.
 * There should be nBandCount values for each layer.
 * @param papszOptions special options controlling rasterization:
 * <ul>
 * <li>"ATTRIBUTE": Identifies an attribute field on the features to be
 * used for a burn in value. The value will be burned into all output
 * bands. If specified, padfLayerBurnValues will not be used and can be a NULL
 * pointer.</li>
 * <li>"CHUNKYSIZE": The height in lines of the chunk to operate on.
 * The larger the chunk size the less times we need to make a pass through all
 * the shapes. If it is not set or set to zero the default chunk size will be
 * used. Default size will be estimated based on the GDAL cache buffer size
 * using formula: cache_size_bytes/scanline_size_bytes, so the chunk will
 * not exceed the cache.</li>
 * <li>"ALL_TOUCHED": May be set to TRUE to set all pixels touched
 * by the line or polygons, not just those whose center is within the polygon
 * (behavior is unspecified when the polygon is just touching the pixel center)
 * or that are selected by Brezenham's line algorithm.  Defaults to FALSE.
 * <li>"BURN_VALUE_FROM": May be set to "Z" to use the Z values of the</li>
 * geometries. The value from padfLayerBurnValues or the attribute field value
 * is added to this before burning. In default case dfBurnValue is burned as it
 * is. This is implemented properly only for points and lines for now. Polygons
 * will be burned using the Z value from the first point. The M value may be
 * supported in the future.</li>
 * <li>"MERGE_ALG": May be REPLACE (the default) or ADD.  REPLACE results in
 * overwriting of value, while ADD adds the new value to the existing raster,
 * suitable for heatmaps for instance.</li>
 * </ul>
 * @param pfnProgress the progress function to report completion.
 * @param pProgressArg callback data for progress function.
 *
 * @return CE_None on success or CE_Failure on error.
 */

CPLErr GDALRasterizeLayers(GDALDatasetH hDS, int nBandCount, int *panBandList,
                           int nLayerCount, OGRLayerH *pahLayers,
                           GDALTransformerFunc pfnTransformer,
                           void *pTransformArg, double *padfLayerBurnValues,
                           char **papszOptions, GDALProgressFunc pfnProgress,
                           void *pProgressArg)

{
    VALIDATE_POINTER1(hDS, "GDALRasterizeLayers", CE_Failure);

    if (pfnProgress == nullptr)
        pfnProgress = GDALDummyProgress;

    /* -------------------------------------------------------------------- */
    /*      Do some rudimentary arg checking.                               */
    /* -------------------------------------------------------------------- */
    if (nBandCount == 0 || nLayerCount == 0)
        return CE_None;

    GDALDataset *poDS = GDALDataset::FromHandle(hDS);

    // Prototype band.
    GDALRasterBand *poBand = poDS->GetRasterBand(panBandList[0]);
    if (poBand == nullptr)
        return CE_Failure;

    /* -------------------------------------------------------------------- */
    /*      Options                                                         */
    /* -------------------------------------------------------------------- */
    int bAllTouched = FALSE;
    GDALBurnValueSrc eBurnValueSource = GBV_UserBurnValue;
    GDALRasterMergeAlg eMergeAlg = GRMA_Replace;
    if (GDALRasterizeOptions(papszOptions, &bAllTouched, &eBurnValueSource,
                             &eMergeAlg, nullptr) == CE_Failure)
    {
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Establish a chunksize to operate on.  The larger the chunk      */
    /*      size the less times we need to make a pass through all the      */
    /*      shapes.                                                         */
    /* -------------------------------------------------------------------- */
    const char *pszYChunkSize = CSLFetchNameValue(papszOptions, "CHUNKYSIZE");

    const GDALDataType eType = poBand->GetRasterDataType();

    const int nScanlineBytes =
        nBandCount * poDS->GetRasterXSize() * GDALGetDataTypeSizeBytes(eType);

    int nYChunkSize = 0;
    if (!(pszYChunkSize && ((nYChunkSize = atoi(pszYChunkSize))) != 0))
    {
        const GIntBig nYChunkSize64 = GDALGetCacheMax64() / nScanlineBytes;
        nYChunkSize = static_cast<int>(
            std::min<GIntBig>(nYChunkSize64, std::numeric_limits<int>::max()));
    }

    if (nYChunkSize < 1)
        nYChunkSize = 1;
    if (nYChunkSize > poDS->GetRasterYSize())
        nYChunkSize = poDS->GetRasterYSize();

    CPLDebug("GDAL", "Rasterizer operating on %d swaths of %d scanlines.",
             DIV_ROUND_UP(poDS->GetRasterYSize(), nYChunkSize), nYChunkSize);
    unsigned char *pabyChunkBuf = static_cast<unsigned char *>(
        VSI_MALLOC2_VERBOSE(nYChunkSize, nScanlineBytes));
    if (pabyChunkBuf == nullptr)
    {
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      Read the image once for all layers if user requested to render  */
    /*      the whole raster in single chunk.                               */
    /* -------------------------------------------------------------------- */
    if (nYChunkSize == poDS->GetRasterYSize())
    {
        if (poDS->RasterIO(GF_Read, 0, 0, poDS->GetRasterXSize(), nYChunkSize,
                           pabyChunkBuf, poDS->GetRasterXSize(), nYChunkSize,
                           eType, nBandCount, panBandList, 0, 0, 0,
                           nullptr) != CE_None)
        {
            CPLFree(pabyChunkBuf);
            return CE_Failure;
        }
    }

    /* ==================================================================== */
    /*      Read the specified layers transforming and rasterizing          */
    /*      geometries.                                                     */
    /* ==================================================================== */
    CPLErr eErr = CE_None;
    const char *pszBurnAttribute = CSLFetchNameValue(papszOptions, "ATTRIBUTE");

    pfnProgress(0.0, nullptr, pProgressArg);

    for (int iLayer = 0; iLayer < nLayerCount; iLayer++)
    {
        OGRLayer *poLayer = reinterpret_cast<OGRLayer *>(pahLayers[iLayer]);

        if (!poLayer)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Layer element number %d is NULL, skipping.", iLayer);
            continue;
        }

        /* --------------------------------------------------------------------
         */
        /*      If the layer does not contain any features just skip it. */
        /*      Do not force the feature count, so if driver doesn't know */
        /*      exact number of features, go down the normal way. */
        /* --------------------------------------------------------------------
         */
        if (poLayer->GetFeatureCount(FALSE) == 0)
            continue;

        int iBurnField = -1;
        double *padfBurnValues = nullptr;

        if (pszBurnAttribute)
        {
            iBurnField =
                poLayer->GetLayerDefn()->GetFieldIndex(pszBurnAttribute);
            if (iBurnField == -1)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Failed to find field %s on layer %s, skipping.",
                         pszBurnAttribute, poLayer->GetLayerDefn()->GetName());
                continue;
            }
        }
        else
        {
            padfBurnValues = padfLayerBurnValues + iLayer * nBandCount;
        }

        /* --------------------------------------------------------------------
         */
        /*      If we have no transformer, create the one from input file */
        /*      projection. Note that each layer can be georefernced */
        /*      separately. */
        /* --------------------------------------------------------------------
         */
        bool bNeedToFreeTransformer = false;

        if (pfnTransformer == nullptr)
        {
            char *pszProjection = nullptr;
            bNeedToFreeTransformer = true;

            OGRSpatialReference *poSRS = poLayer->GetSpatialRef();
            if (!poSRS)
            {
                if (poDS->GetSpatialRef() != nullptr ||
                    poDS->GetGCPSpatialRef() != nullptr ||
                    poDS->GetMetadata("RPC") != nullptr)
                {
                    CPLError(
                        CE_Warning, CPLE_AppDefined,
                        "Failed to fetch spatial reference on layer %s "
                        "to build transformer, assuming matching coordinate "
                        "systems.",
                        poLayer->GetLayerDefn()->GetName());
                }
            }
            else
            {
                poSRS->exportToWkt(&pszProjection);
            }

            char **papszTransformerOptions = nullptr;
            if (pszProjection != nullptr)
                papszTransformerOptions = CSLSetNameValue(
                    papszTransformerOptions, "SRC_SRS", pszProjection);
            GDALGeoTransform gt;
            if (poDS->GetGeoTransform(gt) != CE_None &&
                poDS->GetGCPCount() == 0 && poDS->GetMetadata("RPC") == nullptr)
            {
                papszTransformerOptions = CSLSetNameValue(
                    papszTransformerOptions, "DST_METHOD", "NO_GEOTRANSFORM");
            }

            pTransformArg = GDALCreateGenImgProjTransformer2(
                nullptr, hDS, papszTransformerOptions);
            pfnTransformer = GDALGenImgProjTransform;

            CPLFree(pszProjection);
            CSLDestroy(papszTransformerOptions);
            if (pTransformArg == nullptr)
            {
                CPLFree(pabyChunkBuf);
                return CE_Failure;
            }
        }

        poLayer->ResetReading();

        /* --------------------------------------------------------------------
         */
        /*      Loop over image in designated chunks. */
        /* --------------------------------------------------------------------
         */

        double *padfAttrValues = static_cast<double *>(
            VSI_MALLOC_VERBOSE(sizeof(double) * nBandCount));
        if (padfAttrValues == nullptr)
            eErr = CE_Failure;

        for (int iY = 0; iY < poDS->GetRasterYSize() && eErr == CE_None;
             iY += nYChunkSize)
        {
            int nThisYChunkSize = nYChunkSize;
            if (nThisYChunkSize + iY > poDS->GetRasterYSize())
                nThisYChunkSize = poDS->GetRasterYSize() - iY;

            // Only re-read image if not a single chunk is being rendered.
            if (nYChunkSize < poDS->GetRasterYSize())
            {
                eErr = poDS->RasterIO(
                    GF_Read, 0, iY, poDS->GetRasterXSize(), nThisYChunkSize,
                    pabyChunkBuf, poDS->GetRasterXSize(), nThisYChunkSize,
                    eType, nBandCount, panBandList, 0, 0, 0, nullptr);
                if (eErr != CE_None)
                    break;
            }

            for (auto &poFeat : poLayer)
            {
                OGRGeometry *poGeom = poFeat->GetGeometryRef();

                if (pszBurnAttribute)
                {
                    const double dfAttrValue =
                        poFeat->GetFieldAsDouble(iBurnField);
                    for (int iBand = 0; iBand < nBandCount; iBand++)
                        padfAttrValues[iBand] = dfAttrValue;

                    padfBurnValues = padfAttrValues;
                }

                gv_rasterize_one_shape(
                    pabyChunkBuf, 0, iY, poDS->GetRasterXSize(),
                    nThisYChunkSize, nBandCount, eType, 0, 0, 0, bAllTouched,
                    poGeom, GDT_Float64, padfBurnValues, nullptr,
                    eBurnValueSource, eMergeAlg, pfnTransformer, pTransformArg);
            }

            // Only write image if not a single chunk is being rendered.
            if (nYChunkSize < poDS->GetRasterYSize())
            {
                eErr = poDS->RasterIO(
                    GF_Write, 0, iY, poDS->GetRasterXSize(), nThisYChunkSize,
                    pabyChunkBuf, poDS->GetRasterXSize(), nThisYChunkSize,
                    eType, nBandCount, panBandList, 0, 0, 0, nullptr);
            }

            poLayer->ResetReading();

            if (!pfnProgress((iY + nThisYChunkSize) /
                                 static_cast<double>(poDS->GetRasterYSize()),
                             "", pProgressArg))
            {
                CPLError(CE_Failure, CPLE_UserInterrupt, "User terminated");
                eErr = CE_Failure;
            }
        }

        VSIFree(padfAttrValues);

        if (bNeedToFreeTransformer)
        {
            GDALDestroyTransformer(pTransformArg);
            pTransformArg = nullptr;
            pfnTransformer = nullptr;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Write out the image once for all layers if user requested       */
    /*      to render the whole raster in single chunk.                     */
    /* -------------------------------------------------------------------- */
    if (eErr == CE_None && nYChunkSize == poDS->GetRasterYSize())
    {
        eErr =
            poDS->RasterIO(GF_Write, 0, 0, poDS->GetRasterXSize(), nYChunkSize,
                           pabyChunkBuf, poDS->GetRasterXSize(), nYChunkSize,
                           eType, nBandCount, panBandList, 0, 0, 0, nullptr);
    }

    /* -------------------------------------------------------------------- */
    /*      cleanup                                                         */
    /* -------------------------------------------------------------------- */
    VSIFree(pabyChunkBuf);

    return eErr;
}

/************************************************************************/
/*                        GDALRasterizeLayersBuf()                      */
/************************************************************************/

/**
 * Burn geometries from the specified list of layer into raster.
 *
 * Rasterize all the geometric objects from a list of layers into supplied
 * raster buffer.  The layers are passed as an array of OGRLayerH handlers.
 *
 * If the geometries are in the georeferenced coordinates of the raster
 * dataset, then the pfnTransform may be passed in NULL and one will be
 * derived internally from the geotransform of the dataset.  The transform
 * needs to transform the geometry locations into pixel/line coordinates
 * of the target raster.
 *
 * The output raster may be of any GDAL supported datatype(non complex).
 *
 * @param pData pointer to the output data array.
 *
 * @param nBufXSize width of the output data array in pixels.
 *
 * @param nBufYSize height of the output data array in pixels.
 *
 * @param eBufType data type of the output data array.
 *
 * @param nPixelSpace The byte offset from the start of one pixel value in
 * pData to the start of the next pixel value within a scanline.  If defaulted
 * (0) the size of the datatype eBufType is used.
 *
 * @param nLineSpace The byte offset from the start of one scanline in
 * pData to the start of the next.  If defaulted the size of the datatype
 * eBufType * nBufXSize is used.
 *
 * @param nLayerCount the number of layers being passed in pahLayers array.
 *
 * @param pahLayers the array of layers to burn in.
 *
 * @param pszDstProjection WKT defining the coordinate system of the target
 * raster.
 *
 * @param padfDstGeoTransform geotransformation matrix of the target raster.
 *
 * @param pfnTransformer transformation to apply to geometries to put into
 * pixel/line coordinates on raster.  If NULL a geotransform based one will
 * be created internally.
 *
 * @param pTransformArg callback data for transformer.
 *
 * @param dfBurnValue the value to burn into the raster.
 *
 * @param papszOptions special options controlling rasterization:
 * <ul>
 * <li>"ATTRIBUTE": Identifies an attribute field on the features to be
 * used for a burn in value. The value will be burned into all output
 * bands. If specified, padfLayerBurnValues will not be used and can be a NULL
 * pointer.</li>
 * <li>"ALL_TOUCHED": May be set to TRUE to set all pixels touched
 * by the line or polygons, not just those whose center is within the polygon
 * (behavior is unspecified when the polygon is just touching the pixel center)
 * or that are selected by Brezenham's line algorithm.  Defaults to FALSE.</li>
 * <li>"BURN_VALUE_FROM": May be set to "Z" to use
 * the Z values of the geometries. dfBurnValue or the attribute field value is
 * added to this before burning. In default case dfBurnValue is burned as it
 * is. This is implemented properly only for points and lines for now. Polygons
 * will be burned using the Z value from the first point. The M value may
 * be supported in the future.</li>
 * <li>"MERGE_ALG": May be REPLACE (the default) or ADD.  REPLACE
 * results in overwriting of value, while ADD adds the new value to the
 * existing raster, suitable for heatmaps for instance.</li>
 * </ul>
 *
 * @param pfnProgress the progress function to report completion.
 *
 * @param pProgressArg callback data for progress function.
 *
 *
 * @return CE_None on success or CE_Failure on error.
 */

CPLErr GDALRasterizeLayersBuf(
    void *pData, int nBufXSize, int nBufYSize, GDALDataType eBufType,
    int nPixelSpace, int nLineSpace, int nLayerCount, OGRLayerH *pahLayers,
    const char *pszDstProjection, double *padfDstGeoTransform,
    GDALTransformerFunc pfnTransformer, void *pTransformArg, double dfBurnValue,
    char **papszOptions, GDALProgressFunc pfnProgress, void *pProgressArg)

{
    /* -------------------------------------------------------------------- */
    /*           check eType, Avoid not supporting data types               */
    /* -------------------------------------------------------------------- */
    if (GDALDataTypeIsComplex(eBufType) || eBufType <= GDT_Unknown ||
        eBufType >= GDT_TypeCount)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "GDALRasterizeLayersBuf(): unsupported data type of eBufType");
        return CE_Failure;
    }

    /* -------------------------------------------------------------------- */
    /*      If pixel and line spaceing are defaulted assign reasonable      */
    /*      value assuming a packed buffer.                                 */
    /* -------------------------------------------------------------------- */
    int nTypeSizeBytes = GDALGetDataTypeSizeBytes(eBufType);
    if (nPixelSpace == 0)
    {
        nPixelSpace = nTypeSizeBytes;
    }
    if (nPixelSpace < nTypeSizeBytes)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "GDALRasterizeLayersBuf(): unsupported value of nPixelSpace");
        return CE_Failure;
    }

    if (nLineSpace == 0)
    {
        nLineSpace = nPixelSpace * nBufXSize;
    }
    if (nLineSpace < nPixelSpace * nBufXSize)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "GDALRasterizeLayersBuf(): unsupported value of nLineSpace");
        return CE_Failure;
    }

    if (pfnProgress == nullptr)
        pfnProgress = GDALDummyProgress;

    /* -------------------------------------------------------------------- */
    /*      Do some rudimentary arg checking.                               */
    /* -------------------------------------------------------------------- */
    if (nLayerCount == 0)
        return CE_None;

    /* -------------------------------------------------------------------- */
    /*      Options                                                         */
    /* -------------------------------------------------------------------- */
    int bAllTouched = FALSE;
    GDALBurnValueSrc eBurnValueSource = GBV_UserBurnValue;
    GDALRasterMergeAlg eMergeAlg = GRMA_Replace;
    if (GDALRasterizeOptions(papszOptions, &bAllTouched, &eBurnValueSource,
                             &eMergeAlg, nullptr) == CE_Failure)
    {
        return CE_Failure;
    }

    /* ==================================================================== */
    /*      Read the specified layers transforming and rasterizing          */
    /*      geometries.                                                     */
    /* ==================================================================== */
    CPLErr eErr = CE_None;
    const char *pszBurnAttribute = CSLFetchNameValue(papszOptions, "ATTRIBUTE");

    pfnProgress(0.0, nullptr, pProgressArg);

    for (int iLayer = 0; iLayer < nLayerCount; iLayer++)
    {
        OGRLayer *poLayer = reinterpret_cast<OGRLayer *>(pahLayers[iLayer]);

        if (!poLayer)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Layer element number %d is NULL, skipping.", iLayer);
            continue;
        }

        /* --------------------------------------------------------------------
         */
        /*      If the layer does not contain any features just skip it. */
        /*      Do not force the feature count, so if driver doesn't know */
        /*      exact number of features, go down the normal way. */
        /* --------------------------------------------------------------------
         */
        if (poLayer->GetFeatureCount(FALSE) == 0)
            continue;

        int iBurnField = -1;
        if (pszBurnAttribute)
        {
            iBurnField =
                poLayer->GetLayerDefn()->GetFieldIndex(pszBurnAttribute);
            if (iBurnField == -1)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Failed to find field %s on layer %s, skipping.",
                         pszBurnAttribute, poLayer->GetLayerDefn()->GetName());
                continue;
            }
        }

        /* --------------------------------------------------------------------
         */
        /*      If we have no transformer, create the one from input file */
        /*      projection. Note that each layer can be georefernced */
        /*      separately. */
        /* --------------------------------------------------------------------
         */
        bool bNeedToFreeTransformer = false;

        if (pfnTransformer == nullptr)
        {
            char *pszProjection = nullptr;
            bNeedToFreeTransformer = true;

            OGRSpatialReference *poSRS = poLayer->GetSpatialRef();
            if (!poSRS)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Failed to fetch spatial reference on layer %s "
                         "to build transformer, assuming matching coordinate "
                         "systems.",
                         poLayer->GetLayerDefn()->GetName());
            }
            else
            {
                poSRS->exportToWkt(&pszProjection);
            }

            pTransformArg = GDALCreateGenImgProjTransformer3(
                pszProjection, nullptr, pszDstProjection, padfDstGeoTransform);
            pfnTransformer = GDALGenImgProjTransform;

            CPLFree(pszProjection);
        }

        for (auto &poFeat : poLayer)
        {
            OGRGeometry *poGeom = poFeat->GetGeometryRef();

            if (pszBurnAttribute)
                dfBurnValue = poFeat->GetFieldAsDouble(iBurnField);

            gv_rasterize_one_shape(
                static_cast<unsigned char *>(pData), 0, 0, nBufXSize, nBufYSize,
                1, eBufType, nPixelSpace, nLineSpace, 0, bAllTouched, poGeom,
                GDT_Float64, &dfBurnValue, nullptr, eBurnValueSource, eMergeAlg,
                pfnTransformer, pTransformArg);
        }

        poLayer->ResetReading();

        if (!pfnProgress(1, "", pProgressArg))
        {
            CPLError(CE_Failure, CPLE_UserInterrupt, "User terminated");
            eErr = CE_Failure;
        }

        if (bNeedToFreeTransformer)
        {
            GDALDestroyTransformer(pTransformArg);
            pTransformArg = nullptr;
            pfnTransformer = nullptr;
        }
    }

    return eErr;
}
