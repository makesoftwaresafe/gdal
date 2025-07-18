/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements OGRVRTLayer class.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2009-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "ogr_vrt.h"

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_minixml.h"
#include "cpl_string.h"
#include "gdal.h"
#include "gdal_priv.h"
#include "ogr_api.h"
#include "ogr_core.h"
#include "ogr_feature.h"
#include "ogr_geometry.h"
#include "ogr_spatialref.h"
#include "ogrpgeogeometry.h"
#include "ogrsf_frmts.h"
#include "ogrvrtgeometrytypes.h"
#include "memdataset.h"

#define UNSUPPORTED_OP_READ_ONLY                                               \
    "%s : unsupported operation on a read-only datasource."

/************************************************************************/
/*                   GetFieldIndexCaseSensitiveFirst()                  */
/************************************************************************/

static int GetFieldIndexCaseSensitiveFirst(OGRFeatureDefn *poFDefn,
                                           const char *pszFieldName)
{
    int idx = poFDefn->GetFieldIndexCaseSensitive(pszFieldName);
    if (idx < 0)
        idx = poFDefn->GetFieldIndex(pszFieldName);
    return idx;
}

/************************************************************************/
/*                       OGRVRTGeomFieldProps()                         */
/************************************************************************/

OGRVRTGeomFieldProps::OGRVRTGeomFieldProps() = default;

/************************************************************************/
/*                      ~OGRVRTGeomFieldProps()                         */
/************************************************************************/

OGRVRTGeomFieldProps::~OGRVRTGeomFieldProps()
{
    if (poSRS != nullptr)
        const_cast<OGRSpatialReference *>(poSRS)->Release();
}

/************************************************************************/
/*                            OGRVRTLayer()                             */
/************************************************************************/

OGRVRTLayer::OGRVRTLayer(OGRVRTDataSource *poDSIn) : poDS(poDSIn)
{
}

/************************************************************************/
/*                            ~OGRVRTLayer()                            */
/************************************************************************/

OGRVRTLayer::~OGRVRTLayer()

{
    if (m_nFeaturesRead > 0 && poFeatureDefn != nullptr)
    {
        CPLDebug("VRT", "%d features read on layer '%s'.",
                 static_cast<int>(m_nFeaturesRead), poFeatureDefn->GetName());
    }

    if (poSrcDS != nullptr)
    {
        if (poSrcLayer)
        {
            poSrcLayer->SetIgnoredFields(nullptr);
            poSrcLayer->SetAttributeFilter(nullptr);
            poSrcLayer->SetSpatialFilter(nullptr);
        }

        if (bSrcLayerFromSQL && poSrcLayer)
            poSrcDS->ReleaseResultSet(poSrcLayer);
    }

    if (poFeatureDefn)
        poFeatureDefn->Release();

    CPLFree(pszAttrFilter);
}

/************************************************************************/
/*                         GetSrcLayerDefn()                            */
/************************************************************************/

OGRFeatureDefn *OGRVRTLayer::GetSrcLayerDefn()
{
    if (poSrcFeatureDefn)
        return poSrcFeatureDefn;

    if (poSrcLayer)
        poSrcFeatureDefn = poSrcLayer->GetLayerDefn();

    return poSrcFeatureDefn;
}

/************************************************************************/
/*                         FastInitialize()                             */
/************************************************************************/

bool OGRVRTLayer::FastInitialize(CPLXMLNode *psLTreeIn,
                                 const char *pszVRTDirectory, int bUpdateIn)

