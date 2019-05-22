//
// Copyright (c) 2019 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "D3D12MemAlloc.h"

#include <mutex>
#include <atomic>
#include <algorithm>
#include <malloc.h> // for _aligned_malloc, _aligned_free

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//
// Configuration Begin
//
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

#ifndef D3D12MA_ASSERT
    #include <cassert>
    #define D3D12MA_ASSERT(cond) assert(cond)
#endif

// Assert that will be called very often, like inside data structures e.g. operator[].
// Making it non-empty can make program slow.
#ifndef D3D12MA_HEAVY_ASSERT
    #ifdef _DEBUG
        #define D3D12MA_HEAVY_ASSERT(expr)   //D3D12MA_ASSERT(expr)
    #else
        #define D3D12MA_HEAVY_ASSERT(expr)
    #endif
#endif

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//
// Configuration End
//
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////


namespace D3D12MA
{

////////////////////////////////////////////////////////////////////////////////
// Private globals - CPU memory allocation

static void* DefaultAllocate(size_t Size, size_t Alignment, void* /*pUserData*/)
{
    return _aligned_malloc(Size, Alignment);
}
static void DefaultFree(void* pMemory, void* /*pUserData*/)
{
    return _aligned_free(pMemory);
}

static void* Malloc(const ALLOCATION_CALLBACKS& Callbacks, size_t Size, size_t Alignment)
{
    return (*Callbacks.pAllocate)(Size, Alignment, Callbacks.pUserData);
}
static void Free(const ALLOCATION_CALLBACKS& Callbacks, void* pMemory)
{
    (*Callbacks.pFree)(pMemory, Callbacks.pUserData);
}

template<typename T>
static T* Allocate(const ALLOCATION_CALLBACKS& Callbacks)
{
    return (T*)Malloc(Callbacks, sizeof(T), __alignof(T));
}
template<typename T>
static T* AllocateArray(const ALLOCATION_CALLBACKS& Callbacks, size_t Count)
{
    return (T*)Malloc(Callbacks, sizeof(T) * Count, __alignof(T));
}

#define D3D12MA_NEW(Callbacks, Type) new(Allocate<Type>(Callbacks))(Type)
#define D3D12MA_NEW_ARRAY(Callbacks, Type, Count) new(AllocateArray<Type>((Callbacks), (Count)))(Type)

template<typename T>
static void D3D12MA_DELETE(const ALLOCATION_CALLBACKS& Callbacks, T* pMemory)
{
    pMemory->~T();
    Free(Callbacks, pMemory);
}
template<typename T>
static void D3D12MA_DELETE_ARRAY(const ALLOCATION_CALLBACKS& Callbacks, T* pMemory, size_t Count)
{
    if(pMemory)
    {
        for(size_t i = count; i--; )
        {
            pMemory[i].~T();
        }
        Free(Callbacks, pMemory);
    }
}

static void SetupAllocationCallbacks(ALLOCATION_CALLBACKS& outCallbacks, const ALLOCATOR_DESC& allocatorDesc)
{
    if(allocatorDesc.pAllocationCallbacks)
    {
        outCallbacks = *allocatorDesc.pAllocationCallbacks;
    }
    else
    {
        outCallbacks.pAllocate = &DefaultAllocate;
        outCallbacks.pFree = &DefaultFree;
        outCallbacks.pUserData = NULL;
    }
}

////////////////////////////////////////////////////////////////////////////////
// Private globals - basic facilities

template<typename T>
static inline T D3D12MA_MIN(const T& a, const T& b)
{
    return a <= b ? a : b;
}
template<typename T>
static inline T D3D12MA_MAX(const T& a, const T& b)
{
    return a <= b ? b : a;
}

#ifndef D3D12MA_MUTEX
    class Mutex
    {
    public:
        void Lock() { m_Mutex.lock(); }
        void Unlock() { m_Mutex.unlock(); }
    private:
        std::mutex m_Mutex;
    };
    #define D3D12MA_MUTEX Mutex
#endif

#if !defined(_WIN32) || !defined(WINVER) || WINVER < 0x0600
    #error Required at least WinAPI version supporting: client = Windows Vista, server = Windows Server 2008.
#endif

#ifndef D3D12MA_RW_MUTEX
    class D3D12MARWMutex
    {
    public:
        D3D12MARWMutex() { InitializeSRWLock(&m_Lock); }
        void LockRead() { AcquireSRWLockShared(&m_Lock); }
        void UnlockRead() { ReleaseSRWLockShared(&m_Lock); }
        void LockWrite() { AcquireSRWLockExclusive(&m_Lock); }
        void UnlockWrite() { ReleaseSRWLockExclusive(&m_Lock); }
    private:
        SRWLOCK m_Lock;
    };
    #define D3D12MA_RW_MUTEX D3D12MARWMutex
#endif

/*
If providing your own implementation, you need to implement a subset of std::atomic:

- Constructor(uint32_t desired)
- uint32_t load() const
- void store(uint32_t desired)
- bool compare_exchange_weak(uint32_t& expected, uint32_t desired)
*/
#ifndef D3D12MA_ATOMIC_UINT32
    #define D3D12MA_ATOMIC_UINT32 std::atomic<uint32_t>
#endif

#ifndef D3D12MA_DEBUG_ALWAYS_DEDICATED_MEMORY
    /*
    Every allocation will have its own memory block.
    Define to 1 for debugging purposes only.
    */
    #define D3D12MA_DEBUG_ALWAYS_DEDICATED_MEMORY (0)
#endif

#ifndef D3D12MA_DEBUG_ALIGNMENT
    /*
    Minimum alignment of all allocations, in bytes.
    Set to more than 1 for debugging purposes only. Must be power of two.
    */
    #define D3D12MA_DEBUG_ALIGNMENT (1)
#endif

#ifndef D3D12MA_DEBUG_GLOBAL_MUTEX
    /**
    Set this to 1 for debugging purposes only, to enable single mutex protecting all
    entry calls to the library. Can be useful for debugging multithreading issues.
    */
    #define D3D12MA_DEBUG_GLOBAL_MUTEX (0)
#endif

#ifndef D3D12MA_SMALL_HEAP_MAX_SIZE
   /// Maximum size of a memory heap in Vulkan to consider it "small".
   #define D3D12MA_SMALL_HEAP_MAX_SIZE (1024ull * 1024 * 1024)
#endif

#ifndef D3D12MA_DEFAULT_LARGE_HEAP_BLOCK_SIZE
   /// Default size of a block allocated as single VkDeviceMemory from a "large" heap.
   #define D3D12MA_DEFAULT_LARGE_HEAP_BLOCK_SIZE (256ull * 1024 * 1024)
#endif

// Returns number of bits set to 1 in (v).
static inline uint32_t BitsSet(uint32_t v)
{
	uint32_t c = v - ((v >> 1) & 0x55555555);
	c = ((c >>  2) & 0x33333333) + (c & 0x33333333);
	c = ((c >>  4) + c) & 0x0F0F0F0F;
	c = ((c >>  8) + c) & 0x00FF00FF;
	c = ((c >> 16) + c) & 0x0000FFFF;
	return c;
}

// Aligns given value up to nearest multiply of align value. For example: VmaAlignUp(11, 8) = 16.
// Use types like uint32_t, uint64_t as T.
template <typename T>
static inline T AlignUp(T val, T align)
{
	return (val + align - 1) / align * align;
}
// Aligns given value down to nearest multiply of align value. For example: VmaAlignUp(11, 8) = 8.
// Use types like uint32_t, uint64_t as T.
template <typename T>
static inline T AlignDown(T val, T align)
{
    return val / align * align;
}

// Division with mathematical rounding to nearest number.
template <typename T>
static inline T RoundDiv(T x, T y)
{
	return (x + (y / (T)2)) / y;
}

/*
Returns true if given number is a power of two.
T must be unsigned integer number or signed integer but always nonnegative.
For 0 returns true.
*/
template <typename T>
inline bool IsPow2(T x)
{
    return (x & (x-1)) == 0;
}

// Returns smallest power of 2 greater or equal to v.
static inline uint32_t NextPow2(uint32_t v)
{
	v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}
static inline uint64_t NextPow2(uint64_t v)
{
	v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    v++;
    return v;
}

// Returns largest power of 2 less or equal to v.
static inline uint32_t PrevPow2(uint32_t v)
{
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v = v ^ (v >> 1);
    return v;
}
static inline uint64_t PrevPow2(uint64_t v)
{
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    v = v ^ (v >> 1);
    return v;
}

static inline bool StrIsEmpty(const char* pStr)
{
    return pStr == NULL || *pStr == '\0';
}

// Helper RAII class to lock a mutex in constructor and unlock it in destructor (at the end of scope).
struct MutexLock
{
public:
    MutexLock(D3D12MA_MUTEX& mutex, bool useMutex = true) :
        m_pMutex(useMutex ? &mutex : NULL)
    {
        if(m_pMutex)
        {
            m_pMutex->Lock();
        }
    }
    ~MutexLock()
    {
        if(m_pMutex)
        {
            m_pMutex->Unlock();
        }
    }
private:
    D3D12MA_MUTEX* m_pMutex;

