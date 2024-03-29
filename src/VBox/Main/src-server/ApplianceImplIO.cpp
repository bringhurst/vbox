/* $Id$ */
/** @file
 * IO helper for IAppliance COM class implementations.
 */

/*
 * Copyright (C) 2010-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/******************************************************************************
 *   Header Files                                                             *
 ******************************************************************************/

#include "ProgressImpl.h"
#include "ApplianceImpl.h"
#include "ApplianceImplPrivate.h"
#include "VirtualBoxImpl.h"

#include <iprt/zip.h>
#include <iprt/tar.h>
#include <iprt/sha.h>
#include <iprt/path.h>
#include <iprt/asm.h>
#include <iprt/stream.h>
#include <iprt/circbuf.h>
#include <iprt/vfs.h>
#include <iprt/manifest.h>
#include <VBox/vd-ifs.h>
#include <VBox/vd.h>

/******************************************************************************
 *   Structures and Typedefs                                                  *
 ******************************************************************************/
typedef struct FILESTORAGEINTERNAL
{
    /** File handle. */
    RTFILE         file;
    /** Completion callback. */
    PFNVDCOMPLETED pfnCompleted;
} FILESTORAGEINTERNAL, *PFILESTORAGEINTERNAL;

typedef struct TARSTORAGEINTERNAL
{
    /** Tar handle. */
    RTTARFILE      file;
    /** Completion callback. */
    PFNVDCOMPLETED pfnCompleted;
} TARSTORAGEINTERNAL, *PTARSTORAGEINTERNAL;

typedef struct SHASTORAGEINTERNAL
{
    /** Completion callback. */
    PFNVDCOMPLETED pfnCompleted;
    /** Storage handle for the next callback in chain. */
    void *pvStorage;
    /** Current file open mode. */
    uint32_t fOpenMode;
    /** Our own storage handle. */
    PSHASTORAGE pShaStorage;
    /** Circular buffer used for transferring data from/to the worker thread. */
    PRTCIRCBUF pCircBuf;
    /** Current absolute position (regardless of the real read/written data). */
    uint64_t cbCurAll;
    /** Current real position in the file. */
    uint64_t cbCurFile;
    /** Handle of the worker thread. */
    RTTHREAD pWorkerThread;
    /** Status of the worker thread. */
    volatile uint32_t u32Status;
    /** Event for signaling a new status. */
    RTSEMEVENT newStatusEvent;
    /** Event for signaling a finished task of the worker thread. */
    RTSEMEVENT workFinishedEvent;
    /** SHA1/SHA256 calculation context. */
    union
    {
        RTSHA1CONTEXT    Sha1;
        RTSHA256CONTEXT  Sha256;
    } ctx;
    /** Write mode only: Memory buffer for writing zeros. */
    void *pvZeroBuf;
    /** Write mode only: Size of the zero memory buffer. */
    size_t cbZeroBuf;
    /** Read mode only: Indicate if we reached end of file. */
    volatile bool fEOF;
//    uint64_t calls;
//    uint64_t waits;
} SHASTORAGEINTERNAL, *PSHASTORAGEINTERNAL;

/******************************************************************************
 *   Defined Constants And Macros                                             *
 ******************************************************************************/

#define STATUS_WAIT    UINT32_C(0)
#define STATUS_WRITE   UINT32_C(1)
#define STATUS_WRITING UINT32_C(2)
#define STATUS_READ    UINT32_C(3)
#define STATUS_READING UINT32_C(4)
#define STATUS_END     UINT32_C(5)

/* Enable for getting some flow history. */
#if 0
# define DEBUG_PRINT_FLOW() RTPrintf("%s\n", __FUNCTION__)
#else
# define DEBUG_PRINT_FLOW() do {} while (0)
#endif

/******************************************************************************
 *   Internal Functions                                                       *
 ******************************************************************************/


/******************************************************************************
 *   Internal: RTFile interface
 ******************************************************************************/

static int fileOpenCallback(void * /* pvUser */, const char *pszLocation, uint32_t fOpen,
                              PFNVDCOMPLETED pfnCompleted, void **ppInt)
{
    /* Validate input. */
    AssertPtrReturn(ppInt, VERR_INVALID_POINTER);
    AssertPtrNullReturn(pfnCompleted, VERR_INVALID_PARAMETER);

    DEBUG_PRINT_FLOW();

    PFILESTORAGEINTERNAL pInt = (PFILESTORAGEINTERNAL)RTMemAllocZ(sizeof(FILESTORAGEINTERNAL));
    if (!pInt)
        return VERR_NO_MEMORY;

    pInt->pfnCompleted = pfnCompleted;

    int rc = RTFileOpen(&pInt->file, pszLocation, fOpen);

    if (RT_FAILURE(rc))
        RTMemFree(pInt);
    else
        *ppInt = pInt;

    return rc;
}

static int fileCloseCallback(void * /* pvUser */, void *pvStorage)
{
    /* Validate input. */
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    PFILESTORAGEINTERNAL pInt = (PFILESTORAGEINTERNAL)pvStorage;

    DEBUG_PRINT_FLOW();

    int rc = RTFileClose(pInt->file);

    /* Cleanup */
    RTMemFree(pInt);

    return rc;
}

static int fileDeleteCallback(void * /* pvUser */, const char *pcszFilename)
{
    DEBUG_PRINT_FLOW();

    return RTFileDelete(pcszFilename);
}

static int fileMoveCallback(void * /* pvUser */, const char *pcszSrc, const char *pcszDst, unsigned fMove)
{
    DEBUG_PRINT_FLOW();

    return RTFileMove(pcszSrc, pcszDst, fMove);
}

static int fileGetFreeSpaceCallback(void * /* pvUser */, const char *pcszFilename, int64_t *pcbFreeSpace)
{
    /* Validate input. */
    AssertPtrReturn(pcszFilename, VERR_INVALID_POINTER);
    AssertPtrReturn(pcbFreeSpace, VERR_INVALID_POINTER);

    DEBUG_PRINT_FLOW();

    return VERR_NOT_IMPLEMENTED;
}

static int fileGetModificationTimeCallback(void * /* pvUser */, const char *pcszFilename, PRTTIMESPEC pModificationTime)
{
    /* Validate input. */
    AssertPtrReturn(pcszFilename, VERR_INVALID_POINTER);
    AssertPtrReturn(pModificationTime, VERR_INVALID_POINTER);

    DEBUG_PRINT_FLOW();

    return VERR_NOT_IMPLEMENTED;
}

