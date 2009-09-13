/*
 * Copyright (c) 2008 James Molloy, Jörg Pfähler, Matthew Iselin
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <Module.h>
#include <Log.h>
#include <processor/Processor.h>
#include "sqlite3.h"
#include <utilities/utility.h>
#include <BootstrapInfo.h>
#include <panic.h>

#include <processor/PhysicalMemoryManager.h>
#include <processor/VirtualAddressSpace.h>
#include <processor/MemoryRegion.h>

#include "Config.h"

extern BootstrapStruct_t *g_pBootstrapInfo;

uint8_t *g_pFile = 0;
size_t g_FileSz = 0;

extern "C" int atoi(const char *str)
{
    return strtoul(str, 0, 10);
}

extern "C" int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *c1 = reinterpret_cast<const unsigned char*>(s1);
    const unsigned char *c2 = reinterpret_cast<const unsigned char*>(s2);
    for (size_t i = 0; i < n; i++)
        if (c1[i] != c2[i])
            return (c1[i]>c2[i]) ? 1 : -1;
    return 0;
}

int xClose(sqlite3_file *file)
{
    return 0;
}

int xRead(sqlite3_file *file, void *ptr, int iAmt, sqlite3_int64 iOfst)
{
    int ret = 0;
    if (iOfst+iAmt >= g_FileSz)
    {
        memset(&ptr, 0, iAmt);
        iAmt = g_FileSz - iOfst;
        ret = SQLITE_IOERR_SHORT_READ;
    }
    memcpy(ptr, &g_pFile[iOfst], iAmt);
    return ret;
}

int xReadFail(sqlite3_file *file, void *ptr, int iAmt, sqlite3_int64 iOfst)
{
    memset(ptr, 0, iAmt);
    return SQLITE_IOERR_SHORT_READ;
}

int xWrite(sqlite3_file *file, const void *ptr, int iAmt, sqlite3_int64 iOfst)
{
    if (iOfst+iAmt >= g_FileSz)
    {
        uint8_t *tmp = new uint8_t[g_FileSz*2];
        memset(tmp, 0, g_FileSz*2);
        memcpy(tmp, g_pFile, g_FileSz);
        delete [] g_pFile;
        g_pFile = tmp;
        g_FileSz *= 2;
    }

    memcpy(&g_pFile[iOfst], ptr, iAmt);
    return 0;
}

int xWriteFail(sqlite3_file *file, const void *ptr, int iAmt, sqlite3_int64 iOfst)
{
    return 0;
}

int xTruncate(sqlite3_file *file, sqlite3_int64 size)
{
    return 0;
}

int xSync(sqlite3_file *file, int flags)
{
    return 0;
}

int xFileSize(sqlite3_file *file, sqlite3_int64 *pSize)
{
    *pSize = g_FileSz;
    return 0;
}

int xLock(sqlite3_file *file, int a)
{
    return 0;
}

int xUnlock(sqlite3_file *file, int a)
{
    return 0;
}

int xCheckReservedLock(sqlite3_file *file, int *pResOut)
{
    *pResOut = 0;
    return 0;
}

int xFileControl(sqlite3_file *file, int op, void *pArg)
{
    return 0;
}

int xSectorSize(sqlite3_file *file)
{
    return 1;
}

int xDeviceCharacteristics(sqlite3_file *file)
{
    return 0;
}

static struct sqlite3_io_methods theio = 
{
    1,
    &xClose,
    &xRead,
    &xWrite,
    &xTruncate,
    &xSync,
    &xFileSize,
    &xLock,
    &xUnlock,
    &xCheckReservedLock,
    &xFileControl,
    &xSectorSize,
    &xDeviceCharacteristics
};

static struct sqlite3_io_methods theio_fail = 
{
    1,
    &xClose,
    &xReadFail,
    &xWriteFail,
    &xTruncate,
    &xSync,
    &xFileSize,
    &xLock,
    &xUnlock,
    &xCheckReservedLock,
    &xFileControl,
    &xSectorSize,
    &xDeviceCharacteristics
};


int xOpen(sqlite3_vfs *vfs, const char *zName, sqlite3_file *file, int flags, int *pOutFlags)
{
    if (strcmp(zName, "root»/.pedigree-root"))
    {
        // Assume journal file, return failure functions.
        file->pMethods = &theio_fail;
        return 0;
    }

    if (!g_pBootstrapInfo->isDatabaseLoaded())
    {
        FATAL("Config database not loaded!");
    }

    file->pMethods = &theio;
    return 0;
}

int xDelete(sqlite3_vfs *vfs, const char *zName, int syncDir)
{
    return 0;
}

int xAccess(sqlite3_vfs *vfs, const char *zName, int flags, int *pResOut)
{
    return 0;
}

int xFullPathname(sqlite3_vfs *vfs, const char *zName, int nOut, char *zOut)
{
    strncpy(zOut, zName, nOut);
    return 0;
}

void *xDlOpen(sqlite3_vfs *vfs, const char *zFilename)
{
    return 0;
}

void xDlError(sqlite3_vfs *vfs, int nByte, char *zErrMsg)
{
}

void (*xDlSym(sqlite3_vfs *vfs, void *p, const char *zSymbol))(void)
{
    return 0;
}

void xDlClose(sqlite3_vfs *vfs, void *v)
{
}

int xRandomness(sqlite3_vfs *vfs, int nByte, char *zOut)
{
    return 0;
}

int xSleep(sqlite3_vfs *vfs, int microseconds)
{
    return 0;
}

int xCurrentTime(sqlite3_vfs *vfs, double *)
{
    return 0;
}

int xGetLastError(sqlite3_vfs *vfs, int i, char *c)
{
    return 0;
}



static struct sqlite3_vfs thevfs =
{
    1,
    sizeof(void*),
    32,
    0,
    "no-vfs",
    0,
    &xOpen,
    &xDelete,
    &xAccess,
    &xFullPathname,
    &xDlOpen,
    &xDlError,
    &xDlSym,
    &xDlClose,
    &xRandomness,
    &xSleep,
    &xCurrentTime,
    &xGetLastError
};

int sqlite3_os_init()
{
    sqlite3_vfs_register(&thevfs, 1);
    return 0;
}

int sqlite3_os_end()
{
    return 0;
}

void xCallback(sqlite3_context *context, int n, sqlite3_value **values)
{
    const unsigned char *text = sqlite3_value_text(values[0]);
    uintptr_t x = strtoul(reinterpret_cast<const char*>(text), 0, 16);
    void (*func)(void) = reinterpret_cast< void (*)(void) >(x);
    func();
    sqlite3_result_int(context, 0);
}

sqlite3 *g_pSqlite = 0;

void init()
{
    if (!g_pBootstrapInfo->isDatabaseLoaded())
        FATAL("Database not loaded, cannot continue.");

    uint8_t *pPhys = g_pBootstrapInfo->getDatabaseAddress();
    size_t sSize = g_pBootstrapInfo->getDatabaseSize();

    if ((reinterpret_cast<physical_uintptr_t>(pPhys) & (PhysicalMemoryManager::getPageSize() - 1)) != 0)
        panic("Config: Alignment issues");

    MemoryRegion region("Config");

    if (PhysicalMemoryManager::instance().allocateRegion(region,
                                                         (sSize + PhysicalMemoryManager::getPageSize() - 1) / PhysicalMemoryManager::getPageSize(),
                                                         PhysicalMemoryManager::continuous,
                                                         VirtualAddressSpace::KernelMode,
                                                         reinterpret_cast<physical_uintptr_t>(pPhys))
        == false)
    {
        ERROR("Config: allocateRegion failed.");
    }

    g_pFile = new uint8_t[sSize];
    memcpy(g_pFile, region.virtualAddress(), sSize);
    g_FileSz = sSize;

    sqlite3_initialize();

    int ret = sqlite3_open("root»/.pedigree-root", &g_pSqlite);
    if (ret)
    {
        FATAL("sqlite3 error: " << sqlite3_errmsg(g_pSqlite));
    }

    sqlite3_create_function(g_pSqlite, "pedigree_callback", 1, SQLITE_ANY, 0, &xCallback, 0, 0);
}

void destroy()
{
}

MODULE_NAME("config");
MODULE_ENTRY(&init);
MODULE_EXIT(&destroy);
MODULE_DEPENDS(0);