/******************************************************************************
 *
 * Project:  CPL - Common Portability Library
 * Purpose:  Implement VSI large file api for WebHDFS REST API
 * Author:   Even Rouault, even.rouault at spatialys.com
 *
 ******************************************************************************
 * Copyright (c) 2018, Even Rouault <even.rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "cpl_http.h"
#include "cpl_json.h"
#include "cpl_vsil_curl_priv.h"
#include "cpl_vsil_curl_class.h"

#include <errno.h>

#include <algorithm>
#include <set>
#include <map>
#include <memory>

#include "cpl_alibaba_oss.h"

#ifndef HAVE_CURL

void VSIInstallWebHdfsHandler(void)
{
    // Not supported
}

#else

//! @cond Doxygen_Suppress
#ifndef DOXYGEN_SKIP

#define ENABLE_DEBUG 0

#define unchecked_curl_easy_setopt(handle, opt, param)                         \
    CPL_IGNORE_RET_VAL(curl_easy_setopt(handle, opt, param))

namespace cpl
{

/************************************************************************/
/*                         VSIWebHDFSFSHandler                          */
/************************************************************************/

class VSIWebHDFSFSHandler final : public VSICurlFilesystemHandlerBaseWritable
{
    const std::string m_osPrefix;
    CPL_DISALLOW_COPY_ASSIGN(VSIWebHDFSFSHandler)

  protected:
    VSICurlHandle *CreateFileHandle(const char *pszFilename) override;

    int HasOptimizedReadMultiRange(const char * /* pszPath */) override
    {
        return false;
    }

    char **GetFileList(const char *pszFilename, int nMaxFiles,
                       bool *pbGotFileList) override;

    std::string
    GetURLFromFilename(const std::string &osFilename) const override;

    VSIVirtualHandleUniquePtr
    CreateWriteHandle(const char *pszFilename,
                      CSLConstList papszOptions) override;

  public:
    explicit VSIWebHDFSFSHandler(const char *pszPrefix) : m_osPrefix(pszPrefix)
    {
    }

    ~VSIWebHDFSFSHandler() override = default;

    int Unlink(const char *pszFilename) override;
    int Rmdir(const char *pszFilename) override;
    int Mkdir(const char *pszDirname, long nMode) override;

    const char *GetDebugKey() const override
    {
        return "VSIWEBHDFS";
    }

    std::string GetFSPrefix() const override
    {
        return m_osPrefix;
    }

    const char *GetOptions() override;

    std::string
    GetStreamingFilename(const std::string &osFilename) const override
    {
        return osFilename;
    }

    VSIFilesystemHandler *Duplicate(const char *pszPrefix) override
    {
        return new VSIWebHDFSFSHandler(pszPrefix);
    }
};

/************************************************************************/
/*                            VSIWebHDFSHandle                          */
/************************************************************************/

class VSIWebHDFSHandle final : public VSICurlHandle
{
    CPL_DISALLOW_COPY_ASSIGN(VSIWebHDFSHandle)

    std::string m_osDataNodeHost{};
    std::string m_osUsernameParam{};
    std::string m_osDelegationParam{};

    std::string DownloadRegion(vsi_l_offset startOffset, int nBlocks) override;

  public:
    VSIWebHDFSHandle(VSIWebHDFSFSHandler *poFS, const char *pszFilename,
                     const char *pszURL);
    ~VSIWebHDFSHandle() override = default;

    int ReadMultiRange(int nRanges, void **ppData,
                       const vsi_l_offset *panOffsets,
                       const size_t *panSizes) override
    {
        return VSIVirtualHandle::ReadMultiRange(nRanges, ppData, panOffsets,
                                                panSizes);
    }

    vsi_l_offset GetFileSize(bool bSetError) override;
};

/************************************************************************/
/*                           PatchWebHDFSUrl()                          */
/************************************************************************/

static std::string PatchWebHDFSUrl(const std::string &osURLIn,
                                   const std::string &osNewHost)
{
    std::string osURL(osURLIn);
    size_t nStart = 0;
    if (STARTS_WITH(osURL.c_str(), "http://"))
        nStart = strlen("http://");
    else if (STARTS_WITH(osURL.c_str(), "https://"))
        nStart = strlen("https://");
    if (nStart)
    {
        size_t nHostEnd = osURL.find(':', nStart);
        if (nHostEnd != std::string::npos)
        {
            osURL =
                osURL.substr(0, nStart) + osNewHost + osURL.substr(nHostEnd);
        }
    }
    return osURL;
}

/************************************************************************/
/*                       GetWebHDFSDataNodeHost()                       */
/************************************************************************/

static std::string GetWebHDFSDataNodeHost(const char *pszFilename)
{
    return std::string(
        VSIGetPathSpecificOption(pszFilename, "WEBHDFS_DATANODE_HOST", ""));
}

/************************************************************************/
/*                         VSIWebHDFSWriteHandle                        */
/************************************************************************/

class VSIWebHDFSWriteHandle final : public VSIAppendWriteHandle
{
    CPL_DISALLOW_COPY_ASSIGN(VSIWebHDFSWriteHandle)

    std::string m_osURL{};
    std::string m_osDataNodeHost{};
    std::string m_osUsernameParam{};
    std::string m_osDelegationParam{};
    CPLStringList m_aosHTTPOptions{};