static int fileGetSizeCallback(void * /* pvUser */, void *pvStorage, uint64_t *pcbSize)
{
    /* Validate input. */
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    PFILESTORAGEINTERNAL pInt = (PFILESTORAGEINTERNAL)pvStorage;

    DEBUG_PRINT_FLOW();

    return RTFileGetSize(pInt->file, pcbSize);
}

static int fileSetSizeCallback(void * /* pvUser */, void *pvStorage, uint64_t cbSize)
{
    /* Validate input. */
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    PFILESTORAGEINTERNAL pInt = (PFILESTORAGEINTERNAL)pvStorage;

    DEBUG_PRINT_FLOW();

    return RTFileSetSize(pInt->file, cbSize);
}

static int fileWriteSyncCallback(void * /* pvUser */, void *pvStorage, uint64_t uOffset,
                                   const void *pvBuf, size_t cbWrite, size_t *pcbWritten)
{
    /* Validate input. */
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    PFILESTORAGEINTERNAL pInt = (PFILESTORAGEINTERNAL)pvStorage;

    return RTFileWriteAt(pInt->file, uOffset, pvBuf, cbWrite, pcbWritten);
}

static int fileReadSyncCallback(void * /* pvUser */, void *pvStorage, uint64_t uOffset,
                                  void *pvBuf, size_t cbRead, size_t *pcbRead)
{
    /* Validate input. */
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

//    DEBUG_PRINT_FLOW();

    PFILESTORAGEINTERNAL pInt = (PFILESTORAGEINTERNAL)pvStorage;

    return RTFileReadAt(pInt->file, uOffset, pvBuf, cbRead, pcbRead);
}

static int fileFlushSyncCallback(void * /* pvUser */, void *pvStorage)
{
    /* Validate input. */
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    DEBUG_PRINT_FLOW();

    PFILESTORAGEINTERNAL pInt = (PFILESTORAGEINTERNAL)pvStorage;

    return RTFileFlush(pInt->file);
}

/******************************************************************************
 *   Internal: RTTar interface
 ******************************************************************************/

static int tarOpenCallback(void *pvUser, const char *pszLocation, uint32_t fOpen,
                             PFNVDCOMPLETED pfnCompleted, void **ppInt)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(ppInt, VERR_INVALID_POINTER);
    AssertPtrNullReturn(pfnCompleted, VERR_INVALID_PARAMETER);
//    AssertReturn(!(fOpen & RTFILE_O_READWRITE), VERR_INVALID_PARAMETER);

    RTTAR tar = (RTTAR)pvUser;

    DEBUG_PRINT_FLOW();

    PTARSTORAGEINTERNAL pInt = (PTARSTORAGEINTERNAL)RTMemAllocZ(sizeof(TARSTORAGEINTERNAL));
    if (!pInt)
        return VERR_NO_MEMORY;

    pInt->pfnCompleted = pfnCompleted;

    int rc = VINF_SUCCESS;

    if (fOpen & RTFILE_O_READ
        && !(fOpen & RTFILE_O_WRITE))
    {
        /* Read only is a little bit more complicated than writing, cause we
         * need streaming functionality. First try to open the file on the
         * current file position. If this is the file the caller requested, we
         * are fine. If not seek to the next file in the stream and check
         * again. This is repeated until EOF of the OVA. */
        /*
         *
         *
         *  TODO: recheck this with more VDMKs (or what else) in an test OVA.
         *
         *
         */
        bool fFound = false;

        for (;;)
        {
            char *pszFilename = 0;
            rc = RTTarCurrentFile(tar, &pszFilename);
            if (RT_SUCCESS(rc))
            {
                if (rc == VINF_TAR_DIR_PATH)
                {
                    break;
                }

                fFound = !RTStrICmp(pszFilename, pszLocation);

                RTStrFree(pszFilename);
                if (fFound)
                    break;
                else
                {
                    rc = RTTarSeekNextFile(tar);
                    if (RT_FAILURE(rc))
                    {
                        break;
                    }
                }
            }
            else
                break;
        }
        if (fFound)
            rc = RTTarFileOpenCurrentFile(tar, &pInt->file, 0, fOpen);
    }
    else
        rc = RTTarFileOpen(tar, &pInt->file, RTPathFilename(pszLocation), fOpen);

    if (RT_FAILURE(rc))
        RTMemFree(pInt);
    else
        *ppInt = pInt;

    return rc;
}

static int tarCloseCallback(void *pvUser, void *pvStorage)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    PTARSTORAGEINTERNAL pInt = (PTARSTORAGEINTERNAL)pvStorage;

    DEBUG_PRINT_FLOW();

    int rc = RTTarFileClose(pInt->file);

    /* Cleanup */
    RTMemFree(pInt);

    return rc;
}

static int tarDeleteCallback(void *pvUser, const char *pcszFilename)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pcszFilename, VERR_INVALID_POINTER);

    DEBUG_PRINT_FLOW();

    return VERR_NOT_IMPLEMENTED;
}

static int tarMoveCallback(void *pvUser, const char *pcszSrc, const char *pcszDst, unsigned /* fMove */)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pcszSrc, VERR_INVALID_POINTER);
    AssertPtrReturn(pcszDst, VERR_INVALID_POINTER);

    DEBUG_PRINT_FLOW();

    return VERR_NOT_IMPLEMENTED;
}

static int tarGetFreeSpaceCallback(void *pvUser, const char *pcszFilename, int64_t *pcbFreeSpace)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pcszFilename, VERR_INVALID_POINTER);
    AssertPtrReturn(pcbFreeSpace, VERR_INVALID_POINTER);

    DEBUG_PRINT_FLOW();

    return VERR_NOT_IMPLEMENTED;
}

static int tarGetModificationTimeCallback(void *pvUser, const char *pcszFilename, PRTTIMESPEC pModificationTime)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pcszFilename, VERR_INVALID_POINTER);
    AssertPtrReturn(pModificationTime, VERR_INVALID_POINTER);

    DEBUG_PRINT_FLOW();

    return VERR_NOT_IMPLEMENTED;
}

static int tarGetSizeCallback(void *pvUser, void *pvStorage, uint64_t *pcbSize)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    PTARSTORAGEINTERNAL pInt = (PTARSTORAGEINTERNAL)pvStorage;

    DEBUG_PRINT_FLOW();

    return RTTarFileGetSize(pInt->file, pcbSize);
}

static int tarSetSizeCallback(void *pvUser, void *pvStorage, uint64_t cbSize)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    PTARSTORAGEINTERNAL pInt = (PTARSTORAGEINTERNAL)pvStorage;

    DEBUG_PRINT_FLOW();

    return RTTarFileSetSize(pInt->file, cbSize);
}

