/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2013-2017 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#ifndef Engine_Cache_h
#define Engine_Cache_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>
#include <sstream> // stringstream
#include <fstream>
#include <functional>
#include <list>
#include <set>
#include <cstddef>
#include <utility>
#include <algorithm> // min, max

#include "Global/GlobalDefines.h"

GCC_DIAG_OFF(deprecated)
#include <QtCore/QMutex>
#include <QtCore/QThread>
#include <QtCore/QWaitCondition>
#include <QtCore/QMutexLocker>
#include <QtCore/QObject>
#include <QtCore/QBuffer>
#include <QtCore/QRunnable>
GCC_DIAG_ON(deprecated)
#if !defined(Q_MOC_RUN) && !defined(SBK_RUN)
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#endif

#include "Engine/CacheEntryBase.h"
#include "Engine/EngineFwd.h"

// Each 8 bit tile will have pow(2, tileSizePo2) pixels in each dimension.
// 16 bit tiles will have one side halved
// 32 bit tiles will have both dimension halved (so tile size for 32bit is actually pow(2, tileSizePo2-1)
//
// default is tileSizePo2=6, thus a 8 bit tile will be 64x64 pixels
#define NATRON_8BIT_TILE_SIZE_PO2 6

// tileByteSize = std::pow(2, NATRON_8BIT_TILE_SIZE_PO2);
// tileByteSize *= tileByteSize;
#define NATRON_TILE_SIZE_BYTES 4096

#define NATRON_TILE_SIZE_X_8_BIT 64
#define NATRON_TILE_SIZE_Y_8_BIT 64

#define NATRON_TILE_SIZE_X_16_BIT 64
#define NATRON_TILE_SIZE_Y_16_BIT 32

#define NATRON_TILE_SIZE_X_32_BIT 32
#define NATRON_TILE_SIZE_Y_32_BIT 32

// The name of the directory containing all buckets on disk
#define NATRON_CACHE_DIRECTORY_NAME "Cache"

NATRON_NAMESPACE_ENTER;


struct CacheReportInfo
{
    int nEntries;
    std::size_t ramBytes, diskBytes, glTextureBytes;

    CacheReportInfo()
    : nEntries(0)
    , ramBytes(0)
    , diskBytes(0)
    , glTextureBytes(0)
    {

    }
};



struct CacheBucket;

/**
 * @brief Small RAII style class used to lock an entry corresponding to a hash key to ensure
 * only a single thread can work on it at once.
 * Mainly this is to avoid multiple threads from computing the same image at once.
 **/
struct CacheEntryLockerPrivate;
class CacheEntryLocker;

typedef boost::interprocess::deleter<CacheEntryLocker, MemorySegmentType::segment_manager>  CacheEntryLocker_deleter;
typedef boost::interprocess::shared_ptr<CacheEntryLocker, MM_allocator_void, CacheEntryLocker_deleter> CacheEntryLockerPtr;

class CacheEntryLocker
{
    // For create
    friend class Cache;

    CacheEntryLocker(const CachePtr& cache, const CacheEntryBasePtr& entry);

    static CacheEntryLockerPtr create(const CachePtr& cache, const CacheEntryBasePtr& entry);


public:

    enum CacheEntryStatusEnum
    {
        // The entry is cached and may be retrieved from 
        eCacheEntryStatusCached,

        // The entry was not cached and must be computed by this thread
        // When computed it is expected that this thread calls CacheEntryLocker::insert()
        // when done.
        eCacheEntryStatusMustCompute,

        // The entry was not cached but another thread is already computing it
        // This thread should wait
        eCacheEntryStatusComputationPending
    };


    ~CacheEntryLocker();


    /**
     * @brief Returns the cache entry status
     **/
    CacheEntryStatusEnum getStatus() const;

    /**
     * @brief If the entry status was eCacheEntryStatusMustCompute, it should be called
     * to insert the results into the cache. This will also set the status to eCacheEntryStatusCached
     * and threads waiting for this entry will be woken up and should have it available.
     **/
    void insertInCache();

    /**
     * @brief If the status was eCacheEntryStatusComputationPending, this waits for the results
     * to be available by another thread that called insert(). 
     * If results are ready when woken up
     * the status will become eCacheEntryStatusCached and the entry passed to the constructor is ready.
     * If the status can still not be found in the cache and no other threads is computing it, the status
     * will become eCacheEntryStatusMustCompute and it is expected that this thread computes the entry in turn.
     **/
    CacheEntryStatusEnum waitForPendingEntry();


private:

    void lookupAndSetStatus(bool takeEntryLock);

    boost::scoped_ptr<CacheEntryLockerPrivate> _imp;
};