    bool Send(bool bIsLastBlock) override;
    bool CreateFile();
    bool Append();

    void InvalidateParentDirectory();

  public:
    VSIWebHDFSWriteHandle(VSIWebHDFSFSHandler *poFS, const char *pszFilename);
    virtual ~VSIWebHDFSWriteHandle();
};

/************************************************************************/
/*                        GetWebHDFSBufferSize()                        */
/************************************************************************/

static int GetWebHDFSBufferSize()
{
    int nBufferSize;
    int nChunkSizeMB = atoi(CPLGetConfigOption("VSIWEBHDFS_SIZE", "4"));
    if (nChunkSizeMB <= 0 || nChunkSizeMB > 1000)
        nBufferSize = 4 * 1024 * 1024;
    else
        nBufferSize = nChunkSizeMB * 1024 * 1024;

    // For testing only !
    const char *pszChunkSizeBytes =
        CPLGetConfigOption("VSIWEBHDFS_SIZE_BYTES", nullptr);
    if (pszChunkSizeBytes)
        nBufferSize = atoi(pszChunkSizeBytes);
    if (nBufferSize <= 0 || nBufferSize > 1000 * 1024 * 1024)
        nBufferSize = 4 * 1024 * 1024;
    return nBufferSize;
}

/************************************************************************/
/*                      VSIWebHDFSWriteHandle()                         */
/************************************************************************/

VSIWebHDFSWriteHandle::VSIWebHDFSWriteHandle(VSIWebHDFSFSHandler *poFS,
                                             const char *pszFilename)
    : VSIAppendWriteHandle(poFS, poFS->GetFSPrefix().c_str(), pszFilename,
                           GetWebHDFSBufferSize()),
      m_osURL(pszFilename + poFS->GetFSPrefix().size()),
      m_osDataNodeHost(GetWebHDFSDataNodeHost(pszFilename)),
      m_aosHTTPOptions(CPLHTTPGetOptionsFromEnv(pszFilename))
{
    // cppcheck-suppress useInitializationList
    m_osUsernameParam =
        VSIGetPathSpecificOption(pszFilename, "WEBHDFS_USERNAME", "");
    if (!m_osUsernameParam.empty())
        m_osUsernameParam = "&user.name=" + m_osUsernameParam;
    m_osDelegationParam =
        VSIGetPathSpecificOption(pszFilename, "WEBHDFS_DELEGATION", "");
    if (!m_osDelegationParam.empty())
        m_osDelegationParam = "&delegation=" + m_osDelegationParam;

    if (m_pabyBuffer != nullptr && !CreateFile())
    {
        CPLFree(m_pabyBuffer);
        m_pabyBuffer = nullptr;
    }
}

/************************************************************************/
/*                     ~VSIWebHDFSWriteHandle()                         */
/************************************************************************/

VSIWebHDFSWriteHandle::~VSIWebHDFSWriteHandle()
{
    Close();
}

/************************************************************************/
/*                    InvalidateParentDirectory()                       */
/************************************************************************/

void VSIWebHDFSWriteHandle::InvalidateParentDirectory()
{
    m_poFS->InvalidateCachedData(m_osURL.c_str());

    std::string osFilenameWithoutSlash(m_osFilename);
    if (!osFilenameWithoutSlash.empty() && osFilenameWithoutSlash.back() == '/')
        osFilenameWithoutSlash.pop_back();
    m_poFS->InvalidateDirContent(
        CPLGetDirnameSafe(osFilenameWithoutSlash.c_str()));
}

/************************************************************************/
/*                             Send()                                   */
/************************************************************************/

bool VSIWebHDFSWriteHandle::Send(bool /* bIsLastBlock */)
{
    if (m_nCurOffset > 0)
        return Append();
    return true;
}

/************************************************************************/
/*                           CreateFile()                               */
/************************************************************************/

bool VSIWebHDFSWriteHandle::CreateFile()
{
    if (m_osUsernameParam.empty() && m_osDelegationParam.empty())
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Configuration option WEBHDFS_USERNAME or WEBHDFS_DELEGATION "
                 "should be defined");
        return false;
    }

    NetworkStatisticsFileSystem oContextFS(m_poFS->GetFSPrefix().c_str());
    NetworkStatisticsFile oContextFile(m_osFilename.c_str());
    NetworkStatisticsAction oContextAction("Write");

    std::string osURL = m_osURL + "?op=CREATE&overwrite=true" +
                        m_osUsernameParam + m_osDelegationParam;

    std::string osPermission = VSIGetPathSpecificOption(
        m_osFilename.c_str(), "WEBHDFS_PERMISSION", "");
    if (!osPermission.empty())
        osURL += "&permission=" + osPermission;

    std::string osReplication = VSIGetPathSpecificOption(
        m_osFilename.c_str(), "WEBHDFS_REPLICATION", "");
    if (!osReplication.empty())
        osURL += "&replication=" + osReplication;

    bool bInRedirect = false;