static int tarWriteSyncCallback(void *pvUser, void *pvStorage, uint64_t uOffset,
                                  const void *pvBuf, size_t cbWrite, size_t *pcbWritten)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    PTARSTORAGEINTERNAL pInt = (PTARSTORAGEINTERNAL)pvStorage;

    DEBUG_PRINT_FLOW();

    return RTTarFileWriteAt(pInt->file, uOffset, pvBuf, cbWrite, pcbWritten);
}

static int tarReadSyncCallback(void *pvUser, void *pvStorage, uint64_t uOffset,
                                 void *pvBuf, size_t cbRead, size_t *pcbRead)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    PTARSTORAGEINTERNAL pInt = (PTARSTORAGEINTERNAL)pvStorage;

//    DEBUG_PRINT_FLOW();

    return RTTarFileReadAt(pInt->file, uOffset, pvBuf, cbRead, pcbRead);
}

static int tarFlushSyncCallback(void *pvUser, void *pvStorage)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    DEBUG_PRINT_FLOW();

    return VERR_NOT_IMPLEMENTED;
}

/******************************************************************************
 *   Internal: RTSha interface
 ******************************************************************************/

DECLCALLBACK(int) shaCalcWorkerThread(RTTHREAD /* aThread */, void *pvUser)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);

    PSHASTORAGEINTERNAL pInt = (PSHASTORAGEINTERNAL)pvUser;

    PVDINTERFACEIO pIfIo = VDIfIoGet(pInt->pShaStorage->pVDImageIfaces);
    AssertPtrReturn(pIfIo, VERR_INVALID_PARAMETER);

    int rc = VINF_SUCCESS;
    bool fLoop = true;
    while (fLoop)
    {
        /* What should we do next? */
        uint32_t u32Status = ASMAtomicReadU32(&pInt->u32Status);
//        RTPrintf("status: %d\n", u32Status);
        switch (u32Status)
        {
            case STATUS_WAIT:
            {
                /* Wait for new work. */
                rc = RTSemEventWait(pInt->newStatusEvent, 100);
                if (   RT_FAILURE(rc)
                    && rc != VERR_TIMEOUT)
                    fLoop = false;
                break;
            }
            case STATUS_WRITE:
            {
                ASMAtomicCmpXchgU32(&pInt->u32Status, STATUS_WRITING, STATUS_WRITE);
                size_t cbAvail = RTCircBufUsed(pInt->pCircBuf);
                size_t cbMemAllRead = 0;
                /* First loop over all the free memory in the circular
                 * memory buffer (could be turn around at the end). */
                for (;;)
                {
                    if (   cbMemAllRead == cbAvail
                        || fLoop == false)
                        break;
                    char *pcBuf;
                    size_t cbMemToRead = cbAvail - cbMemAllRead;
                    size_t cbMemRead = 0;
                    /* Try to acquire all the used space of the circular buffer. */
                    RTCircBufAcquireReadBlock(pInt->pCircBuf, cbMemToRead, (void**)&pcBuf, &cbMemRead);
                    size_t cbAllWritten = 0;
                    /* Second, write as long as used memory is there. The write
                     * method could also split the writes up into to smaller
                     * parts. */
                    for (;;)
                    {
                        if (cbAllWritten == cbMemRead)
                            break;
                        size_t cbToWrite = cbMemRead - cbAllWritten;
                        size_t cbWritten = 0;
                        rc = vdIfIoFileWriteSync(pIfIo, pInt->pvStorage, pInt->cbCurFile, &pcBuf[cbAllWritten], cbToWrite, &cbWritten);
//                        RTPrintf ("%lu %lu %lu %Rrc\n", pInt->cbCurFile, cbToRead, cbRead, rc);
                        if (RT_FAILURE(rc))
                        {
                            fLoop = false;
                            break;
                        }
                        cbAllWritten += cbWritten;
                        pInt->cbCurFile += cbWritten;
                    }
                    /* Update the SHA1/SHA256 context with the next data block. */
                    if (   RT_SUCCESS(rc)
                        && pInt->pShaStorage->fCreateDigest)
                    {
                        if (pInt->pShaStorage->fSha256)
                            RTSha256Update(&pInt->ctx.Sha256, pcBuf, cbAllWritten);
                        else
                            RTSha1Update(&pInt->ctx.Sha1, pcBuf, cbAllWritten);
                    }
                    /* Mark the block as empty. */
                    RTCircBufReleaseReadBlock(pInt->pCircBuf, cbAllWritten);
                    cbMemAllRead += cbAllWritten;
                }
                /* Reset the thread status and signal the main thread that we
                 * are finished. Use CmpXchg, so we not overwrite other states
                 * which could be signaled in the meantime. */
                if (ASMAtomicCmpXchgU32(&pInt->u32Status, STATUS_WAIT, STATUS_WRITING))
                    rc = RTSemEventSignal(pInt->workFinishedEvent);
                break;
            }
            case STATUS_READ:
            {
                ASMAtomicCmpXchgU32(&pInt->u32Status, STATUS_READING, STATUS_READ);
                size_t cbAvail = RTCircBufFree(pInt->pCircBuf);
                size_t cbMemAllWrite = 0;
                /* First loop over all the available memory in the circular
                 * memory buffer (could be turn around at the end). */
                for (;;)
                {
                    if (   cbMemAllWrite == cbAvail
                        || fLoop == false)
                        break;
                    char *pcBuf;
                    size_t cbMemToWrite = cbAvail - cbMemAllWrite;
                    size_t cbMemWrite = 0;
                    /* Try to acquire all the free space of the circular buffer. */
                    RTCircBufAcquireWriteBlock(pInt->pCircBuf, cbMemToWrite, (void**)&pcBuf, &cbMemWrite);
                    /* Second, read as long as we filled all the memory. The
                     * read method could also split the reads up into to
                     * smaller parts. */
                    size_t cbAllRead = 0;
                    for (;;)
                    {
                        if (cbAllRead == cbMemWrite)
                            break;
                        size_t cbToRead = cbMemWrite - cbAllRead;
                        size_t cbRead = 0;
                        rc = vdIfIoFileReadSync(pIfIo, pInt->pvStorage, pInt->cbCurFile, &pcBuf[cbAllRead], cbToRead, &cbRead);
//                        RTPrintf ("%lu %lu %lu %Rrc\n", pInt->cbCurFile, cbToRead, cbRead, rc);
                        if (RT_FAILURE(rc))
                        {
                            fLoop = false;
                            break;
                        }
                        /* This indicates end of file. Stop reading. */
                        if (cbRead == 0)
                        {
                            fLoop = false;
                            ASMAtomicWriteBool(&pInt->fEOF, true);
                            break;
                        }
                        cbAllRead += cbRead;
                        pInt->cbCurFile += cbRead;
                    }
                    /* Update the SHA1/SHA256 context with the next data block. */
                    if (   RT_SUCCESS(rc)
                        && pInt->pShaStorage->fCreateDigest)
                    {
                        if (pInt->pShaStorage->fSha256)
                            RTSha256Update(&pInt->ctx.Sha256, pcBuf, cbAllRead);
                        else
                            RTSha1Update(&pInt->ctx.Sha1, pcBuf, cbAllRead);
                    }
                    /* Mark the block as full. */
                    RTCircBufReleaseWriteBlock(pInt->pCircBuf, cbAllRead);
                    cbMemAllWrite += cbAllRead;
                }
                /* Reset the thread status and signal the main thread that we
                 * are finished. Use CmpXchg, so we not overwrite other states
                 * which could be signaled in the meantime. */
                if (ASMAtomicCmpXchgU32(&pInt->u32Status, STATUS_WAIT, STATUS_READING))
                    rc = RTSemEventSignal(pInt->workFinishedEvent);
                break;
            }
            case STATUS_END:
            {
                /* End signaled */
                fLoop = false;
                break;
            }
        }
    }
    /* Cleanup any status changes to indicate we are finished. */
    ASMAtomicWriteU32(&pInt->u32Status, STATUS_END);
    rc = RTSemEventSignal(pInt->workFinishedEvent);
    return rc;
}