{
    psLTree = psLTreeIn;
    bUpdate = CPL_TO_BOOL(bUpdateIn);
    osVRTDirectory = pszVRTDirectory;

    if (!EQUAL(psLTree->pszValue, "OGRVRTLayer"))
        return FALSE;

    // Get layer name.
    const char *pszLayerName = CPLGetXMLValue(psLTree, "name", nullptr);

    if (pszLayerName == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Missing name attribute on OGRVRTLayer");
        return FALSE;
    }

    osName = pszLayerName;
    SetDescription(pszLayerName);

    // Do we have a fixed geometry type?  If so, use it.
    CPLXMLNode *psGeometryFieldNode = CPLGetXMLNode(psLTree, "GeometryField");
    const char *pszGType = CPLGetXMLValue(psLTree, "GeometryType", nullptr);
    if (pszGType == nullptr && psGeometryFieldNode != nullptr)
        pszGType = CPLGetXMLValue(psGeometryFieldNode, "GeometryType", nullptr);
    if (pszGType != nullptr)
    {
        int l_bError = FALSE;
        const OGRwkbGeometryType eGeomType =
            OGRVRTGetGeometryType(pszGType, &l_bError);
        if (l_bError)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "GeometryType %s not recognised.", pszGType);
            return FALSE;
        }
        if (eGeomType != wkbNone)
        {
            apoGeomFieldProps.push_back(
                std::make_unique<OGRVRTGeomFieldProps>());
            apoGeomFieldProps[0]->eGeomType = eGeomType;
        }
    }

    // Apply a spatial reference system if provided.
    const char *pszLayerSRS = CPLGetXMLValue(psLTree, "LayerSRS", nullptr);
    if (pszLayerSRS == nullptr && psGeometryFieldNode != nullptr)
        pszLayerSRS = CPLGetXMLValue(psGeometryFieldNode, "SRS", nullptr);
    if (pszLayerSRS != nullptr)
    {
        if (apoGeomFieldProps.empty())
        {
            apoGeomFieldProps.push_back(
                std::make_unique<OGRVRTGeomFieldProps>());
        }
        if (!(EQUAL(pszLayerSRS, "NULL")))
        {
            OGRSpatialReference oSRS;
            oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

            if (oSRS.SetFromUserInput(
                    pszLayerSRS,
                    OGRSpatialReference::
                        SET_FROM_USER_INPUT_LIMITATIONS_get()) != OGRERR_NONE)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Failed to import LayerSRS `%s'.", pszLayerSRS);
                return FALSE;
            }
            apoGeomFieldProps[0]->poSRS = oSRS.Clone();
        }
    }

    // Set FeatureCount if provided.
    const char *pszFeatureCount =
        CPLGetXMLValue(psLTree, "FeatureCount", nullptr);
    if (pszFeatureCount != nullptr)
    {
        nFeatureCount = CPLAtoGIntBig(pszFeatureCount);
    }

    // Set Extent if provided.
    const char *pszExtentXMin = CPLGetXMLValue(psLTree, "ExtentXMin", nullptr);
    const char *pszExtentYMin = CPLGetXMLValue(psLTree, "ExtentYMin", nullptr);
    const char *pszExtentXMax = CPLGetXMLValue(psLTree, "ExtentXMax", nullptr);
    const char *pszExtentYMax = CPLGetXMLValue(psLTree, "ExtentYMax", nullptr);
    if (pszExtentXMin == nullptr && psGeometryFieldNode != nullptr)
    {
        pszExtentXMin =
            CPLGetXMLValue(psGeometryFieldNode, "ExtentXMin", nullptr);
        pszExtentYMin =
            CPLGetXMLValue(psGeometryFieldNode, "ExtentYMin", nullptr);
        pszExtentXMax =
            CPLGetXMLValue(psGeometryFieldNode, "ExtentXMax", nullptr);
        pszExtentYMax =
            CPLGetXMLValue(psGeometryFieldNode, "ExtentYMax", nullptr);
    }
    if (pszExtentXMin != nullptr && pszExtentYMin != nullptr &&
        pszExtentXMax != nullptr && pszExtentYMax != nullptr)
    {
        if (apoGeomFieldProps.empty())
        {
            apoGeomFieldProps.push_back(
                std::make_unique<OGRVRTGeomFieldProps>());
            assert(!apoGeomFieldProps.empty());
        }
        apoGeomFieldProps[0]->sStaticEnvelope.MinX = CPLAtof(pszExtentXMin);
        apoGeomFieldProps[0]->sStaticEnvelope.MinY = CPLAtof(pszExtentYMin);
        apoGeomFieldProps[0]->sStaticEnvelope.MaxX = CPLAtof(pszExtentXMax);
        apoGeomFieldProps[0]->sStaticEnvelope.MaxY = CPLAtof(pszExtentYMax);
    }

    return TRUE;
}

/************************************************************************/
/*                       ParseGeometryField()                           */
/************************************************************************/

bool OGRVRTLayer::ParseGeometryField(CPLXMLNode *psNode,
                                     CPLXMLNode *psNodeParent,
                                     OGRVRTGeomFieldProps *poProps)
{
    const char *pszName = CPLGetXMLValue(psNode, "name", nullptr);
    poProps->osName = pszName ? pszName : "";
    if (pszName == nullptr && apoGeomFieldProps.size() > 1 &&
        poProps != apoGeomFieldProps[0].get())
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "A 'name' attribute should be defined when there are "
                 "several geometry fields");
    }

    // Do we have a fixed geometry type?
    const char *pszGType = CPLGetXMLValue(psNode, "GeometryType", nullptr);
    if (pszGType == nullptr && poProps == apoGeomFieldProps[0].get())
        pszGType = CPLGetXMLValue(psNodeParent, "GeometryType", nullptr);
    if (pszGType != nullptr)
    {
        int l_bError = FALSE;
        poProps->eGeomType = OGRVRTGetGeometryType(pszGType, &l_bError);
        if (l_bError)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "GeometryType %s not recognised.", pszGType);
            return false;
        }
    }

    // Determine which field(s) to get the geometry from.
    const char *pszEncoding = CPLGetXMLValue(psNode, "encoding", "direct");

    if (EQUAL(pszEncoding, "Direct"))
        poProps->eGeometryStyle = VGS_Direct;
    else if (EQUAL(pszEncoding, "None"))
        poProps->eGeometryStyle = VGS_None;
    else if (EQUAL(pszEncoding, "WKT"))
        poProps->eGeometryStyle = VGS_WKT;
    else if (EQUAL(pszEncoding, "WKB"))
        poProps->eGeometryStyle = VGS_WKB;
    else if (EQUAL(pszEncoding, "Shape"))
        poProps->eGeometryStyle = VGS_Shape;
    else if (EQUAL(pszEncoding, "PointFromColumns"))
    {
        poProps->eGeometryStyle = VGS_PointFromColumns;
        poProps->bUseSpatialSubquery = CPLTestBool(
            CPLGetXMLValue(psNode, "GeometryField.useSpatialSubquery", "TRUE"));

        poProps->iGeomXField = GetFieldIndexCaseSensitiveFirst(
            GetSrcLayerDefn(), CPLGetXMLValue(psNode, "x", "missing"));
        poProps->iGeomYField = GetFieldIndexCaseSensitiveFirst(
            GetSrcLayerDefn(), CPLGetXMLValue(psNode, "y", "missing"));
        poProps->iGeomZField = GetFieldIndexCaseSensitiveFirst(
            GetSrcLayerDefn(), CPLGetXMLValue(psNode, "z", "missing"));
        poProps->iGeomMField = GetFieldIndexCaseSensitiveFirst(
            GetSrcLayerDefn(), CPLGetXMLValue(psNode, "m", "missing"));

        if (poProps->iGeomXField == -1 || poProps->iGeomYField == -1)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to identify source X or Y field for "
                     "PointFromColumns encoding.");
            return false;
        }

        if (pszGType == nullptr)
        {
            poProps->eGeomType = wkbPoint;
            if (poProps->iGeomZField != -1)
                poProps->eGeomType = OGR_GT_SetZ(poProps->eGeomType);
            if (poProps->iGeomMField != -1)
                poProps->eGeomType = OGR_GT_SetM(poProps->eGeomType);
        }
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined, "encoding=\"%s\" not recognised.",
                 pszEncoding);
        return false;
    }

    if (poProps->eGeometryStyle == VGS_WKT ||
        poProps->eGeometryStyle == VGS_WKB ||
        poProps->eGeometryStyle == VGS_Shape)
    {
        const char *pszFieldName = CPLGetXMLValue(psNode, "field", "missing");

        poProps->iGeomField =
            GetFieldIndexCaseSensitiveFirst(GetSrcLayerDefn(), pszFieldName);

        if (poProps->iGeomField == -1)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to identify source field '%s' for geometry.",
                     pszFieldName);
            return false;
        }
    }
    else if (poProps->eGeometryStyle == VGS_Direct)
    {
        const char *pszFieldName = CPLGetXMLValue(psNode, "field", nullptr);

        if (pszFieldName != nullptr ||
            GetSrcLayerDefn()->GetGeomFieldCount() > 1)
        {
            if (pszFieldName == nullptr)
                pszFieldName = poProps->osName;
            poProps->iGeomField =
                GetSrcLayerDefn()->GetGeomFieldIndex(pszFieldName);

            if (poProps->iGeomField == -1)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Unable to identify source geometry field '%s' "
                         "for geometry.",
                         pszFieldName);
                return false;
            }
        }
        else if (GetSrcLayerDefn()->GetGeomFieldCount() == 1)
        {
            poProps->iGeomField = 0;
        }
        else if (psNode != nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to identify source geometry field.");
            return false;
        }
    }

    poProps->bReportSrcColumn =
        CPLTestBool(CPLGetXMLValue(psNode, "reportSrcColumn", "YES"));

    // Guess geometry type if not explicitly provided (or computed).
    if (pszGType == nullptr && poProps->eGeomType == wkbUnknown)
    {
        if (GetSrcLayerDefn()->GetGeomFieldCount() == 1)
            poProps->eGeomType = poSrcLayer->GetGeomType();
        else if (poProps->eGeometryStyle == VGS_Direct &&
                 poProps->iGeomField >= 0)
        {
            poProps->eGeomType = GetSrcLayerDefn()
                                     ->GetGeomFieldDefn(poProps->iGeomField)
                                     ->GetType();
        }
    }

    // Copy spatial reference system from source if not provided.
    const char *pszSRS = CPLGetXMLValue(psNode, "SRS", nullptr);
    if (pszSRS == nullptr && poProps == apoGeomFieldProps[0].get())
        pszSRS = CPLGetXMLValue(psNodeParent, "LayerSRS", nullptr);
    if (pszSRS == nullptr)
    {
        const OGRSpatialReference *poSRS = nullptr;
        if (GetSrcLayerDefn()->GetGeomFieldCount() == 1)
        {
            poSRS = poSrcLayer->GetSpatialRef();
        }
        else if (poProps->eGeometryStyle == VGS_Direct &&
                 poProps->iGeomField >= 0)
        {
            poSRS = GetSrcLayerDefn()
                        ->GetGeomFieldDefn(poProps->iGeomField)
                        ->GetSpatialRef();
        }
        if (poSRS != nullptr)
            poProps->poSRS = poSRS->Clone();
    }
    else if (poProps->poSRS == nullptr)
    {
        if (!(EQUAL(pszSRS, "NULL")))
        {
            OGRSpatialReference oSRS;
            oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

            if (oSRS.SetFromUserInput(
                    pszSRS,
                    OGRSpatialReference::
                        SET_FROM_USER_INPUT_LIMITATIONS_get()) != OGRERR_NONE)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Failed to import SRS `%s'.", pszSRS);
                return false;
            }
            poProps->poSRS = oSRS.Clone();
        }
    }

    // Do we have a SrcRegion?
    const CPLXMLNode *psSrcRegionNode = CPLGetXMLNode(psNode, "SrcRegion");
    if (psSrcRegionNode == nullptr && poProps == apoGeomFieldProps[0].get())
        psSrcRegionNode = CPLGetXMLNode(psNodeParent, "SrcRegion");
    const char *pszSrcRegion = CPLGetXMLValue(psSrcRegionNode, "", nullptr);
    if (pszSrcRegion != nullptr)
    {
        poProps->poSrcRegion =
            OGRGeometryFactory::createFromWkt(pszSrcRegion).first;

        if (poProps->poSrcRegion == nullptr)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Ignoring SrcRegion. It must be a valid WKT geometry");
        }

        poProps->bSrcClip =
            CPLTestBool(CPLGetXMLValue(psSrcRegionNode, "clip", "FALSE"));
    }

    // Set Extent if provided.
    const char *pszExtentXMin = CPLGetXMLValue(psNode, "ExtentXMin", nullptr);
    const char *pszExtentYMin = CPLGetXMLValue(psNode, "ExtentYMin", nullptr);
    const char *pszExtentXMax = CPLGetXMLValue(psNode, "ExtentXMax", nullptr);
    const char *pszExtentYMax = CPLGetXMLValue(psNode, "ExtentYMax", nullptr);
    if (pszExtentXMin != nullptr && pszExtentYMin != nullptr &&
        pszExtentXMax != nullptr && pszExtentYMax != nullptr)
    {
        poProps->sStaticEnvelope.MinX = CPLAtof(pszExtentXMin);
        poProps->sStaticEnvelope.MinY = CPLAtof(pszExtentYMin);
        poProps->sStaticEnvelope.MaxX = CPLAtof(pszExtentXMax);
        poProps->sStaticEnvelope.MaxY = CPLAtof(pszExtentYMax);
    }

    poProps->bNullable =
        CPLTestBool(CPLGetXMLValue(psNode, "nullable", "TRUE"));

    if (GetSrcLayerDefn()->GetGeomFieldCount() == 1)
    {
        poProps->sCoordinatePrecision =
            GetSrcLayerDefn()->GetGeomFieldDefn(0)->GetCoordinatePrecision();
    }
    else if (poProps->eGeometryStyle == VGS_Direct && poProps->iGeomField >= 0)
    {
        poProps->sCoordinatePrecision =
            GetSrcLayerDefn()
                ->GetGeomFieldDefn(poProps->iGeomField)
                ->GetCoordinatePrecision();
    }
    if (const char *pszXYResolution =
            CPLGetXMLValue(psNode, "XYResolution", nullptr))
    {
        poProps->sCoordinatePrecision.dfXYResolution = CPLAtof(pszXYResolution);
    }
    if (const char *pszZResolution =
            CPLGetXMLValue(psNode, "ZResolution", nullptr))
    {
        poProps->sCoordinatePrecision.dfZResolution = CPLAtof(pszZResolution);
    }
    if (const char *pszMResolution =
            CPLGetXMLValue(psNode, "MResolution", nullptr))
    {
        poProps->sCoordinatePrecision.dfMResolution = CPLAtof(pszMResolution);
    }

    return true;
}

/************************************************************************/
/*                         FullInitialize()                             */
/************************************************************************/

// TODO(schwehr): Remove gotos.
bool OGRVRTLayer::FullInitialize()
{
    if (bHasFullInitialized)
        return true;

    const char *pszSharedSetting = nullptr;
    const char *pszSQL = nullptr;
    const char *pszStyleFieldName = nullptr;
    CPLXMLNode *psChild = nullptr;
    bool bFoundGeometryField = false;

    bHasFullInitialized = true;

    poFeatureDefn = new OGRFeatureDefn(osName);
    poFeatureDefn->Reference();

    if (poDS->GetRecursionDetected())
        return false;

    // Figure out the data source name.  It may be treated relative
    // to vrt filename, but normally it is used directly.
    std::string osSrcDSName = CPLGetXMLValue(psLTree, "SrcDataSource", "");

    if (osSrcDSName.empty())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Missing SrcDataSource for layer %s.", osName.c_str());
        goto error;
    }

    if (CPLTestBool(
            CPLGetXMLValue(psLTree, "SrcDataSource.relativetoVRT", "0")))
    {
        static const char *const apszPrefixes[] = {"CSV:", "GPSBABEL:"};
        bool bDone = false;
        for (size_t i = 0; i < sizeof(apszPrefixes) / sizeof(apszPrefixes[0]);
             i++)
        {
            const char *pszPrefix = apszPrefixes[i];
            if (EQUALN(osSrcDSName.c_str(), pszPrefix, strlen(pszPrefix)))
            {
                auto nLastPart = osSrcDSName.find(':') + 1;
                // CSV:z:/foo.xyz
                if ((osSrcDSName[nLastPart] == '/' ||
                     osSrcDSName[nLastPart] == '\\') &&
                    nLastPart >= 3 && osSrcDSName[nLastPart - 3] == ':')
                    nLastPart -= 2;
                CPLString osPrefix(osSrcDSName);
                osPrefix.resize(nLastPart);
                osSrcDSName = osPrefix + CPLProjectRelativeFilenameSafe(
                                             osVRTDirectory,
                                             osSrcDSName.c_str() + nLastPart);
                bDone = true;
                break;
            }
        }
        if (!bDone)
        {
            osSrcDSName = CPLProjectRelativeFilenameSafe(osVRTDirectory,
                                                         osSrcDSName.c_str());
        }
    }

    // Are we accessing this datasource in shared mode?  We default
    // to shared for SrcSQL requests, but we also allow the XML to
    // control our shared setting with an attribute on the
    // datasource element.
    pszSharedSetting = CPLGetXMLValue(psLTree, "SrcDataSource.shared", nullptr);
    if (pszSharedSetting == nullptr)
    {
        if (CPLGetXMLValue(psLTree, "SrcSQL", nullptr) == nullptr)
            pszSharedSetting = "OFF";
        else
            pszSharedSetting = "ON";
    }

    bSrcDSShared = CPLTestBool(pszSharedSetting);

    // Update mode doesn't make sense if we have a SrcSQL element.
    if (CPLGetXMLValue(psLTree, "SrcSQL", nullptr) != nullptr)
        bUpdate = false;

    // Try to access the datasource.
