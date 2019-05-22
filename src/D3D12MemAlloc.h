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

#pragma once

#include <d3d12.h>

#define D3D12MA_CLASS_NO_COPY(className) \
    private: \
        className(const className&) = delete; \
        className(className&&) = delete; \
        className& operator=(const className&) = delete; \
        className& operator=(className&&) = delete;

namespace D3D12MA
{

typedef enum ALLOCATION_FLAGS
{
    ALLOCATION_FLAG_DEDICATED_MEMORY = 0x1,
    ALLOCATION_FLAG_MAPPED = 0x2,
} ALLOCATION_FLAGS;

struct ALLOCATION_DESC
{
    ALLOCATION_FLAGS Flags;
    D3D12_HEAP_TYPE HeapType;
};

class Allocation
{
public:
    ~Allocation();

private:
    D3D12MA_CLASS_NO_COPY(Allocation)
};

typedef enum ALLOCATOR_FLAGS
{
    ALLOCATOR_FLAG_EXTERNALLY_SYNCHRONIZED = 0x1,
} ALLOCATOR_FLAGS;

typedef void* (*ALLOCATE_FUNC_PTR)(size_t Size, size_t Alignment, void* pUserData);
typedef void (*FREE_FUNC_PTR)(void* pMemory, void* pUserData);

struct ALLOCATION_CALLBACKS
{
    ALLOCATE_FUNC_PTR pAllocate;
    FREE_FUNC_PTR pFree;
    void* pUserData;
};

struct ALLOCATOR_DESC
{
    // Use @ALLOCATOR_FLAGS
    UINT Flags;
    ID3D12Device* pDevice;
    /// Preferred size of a single `ID3D12Heap` block to be allocated from large heaps > 1 GiB. Optional.
    /** Set to 0 to use default, which is currently 256 MiB. */
    UINT64 PreferredLargeHeapBlockSize;
    /// Custom CPU memory allocation callbacks. Optional.
    /** Optional, can be null. When specified, will be used for all CPU-side memory allocations. */
    const ALLOCATION_CALLBACKS* pAllocationCallbacks;
};

class AllocatorPimpl;

class Allocator
{
public:
    HRESULT CreateResource(
        const ALLOCATION_DESC* pAllocDesc,
        const D3D12_RESOURCE_DESC* pResourceDesc,
        D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE *pOptimizedClearValue,
        Allocation** ppAllocation,
        REFIID riidResource,
        void** ppvResource);

    void Test();

private:
    friend HRESULT CreateAllocator(const ALLOCATOR_DESC*, Allocator**);
    friend void DestroyAllocator(Allocator*);
    template<typename T> friend void D3D12MA_DELETE(const ALLOCATION_CALLBACKS&, T*);

    Allocator(const ALLOCATION_CALLBACKS& allocationCallbacks, const ALLOCATOR_DESC& desc);
    ~Allocator();
    
    AllocatorPimpl* m_Pimpl;
    
    D3D12MA_CLASS_NO_COPY(Allocator)
};

HRESULT CreateAllocator(const ALLOCATOR_DESC* pDesc, Allocator** ppAllocator);
void DestroyAllocator(Allocator* pAllocator);

} // namespace D3D12MA

DEFINE_ENUM_FLAG_OPERATORS(D3D12MA::ALLOCATION_FLAGS);
DEFINE_ENUM_FLAG_OPERATORS(D3D12MA::ALLOCATOR_FLAGS);