DECLINLINE(int) shaSignalManifestThread(PSHASTORAGEINTERNAL pInt, uint32_t uStatus)
{
    ASMAtomicWriteU32(&pInt->u32Status, uStatus);
    return RTSemEventSignal(pInt->newStatusEvent);
}

DECLINLINE(int) shaWaitForManifestThreadFinished(PSHASTORAGEINTERNAL pInt)
{
//    RTPrintf("start\n");
    int rc = VINF_SUCCESS;
    for (;;)
    {
//        RTPrintf(" wait\n");
        uint32_t u32Status = ASMAtomicReadU32(&pInt->u32Status);
        if (!(   u32Status == STATUS_WRITE
              || u32Status == STATUS_WRITING
              || u32Status == STATUS_READ
              || u32Status == STATUS_READING))
            break;
        rc = RTSemEventWait(pInt->workFinishedEvent, 100);
    }
    if (rc == VERR_TIMEOUT)
        rc = VINF_SUCCESS;
    return rc;
}

DECLINLINE(int) shaFlushCurBuf(PSHASTORAGEINTERNAL pInt)
{
    int rc = VINF_SUCCESS;
    if (pInt->fOpenMode & RTFILE_O_WRITE)
    {
        /* Let the write worker thread start immediately. */
        rc = shaSignalManifestThread(pInt, STATUS_WRITE);
        if (RT_FAILURE(rc))
            return rc;

        /* Wait until the write worker thread has finished. */
        rc = shaWaitForManifestThreadFinished(pInt);
    }

    return rc;
}

static int shaOpenCallback(void *pvUser, const char *pszLocation, uint32_t fOpen,
                              PFNVDCOMPLETED pfnCompleted, void **ppInt)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pszLocation, VERR_INVALID_POINTER);
    AssertPtrNullReturn(pfnCompleted, VERR_INVALID_PARAMETER);
    AssertPtrReturn(ppInt, VERR_INVALID_POINTER);
    AssertReturn((fOpen & RTFILE_O_READWRITE) != RTFILE_O_READWRITE, VERR_INVALID_PARAMETER); /* No read/write allowed */

    PSHASTORAGE pShaStorage = (PSHASTORAGE)pvUser;
    PVDINTERFACEIO pIfIo = VDIfIoGet(pShaStorage->pVDImageIfaces);
    AssertPtrReturn(pIfIo, VERR_INVALID_PARAMETER);

    DEBUG_PRINT_FLOW();

    PSHASTORAGEINTERNAL pInt = (PSHASTORAGEINTERNAL)RTMemAllocZ(sizeof(SHASTORAGEINTERNAL));
    if (!pInt)
        return VERR_NO_MEMORY;

    int rc = VINF_SUCCESS;
    do
    {
        pInt->pfnCompleted = pfnCompleted;
        pInt->pShaStorage  = pShaStorage;
        pInt->fEOF         = false;
        pInt->fOpenMode    = fOpen;
        pInt->u32Status    = STATUS_WAIT;

        /* Circular buffer in the read case. */
        rc = RTCircBufCreate(&pInt->pCircBuf, _1M * 2);
        if (RT_FAILURE(rc))
            break;

        if (fOpen & RTFILE_O_WRITE)
        {
            /* The zero buffer is used for appending empty parts at the end of the
             * file (or our buffer) in setSize or when uOffset in writeSync is
             * increased in steps bigger than a byte. */
            pInt->cbZeroBuf = _1K;
            pInt->pvZeroBuf = RTMemAllocZ(pInt->cbZeroBuf);
            if (!pInt->pvZeroBuf)
            {
                rc = VERR_NO_MEMORY;
                break;
            }
        }

        /* Create an event semaphore to indicate a state change for the worker
         * thread. */
        rc = RTSemEventCreate(&pInt->newStatusEvent);
        if (RT_FAILURE(rc))
            break;
        /* Create an event semaphore to indicate a finished calculation of the
           worker thread. */
        rc = RTSemEventCreate(&pInt->workFinishedEvent);
        if (RT_FAILURE(rc))
            break;
        /* Create the worker thread. */
        rc = RTThreadCreate(&pInt->pWorkerThread, shaCalcWorkerThread, pInt, 0, RTTHREADTYPE_MAIN_HEAVY_WORKER, RTTHREADFLAGS_WAITABLE, "SHA-Worker");
        if (RT_FAILURE(rc))
            break;

        if (pShaStorage->fCreateDigest)
        {
            /* Create a SHA1/SHA256 context the worker thread will work with. */
            if (pShaStorage->fSha256)
                RTSha256Init(&pInt->ctx.Sha256);
            else
                RTSha1Init(&pInt->ctx.Sha1);
        }

        /* Open the file. */
        rc = vdIfIoFileOpen(pIfIo, pszLocation, fOpen, pInt->pfnCompleted,
                            &pInt->pvStorage);
        if (RT_FAILURE(rc))
            break;

        if (fOpen & RTFILE_O_READ)
        {
            /* Immediately let the worker thread start the reading. */
            rc = shaSignalManifestThread(pInt, STATUS_READ);
        }
    }
    while (0);

    if (RT_FAILURE(rc))
    {
        if (pInt->pWorkerThread)
        {
            shaSignalManifestThread(pInt, STATUS_END);
            RTThreadWait(pInt->pWorkerThread, RT_INDEFINITE_WAIT, 0);
        }
        if (pInt->workFinishedEvent)
            RTSemEventDestroy(pInt->workFinishedEvent);
        if (pInt->newStatusEvent)
            RTSemEventDestroy(pInt->newStatusEvent);
        if (pInt->pCircBuf)
            RTCircBufDestroy(pInt->pCircBuf);
        if (pInt->pvZeroBuf)
            RTMemFree(pInt->pvZeroBuf);
        RTMemFree(pInt);
    }
    else
        *ppInt = pInt;

    return rc;
}