try_again:
    CPLErrorReset();
    if (EQUAL(osSrcDSName.c_str(), "@dummy@"))
    {
        poSrcDS.reset(
            MEMDataset::Create("@dummy@", 0, 0, 0, GDT_Unknown, nullptr));
        poSrcDS->CreateLayer("@dummy@");
    }
    else if (bSrcDSShared)
    {
        if (poDS->IsInForbiddenNames(osSrcDSName.c_str()))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cyclic VRT opening detected!");
            poDS->SetRecursionDetected();
        }
        else
        {
            CPLStringList aosOpenOptions(
                GDALDeserializeOpenOptionsFromXML(psLTree));
            int l_nFlags = GDAL_OF_VECTOR | GDAL_OF_SHARED;
            if (bUpdate)
                l_nFlags |= GDAL_OF_UPDATE;
            poSrcDS.reset(GDALDataset::Open(osSrcDSName.c_str(), l_nFlags,
                                            nullptr, aosOpenOptions.List(),
                                            nullptr));
            // Is it a VRT datasource?
            if (poSrcDS != nullptr && poSrcDS->GetDriver() == poDS->GetDriver())
            {
                OGRVRTDataSource *poVRTSrcDS =
                    cpl::down_cast<OGRVRTDataSource *>(poSrcDS.get());
                poVRTSrcDS->AddForbiddenNames(poDS->GetDescription());
            }
        }
    }
    else
    {
        if (poDS->GetCallLevel() < 32)
        {
            CPLStringList aosOpenOptions(
                GDALDeserializeOpenOptionsFromXML(psLTree));
            int l_nFlags = GDAL_OF_VECTOR;
            if (bUpdate)
                l_nFlags |= GDAL_OF_UPDATE;
            poSrcDS.reset(GDALDataset::Open(osSrcDSName.c_str(), l_nFlags,
                                            nullptr, aosOpenOptions.List(),
                                            nullptr));
            // Is it a VRT datasource?
            if (poSrcDS != nullptr && poSrcDS->GetDriver() == poDS->GetDriver())
            {
                OGRVRTDataSource *poVRTSrcDS =
                    dynamic_cast<OGRVRTDataSource *>(poSrcDS.get());
                if (poVRTSrcDS)
                {
                    poVRTSrcDS->SetCallLevel(poDS->GetCallLevel() + 1);
                    poVRTSrcDS->SetParentDS(poDS);
                }
            }
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Trying to open a VRT from a VRT from a VRT from ... "
                     "[32 times] a VRT!");

            poDS->SetRecursionDetected();

            OGRVRTDataSource *poParent = poDS->GetParentDS();
            while (poParent != nullptr)
            {
                poParent->SetRecursionDetected();
                poParent = poParent->GetParentDS();
            }
        }
    }

    if (poSrcDS == nullptr)
    {
        if (bUpdate)
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Cannot open datasource `%s' in update mode. "
                     "Trying again in read-only mode",
                     osSrcDSName.c_str());
            bUpdate = false;
            goto try_again;
        }
        if (strlen(CPLGetLastErrorMsg()) == 0)
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Failed to open datasource `%s'.", osSrcDSName.c_str());
        goto error;
    }

    // Apply any metadata.
    oMDMD.XMLInit(psLTree, TRUE);

    // Is this layer derived from an SQL query result?
    pszSQL = CPLGetXMLValue(psLTree, "SrcSQL", nullptr);

    if (pszSQL != nullptr)
    {
        const char *pszDialect =
            CPLGetXMLValue(psLTree, "SrcSQL.dialect", nullptr);
        if (pszDialect != nullptr && pszDialect[0] == '\0')
            pszDialect = nullptr;
        poSrcLayer = poSrcDS->ExecuteSQL(pszSQL, nullptr, pszDialect);
        if (poSrcLayer == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "SQL statement failed, or returned no layer result:\n%s",
                     pszSQL);
            goto error;
        }
        bSrcLayerFromSQL = true;
    }

    // Fetch the layer if it is a regular layer.
    if (poSrcLayer == nullptr)
    {
        const char *pszSrcLayerName =
            CPLGetXMLValue(psLTree, "SrcLayer", osName);

        poSrcLayer = poSrcDS->GetLayerByName(pszSrcLayerName);
        if (poSrcLayer == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Failed to find layer '%s' on datasource '%s'.",
                     pszSrcLayerName, osSrcDSName.c_str());
            goto error;
        }
    }

    // Search for GeometryField definitions.

    // Create as many OGRVRTGeomFieldProps as there are
    // GeometryField elements.
    for (psChild = psLTree->psChild; psChild != nullptr;
         psChild = psChild->psNext)
    {
        if (psChild->eType == CXT_Element &&
            EQUAL(psChild->pszValue, "GeometryField"))
        {
            if (!bFoundGeometryField)
            {
                bFoundGeometryField = true;

                // Recreate the first one if already taken into account in
                // FastInitialize().
                if (apoGeomFieldProps.size() == 1)
                {
                    apoGeomFieldProps.clear();
                }
            }

            apoGeomFieldProps.push_back(
                std::make_unique<OGRVRTGeomFieldProps>());
            if (!ParseGeometryField(psChild, psLTree,
                                    apoGeomFieldProps.back().get()))
            {
                goto error;
            }
        }
    }

    if (!bFoundGeometryField &&
        CPLGetXMLValue(psLTree, "SrcRegion", nullptr) != nullptr)
    {
        apoGeomFieldProps.push_back(std::make_unique<OGRVRTGeomFieldProps>());
    }

    if (!bFoundGeometryField && apoGeomFieldProps.size() == 1)
    {
        // Otherwise use the top-level elements such as SrcRegion.
        if (!ParseGeometryField(nullptr, psLTree, apoGeomFieldProps[0].get()))
            goto error;
    }

    if (apoGeomFieldProps.empty() &&
        CPLGetXMLValue(psLTree, "GeometryType", nullptr) == nullptr)
    {
        // If no GeometryField is found but source geometry fields
        // exist, use them.
        for (int iGeomField = 0;
             iGeomField < GetSrcLayerDefn()->GetGeomFieldCount(); iGeomField++)
        {
            apoGeomFieldProps.push_back(
                std::make_unique<OGRVRTGeomFieldProps>());
            OGRVRTGeomFieldProps *poProps = apoGeomFieldProps.back().get();
            OGRGeomFieldDefn *poFDefn =
                GetSrcLayerDefn()->GetGeomFieldDefn(iGeomField);
            poProps->osName = poFDefn->GetNameRef();
            poProps->eGeomType = poFDefn->GetType();
            if (poFDefn->GetSpatialRef() != nullptr)
                poProps->poSRS = poFDefn->GetSpatialRef()->Clone();
            poProps->iGeomField = iGeomField;
            poProps->bNullable = CPL_TO_BOOL(poFDefn->IsNullable());
        }
    }

    // Instantiate real geometry fields from VRT properties.
    poFeatureDefn->SetGeomType(wkbNone);
    for (const auto &poProp : apoGeomFieldProps)
    {
        OGRGeomFieldDefn oFieldDefn(poProp->osName, poProp->eGeomType);
        oFieldDefn.SetSpatialRef(poProp->poSRS);
        oFieldDefn.SetNullable(poProp->bNullable);
        oFieldDefn.SetCoordinatePrecision(poProp->sCoordinatePrecision);
        poFeatureDefn->AddGeomFieldDefn(&oFieldDefn);
    }

    bAttrFilterPassThrough = true;

    // Figure out what should be used as an FID.
    {
        CPLXMLNode *psFIDNode = CPLGetXMLNode(psLTree, "FID");
        if (psFIDNode != nullptr)
        {
            const char *pszSrcFIDFieldName =
                CPLGetXMLValue(psFIDNode, nullptr, "");
            if (!EQUAL(pszSrcFIDFieldName, ""))
            {
                iFIDField = GetFieldIndexCaseSensitiveFirst(GetSrcLayerDefn(),
                                                            pszSrcFIDFieldName);
                if (iFIDField == -1)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Unable to identify FID field '%s'.",
                             pszSrcFIDFieldName);
                    goto error;
                }
            }

            // User facing FID column name.
            osFIDFieldName =
                CPLGetXMLValue(psFIDNode, "name", pszSrcFIDFieldName);
            if (!EQUAL(osFIDFieldName, poSrcLayer->GetFIDColumn()))
            {
                bAttrFilterPassThrough = false;
            }
        }
        else
        {
            osFIDFieldName = poSrcLayer->GetFIDColumn();
        }
    }

    // Figure out what should be used as a Style.
    pszStyleFieldName = CPLGetXMLValue(psLTree, "Style", nullptr);

    if (pszStyleFieldName != nullptr)
    {
        iStyleField = GetFieldIndexCaseSensitiveFirst(GetSrcLayerDefn(),
                                                      pszStyleFieldName);
        if (iStyleField == -1)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to identify Style field '%s'.", pszStyleFieldName);
            goto error;
        }

        if (!EQUAL(pszStyleFieldName, "OGR_STYLE"))
        {
            bAttrFilterPassThrough = false;
        }
    }

    // Search for schema definitions in the VRT.
    for (psChild = psLTree->psChild; psChild != nullptr;
         psChild = psChild->psNext)
    {
        if (psChild->eType == CXT_Element && EQUAL(psChild->pszValue, "Field"))
        {
            // Field name.
            const char *pszName = CPLGetXMLValue(psChild, "name", nullptr);
            if (pszName == nullptr)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Unable to identify Field name.");
                goto error;
            }

            OGRFieldDefn oFieldDefn(pszName, OFTString);

            // Type.
            const char *pszArg = CPLGetXMLValue(psChild, "type", nullptr);

            if (pszArg != nullptr)
            {
                int iType = 0;  // Used after for.

                for (; iType <= static_cast<int>(OFTMaxType); iType++)
                {
                    if (EQUAL(pszArg, OGRFieldDefn::GetFieldTypeName(
                                          static_cast<OGRFieldType>(iType))))
                    {
                        oFieldDefn.SetType(static_cast<OGRFieldType>(iType));
                        break;
                    }
                }

                if (iType > static_cast<int>(OFTMaxType))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Unable to identify Field type '%s'.", pszArg);
                    goto error;
                }
            }

            // Subtype.
            pszArg = CPLGetXMLValue(psChild, "subtype", nullptr);
            if (pszArg != nullptr)
            {
                OGRFieldSubType eSubType = OFSTNone;

                int iType = 0;  // Used after for.
                for (; iType <= static_cast<int>(OFSTMaxSubType); iType++)
                {
                    if (EQUAL(pszArg, OGRFieldDefn::GetFieldSubTypeName(
                                          static_cast<OGRFieldSubType>(iType))))
                    {
                        eSubType = static_cast<OGRFieldSubType>(iType);
                        break;
                    }
                }

                if (iType > static_cast<int>(OFSTMaxSubType))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Unable to identify Field subtype '%s'.", pszArg);
                    goto error;
                }

                if (!OGR_AreTypeSubTypeCompatible(oFieldDefn.GetType(),
                                                  eSubType))
                {
                    CPLError(
                        CE_Failure, CPLE_AppDefined,
                        "Invalid subtype '%s' for type '%s'.", pszArg,
                        OGRFieldDefn::GetFieldTypeName(oFieldDefn.GetType()));
                    goto error;
                }

                oFieldDefn.SetSubType(eSubType);
            }

            // Width and precision.
            int nWidth = atoi(CPLGetXMLValue(psChild, "width", "0"));
            if (nWidth < 0)
            {
                CPLError(CE_Failure, CPLE_IllegalArg,
                         "Invalid width for field %s.", pszName);
                goto error;
            }
            oFieldDefn.SetWidth(nWidth);

            int nPrecision = atoi(CPLGetXMLValue(psChild, "precision", "0"));
            if (nPrecision < 0 || nPrecision > 1024)
            {
                CPLError(CE_Failure, CPLE_IllegalArg,
                         "Invalid precision for field %s.", pszName);
                goto error;
            }
            oFieldDefn.SetPrecision(nPrecision);

            // Nullable attribute.
            const bool bNullable =
                CPLTestBool(CPLGetXMLValue(psChild, "nullable", "true"));
            oFieldDefn.SetNullable(bNullable);

            // Unique attribute.
            const bool bUnique =
                CPLTestBool(CPLGetXMLValue(psChild, "unique", "false"));
            oFieldDefn.SetUnique(bUnique);

            // Default attribute.
            oFieldDefn.SetDefault(CPLGetXMLValue(psChild, "default", nullptr));

            const char *pszAlternativeName =
                CPLGetXMLValue(psChild, "alternativeName", nullptr);
            if (pszAlternativeName)
                oFieldDefn.SetAlternativeName(pszAlternativeName);

            const char *pszComment =
                CPLGetXMLValue(psChild, "comment", nullptr);
            if (pszComment)
                oFieldDefn.SetComment(pszComment);

            // Create the field.
            poFeatureDefn->AddFieldDefn(&oFieldDefn);

            abDirectCopy.push_back(FALSE);

            // Source field.
            int iSrcField =
                GetFieldIndexCaseSensitiveFirst(GetSrcLayerDefn(), pszName);

            pszArg = CPLGetXMLValue(psChild, "src", nullptr);

            if (pszArg != nullptr)
            {
                iSrcField =
                    GetFieldIndexCaseSensitiveFirst(GetSrcLayerDefn(), pszArg);
                if (iSrcField == -1)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Unable to find source field '%s'.", pszArg);
                    goto error;
                }
            }

            if (iSrcField < 0 ||
                (pszArg != nullptr && strcmp(pszArg, pszName) != 0))
            {
                bAttrFilterPassThrough = false;
            }
            else
            {
                OGRFieldDefn *poSrcFieldDefn =
                    GetSrcLayerDefn()->GetFieldDefn(iSrcField);
                if (poSrcFieldDefn->GetType() != oFieldDefn.GetType())
                    bAttrFilterPassThrough = false;
            }

            anSrcField.push_back(iSrcField);
        }
    }

    CPLAssert(poFeatureDefn->GetFieldCount() ==
              static_cast<int>(anSrcField.size()));

    // Create the schema, if it was not explicitly in the VRT.
    if (poFeatureDefn->GetFieldCount() == 0)
    {
        const int nSrcFieldCount = GetSrcLayerDefn()->GetFieldCount();

        for (int iSrcField = 0; iSrcField < nSrcFieldCount; iSrcField++)
        {
            bool bSkip = false;
            for (const auto &poProp : apoGeomFieldProps)
            {
                if (!poProp->bReportSrcColumn &&
                    (iSrcField == poProp->iGeomXField ||
                     iSrcField == poProp->iGeomYField ||
                     iSrcField == poProp->iGeomZField ||
                     iSrcField == poProp->iGeomMField ||
                     (poProp->eGeometryStyle != VGS_Direct &&
                      iSrcField == poProp->iGeomField)))
                {
                    bSkip = true;
                    break;
                }
            }
            if (bSkip)
                continue;

            poFeatureDefn->AddFieldDefn(
                GetSrcLayerDefn()->GetFieldDefn(iSrcField));
            anSrcField.push_back(iSrcField);
            abDirectCopy.push_back(TRUE);
        }

        bAttrFilterPassThrough = true;
    }

    // Is VRT layer definition identical to the source layer defn?
    // If so, use it directly, and save the translation of features.
    if (GetSrcLayerDefn() != nullptr && iFIDField == -1 && iStyleField == -1 &&
        GetSrcLayerDefn()->IsSame(poFeatureDefn))
    {
        bool bSame = true;
        for (size_t i = 0; i < apoGeomFieldProps.size(); i++)
        {
            if (apoGeomFieldProps[i]->eGeometryStyle != VGS_Direct ||
                apoGeomFieldProps[i]->iGeomField != static_cast<int>(i))
            {
                bSame = false;
                break;
            }
        }
        if (bSame)
        {
            CPLDebug("VRT", "Source feature definition is identical to VRT "
                            "feature definition. Use optimized path");
            poFeatureDefn->Release();
            poFeatureDefn = GetSrcLayerDefn();
            poFeatureDefn->Reference();
            for (int i = 0; i < poFeatureDefn->GetGeomFieldCount(); i++)
            {
                if (apoGeomFieldProps[i]->poSRS != nullptr)
                    const_cast<OGRSpatialReference *>(
                        apoGeomFieldProps[i]->poSRS)
                        ->Release();
                apoGeomFieldProps[i]->poSRS =
                    poFeatureDefn->GetGeomFieldDefn(i)->GetSpatialRef();
                if (apoGeomFieldProps[i]->poSRS != nullptr)
                    const_cast<OGRSpatialReference *>(
                        apoGeomFieldProps[i]->poSRS)
                        ->Reference();
            }
        }
    }

    CPLAssert(poFeatureDefn->GetGeomFieldCount() ==
              static_cast<int>(apoGeomFieldProps.size()));

    // Allow vrt to override whether attribute filters should be
    // passed through.
    if (CPLGetXMLValue(psLTree, "attrFilterPassThrough", nullptr) != nullptr)
        bAttrFilterPassThrough = CPLTestBool(
            CPLGetXMLValue(psLTree, "attrFilterPassThrough", "TRUE"));

    SetIgnoredFields(nullptr);

    return true;

