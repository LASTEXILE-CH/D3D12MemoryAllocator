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