static int shaCloseCallback(void *pvUser, void *pvStorage)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    PSHASTORAGE pShaStorage = (PSHASTORAGE)pvUser;
    PVDINTERFACEIO pIfIo = VDIfIoGet(pShaStorage->pVDImageIfaces);
    AssertPtrReturn(pIfIo, VERR_INVALID_PARAMETER);

    PSHASTORAGEINTERNAL pInt = (PSHASTORAGEINTERNAL)pvStorage;

    DEBUG_PRINT_FLOW();

    int rc = VINF_SUCCESS;

    /* Make sure all pending writes are flushed */
    rc = shaFlushCurBuf(pInt);

    if (pInt->pWorkerThread)
    {
        /* Signal the worker thread to end himself */
        rc = shaSignalManifestThread(pInt, STATUS_END);
        /* Worker thread stopped? */
        rc = RTThreadWait(pInt->pWorkerThread, RT_INDEFINITE_WAIT, 0);
    }

    if (   RT_SUCCESS(rc)
        && pShaStorage->fCreateDigest)
    {
        /* Finally calculate & format the SHA1/SHA256 sum */
        unsigned char auchDig[RTSHA256_HASH_SIZE];
        char *pszDigest;
        size_t cbDigest;
        if (pShaStorage->fSha256)
        {
            RTSha256Final(&pInt->ctx.Sha256, auchDig);
            cbDigest = RTSHA256_DIGEST_LEN;
        }
        else
        {
            RTSha1Final(&pInt->ctx.Sha1, auchDig);
            cbDigest = RTSHA1_DIGEST_LEN;
        }
        rc = RTStrAllocEx(&pszDigest, cbDigest + 1);
        if (RT_SUCCESS(rc))
        {
            if (pShaStorage->fSha256)
                rc = RTSha256ToString(auchDig, pszDigest, cbDigest + 1);
            else
                rc = RTSha1ToString(auchDig, pszDigest, cbDigest + 1);
            if (RT_SUCCESS(rc))
                pShaStorage->strDigest = pszDigest;
            RTStrFree(pszDigest);
        }
    }

    /* Close the file */
    rc = vdIfIoFileClose(pIfIo, pInt->pvStorage);

//    RTPrintf("%lu %lu\n", pInt->calls, pInt->waits);

    /* Cleanup */
    if (pInt->workFinishedEvent)
        RTSemEventDestroy(pInt->workFinishedEvent);
    if (pInt->newStatusEvent)
        RTSemEventDestroy(pInt->newStatusEvent);
    if (pInt->pCircBuf)
        RTCircBufDestroy(pInt->pCircBuf);
    if (pInt->pvZeroBuf)
        RTMemFree(pInt->pvZeroBuf);
    RTMemFree(pInt);

    return rc;
}

static int shaDeleteCallback(void *pvUser, const char *pcszFilename)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);

    PSHASTORAGE pShaStorage = (PSHASTORAGE)pvUser;
    PVDINTERFACEIO pIfIo = VDIfIoGet(pShaStorage->pVDImageIfaces);
    AssertPtrReturn(pIfIo, VERR_INVALID_PARAMETER);

    DEBUG_PRINT_FLOW();

    return vdIfIoFileDelete(pIfIo, pcszFilename);
}

static int shaMoveCallback(void *pvUser, const char *pcszSrc, const char *pcszDst, unsigned fMove)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);

    PSHASTORAGE pShaStorage = (PSHASTORAGE)pvUser;
    PVDINTERFACEIO pIfIo = VDIfIoGet(pShaStorage->pVDImageIfaces);
    AssertPtrReturn(pIfIo, VERR_INVALID_PARAMETER);


    DEBUG_PRINT_FLOW();

    return vdIfIoFileMove(pIfIo, pcszSrc, pcszDst, fMove);
}

static int shaGetFreeSpaceCallback(void *pvUser, const char *pcszFilename, int64_t *pcbFreeSpace)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);

    PSHASTORAGE pShaStorage = (PSHASTORAGE)pvUser;
    PVDINTERFACEIO pIfIo = VDIfIoGet(pShaStorage->pVDImageIfaces);
    AssertPtrReturn(pIfIo, VERR_INVALID_PARAMETER);

    DEBUG_PRINT_FLOW();

    return vdIfIoFileGetFreeSpace(pIfIo, pcszFilename, pcbFreeSpace);
}

static int shaGetModificationTimeCallback(void *pvUser, const char *pcszFilename, PRTTIMESPEC pModificationTime)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);

    PSHASTORAGE pShaStorage = (PSHASTORAGE)pvUser;
    PVDINTERFACEIO pIfIo = VDIfIoGet(pShaStorage->pVDImageIfaces);
    AssertPtrReturn(pIfIo, VERR_INVALID_PARAMETER);

    DEBUG_PRINT_FLOW();

    return vdIfIoFileGetModificationTime(pIfIo, pcszFilename, pModificationTime);
}


static int shaGetSizeCallback(void *pvUser, void *pvStorage, uint64_t *pcbSize)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    PSHASTORAGE pShaStorage = (PSHASTORAGE)pvUser;
    PVDINTERFACEIO pIfIo = VDIfIoGet(pShaStorage->pVDImageIfaces);
    AssertPtrReturn(pIfIo, VERR_INVALID_PARAMETER);

    PSHASTORAGEINTERNAL pInt = (PSHASTORAGEINTERNAL)pvStorage;

    DEBUG_PRINT_FLOW();

    uint64_t cbSize;
    int rc = vdIfIoFileGetSize(pIfIo, pInt->pvStorage, &cbSize);
    if (RT_FAILURE(rc))
        return rc;

    *pcbSize = RT_MAX(pInt->cbCurAll, cbSize);

    return VINF_SUCCESS;
}

