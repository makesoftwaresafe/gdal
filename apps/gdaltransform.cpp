/******************************************************************************
 *
 * Project:  GDAL
 * Purpose:  Command line point transformer.
 * Author:   Frank Warmerdam <warmerdam@pobox.com>
 *
 ******************************************************************************
 * Copyright (c) 2007, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"

#include <cstdio>
#include <cstdlib>
#include <cctype>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "gdal_version.h"
#include "gdal_alg.h"
#include "gdalwarper.h"
#include "gdal.h"
#include "ogr_api.h"
#include "ogr_core.h"
#include "ogr_spatialref.h"
#include "ogr_srs_api.h"
#include "commonutils.h"

/************************************************************************/
/*                               Usage()                                */
/************************************************************************/

static void Usage(bool bIsError, const char *pszErrorMsg = nullptr,
                  bool bHelpDoc = false)

{
    fprintf(bIsError ? stderr : stdout,
            "Usage: gdaltransform [--help] [--help-general]\n"
            "       [-i] [-s_srs <srs_def>] [-t_srs <srs_def>] "
            "[-to >NAME>=<VALUE>]...\n"
            "       [-s_coord_epoch <epoch>] [-t_coord_epoch <epoch>]\n"
            "       [-ct <proj_string>] [-order <n>] [-tps] [-rpc] [-geoloc] \n"
            "       [-gcp <pixel> <line> <easting> <northing> [elevation]]...\n"
            "       [-output_xy] [-E] [-field_sep <sep>] "
            "[-ignore_extra_input]\n"
            "       [<srcfile> [<dstfile>]]\n");

    if (bHelpDoc)
        exit(0);

    fprintf(bIsError ? stderr : stdout, "\n");
    if (pszErrorMsg != nullptr)
        fprintf(stderr, "\nFAILURE: %s\n", pszErrorMsg);

    exit(bIsError ? 1 : 0);
}

/************************************************************************/
/*                             IsValidSRS                               */
/************************************************************************/

static bool IsValidSRS(const char *pszUserInput)

{
    OGRSpatialReferenceH hSRS;
    bool bRes = true;

    CPLErrorReset();

    hSRS = OSRNewSpatialReference(nullptr);
    if (OSRSetFromUserInput(hSRS, pszUserInput) != OGRERR_NONE)
    {
        bRes = false;
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Translating source or target SRS failed:\n%s", pszUserInput);
    }

    OSRDestroySpatialReference(hSRS);

    return bRes;
}

/************************************************************************/
/*                                main()                                */
/************************************************************************/

#define CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(nExtraArg)                            \
    do                                                                         \
    {                                                                          \
        if (i + nExtraArg >= argc)                                             \
            Usage(true, CPLSPrintf("%s option requires %d argument(s)",        \
                                   argv[i], nExtraArg));                       \
    } while (false)

MAIN_START(argc, argv)