retry:
    CURL *hCurlHandle = curl_easy_init();

    struct curl_slist *headers = static_cast<struct curl_slist *>(
        CPLHTTPSetOptions(hCurlHandle, osURL.c_str(), m_aosHTTPOptions.List()));

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_CUSTOMREQUEST, "PUT");
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_INFILESIZE, 0);

    if (!m_osDataNodeHost.empty())
    {
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_FOLLOWLOCATION, 0);
    }

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

    WriteFuncStruct sWriteFuncData;
    VSICURLInitWriteFuncStruct(&sWriteFuncData, nullptr, nullptr, nullptr);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA, &sWriteFuncData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                               VSICurlHandleWriteFunc);

    VSICURLMultiPerform(m_poFS->GetCurlMultiHandleFor(m_osURL), hCurlHandle);

    curl_slist_free_all(headers);

    NetworkStatisticsLogger::LogPUT(0);

    long response_code = 0;
    curl_easy_getinfo(hCurlHandle, CURLINFO_HTTP_CODE, &response_code);

    if (!bInRedirect)
    {
        char *pszRedirectURL = nullptr;
        curl_easy_getinfo(hCurlHandle, CURLINFO_REDIRECT_URL, &pszRedirectURL);
        if (pszRedirectURL && strstr(pszRedirectURL, osURL.c_str()) == nullptr)
        {
            CPLDebug("WEBHDFS", "Redirect URL: %s", pszRedirectURL);

            bInRedirect = true;
            osURL = pszRedirectURL;
            if (!m_osDataNodeHost.empty())
            {
                osURL = PatchWebHDFSUrl(osURL, m_osDataNodeHost);
            }

            curl_easy_cleanup(hCurlHandle);
            CPLFree(sWriteFuncData.pBuffer);

            goto retry;
        }
    }

    curl_easy_cleanup(hCurlHandle);

    if (response_code == 201)
    {
        InvalidateParentDirectory();
    }
    else
    {
        CPLDebug("WEBHDFS", "%s",
                 sWriteFuncData.pBuffer ? sWriteFuncData.pBuffer : "(null)");
        CPLError(CE_Failure, CPLE_AppDefined, "PUT of %s failed",
                 m_osURL.c_str());
    }
    CPLFree(sWriteFuncData.pBuffer);

    return response_code == 201;
}

/************************************************************************/
/*                             Append()                                 */
/************************************************************************/

bool VSIWebHDFSWriteHandle::Append()
{
    NetworkStatisticsFileSystem oContextFS(m_poFS->GetFSPrefix().c_str());
    NetworkStatisticsFile oContextFile(m_osFilename.c_str());
    NetworkStatisticsAction oContextAction("Write");

    std::string osURL =
        m_osURL + "?op=APPEND" + m_osUsernameParam + m_osDelegationParam;

    CURL *hCurlHandle = curl_easy_init();

    struct curl_slist *headers = static_cast<struct curl_slist *>(
        CPLHTTPSetOptions(hCurlHandle, osURL.c_str(), m_aosHTTPOptions.List()));

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_CUSTOMREQUEST, "POST");
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_FOLLOWLOCATION, 0);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

    WriteFuncStruct sWriteFuncData;
    VSICURLInitWriteFuncStruct(&sWriteFuncData, nullptr, nullptr, nullptr);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA, &sWriteFuncData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                               VSICurlHandleWriteFunc);

    VSICURLMultiPerform(m_poFS->GetCurlMultiHandleFor(m_osURL), hCurlHandle);

    curl_slist_free_all(headers);

    NetworkStatisticsLogger::LogPOST(0, 0);

    long response_code = 0;
    curl_easy_getinfo(hCurlHandle, CURLINFO_HTTP_CODE, &response_code);

    if (response_code != 307)
    {
        CPLDebug("WEBHDFS", "%s",
                 sWriteFuncData.pBuffer ? sWriteFuncData.pBuffer : "(null)");
        CPLError(CE_Failure, CPLE_AppDefined, "POST of %s failed",
                 m_osURL.c_str());
        curl_easy_cleanup(hCurlHandle);
        CPLFree(sWriteFuncData.pBuffer);
        return false;
    }

    char *pszRedirectURL = nullptr;
    curl_easy_getinfo(hCurlHandle, CURLINFO_REDIRECT_URL, &pszRedirectURL);
    if (pszRedirectURL == nullptr)
    {
        curl_easy_cleanup(hCurlHandle);
        CPLFree(sWriteFuncData.pBuffer);
        return false;
    }
    CPLDebug("WEBHDFS", "Redirect URL: %s", pszRedirectURL);

    osURL = pszRedirectURL;
    if (!m_osDataNodeHost.empty())
    {
        osURL = PatchWebHDFSUrl(osURL, m_osDataNodeHost);
    }

    curl_easy_cleanup(hCurlHandle);
    CPLFree(sWriteFuncData.pBuffer);

    // After redirection

    hCurlHandle = curl_easy_init();

    headers = static_cast<struct curl_slist *>(
        CPLHTTPSetOptions(hCurlHandle, osURL.c_str(), m_aosHTTPOptions.List()));
    headers =
        curl_slist_append(headers, "Content-Type: application/octet-stream");

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_POSTFIELDS, m_pabyBuffer);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_POSTFIELDSIZE,
                               m_nBufferOff);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_FOLLOWLOCATION, 0);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

    VSICURLInitWriteFuncStruct(&sWriteFuncData, nullptr, nullptr, nullptr);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA, &sWriteFuncData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                               VSICurlHandleWriteFunc);

    VSICURLMultiPerform(m_poFS->GetCurlMultiHandleFor(m_osURL), hCurlHandle);

    curl_slist_free_all(headers);

    NetworkStatisticsLogger::LogPOST(m_nBufferOff, 0);

    response_code = 0;
    curl_easy_getinfo(hCurlHandle, CURLINFO_HTTP_CODE, &response_code);

    curl_easy_cleanup(hCurlHandle);

    if (response_code != 200)
    {
        CPLDebug("WEBHDFS", "%s",
                 sWriteFuncData.pBuffer ? sWriteFuncData.pBuffer : "(null)");
        CPLError(CE_Failure, CPLE_AppDefined, "POST of %s failed",
                 m_osURL.c_str());
    }
    CPLFree(sWriteFuncData.pBuffer);

    return response_code == 200;
}

