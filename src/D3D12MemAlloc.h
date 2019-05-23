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

/** \mainpage Direct3D 12 Memory Allocator

<b>Version 0.0.1-development</b> (2019-05-23)

Copyright (c) 2019 Advanced Micro Devices, Inc. All rights reserved. \n
License: MIT

Documentation of all members: D3D12MemAlloc.h

\section main_table_of_contents Table of contents

- <b>User guide</b>
    - \subpage quick_start
        - [Project setup](@ref quick_start_project_setup)

\section main_see_also See also

- [Product page on GPUOpen](https://gpuopen.com/gaming-product/direct3d12-memory-allocator/) (TODO not implemented yet)
- [Source repository on GitHub](https://github.com/GPUOpen-LibrariesAndSDKs/Direct3D12MemoryAllocator) (TODO not implemented yet)


\page quick_start Quick start

\section quick_start_project_setup Project setup

Project setup goes here TODO...
*/

#include <d3d12.h>

/// \cond INTERNAL
#define D3D12MA_CLASS_NO_COPY(className) \
    private: \
        className(const className&) = delete; \
        className(className&&) = delete; \
        className& operator=(const className&) = delete; \
        className& operator=(className&&) = delete;
#define FACILITY_D3D12MA 3542
/// \endcond

/// Test error code TODO remove
#define D3D12MA_E_TEST_ERROR   MAKE_HRESULT(SEVERITY_ERROR, FACILITY_D3D12MA, 1)

namespace D3D12MA
{

/// \brief Bit flags to be used with ALLOCATION_DESC::Flags.
typedef enum ALLOCATION_FLAGS
{
    /** \brief Set this flag if the allocation should have its own memory heap.
    
    Use it for special, big resources, like fullscreen textures used as render targets.
   
    You should not use this flag if ALLOCATION_DESC::pPool is not null. (TODO not yet implemented)
    */
    ALLOCATION_FLAG_DEDICATED_MEMORY = 0x1,

    /** \brief Set this flag to only try to allocate from existing memory heaps and never create new such heap.

    If new allocation cannot be placed in any of the existing heaps, allocation
    fails with `TODO` error.

    You should not use #ALLOCATION_FLAG_DEDICATED_MEMORY and
    #ALLOCATION_FLAG_NEVER_ALLOCATE at the same time. It makes no sense.

    If ALLOCATION_DESC::pPool is not null, this flag is implied and ignored. (TODO not yet implemented)
    */
    ALLOCATION_FLAG_NEVER_ALLOCATE = 0x2,

    /** \brief Set this flag to use a memory that will be persistently mapped and retrieve pointer to it.

    Pointer to mapped memory will be returned through ALLOCATION_INFO::pMappedData.
    */
    ALLOCATION_FLAG_MAPPED = 0x4,
} ALLOCATION_FLAGS;

/// \brief Parameters of created Allocation object. To be used with Allocator::CreateResource.
struct ALLOCATION_DESC
{
    /// Use #ALLOCATION_FLAGS.
    UINT Flags;
    /// The type of memory heap where the new allocation should be placed.
    D3D12_HEAP_TYPE HeapType;
};

/** \brief Represents single memory allocation.

It may be either implicit memory heap dedicated to a single resource or a
specific region of a bigger heap plus unique offset.

To create such object, fill structure D3D12MA::ALLOCATION_DESC and call function
Allocator::CreateResource.

The object also remembers size and some other information.
To retrieve this information, use methods of this class.
*/
class Allocation
{
public:
    /** \brief Deletes this object.

    This function must be used instead of destructor, which is private.
    There is no reference counting involved.
    */
    void Release();

private:
    ~Allocation();