error:
    bError = true;
    poFeatureDefn->Release();
    poFeatureDefn = new OGRFeatureDefn(osName);
    poFeatureDefn->SetGeomType(wkbNone);
    poFeatureDefn->Reference();
    apoGeomFieldProps.clear();
    return false;
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/

void OGRVRTLayer::ResetReading()
{
    bNeedReset = true;
}

/************************************************************************/
/*                         ResetSourceReading()                         */
/************************************************************************/

bool OGRVRTLayer::ResetSourceReading()

{
    bool bSuccess = true;

    // Do we want to let source layer do spatial restriction?
    char *pszFilter = nullptr;
    for (size_t i = 0; i < apoGeomFieldProps.size(); i++)
    {
        if ((m_poFilterGeom || apoGeomFieldProps[i]->poSrcRegion) &&
            apoGeomFieldProps[i]->bUseSpatialSubquery &&
            apoGeomFieldProps[i]->eGeometryStyle == VGS_PointFromColumns)
        {
            OGRFieldDefn *poXField = poSrcLayer->GetLayerDefn()->GetFieldDefn(
                apoGeomFieldProps[i]->iGeomXField);
            OGRFieldDefn *poYField = poSrcLayer->GetLayerDefn()->GetFieldDefn(
                apoGeomFieldProps[i]->iGeomYField);

            const char *pszXField = poXField->GetNameRef();
            const char *pszYField = poYField->GetNameRef();

            OGRFieldType xType = poXField->GetType();
            OGRFieldType yType = poYField->GetType();
            if (!((xType == OFTReal || xType == OFTInteger ||
                   xType == OFTInteger64) &&
                  (yType == OFTReal || yType == OFTInteger ||
                   yType == OFTInteger64)))
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "The '%s' and/or '%s' fields of the source layer "
                         "are not declared as numeric fields, "
                         "so the spatial filter cannot be turned into an "
                         "attribute filter on them",
                         pszXField, pszYField);
                apoGeomFieldProps[i]->bUseSpatialSubquery = false;
            }

            if (apoGeomFieldProps[i]->bUseSpatialSubquery)
            {
                OGREnvelope sEnvelope;
                CPLString osFilter;

                if (apoGeomFieldProps[i]->poSrcRegion != nullptr)
                {
                    if (m_poFilterGeom == nullptr)
                    {
                        apoGeomFieldProps[i]->poSrcRegion->getEnvelope(
                            &sEnvelope);
                    }
                    else
                    {
                        auto poIntersection = std::unique_ptr<OGRGeometry>(
                            apoGeomFieldProps[i]->poSrcRegion->Intersection(
                                m_poFilterGeom));
                        if (poIntersection && !poIntersection->IsEmpty())
                        {
                            poIntersection->getEnvelope(&sEnvelope);
                        }
                        else
                        {
                            sEnvelope.MinX = 0;
                            sEnvelope.MaxX = 0;
                            sEnvelope.MinY = 0;
                            sEnvelope.MaxY = 0;
                        }
                    }
                }
                else
                {
                    CPLAssert(m_poFilterGeom);
                    m_poFilterGeom->getEnvelope(&sEnvelope);
                }

                if (!std::isinf(sEnvelope.MinX))
                    osFilter +=
                        CPLSPrintf("\"%s\" > %.15g", pszXField, sEnvelope.MinX);
                else if (sEnvelope.MinX > 0)
                    osFilter += "0 = 1";

                if (!std::isinf(sEnvelope.MaxX))
                {
                    if (!osFilter.empty())
                        osFilter += " AND ";
                    osFilter +=
                        CPLSPrintf("\"%s\" < %.15g", pszXField, sEnvelope.MaxX);
                }
                else if (sEnvelope.MaxX < 0)
                {
                    if (!osFilter.empty())
                        osFilter += " AND ";
                    osFilter += "0 = 1";
                }

                if (!std::isinf(sEnvelope.MinY))
                {
                    if (!osFilter.empty())
                        osFilter += " AND ";
                    osFilter +=
                        CPLSPrintf("\"%s\" > %.15g", pszYField, sEnvelope.MinY);
                }
                else if (sEnvelope.MinY > 0)
                {
                    if (!osFilter.empty())
                        osFilter += " AND ";
                    osFilter += "0 = 1";
                }

                if (!std::isinf(sEnvelope.MaxY))
                {
                    if (!osFilter.empty())
                        osFilter += " AND ";
                    osFilter +=
                        CPLSPrintf("\"%s\" < %.15g", pszYField, sEnvelope.MaxY);
                }
                else if (sEnvelope.MaxY < 0)
                {
                    if (!osFilter.empty())
                        osFilter += " AND ";
                    osFilter += "0 = 1";
                }

                if (!osFilter.empty())
                {
                    pszFilter = CPLStrdup(osFilter);
                }
            }

            // Just do it on one geometry field. To complicated otherwise!
            break;
        }
    }

    // Install spatial + attr filter query on source layer.
    if (pszFilter == nullptr && pszAttrFilter == nullptr)
    {
        bSuccess = poSrcLayer->SetAttributeFilter(nullptr) == OGRERR_NONE;
    }
    else if (pszFilter != nullptr && pszAttrFilter == nullptr)
    {
        bSuccess = poSrcLayer->SetAttributeFilter(pszFilter) == OGRERR_NONE;
    }
    else if (pszFilter == nullptr && pszAttrFilter != nullptr)
    {
        bSuccess = poSrcLayer->SetAttributeFilter(pszAttrFilter) == OGRERR_NONE;
    }
    else
    {
        CPLString osMerged = pszFilter;

        osMerged += " AND (";
        osMerged += pszAttrFilter;
        osMerged += ")";

        bSuccess = poSrcLayer->SetAttributeFilter(osMerged) == OGRERR_NONE;
    }

    CPLFree(pszFilter);

    m_bEmptyResultSet = false;

    // Clear spatial filter (to be safe) for non direct geometries
    // and reset reading.
    if (m_iGeomFieldFilter < static_cast<int>(apoGeomFieldProps.size()) &&
        apoGeomFieldProps[m_iGeomFieldFilter]->eGeometryStyle == VGS_Direct &&
        apoGeomFieldProps[m_iGeomFieldFilter]->iGeomField >= 0)
    {
        OGRGeometry *poNewSpatialGeom = nullptr;
        OGRGeometry *poSrcRegion =
            apoGeomFieldProps[m_iGeomFieldFilter]->poSrcRegion.get();
        std::unique_ptr<OGRGeometry> poIntersection;

        if (poSrcRegion == nullptr)
        {
            poNewSpatialGeom = m_poFilterGeom;
        }
        else if (m_poFilterGeom == nullptr)
        {
            poNewSpatialGeom = poSrcRegion;
        }
        else
        {
            bool bDoIntersection = true;
            if (m_bFilterIsEnvelope)
            {
                OGREnvelope sEnvelope;
                m_poFilterGeom->getEnvelope(&sEnvelope);
                if (std::isinf(sEnvelope.MinX) && std::isinf(sEnvelope.MinY) &&
                    std::isinf(sEnvelope.MaxX) && std::isinf(sEnvelope.MaxY) &&
                    sEnvelope.MinX < 0 && sEnvelope.MinY < 0 &&
                    sEnvelope.MaxX > 0 && sEnvelope.MaxY > 0)
                {
                    poNewSpatialGeom = poSrcRegion;
                    bDoIntersection = false;
                }
            }
            if (bDoIntersection)
            {
                poIntersection.reset(m_poFilterGeom->Intersection(poSrcRegion));
                poNewSpatialGeom = poIntersection.get();
                if (!poIntersection)
                    m_bEmptyResultSet = true;
            }
        }
        poSrcLayer->SetSpatialFilter(
            apoGeomFieldProps[m_iGeomFieldFilter]->iGeomField,
            poNewSpatialGeom);
    }
    else
    {
        poSrcLayer->SetSpatialFilter(nullptr);
    }
    poSrcLayer->ResetReading();
    bNeedReset = false;

    return bSuccess;
}