/************************************************************************/
/*                          CreateWriteHandle()                         */
/************************************************************************/

VSIVirtualHandleUniquePtr
VSIWebHDFSFSHandler::CreateWriteHandle(const char *pszFilename,
                                       CSLConstList /*papszOptions*/)
{
    auto poHandle = std::make_unique<VSIWebHDFSWriteHandle>(this, pszFilename);
    if (!poHandle->IsOK())
    {
        return nullptr;
    }
    return VSIVirtualHandleUniquePtr(poHandle.release());
}

/************************************************************************/
/*                           GetOptions()                               */
/************************************************************************/

const char *VSIWebHDFSFSHandler::GetOptions()
{
    static std::string osOptions(
        std::string("<Options>") +
        "  <Option name='WEBHDFS_USERNAME' type='string' "
        "description='username (when security is off)'/>"
        "  <Option name='WEBHDFS_DELEGATION' type='string' "
        "description='Hadoop delegation token (when security is on)'/>"
        "  <Option name='WEBHDFS_DATANODE_HOST' type='string' "
        "description='For APIs using redirect, substitute the redirection "
        "hostname with the one provided by this option (normally resolvable "
        "hostname should be rewritten by a proxy)'/>"
        "  <Option name='WEBHDFS_REPLICATION' type='integer' "
        "description='Replication value used when creating a file'/>"
        "  <Option name='WEBHDFS_PERMISSION' type='integer' "
        "description='Permission mask (to provide as decimal number) when "
        "creating a file or directory'/>" +
        VSICurlFilesystemHandlerBase::GetOptionsStatic() + "</Options>");
    return osOptions.c_str();
}

/************************************************************************/
/*                          CreateFileHandle()                          */
/************************************************************************/

VSICurlHandle *VSIWebHDFSFSHandler::CreateFileHandle(const char *pszFilename)
{
    return new VSIWebHDFSHandle(this, pszFilename,
                                pszFilename + GetFSPrefix().size());
}

/************************************************************************/
/*                          GetURLFromFilename()                        */
/************************************************************************/

std::string
VSIWebHDFSFSHandler::GetURLFromFilename(const std::string &osFilename) const
{
    return osFilename.substr(GetFSPrefix().size());
}

/************************************************************************/
/*                           GetFileList()                              */
/************************************************************************/

char **VSIWebHDFSFSHandler::GetFileList(const char *pszDirname,
                                        int /*nMaxFiles*/, bool *pbGotFileList)
{
    if (ENABLE_DEBUG)
        CPLDebug("WEBHDFS", "GetFileList(%s)", pszDirname);
    *pbGotFileList = false;

    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("ListBucket");

    CPLAssert(strlen(pszDirname) >= GetFSPrefix().size());

    std::string osBaseURL = pszDirname + GetFSPrefix().size();
    if (!osBaseURL.empty() && osBaseURL.back() != '/')
        osBaseURL += '/';

    CURLM *hCurlMultiHandle = GetCurlMultiHandleFor(osBaseURL);

    std::string osUsernameParam =
        VSIGetPathSpecificOption(pszDirname, "WEBHDFS_USERNAME", "");
    if (!osUsernameParam.empty())
        osUsernameParam = "&user.name=" + osUsernameParam;
    std::string osDelegationParam =
        VSIGetPathSpecificOption(pszDirname, "WEBHDFS_DELEGATION", "");
    if (!osDelegationParam.empty())
        osDelegationParam = "&delegation=" + osDelegationParam;
    std::string osURL =
        osBaseURL + "?op=LISTSTATUS" + osUsernameParam + osDelegationParam;
    const CPLStringList aosHTTPOptions(CPLHTTPGetOptionsFromEnv(pszDirname));

    CURL *hCurlHandle = curl_easy_init();

    struct curl_slist *headers =
        VSICurlSetOptions(hCurlHandle, osURL.c_str(), aosHTTPOptions.List());

    WriteFuncStruct sWriteFuncData;
    VSICURLInitWriteFuncStruct(&sWriteFuncData, nullptr, nullptr, nullptr);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA, &sWriteFuncData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                               VSICurlHandleWriteFunc);

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

    VSICURLMultiPerform(hCurlMultiHandle, hCurlHandle);

    VSICURLResetHeaderAndWriterFunctions(hCurlHandle);

    curl_slist_free_all(headers);

    NetworkStatisticsLogger::LogGET(sWriteFuncData.nSize);

    long response_code = 0;
    curl_easy_getinfo(hCurlHandle, CURLINFO_HTTP_CODE, &response_code);

    CPLStringList aosList;
    bool bOK = false;
    if (response_code == 200 && sWriteFuncData.pBuffer)
    {
        CPLJSONDocument oDoc;
        if (oDoc.LoadMemory(
                reinterpret_cast<const GByte *>(sWriteFuncData.pBuffer)))
        {
            CPLJSONArray oFileStatus =
                oDoc.GetRoot().GetArray("FileStatuses/FileStatus");
            bOK = oFileStatus.IsValid();
            for (int i = 0; i < oFileStatus.Size(); i++)
            {
                CPLJSONObject oItem = oFileStatus[i];
                vsi_l_offset fileSize = oItem.GetLong("length");
                size_t mTime = static_cast<size_t>(
                    oItem.GetLong("modificationTime") / 1000);
                bool bIsDirectory = oItem.GetString("type") == "DIRECTORY";
                std::string osName = oItem.GetString("pathSuffix");
                // can be empty if we for example ask to list a file: in that
                // case the file entry is reported but with an empty pathSuffix
                if (!osName.empty())
                {
                    aosList.AddString(osName.c_str());

                    FileProp prop;
                    prop.eExists = EXIST_YES;
                    prop.bIsDirectory = bIsDirectory;
                    prop.bHasComputedFileSize = true;
                    prop.fileSize = fileSize;
                    prop.mTime = mTime;
                    std::string osCachedFilename(osBaseURL + osName);
#if DEBUG_VERBOSE
                    CPLDebug("WEBHDFS", "Cache %s", osCachedFilename.c_str());
#endif
                    SetCachedFileProp(osCachedFilename.c_str(), prop);
                }
            }
        }
    }

    *pbGotFileList = bOK;

    CPLFree(sWriteFuncData.pBuffer);
    curl_easy_cleanup(hCurlHandle);

    if (bOK)
        return aosList.StealList();
    else
        return nullptr;
}