    D3D12MA_CLASS_NO_COPY(Allocation)
};

/// \brief Bit flags to be used with ALLOCATOR_DESC::Flags.
typedef enum ALLOCATOR_FLAGS
{
    /** \brief Allocator and all objects created from it will not be synchronized internally, so you must guarantee they are used from only one thread at a time or synchronized externally by you.

    Using this flag may increase performance because internal mutexes are not used.
    */
    ALLOCATOR_FLAG_EXTERNALLY_SYNCHRONIZED = 0x1,
} ALLOCATOR_FLAGS;

/// Pointer to custom callback function that allocates CPU memory.
typedef void* (*ALLOCATE_FUNC_PTR)(size_t Size, size_t Alignment, void* pUserData);
/// Pointer to custom callback function that deallocates CPU memory. pMemory = null show be accepted and ignored.
typedef void (*FREE_FUNC_PTR)(void* pMemory, void* pUserData);

/// Custom callbacks to CPU memory allocation functions.
struct ALLOCATION_CALLBACKS
{
    /// Allocation function.
    ALLOCATE_FUNC_PTR pAllocate;
    /// Dellocation function.
    FREE_FUNC_PTR pFree;
    /// Custom data that will be passed to allocation and deallocation functions as `pUserData` parameter.
    void* pUserData;
};

/// \brief Parameters of created Allocator object. To be used with CreateAllocator().
struct ALLOCATOR_DESC
{
    /// Use #ALLOCATOR_FLAGS
    UINT Flags;
    
    /// Direct3D device object that the allocator should be attached to.
    ID3D12Device* pDevice;
    
    /** \brief Preferred size of a single `ID3D12Heap` block to be allocated from large heaps > 1 GiB. Optional.
    
    Set to 0 to use default, which is currently 256 MiB.
    */
    UINT64 PreferredLargeHeapBlockSize;
    
    /** \brief Custom CPU memory allocation callbacks. Optional.

    Optional, can be null. When specified, will be used for all CPU-side memory allocations.
    */
    const ALLOCATION_CALLBACKS* pAllocationCallbacks;
};

/// \cond INTERNAL
class AllocatorPimpl;
/// \endcond

/**
\brief Represents main object of this library initialized.

Fill structure D3D12MA::ALLOCATOR_DESC and call function CreateAllocator() to create it.
Call method Allocator::Release to destroy it.

It is recommended to create just one object of this type per `ID3D12Device` object,
right after Direct3D 12 is initialized and keep it alive until before Direct3D device is destroyed.
*/
class Allocator
{
public:
    /** \brief Deletes this object.
    
    This function must be used instead of destructor, which is private.
    There is no reference counting involved.
    */
    void Release();
    
    /** \brief Allocates memory and creates a D3D12 resource. This is the main allocation function.

    */
    HRESULT CreateResource(
        const ALLOCATION_DESC* pAllocDesc,
        const D3D12_RESOURCE_DESC* pResourceDesc,
        D3D12_RESOURCE_STATES InitialResourceState,
        const D3D12_CLEAR_VALUE *pOptimizedClearValue,
        Allocation** ppAllocation,
        REFIID riidResource,
        void** ppvResource);

    /// Returns cached options retrieved from D3D12 device.
    const D3D12_FEATURE_DATA_D3D12_OPTIONS& GetD3D12Options() const;

    /// \cond INTERNAL
    void Test();
    /// \endcond

private:
    friend HRESULT CreateAllocator(const ALLOCATOR_DESC*, Allocator**);
    template<typename T> friend void D3D12MA_DELETE(const ALLOCATION_CALLBACKS&, T*);

    Allocator(const ALLOCATION_CALLBACKS& allocationCallbacks, const ALLOCATOR_DESC& desc);
    ~Allocator();
    
    AllocatorPimpl* m_Pimpl;
    
    D3D12MA_CLASS_NO_COPY(Allocator)
};

/** \brief Creates new main Allocator object and returns it through ppAllocator.

You normally only need to call it once and keep a single Allocator object for your `ID3D12Device`.
*/
HRESULT CreateAllocator(const ALLOCATOR_DESC* pDesc, Allocator** ppAllocator);

} // namespace D3D12MA

/// \cond INTERNAL
DEFINE_ENUM_FLAG_OPERATORS(D3D12MA::ALLOCATION_FLAGS);
DEFINE_ENUM_FLAG_OPERATORS(D3D12MA::ALLOCATOR_FLAGS);
/// \endcond