    D3D12MA_CLASS_NO_COPY(MutexLock)
};

// Helper RAII class to lock a RW mutex in constructor and unlock it in destructor (at the end of scope), for reading.
struct D3D12MAMutexLockRead
{
public:
    D3D12MAMutexLockRead(D3D12MA_RW_MUTEX& mutex, bool useMutex) :
        m_pMutex(useMutex ? &mutex : NULL)
    {
        if(m_pMutex)
        {
            m_pMutex->LockRead();
        }
    }
    ~D3D12MAMutexLockRead()
    {
        if(m_pMutex)
        {
            m_pMutex->UnlockRead();
        }
    }
private:
    D3D12MA_RW_MUTEX* m_pMutex;

    D3D12MA_CLASS_NO_COPY(D3D12MAMutexLockRead)
};

// Helper RAII class to lock a RW mutex in constructor and unlock it in destructor (at the end of scope), for writing.
struct D3D12MAMutexLockWrite
{
public:
    D3D12MAMutexLockWrite(D3D12MA_RW_MUTEX& mutex, bool useMutex) :
        m_pMutex(useMutex ? &mutex : NULL)
    {
        if(m_pMutex)
        {
            m_pMutex->LockWrite();
        }
    }
    ~D3D12MAMutexLockWrite()
    {
        if(m_pMutex)
        {
            m_pMutex->UnlockWrite();
        }
    }
private:
    D3D12MA_RW_MUTEX* m_pMutex;