/************************************************************************/
/*                           GetNextFeature()                           */
/************************************************************************/

OGRFeature *OGRVRTLayer::GetNextFeature()

{
    if (m_bEmptyResultSet)
        return nullptr;
    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || poDS->GetRecursionDetected())
        return nullptr;
    if (bError)
        return nullptr;

    if (bNeedReset)
    {
        if (!ResetSourceReading())
            return nullptr;
    }

    for (; true;)
    {
        OGRFeature *poSrcFeature = poSrcLayer->GetNextFeature();
        if (poSrcFeature == nullptr)
            return nullptr;

        OGRFeature *poFeature = nullptr;
        if (poFeatureDefn == GetSrcLayerDefn())
        {
            poFeature = poSrcFeature;
            ClipAndAssignSRS(poFeature);
        }
        else
        {
            poFeature = TranslateFeature(poSrcFeature, TRUE);
            delete poSrcFeature;
        }

        if (poFeature == nullptr)
            return nullptr;

        if (((m_iGeomFieldFilter < static_cast<int>(apoGeomFieldProps.size()) &&
              apoGeomFieldProps[m_iGeomFieldFilter]->eGeometryStyle ==
                  VGS_Direct) ||
             m_poFilterGeom == nullptr ||
             FilterGeometry(poFeature->GetGeomFieldRef(m_iGeomFieldFilter))) &&
            (m_poAttrQuery == nullptr || m_poAttrQuery->Evaluate(poFeature)))
            return poFeature;

        delete poFeature;
    }
}

/************************************************************************/
/*                          ClipAndAssignSRS()                          */
/************************************************************************/

void OGRVRTLayer::ClipAndAssignSRS(OGRFeature *poFeature)
{
    for (int i = 0; i < poFeatureDefn->GetGeomFieldCount(); i++)
    {
        // Clip the geometry to the SrcRegion if asked.
        OGRGeometry *poGeom = poFeature->GetGeomFieldRef(i);
        if (apoGeomFieldProps[i]->poSrcRegion != nullptr &&
            apoGeomFieldProps[i]->bSrcClip && poGeom != nullptr)
        {
            poGeom =
                poGeom->Intersection(apoGeomFieldProps[i]->poSrcRegion.get());
            if (poGeom != nullptr)
                poGeom->assignSpatialReference(
                    GetLayerDefn()->GetGeomFieldDefn(i)->GetSpatialRef());

            poFeature->SetGeomFieldDirectly(i, poGeom);
        }
        else if (poGeom != nullptr)
            poGeom->assignSpatialReference(
                GetLayerDefn()->GetGeomFieldDefn(i)->GetSpatialRef());
    }
}

/************************************************************************/
/*                          TranslateFeature()                          */
/*                                                                      */
/*      Translate a source feature into a feature for this layer.       */
/************************************************************************/

OGRFeature *OGRVRTLayer::TranslateFeature(OGRFeature *&poSrcFeat,
                                          int bUseSrcRegion)

