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
#include <cstdlib>
#include <malloc.h> // for _aligned_malloc, _aligned_free

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
//
// Configuration Begin
//
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

#ifndef D3D12MA_DEBUG_MARGIN
    // Minimum margin before and after every allocation, in bytes.
    // Set nonzero for debugging purposes only.
    #define D3D12MA_DEBUG_MARGIN (0)
#endif

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

#define D3D12MA_NEW(Callbacks, Type) new(D3D12MA::Allocate<Type>(Callbacks))(Type)
#define D3D12MA_NEW_ARRAY(Callbacks, Type, Count) new(D3D12MA::AllocateArray<Type>((Callbacks), (Count)))(Type)

template<typename T>
static void D3D12MA_DELETE(const ALLOCATION_CALLBACKS& Callbacks, T* pMemory)
{
    if(pMemory)
    {
        pMemory->~T();
        Free(Callbacks, pMemory);
    }
}
template<typename T>
static void D3D12MA_DELETE_ARRAY(const ALLOCATION_CALLBACKS& Callbacks, T* pMemory, size_t Count)
{
    if(pMemory)
    {
        for(size_t i = Count; i--; )
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

#define D3D12MA_VALIDATE(cond) do { if(!(cond)) { \
        D3D12MA_ASSERT(0 && "Validation failed: " #cond); \
        return false; \
    } } while(false)

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

template<typename T>
static inline void D3D12MA_SWAP(T& a, T& b)
{
    T tmp = a; a = b; b = tmp;
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
    class RWMutex
    {
    public:
        RWMutex() { InitializeSRWLock(&m_Lock); }
        void LockRead() { AcquireSRWLockShared(&m_Lock); }
        void UnlockRead() { ReleaseSRWLockShared(&m_Lock); }
        void LockWrite() { AcquireSRWLockExclusive(&m_Lock); }
        void UnlockWrite() { ReleaseSRWLockExclusive(&m_Lock); }
    private:
        SRWLOCK m_Lock;
    };
    #define D3D12MA_RW_MUTEX RWMutex
#endif

/*
If providing your own implementation, you need to implement a subset of std::atomic:

- Constructor(UINT desired)
- UINT load() const
- void store(UINT desired)
- bool compare_exchange_weak(UINT& expected, UINT desired)
*/
#ifndef D3D12MA_ATOMIC_UINT32
    #define D3D12MA_ATOMIC_UINT32 std::atomic<UINT>
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

#ifndef D3D12MA_DEFAULT_BLOCK_SIZE
   /// Default size of a block allocated as single ID3D12Heap.
   #define D3D12MA_DEFAULT_BLOCK_SIZE (256ull * 1024 * 1024)
#endif

// Returns number of bits set to 1 in (v).
static inline UINT BitsSet(UINT v)
{
	UINT c = v - ((v >> 1) & 0x55555555);
	c = ((c >>  2) & 0x33333333) + (c & 0x33333333);
	c = ((c >>  4) + c) & 0x0F0F0F0F;
	c = ((c >>  8) + c) & 0x00FF00FF;
	c = ((c >> 16) + c) & 0x0000FFFF;
	return c;
}

// Aligns given value up to nearest multiply of align value. For example: AlignUp(11, 8) = 16.
// Use types like UINT, uint64_t as T.
template <typename T>
static inline T AlignUp(T val, T align)
{
	return (val + align - 1) / align * align;
}
// Aligns given value down to nearest multiply of align value. For example: AlignUp(11, 8) = 8.
// Use types like UINT, uint64_t as T.
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
static inline UINT NextPow2(UINT v)
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
static inline UINT PrevPow2(UINT v)
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
struct MutexLockRead
{
public:
    MutexLockRead(D3D12MA_RW_MUTEX& mutex, bool useMutex) :
        m_pMutex(useMutex ? &mutex : NULL)
    {
        if(m_pMutex)
        {
            m_pMutex->LockRead();
        }
    }
    ~MutexLockRead()
    {
        if(m_pMutex)
        {
            m_pMutex->UnlockRead();
        }
    }
private:
    D3D12MA_RW_MUTEX* m_pMutex;

    D3D12MA_CLASS_NO_COPY(MutexLockRead)
};

// Helper RAII class to lock a RW mutex in constructor and unlock it in destructor (at the end of scope), for writing.
struct MutexLockWrite
{
public:
    MutexLockWrite(D3D12MA_RW_MUTEX& mutex, bool useMutex) :
        m_pMutex(useMutex ? &mutex : NULL)
    {
        if(m_pMutex)
        {
            m_pMutex->LockWrite();
        }
    }
    ~MutexLockWrite()
    {
        if(m_pMutex)
        {
            m_pMutex->UnlockWrite();
        }
    }
private:
    D3D12MA_RW_MUTEX* m_pMutex;

    D3D12MA_CLASS_NO_COPY(MutexLockWrite)
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
static IterT BinaryFindFirstNotLess(IterT beg, IterT end, const KeyT &key, const CmpLess& cmp)
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

template<typename CmpLess, typename IterT, typename KeyT>
IterT BinaryFindSorted(const IterT& beg, const IterT& end, const KeyT& value, const CmpLess& cmp)
{
    IterT it = BinaryFindFirstNotLess<CmpLess, IterT, KeyT>(beg, end, value, cmp);
    if(it == end ||
        (!cmp(*it, value) && !cmp(value, *it)))
    {
        return it;
    }
    return end;
}

struct PointerLess
{
    bool operator()(const void* lhs, const void* rhs) const
    {
        return lhs < rhs;
    }
};

static const UINT HEAP_TYPE_COUNT = 3;

static UINT HeapTypeToIndex(D3D12_HEAP_TYPE type)
{
    switch(type)
    {
    case D3D12_HEAP_TYPE_DEFAULT:  return 0;
    case D3D12_HEAP_TYPE_UPLOAD:   return 1;
    case D3D12_HEAP_TYPE_READBACK: return 2;
    default: D3D12MA_ASSERT(0); return (UINT)-1;
    }
}

////////////////////////////////////////////////////////////////////////////////
// Private class Vector

/*
Dynamically resizing continuous array. Class with interface similar to std::vector.
T must be POD because constructors and destructors are not called and memcpy is
used for these objects.
*/
template<typename T>
class Vector
{
public:
    typedef T value_type;

    // allocationCallbacks externally owned, must outlive this object.
    Vector(const ALLOCATION_CALLBACKS& allocationCallbacks) :
        m_AllocationCallbacks(allocationCallbacks),
        m_pArray(NULL),
        m_Count(0),
        m_Capacity(0)
    {
    }

    Vector(size_t count, const ALLOCATION_CALLBACKS& allocationCallbacks) :
        m_AllocationCallbacks(allocationCallbacks),
        m_pArray(count ? AllocateArray<T>(allocationCallbacks, count) : NULL),
        m_Count(count),
        m_Capacity(count)
    {
    }

    Vector(const Vector<T>& src) :
        m_AllocationCallbacks(src.m_AllocationCallbacks),
        m_pArray(src.m_Count ? AllocateArray<T>(src.m_AllocationCallbacks, src.m_Count) : NULL),
        m_Count(src.m_Count),
        m_Capacity(src.m_Count)
    {
        if(m_Count > 0)
        {
            memcpy(m_pArray, src.m_pArray, m_Count * sizeof(T));
        }
    }

    ~Vector()
    {
        Free(m_AllocationCallbacks, m_pArray);
    }

    Vector& operator=(const Vector<T>& rhs)
    {
        if(&rhs != this)
        {
            resize(rhs.m_Count);
            if(m_Count != 0)
            {
                memcpy(m_pArray, rhs.m_pArray, m_Count * sizeof(T));
            }
        }
        return *this;
    }

    bool empty() const { return m_Count == 0; }
    size_t size() const { return m_Count; }
    T* data() { return m_pArray; }
    const T* data() const { return m_pArray; }

    T& operator[](size_t index)
    {
        D3D12MA_HEAVY_ASSERT(index < m_Count);
        return m_pArray[index];
    }
    const T& operator[](size_t index) const
    {
        D3D12MA_HEAVY_ASSERT(index < m_Count);
        return m_pArray[index];
    }

    T& front()
    {
        D3D12MA_HEAVY_ASSERT(m_Count > 0);
        return m_pArray[0];
    }
    const T& front() const
    {
        D3D12MA_HEAVY_ASSERT(m_Count > 0);
        return m_pArray[0];
    }
    T& back()
    {
        D3D12MA_HEAVY_ASSERT(m_Count > 0);
        return m_pArray[m_Count - 1];
    }
    const T& back() const
    {
        D3D12MA_HEAVY_ASSERT(m_Count > 0);
        return m_pArray[m_Count - 1];
    }

    void reserve(size_t newCapacity, bool freeMemory = false)
    {
        newCapacity = D3D12MA_MAX(newCapacity, m_Count);

        if((newCapacity < m_Capacity) && !freeMemory)
        {
            newCapacity = m_Capacity;
        }

        if(newCapacity != m_Capacity)
        {
            T* const newArray = newCapacity ? AllocateArray<T>(m_AllocationCallbacks, newCapacity) : NULL;
            if(m_Count != 0)
            {
                memcpy(newArray, m_pArray, m_Count * sizeof(T));
            }
            Free(m_Allocator.m_pCallbacks, m_pArray);
            m_Capacity = newCapacity;
            m_pArray = newArray;
        }
    }

    void resize(size_t newCount, bool freeMemory = false)
    {
        size_t newCapacity = m_Capacity;
        if(newCount > m_Capacity)
        {
            newCapacity = D3D12MA_MAX(newCount, D3D12MA_MAX(m_Capacity * 3 / 2, (size_t)8));
        }
        else if(freeMemory)
        {
            newCapacity = newCount;
        }

        if(newCapacity != m_Capacity)
        {
            T* const newArray = newCapacity ? AllocateArray<T>(m_AllocationCallbacks, newCapacity) : NULL;
            const size_t elementsToCopy = D3D12MA_MIN(m_Count, newCount);
            if(elementsToCopy != 0)
            {
                memcpy(newArray, m_pArray, elementsToCopy * sizeof(T));
            }
            Free(m_AllocationCallbacks, m_pArray);
            m_Capacity = newCapacity;
            m_pArray = newArray;
        }

        m_Count = newCount;
    }

    void clear(bool freeMemory = false)
    {
        resize(0, freeMemory);
    }

    void insert(size_t index, const T& src)
    {
        D3D12MA_HEAVY_ASSERT(index <= m_Count);
        const size_t oldCount = size();
        resize(oldCount + 1);
        if(index < oldCount)
        {
            memmove(m_pArray + (index + 1), m_pArray + index, (oldCount - index) * sizeof(T));
        }
        m_pArray[index] = src;
    }

    void remove(size_t index)
    {
        D3D12MA_HEAVY_ASSERT(index < m_Count);
        const size_t oldCount = size();
        if(index < oldCount - 1)
        {
            memmove(m_pArray + index, m_pArray + (index + 1), (oldCount - index - 1) * sizeof(T));
        }
        resize(oldCount - 1);
    }

    void push_back(const T& src)
    {
        const size_t newIndex = size();
        resize(newIndex + 1);
        m_pArray[newIndex] = src;
    }

    void pop_back()
    {
        D3D12MA_HEAVY_ASSERT(m_Count > 0);
        resize(size() - 1);
    }

    void push_front(const T& src)
    {
        insert(0, src);
    }

    void pop_front()
    {
        D3D12MA_HEAVY_ASSERT(m_Count > 0);
        remove(0);
    }

    typedef T* iterator;

    iterator begin() { return m_pArray; }
    iterator end() { return m_pArray + m_Count; }

    template<typename CmpLess>
    size_t InsertSorted(const T& value, const CmpLess& cmp)
    {
        const size_t indexToInsert = BinaryFindFirstNotLess<CmpLess, iterator, T>(
            m_pArray,
            m_pArray + m_Count,
            value,
            cmp) - m_pArray;
        insert(indexToInsert, value);
        return indexToInsert;
    }

    template<typename CmpLess>
    bool RemoveSorted(const T& value, const CmpLess& cmp)
    {
        const iterator it = BinaryFindFirstNotLess(
            m_pArray,
            m_pArray + m_Count,
            value,
            cmp);
        if((it != end()) && !cmp(*it, value) && !cmp(value, *it))
        {
            size_t indexToRemove = it - begin();
            remove(indexToRemove);
            return true;
        }
        return false;
    }

private:
    const ALLOCATION_CALLBACKS& m_AllocationCallbacks;
    T* m_pArray;
    size_t m_Count;
    size_t m_Capacity;
};

////////////////////////////////////////////////////////////////////////////////
// Private class PoolAllocator

/*
Allocator for objects of type T using a list of arrays (pools) to speed up
allocation. Number of elements that can be allocated is not bounded because
allocator can create multiple blocks.
T should be POD because constructor and destructor is not called in Alloc or
Free.
*/
template<typename T>
class PoolAllocator
{
    D3D12MA_CLASS_NO_COPY(PoolAllocator)
public:
    // allocationCallbacks externally owned, must outlive this object.
    PoolAllocator(const ALLOCATION_CALLBACKS& allocationCallbacks, UINT firstBlockCapacity);
    ~PoolAllocator() { Clear(); }
    void Clear();
    T* Alloc();
    void Free(T* ptr);

private:
    union Item
    {
        UINT NextFreeIndex; // UINT32_MAX means end of list.
        T Value;
    };

    struct ItemBlock
    {
        Item* pItems;
        UINT Capacity;
        UINT FirstFreeIndex;
    };

    const ALLOCATION_CALLBACKS& m_AllocationCallbacks;
    const UINT m_FirstBlockCapacity;
    Vector<ItemBlock> m_ItemBlocks;

    ItemBlock& CreateNewBlock();
};

template<typename T>
PoolAllocator<T>::PoolAllocator(const ALLOCATION_CALLBACKS& allocationCallbacks, UINT firstBlockCapacity) :
    m_AllocationCallbacks(allocationCallbacks),
    m_FirstBlockCapacity(firstBlockCapacity),
    m_ItemBlocks(allocationCallbacks)
{
    D3D12MA_ASSERT(m_FirstBlockCapacity > 1);
}

template<typename T>
void PoolAllocator<T>::Clear()
{
    for(size_t i = m_ItemBlocks.size(); i--; )
    {
        D3D12MA_DELETE_ARRAY(m_AllocationCallbacks, m_ItemBlocks[i].pItems, m_ItemBlocks[i].Capacity);
    }
    m_ItemBlocks.clear(true);
}

template<typename T>
T* PoolAllocator<T>::Alloc()
{
    for(size_t i = m_ItemBlocks.size(); i--; )
    {
        ItemBlock& block = m_ItemBlocks[i];
        // This block has some free items: Use first one.
        if(block.FirstFreeIndex != UINT32_MAX)
        {
            Item* const pItem = &block.pItems[block.FirstFreeIndex];
            block.FirstFreeIndex = pItem->NextFreeIndex;
            return &pItem->Value;
        }
    }

    // No block has free item: Create new one and use it.
    ItemBlock& newBlock = CreateNewBlock();
    Item* const pItem = &newBlock.pItems[0];
    newBlock.FirstFreeIndex = pItem->NextFreeIndex;
    return &pItem->Value;
}

template<typename T>
void PoolAllocator<T>::Free(T* ptr)
{
    // Search all memory blocks to find ptr.
    for(size_t i = m_ItemBlocks.size(); i--; )
    {
        ItemBlock& block = m_ItemBlocks[i];

        Item* pItemPtr;
        memcpy(&pItemPtr, &ptr, sizeof(pItemPtr));

        // Check if pItemPtr is in address range of this block.
        if((pItemPtr >= block.pItems) && (pItemPtr < block.pItems + block.Capacity))
        {
            const UINT index = static_cast<UINT>(pItemPtr - block.pItems);
            pItemPtr->NextFreeIndex = block.FirstFreeIndex;
            block.FirstFreeIndex = index;
            return;
        }
    }
    D3D12MA_ASSERT(0 && "Pointer doesn't belong to this memory pool.");
}

template<typename T>
typename PoolAllocator<T>::ItemBlock& PoolAllocator<T>::CreateNewBlock()
{
    const UINT newBlockCapacity = m_ItemBlocks.empty() ?
        m_FirstBlockCapacity : m_ItemBlocks.back().Capacity * 3 / 2;

    const ItemBlock newBlock = {
        D3D12MA_NEW_ARRAY(m_AllocationCallbacks, Item, newBlockCapacity),
        newBlockCapacity,
        0 };

    m_ItemBlocks.push_back(newBlock);

    // Setup singly-linked list of all free items in this block.
    for(UINT i = 0; i < newBlockCapacity - 1; ++i)
    {
        newBlock.pItems[i].NextFreeIndex = i + 1;
    }
    newBlock.pItems[newBlockCapacity - 1].NextFreeIndex = UINT32_MAX;
    return m_ItemBlocks.back();
}

////////////////////////////////////////////////////////////////////////////////
// Private class List

/*
Doubly linked list, with elements allocated out of PoolAllocator.
Has custom interface, as well as STL-style interface, including iterator and
const_iterator.
*/
template<typename T>
class List
{
    D3D12MA_CLASS_NO_COPY(List)
public:
    struct Item
    {
        Item* pPrev;
        Item* pNext;
        T Value;
    };

    // allocationCallbacks externally owned, must outlive this object.
    List(const ALLOCATION_CALLBACKS& allocationCallbacks);
    ~List();
    void Clear();

    size_t GetCount() const { return m_Count; }
    bool IsEmpty() const { return m_Count == 0; }

    Item* Front() { return m_pFront; }
    const Item* Front() const { return m_pFront; }
    Item* Back() { return m_pBack; }
    const Item* Back() const { return m_pBack; }

    Item* PushBack();
    Item* PushFront();
    Item* PushBack(const T& value);
    Item* PushFront(const T& value);
    void PopBack();
    void PopFront();

    // Item can be null - it means PushBack.
    Item* InsertBefore(Item* pItem);
    // Item can be null - it means PushFront.
    Item* InsertAfter(Item* pItem);

    Item* InsertBefore(Item* pItem, const T& value);
    Item* InsertAfter(Item* pItem, const T& value);

    void Remove(Item* pItem);

    class iterator
    {
    public:
        iterator() :
            m_pList(NULL),
            m_pItem(NULL)
        {
        }

        T& operator*() const
        {
            D3D12MA_HEAVY_ASSERT(m_pItem != NULL);
            return m_pItem->Value;
        }
        T* operator->() const
        {
            D3D12MA_HEAVY_ASSERT(m_pItem != NULL);
            return &m_pItem->Value;
        }

        iterator& operator++()
        {
            D3D12MA_HEAVY_ASSERT(m_pItem != NULL);
            m_pItem = m_pItem->pNext;
            return *this;
        }
        iterator& operator--()
        {
            if(m_pItem != NULL)
            {
                m_pItem = m_pItem->pPrev;
            }
            else
            {
                D3D12MA_HEAVY_ASSERT(!m_pList->IsEmpty());
                m_pItem = m_pList->Back();
            }
            return *this;
        }

        iterator operator++(int)
        {
            iterator result = *this;
            ++*this;
            return result;
        }
        iterator operator--(int)
        {
            iterator result = *this;
            --*this;
            return result;
        }

        bool operator==(const iterator& rhs) const
        {
            D3D12MA_HEAVY_ASSERT(m_pList == rhs.m_pList);
            return m_pItem == rhs.m_pItem;
        }
        bool operator!=(const iterator& rhs) const
        {
            D3D12MA_HEAVY_ASSERT(m_pList == rhs.m_pList);
            return m_pItem != rhs.m_pItem;
        }

    private:
        List<T>* m_pList;
        List<T>::Item* m_pItem;

        iterator(List<T>* pList, List<T>::Item* pItem) :
            m_pList(pList),
            m_pItem(pItem)
        {
        }

        friend class List<T>;
    };

    class const_iterator
    {
    public:
        const_iterator() :
            m_pList(NULL),
            m_pItem(NULL)
        {
        }

        const_iterator(const iterator& src) :
            m_pList(src.m_pList),
            m_pItem(src.m_pItem)
        {
        }

        const T& operator*() const
        {
            D3D12MA_HEAVY_ASSERT(m_pItem != NULL);
            return m_pItem->Value;
        }
        const T* operator->() const
        {
            D3D12MA_HEAVY_ASSERT(m_pItem != NULL);
            return &m_pItem->Value;
        }

        const_iterator& operator++()
        {
            D3D12MA_HEAVY_ASSERT(m_pItem != NULL);
            m_pItem = m_pItem->pNext;
            return *this;
        }
        const_iterator& operator--()
        {
            if(m_pItem != NULL)
            {
                m_pItem = m_pItem->pPrev;
            }
            else
            {
                D3D12MA_HEAVY_ASSERT(!m_pList->IsEmpty());
                m_pItem = m_pList->Back();
            }
            return *this;
        }

        const_iterator operator++(int)
        {
            const_iterator result = *this;
            ++*this;
            return result;
        }
        const_iterator operator--(int)
        {
            const_iterator result = *this;
            --*this;
            return result;
        }

        bool operator==(const const_iterator& rhs) const
        {
            D3D12MA_HEAVY_ASSERT(m_pList == rhs.m_pList);
            return m_pItem == rhs.m_pItem;
        }
        bool operator!=(const const_iterator& rhs) const
        {
            D3D12MA_HEAVY_ASSERT(m_pList == rhs.m_pList);
            return m_pItem != rhs.m_pItem;
        }

    private:
        const_iterator(const List<T>* pList, const List<T>::Item* pItem) :
            m_pList(pList),
            m_pItem(pItem)
        {
        }

        const List<T>* m_pList;
        const List<T>::Item* m_pItem;

        friend class List<T>;
    };

    bool empty() const { return IsEmpty(); }
    size_t size() const { return GetCount(); }

    iterator begin() { return iterator(this, Front()); }
    iterator end() { return iterator(this, NULL); }

    const_iterator cbegin() const { return const_iterator(this, Front()); }
    const_iterator cend() const { return const_iterator(this, NULL); }

    void clear() { Clear(); }
    void push_back(const T& value) { PushBack(value); }
    void erase(iterator it) { Remove(it.m_pItem); }
    iterator insert(iterator it, const T& value) { return iterator(this, InsertBefore(it.m_pItem, value)); }

private:
    const ALLOCATION_CALLBACKS& m_AllocationCallbacks;
    PoolAllocator<Item> m_ItemAllocator;
    Item* m_pFront;
    Item* m_pBack;
    size_t m_Count;
};

template<typename T>
List<T>::List(const ALLOCATION_CALLBACKS& allocationCallbacks) :
    m_AllocationCallbacks(allocationCallbacks),
    m_ItemAllocator(allocationCallbacks, 128),
    m_pFront(NULL),
    m_pBack(NULL),
    m_Count(0)
{
}

template<typename T>
List<T>::~List()
{
    // Intentionally not calling Clear, because that would be unnecessary
    // computations to return all items to m_ItemAllocator as free.
}

template<typename T>
void List<T>::Clear()
{
    if(!IsEmpty())
    {
        Item* pItem = m_pBack;
        while(pItem != NULL)
        {
            Item* const pPrevItem = pItem->pPrev;
            m_ItemAllocator.Free(pItem);
            pItem = pPrevItem;
        }
        m_pFront = NULL;
        m_pBack = NULL;
        m_Count = 0;
    }
}

template<typename T>
typename List<T>::Item* List<T>::PushBack()
{
    Item* const pNewItem = m_ItemAllocator.Alloc();
    pNewItem->pNext = NULL;
    if(IsEmpty())
    {
        pNewItem->pPrev = NULL;
        m_pFront = pNewItem;
        m_pBack = pNewItem;
        m_Count = 1;
    }
    else
    {
        pNewItem->pPrev = m_pBack;
        m_pBack->pNext = pNewItem;
        m_pBack = pNewItem;
        ++m_Count;
    }
    return pNewItem;
}

template<typename T>
typename List<T>::Item* List<T>::PushFront()
{
    Item* const pNewItem = m_ItemAllocator.Alloc();
    pNewItem->pPrev = NULL;
    if(IsEmpty())
    {
        pNewItem->pNext = NULL;
        m_pFront = pNewItem;
        m_pBack = pNewItem;
        m_Count = 1;
    }
    else
    {
        pNewItem->pNext = m_pFront;
        m_pFront->pPrev = pNewItem;
        m_pFront = pNewItem;
        ++m_Count;
    }
    return pNewItem;
}

template<typename T>
typename List<T>::Item* List<T>::PushBack(const T& value)
{
    Item* const pNewItem = PushBack();
    pNewItem->Value = value;
    return pNewItem;
}

template<typename T>
typename List<T>::Item* List<T>::PushFront(const T& value)
{
    Item* const pNewItem = PushFront();
    pNewItem->Value = value;
    return pNewItem;
}

template<typename T>
void List<T>::PopBack()
{
    D3D12MA_HEAVY_ASSERT(m_Count > 0);
    Item* const pBackItem = m_pBack;
    Item* const pPrevItem = pBackItem->pPrev;
    if(pPrevItem != NULL)
    {
        pPrevItem->pNext = NULL;
    }
    m_pBack = pPrevItem;
    m_ItemAllocator.Free(pBackItem);
    --m_Count;
}

template<typename T>
void List<T>::PopFront()
{
    D3D12MA_HEAVY_ASSERT(m_Count > 0);
    Item* const pFrontItem = m_pFront;
    Item* const pNextItem = pFrontItem->pNext;
    if(pNextItem != NULL)
    {
        pNextItem->pPrev = NULL;
    }
    m_pFront = pNextItem;
    m_ItemAllocator.Free(pFrontItem);
    --m_Count;
}

template<typename T>
void List<T>::Remove(Item* pItem)
{
    D3D12MA_HEAVY_ASSERT(pItem != NULL);
    D3D12MA_HEAVY_ASSERT(m_Count > 0);

    if(pItem->pPrev != NULL)
    {
        pItem->pPrev->pNext = pItem->pNext;
    }
    else
    {
        D3D12MA_HEAVY_ASSERT(m_pFront == pItem);
        m_pFront = pItem->pNext;
    }

    if(pItem->pNext != NULL)
    {
        pItem->pNext->pPrev = pItem->pPrev;
    }
    else
    {
        D3D12MA_HEAVY_ASSERT(m_pBack == pItem);
        m_pBack = pItem->pPrev;
    }

    m_ItemAllocator.Free(pItem);
    --m_Count;
}

template<typename T>
typename List<T>::Item* List<T>::InsertBefore(Item* pItem)
{
    if(pItem != NULL)
    {
        Item* const prevItem = pItem->pPrev;
        Item* const newItem = m_ItemAllocator.Alloc();
        newItem->pPrev = prevItem;
        newItem->pNext = pItem;
        pItem->pPrev = newItem;
        if(prevItem != NULL)
        {
            prevItem->pNext = newItem;
        }
        else
        {
            D3D12MA_HEAVY_ASSERT(m_pFront == pItem);
            m_pFront = newItem;
        }
        ++m_Count;
        return newItem;
    }
    else
    {
        return PushBack();
    }
}

template<typename T>
typename List<T>::Item* List<T>::InsertAfter(Item* pItem)
{
    if(pItem != NULL)
    {
        Item* const nextItem = pItem->pNext;
        Item* const newItem = m_ItemAllocator.Alloc();
        newItem->pNext = nextItem;
        newItem->pPrev = pItem;
        pItem->pNext = newItem;
        if(nextItem != NULL)
        {
            nextItem->pPrev = newItem;
        }
        else
        {
            D3D12MA_HEAVY_ASSERT(m_pBack == pItem);
            m_pBack = newItem;
        }
        ++m_Count;
        return newItem;
    }
    else
        return PushFront();
}

template<typename T>
typename List<T>::Item* List<T>::InsertBefore(List<T>::Item* pItem, const T& value)
{
    Item* const newItem = InsertBefore(pItem);
    newItem->Value = value;
    return newItem;
}

template<typename T>
typename List<T>::Item* List<T>::InsertAfter(List<T>::Item* pItem, const T& value)
{
    Item* const newItem = InsertAfter(pItem);
    newItem->Value = value;
    return newItem;
}

////////////////////////////////////////////////////////////////////////////////
// Private class BlockMetadata and derived classes - declarations

enum SuballocationType
{
    SUBALLOCATION_TYPE_FREE = 0,
    SUBALLOCATION_TYPE_ALLOCATION = 1,
};

/*
Represents a region of DeviceMemoryBlock that is either assigned and returned as
allocated memory block or free.
*/
struct Suballocation
{
    UINT64 offset;
    UINT64 size;
    Allocation* allocation;
    SuballocationType type;
};

// Comparator for offsets.
struct VmaSuballocationOffsetLess
{
    bool operator()(const Suballocation& lhs, const Suballocation& rhs) const
    {
        return lhs.offset < rhs.offset;
    }
};
struct VmaSuballocationOffsetGreater
{
    bool operator()(const Suballocation& lhs, const Suballocation& rhs) const
    {
        return lhs.offset > rhs.offset;
    }
};

typedef List<Suballocation> SuballocationList;

struct SuballocationItemSizeLess
{
    bool operator()(const SuballocationList::iterator lhs, const SuballocationList::iterator rhs) const
    {
        return lhs->size < rhs->size;
    }
    bool operator()(const SuballocationList::iterator lhs, UINT64 rhsSize) const
    {
        return lhs->size < rhsSize;
    }
};

enum class AllocationRequestType
{
    Normal,
    // Used by "Linear" algorithm.
    UpperAddress,
    EndOf1st,
    EndOf2nd,
};

// Cost of one additional allocation lost, as equivalent in bytes.
static const UINT64 VMA_LOST_ALLOCATION_COST = 1048576;

/*
Parameters of planned allocation inside a DeviceMemoryBlock.

If canMakeOtherLost was false:
- item points to a FREE suballocation.
- itemsToMakeLostCount is 0.

If canMakeOtherLost was true:
- item points to first of sequence of suballocations, which are either FREE,
or point to Allocations that can become lost.
- itemsToMakeLostCount is the number of Allocations that need to be made lost for
the requested allocation to succeed.
*/
struct VmaAllocationRequest
{
    UINT64 offset;
    UINT64 sumFreeSize; // Sum size of free items that overlap with proposed allocation.
    UINT64 sumItemSize; // Sum size of items to make lost that overlap with proposed allocation.
    SuballocationList::iterator item;
    size_t itemsToMakeLostCount;
    void* customData;
    AllocationRequestType type;

    UINT64 CalcCost() const
    {
        return sumItemSize + itemsToMakeLostCount * VMA_LOST_ALLOCATION_COST;
    }
};

/*
Data structure used for bookkeeping of allocations and unused ranges of memory
in a single ID3D12Heap memory block.
*/
class BlockMetadata
{
public:
    BlockMetadata(const ALLOCATION_CALLBACKS* allocationCallbacks);
    virtual ~BlockMetadata() { }
    virtual void Init(UINT64 size) { m_Size = size; }

    // Validates all data structures inside this object. If not valid, returns false.
    virtual bool Validate() const = 0;
    UINT64 GetSize() const { return m_Size; }
    virtual size_t GetAllocationCount() const = 0;
    virtual UINT64 GetSumFreeSize() const = 0;
    virtual UINT64 GetUnusedRangeSizeMax() const = 0;
    // Returns true if this block is empty - contains only single free suballocation.
    virtual bool IsEmpty() const = 0;

    // Tries to find a place for suballocation with given parameters inside this block.
    // If succeeded, fills pAllocationRequest and returns true.
    // If failed, returns false.
    virtual bool CreateAllocationRequest(
        UINT currentFrameIndex,
        UINT frameInUseCount,
        UINT64 allocSize,
        UINT64 allocAlignment,
        bool upperAddress,
        bool canMakeOtherLost,
        // Always one of VMA_ALLOCATION_CREATE_STRATEGY_* or VMA_ALLOCATION_INTERNAL_STRATEGY_* flags. TODO
        UINT strategy,
        VmaAllocationRequest* pAllocationRequest) = 0;

    // Makes actual allocation based on request. Request must already be checked and valid.
    virtual void Alloc(
        const VmaAllocationRequest& request,
        UINT64 allocSize,
        Allocation* Allocation) = 0;

    // Frees suballocation assigned to given memory region.
    virtual void Free(const Allocation* allocation) = 0;
    virtual void FreeAtOffset(UINT64 offset) = 0;

protected:
    const ALLOCATION_CALLBACKS* GetAllocs() const { return m_pAllocationCallbacks; }

private:
    UINT64 m_Size;
    const ALLOCATION_CALLBACKS* m_pAllocationCallbacks;

    D3D12MA_CLASS_NO_COPY(BlockMetadata);
};

class BlockMetadata_Generic : public BlockMetadata
{
public:
    BlockMetadata_Generic(const ALLOCATION_CALLBACKS* allocationCallbacks);
    virtual ~BlockMetadata_Generic();
    virtual void Init(UINT64 size);

    virtual bool Validate() const;
    virtual size_t GetAllocationCount() const { return m_Suballocations.size() - m_FreeCount; }
    virtual UINT64 GetSumFreeSize() const { return m_SumFreeSize; }
    virtual UINT64 GetUnusedRangeSizeMax() const;
    virtual bool IsEmpty() const;

    virtual bool CreateAllocationRequest(
        UINT currentFrameIndex,
        UINT frameInUseCount,
        UINT64 allocSize,
        UINT64 allocAlignment,
        bool upperAddress,
        bool canMakeOtherLost,
        UINT strategy,
        VmaAllocationRequest* pAllocationRequest);

    virtual void Alloc(
        const VmaAllocationRequest& request,
        UINT64 allocSize,
        Allocation* hAllocation);

    virtual void Free(const Allocation* allocation);
    virtual void FreeAtOffset(UINT64 offset);

private:
    UINT m_FreeCount;
    UINT64 m_SumFreeSize;
    SuballocationList m_Suballocations;
    // Suballocations that are free and have size greater than certain threshold.
    // Sorted by size, ascending.
    Vector<SuballocationList::iterator> m_FreeSuballocationsBySize;

    bool ValidateFreeSuballocationList() const;

    // Checks if requested suballocation with given parameters can be placed in given pFreeSuballocItem.
    // If yes, fills pOffset and returns true. If no, returns false.
    bool CheckAllocation(
        UINT currentFrameIndex,
        UINT frameInUseCount,
        UINT64 allocSize,
        UINT64 allocAlignment,
        SuballocationList::const_iterator suballocItem,
        bool canMakeOtherLost,
        UINT64* pOffset,
        size_t* itemsToMakeLostCount,
        UINT64* pSumFreeSize,
        UINT64* pSumItemSize) const;
    // Given free suballocation, it merges it with following one, which must also be free.
    void MergeFreeWithNext(SuballocationList::iterator item);
    // Releases given suballocation, making it free.
    // Merges it with adjacent free suballocations if applicable.
    // Returns iterator to new free suballocation at this place.
    SuballocationList::iterator FreeSuballocation(SuballocationList::iterator suballocItem);
    // Given free suballocation, it inserts it into sorted list of
    // m_FreeSuballocationsBySize if it's suitable.
    void RegisterFreeSuballocation(SuballocationList::iterator item);
    // Given free suballocation, it removes it from sorted list of
    // m_FreeSuballocationsBySize if it's suitable.
    void UnregisterFreeSuballocation(SuballocationList::iterator item);

    D3D12MA_CLASS_NO_COPY(BlockMetadata_Generic)
};

////////////////////////////////////////////////////////////////////////////////
// Private class DeviceMemoryBlock definition

/*
Represents a single block of device memory (heap) with all the data about its
regions (aka suballocations, #Allocation), assigned and free.

Thread-safety: This class must be externally synchronized.
*/
class DeviceMemoryBlock
{
public:
    BlockMetadata* m_pMetadata;

    DeviceMemoryBlock();

    ~DeviceMemoryBlock()
    {
        D3D12MA_ASSERT(m_Heap == NULL);
    }

    // Always call after construction.
    void Init(
        AllocatorPimpl* allocator,
        BlockVector* blockVector,
        //VmaPool hParentPool,
        D3D12_HEAP_TYPE newHeapType,
        ID3D12Heap* newHeap,
        UINT64 newSize,
        UINT id,
        UINT algorithm);
    // Always call before destruction.
    void Destroy(AllocatorPimpl* allocator);

    //VmaPool GetParentPool() const { return m_hParentPool; }
    BlockVector* GetBlockVector() const { return m_BlockVector; }
    ID3D12Heap* GetHeap() const { return m_Heap; }
    D3D12_HEAP_TYPE GetHeapType() const { return m_HeapType; }
    UINT GetId() const { return m_Id; }

    // Validates all data structures inside this object. If not valid, returns false.
    bool Validate() const;

private:
    BlockVector* m_BlockVector;
    //VmaPool m_hParentPool; // VK_NULL_HANDLE if not belongs to custom pool.
    D3D12_HEAP_TYPE m_HeapType; // TODO needed?
    UINT m_Id;
    ID3D12Heap* m_Heap;

    D3D12MA_CLASS_NO_COPY(DeviceMemoryBlock)
};

////////////////////////////////////////////////////////////////////////////////
// Private class BlockVector definition

// TEMP
class Pool;

/*
Sequence of DeviceMemoryBlock. Represents memory blocks allocated for a specific
heap type and possibly resource type (if only Tier 1 is supported).

Synchronized internally with a mutex.
*/
class BlockVector
{
    D3D12MA_CLASS_NO_COPY(BlockVector)
public:
    BlockVector(
        AllocatorPimpl* hAllocator,
        Pool* hParentPool,
        D3D12_HEAP_TYPE heapType,
        D3D12_HEAP_FLAGS heapFlags,
        UINT64 preferredBlockSize,
        size_t minBlockCount,
        size_t maxBlockCount,
        UINT frameInUseCount,
        bool isCustomPool,
        bool explicitBlockSize,
        UINT algorithm);
    ~BlockVector();

    HRESULT CreateMinBlocks();

    Pool* GetParentPool() const { return m_hParentPool; }
    UINT GetHeapType() const { return m_HeapType; }
    UINT64 GetPreferredBlockSize() const { return m_PreferredBlockSize; }
    UINT GetFrameInUseCount() const { return m_FrameInUseCount; }
    UINT GetAlgorithm() const { return m_Algorithm; }

    bool IsEmpty() const { return m_Blocks.empty(); }

    HRESULT Allocate(
        UINT currentFrameIndex,
        UINT64 size,
        UINT64 alignment,
        const ALLOCATION_DESC& createInfo,
        size_t allocationCount,
        Allocation** pAllocations);

    void Free(
        Allocation* hAllocation);

private:
    static UINT64 HeapFlagsToAlignment(D3D12_HEAP_FLAGS flags);

    AllocatorPimpl* const m_hAllocator;
    Pool* const m_hParentPool;
    const D3D12_HEAP_TYPE m_HeapType;
    const D3D12_HEAP_FLAGS m_HeapFlags;
    const UINT64 m_PreferredBlockSize;
    const size_t m_MinBlockCount;
    const size_t m_MaxBlockCount;
    const UINT m_FrameInUseCount;
    const bool m_IsCustomPool;
    const bool m_ExplicitBlockSize;
    const UINT m_Algorithm;
    /* There can be at most one allocation that is completely empty - a
    hysteresis to avoid pessimistic case of alternating creation and destruction
    of a VkDeviceMemory. */
    bool m_HasEmptyBlock;
    D3D12MA_RW_MUTEX m_Mutex;
    // Incrementally sorted by sumFreeSize, ascending.
    Vector<DeviceMemoryBlock*> m_Blocks;
    UINT m_NextBlockId;

    UINT64 CalcMaxBlockSize() const;

    // Finds and removes given block from vector.
    void Remove(DeviceMemoryBlock* pBlock);

    // Performs single step in sorting m_Blocks. They may not be fully sorted
    // after this call.
    void IncrementallySortBlocks();

    HRESULT AllocatePage(
        UINT currentFrameIndex,
        UINT64 size,
        UINT64 alignment,
        const ALLOCATION_DESC& createInfo,
        Allocation** pAllocation);

    // To be used only without CAN_MAKE_OTHER_LOST flag.
    HRESULT AllocateFromBlock(
        DeviceMemoryBlock* pBlock,
        UINT currentFrameIndex,
        UINT64 size,
        UINT64 alignment,
        ALLOCATION_FLAGS allocFlags,
        void* pUserData,
        UINT strategy,
        Allocation** pAllocation);

    HRESULT CreateBlock(UINT64 blockSize, size_t* pNewBlockIndex);
    HRESULT CreateD3d12Heap(ID3D12Heap*& outHeap, UINT64 size) const;
};

////////////////////////////////////////////////////////////////////////////////
// Private class AllocatorPimpl definition

static const UINT DEFAULT_POOL_MAX_COUNT = 9;

class AllocatorPimpl
{
public:
    AllocatorPimpl(const ALLOCATION_CALLBACKS& allocationCallbacks, const ALLOCATOR_DESC& desc);
    HRESULT Init();
    ~AllocatorPimpl();

    ID3D12Device* GetDevice() const { return m_Device; }
    // Shortcut for "Allocation Callbacks", because this function is called so often.
    const ALLOCATION_CALLBACKS& GetAllocs() const { return m_AllocationCallbacks; }
    const D3D12_FEATURE_DATA_D3D12_OPTIONS& GetD3D12Options() const { return m_D3D12Options; }
    bool SupportsResourceHeapTier2() const { return m_D3D12Options.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2; }
    bool UseMutex() const { return m_UseMutex; }

    HRESULT CreateResource(
        const ALLOCATION_DESC* pAllocDesc,
        const D3D12_RESOURCE_DESC* pResourceDesc,
        D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE *pOptimizedClearValue,
        Allocation** ppAllocation,
        REFIID riidResource,
        void** ppvResource);

    // Unregisters allocation from the collection of dedicated allocations.
    // Allocation object must be deleted externally afterwards.
    void FreeDedicatedMemory(Allocation* allocation);
    // Unregisters allocation from the collection of placed allocations.
    // Allocation object must be deleted externally afterwards.
    void FreePlacedMemory(Allocation* allocation);

private:
    friend class Allocator;

    /*
    Heuristics that decides whether a resource should better be placed in its own,
    dedicated allocation (committed resource rather than placed resource).
    */
    static bool PrefersDedicatedAllocation(const D3D12_RESOURCE_DESC& resourceDesc);

    bool m_UseMutex;
    ID3D12Device* m_Device;
    UINT64 m_PreferredBlockSize;
    ALLOCATION_CALLBACKS m_AllocationCallbacks;

    D3D12_FEATURE_DATA_D3D12_OPTIONS m_D3D12Options;

    typedef Vector<Allocation*> AllocationVectorType;
    AllocationVectorType* m_pDedicatedAllocations[HEAP_TYPE_COUNT];
    D3D12MA_RW_MUTEX m_DedicatedAllocationsMutex[HEAP_TYPE_COUNT];

    // Default pools.
    BlockVector* m_BlockVectors[DEFAULT_POOL_MAX_COUNT];

    // Allocates and registers new committed resource with implicit heap, as dedicated allocation.
    // Creates and returns Allocation objects.
    HRESULT AllocateDedicatedMemory(
        const ALLOCATION_DESC* pAllocDesc,
        const D3D12_RESOURCE_DESC* pResourceDesc,
        const D3D12_RESOURCE_ALLOCATION_INFO& resAllocInfo,
        D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE *pOptimizedClearValue,
        Allocation** ppAllocation,
        REFIID riidResource,
        void** ppvResource);

    /*
    If SupportsResourceHeapTier2():
        0: D3D12_HEAP_TYPE_DEFAULT
        1: D3D12_HEAP_TYPE_UPLOAD
        2: D3D12_HEAP_TYPE_READBACK
    else:
        0: D3D12_HEAP_TYPE_DEFAULT + buffer
        1: D3D12_HEAP_TYPE_DEFAULT + texture
        2: D3D12_HEAP_TYPE_DEFAULT + texture RT or DS
        3: D3D12_HEAP_TYPE_UPLOAD + buffer
        4: D3D12_HEAP_TYPE_UPLOAD + texture
        5: D3D12_HEAP_TYPE_UPLOAD + texture RT or DS
        6: D3D12_HEAP_TYPE_READBACK + buffer
        7: D3D12_HEAP_TYPE_READBACK + texture
        8: D3D12_HEAP_TYPE_READBACK + texture RT or DS
    */
    UINT CalcDefaultPoolCount() const;
    UINT CalcDefaultPoolIndex(const ALLOCATION_DESC& allocDesc, const D3D12_RESOURCE_DESC& resourceDesc) const;
    void CalcDefaultPoolParams(D3D12_HEAP_TYPE& outHeapType, D3D12_HEAP_FLAGS& outHeapFlags, UINT index) const;
};

////////////////////////////////////////////////////////////////////////////////
// Private class BlockMetadata implementation

BlockMetadata::BlockMetadata(const ALLOCATION_CALLBACKS* allocationCallbacks) :
    m_Size(0),
    m_pAllocationCallbacks(allocationCallbacks)
{
    D3D12MA_ASSERT(allocationCallbacks);
}

////////////////////////////////////////////////////////////////////////////////
// Private class BlockMetadata_Generic implementation

BlockMetadata_Generic::BlockMetadata_Generic(const ALLOCATION_CALLBACKS* allocationCallbacks) :
    BlockMetadata(allocationCallbacks),
    m_FreeCount(0),
    m_SumFreeSize(0),
    m_Suballocations(*allocationCallbacks),
    m_FreeSuballocationsBySize(*allocationCallbacks)
{
    D3D12MA_ASSERT(allocationCallbacks);
}

BlockMetadata_Generic::~BlockMetadata_Generic()
{
}

void BlockMetadata_Generic::Init(UINT64 size)
{
    BlockMetadata::Init(size);

    m_FreeCount = 1;
    m_SumFreeSize = size;

    Suballocation suballoc = {};
    suballoc.offset = 0;
    suballoc.size = size;
    suballoc.type = SUBALLOCATION_TYPE_FREE;
    suballoc.allocation = NULL;

    D3D12MA_ASSERT(size > MIN_FREE_SUBALLOCATION_SIZE_TO_REGISTER);
    m_Suballocations.push_back(suballoc);
    SuballocationList::iterator suballocItem = m_Suballocations.end();
    --suballocItem;
    m_FreeSuballocationsBySize.push_back(suballocItem);
}

bool BlockMetadata_Generic::Validate() const
{
    D3D12MA_VALIDATE(!m_Suballocations.empty());

    // Expected offset of new suballocation as calculated from previous ones.
    UINT64 calculatedOffset = 0;
    // Expected number of free suballocations as calculated from traversing their list.
    UINT calculatedFreeCount = 0;
    // Expected sum size of free suballocations as calculated from traversing their list.
    UINT64 calculatedSumFreeSize = 0;
    // Expected number of free suballocations that should be registered in
    // m_FreeSuballocationsBySize calculated from traversing their list.
    size_t freeSuballocationsToRegister = 0;
    // True if previous visited suballocation was free.
    bool prevFree = false;

    for(SuballocationList::const_iterator suballocItem = m_Suballocations.cbegin();
        suballocItem != m_Suballocations.cend();
        ++suballocItem)
    {
        const Suballocation& subAlloc = *suballocItem;

        // Actual offset of this suballocation doesn't match expected one.
        D3D12MA_VALIDATE(subAlloc.offset == calculatedOffset);

        const bool currFree = (subAlloc.type == SUBALLOCATION_TYPE_FREE);
        // Two adjacent free suballocations are invalid. They should be merged.
        D3D12MA_VALIDATE(!prevFree || !currFree);

        D3D12MA_VALIDATE(currFree == (subAlloc.allocation == NULL));

        if(currFree)
        {
            calculatedSumFreeSize += subAlloc.size;
            ++calculatedFreeCount;
            if(subAlloc.size >= MIN_FREE_SUBALLOCATION_SIZE_TO_REGISTER)
            {
                ++freeSuballocationsToRegister;
            }

            // Margin required between allocations - every free space must be at least that large.
            D3D12MA_VALIDATE(subAlloc.size >= D3D12MA_DEBUG_MARGIN);
        }
        else
        {
            D3D12MA_VALIDATE(subAlloc.allocation->GetOffset() == subAlloc.offset);
            D3D12MA_VALIDATE(subAlloc.allocation->GetSize() == subAlloc.size);

            // Margin required between allocations - previous allocation must be free.
            D3D12MA_VALIDATE(D3D12MA_DEBUG_MARGIN == 0 || prevFree);
        }

        calculatedOffset += subAlloc.size;
        prevFree = currFree;
    }

    // Number of free suballocations registered in m_FreeSuballocationsBySize doesn't
    // match expected one.
    D3D12MA_VALIDATE(m_FreeSuballocationsBySize.size() == freeSuballocationsToRegister);

    UINT64 lastSize = 0;
    for(size_t i = 0; i < m_FreeSuballocationsBySize.size(); ++i)
    {
        SuballocationList::iterator suballocItem = m_FreeSuballocationsBySize[i];

        // Only free suballocations can be registered in m_FreeSuballocationsBySize.
        D3D12MA_VALIDATE(suballocItem->type == SUBALLOCATION_TYPE_FREE);
        // They must be sorted by size ascending.
        D3D12MA_VALIDATE(suballocItem->size >= lastSize);

        lastSize = suballocItem->size;
    }

    // Check if totals match calculacted values.
    D3D12MA_VALIDATE(ValidateFreeSuballocationList());
    D3D12MA_VALIDATE(calculatedOffset == GetSize());
    D3D12MA_VALIDATE(calculatedSumFreeSize == m_SumFreeSize);
    D3D12MA_VALIDATE(calculatedFreeCount == m_FreeCount);

    return true;
}

UINT64 BlockMetadata_Generic::GetUnusedRangeSizeMax() const
{
    if(!m_FreeSuballocationsBySize.empty())
    {
        return m_FreeSuballocationsBySize.back()->size;
    }
    else
    {
        return 0;
    }
}

bool BlockMetadata_Generic::IsEmpty() const
{
    return (m_Suballocations.size() == 1) && (m_FreeCount == 1);
}

bool BlockMetadata_Generic::CreateAllocationRequest(
    UINT currentFrameIndex,
    UINT frameInUseCount,
    UINT64 allocSize,
    UINT64 allocAlignment,
    bool upperAddress,
    bool canMakeOtherLost,
    UINT strategy,
    VmaAllocationRequest* pAllocationRequest)
{
    D3D12MA_ASSERT(allocSize > 0);
    D3D12MA_ASSERT(!upperAddress);
    D3D12MA_ASSERT(pAllocationRequest != NULL);
    D3D12MA_HEAVY_ASSERT(Validate());

    pAllocationRequest->type = AllocationRequestType::Normal;

    // There is not enough total free space in this block to fullfill the request: Early return.
    if(canMakeOtherLost == false &&
        m_SumFreeSize < allocSize + 2 * D3D12MA_DEBUG_MARGIN)
    {
        return false;
    }

    // New algorithm, efficiently searching freeSuballocationsBySize.
    const size_t freeSuballocCount = m_FreeSuballocationsBySize.size();
    if(freeSuballocCount > 0)
    {
        //if(strategy == VMA_ALLOCATION_CREATE_STRATEGY_BEST_FIT_BIT)
        {
            // Find first free suballocation with size not less than allocSize + 2 * D3D12MA_DEBUG_MARGIN.
            SuballocationList::iterator* const it = BinaryFindFirstNotLess(
                m_FreeSuballocationsBySize.data(),
                m_FreeSuballocationsBySize.data() + freeSuballocCount,
                allocSize + 2 * D3D12MA_DEBUG_MARGIN,
                SuballocationItemSizeLess());
            size_t index = it - m_FreeSuballocationsBySize.data();
            for(; index < freeSuballocCount; ++index)
            {
                if(CheckAllocation(
                    currentFrameIndex,
                    frameInUseCount,
                    allocSize,
                    allocAlignment,
                    m_FreeSuballocationsBySize[index],
                    false, // canMakeOtherLost
                    &pAllocationRequest->offset,
                    &pAllocationRequest->itemsToMakeLostCount,
                    &pAllocationRequest->sumFreeSize,
                    &pAllocationRequest->sumItemSize))
                {
                    pAllocationRequest->item = m_FreeSuballocationsBySize[index];
                    return true;
                }
            }
        }
#if 0
        else if(strategy == VMA_ALLOCATION_INTERNAL_STRATEGY_MIN_OFFSET)
        {
            for(SuballocationList::iterator it = m_Suballocations.begin();
                it != m_Suballocations.end();
                ++it)
            {
                if(it->type == SUBALLOCATION_TYPE_FREE && CheckAllocation(
                    currentFrameIndex,
                    frameInUseCount,
                    allocSize,
                    allocAlignment,
                    it,
                    false, // canMakeOtherLost
                    &pAllocationRequest->offset,
                    &pAllocationRequest->itemsToMakeLostCount,
                    &pAllocationRequest->sumFreeSize,
                    &pAllocationRequest->sumItemSize))
                {
                    pAllocationRequest->item = it;
                    return true;
                }
            }
        }
        else // WORST_FIT, FIRST_FIT
        {
            // Search staring from biggest suballocations.
            for(size_t index = freeSuballocCount; index--; )
            {
                if(CheckAllocation(
                    currentFrameIndex,
                    frameInUseCount,
                    allocSize,
                    allocAlignment,
                    m_FreeSuballocationsBySize[index],
                    false, // canMakeOtherLost
                    &pAllocationRequest->offset,
                    &pAllocationRequest->itemsToMakeLostCount,
                    &pAllocationRequest->sumFreeSize,
                    &pAllocationRequest->sumItemSize))
                {
                    pAllocationRequest->item = m_FreeSuballocationsBySize[index];
                    return true;
                }
            }
        }
#endif
    }

#if 0
    if(canMakeOtherLost)
    {
        // Brute-force algorithm. TODO: Come up with something better.

        bool found = false;
        VmaAllocationRequest tmpAllocRequest = {};
        tmpAllocRequest.type = AllocationRequestType::Normal;
        for(SuballocationList::iterator suballocIt = m_Suballocations.begin();
            suballocIt != m_Suballocations.end();
            ++suballocIt)
        {
            if(suballocIt->type == SUBALLOCATION_TYPE_FREE ||
                suballocIt->hAllocation->CanBecomeLost())
            {
                if(CheckAllocation(
                    currentFrameIndex,
                    frameInUseCount,
                    allocSize,
                    allocAlignment,
                    suballocIt,
                    canMakeOtherLost,
                    &tmpAllocRequest.offset,
                    &tmpAllocRequest.itemsToMakeLostCount,
                    &tmpAllocRequest.sumFreeSize,
                    &tmpAllocRequest.sumItemSize))
                {
                    if(strategy == VMA_ALLOCATION_CREATE_STRATEGY_FIRST_FIT_BIT)
                    {
                        *pAllocationRequest = tmpAllocRequest;
                        pAllocationRequest->item = suballocIt;
                        break;
                    }
                    if(!found || tmpAllocRequest.CalcCost() < pAllocationRequest->CalcCost())
                    {
                        *pAllocationRequest = tmpAllocRequest;
                        pAllocationRequest->item = suballocIt;
                        found = true;
                    }
                }
            }
        }

        return found;
    }
#endif

    return false;
}

void BlockMetadata_Generic::Alloc(
    const VmaAllocationRequest& request,
    UINT64 allocSize,
    Allocation* allocation)
{
    D3D12MA_ASSERT(request.type == AllocationRequestType::Normal);
    D3D12MA_ASSERT(request.item != m_Suballocations.end());
    Suballocation& suballoc = *request.item;
    // Given suballocation is a free block.
    D3D12MA_ASSERT(suballoc.type == SUBALLOCATION_TYPE_FREE);
    // Given offset is inside this suballocation.
    D3D12MA_ASSERT(request.offset >= suballoc.offset);
    const UINT64 paddingBegin = request.offset - suballoc.offset;
    D3D12MA_ASSERT(suballoc.size >= paddingBegin + allocSize);
    const UINT64 paddingEnd = suballoc.size - paddingBegin - allocSize;

    // Unregister this free suballocation from m_FreeSuballocationsBySize and update
    // it to become used.
    UnregisterFreeSuballocation(request.item);

    suballoc.offset = request.offset;
    suballoc.size = allocSize;
    suballoc.type = SUBALLOCATION_TYPE_ALLOCATION;
    suballoc.allocation = allocation;

    // If there are any free bytes remaining at the end, insert new free suballocation after current one.
    if(paddingEnd)
    {
        Suballocation paddingSuballoc = {};
        paddingSuballoc.offset = request.offset + allocSize;
        paddingSuballoc.size = paddingEnd;
        paddingSuballoc.type = SUBALLOCATION_TYPE_FREE;
        SuballocationList::iterator next = request.item;
        ++next;
        const SuballocationList::iterator paddingEndItem =
            m_Suballocations.insert(next, paddingSuballoc);
        RegisterFreeSuballocation(paddingEndItem);
    }

    // If there are any free bytes remaining at the beginning, insert new free suballocation before current one.
    if(paddingBegin)
    {
        Suballocation paddingSuballoc = {};
        paddingSuballoc.offset = request.offset - paddingBegin;
        paddingSuballoc.size = paddingBegin;
        paddingSuballoc.type = SUBALLOCATION_TYPE_FREE;
        const SuballocationList::iterator paddingBeginItem =
            m_Suballocations.insert(request.item, paddingSuballoc);
        RegisterFreeSuballocation(paddingBeginItem);
    }

    // Update totals.
    m_FreeCount = m_FreeCount - 1;
    if(paddingBegin > 0)
    {
        ++m_FreeCount;
    }
    if(paddingEnd > 0)
    {
        ++m_FreeCount;
    }
    m_SumFreeSize -= allocSize;
}

void BlockMetadata_Generic::Free(const Allocation* allocation)
{
    for(SuballocationList::iterator suballocItem = m_Suballocations.begin();
        suballocItem != m_Suballocations.end();
        ++suballocItem)
    {
        Suballocation& suballoc = *suballocItem;
        if(suballoc.allocation == allocation)
        {
            FreeSuballocation(suballocItem);
            D3D12MA_HEAVY_ASSERT(Validate());
            return;
        }
    }
    D3D12MA_ASSERT(0 && "Not found!");
}

void BlockMetadata_Generic::FreeAtOffset(UINT64 offset)
{
    for(SuballocationList::iterator suballocItem = m_Suballocations.begin();
        suballocItem != m_Suballocations.end();
        ++suballocItem)
    {
        Suballocation& suballoc = *suballocItem;
        if(suballoc.offset == offset)
        {
            FreeSuballocation(suballocItem);
            return;
        }
    }
    D3D12MA_ASSERT(0 && "Not found!");
}

bool BlockMetadata_Generic::ValidateFreeSuballocationList() const
{
    UINT64 lastSize = 0;
    for(size_t i = 0, count = m_FreeSuballocationsBySize.size(); i < count; ++i)
    {
        const SuballocationList::iterator it = m_FreeSuballocationsBySize[i];

        D3D12MA_VALIDATE(it->type == SUBALLOCATION_TYPE_FREE);
        D3D12MA_VALIDATE(it->size >= MIN_FREE_SUBALLOCATION_SIZE_TO_REGISTER);
        D3D12MA_VALIDATE(it->size >= lastSize);
        lastSize = it->size;
    }
    return true;
}

bool BlockMetadata_Generic::CheckAllocation(
    UINT currentFrameIndex,
    UINT frameInUseCount,
    UINT64 allocSize,
    UINT64 allocAlignment,
    SuballocationList::const_iterator suballocItem,
    bool canMakeOtherLost,
    UINT64* pOffset,
    size_t* itemsToMakeLostCount,
    UINT64* pSumFreeSize,
    UINT64* pSumItemSize) const
{
    D3D12MA_ASSERT(allocSize > 0);
    D3D12MA_ASSERT(suballocItem != m_Suballocations.cend());
    D3D12MA_ASSERT(pOffset != NULL);

    *itemsToMakeLostCount = 0;
    *pSumFreeSize = 0;
    *pSumItemSize = 0;

#if 0
    if(canMakeOtherLost)
    {
        if(suballocItem->type == SUBALLOCATION_TYPE_FREE)
        {
            *pSumFreeSize = suballocItem->size;
        }
        else
        {
            if(suballocItem->hAllocation->CanBecomeLost() &&
                suballocItem->hAllocation->GetLastUseFrameIndex() + frameInUseCount < currentFrameIndex)
            {
                ++*itemsToMakeLostCount;
                *pSumItemSize = suballocItem->size;
            }
            else
            {
                return false;
            }
        }

        // Remaining size is too small for this request: Early return.
        if(GetSize() - suballocItem->offset < allocSize)
        {
            return false;
        }

        // Start from offset equal to beginning of this suballocation.
        *pOffset = suballocItem->offset;

        // Apply D3D12MA_DEBUG_MARGIN at the beginning.
        if(D3D12MA_DEBUG_MARGIN > 0)
        {
            *pOffset += D3D12MA_DEBUG_MARGIN;
        }

        // Apply alignment.
        *pOffset = AlignUp(*pOffset, allocAlignment);

        // Check previous suballocations for BufferImageGranularity conflicts.
        // Make bigger alignment if necessary.
        if(bufferImageGranularity > 1)
        {
            bool bufferImageGranularityConflict = false;
            SuballocationList::const_iterator prevSuballocItem = suballocItem;
            while(prevSuballocItem != m_Suballocations.cbegin())
            {
                --prevSuballocItem;
                const Suballocation& prevSuballoc = *prevSuballocItem;
                if(VmaBlocksOnSamePage(prevSuballoc.offset, prevSuballoc.size, *pOffset, bufferImageGranularity))
                {
                    if(VmaIsBufferImageGranularityConflict(prevSuballoc.type, allocType))
                    {
                        bufferImageGranularityConflict = true;
                        break;
                    }
                }
                else
                    // Already on previous page.
                    break;
            }
            if(bufferImageGranularityConflict)
            {
                *pOffset = AlignUp(*pOffset, bufferImageGranularity);
            }
        }

        // Now that we have final *pOffset, check if we are past suballocItem.
        // If yes, return false - this function should be called for another suballocItem as starting point.
        if(*pOffset >= suballocItem->offset + suballocItem->size)
        {
            return false;
        }

        // Calculate padding at the beginning based on current offset.
        const UINT64 paddingBegin = *pOffset - suballocItem->offset;

        // Calculate required margin at the end.
        const UINT64 requiredEndMargin = D3D12MA_DEBUG_MARGIN;

        const UINT64 totalSize = paddingBegin + allocSize + requiredEndMargin;
        // Another early return check.
        if(suballocItem->offset + totalSize > GetSize())
        {
            return false;
        }

        // Advance lastSuballocItem until desired size is reached.
        // Update itemsToMakeLostCount.
        SuballocationList::const_iterator lastSuballocItem = suballocItem;
        if(totalSize > suballocItem->size)
        {
            UINT64 remainingSize = totalSize - suballocItem->size;
            while(remainingSize > 0)
            {
                ++lastSuballocItem;
                if(lastSuballocItem == m_Suballocations.cend())
                {
                    return false;
                }
                if(lastSuballocItem->type == SUBALLOCATION_TYPE_FREE)
                {
                    *pSumFreeSize += lastSuballocItem->size;
                }
                else
                {
                    D3D12MA_ASSERT(lastSuballocItem->hAllocation != NULL);
                    if(lastSuballocItem->hAllocation->CanBecomeLost() &&
                        lastSuballocItem->hAllocation->GetLastUseFrameIndex() + frameInUseCount < currentFrameIndex)
                    {
                        ++*itemsToMakeLostCount;
                        *pSumItemSize += lastSuballocItem->size;
                    }
                    else
                    {
                        return false;
                    }
                }
                remainingSize = (lastSuballocItem->size < remainingSize) ?
                    remainingSize - lastSuballocItem->size : 0;
            }
        }

        // Check next suballocations for BufferImageGranularity conflicts.
        // If conflict exists, we must mark more allocations lost or fail.
        if(bufferImageGranularity > 1)
        {
            SuballocationList::const_iterator nextSuballocItem = lastSuballocItem;
            ++nextSuballocItem;
            while(nextSuballocItem != m_Suballocations.cend())
            {
                const Suballocation& nextSuballoc = *nextSuballocItem;
                if(VmaBlocksOnSamePage(*pOffset, allocSize, nextSuballoc.offset, bufferImageGranularity))
                {
                    if(VmaIsBufferImageGranularityConflict(allocType, nextSuballoc.type))
                    {
                        D3D12MA_ASSERT(nextSuballoc.hAllocation != NULL);
                        if(nextSuballoc.hAllocation->CanBecomeLost() &&
                            nextSuballoc.hAllocation->GetLastUseFrameIndex() + frameInUseCount < currentFrameIndex)
                        {
                            ++*itemsToMakeLostCount;
                        }
                        else
                        {
                            return false;
                        }
                    }
                }
                else
                {
                    // Already on next page.
                    break;
                }
                ++nextSuballocItem;
            }
        }
    }
    else
#endif
    {
        const Suballocation& suballoc = *suballocItem;
        D3D12MA_ASSERT(suballoc.type == SUBALLOCATION_TYPE_FREE);

        *pSumFreeSize = suballoc.size;

        // Size of this suballocation is too small for this request: Early return.
        if(suballoc.size < allocSize)
        {
            return false;
        }

        // Start from offset equal to beginning of this suballocation.
        *pOffset = suballoc.offset;

        // Apply D3D12MA_DEBUG_MARGIN at the beginning.
        if(D3D12MA_DEBUG_MARGIN > 0)
        {
            *pOffset += D3D12MA_DEBUG_MARGIN;
        }

        // Apply alignment.
        *pOffset = AlignUp(*pOffset, allocAlignment);

        // Calculate padding at the beginning based on current offset.
        const UINT64 paddingBegin = *pOffset - suballoc.offset;

        // Calculate required margin at the end.
        const UINT64 requiredEndMargin = D3D12MA_DEBUG_MARGIN;

        // Fail if requested size plus margin before and after is bigger than size of this suballocation.
        if(paddingBegin + allocSize + requiredEndMargin > suballoc.size)
        {
            return false;
        }
    }

    // All tests passed: Success. pOffset is already filled.
    return true;
}

void BlockMetadata_Generic::MergeFreeWithNext(SuballocationList::iterator item)
{
    D3D12MA_ASSERT(item != m_Suballocations.end());
    D3D12MA_ASSERT(item->type == SUBALLOCATION_TYPE_FREE);

    SuballocationList::iterator nextItem = item;
    ++nextItem;
    D3D12MA_ASSERT(nextItem != m_Suballocations.end());
    D3D12MA_ASSERT(nextItem->type == SUBALLOCATION_TYPE_FREE);

    item->size += nextItem->size;
    --m_FreeCount;
    m_Suballocations.erase(nextItem);
}

SuballocationList::iterator BlockMetadata_Generic::FreeSuballocation(SuballocationList::iterator suballocItem)
{
    // Change this suballocation to be marked as free.
    Suballocation& suballoc = *suballocItem;
    suballoc.type = SUBALLOCATION_TYPE_FREE;
    suballoc.allocation = NULL;

    // Update totals.
    ++m_FreeCount;
    m_SumFreeSize += suballoc.size;

    // Merge with previous and/or next suballocation if it's also free.
    bool mergeWithNext = false;
    bool mergeWithPrev = false;

    SuballocationList::iterator nextItem = suballocItem;
    ++nextItem;
    if((nextItem != m_Suballocations.end()) && (nextItem->type == SUBALLOCATION_TYPE_FREE))
    {
        mergeWithNext = true;
    }

    SuballocationList::iterator prevItem = suballocItem;
    if(suballocItem != m_Suballocations.begin())
    {
        --prevItem;
        if(prevItem->type == SUBALLOCATION_TYPE_FREE)
        {
            mergeWithPrev = true;
        }
    }

    if(mergeWithNext)
    {
        UnregisterFreeSuballocation(nextItem);
        MergeFreeWithNext(suballocItem);
    }

    if(mergeWithPrev)
    {
        UnregisterFreeSuballocation(prevItem);
        MergeFreeWithNext(prevItem);
        RegisterFreeSuballocation(prevItem);
        return prevItem;
    }
    else
    {
        RegisterFreeSuballocation(suballocItem);
        return suballocItem;
    }
}

void BlockMetadata_Generic::RegisterFreeSuballocation(SuballocationList::iterator item)
{
    D3D12MA_ASSERT(item->type == SUBALLOCATION_TYPE_FREE);
    D3D12MA_ASSERT(item->size > 0);

    // You may want to enable this validation at the beginning or at the end of
    // this function, depending on what do you want to check.
    D3D12MA_HEAVY_ASSERT(ValidateFreeSuballocationList());

    if(item->size >= MIN_FREE_SUBALLOCATION_SIZE_TO_REGISTER)
    {
        if(m_FreeSuballocationsBySize.empty())
        {
            m_FreeSuballocationsBySize.push_back(item);
        }
        else
        {
            m_FreeSuballocationsBySize.InsertSorted(item, SuballocationItemSizeLess());
        }
    }

    //D3D12MA_HEAVY_ASSERT(ValidateFreeSuballocationList());
}


void BlockMetadata_Generic::UnregisterFreeSuballocation(SuballocationList::iterator item)
{
    D3D12MA_ASSERT(item->type == SUBALLOCATION_TYPE_FREE);
    D3D12MA_ASSERT(item->size > 0);

    // You may want to enable this validation at the beginning or at the end of
    // this function, depending on what do you want to check.
    D3D12MA_HEAVY_ASSERT(ValidateFreeSuballocationList());

    if(item->size >= MIN_FREE_SUBALLOCATION_SIZE_TO_REGISTER)
    {
        SuballocationList::iterator* const it = BinaryFindFirstNotLess(
            m_FreeSuballocationsBySize.data(),
            m_FreeSuballocationsBySize.data() + m_FreeSuballocationsBySize.size(),
            item,
            SuballocationItemSizeLess());
        for(size_t index = it - m_FreeSuballocationsBySize.data();
            index < m_FreeSuballocationsBySize.size();
            ++index)
        {
            if(m_FreeSuballocationsBySize[index] == item)
            {
                m_FreeSuballocationsBySize.remove(index);
                return;
            }
            D3D12MA_ASSERT((m_FreeSuballocationsBySize[index]->size == item->size) && "Not found.");
        }
        D3D12MA_ASSERT(0 && "Not found.");
    }

    //D3D12MA_HEAVY_ASSERT(ValidateFreeSuballocationList());
}

////////////////////////////////////////////////////////////////////////////////
// Private class DeviceMemoryBlock implementation

DeviceMemoryBlock::DeviceMemoryBlock() :
    m_pMetadata(NULL),
    m_BlockVector(NULL),
    m_HeapType(D3D12_HEAP_TYPE_CUSTOM),
    m_Id(0),
    m_Heap(NULL)
{
}

void DeviceMemoryBlock::Init(
    AllocatorPimpl* allocator,
    BlockVector* blockVector,
    //VmaPool hParentPool,
    D3D12_HEAP_TYPE newHeapType,
    ID3D12Heap* newHeap,
    UINT64 newSize,
    UINT id,
    UINT algorithm)
{
    D3D12MA_ASSERT(m_Heap == NULL);

    m_BlockVector = blockVector;
    //m_hParentPool = hParentPool;
    m_HeapType = newHeapType;
    m_Id = id;
    m_Heap = newHeap;

    const ALLOCATION_CALLBACKS& allocs = allocator->GetAllocs();

    switch(algorithm)
    {
#if 0
    case VMA_POOL_CREATE_LINEAR_ALGORITHM_BIT:
        m_pMetadata = vma_new(hAllocator, VmaBlockMetadata_Linear)(hAllocator);
        break;
    case VMA_POOL_CREATE_BUDDY_ALGORITHM_BIT:
        m_pMetadata = vma_new(hAllocator, VmaBlockMetadata_Buddy)(hAllocator);
        break;
#endif
    default:
        D3D12MA_ASSERT(0);
        // Fall-through.
    case 0:
        m_pMetadata = D3D12MA_NEW(allocs, BlockMetadata_Generic)(&allocs);
    }
    m_pMetadata->Init(newSize);
}

void DeviceMemoryBlock::Destroy(AllocatorPimpl* allocator)
{
    // This is the most important assert in the entire library.
    // Hitting it means you have some memory leak - unreleased Allocation objects.
    D3D12MA_ASSERT(m_pMetadata->IsEmpty() && "Some allocations were not freed before destruction of this memory block!");

    D3D12MA_ASSERT(m_Heap != NULL);
    m_Heap->Release();
    m_Heap = NULL;

    D3D12MA_DELETE(allocator->GetAllocs(), m_pMetadata);
    m_pMetadata = NULL;
}

bool DeviceMemoryBlock::Validate() const
{
    D3D12MA_VALIDATE(m_Heap && m_pMetadata && m_pMetadata->GetSize() != 0);

    return m_pMetadata->Validate();
}

////////////////////////////////////////////////////////////////////////////////
// Private class BlockVector implementation

BlockVector::BlockVector(
    AllocatorPimpl* hAllocator,
    Pool* hParentPool,
    D3D12_HEAP_TYPE heapType,
    D3D12_HEAP_FLAGS heapFlags,
    UINT64 preferredBlockSize,
    size_t minBlockCount,
    size_t maxBlockCount,
    UINT frameInUseCount,
    bool isCustomPool,
    bool explicitBlockSize,
    UINT algorithm) :
    m_hAllocator(hAllocator),
    m_hParentPool(hParentPool),
    m_HeapType(heapType),
    m_HeapFlags(heapFlags),
    m_PreferredBlockSize(preferredBlockSize),
    m_MinBlockCount(minBlockCount),
    m_MaxBlockCount(maxBlockCount),
    m_FrameInUseCount(frameInUseCount),
    m_IsCustomPool(isCustomPool),
    m_ExplicitBlockSize(explicitBlockSize),
    m_Algorithm(algorithm),
    m_HasEmptyBlock(false),
    m_Blocks(hAllocator->GetAllocs()),
    m_NextBlockId(0)
{
}

BlockVector::~BlockVector()
{
    for(size_t i = m_Blocks.size(); i--; )
    {
        m_Blocks[i]->Destroy(m_hAllocator);
        D3D12MA_DELETE(m_hAllocator->GetAllocs(), m_Blocks[i]);
    }
}

HRESULT BlockVector::CreateMinBlocks()
{
    for(size_t i = 0; i < m_MinBlockCount; ++i)
    {
        HRESULT hr = CreateBlock(m_PreferredBlockSize, NULL);
        if(FAILED(hr))
        {
            return hr;
        }
    }
    return S_OK;
}

static const UINT VMA_ALLOCATION_TRY_COUNT = 32;

HRESULT BlockVector::Allocate(
    UINT currentFrameIndex,
    UINT64 size,
    UINT64 alignment,
    const ALLOCATION_DESC& createInfo,
    size_t allocationCount,
    Allocation** pAllocations)
{
    size_t allocIndex;
    HRESULT hr = S_OK;

    {
        MutexLockWrite lock(m_Mutex, m_hAllocator->UseMutex());
        for(allocIndex = 0; allocIndex < allocationCount; ++allocIndex)
        {
            hr = AllocatePage(
                currentFrameIndex,
                size,
                alignment,
                createInfo,
                pAllocations + allocIndex);
            if(FAILED(hr))
            {
                break;
            }
        }
    }

    if(FAILED(hr))
    {
        // Free all already created allocations.
        while(allocIndex--)
        {
            Free(pAllocations[allocIndex]);
        }
        memset(pAllocations, 0, sizeof(Allocation*) * allocationCount);
    }

    return hr;
}

HRESULT BlockVector::AllocatePage(
    UINT currentFrameIndex,
    UINT64 size,
    UINT64 alignment,
    const ALLOCATION_DESC& createInfo,
    Allocation** pAllocation)
{
    //const bool isUpperAddress = (createInfo.flags & VMA_ALLOCATION_CREATE_UPPER_ADDRESS_BIT) != 0;
    bool canMakeOtherLost = false;//(createInfo.flags & VMA_ALLOCATION_CREATE_CAN_MAKE_OTHER_LOST_BIT) != 0;
    //const bool mapped = (createInfo.flags & VMA_ALLOCATION_CREATE_MAPPED_BIT) != 0;
    //const bool isUserDataString = (createInfo.flags & VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT) != 0;
    const bool canCreateNewBlock =
        ((createInfo.Flags & ALLOCATION_FLAG_NEVER_ALLOCATE) == 0) &&
        (m_Blocks.size() < m_MaxBlockCount);
    //UINT strategy = createInfo.flags & VMA_ALLOCATION_CREATE_STRATEGY_MASK;

    // If linearAlgorithm is used, canMakeOtherLost is available only when used as ring buffer.
    // Which in turn is available only when maxBlockCount = 1.
    /*
    if(m_Algorithm == VMA_POOL_CREATE_LINEAR_ALGORITHM_BIT && m_MaxBlockCount > 1)
    {
        canMakeOtherLost = false;
    }

    // Upper address can only be used with linear allocator and within single memory block.
    if(isUpperAddress &&
        (m_Algorithm != VMA_POOL_CREATE_LINEAR_ALGORITHM_BIT || m_MaxBlockCount > 1))
    {
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    // Validate strategy.
    switch(strategy)
    {
    case 0:
        strategy = VMA_ALLOCATION_CREATE_STRATEGY_BEST_FIT_BIT;
        break;
    case VMA_ALLOCATION_CREATE_STRATEGY_BEST_FIT_BIT:
    case VMA_ALLOCATION_CREATE_STRATEGY_WORST_FIT_BIT:
    case VMA_ALLOCATION_CREATE_STRATEGY_FIRST_FIT_BIT:
        break;
    default:
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    */

    // Early reject: requested allocation size is larger that maximum block size for this block vector.
    if(size + 2 * D3D12MA_DEBUG_MARGIN > m_PreferredBlockSize)
    {
        return E_OUTOFMEMORY;
    }

    /*
    Under certain condition, this whole section can be skipped for optimization, so
    we move on directly to trying to allocate with canMakeOtherLost. That's the case
    e.g. for custom pools with linear algorithm.
    */
    if(!canMakeOtherLost || canCreateNewBlock)
    {
        // 1. Search existing allocations. Try to allocate without making other allocations lost.
        ALLOCATION_FLAGS allocFlagsCopy = createInfo.Flags;
        //allocFlagsCopy &= ~VMA_ALLOCATION_CREATE_CAN_MAKE_OTHER_LOST_BIT;

        /*
        if(m_Algorithm == VMA_POOL_CREATE_LINEAR_ALGORITHM_BIT)
        {
            // Use only last block.
            if(!m_Blocks.empty())
            {
                VmaDeviceMemoryBlock* const pCurrBlock = m_Blocks.back();
                D3D12MA_ASSERT(pCurrBlock);
                VkResult res = AllocateFromBlock(
                    pCurrBlock,
                    currentFrameIndex,
                    size,
                    alignment,
                    allocFlagsCopy,
                    createInfo.pUserData,
                    suballocType,
                    strategy,
                    pAllocation);
                if(res == VK_SUCCESS)
                {
                    VMA_DEBUG_LOG("    Returned from last block #%u", (UINT)(m_Blocks.size() - 1));
                    return VK_SUCCESS;
                }
            }
        }
        else
        */
        {
            //if(strategy == VMA_ALLOCATION_CREATE_STRATEGY_BEST_FIT_BIT)
            {
                // Forward order in m_Blocks - prefer blocks with smallest amount of free space.
                for(size_t blockIndex = 0; blockIndex < m_Blocks.size(); ++blockIndex )
                {
                    DeviceMemoryBlock* const pCurrBlock = m_Blocks[blockIndex];
                    D3D12MA_ASSERT(pCurrBlock);
                    HRESULT hr = AllocateFromBlock(
                        pCurrBlock,
                        currentFrameIndex,
                        size,
                        alignment,
                        allocFlagsCopy,
                        NULL,//createInfo.pUserData,
                        0,//strategy,
                        pAllocation);
                    if(SUCCEEDED(hr))
                    {
                        //VMA_DEBUG_LOG("    Returned from existing block #%u", (UINT)blockIndex);
                        return hr;
                    }
                }
            }
            /*
            else // WORST_FIT, FIRST_FIT
            {
                // Backward order in m_Blocks - prefer blocks with largest amount of free space.
                for(size_t blockIndex = m_Blocks.size(); blockIndex--; )
                {
                    VmaDeviceMemoryBlock* const pCurrBlock = m_Blocks[blockIndex];
                    D3D12MA_ASSERT(pCurrBlock);
                    VkResult res = AllocateFromBlock(
                        pCurrBlock,
                        currentFrameIndex,
                        size,
                        alignment,
                        allocFlagsCopy,
                        createInfo.pUserData,
                        suballocType,
                        strategy,
                        pAllocation);
                    if(res == VK_SUCCESS)
                    {
                        VMA_DEBUG_LOG("    Returned from existing block #%u", (UINT)blockIndex);
                        return VK_SUCCESS;
                    }
                }
            }
            */
        }

        // 2. Try to create new block.
        if(canCreateNewBlock)
        {
            // Calculate optimal size for new block.
            UINT64 newBlockSize = m_PreferredBlockSize;
            UINT newBlockSizeShift = 0;
            const UINT NEW_BLOCK_SIZE_SHIFT_MAX = 3;

            if(!m_ExplicitBlockSize)
            {
                // Allocate 1/8, 1/4, 1/2 as first blocks.
                const UINT64 maxExistingBlockSize = CalcMaxBlockSize();
                for(UINT i = 0; i < NEW_BLOCK_SIZE_SHIFT_MAX; ++i)
                {
                    const UINT64 smallerNewBlockSize = newBlockSize / 2;
                    if(smallerNewBlockSize > maxExistingBlockSize && smallerNewBlockSize >= size * 2)
                    {
                        newBlockSize = smallerNewBlockSize;
                        ++newBlockSizeShift;
                    }
                    else
                    {
                        break;
                    }
                }
            }

            size_t newBlockIndex = 0;
            HRESULT hr = CreateBlock(newBlockSize, &newBlockIndex);
            // Allocation of this size failed? Try 1/2, 1/4, 1/8 of m_PreferredBlockSize.
            if(!m_ExplicitBlockSize)
            {
                while(FAILED(hr) && newBlockSizeShift < NEW_BLOCK_SIZE_SHIFT_MAX)
                {
                    const UINT64 smallerNewBlockSize = newBlockSize / 2;
                    if(smallerNewBlockSize >= size)
                    {
                        newBlockSize = smallerNewBlockSize;
                        ++newBlockSizeShift;
                        hr = CreateBlock(newBlockSize, &newBlockIndex);
                    }
                    else
                    {
                        break;
                    }
                }
            }

            if(SUCCEEDED(hr))
            {
                DeviceMemoryBlock* const pBlock = m_Blocks[newBlockIndex];
                D3D12MA_ASSERT(pBlock->m_pMetadata->GetSize() >= size);

                hr = AllocateFromBlock(
                    pBlock,
                    currentFrameIndex,
                    size,
                    alignment,
                    allocFlagsCopy,
                    NULL,//createInfo.pUserData,
                    0,//strategy,
                    pAllocation);
                if(SUCCEEDED(hr))
                {
                    //VMA_DEBUG_LOG("    Created new block Size=%llu", newBlockSize);
                    return hr;
                }
                else
                {
                    // Allocation from new block failed, possibly due to VMA_DEBUG_MARGIN or alignment.
                    return E_OUTOFMEMORY;
                }
            }
        }
    }

    // 3. Try to allocate from existing blocks with making other allocations lost.
#if 0
    if(canMakeOtherLost)
    {
        UINT tryIndex = 0;
        for(; tryIndex < VMA_ALLOCATION_TRY_COUNT; ++tryIndex)
        {
            VmaDeviceMemoryBlock* pBestRequestBlock = NULL;
            VmaAllocationRequest bestRequest = {};
            UINT64 bestRequestCost = VK_WHOLE_SIZE;

            // 1. Search existing allocations.
            if(strategy == VMA_ALLOCATION_CREATE_STRATEGY_BEST_FIT_BIT)
            {
                // Forward order in m_Blocks - prefer blocks with smallest amount of free space.
                for(size_t blockIndex = 0; blockIndex < m_Blocks.size(); ++blockIndex )
                {
                    VmaDeviceMemoryBlock* const pCurrBlock = m_Blocks[blockIndex];
                    D3D12MA_ASSERT(pCurrBlock);
                    VmaAllocationRequest currRequest = {};
                    if(pCurrBlock->m_pMetadata->CreateAllocationRequest(
                        currentFrameIndex,
                        m_FrameInUseCount,
                        m_BufferImageGranularity,
                        size,
                        alignment,
                        (createInfo.flags & VMA_ALLOCATION_CREATE_UPPER_ADDRESS_BIT) != 0,
                        suballocType,
                        canMakeOtherLost,
                        strategy,
                        &currRequest))
                    {
                        const UINT64 currRequestCost = currRequest.CalcCost();
                        if(pBestRequestBlock == NULL ||
                            currRequestCost < bestRequestCost)
                        {
                            pBestRequestBlock = pCurrBlock;
                            bestRequest = currRequest;
                            bestRequestCost = currRequestCost;

                            if(bestRequestCost == 0)
                            {
                                break;
                            }
                        }
                    }
                }
            }
            else // WORST_FIT, FIRST_FIT
            {
                // Backward order in m_Blocks - prefer blocks with largest amount of free space.
                for(size_t blockIndex = m_Blocks.size(); blockIndex--; )
                {
                    VmaDeviceMemoryBlock* const pCurrBlock = m_Blocks[blockIndex];
                    D3D12MA_ASSERT(pCurrBlock);
                    VmaAllocationRequest currRequest = {};
                    if(pCurrBlock->m_pMetadata->CreateAllocationRequest(
                        currentFrameIndex,
                        m_FrameInUseCount,
                        m_BufferImageGranularity,
                        size,
                        alignment,
                        (createInfo.flags & VMA_ALLOCATION_CREATE_UPPER_ADDRESS_BIT) != 0,
                        suballocType,
                        canMakeOtherLost,
                        strategy,
                        &currRequest))
                    {
                        const UINT64 currRequestCost = currRequest.CalcCost();
                        if(pBestRequestBlock == NULL ||
                            currRequestCost < bestRequestCost ||
                            strategy == VMA_ALLOCATION_CREATE_STRATEGY_FIRST_FIT_BIT)
                        {
                            pBestRequestBlock = pCurrBlock;
                            bestRequest = currRequest;
                            bestRequestCost = currRequestCost;

                            if(bestRequestCost == 0 ||
                                strategy == VMA_ALLOCATION_CREATE_STRATEGY_FIRST_FIT_BIT)
                            {
                                break;
                            }
                        }
                    }
                }
            }

            if(pBestRequestBlock != NULL)
            {
                if(mapped)
                {
                    VkResult res = pBestRequestBlock->Map(m_hAllocator, 1, NULL);
                    if(res != VK_SUCCESS)
                    {
                        return res;
                    }
                }

                if(pBestRequestBlock->m_pMetadata->MakeRequestedAllocationsLost(
                    currentFrameIndex,
                    m_FrameInUseCount,
                    &bestRequest))
                {
                    // We no longer have an empty Allocation.
                    if(pBestRequestBlock->m_pMetadata->IsEmpty())
                    {
                        m_HasEmptyBlock = false;
                    }
                    // Allocate from this pBlock.
                    *pAllocation = m_hAllocator->m_AllocationObjectAllocator.Allocate();
                    (*pAllocation)->Ctor(currentFrameIndex, isUserDataString);
                    pBestRequestBlock->m_pMetadata->Alloc(bestRequest, suballocType, size, *pAllocation);
                    (*pAllocation)->InitBlockAllocation(
                        pBestRequestBlock,
                        bestRequest.offset,
                        alignment,
                        size,
                        suballocType,
                        mapped,
                        (createInfo.flags & VMA_ALLOCATION_CREATE_CAN_BECOME_LOST_BIT) != 0);
                    D3D12MA_HEAVY_ASSERT(pBestRequestBlock->Validate());
                    VMA_DEBUG_LOG("    Returned from existing block");
                    (*pAllocation)->SetUserData(m_hAllocator, createInfo.pUserData);
                    if(VMA_DEBUG_INITIALIZE_ALLOCATIONS)
                    {
                        m_hAllocator->FillAllocation(*pAllocation, VMA_ALLOCATION_FILL_PATTERN_CREATED);
                    }
                    if(IsCorruptionDetectionEnabled())
                    {
                        VkResult res = pBestRequestBlock->WriteMagicValueAroundAllocation(m_hAllocator, bestRequest.offset, size);
                        D3D12MA_ASSERT(res == VK_SUCCESS && "Couldn't map block memory to write magic value.");
                    }
                    return VK_SUCCESS;
                }
                // else: Some allocations must have been touched while we are here. Next try.
            }
            else
            {
                // Could not find place in any of the blocks - break outer loop.
                break;
            }
        }
        /* Maximum number of tries exceeded - a very unlike event when many other
        threads are simultaneously touching allocations making it impossible to make
        lost at the same time as we try to allocate. */
        if(tryIndex == VMA_ALLOCATION_TRY_COUNT)
        {
            return VK_ERROR_TOO_MANY_OBJECTS;
        }
    }
#endif

    return E_OUTOFMEMORY;
}

void BlockVector::Free(Allocation* hAllocation)
{
    DeviceMemoryBlock* pBlockToDelete = NULL;

    // Scope for lock.
    {
        MutexLockWrite lock(m_Mutex, m_hAllocator->UseMutex());

        DeviceMemoryBlock* pBlock = hAllocation->GetBlock();

        /*
        if(IsCorruptionDetectionEnabled())
        {
            VkResult res = pBlock->ValidateMagicValueAroundAllocation(m_hAllocator, hAllocation->GetOffset(), hAllocation->GetSize());
            D3D12MA_ASSERT(res == VK_SUCCESS && "Couldn't map block memory to validate magic value.");
        }

        if(hAllocation->IsPersistentMap())
        {
            pBlock->Unmap(m_hAllocator, 1);
        }
        */

        pBlock->m_pMetadata->Free(hAllocation);
        D3D12MA_HEAVY_ASSERT(pBlock->Validate());

        //VMA_DEBUG_LOG("  Freed from MemoryTypeIndex=%u", m_MemoryTypeIndex);

        // pBlock became empty after this deallocation.
        if(pBlock->m_pMetadata->IsEmpty())
        {
            // Already has empty Allocation. We don't want to have two, so delete this one.
            if(m_HasEmptyBlock && m_Blocks.size() > m_MinBlockCount)
            {
                pBlockToDelete = pBlock;
                Remove(pBlock);
            }
            // We now have first empty block.
            else
            {
                m_HasEmptyBlock = true;
            }
        }
        // pBlock didn't become empty, but we have another empty block - find and free that one.
        // (This is optional, heuristics.)
        else if(m_HasEmptyBlock)
        {
            DeviceMemoryBlock* pLastBlock = m_Blocks.back();
            if(pLastBlock->m_pMetadata->IsEmpty() && m_Blocks.size() > m_MinBlockCount)
            {
                pBlockToDelete = pLastBlock;
                m_Blocks.pop_back();
                m_HasEmptyBlock = false;
            }
        }

        IncrementallySortBlocks();
    }

    // Destruction of a free Allocation. Deferred until this point, outside of mutex
    // lock, for performance reason.
    if(pBlockToDelete != NULL)
    {
        //VMA_DEBUG_LOG("    Deleted empty allocation");
        pBlockToDelete->Destroy(m_hAllocator);
        D3D12MA_DELETE(m_hAllocator->GetAllocs(), pBlockToDelete);
    }
}

UINT64 BlockVector::HeapFlagsToAlignment(D3D12_HEAP_FLAGS flags)
{
    /*
    Documentation of D3D12_HEAP_DESC structure says:

    - D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT   defined as 64KB.
    - D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT   defined as 4MB. An
      application must decide whether the heap will contain multi-sample
      anti-aliasing (MSAA), in which case, the application must choose [this flag].

    https://docs.microsoft.com/en-us/windows/desktop/api/d3d12/ns-d3d12-d3d12_heap_desc
    */

    const D3D12_HEAP_FLAGS denyAllTexturesFlags =
        D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
    const bool canContainMsaaResources =
        (flags & denyAllTexturesFlags) != denyAllTexturesFlags;
    return canContainMsaaResources ?
        D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
}

UINT64 BlockVector::CalcMaxBlockSize() const
{
    UINT64 result = 0;
    for(size_t i = m_Blocks.size(); i--; )
    {
        result = D3D12MA_MAX(result, m_Blocks[i]->m_pMetadata->GetSize());
        if(result >= m_PreferredBlockSize)
        {
            break;
        }
    }
    return result;
}

void BlockVector::Remove(DeviceMemoryBlock* pBlock)
{
    for(UINT blockIndex = 0; blockIndex < m_Blocks.size(); ++blockIndex)
    {
        if(m_Blocks[blockIndex] == pBlock)
        {
            m_Blocks.remove(blockIndex);
            return;
        }
    }
    D3D12MA_ASSERT(0);
}

void BlockVector::IncrementallySortBlocks()
{
    //if(m_Algorithm != VMA_POOL_CREATE_LINEAR_ALGORITHM_BIT)
    {
        // Bubble sort only until first swap.
        for(size_t i = 1; i < m_Blocks.size(); ++i)
        {
            if(m_Blocks[i - 1]->m_pMetadata->GetSumFreeSize() > m_Blocks[i]->m_pMetadata->GetSumFreeSize())
            {
                D3D12MA_SWAP(m_Blocks[i - 1], m_Blocks[i]);
                return;
            }
        }
    }
}

HRESULT BlockVector::AllocateFromBlock(
    DeviceMemoryBlock* pBlock,
    UINT currentFrameIndex,
    UINT64 size,
    UINT64 alignment,
    ALLOCATION_FLAGS allocFlags,
    void* pUserData,
    UINT strategy,
    Allocation** pAllocation)
{
    //D3D12MA_ASSERT((allocFlags & VMA_ALLOCATION_CREATE_CAN_MAKE_OTHER_LOST_BIT) == 0);
    //const bool isUpperAddress = (allocFlags & VMA_ALLOCATION_CREATE_UPPER_ADDRESS_BIT) != 0;
    //const bool mapped = (allocFlags & VMA_ALLOCATION_CREATE_MAPPED_BIT) != 0;
    //const bool isUserDataString = (allocFlags & VMA_ALLOCATION_CREATE_USER_DATA_COPY_STRING_BIT) != 0;

    VmaAllocationRequest currRequest = {};
    if(pBlock->m_pMetadata->CreateAllocationRequest(
        currentFrameIndex,
        m_FrameInUseCount,
        size,
        alignment,
        false,//isUpperAddress,
        false, // canMakeOtherLost
        strategy,
        &currRequest))
    {
        // Allocate from pCurrBlock.
        D3D12MA_ASSERT(currRequest.itemsToMakeLostCount == 0);

        /*
        if(mapped)
        {
            VkResult res = pBlock->Map(m_hAllocator, 1, NULL);
            if(res != VK_SUCCESS)
            {
                return res;
            }
        }
        */

        // We no longer have an empty Allocation.
        if(pBlock->m_pMetadata->IsEmpty())
        {
            m_HasEmptyBlock = false;
        }

        *pAllocation = D3D12MA_NEW(m_hAllocator->GetAllocs(), Allocation)();
        pBlock->m_pMetadata->Alloc(currRequest, size, *pAllocation);
        (*pAllocation)->InitPlaced(
            m_hAllocator,
            size,
            currRequest.offset,
            alignment,
            pBlock);
        D3D12MA_HEAVY_ASSERT(pBlock->Validate());
        /*
        (*pAllocation)->SetUserData(m_hAllocator, pUserData);
        if(VMA_DEBUG_INITIALIZE_ALLOCATIONS)
        {
            m_hAllocator->FillAllocation(*pAllocation, VMA_ALLOCATION_FILL_PATTERN_CREATED);
        }
        if(IsCorruptionDetectionEnabled())
        {
            VkResult res = pBlock->WriteMagicValueAroundAllocation(m_hAllocator, currRequest.offset, size);
            D3D12MA_ASSERT(res == VK_SUCCESS && "Couldn't map block memory to write magic value.");
        }
        */
        return S_OK;
    }
    return E_OUTOFMEMORY;
}

HRESULT BlockVector::CreateBlock(UINT64 blockSize, size_t* pNewBlockIndex)
{
    ID3D12Heap* heap = NULL;
    HRESULT hr = CreateD3d12Heap(heap, blockSize);
    if(FAILED(hr))
    {
        return hr;
    }

    DeviceMemoryBlock* const pBlock = D3D12MA_NEW(m_hAllocator->GetAllocs(), DeviceMemoryBlock)();
    pBlock->Init(
        m_hAllocator,
        this,
        m_HeapType,
        heap,
        blockSize,
        m_NextBlockId++,
        m_Algorithm);

    m_Blocks.push_back(pBlock);
    if(pNewBlockIndex != NULL)
    {
        *pNewBlockIndex = m_Blocks.size() - 1;
    }

    return hr;
}

HRESULT BlockVector::CreateD3d12Heap(ID3D12Heap*& outHeap, UINT64 size) const
{
    D3D12_HEAP_DESC heapDesc = {};
    heapDesc.SizeInBytes = size;
    heapDesc.Properties.Type = m_HeapType;
    heapDesc.Alignment = HeapFlagsToAlignment(m_HeapFlags);
    heapDesc.Flags = m_HeapFlags;

    ID3D12Heap* heap = NULL;
    return m_hAllocator->GetDevice()->CreateHeap(&heapDesc, IID_PPV_ARGS(&outHeap));
}

////////////////////////////////////////////////////////////////////////////////
// Private class AllocatorPimpl implementation

AllocatorPimpl::AllocatorPimpl(const ALLOCATION_CALLBACKS& allocationCallbacks, const ALLOCATOR_DESC& desc) :
    m_UseMutex((desc.Flags & ALLOCATOR_FLAG_EXTERNALLY_SYNCHRONIZED) == 0),
    m_Device(desc.pDevice),
    m_PreferredBlockSize(desc.PreferredBlockSize != 0 ? desc.PreferredBlockSize : D3D12MA_DEFAULT_BLOCK_SIZE),
    m_AllocationCallbacks(allocationCallbacks)
{
    // desc.pAllocationCallbacks intentionally ignored here, preprocessed by CreateAllocator.
    ZeroMemory(&m_D3D12Options, sizeof(m_D3D12Options));

    ZeroMemory(m_pDedicatedAllocations, sizeof(m_pDedicatedAllocations));
    ZeroMemory(m_BlockVectors, sizeof(m_BlockVectors));

    for(UINT heapTypeIndex = 0; heapTypeIndex < HEAP_TYPE_COUNT; ++heapTypeIndex)
    {
        m_pDedicatedAllocations[heapTypeIndex] = D3D12MA_NEW(GetAllocs(), AllocationVectorType)(GetAllocs());
    }
}

HRESULT AllocatorPimpl::Init()
{
    HRESULT hr = m_Device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &m_D3D12Options, sizeof(m_D3D12Options));
    if(FAILED(hr))
    {
        return hr;
    }

    const UINT defaultPoolCount = CalcDefaultPoolCount();
    for(UINT i = 0; i < defaultPoolCount; ++i)
    {
        D3D12_HEAP_TYPE heapType;
        D3D12_HEAP_FLAGS heapFlags;
        CalcDefaultPoolParams(heapType, heapFlags, i);

        m_BlockVectors[i] = D3D12MA_NEW(GetAllocs(), BlockVector)(
            this, // hAllocator
            NULL, // parentPool
            heapType, // heapType
            heapFlags, // heapFlags
            m_PreferredBlockSize,
            0, // minBlockCount
            SIZE_MAX, // maxBlockCount
            0, // TODO frameInUseCount
            false, // isCustomPool
            false, // explicitBlockSize
            0); // TODO algorithm
        // No need to call m_pBlockVectors[i]->CreateMinBlocks here, becase minBlockCount is 0.
    }

    return S_OK;
}

AllocatorPimpl::~AllocatorPimpl()
{
    for(UINT i = DEFAULT_POOL_MAX_COUNT; i--; )
    {
        D3D12MA_DELETE(GetAllocs(), m_BlockVectors[i]);
    }

    for(UINT i = HEAP_TYPE_COUNT; i--; )
    {
        if(m_pDedicatedAllocations[i] && !m_pDedicatedAllocations[i]->empty())
        {
            D3D12MA_ASSERT(0 && "Unfreed dedicated allocations found.");
        }

        D3D12MA_DELETE(GetAllocs(), m_pDedicatedAllocations[i]);
    }
}

HRESULT AllocatorPimpl::CreateResource(
    const ALLOCATION_DESC* pAllocDesc,
    const D3D12_RESOURCE_DESC* pResourceDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE *pOptimizedClearValue,
    Allocation** ppAllocation,
    REFIID riidResource,
    void** ppvResource)
{
    if(pAllocDesc->HeapType != D3D12_HEAP_TYPE_DEFAULT &&
        pAllocDesc->HeapType != D3D12_HEAP_TYPE_UPLOAD &&
        pAllocDesc->HeapType != D3D12_HEAP_TYPE_READBACK)
    {
        return E_INVALIDARG;
    }

    ALLOCATION_DESC finalAllocDesc = *pAllocDesc;

    *ppvResource = NULL;

    D3D12_RESOURCE_ALLOCATION_INFO resAllocInfo = m_Device->GetResourceAllocationInfo(0, 1, pResourceDesc);
    D3D12MA_ASSERT(IsPow2(resAllocInfo.Alignment));
    D3D12MA_ASSERT(resAllocInfo.SizeInBytes > 0);

    const UINT defaultPoolIndex = CalcDefaultPoolIndex(*pAllocDesc, *pResourceDesc);
    BlockVector* blockVector = m_BlockVectors[defaultPoolIndex];
    D3D12MA_ASSERT(blockVector);

    const UINT64 preferredBlockSize = blockVector->GetPreferredBlockSize();
    bool preferDedicatedMemory =
        D3D12MA_DEBUG_ALWAYS_DEDICATED_MEMORY ||
        PrefersDedicatedAllocation(*pResourceDesc) ||
        // Heuristics: Allocate dedicated memory if requested size if greater than half of preferred block size.
        resAllocInfo.SizeInBytes > preferredBlockSize / 2;
    if(preferDedicatedMemory &&
        (finalAllocDesc.Flags & ALLOCATION_FLAG_NEVER_ALLOCATE) == 0)
    {
        finalAllocDesc.Flags |= ALLOCATION_FLAG_DEDICATED_MEMORY;
    }

    if((finalAllocDesc.Flags & ALLOCATION_FLAG_DEDICATED_MEMORY) != 0)
    {
        return AllocateDedicatedMemory(
            &finalAllocDesc,
            pResourceDesc,
            resAllocInfo,
            InitialResourceState,
            pOptimizedClearValue,
            ppAllocation,
            riidResource,
            ppvResource);
    }
    else
    {
        HRESULT hr = blockVector->Allocate(
            0, // TODO currentFrameIndex
            resAllocInfo.SizeInBytes,
            resAllocInfo.Alignment,
            finalAllocDesc,
            1,
            (Allocation**)ppAllocation);
        if(SUCCEEDED(hr))
        {
            hr = m_Device->CreatePlacedResource(
                (*ppAllocation)->GetBlock()->GetHeap(),
                (*ppAllocation)->GetOffset(),
                pResourceDesc,
                InitialResourceState,
                pOptimizedClearValue,
                riidResource,
                ppvResource);
            if(SUCCEEDED(hr))
            {
                return hr;
            }
            else
            {
                (*ppAllocation)->Release();
                return hr;
            }
        }

        return AllocateDedicatedMemory(
            &finalAllocDesc,
            pResourceDesc,
            resAllocInfo,
            InitialResourceState,
            pOptimizedClearValue,
            ppAllocation,
            riidResource,
            ppvResource);
    }
}

bool AllocatorPimpl::PrefersDedicatedAllocation(const D3D12_RESOURCE_DESC& resourceDesc)
{
    // Intentional. It may change in the future.
    return false;
}

HRESULT AllocatorPimpl::AllocateDedicatedMemory(
    const ALLOCATION_DESC* pAllocDesc,
    const D3D12_RESOURCE_DESC* pResourceDesc,
    const D3D12_RESOURCE_ALLOCATION_INFO& resAllocInfo,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE *pOptimizedClearValue,
    Allocation** ppAllocation,
    REFIID riidResource,
    void** ppvResource)
{
    if((pAllocDesc->Flags & ALLOCATION_FLAG_NEVER_ALLOCATE) != 0)
    {
        return E_OUTOFMEMORY;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = pAllocDesc->HeapType;
    HRESULT hr = m_Device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, pResourceDesc, InitialResourceState,
        pOptimizedClearValue, riidResource, ppvResource);
    if(SUCCEEDED(hr))
    {
        Allocation* alloc = D3D12MA_NEW(m_AllocationCallbacks, Allocation)();
        alloc->InitCommitted(this, resAllocInfo.SizeInBytes, pAllocDesc->HeapType);
        *ppAllocation = alloc;

        const UINT heapTypeIndex = HeapTypeToIndex(pAllocDesc->HeapType);

        {
            MutexLockWrite lock(m_DedicatedAllocationsMutex[heapTypeIndex], m_UseMutex);
            AllocationVectorType* const dedicatedAllocations = m_pDedicatedAllocations[heapTypeIndex];
            D3D12MA_ASSERT(dedicatedAllocations);
            dedicatedAllocations->InsertSorted(alloc, PointerLess());
        }
    }
    return hr;
}

UINT AllocatorPimpl::CalcDefaultPoolCount() const
{
    if(SupportsResourceHeapTier2())
    {
        return 3;
    }
    else
    {
        return 9;
    }
}

UINT AllocatorPimpl::CalcDefaultPoolIndex(const ALLOCATION_DESC& allocDesc, const D3D12_RESOURCE_DESC& resourceDesc) const
{
    UINT poolIndex = UINT_MAX;
    switch(allocDesc.HeapType)
    {
    case D3D12_HEAP_TYPE_DEFAULT:  poolIndex = 0; break;
    case D3D12_HEAP_TYPE_UPLOAD:   poolIndex = 1; break;
    case D3D12_HEAP_TYPE_READBACK: poolIndex = 2; break;
    default: D3D12MA_ASSERT(0);
    }

    if(!SupportsResourceHeapTier2())
    {
        poolIndex *= 3;
        if(resourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            ++poolIndex;
            const bool isRenderTargetOrDepthStencil =
                (resourceDesc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
            if(isRenderTargetOrDepthStencil)
            {
                ++poolIndex;
            }
        }
    }

    return poolIndex;
}

void AllocatorPimpl::CalcDefaultPoolParams(D3D12_HEAP_TYPE& outHeapType, D3D12_HEAP_FLAGS& outHeapFlags, UINT index) const
{
    outHeapType = D3D12_HEAP_TYPE_DEFAULT;
    outHeapFlags = D3D12_HEAP_FLAG_NONE;

    if(!SupportsResourceHeapTier2())
    {
        switch(index % 3)
        {
        case 0:
            outHeapFlags = D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES | D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
            break;
        case 1:
            outHeapFlags = D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
            break;
        case 2:
            outHeapFlags = D3D12_HEAP_FLAG_DENY_BUFFERS | D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
            break;
        }

        index /= 3;
    }

    switch(index)
    {
    case 0:
        outHeapType = D3D12_HEAP_TYPE_DEFAULT;
        break;
    case 1:
        outHeapType = D3D12_HEAP_TYPE_UPLOAD;
        break;
    case 2:
        outHeapType = D3D12_HEAP_TYPE_READBACK;
        break;
    default:
        D3D12MA_ASSERT(0);
    }
}

void AllocatorPimpl::FreeDedicatedMemory(Allocation* allocation)
{
    D3D12MA_ASSERT(allocation && allocation->m_Type == Allocation::TYPE_COMMITTED);
    const UINT heapTypeIndex = HeapTypeToIndex(allocation->m_Committed.heapType);

    {
        MutexLockWrite lock(m_DedicatedAllocationsMutex[heapTypeIndex], m_UseMutex);
        AllocationVectorType* const dedicatedAllocations = m_pDedicatedAllocations[heapTypeIndex];
        D3D12MA_ASSERT(dedicatedAllocations);
        bool success = dedicatedAllocations->RemoveSorted(allocation, PointerLess());
        D3D12MA_ASSERT(success);
    }
}

void AllocatorPimpl::FreePlacedMemory(Allocation* allocation)
{
    D3D12MA_ASSERT(allocation && allocation->m_Type == Allocation::TYPE_PLACED);
    
    DeviceMemoryBlock* const block = allocation->GetBlock();
    D3D12MA_ASSERT(block);
    BlockVector* const blockVector = block->GetBlockVector();
    D3D12MA_ASSERT(blockVector);
    blockVector->Free(allocation);
}


////////////////////////////////////////////////////////////////////////////////
// Public class Allocation implementation

void Allocation::Release()
{
    if(this == NULL)
    {
        return;
    }

    switch(m_Type)
    {
    case TYPE_COMMITTED:
        m_Allocator->FreeDedicatedMemory(this);
        break;
    case TYPE_PLACED:
        m_Allocator->FreePlacedMemory(this);
        break;
    }

    D3D12MA_DELETE(m_Allocator->GetAllocs(), this);
}

UINT64 Allocation::GetOffset()
{
    switch(m_Type)
    {
    case TYPE_COMMITTED:
        return 0;
    case TYPE_PLACED:
        return m_Placed.offset;
    default:
        D3D12MA_ASSERT(0);
        return 0;
    }
}

ID3D12Heap* Allocation::GetHeap()
{
    switch(m_Type)
    {
    case TYPE_COMMITTED:
        return NULL;
    case TYPE_PLACED:
        return m_Placed.block->GetHeap();
    default:
        D3D12MA_ASSERT(0);
        return 0;
    }
}

Allocation::Allocation()
{
    // Must be empty because Allocation objects are allocated out of PoolAllocator
    // and may not call constructor and destructor at the right time.
    // Use Init* methods instead.
}

Allocation::~Allocation()
{
    // Must be empty because Allocation objects are allocated out of PoolAllocator
    // and may not call constructor and destructor at the right time.
    // Use Release method instead.
}

void Allocation::InitCommitted(AllocatorPimpl* allocator, UINT64 size, D3D12_HEAP_TYPE heapType)
{
    m_Allocator = allocator;
    m_Type = TYPE_COMMITTED;
    m_Size = size;
    m_Committed.heapType = heapType;
}

void Allocation::InitPlaced(AllocatorPimpl* allocator, UINT64 size, UINT64 offset, UINT64 alignment, DeviceMemoryBlock* block)
{
    m_Allocator = allocator;
    m_Type = TYPE_PLACED;
    m_Size = size;
    m_Placed.offset = offset;
    m_Placed.alignment = alignment;
    m_Placed.block = block;
}

DeviceMemoryBlock* Allocation::GetBlock()
{
    D3D12MA_ASSERT(m_Type == TYPE_PLACED);
    return m_Placed.block;
}

////////////////////////////////////////////////////////////////////////////////
// Public class Allocator implementation

Allocator::Allocator(const ALLOCATION_CALLBACKS& allocationCallbacks, const ALLOCATOR_DESC& desc) :
    m_Pimpl(D3D12MA_NEW(allocationCallbacks, AllocatorPimpl)(allocationCallbacks, desc))
{
}

Allocator::~Allocator()
{
    D3D12MA_DELETE(m_Pimpl->GetAllocs(), m_Pimpl);
}

void Allocator::Release()
{
    // Copy is needed because otherwise we would call destructor and invalidate the structure with callbacks before using it to free memory.
    const ALLOCATION_CALLBACKS allocationCallbacksCopy = m_Pimpl->GetAllocs();
    D3D12MA_DELETE(allocationCallbacksCopy, this);
}



const D3D12_FEATURE_DATA_D3D12_OPTIONS& Allocator::GetD3D12Options() const
{
    return m_Pimpl->GetD3D12Options();
}

HRESULT Allocator::CreateResource(
    const ALLOCATION_DESC* pAllocDesc,
    const D3D12_RESOURCE_DESC* pResourceDesc,
    D3D12_RESOURCE_STATES InitialResourceState,
    const D3D12_CLEAR_VALUE *pOptimizedClearValue,
    Allocation** ppAllocation,
    REFIID riidResource,
    void** ppvResource)
{
    D3D12MA_ASSERT(pAllocDesc && pResourceDesc && ppAllocation && riidResource != IID_NULL && ppvResource);
    return m_Pimpl->CreateResource(pAllocDesc, pResourceDesc, InitialResourceState, pOptimizedClearValue, ppAllocation, riidResource, ppvResource);
}

void Allocator::Test()
{
#if 0
    Vector<size_t> v(m_Pimpl->GetAllocs());
    for(size_t i = 0; i < 1000; ++i)
        v.push_back(i);
#endif

#if 0
    PoolAllocator<size_t> poolAlloc{m_Pimpl->GetAllocs(), 8};
    Vector<size_t*> allocatedItems{m_Pimpl->GetAllocs()};
    for(size_t i = 0; i < 1000; ++i)
    {
        size_t* const allocatedItem = poolAlloc.Alloc();
        allocatedItems.push_back(allocatedItem);
        *allocatedItem = i * 2;
    }

    while(!allocatedItems.empty())
    {
        const size_t index = (size_t)rand() % allocatedItems.size();
        poolAlloc.Free(allocatedItems[index]);
        allocatedItems.remove(index);
    }
#endif

#if 0
    struct FooStruct { int foo[10]; };
    List<FooStruct> list(m_Pimpl->GetAllocs());
    for(size_t i = 0; i < 1000; ++i)
    {
        list.PushBack();
    }
    while(!list.empty())
    {
        if(rand() & 0x80)
            list.PopBack();
        else
            list.PopFront();
    }
#endif
}

////////////////////////////////////////////////////////////////////////////////
// Public global functions

HRESULT CreateAllocator(const ALLOCATOR_DESC* pDesc, Allocator** ppAllocator)
{
    D3D12MA_ASSERT(pDesc && ppAllocator);
    D3D12MA_ASSERT(pDesc->pDevice);
    D3D12MA_ASSERT(pDesc->PreferredBlockSize == 0 || (pDesc->PreferredBlockSize >= 16 && pDesc->PreferredBlockSize < 0x10000000000ull));

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

} // namespace D3D12MA