    D3D12MA_CLASS_NO_COPY(D3D12MAMutexLockWrite)
};

#if D3D12MA_DEBUG_GLOBAL_MUTEX
    static D3D12MA_MUTEX g_DebugGlobalMutex;
    #define D3D12MA_DEBUG_GLOBAL_MUTEX_LOCK D3D12MAMutexLock debugGlobalMutexLock(g_DebugGlobalMutex, true);
#else
    #define D3D12MA_DEBUG_GLOBAL_MUTEX_LOCK
#endif

// Minimum size of a free suballocation to register it in the free suballocation collection.
static const UINT64 MIN_FREE_SUBALLOCATION_SIZE_TO_REGISTER = 16;

/*
Performs binary search and returns iterator to first element that is greater or
equal to (key), according to comparison (cmp).

Cmp should return true if first argument is less than second argument.

Returned value is the found element, if present in the collection or place where
new element with value (key) should be inserted.
*/
template <typename CmpLess, typename IterT, typename KeyT>
static IterT BinaryFindFirstNotLess(IterT beg, IterT end, const KeyT &key, CmpLess cmp)
{
    size_t down = 0, up = (end - beg);
    while(down < up)
    {
        const size_t mid = (down + up) / 2;
        if(cmp(*(beg+mid), key))
        {
            down = mid + 1;
        }
        else
        {
            up = mid;
        }
    }
    return beg + down;
}

////////////////////////////////////////////////////////////////////////////////
// Private class Vector

////////////////////////////////////////////////////////////////////////////////
// Private class AllocatorPimpl definition and implementation

class AllocatorPimpl
{
public:
    AllocatorPimpl(const ALLOCATION_CALLBACKS& allocationCallbacks, const ALLOCATOR_DESC& desc);
    HRESULT Init();
    ~AllocatorPimpl();