{
retry:
    OGRFeature *poDstFeat = new OGRFeature(poFeatureDefn);

    m_nFeaturesRead++;

    // Handle FID.
    if (iFIDField == -1)
        poDstFeat->SetFID(poSrcFeat->GetFID());
    else
        poDstFeat->SetFID(poSrcFeat->GetFieldAsInteger64(iFIDField));

    // Handle style string.
    if (iStyleField != -1)
    {
        if (poSrcFeat->IsFieldSetAndNotNull(iStyleField))
            poDstFeat->SetStyleString(poSrcFeat->GetFieldAsString(iStyleField));
    }
    else
    {
        if (poSrcFeat->GetStyleString() != nullptr)
            poDstFeat->SetStyleString(poSrcFeat->GetStyleString());
    }

    for (int i = 0; i < poFeatureDefn->GetGeomFieldCount(); i++)
    {
        OGRVRTGeometryStyle eGeometryStyle =
            apoGeomFieldProps[i]->eGeometryStyle;
        int iGeomField = apoGeomFieldProps[i]->iGeomField;

        // Handle the geometry.  Eventually there will be several more
        // supported options.
        if (eGeometryStyle == VGS_None ||
            GetLayerDefn()->GetGeomFieldDefn(i)->IsIgnored())
        {
            // Do nothing.
        }
        else if (eGeometryStyle == VGS_WKT && iGeomField != -1)
        {
            const char *pszWKT = poSrcFeat->GetFieldAsString(iGeomField);

            if (pszWKT != nullptr)
            {
                auto [poGeom, _] = OGRGeometryFactory::createFromWkt(pszWKT);
                if (poGeom == nullptr)
                    CPLDebug("OGR_VRT", "Did not get geometry from %s", pszWKT);

                poDstFeat->SetGeomField(i, std::move(poGeom));
            }
        }
        else if (eGeometryStyle == VGS_WKB && iGeomField != -1)
        {
            int nBytes = 0;
            GByte *pabyWKB = nullptr;
            bool bNeedFree = false;

            if (poSrcFeat->GetFieldDefnRef(iGeomField)->GetType() == OFTBinary)
            {
                pabyWKB = poSrcFeat->GetFieldAsBinary(iGeomField, &nBytes);
            }
            else
            {
                const char *pszWKT = poSrcFeat->GetFieldAsString(iGeomField);

                pabyWKB = CPLHexToBinary(pszWKT, &nBytes);
                bNeedFree = true;
            }

            if (pabyWKB != nullptr)
            {
                OGRGeometry *poGeom = nullptr;

                if (OGRGeometryFactory::createFromWkb(pabyWKB, nullptr, &poGeom,
                                                      nBytes) == OGRERR_NONE)
                    poDstFeat->SetGeomFieldDirectly(i, poGeom);
            }

            if (bNeedFree)
                CPLFree(pabyWKB);
        }
        else if (eGeometryStyle == VGS_Shape && iGeomField != -1)
        {
            int nBytes = 0;
            GByte *pabyWKB = nullptr;
            bool bNeedFree = false;

            if (poSrcFeat->GetFieldDefnRef(iGeomField)->GetType() == OFTBinary)
            {
                pabyWKB = poSrcFeat->GetFieldAsBinary(iGeomField, &nBytes);
            }
            else
            {
                const char *pszWKT = poSrcFeat->GetFieldAsString(iGeomField);

                pabyWKB = CPLHexToBinary(pszWKT, &nBytes);
                bNeedFree = true;
            }

            if (pabyWKB != nullptr)
            {
                OGRGeometry *poGeom = nullptr;

                if (OGRCreateFromShapeBin(pabyWKB, &poGeom, nBytes) ==
                    OGRERR_NONE)
                    poDstFeat->SetGeomFieldDirectly(i, poGeom);
            }

            if (bNeedFree)
                CPLFree(pabyWKB);
        }
        else if (eGeometryStyle == VGS_Direct && iGeomField != -1)
        {
            poDstFeat->SetGeomField(i, poSrcFeat->GetGeomFieldRef(iGeomField));
        }
        else if (eGeometryStyle == VGS_PointFromColumns)
        {
            OGRPoint *poPoint = nullptr;
            if (apoGeomFieldProps[i]->iGeomZField != -1)
            {
                poPoint = new OGRPoint(poSrcFeat->GetFieldAsDouble(
                                           apoGeomFieldProps[i]->iGeomXField),
                                       poSrcFeat->GetFieldAsDouble(
                                           apoGeomFieldProps[i]->iGeomYField),
                                       poSrcFeat->GetFieldAsDouble(
                                           apoGeomFieldProps[i]->iGeomZField));
            }
            else
            {
                poPoint = new OGRPoint(poSrcFeat->GetFieldAsDouble(
                                           apoGeomFieldProps[i]->iGeomXField),
                                       poSrcFeat->GetFieldAsDouble(
                                           apoGeomFieldProps[i]->iGeomYField));
            }
            if (apoGeomFieldProps[i]->iGeomMField >= 0)
            {
                poPoint->setM(poSrcFeat->GetFieldAsDouble(
                    apoGeomFieldProps[i]->iGeomMField));
            }
            poDstFeat->SetGeomFieldDirectly(i, poPoint);
        }
        else
        {
            // Add other options here.
        }

        // In the non-direct case, we need to check that the geometry
        // intersects the source region before an optional clipping.
        if (bUseSrcRegion &&
            apoGeomFieldProps[i]->eGeometryStyle != VGS_Direct &&
            apoGeomFieldProps[i]->poSrcRegion != nullptr)
        {
            OGRGeometry *poGeom = poDstFeat->GetGeomFieldRef(i);
            if (poGeom != nullptr &&
                !poGeom->Intersects(apoGeomFieldProps[i]->poSrcRegion.get()))
            {
                delete poSrcFeat;
                delete poDstFeat;

                // Fetch next source feature and retry translating it.
                poSrcFeat = poSrcLayer->GetNextFeature();
                if (poSrcFeat == nullptr)
                    return nullptr;

                goto retry;
            }
        }
    }

    ClipAndAssignSRS(poDstFeat);

    // Copy fields.
    for (int iVRTField = 0; iVRTField < poFeatureDefn->GetFieldCount();
         iVRTField++)
    {
        if (anSrcField[iVRTField] == -1)
            continue;

        OGRFieldDefn *poDstDefn = poFeatureDefn->GetFieldDefn(iVRTField);
        OGRFieldDefn *poSrcDefn =
            poSrcLayer->GetLayerDefn()->GetFieldDefn(anSrcField[iVRTField]);

        if (!poSrcFeat->IsFieldSetAndNotNull(anSrcField[iVRTField]) ||
            poDstDefn->IsIgnored())
            continue;

        if (abDirectCopy[iVRTField] &&
            poDstDefn->GetType() == poSrcDefn->GetType())
        {
            poDstFeat->SetField(
                iVRTField, poSrcFeat->GetRawFieldRef(anSrcField[iVRTField]));
        }
        else
        {
            // Eventually we need to offer some more sophisticated translation
            // options here for more esoteric types.
            if (poDstDefn->GetType() == OFTReal)
                poDstFeat->SetField(iVRTField, poSrcFeat->GetFieldAsDouble(
                                                   anSrcField[iVRTField]));
            else
                poDstFeat->SetField(iVRTField, poSrcFeat->GetFieldAsString(
                                                   anSrcField[iVRTField]));
        }
    }

    return poDstFeat;
}

/************************************************************************/
/*                             GetFeature()                             */
/************************************************************************/

OGRFeature *OGRVRTLayer::GetFeature(GIntBig nFeatureId)

{
    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || poDS->GetRecursionDetected())
        return nullptr;

    bNeedReset = true;

    // If the FID is directly mapped, we can do a simple
    // GetFeature() to get our target feature.  Otherwise we need
    // to setup an appropriate query to get it.
    OGRFeature *poSrcFeature = nullptr;
    OGRFeature *poFeature = nullptr;

    if (iFIDField == -1)
    {
        poSrcFeature = poSrcLayer->GetFeature(nFeatureId);
    }
    else
    {
        const char *pszFID =
            poSrcLayer->GetLayerDefn()->GetFieldDefn(iFIDField)->GetNameRef();
        char *pszFIDQuery = static_cast<char *>(CPLMalloc(strlen(pszFID) + 64));

        poSrcLayer->ResetReading();
        snprintf(pszFIDQuery, strlen(pszFID) + 64, "%s = " CPL_FRMT_GIB, pszFID,
                 nFeatureId);
        poSrcLayer->SetSpatialFilter(nullptr);
        poSrcLayer->SetAttributeFilter(pszFIDQuery);
        CPLFree(pszFIDQuery);

        poSrcFeature = poSrcLayer->GetNextFeature();
    }

    if (poSrcFeature == nullptr)
        return nullptr;

    // Translate feature and return it.
    if (poFeatureDefn == GetSrcLayerDefn())
    {
        poFeature = poSrcFeature;
        ClipAndAssignSRS(poFeature);
    }
    else
    {
        poFeature = TranslateFeature(poSrcFeature, FALSE);
        delete poSrcFeature;
    }

    return poFeature;
}

/************************************************************************/
/*                          SetNextByIndex()                            */
/************************************************************************/

OGRErr OGRVRTLayer::SetNextByIndex(GIntBig nIndex)
{
    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || poDS->GetRecursionDetected())
        return OGRERR_FAILURE;

    if (bNeedReset)
    {
        if (!ResetSourceReading())
            return OGRERR_FAILURE;
    }

    if (TestCapability(OLCFastSetNextByIndex))
        return poSrcLayer->SetNextByIndex(nIndex);

    return OGRLayer::SetNextByIndex(nIndex);
}

/************************************************************************/
/*               TranslateVRTFeatureToSrcFeature()                      */
/*                                                                      */
/*      Translate a VRT feature into a feature for the source layer     */
/************************************************************************/

OGRFeature *
OGRVRTLayer::TranslateVRTFeatureToSrcFeature(OGRFeature *poVRTFeature)
{
    OGRFeature *poSrcFeat = new OGRFeature(poSrcLayer->GetLayerDefn());

    poSrcFeat->SetFID(poVRTFeature->GetFID());

    // Handle style string.
    if (iStyleField != -1)
    {
        if (poVRTFeature->GetStyleString() != nullptr)
            poSrcFeat->SetField(iStyleField, poVRTFeature->GetStyleString());
    }
    else
    {
        if (poVRTFeature->GetStyleString() != nullptr)
            poSrcFeat->SetStyleString(poVRTFeature->GetStyleString());
    }

    // Handle the geometry.  Eventually there will be several more
    // supported options.
    for (int i = 0; i < poFeatureDefn->GetGeomFieldCount(); i++)
    {
        OGRVRTGeometryStyle eGeometryStyle =
            apoGeomFieldProps[i]->eGeometryStyle;
        int iGeomField = apoGeomFieldProps[i]->iGeomField;

        if (eGeometryStyle == VGS_None)
        {
            // Do nothing.
        }
        else if (eGeometryStyle == VGS_WKT && iGeomField >= 0)
        {
            OGRGeometry *poGeom = poVRTFeature->GetGeomFieldRef(i);
            if (poGeom != nullptr)
            {
                char *pszWKT = nullptr;
                if (poGeom->exportToWkt(&pszWKT) == OGRERR_NONE)
                {
                    poSrcFeat->SetField(iGeomField, pszWKT);
                }
                CPLFree(pszWKT);
            }
        }
        else if (eGeometryStyle == VGS_WKB && iGeomField >= 0)
        {
            OGRGeometry *poGeom = poVRTFeature->GetGeomFieldRef(i);
            if (poGeom != nullptr)
            {
                const size_t nSize = poGeom->WkbSize();
                if (nSize >
                    static_cast<size_t>(std::numeric_limits<int>::max()))
                {
                }
                else
                {
                    GByte *pabyData =
                        static_cast<GByte *>(VSI_MALLOC_VERBOSE(nSize));
                    if (pabyData &&
                        poGeom->exportToWkb(wkbNDR, pabyData) == OGRERR_NONE)
                    {
                        if (poSrcFeat->GetFieldDefnRef(iGeomField)->GetType() ==
                            OFTBinary)
                        {
                            poSrcFeat->SetField(
                                iGeomField, static_cast<int>(nSize), pabyData);
                        }
                        else
                        {
                            char *pszHexWKB = CPLBinaryToHex(
                                static_cast<int>(nSize), pabyData);
                            poSrcFeat->SetField(iGeomField, pszHexWKB);
                            CPLFree(pszHexWKB);
                        }
                    }
                    CPLFree(pabyData);
                }
            }
        }
        else if (eGeometryStyle == VGS_Shape)
        {
            CPLDebug("OGR_VRT", "Update of VGS_Shape geometries not supported");
        }
        else if (eGeometryStyle == VGS_Direct && iGeomField >= 0)
        {
            poSrcFeat->SetGeomField(iGeomField,
                                    poVRTFeature->GetGeomFieldRef(i));
        }
        else if (eGeometryStyle == VGS_PointFromColumns)
        {
            OGRGeometry *poGeom = poVRTFeature->GetGeomFieldRef(i);
            if (poGeom != nullptr)
            {
                if (wkbFlatten(poGeom->getGeometryType()) != wkbPoint)
                {
                    CPLError(CE_Warning, CPLE_NotSupported,
                             "Cannot set a non ponctual geometry for "
                             "PointFromColumns geometry");
                }
                else
                {
                    auto poPoint = poGeom->toPoint();
                    poSrcFeat->SetField(apoGeomFieldProps[i]->iGeomXField,
                                        poPoint->getX());
                    poSrcFeat->SetField(apoGeomFieldProps[i]->iGeomYField,
                                        poPoint->getY());
                    if (apoGeomFieldProps[i]->iGeomZField != -1)
                    {
                        poSrcFeat->SetField(apoGeomFieldProps[i]->iGeomZField,
                                            poPoint->getZ());
                    }
                    if (apoGeomFieldProps[i]->iGeomMField != -1)
                    {
                        poSrcFeat->SetField(apoGeomFieldProps[i]->iGeomMField,
                                            poPoint->getM());
                    }
                }
            }
        }
        else
        {
            // Add other options here.
        }

        OGRGeometry *poGeom = poSrcFeat->GetGeomFieldRef(i);
        if (poGeom != nullptr)
            poGeom->assignSpatialReference(
                GetLayerDefn()->GetGeomFieldDefn(i)->GetSpatialRef());
    }

    // Copy fields.
    for (int iVRTField = 0; iVRTField < poFeatureDefn->GetFieldCount();
         iVRTField++)
    {
        bool bSkip = false;
        for (int i = 0; i < poFeatureDefn->GetGeomFieldCount(); i++)
        {
            // Do not set source geometry columns. Have been set just above.
            if ((apoGeomFieldProps[i]->eGeometryStyle != VGS_Direct &&
                 anSrcField[iVRTField] == apoGeomFieldProps[i]->iGeomField) ||
                anSrcField[iVRTField] == apoGeomFieldProps[i]->iGeomXField ||
                anSrcField[iVRTField] == apoGeomFieldProps[i]->iGeomYField ||
                anSrcField[iVRTField] == apoGeomFieldProps[i]->iGeomZField ||
                anSrcField[iVRTField] == apoGeomFieldProps[i]->iGeomMField)
            {
                bSkip = true;
                break;
            }
        }
        if (bSkip)
            continue;

        OGRFieldDefn *poVRTDefn = poFeatureDefn->GetFieldDefn(iVRTField);
        OGRFieldDefn *poSrcDefn =
            poSrcLayer->GetLayerDefn()->GetFieldDefn(anSrcField[iVRTField]);

        if (abDirectCopy[iVRTField] &&
            poVRTDefn->GetType() == poSrcDefn->GetType())
        {
            poSrcFeat->SetField(anSrcField[iVRTField],
                                poVRTFeature->GetRawFieldRef(iVRTField));
        }
        else
        {
            // Eventually we need to offer some more sophisticated translation
            // options here for more esoteric types.
            poSrcFeat->SetField(anSrcField[iVRTField],
                                poVRTFeature->GetFieldAsString(iVRTField));
        }
    }

    return poSrcFeat;
}