static int shaSetSizeCallback(void *pvUser, void *pvStorage, uint64_t cbSize)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    PSHASTORAGE pShaStorage = (PSHASTORAGE)pvUser;
    PVDINTERFACEIO pIfIo = VDIfIoGet(pShaStorage->pVDImageIfaces);
    AssertPtrReturn(pIfIo, VERR_INVALID_PARAMETER);

    PSHASTORAGEINTERNAL pInt = (PSHASTORAGEINTERNAL)pvStorage;

    DEBUG_PRINT_FLOW();

    return vdIfIoFileSetSize(pIfIo, pInt->pvStorage, cbSize);
}

static int shaWriteSyncCallback(void *pvUser, void *pvStorage, uint64_t uOffset,
                                 const void *pvBuf, size_t cbWrite, size_t *pcbWritten)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    PSHASTORAGE pShaStorage = (PSHASTORAGE)pvUser;
    PVDINTERFACEIO pIfIo = VDIfIoGet(pShaStorage->pVDImageIfaces);
    AssertPtrReturn(pIfIo, VERR_INVALID_PARAMETER);

    PSHASTORAGEINTERNAL pInt = (PSHASTORAGEINTERNAL)pvStorage;

    DEBUG_PRINT_FLOW();

    /* Check that the write is linear */
    AssertMsgReturn(pInt->cbCurAll <= uOffset, ("Backward seeking is not allowed (uOffset: %7lu cbCurAll: %7lu)!", uOffset, pInt->cbCurAll), VERR_INVALID_PARAMETER);

    int rc = VINF_SUCCESS;

    /* Check if we have to add some free space at the end, before we start the
     * real write. */
    if (pInt->cbCurAll < uOffset)
    {
        size_t cbSize = (size_t)(uOffset - pInt->cbCurAll);
        size_t cbAllWritten = 0;
        for (;;)
        {
            /* Finished? */
            if (cbAllWritten == cbSize)
                break;
            size_t cbToWrite = RT_MIN(pInt->cbZeroBuf, cbSize - cbAllWritten);
            size_t cbWritten = 0;
            rc = shaWriteSyncCallback(pvUser, pvStorage, pInt->cbCurAll,
                                      pInt->pvZeroBuf, cbToWrite, &cbWritten);
            if (RT_FAILURE(rc))
                break;
            cbAllWritten += cbWritten;
        }
        if (RT_FAILURE(rc))
            return rc;
    }
//    RTPrintf("Write uOffset: %7lu cbWrite: %7lu = %7lu\n", uOffset, cbWrite, uOffset + cbWrite);

    size_t cbAllWritten = 0;
    for (;;)
    {
        /* Finished? */
        if (cbAllWritten == cbWrite)
            break;
        size_t cbAvail = RTCircBufFree(pInt->pCircBuf);
        if (   cbAvail == 0
            && pInt->fEOF)
            return VERR_EOF;
        /* If there isn't enough free space make sure the worker thread is
         * writing some data. */
        if ((cbWrite - cbAllWritten) > cbAvail)
        {
            rc = shaSignalManifestThread(pInt, STATUS_WRITE);
            if (RT_FAILURE(rc))
                break;
            /* If there is _no_ free space available, we have to wait until it is. */
            if (cbAvail == 0)
            {
                rc = shaWaitForManifestThreadFinished(pInt);
                if (RT_FAILURE(rc))
                    break;
                cbAvail = RTCircBufFree(pInt->pCircBuf);
//                RTPrintf("############## wait %lu %lu %lu \n", cbRead, cbAllRead, cbAvail);
//                pInt->waits++;
            }
        }
        size_t cbToWrite = RT_MIN(cbWrite - cbAllWritten, cbAvail);
        char *pcBuf;
        size_t cbMemWritten = 0;
        /* Acquire a block for writing from our circular buffer. */
        RTCircBufAcquireWriteBlock(pInt->pCircBuf, cbToWrite, (void**)&pcBuf, &cbMemWritten);
        memcpy(pcBuf, &((char*)pvBuf)[cbAllWritten], cbMemWritten);
        /* Mark the block full. */
        RTCircBufReleaseWriteBlock(pInt->pCircBuf, cbMemWritten);
        cbAllWritten += cbMemWritten;
        pInt->cbCurAll += cbMemWritten;
    }

    if (pcbWritten)
        *pcbWritten = cbAllWritten;

    /* Signal the thread to write more data in the mean time. */
    if (   RT_SUCCESS(rc)
           && RTCircBufUsed(pInt->pCircBuf) >= (RTCircBufSize(pInt->pCircBuf) / 2))
        rc = shaSignalManifestThread(pInt, STATUS_WRITE);

    return rc;
}

static int shaReadSyncCallback(void *pvUser, void *pvStorage, uint64_t uOffset,
                               void *pvBuf, size_t cbRead, size_t *pcbRead)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    PSHASTORAGE pShaStorage = (PSHASTORAGE)pvUser;
    PVDINTERFACEIO pIfIo = VDIfIoGet(pShaStorage->pVDImageIfaces);
    AssertPtrReturn(pIfIo, VERR_INVALID_PARAMETER);

//    DEBUG_PRINT_FLOW();

    PSHASTORAGEINTERNAL pInt = (PSHASTORAGEINTERNAL)pvStorage;

    int rc = VINF_SUCCESS;