/************************************************************************/
/*                               Unlink()                               */
/************************************************************************/

int VSIWebHDFSFSHandler::Unlink(const char *pszFilename)
{
    if (!STARTS_WITH_CI(pszFilename, GetFSPrefix().c_str()))
        return -1;

    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("Unlink");

    std::string osBaseURL = GetURLFromFilename(pszFilename);

    CURLM *hCurlMultiHandle = GetCurlMultiHandleFor(osBaseURL);

    std::string osUsernameParam =
        VSIGetPathSpecificOption(pszFilename, "WEBHDFS_USERNAME", "");
    if (!osUsernameParam.empty())
        osUsernameParam = "&user.name=" + osUsernameParam;
    std::string osDelegationParam =
        VSIGetPathSpecificOption(pszFilename, "WEBHDFS_DELEGATION", "");
    if (!osDelegationParam.empty())
        osDelegationParam = "&delegation=" + osDelegationParam;
    std::string osURL =
        osBaseURL + "?op=DELETE" + osUsernameParam + osDelegationParam;
    const CPLStringList aosHTTPOptions(CPLHTTPGetOptionsFromEnv(pszFilename));

    CURL *hCurlHandle = curl_easy_init();

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_CUSTOMREQUEST, "DELETE");

    struct curl_slist *headers =
        VSICurlSetOptions(hCurlHandle, osURL.c_str(), aosHTTPOptions.List());

    WriteFuncStruct sWriteFuncData;
    VSICURLInitWriteFuncStruct(&sWriteFuncData, nullptr, nullptr, nullptr);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA, &sWriteFuncData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                               VSICurlHandleWriteFunc);

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

    VSICURLMultiPerform(hCurlMultiHandle, hCurlHandle);

    VSICURLResetHeaderAndWriterFunctions(hCurlHandle);

    curl_slist_free_all(headers);

    NetworkStatisticsLogger::LogDELETE();

    long response_code = 0;
    curl_easy_getinfo(hCurlHandle, CURLINFO_HTTP_CODE, &response_code);

    CPLStringList aosList;
    bool bOK = false;
    if (response_code == 200 && sWriteFuncData.pBuffer)
    {
        CPLJSONDocument oDoc;
        if (oDoc.LoadMemory(
                reinterpret_cast<const GByte *>(sWriteFuncData.pBuffer)))
        {
            bOK = oDoc.GetRoot().GetBool("boolean");
        }
    }
    if (bOK)
    {
        InvalidateCachedData(osBaseURL.c_str());

        std::string osFilenameWithoutSlash(pszFilename);
        if (!osFilenameWithoutSlash.empty() &&
            osFilenameWithoutSlash.back() == '/')
            osFilenameWithoutSlash.pop_back();

        InvalidateDirContent(CPLGetDirnameSafe(osFilenameWithoutSlash.c_str()));
    }
    else
    {
        CPLDebug("WEBHDFS", "%s",
                 sWriteFuncData.pBuffer ? sWriteFuncData.pBuffer : "(null)");
    }

    CPLFree(sWriteFuncData.pBuffer);
    curl_easy_cleanup(hCurlHandle);

    return bOK ? 0 : -1;
}

/************************************************************************/
/*                               Rmdir()                                */
/************************************************************************/

int VSIWebHDFSFSHandler::Rmdir(const char *pszFilename)
{
    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("Rmdir");

    return Unlink(pszFilename);
}

/************************************************************************/
/*                               Mkdir()                                */
/************************************************************************/