    const ALLOCATION_CALLBACKS& GetAllocationCallbacks() const { return m_AllocationCallbacks; }

private:
    UINT m_Flags; // ALLOCATOR_FLAGS
    ID3D12Device* m_Device;
    UINT64 m_PreferredLargeHeapBlockSize;
    ALLOCATION_CALLBACKS m_AllocationCallbacks;
};

AllocatorPimpl::AllocatorPimpl(const ALLOCATION_CALLBACKS& allocationCallbacks, const ALLOCATOR_DESC& desc) :
    m_Flags(desc.Flags),
    m_Device(desc.pDevice),
    m_PreferredLargeHeapBlockSize(desc.PreferredLargeHeapBlockSize),
    m_AllocationCallbacks(allocationCallbacks)
{
    // desc.pAllocationCallbacks intentionally ignored here, preprocessed by CreateAllocator.
}

HRESULT AllocatorPimpl::Init()
{
    return S_OK;
}

AllocatorPimpl::~AllocatorPimpl()
{
}

////////////////////////////////////////////////////////////////////////////////
// Public class Allocator implementation

Allocator::Allocator(const ALLOCATION_CALLBACKS& allocationCallbacks, const ALLOCATOR_DESC& desc) :
    m_Pimpl(D3D12MA_NEW(allocationCallbacks, AllocatorPimpl)(allocationCallbacks, desc))
{
}

Allocator::~Allocator()
{
    D3D12MA_DELETE(m_Pimpl->GetAllocationCallbacks(), m_Pimpl);
}

////////////////////////////////////////////////////////////////////////////////
// Public global functions

HRESULT CreateAllocator(const ALLOCATOR_DESC* pDesc, Allocator** ppAllocator)
{
    D3D12MA_ASSERT(pDesc);
    D3D12MA_ASSERT(pDesc->pDevice);
    D3D12MA_ASSERT(pDesc->PreferredLargeHeapBlockSize == 0 || (pDesc->PreferredLargeHeapBlockSize >= 16 && pDesc->PreferredLargeHeapBlockSize < 0x10000000000ull));

    ALLOCATION_CALLBACKS allocationCallbacks;
    SetupAllocationCallbacks(allocationCallbacks, *pDesc);

    *ppAllocator = D3D12MA_NEW(allocationCallbacks, Allocator)(allocationCallbacks, *pDesc);
    HRESULT hr = (*ppAllocator)->m_Pimpl->Init();
    if(FAILED(hr))
    {
        D3D12MA_DELETE(allocationCallbacks, *ppAllocator);
        *ppAllocator = NULL;
    }
    return hr;
}

void DestroyAllocator(Allocator* pAllocator)
{
    // Copy is needed because otherwise we would call destructor and invalidate the structure with callbacks ub before using it to free memory.
    const ALLOCATION_CALLBACKS allocationCallbacksCopy = pAllocator->m_Pimpl->GetAllocationCallbacks();
    D3D12MA_DELETE(allocationCallbacksCopy, pAllocator);
}

} // namespace D3D12MA