struct CachePrivate;
class Cache
: public QObject
, public boost::enable_shared_from_this<Cache>
{
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

    // For removeAllEntriesForPluginBlocking
    friend class CacheCleanerThread;

    // For allocTile, freeTile etc...
    friend class CacheImageStorage;

    // For notifyMemoryAllocated, etc...
    friend class ImageStorageBase;

    friend class CacheEntryLocker;
    friend class CacheBucket;
    
private:

    Cache();

public:

    /**
     * @brief Create a new instance of a cache. There should be a single Cache across the application as it
     * better keeps track of allocated resources.
     **/
    static CachePtr create();
    
    virtual ~Cache();

    /**
     * @brief Returns the absolute path to the cache directory
     **/
    std::string getCacheDirectoryPath() const;

    /**
     * @brief Returns the tile size (of one dimension) in pixels for the given bitdepth/
     **/
    static void getTileSizePx(ImageBitDepthEnum bitdepth, int *tx, int *ty);

    /**
     * @brief Set the maximum cache size available for the given storage.
     * Note that if shrinking, this will clear from the cache exceeding entries.
     **/
    void setMaximumCacheSize(StorageModeEnum storage, std::size_t size);

    /**
     * @brief Returns the maximum cache size for the given storage.
     **/
    std::size_t getMaximumCacheSize(StorageModeEnum storage) const;

    /**
     * @breif Returns the actual size taken in memory for the given storagE.
     **/
    std::size_t getCurrentSize(StorageModeEnum storage) const;

    /**
     * @brief Check if the given file exists on disk
     **/
    static bool fileExists(const std::string& filename);

    /**
     * @brief Look-up the cache for the given entry's key.
     * The entry is assumed to have its key set.
     * This function return a cache entry locker object that itself indicate
     * the CacheEntryLocker:CacheEntryStatusEnum of the entry. Depending
     * on the status the thread must either call CacheEntryLocker::getCachedEntry()
     * if results were found in cache, or compute the entry and then call CacheEntryLocker::insert()
     * or just wait for another thread that is already computing the entry with
     * CacheEntryLocker::waitForPendingEntry
     **/
    CacheEntryLockerPtr get(const CacheEntryBasePtr& entry) const;

    /**
     * @brief Clears the cache of its last recently used entries so at least nBytesToFree are available for the given storage.
     * This should be called before allocating any buffer in the application to ensure we do not hit the swap.
     *
     * This function is not blocking and it is not guaranteed that the memory is available when returning. 
     * Evicted entries will be deleted in a separate thread so this thread can continue its own work.
     **/
    void evictLRUEntries(std::size_t nBytesToFree, StorageModeEnum storage);

    /**
     * @brief Clear the cache of entries that can be purged.
     **/
    void clear();

    /**
     * @brief Removes all cache entries for the given pluginID.
     * @param blocking If true, this function will not return until all entries for the plug-in are removed from the cache,
     * otherwise they are removed from a separate thread.
     **/
    void removeAllEntriesForPlugin(const std::string& pluginID,  bool blocking);

    /**
     * @brief Removes this entry from the cache
     **/
    void removeEntry(const CacheEntryBasePtr& entry);

    /**
     * @brief Returns cache stats for each plug-in
     **/
    void getMemoryStats(std::map<std::string, CacheReportInfo>* infos) const;

    /**
     * @brief Returns all entries in the cache that have their getUniqueID() function 
     * returning an ID equal to the given one that thave cache signalling enabled.
     **/
    void getAllEntriesByKeyIDWithCacheSignalEnabled(int uniqueID, std::list<CacheEntryBasePtr>* entries) const;

    static int getBucketCacheBucketIndex(U64 hash);

Q_SIGNALS:

    /**
     * @brief Emitted whenever an entry that has its isCacheSignalRequired() return true is inserted or
     * removed from the cache. This is used for example to update the timeline when a cached texture
     * is inserted or removed.
     **/
    void cacheChanged();
    
private:

    /**
     * @brief Removes all cache entries for the given pluginID.
     * This function will not return until all entries for the plug-in are removed from the cache
     **/
    void removeAllEntriesForPluginBlocking(const std::string& pluginID);

    /**
     * @brief Mark one tile in the tiled memory mapped file as allocated. If not enough space is available, the file size 
     * will grow.
     **/
    std::size_t allocTile(int bucketIndex);

    /**
     * @brief Free a tile from the cache that was previously allocated with allocTile. It will be made available again for other entries.
     **/
    void freeTile(int bucketIndex, std::size_t cacheFileChunkIndex);

    /**
     * @brief Called when an entry is inserted from the cache. The bucket mutex may still be locked.
     **/
    void onEntryInsertedInCache(const CacheEntryBasePtr& entry);

    /**
     * @brief Called when an entry is allocated
     **/
    void notifyMemoryAllocated(std::size_t size, StorageModeEnum storage);

    /**
     * @brief Called when an entry is deallocated
     **/
    void notifyMemoryDeallocated(std::size_t size, StorageModeEnum storage);

private:

    boost::scoped_ptr<CachePrivate> _imp;

};



NATRON_NAMESPACE_EXIT;


#endif /*Engine_Cache_h_ */