int VSIWebHDFSFSHandler::Mkdir(const char *pszDirname, long nMode)
{
    if (!STARTS_WITH_CI(pszDirname, GetFSPrefix().c_str()))
        return -1;

    std::string osDirnameWithoutEndSlash(pszDirname);
    if (!osDirnameWithoutEndSlash.empty() &&
        osDirnameWithoutEndSlash.back() == '/')
    {
        osDirnameWithoutEndSlash.pop_back();
    }

    if (osDirnameWithoutEndSlash.find("/webhdfs/v1") ==
            osDirnameWithoutEndSlash.size() - strlen("/webhdfs/v1") &&
        std::count(osDirnameWithoutEndSlash.begin(),
                   osDirnameWithoutEndSlash.end(), '/') == 6)
    {
        // The server does weird things (creating a webhdfs/v1 subfolder)
        // if we provide the root directory like
        // /vsiwebhdfs/http://localhost:50070/webhdfs/v1
        return -1;
    }

    NetworkStatisticsFileSystem oContextFS(GetFSPrefix().c_str());
    NetworkStatisticsAction oContextAction("Mkdir");

    std::string osBaseURL =
        GetURLFromFilename(osDirnameWithoutEndSlash.c_str());

    CURLM *hCurlMultiHandle = GetCurlMultiHandleFor(osBaseURL);

    std::string osUsernameParam =
        VSIGetPathSpecificOption(pszDirname, "WEBHDFS_USERNAME", "");
    if (!osUsernameParam.empty())
        osUsernameParam = "&user.name=" + osUsernameParam;
    std::string osDelegationParam =
        VSIGetPathSpecificOption(pszDirname, "WEBHDFS_DELEGATION", "");
    if (!osDelegationParam.empty())
        osDelegationParam = "&delegation=" + osDelegationParam;
    std::string osURL =
        osBaseURL + "?op=MKDIRS" + osUsernameParam + osDelegationParam;
    if (nMode)
    {
        osURL += "&permission=";
        osURL += CPLSPrintf("%o", static_cast<int>(nMode));
    }
    const CPLStringList aosHTTPOptions(CPLHTTPGetOptionsFromEnv(pszDirname));

    CURL *hCurlHandle = curl_easy_init();

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_CUSTOMREQUEST, "PUT");

    struct curl_slist *headers =
        VSICurlSetOptions(hCurlHandle, osURL.c_str(), aosHTTPOptions.List());

    WriteFuncStruct sWriteFuncData;
    VSICURLInitWriteFuncStruct(&sWriteFuncData, nullptr, nullptr, nullptr);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA, &sWriteFuncData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                               VSICurlHandleWriteFunc);

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

    VSICURLMultiPerform(hCurlMultiHandle, hCurlHandle);

    VSICURLResetHeaderAndWriterFunctions(hCurlHandle);

    curl_slist_free_all(headers);

    NetworkStatisticsLogger::LogPUT(0);

    long response_code = 0;
    curl_easy_getinfo(hCurlHandle, CURLINFO_HTTP_CODE, &response_code);

    CPLStringList aosList;
    bool bOK = false;
    if (response_code == 200 && sWriteFuncData.pBuffer)
    {
        CPLJSONDocument oDoc;
        if (oDoc.LoadMemory(
                reinterpret_cast<const GByte *>(sWriteFuncData.pBuffer)))
        {
            bOK = oDoc.GetRoot().GetBool("boolean");
        }
    }
    if (bOK)
    {
        InvalidateDirContent(
            CPLGetDirnameSafe(osDirnameWithoutEndSlash.c_str()));

        FileProp cachedFileProp;
        cachedFileProp.eExists = EXIST_YES;
        cachedFileProp.bIsDirectory = true;
        cachedFileProp.bHasComputedFileSize = true;
        SetCachedFileProp(
            GetURLFromFilename(osDirnameWithoutEndSlash.c_str()).c_str(),
            cachedFileProp);

        RegisterEmptyDir(osDirnameWithoutEndSlash);
    }
    else
    {
        CPLDebug("WEBHDFS", "%s",
                 sWriteFuncData.pBuffer ? sWriteFuncData.pBuffer : "(null)");
    }

    CPLFree(sWriteFuncData.pBuffer);
    curl_easy_cleanup(hCurlHandle);

    return bOK ? 0 : -1;
}

/************************************************************************/
/*                            VSIWebHDFSHandle()                        */
/************************************************************************/

VSIWebHDFSHandle::VSIWebHDFSHandle(VSIWebHDFSFSHandler *poFSIn,
                                   const char *pszFilename, const char *pszURL)
    : VSICurlHandle(poFSIn, pszFilename, pszURL),
      m_osDataNodeHost(GetWebHDFSDataNodeHost(pszFilename))
{
    // cppcheck-suppress useInitializationList
    m_osUsernameParam =
        VSIGetPathSpecificOption(pszFilename, "WEBHDFS_USERNAME", "");
    if (!m_osUsernameParam.empty())
        m_osUsernameParam = "&user.name=" + m_osUsernameParam;
    m_osDelegationParam =
        VSIGetPathSpecificOption(pszFilename, "WEBHDFS_DELEGATION", "");
    if (!m_osDelegationParam.empty())
        m_osDelegationParam = "&delegation=" + m_osDelegationParam;
}