{
    // Check that we are running against at least GDAL 1.5.
    // Note to developers: if we use newer API, please change the requirement.
    if (atoi(GDALVersionInfo("VERSION_NUM")) < 1500)
    {
        fprintf(stderr,
                "At least, GDAL >= 1.5.0 is required for this version of %s, "
                "which was compiled against GDAL %s\n",
                argv[0], GDAL_RELEASE_NAME);
        exit(1);
    }

    GDALAllRegister();
    argc = GDALGeneralCmdLineProcessor(argc, &argv, 0);
    if (argc < 1)
        exit(-argc);

    const char *pszSrcFilename = nullptr;
    const char *pszDstFilename = nullptr;
    int nOrder = 0;
    void *hTransformArg;
    GDALTransformerFunc pfnTransformer = nullptr;
    int nGCPCount = 0;
    GDAL_GCP *pasGCPs = nullptr;
    int bInverse = FALSE;
    CPLStringList aosTO;
    int bOutputXY = FALSE;
    double dfX = 0.0;
    double dfY = 0.0;
    double dfZ = 0.0;
    double dfT = 0.0;
    bool bCoordOnCommandLine = false;
    bool bIgnoreExtraInput = false;
    bool bEchoInput = false;
    std::string osFieldSep = " ";

    /* -------------------------------------------------------------------- */
    /*      Parse arguments.                                                */
    /* -------------------------------------------------------------------- */
    for (int i = 1; i < argc && argv[i] != nullptr; i++)
    {
        if (EQUAL(argv[i], "--utility_version"))
        {
            printf("%s was compiled against GDAL %s and "
                   "is running against GDAL %s\n",
                   argv[0], GDAL_RELEASE_NAME, GDALVersionInfo("RELEASE_NAME"));
            CSLDestroy(argv);
            return 0;
        }
        else if (EQUAL(argv[i], "--help"))
        {
            Usage(false);
        }
        else if (EQUAL(argv[i], "--help-doc"))
        {
            Usage(false, nullptr, true);
        }
        else if (EQUAL(argv[i], "-t_srs"))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            const char *pszSRS = argv[++i];
            if (!IsValidSRS(pszSRS))
                exit(1);
            aosTO.SetNameValue("DST_SRS", pszSRS);
        }
        else if (EQUAL(argv[i], "-s_srs"))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            const char *pszSRS = argv[++i];
            // coverity[tainted_data]
            if (!IsValidSRS(pszSRS))
                exit(1);
            aosTO.SetNameValue("SRC_SRS", pszSRS);
        }
        else if (EQUAL(argv[i], "-s_coord_epoch"))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            const char *pszCoordinateEpoch = argv[++i];
            aosTO.SetNameValue("SRC_COORDINATE_EPOCH", pszCoordinateEpoch);
        }
        else if (EQUAL(argv[i], "-t_coord_epoch"))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            const char *pszCoordinateEpoch = argv[++i];
            aosTO.SetNameValue("DST_COORDINATE_EPOCH", pszCoordinateEpoch);
        }
        else if (EQUAL(argv[i], "-ct"))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            const char *pszCT = argv[++i];
            aosTO.SetNameValue("COORDINATE_OPERATION", pszCT);
        }
        else if (EQUAL(argv[i], "-order"))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            nOrder = atoi(argv[++i]);
            aosTO.SetNameValue("MAX_GCP_ORDER", argv[i]);
        }
        else if (EQUAL(argv[i], "-tps"))
        {
            aosTO.SetNameValue("METHOD", "GCP_TPS");
            nOrder = -1;
        }
        else if (EQUAL(argv[i], "-rpc"))
        {
            aosTO.SetNameValue("METHOD", "RPC");
        }
        else if (EQUAL(argv[i], "-geoloc"))
        {
            aosTO.SetNameValue("METHOD", "GEOLOC_ARRAY");
        }
        else if (EQUAL(argv[i], "-i"))
        {
            bInverse = TRUE;
        }
        else if (EQUAL(argv[i], "-to"))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            aosTO.AddString(argv[++i]);
        }
        else if (EQUAL(argv[i], "-gcp"))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(4);
            char *endptr = nullptr;
            /* -gcp pixel line easting northing [elev] */

            nGCPCount++;
            pasGCPs = static_cast<GDAL_GCP *>(
                CPLRealloc(pasGCPs, sizeof(GDAL_GCP) * nGCPCount));
            GDALInitGCPs(1, pasGCPs + nGCPCount - 1);

            // coverity[tainted_data]
            pasGCPs[nGCPCount - 1].dfGCPPixel = CPLAtof(argv[++i]);
            // coverity[tainted_data]
            pasGCPs[nGCPCount - 1].dfGCPLine = CPLAtof(argv[++i]);
            // coverity[tainted_data]
            pasGCPs[nGCPCount - 1].dfGCPX = CPLAtof(argv[++i]);
            // coverity[tainted_data]
            pasGCPs[nGCPCount - 1].dfGCPY = CPLAtof(argv[++i]);
            if (argv[i + 1] != nullptr &&
                (CPLStrtod(argv[i + 1], &endptr) != 0.0 ||
                 argv[i + 1][0] == '0'))
            {
                // Check that last argument is really a number and not a
                // filename looking like a number (see ticket #863).
                if (endptr && *endptr == 0)
                {
                    // coverity[tainted_data]
                    pasGCPs[nGCPCount - 1].dfGCPZ = CPLAtof(argv[++i]);
                }
            }

            /* should set id and info? */
        }
        else if (EQUAL(argv[i], "-output_xy"))
        {
            bOutputXY = TRUE;
        }
        else if (EQUAL(argv[i], "-ignore_extra_input"))
        {
            bIgnoreExtraInput = true;
        }
        else if (EQUAL(argv[i], "-E"))
        {
            bEchoInput = true;
        }
        else if (i < argc - 1 && EQUAL(argv[i], "-field_sep"))
        {
            osFieldSep = CPLString(argv[++i])
                             .replaceAll("\\t", '\t')
                             .replaceAll("\\r", '\r')
                             .replaceAll("\\n", '\n');
        }
        else if (EQUAL(argv[i], "-coord") && i + 2 < argc)
        {
            bCoordOnCommandLine = true;
            dfX = CPLAtof(argv[++i]);
            dfY = CPLAtof(argv[++i]);
            if (i + 1 < argc &&
                CPLGetValueType(argv[i + 1]) != CPL_VALUE_STRING)
                dfZ = CPLAtof(argv[++i]);
            if (i + 1 < argc &&
                CPLGetValueType(argv[i + 1]) != CPL_VALUE_STRING)
                dfT = CPLAtof(argv[++i]);
        }
        else if (argv[i][0] == '-')
        {
            Usage(true, CPLSPrintf("Unknown option name '%s'", argv[i]));
        }
        else if (pszSrcFilename == nullptr)
        {
            pszSrcFilename = argv[i];
        }
        else if (pszDstFilename == nullptr)
        {
            pszDstFilename = argv[i];
        }
        else
        {
            Usage(true, "Too many command options.");
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Open src and destination file, if appropriate.                  */
    /* -------------------------------------------------------------------- */
    GDALDatasetH hSrcDS = nullptr;
    if (pszSrcFilename != nullptr)
    {
        hSrcDS = GDALOpen(pszSrcFilename, GA_ReadOnly);
        if (hSrcDS == nullptr)
            exit(1);
    }

    GDALDatasetH hDstDS = nullptr;
    if (pszDstFilename != nullptr)
    {
        hDstDS = GDALOpen(pszDstFilename, GA_ReadOnly);
        if (hDstDS == nullptr)
            exit(1);
    }

    if (hSrcDS != nullptr && nGCPCount > 0)
    {
        fprintf(stderr, "Command line GCPs and input file specified, "
                        "specify one or the other.\n");
        exit(1);
    }

    /* -------------------------------------------------------------------- */
    /*      Create a transformation object from the source to               */
    /*      destination coordinate system.                                  */
    /* -------------------------------------------------------------------- */
    if (nGCPCount != 0 && nOrder == -1)
    {
        pfnTransformer = GDALTPSTransform;
        hTransformArg = GDALCreateTPSTransformer(nGCPCount, pasGCPs, FALSE);
    }
    else if (nGCPCount != 0)
    {
        pfnTransformer = GDALGCPTransform;
        hTransformArg =
            GDALCreateGCPTransformer(nGCPCount, pasGCPs, nOrder, FALSE);
    }
    else
    {
        pfnTransformer = GDALGenImgProjTransform;
        hTransformArg =
            GDALCreateGenImgProjTransformer2(hSrcDS, hDstDS, aosTO.List());
    }

    if (hTransformArg == nullptr)
    {
        exit(1);
    }

    /* -------------------------------------------------------------------- */
    /*      Read points from stdin, transform and write to stdout.          */
    /* -------------------------------------------------------------------- */
    double dfLastT = 0.0;

    if (!bCoordOnCommandLine)
    {
        // Is it an interactive terminal ?
        if (CPLIsInteractive(stdin))
        {
            if (pszSrcFilename != nullptr)
            {
                fprintf(stderr, "Enter column line values separated by space, "
                                "and press Return.\n");
            }
            else
            {
                fprintf(stderr, "Enter X Y [Z [T]] values separated by space, "
                                "and press Return.\n");
            }
        }
    }

    int nLine = 0;
    while (bCoordOnCommandLine || !feof(stdin))
    {
        std::string osExtraContent;
        if (!bCoordOnCommandLine)
        {
            char szLine[1024];

            if (fgets(szLine, sizeof(szLine) - 1, stdin) == nullptr)
                break;

            size_t nLen = strlen(szLine);
            if (nLen && szLine[nLen - 1] == '\n')
                szLine[--nLen] = 0;
            if (nLen && szLine[nLen - 1] == '\r')
                szLine[--nLen] = 0;
            const CPLStringList aosTokens(CSLTokenizeString(szLine));
            const int nCount = aosTokens.size();

            ++nLine;
            if (nCount < 2)
            {
                fprintf(stderr, "Not enough values at line %d\n", nLine);
                continue;
            }

            dfX = CPLAtof(aosTokens[0]);
            dfY = CPLAtof(aosTokens[1]);
            dfZ = 0.0;
            dfT = 0.0;
            int iStartExtraContent = nCount;
            if (nCount >= 3)
            {
                if (CPLGetValueType(aosTokens[2]) == CPL_VALUE_STRING)
                {
                    iStartExtraContent = 2;
                }
                else
                {
                    dfZ = CPLAtof(aosTokens[2]);

                    if (nCount >= 4)
                    {
                        if (CPLGetValueType(aosTokens[3]) == CPL_VALUE_STRING)
                        {
                            iStartExtraContent = 3;
                        }
                        else
                        {
                            dfT = CPLAtof(aosTokens[3]);
                            iStartExtraContent = 4;
                        }
                    }
                }
            }

            if (!bIgnoreExtraInput)
            {
                for (int i = iStartExtraContent; i < nCount; ++i)
                {
                    if (!osExtraContent.empty())
                        osExtraContent += ' ';
                    osExtraContent += aosTokens[i];
                }
                while (!osExtraContent.empty() &&
                       isspace(static_cast<int>(osExtraContent.back())))
                {
                    osExtraContent.pop_back();
                }
                if (!osExtraContent.empty())
                    osExtraContent = osFieldSep + osExtraContent;
            }
        }
        if (dfT != dfLastT && nGCPCount == 0)
        {
            if (dfT != 0.0)
            {
                aosTO.SetNameValue("COORDINATE_EPOCH", CPLSPrintf("%g", dfT));
            }
            else
            {
                aosTO.SetNameValue("COORDINATE_EPOCH", nullptr);
            }
            GDALDestroyGenImgProjTransformer(hTransformArg);
            hTransformArg =
                GDALCreateGenImgProjTransformer2(hSrcDS, hDstDS, aosTO.List());
        }

        int bSuccess = TRUE;
        const double dfXBefore = dfX;
        const double dfYBefore = dfY;
        const double dfZBefore = dfZ;
        if (pfnTransformer(hTransformArg, bInverse, 1, &dfX, &dfY, &dfZ,
                           &bSuccess) &&
            bSuccess)
        {
            if (bEchoInput)
            {
                if (bOutputXY)
                    CPLprintf("%.15g%s%.15g%s", dfXBefore, osFieldSep.c_str(),
                              dfYBefore, osFieldSep.c_str());
                else
                    CPLprintf("%.15g%s%.15g%s%.15g%s", dfXBefore,
                              osFieldSep.c_str(), dfYBefore, osFieldSep.c_str(),
                              dfZBefore, osFieldSep.c_str());
            }
            if (bOutputXY)
                CPLprintf("%.15g%s%.15g%s\n", dfX, osFieldSep.c_str(), dfY,
                          osExtraContent.c_str());
            else
                CPLprintf("%.15g%s%.15g%s%.15g%s\n", dfX, osFieldSep.c_str(),
                          dfY, osFieldSep.c_str(), dfZ, osExtraContent.c_str());
        }
        else
        {
            printf("transformation failed.\n");
        }

        if (bCoordOnCommandLine)
            break;
        dfLastT = dfT;
    }

    if (nGCPCount != 0 && nOrder == -1)
    {
        GDALDestroyTPSTransformer(hTransformArg);
    }
    else if (nGCPCount != 0)
    {
        GDALDestroyGCPTransformer(hTransformArg);
    }
    else
    {
        GDALDestroyGenImgProjTransformer(hTransformArg);
    }

    if (nGCPCount)
    {
        GDALDeinitGCPs(nGCPCount, pasGCPs);
        CPLFree(pasGCPs);
    }

    if (hSrcDS)
        GDALClose(hSrcDS);

    if (hDstDS)
        GDALClose(hDstDS);

    GDALDumpOpenDatasets(stderr);
    GDALDestroyDriverManager();

    CSLDestroy(argv);

    return 0;
}

MAIN_END