//    pInt->calls++;
//    RTPrintf("Read uOffset: %7lu cbRead: %7lu = %7lu\n", uOffset, cbRead, uOffset + cbRead);

    /* Check if we jump forward in the file. If so we have to read the
     * remaining stuff in the gap anyway (SHA1/SHA256; streaming). */
    if (pInt->cbCurAll < uOffset)
    {
        rc = shaReadSyncCallback(pvUser, pvStorage, pInt->cbCurAll, 0,
                                 (size_t)(uOffset - pInt->cbCurAll), 0);
        if (RT_FAILURE(rc))
            return rc;
//        RTPrintf("Gap Read uOffset: %7lu cbRead: %7lu = %7lu\n", uOffset, cbRead, uOffset + cbRead);
    }
    else if (uOffset < pInt->cbCurAll)
        AssertMsgFailed(("Jumping backwards is not possible, sequential access is supported only\n"));

    size_t cbAllRead = 0;
    for (;;)
    {
        /* Finished? */
        if (cbAllRead == cbRead)
            break;
        size_t cbAvail = RTCircBufUsed(pInt->pCircBuf);
        if (    cbAvail == 0
            &&  pInt->fEOF
            && !RTCircBufIsWriting(pInt->pCircBuf))
        {
            break;
        }
        /* If there isn't enough data make sure the worker thread is fetching
         * more. */
        if ((cbRead - cbAllRead) > cbAvail)
        {
            rc = shaSignalManifestThread(pInt, STATUS_READ);
            if (RT_FAILURE(rc))
                break;
            /* If there is _no_ data available, we have to wait until it is. */
            if (cbAvail == 0)
            {
                rc = shaWaitForManifestThreadFinished(pInt);
                if (RT_FAILURE(rc))
                    break;
                cbAvail = RTCircBufUsed(pInt->pCircBuf);
//                RTPrintf("############## wait %lu %lu %lu \n", cbRead, cbAllRead, cbAvail);
//                pInt->waits++;
            }
        }
        size_t cbToRead = RT_MIN(cbRead - cbAllRead, cbAvail);
        char *pcBuf;
        size_t cbMemRead = 0;
        /* Acquire a block for reading from our circular buffer. */
        RTCircBufAcquireReadBlock(pInt->pCircBuf, cbToRead, (void**)&pcBuf, &cbMemRead);
        if (pvBuf) /* Make it possible to blind read data (for skipping) */
            memcpy(&((char*)pvBuf)[cbAllRead], pcBuf, cbMemRead);
        /* Mark the block as empty again. */
        RTCircBufReleaseReadBlock(pInt->pCircBuf, cbMemRead);
        cbAllRead += cbMemRead;

        pInt->cbCurAll += cbMemRead;
    }

    if (pcbRead)
        *pcbRead = cbAllRead;

    if (rc == VERR_EOF)
        rc = VINF_SUCCESS;

    /* Signal the thread to read more data in the mean time. */
    if (   RT_SUCCESS(rc)
        && RTCircBufFree(pInt->pCircBuf) >= (RTCircBufSize(pInt->pCircBuf) / 2))
        rc = shaSignalManifestThread(pInt, STATUS_READ);

    return rc;
}

static int shaFlushSyncCallback(void *pvUser, void *pvStorage)
{
    /* Validate input. */
    AssertPtrReturn(pvUser, VERR_INVALID_POINTER);
    AssertPtrReturn(pvStorage, VERR_INVALID_POINTER);

    PSHASTORAGE pShaStorage = (PSHASTORAGE)pvUser;
    PVDINTERFACEIO pIfIo = VDIfIoGet(pShaStorage->pVDImageIfaces);
    AssertPtrReturn(pIfIo, VERR_INVALID_PARAMETER);

    DEBUG_PRINT_FLOW();

    PSHASTORAGEINTERNAL pInt = (PSHASTORAGEINTERNAL)pvStorage;

    /* Check if there is still something in the buffer. If yes, flush it. */
    int rc = shaFlushCurBuf(pInt);
    if (RT_FAILURE(rc))
        return rc;

    return vdIfIoFileFlushSync(pIfIo, pInt->pvStorage);
}

PVDINTERFACEIO ShaCreateInterface()
{
    PVDINTERFACEIO pCallbacks = (PVDINTERFACEIO)RTMemAllocZ(sizeof(VDINTERFACEIO));
    if (!pCallbacks)
        return NULL;

    pCallbacks->pfnOpen                = shaOpenCallback;
    pCallbacks->pfnClose               = shaCloseCallback;
    pCallbacks->pfnDelete              = shaDeleteCallback;
    pCallbacks->pfnMove                = shaMoveCallback;
    pCallbacks->pfnGetFreeSpace        = shaGetFreeSpaceCallback;
    pCallbacks->pfnGetModificationTime = shaGetModificationTimeCallback;
    pCallbacks->pfnGetSize             = shaGetSizeCallback;
    pCallbacks->pfnSetSize             = shaSetSizeCallback;
    pCallbacks->pfnReadSync            = shaReadSyncCallback;
    pCallbacks->pfnWriteSync           = shaWriteSyncCallback;
    pCallbacks->pfnFlushSync           = shaFlushSyncCallback;

    return pCallbacks;
}

PVDINTERFACEIO FileCreateInterface()
{
    PVDINTERFACEIO pCallbacks = (PVDINTERFACEIO)RTMemAllocZ(sizeof(VDINTERFACEIO));
    if (!pCallbacks)
        return NULL;

    pCallbacks->pfnOpen                = fileOpenCallback;
    pCallbacks->pfnClose               = fileCloseCallback;
    pCallbacks->pfnDelete              = fileDeleteCallback;
    pCallbacks->pfnMove                = fileMoveCallback;
    pCallbacks->pfnGetFreeSpace        = fileGetFreeSpaceCallback;
    pCallbacks->pfnGetModificationTime = fileGetModificationTimeCallback;
    pCallbacks->pfnGetSize             = fileGetSizeCallback;
    pCallbacks->pfnSetSize             = fileSetSizeCallback;
    pCallbacks->pfnReadSync            = fileReadSyncCallback;
    pCallbacks->pfnWriteSync           = fileWriteSyncCallback;
    pCallbacks->pfnFlushSync           = fileFlushSyncCallback;

    return pCallbacks;
}

PVDINTERFACEIO TarCreateInterface()
{
    PVDINTERFACEIO pCallbacks = (PVDINTERFACEIO)RTMemAllocZ(sizeof(VDINTERFACEIO));
    if (!pCallbacks)
        return NULL;

    pCallbacks->pfnOpen                = tarOpenCallback;
    pCallbacks->pfnClose               = tarCloseCallback;
    pCallbacks->pfnDelete              = tarDeleteCallback;
    pCallbacks->pfnMove                = tarMoveCallback;
    pCallbacks->pfnGetFreeSpace        = tarGetFreeSpaceCallback;
    pCallbacks->pfnGetModificationTime = tarGetModificationTimeCallback;
    pCallbacks->pfnGetSize             = tarGetSizeCallback;
    pCallbacks->pfnSetSize             = tarSetSizeCallback;
    pCallbacks->pfnReadSync            = tarReadSyncCallback;
    pCallbacks->pfnWriteSync           = tarWriteSyncCallback;
    pCallbacks->pfnFlushSync           = tarFlushSyncCallback;

    return pCallbacks;
}