/************************************************************************/
/*                           GetFileSize()                              */
/************************************************************************/

vsi_l_offset VSIWebHDFSHandle::GetFileSize(bool bSetError)
{
    if (oFileProp.bHasComputedFileSize)
        return oFileProp.fileSize;

    NetworkStatisticsFileSystem oContextFS(poFS->GetFSPrefix().c_str());
    NetworkStatisticsFile oContextFile(m_osFilename.c_str());
    NetworkStatisticsAction oContextAction("GetFileSize");

    oFileProp.bHasComputedFileSize = true;

    CURLM *hCurlMultiHandle = poFS->GetCurlMultiHandleFor(m_pszURL);

    std::string osURL(m_pszURL);

    if (osURL.size() > strlen("/webhdfs/v1") &&
        osURL.find("/webhdfs/v1") == osURL.size() - strlen("/webhdfs/v1") &&
        std::count(osURL.begin(), osURL.end(), '/') == 4)
    {
        // If this is the root directory, add a trailing slash
        osURL += "/";
    }

    osURL += "?op=GETFILESTATUS" + m_osUsernameParam + m_osDelegationParam;

    CURL *hCurlHandle = curl_easy_init();

    struct curl_slist *headers =
        VSICurlSetOptions(hCurlHandle, osURL.c_str(), m_aosHTTPOptions.List());

    WriteFuncStruct sWriteFuncData;
    VSICURLInitWriteFuncStruct(&sWriteFuncData, nullptr, nullptr, nullptr);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA, &sWriteFuncData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                               VSICurlHandleWriteFunc);

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

    char szCurlErrBuf[CURL_ERROR_SIZE + 1] = {};
    szCurlErrBuf[0] = '\0';
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_ERRORBUFFER, szCurlErrBuf);

    VSICURLMultiPerform(hCurlMultiHandle, hCurlHandle);

    VSICURLResetHeaderAndWriterFunctions(hCurlHandle);

    curl_slist_free_all(headers);

    NetworkStatisticsLogger::LogGET(sWriteFuncData.nSize);

    long response_code = 0;
    curl_easy_getinfo(hCurlHandle, CURLINFO_HTTP_CODE, &response_code);

    oFileProp.eExists = EXIST_NO;
    if (response_code == 200 && sWriteFuncData.pBuffer)
    {
        CPLJSONDocument oDoc;
        if (oDoc.LoadMemory(
                reinterpret_cast<const GByte *>(sWriteFuncData.pBuffer)))
        {
            CPLJSONObject oFileStatus = oDoc.GetRoot().GetObj("FileStatus");
            oFileProp.fileSize = oFileStatus.GetLong("length");
            oFileProp.mTime = static_cast<size_t>(
                oFileStatus.GetLong("modificationTime") / 1000);
            oFileProp.bIsDirectory =
                oFileStatus.GetString("type") == "DIRECTORY";
            oFileProp.eExists = EXIST_YES;
        }
    }

    // If there was no VSI error thrown in the process,
    // fail by reporting the HTTP response code.
    if (response_code != 200 && bSetError && VSIGetLastErrorNo() == 0)
    {
        if (strlen(szCurlErrBuf) > 0)
        {
            if (response_code == 0)
            {
                VSIError(VSIE_HttpError, "CURL error: %s", szCurlErrBuf);
            }
            else
            {
                VSIError(VSIE_HttpError, "HTTP response code: %d - %s",
                         static_cast<int>(response_code), szCurlErrBuf);
            }
        }
        else
        {
            VSIError(VSIE_HttpError, "HTTP response code: %d",
                     static_cast<int>(response_code));
        }
    }

    if (ENABLE_DEBUG)
        CPLDebug(
            "WEBHDFS", "GetFileSize(%s)=" CPL_FRMT_GUIB "  response_code=%d",
            osURL.c_str(), oFileProp.fileSize, static_cast<int>(response_code));

    CPLFree(sWriteFuncData.pBuffer);
    curl_easy_cleanup(hCurlHandle);

    oFileProp.bHasComputedFileSize = true;
    poFS->SetCachedFileProp(m_pszURL, oFileProp);

    return oFileProp.fileSize;
}

/************************************************************************/
/*                          DownloadRegion()                            */
/************************************************************************/