/************************************************************************/
/*                           ICreateFeature()                            */
/************************************************************************/

OGRErr OGRVRTLayer::ICreateFeature(OGRFeature *poVRTFeature)
{
    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || poDS->GetRecursionDetected())
        return OGRERR_FAILURE;

    if (!bUpdate)
    {
        CPLError(CE_Failure, CPLE_NotSupported, UNSUPPORTED_OP_READ_ONLY,
                 "CreateFeature");
        return OGRERR_FAILURE;
    }

    if (iFIDField != -1)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "The CreateFeature() operation is not supported "
                 "if the FID option is specified.");
        return OGRERR_FAILURE;
    }

    if (GetSrcLayerDefn() == poFeatureDefn)
        return poSrcLayer->CreateFeature(poVRTFeature);

    OGRFeature *poSrcFeature = TranslateVRTFeatureToSrcFeature(poVRTFeature);
    poSrcFeature->SetFID(OGRNullFID);
    OGRErr eErr = poSrcLayer->CreateFeature(poSrcFeature);
    if (eErr == OGRERR_NONE)
    {
        poVRTFeature->SetFID(poSrcFeature->GetFID());
    }
    delete poSrcFeature;
    return eErr;
}

/************************************************************************/
/*                             ISetFeature()                             */
/************************************************************************/

OGRErr OGRVRTLayer::ISetFeature(OGRFeature *poVRTFeature)
{
    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || poDS->GetRecursionDetected())
        return OGRERR_FAILURE;

    if (!bUpdate)
    {
        CPLError(CE_Failure, CPLE_NotSupported, UNSUPPORTED_OP_READ_ONLY,
                 "SetFeature");
        return OGRERR_FAILURE;
    }

    if (iFIDField != -1)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "The SetFeature() operation is not supported "
                 "if the FID option is specified.");
        return OGRERR_FAILURE;
    }

    if (GetSrcLayerDefn() == poFeatureDefn)
        return poSrcLayer->SetFeature(poVRTFeature);

    OGRFeature *poSrcFeature = TranslateVRTFeatureToSrcFeature(poVRTFeature);
    OGRErr eErr = poSrcLayer->SetFeature(poSrcFeature);
    delete poSrcFeature;
    return eErr;
}

/************************************************************************/
/*                           DeleteFeature()                            */
/************************************************************************/

OGRErr OGRVRTLayer::DeleteFeature(GIntBig nFID)

{
    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || poDS->GetRecursionDetected())
        return OGRERR_FAILURE;

    if (!bUpdate)
    {
        CPLError(CE_Failure, CPLE_NotSupported, UNSUPPORTED_OP_READ_ONLY,
                 "DeleteFeature");
        return OGRERR_FAILURE;
    }

    if (iFIDField != -1)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "The DeleteFeature() operation is not supported "
                 "if the FID option is specified.");
        return OGRERR_FAILURE;
    }

    return poSrcLayer->DeleteFeature(nFID);
}

/************************************************************************/
/*                         SetAttributeFilter()                         */
/************************************************************************/

OGRErr OGRVRTLayer::SetAttributeFilter(const char *pszNewQuery)

{
    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || poDS->GetRecursionDetected())
        return OGRERR_FAILURE;

    if (bAttrFilterPassThrough)
    {
        CPLFree(pszAttrFilter);
        if (pszNewQuery == nullptr || strlen(pszNewQuery) == 0)
            pszAttrFilter = nullptr;
        else
            pszAttrFilter = CPLStrdup(pszNewQuery);

        ResetReading();
        return OGRERR_NONE;
    }
    else
    {
        // Setup m_poAttrQuery.
        return OGRLayer::SetAttributeFilter(pszNewQuery);
    }
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRVRTLayer::TestCapability(const char *pszCap)

{
    if (EQUAL(pszCap, OLCFastFeatureCount) && nFeatureCount >= 0 &&
        m_poFilterGeom == nullptr && m_poAttrQuery == nullptr)
        return TRUE;

    if (EQUAL(pszCap, OLCFastGetExtent) && apoGeomFieldProps.size() == 1 &&
        apoGeomFieldProps[0]->sStaticEnvelope.IsInit())
        return TRUE;

    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || poDS->GetRecursionDetected())
        return FALSE;

    if (EQUAL(pszCap, OLCFastFeatureCount) ||
        EQUAL(pszCap, OLCFastSetNextByIndex))
    {
        if (m_poAttrQuery == nullptr)
        {
            bool bForward = true;
            for (size_t i = 0; i < apoGeomFieldProps.size(); i++)
            {
                if (!(apoGeomFieldProps[i]->eGeometryStyle == VGS_Direct ||
                      (apoGeomFieldProps[i]->poSrcRegion == nullptr &&
                       m_poFilterGeom == nullptr)))
                {
                    bForward = false;
                    break;
                }
            }
            if (bForward)
            {
                return poSrcLayer->TestCapability(pszCap);
            }
        }
        return FALSE;
    }

    else if (EQUAL(pszCap, OLCFastSpatialFilter))
        return apoGeomFieldProps.size() == 1 &&
               apoGeomFieldProps[0]->eGeometryStyle == VGS_Direct &&
               m_poAttrQuery == nullptr && poSrcLayer->TestCapability(pszCap);

    else if (EQUAL(pszCap, OLCFastGetExtent))
        return apoGeomFieldProps.size() == 1 &&
               apoGeomFieldProps[0]->eGeometryStyle == VGS_Direct &&
               m_poAttrQuery == nullptr &&
               (apoGeomFieldProps[0]->poSrcRegion == nullptr ||
                apoGeomFieldProps[0]->bSrcClip) &&
               poSrcLayer->TestCapability(pszCap);

    else if (EQUAL(pszCap, OLCRandomRead))
        return iFIDField == -1 && poSrcLayer->TestCapability(pszCap);

    else if (EQUAL(pszCap, OLCSequentialWrite) ||
             EQUAL(pszCap, OLCRandomWrite) || EQUAL(pszCap, OLCDeleteFeature))
        return bUpdate && iFIDField == -1 && poSrcLayer->TestCapability(pszCap);

    else if (EQUAL(pszCap, OLCStringsAsUTF8))
        return poSrcLayer->TestCapability(pszCap);

    else if (EQUAL(pszCap, OLCTransactions))
        return bUpdate && poSrcLayer->TestCapability(pszCap);

    else if (EQUAL(pszCap, OLCIgnoreFields) ||
             EQUAL(pszCap, OLCCurveGeometries) ||
             EQUAL(pszCap, OLCZGeometries) ||
             EQUAL(pszCap, OLCMeasuredGeometries))
        return poSrcLayer->TestCapability(pszCap);

    return FALSE;
}

/************************************************************************/
/*                             IGetExtent()                             */
/************************************************************************/

OGRErr OGRVRTLayer::IGetExtent(int iGeomField, OGREnvelope *psExtent,
                               bool bForce)
{
    if (static_cast<size_t>(iGeomField) >= apoGeomFieldProps.size())
        return OGRERR_FAILURE;

    if (apoGeomFieldProps[iGeomField]->sStaticEnvelope.IsInit())
    {
        *psExtent = apoGeomFieldProps[iGeomField]->sStaticEnvelope;
        return OGRERR_NONE;
    }

    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || poDS->GetRecursionDetected())
        return OGRERR_FAILURE;

    if (apoGeomFieldProps[iGeomField]->eGeometryStyle == VGS_Direct &&
        m_poAttrQuery == nullptr &&
        (apoGeomFieldProps[iGeomField]->poSrcRegion == nullptr ||
         apoGeomFieldProps[iGeomField]->bSrcClip))
    {
        if (bNeedReset)
            ResetSourceReading();

        OGRErr eErr = poSrcLayer->GetExtent(
            apoGeomFieldProps[iGeomField]->iGeomField, psExtent, bForce);
        if (eErr != OGRERR_NONE ||
            apoGeomFieldProps[iGeomField]->poSrcRegion == nullptr)
            return eErr;

        OGREnvelope sSrcRegionEnvelope;
        apoGeomFieldProps[iGeomField]->poSrcRegion->getEnvelope(
            &sSrcRegionEnvelope);

        psExtent->Intersect(sSrcRegionEnvelope);
        return eErr;
    }

    return OGRLayer::IGetExtent(iGeomField, psExtent, bForce);
}