int ShaReadBuf(const char *pcszFilename, void **ppvBuf, size_t *pcbSize, PVDINTERFACEIO pIfIo, void *pvUser)
{
    /* Validate input. */
    AssertPtrReturn(ppvBuf, VERR_INVALID_POINTER);
    AssertPtrReturn(pcbSize, VERR_INVALID_POINTER);
    AssertPtrReturn(pIfIo, VERR_INVALID_POINTER);

    void *pvStorage;
    int rc = pIfIo->pfnOpen(pvUser, pcszFilename,
                            RTFILE_O_OPEN | RTFILE_O_READ | RTFILE_O_DENY_NONE, 0,
                            &pvStorage);
    if (RT_FAILURE(rc))
        return rc;

    void *pvTmpBuf = 0;
    void *pvBuf = 0;
    uint64_t cbTmpSize = _1M;
    size_t cbAllRead = 0;
    do
    {
        pvTmpBuf = RTMemAlloc(cbTmpSize);
        if (!pvTmpBuf)
        {
            rc = VERR_NO_MEMORY;
            break;
        }

        for (;;)
        {
            size_t cbRead = 0;
            rc = pIfIo->pfnReadSync(pvUser, pvStorage, cbAllRead, pvTmpBuf, cbTmpSize, &cbRead);
            if (   RT_FAILURE(rc)
                || cbRead == 0)
                break;
            pvBuf = RTMemRealloc(pvBuf, cbAllRead + cbRead);
            if (!pvBuf)
            {
                rc = VERR_NO_MEMORY;
                break;
            }
            memcpy(&((char*)pvBuf)[cbAllRead], pvTmpBuf, cbRead);
            cbAllRead += cbRead;
        }
    } while (0);

    pIfIo->pfnClose(pvUser, pvStorage);

    if (rc == VERR_EOF)
        rc = VINF_SUCCESS;

    if (pvTmpBuf)
        RTMemFree(pvTmpBuf);

    if (RT_SUCCESS(rc))
    {
        *ppvBuf = pvBuf;
        *pcbSize = cbAllRead;
    }
    else
    {
        if (pvBuf)
            RTMemFree(pvBuf);
    }

    return rc;
}

int ShaWriteBuf(const char *pcszFilename, void *pvBuf, size_t cbSize, PVDINTERFACEIO pIfIo, void *pvUser)
{
    /* Validate input. */
    AssertPtrReturn(pvBuf, VERR_INVALID_POINTER);
    AssertReturn(cbSize, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pIfIo, VERR_INVALID_POINTER);

    void *pvStorage;
    int rc = pIfIo->pfnOpen(pvUser, pcszFilename,
                            RTFILE_O_CREATE | RTFILE_O_WRITE | RTFILE_O_DENY_ALL, 0,
                            &pvStorage);
    if (RT_FAILURE(rc))
        return rc;

    size_t cbAllWritten = 0;
    for (;;)
    {
        if (cbAllWritten >= cbSize)
            break;
        size_t cbToWrite = cbSize - cbAllWritten;
        size_t cbWritten = 0;
        rc = pIfIo->pfnWriteSync(pvUser, pvStorage, cbAllWritten, &((char*)pvBuf)[cbAllWritten], cbToWrite, &cbWritten);
        if (RT_FAILURE(rc))
            break;
        cbAllWritten += cbWritten;
    }

    rc = pIfIo->pfnClose(pvUser, pvStorage);

    return rc;
}

int decompressImageAndSave(const char *pcszFullFilenameIn, const char *pcszFullFilenameOut, PVDINTERFACEIO pIfIo, void *pvUser)
{
    /* Validate input. */
    AssertPtrReturn(pIfIo, VERR_INVALID_POINTER);

    PSHASTORAGE pShaStorage = (PSHASTORAGE)pvUser;
    /*
     * Open the source file.
     */
    void *pvStorage;
    int rc = pIfIo->pfnOpen(pvUser, pcszFullFilenameIn,
                            RTFILE_O_OPEN | RTFILE_O_READ | RTFILE_O_DENY_NONE, 0,
                            &pvStorage);
    if (RT_FAILURE(rc))
        return rc;

    /* Turn the source file handle/whatever into a VFS stream. */
    RTVFSIOSTREAM hVfsIosCompressedSrc;

    rc = VDIfCreateVfsStream(pIfIo, pvStorage, RTFILE_O_READ, &hVfsIosCompressedSrc);
    if (RT_SUCCESS(rc))
    {
        /* Pass the source thru gunzip. */
        RTVFSIOSTREAM hVfsIosSrc;
        rc = RTZipGzipDecompressIoStream(hVfsIosCompressedSrc, 0, &hVfsIosSrc);
        if (RT_SUCCESS(rc))
        {
            /*
             * Create the output file, including necessary paths.
             * Any existing file will be overwritten.
             */
            rc = VirtualBox::ensureFilePathExists(Utf8Str(pcszFullFilenameOut), true /*fCreate*/);
            if (RT_SUCCESS(rc))
            {
                RTVFSIOSTREAM hVfsIosDst;
                rc = RTVfsIoStrmOpenNormal(pcszFullFilenameOut,
                                           RTFILE_O_CREATE_REPLACE | RTFILE_O_WRITE | RTFILE_O_DENY_ALL,
                                           &hVfsIosDst);
                if (RT_SUCCESS(rc))
                {
                    /*
                     * Pump the bytes thru. If we fail, delete the output file.
                     */
                    RTMANIFEST hFileManifest = NIL_RTMANIFEST;
                    rc = RTManifestCreate(0 /*fFlags*/, &hFileManifest);
                    if (RT_SUCCESS(rc))
                    {
                        RTVFSIOSTREAM hVfsIosMfst;

                        uint32_t digestType = (pShaStorage->fSha256 == true) ? RTMANIFEST_ATTR_SHA256: RTMANIFEST_ATTR_SHA1;

                        rc = RTManifestEntryAddPassthruIoStream(hFileManifest,
                                                                hVfsIosSrc,
                                                                "ovf import",
                                                                digestType,
                                                                true /*read*/, &hVfsIosMfst);
                        if (RT_SUCCESS(rc))
                        {
                            rc = RTVfsUtilPumpIoStreams(hVfsIosMfst, hVfsIosDst, 0);
                        }

                        RTVfsIoStrmRelease(hVfsIosMfst);
                    }

                    RTVfsIoStrmRelease(hVfsIosDst);
                }
            }

            RTVfsIoStrmRelease(hVfsIosSrc);
        }
    }
    pIfIo->pfnClose(pvUser, pvStorage);

    return rc;
}