std::string VSIWebHDFSHandle::DownloadRegion(const vsi_l_offset startOffset,
                                             const int nBlocks)
{
    if (bInterrupted && bStopOnInterruptUntilUninstall)
        return std::string();

    poFS->GetCachedFileProp(m_pszURL, oFileProp);
    if (oFileProp.eExists == EXIST_NO)
        return std::string();

    NetworkStatisticsFileSystem oContextFS(poFS->GetFSPrefix().c_str());
    NetworkStatisticsFile oContextFile(m_osFilename.c_str());
    NetworkStatisticsAction oContextAction("Read");

    CURLM *hCurlMultiHandle = poFS->GetCurlMultiHandleFor(m_pszURL);

    std::string osURL(m_pszURL);

    WriteFuncStruct sWriteFuncData;
    CPLHTTPRetryContext oRetryContext(m_oRetryParameters);
    bool bInRedirect = false;
    const vsi_l_offset nEndOffset =
        startOffset +
        static_cast<vsi_l_offset>(nBlocks) * VSICURLGetDownloadChunkSize() - 1;

retry:
    CURL *hCurlHandle = curl_easy_init();

    VSICURLInitWriteFuncStruct(&sWriteFuncData, this, pfnReadCbk,
                               pReadCbkUserData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEDATA, &sWriteFuncData);
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_WRITEFUNCTION,
                               VSICurlHandleWriteFunc);

    if (!bInRedirect)
    {
        osURL += "?op=OPEN&offset=";
        osURL += CPLSPrintf(CPL_FRMT_GUIB, startOffset);
        osURL += "&length=";
        osURL += CPLSPrintf(CPL_FRMT_GUIB, nEndOffset - startOffset + 1);
        osURL += m_osUsernameParam + m_osDelegationParam;
    }

    struct curl_slist *headers =
        VSICurlSetOptions(hCurlHandle, osURL.c_str(), m_aosHTTPOptions.List());

    if (!m_osDataNodeHost.empty())
    {
        unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_FOLLOWLOCATION, 0);
    }

    if (ENABLE_DEBUG)
        CPLDebug("WEBHDFS", "Downloading %s...", osURL.c_str());

    char szCurlErrBuf[CURL_ERROR_SIZE + 1] = {};
    szCurlErrBuf[0] = '\0';
    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_ERRORBUFFER, szCurlErrBuf);

    unchecked_curl_easy_setopt(hCurlHandle, CURLOPT_HTTPHEADER, headers);

    VSICURLMultiPerform(hCurlMultiHandle, hCurlHandle);

    VSICURLResetHeaderAndWriterFunctions(hCurlHandle);

    curl_slist_free_all(headers);

    NetworkStatisticsLogger::LogGET(sWriteFuncData.nSize);

    if (sWriteFuncData.bInterrupted)
    {
        bInterrupted = true;

        CPLFree(sWriteFuncData.pBuffer);
        curl_easy_cleanup(hCurlHandle);

        return std::string();
    }

    long response_code = 0;
    curl_easy_getinfo(hCurlHandle, CURLINFO_HTTP_CODE, &response_code);

    if (ENABLE_DEBUG)
        CPLDebug("WEBHDFS", "Got response_code=%ld", response_code);

    if (!bInRedirect)
    {
        char *pszRedirectURL = nullptr;
        curl_easy_getinfo(hCurlHandle, CURLINFO_REDIRECT_URL, &pszRedirectURL);
        if (pszRedirectURL && strstr(pszRedirectURL, m_pszURL) == nullptr)
        {
            CPLDebug("WEBHDFS", "Redirect URL: %s", pszRedirectURL);

            bInRedirect = true;
            osURL = pszRedirectURL;
            if (!m_osDataNodeHost.empty())
            {
                osURL = PatchWebHDFSUrl(osURL, m_osDataNodeHost);
            }

            CPLFree(sWriteFuncData.pBuffer);
            curl_easy_cleanup(hCurlHandle);

            goto retry;
        }
    }

    if (response_code != 200)
    {
        if (oRetryContext.CanRetry(static_cast<int>(response_code), nullptr,
                                   szCurlErrBuf))
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "HTTP error code: %d - %s. "
                     "Retrying again in %.1f secs",
                     static_cast<int>(response_code), m_pszURL,
                     oRetryContext.GetCurrentDelay());
            CPLSleep(oRetryContext.GetCurrentDelay());
            CPLFree(sWriteFuncData.pBuffer);
            curl_easy_cleanup(hCurlHandle);
            goto retry;
        }

        if (response_code >= 400 && szCurlErrBuf[0] != '\0')
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%d: %s",
                     static_cast<int>(response_code), szCurlErrBuf);
        }
        if (!oFileProp.bHasComputedFileSize && startOffset == 0)
        {
            oFileProp.bHasComputedFileSize = true;
            oFileProp.fileSize = 0;
            oFileProp.eExists = EXIST_NO;
            poFS->SetCachedFileProp(m_pszURL, oFileProp);
        }
        CPLFree(sWriteFuncData.pBuffer);
        curl_easy_cleanup(hCurlHandle);
        return std::string();
    }

    oFileProp.eExists = EXIST_YES;
    poFS->SetCachedFileProp(m_pszURL, oFileProp);

    DownloadRegionPostProcess(startOffset, nBlocks, sWriteFuncData.pBuffer,
                              sWriteFuncData.nSize);

    std::string osRet;
    osRet.assign(sWriteFuncData.pBuffer, sWriteFuncData.nSize);

    CPLFree(sWriteFuncData.pBuffer);
    curl_easy_cleanup(hCurlHandle);

    return osRet;
}

} /* end of namespace cpl */

#endif  // DOXYGEN_SKIP
//! @endcond

/************************************************************************/
/*                      VSIInstallWebHdfsHandler()                      */
/************************************************************************/

/*!
 \brief Install /vsiwebhdfs/ WebHDFS (Hadoop File System) REST API file
 system handler (requires libcurl)

 \verbatim embed:rst
 See :ref:`/vsiwebhdfs/ documentation <vsiwebhdfs>`
 \endverbatim

 @since GDAL 2.4
 */
void VSIInstallWebHdfsHandler(void)
{
    VSIFileManager::InstallHandler(
        "/vsiwebhdfs/", new cpl::VSIWebHDFSFSHandler("/vsiwebhdfs/"));
}

#endif /* HAVE_CURL */
