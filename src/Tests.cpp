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
#include "Common.h"

extern CComPtr<ID3D12Device> g_Device;
extern D3D12MA::Allocator* g_Allocator;

struct D3D12MAAllocationDeleter
{
    void operator()(D3D12MA::Allocation* obj) const
    {
        if(obj)
        {
            obj->Release();
        }
    }
};

typedef std::unique_ptr<D3D12MA::Allocation, D3D12MAAllocationDeleter> AllocationUniquePtr;

struct ResourceInfo
{
    CComPtr<ID3D12Resource> resource;
    AllocationUniquePtr allocation;
};

static void FillResourceDescForBuffer(D3D12_RESOURCE_DESC& outResourceDesc, UINT64 size)
{
    outResourceDesc = {};
    outResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    outResourceDesc.Alignment = 0;
    outResourceDesc.Width = size;
    outResourceDesc.Height = 1;
    outResourceDesc.DepthOrArraySize = 1;
    outResourceDesc.MipLevels = 1;
    outResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    outResourceDesc.SampleDesc.Count = 1;
    outResourceDesc.SampleDesc.Quality = 0;
    outResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    outResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
}

static void TestCommittedResources()
{
    wprintf(L"Test committed resources\n");
    
    const UINT count = 4;
    ResourceInfo resources[count];

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;
    allocDesc.Flags = D3D12MA::ALLOCATION_FLAG_DEDICATED_MEMORY;

    D3D12_RESOURCE_DESC resourceDesc;
    FillResourceDescForBuffer(resourceDesc, 32ull * 1024);

    for(size_t i = 0; i < count; ++i)
    {
        D3D12MA::Allocation* alloc = nullptr;
        CHECK_HR( g_Allocator->CreateResource(
            &allocDesc,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            NULL,
            &alloc,
            IID_PPV_ARGS(&resources[i].resource)) );
        resources[i].allocation.reset(alloc);
        
        // Make sure it has implicit heap.
        CHECK_BOOL( resources[i].allocation->GetHeap() == NULL && resources[i].allocation->GetOffset() == 0 );
    }
}

static void TestPlacedResources()
{
    wprintf(L"Test placed resources\n");

    const UINT count = 4;
    ResourceInfo resources[count];

    D3D12MA::ALLOCATION_DESC allocDesc = {};
    allocDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resourceDesc;
    FillResourceDescForBuffer(resourceDesc, 32ull * 1024);

    for(size_t i = 0; i < count; ++i)
    {
        D3D12MA::Allocation* alloc = nullptr;
        CHECK_HR( g_Allocator->CreateResource(
            &allocDesc,
            &resourceDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            NULL,
            &alloc,
            IID_PPV_ARGS(&resources[i].resource)) );
        resources[i].allocation.reset(alloc);

        // Make sure it doesn't have implicit heap.
        CHECK_BOOL( resources[i].allocation->GetHeap() != NULL );
    }

    // Make sure at least some of the resources belong to the same heap, but their memory ranges don't overlap.
    bool sameHeapFound = false;
    for(size_t i = 0; i < count; ++i)
    {
        for(size_t j = i + 1; j < count; ++j)
        {
            const ResourceInfo& resInfoI = resources[i];
            const ResourceInfo& resInfoJ = resources[j];
            if(resInfoI.allocation->GetHeap() != NULL &&
                resInfoI.allocation->GetHeap() == resInfoJ.allocation->GetHeap())
            {
                sameHeapFound = true;
                CHECK_BOOL(resInfoI.allocation->GetOffset() + resInfoI.allocation->GetSize() <= resInfoJ.allocation->GetOffset() ||
                    resInfoJ.allocation->GetOffset() + resInfoJ.allocation->GetSize() <= resInfoI.allocation->GetOffset());
            }
        }
    }
    CHECK_BOOL(sameHeapFound);
}

static void TestGroupBasics()
{
    TestCommittedResources();
    TestPlacedResources();
}

void Test()
{
    wprintf(L"TESTS BEGIN\n");

    TestGroupBasics();

    wprintf(L"TESTS END\n");
}