/************************************************************************/
/*                          GetFeatureCount()                           */
/************************************************************************/

GIntBig OGRVRTLayer::GetFeatureCount(int bForce)

{
    if (m_bEmptyResultSet)
        return 0;
    if (nFeatureCount >= 0 && m_poFilterGeom == nullptr &&
        m_poAttrQuery == nullptr)
    {
        return nFeatureCount;
    }

    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || poDS->GetRecursionDetected())
        return 0;

    if (TestCapability(OLCFastFeatureCount))
    {
        if (bNeedReset)
            ResetSourceReading();

        return poSrcLayer->GetFeatureCount(bForce);
    }

    return OGRLayer::GetFeatureCount(bForce);
}

/************************************************************************/
/*                          ISetSpatialFilter()                         */
/************************************************************************/

OGRErr OGRVRTLayer::ISetSpatialFilter(int iGeomField,
                                      const OGRGeometry *poGeomIn)
{
    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || poDS->GetRecursionDetected())
        return OGRERR_FAILURE;

    if (iGeomField >= 0 && iGeomField < GetLayerDefn()->GetGeomFieldCount() &&
        apoGeomFieldProps[iGeomField]->eGeometryStyle == VGS_Direct)
        bNeedReset = true;

    m_iGeomFieldFilter = iGeomField;
    if (InstallFilter(poGeomIn))
        ResetReading();
    return OGRERR_NONE;
}

/************************************************************************/
/*                             SyncToDisk()                             */
/************************************************************************/

OGRErr OGRVRTLayer::SyncToDisk()
{
    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || poDS->GetRecursionDetected())
        return OGRERR_FAILURE;

    return poSrcLayer->SyncToDisk();
}

/************************************************************************/
/*                            GetLayerDefn()                            */
/************************************************************************/

OGRFeatureDefn *OGRVRTLayer::GetLayerDefn()
{
    if (!bHasFullInitialized)
        FullInitialize();

    return poFeatureDefn;
}

/************************************************************************/
/*                             GetGeomType()                            */
/************************************************************************/

OGRwkbGeometryType OGRVRTLayer::GetGeomType()
{
    if (CPLGetXMLValue(psLTree, "GeometryType", nullptr) != nullptr ||
        CPLGetXMLValue(psLTree, "GeometryField.GeometryType", nullptr) !=
            nullptr)
    {
        if (apoGeomFieldProps.size() >= 1)
            return apoGeomFieldProps[0]->eGeomType;
        return wkbNone;
    }

    return GetLayerDefn()->GetGeomType();
}

/************************************************************************/
/*                             GetFIDColumn()                           */
/************************************************************************/

const char *OGRVRTLayer::GetFIDColumn()
{
    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || poDS->GetRecursionDetected())
        return "";

    return osFIDFieldName;
}

/************************************************************************/
/*                           StartTransaction()                         */
/************************************************************************/

OGRErr OGRVRTLayer::StartTransaction()
{
    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || !bUpdate || poDS->GetRecursionDetected())
        return OGRERR_FAILURE;

    return poSrcLayer->StartTransaction();
}

/************************************************************************/
/*                           CommitTransaction()                        */
/************************************************************************/

OGRErr OGRVRTLayer::CommitTransaction()
{
    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || !bUpdate || poDS->GetRecursionDetected())
        return OGRERR_FAILURE;

    return poSrcLayer->CommitTransaction();
}

/************************************************************************/
/*                          RollbackTransaction()                       */
/************************************************************************/

OGRErr OGRVRTLayer::RollbackTransaction()
{
    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || !bUpdate || poDS->GetRecursionDetected())
        return OGRERR_FAILURE;

    return poSrcLayer->RollbackTransaction();
}

/************************************************************************/
/*                          SetIgnoredFields()                          */
/************************************************************************/

OGRErr OGRVRTLayer::SetIgnoredFields(CSLConstList papszFields)
{
    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || poDS->GetRecursionDetected())
        return OGRERR_FAILURE;

    if (!poSrcLayer->TestCapability(OLCIgnoreFields))
        return OGRERR_FAILURE;

    OGRErr eErr = OGRLayer::SetIgnoredFields(papszFields);
    if (eErr != OGRERR_NONE)
        return eErr;

    CPLStringList aosFieldsSrc;

    // Translate explicitly ignored fields of VRT layers to their equivalent
    // source fields.
    for (const char *pszFieldName : cpl::Iterate(papszFields))
    {
        if (EQUAL(pszFieldName, "OGR_GEOMETRY") ||
            EQUAL(pszFieldName, "OGR_STYLE"))
        {
            aosFieldsSrc.AddString(pszFieldName);
        }
        else
        {
            int iVRTField =
                GetFieldIndexCaseSensitiveFirst(GetLayerDefn(), pszFieldName);
            if (iVRTField >= 0)
            {
                int iSrcField = anSrcField[iVRTField];
                if (iSrcField >= 0)
                {
                    // If we are asked to ignore x or y for a
                    // VGS_PointFromColumns geometry field, we must NOT pass
                    // that order to the underlying layer.
                    bool bOKToIgnore = true;
                    for (int iGeomVRTField = 0;
                         iGeomVRTField < GetLayerDefn()->GetGeomFieldCount();
                         iGeomVRTField++)
                    {
                        if (iSrcField ==
                                apoGeomFieldProps[iGeomVRTField]->iGeomXField ||
                            iSrcField ==
                                apoGeomFieldProps[iGeomVRTField]->iGeomYField ||
                            iSrcField ==
                                apoGeomFieldProps[iGeomVRTField]->iGeomZField ||
                            iSrcField ==
                                apoGeomFieldProps[iGeomVRTField]->iGeomMField)
                        {
                            bOKToIgnore = false;
                            break;
                        }
                    }
                    if (bOKToIgnore)
                    {
                        const OGRFieldDefn *poSrcDefn =
                            GetSrcLayerDefn()->GetFieldDefn(iSrcField);
                        aosFieldsSrc.AddString(poSrcDefn->GetNameRef());
                    }
                }
            }
            else
            {
                iVRTField = GetLayerDefn()->GetGeomFieldIndex(pszFieldName);
                if (iVRTField >= 0 &&
                    apoGeomFieldProps[iVRTField]->eGeometryStyle == VGS_Direct)
                {
                    int iSrcField = apoGeomFieldProps[iVRTField]->iGeomField;
                    if (iSrcField >= 0)
                    {
                        const OGRGeomFieldDefn *poSrcDefn =
                            GetSrcLayerDefn()->GetGeomFieldDefn(iSrcField);
                        aosFieldsSrc.AddString(poSrcDefn->GetNameRef());
                    }
                }
            }
        }
    }

    // Add source fields that are not referenced by VRT layer.
    std::vector<bool> abSrcFieldUsed(GetSrcLayerDefn()->GetFieldCount());
    for (int iVRTField = 0; iVRTField < GetLayerDefn()->GetFieldCount();
         iVRTField++)
    {
        const int iSrcField = anSrcField[iVRTField];
        if (iSrcField >= 0)
            abSrcFieldUsed[iSrcField] = true;
    }
    for (int iVRTField = 0; iVRTField < GetLayerDefn()->GetGeomFieldCount();
         iVRTField++)
    {
        OGRVRTGeometryStyle eGeometryStyle =
            apoGeomFieldProps[iVRTField]->eGeometryStyle;
        // For a VGS_PointFromColumns geometry field, we must not ignore
        // the fields that help building it.
        if (eGeometryStyle == VGS_PointFromColumns)
        {
            int iSrcField = apoGeomFieldProps[iVRTField]->iGeomXField;
            if (iSrcField >= 0)
                abSrcFieldUsed[iSrcField] = true;
            iSrcField = apoGeomFieldProps[iVRTField]->iGeomYField;
            if (iSrcField >= 0)
                abSrcFieldUsed[iSrcField] = true;
            iSrcField = apoGeomFieldProps[iVRTField]->iGeomZField;
            if (iSrcField >= 0)
                abSrcFieldUsed[iSrcField] = true;
            iSrcField = apoGeomFieldProps[iVRTField]->iGeomMField;
            if (iSrcField >= 0)
                abSrcFieldUsed[iSrcField] = true;
        }
        // Similarly for other kinds of geometry fields.
        else if (eGeometryStyle == VGS_WKT || eGeometryStyle == VGS_WKB ||
                 eGeometryStyle == VGS_Shape)
        {
            int iSrcField = apoGeomFieldProps[iVRTField]->iGeomField;
            if (iSrcField >= 0)
                abSrcFieldUsed[iSrcField] = true;
        }
    }
    if (iStyleField >= 0)
        abSrcFieldUsed[iStyleField] = true;
    if (iFIDField >= 0)
        abSrcFieldUsed[iFIDField] = true;
    for (int iSrcField = 0; iSrcField < GetSrcLayerDefn()->GetFieldCount();
         iSrcField++)
    {
        if (!abSrcFieldUsed[iSrcField])
        {
            const OGRFieldDefn *poSrcDefn =
                GetSrcLayerDefn()->GetFieldDefn(iSrcField);
            aosFieldsSrc.AddString(poSrcDefn->GetNameRef());
        }
    }

    // Add source geometry fields that are not referenced by VRT layer.
    abSrcFieldUsed.clear();
    abSrcFieldUsed.resize(GetSrcLayerDefn()->GetGeomFieldCount());
    for (int iVRTField = 0; iVRTField < GetLayerDefn()->GetGeomFieldCount();
         iVRTField++)
    {
        if (apoGeomFieldProps[iVRTField]->eGeometryStyle == VGS_Direct)
        {
            const int iSrcField = apoGeomFieldProps[iVRTField]->iGeomField;
            if (iSrcField >= 0)
                abSrcFieldUsed[iSrcField] = true;
        }
    }
    for (int iSrcField = 0; iSrcField < GetSrcLayerDefn()->GetGeomFieldCount();
         iSrcField++)
    {
        if (!abSrcFieldUsed[iSrcField])
        {
            const OGRGeomFieldDefn *poSrcDefn =
                GetSrcLayerDefn()->GetGeomFieldDefn(iSrcField);
            aosFieldsSrc.AddString(poSrcDefn->GetNameRef());
        }
    }

    return poSrcLayer->SetIgnoredFields(aosFieldsSrc.List());
}

/************************************************************************/
/*                          GetSrcDataset()                             */
/************************************************************************/

GDALDataset *OGRVRTLayer::GetSrcDataset()
{
    if (!bHasFullInitialized)
        FullInitialize();
    if (!poSrcLayer || poDS->GetRecursionDetected())
        return nullptr;
    return poSrcDS.get();
}
