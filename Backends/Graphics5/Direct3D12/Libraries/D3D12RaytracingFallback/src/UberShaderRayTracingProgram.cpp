//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
#include "pch.h"
#include "CompiledShaders\StateMachineLib.h"

namespace FallbackLayer
{
    void CompilePSO(ID3D12Device *pDevice, D3D12_SHADER_BYTECODE shaderByteCode, const StateObjectCollection &stateObjectCollection, ID3D12PipelineState **ppPipelineState)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.CS = CD3DX12_SHADER_BYTECODE(shaderByteCode);
        psoDesc.NodeMask = stateObjectCollection.m_nodeMask;
        psoDesc.pRootSignature = stateObjectCollection.m_pGlobalRootSignature;

        ThrowInternalFailure(pDevice->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(ppPipelineState)));
    }

    StateIdentifier UberShaderRaytracingProgram::GetStateIdentfier(LPCWSTR pExportName)
    {
        StateIdentifier id = 0;
        if (pExportName)
        {
            auto shaderIdentifier = m_ExportNameToShaderIdentifier.find(pExportName);
            if (shaderIdentifier != m_ExportNameToShaderIdentifier.end())
            {
                id = shaderIdentifier->second.StateId;
            }
            else
            {
                ThrowFailure(E_INVALIDARG, L"Hit group is referring to a shader name that wasn't found in the state object");
            }
        }
        return id;
    }


    UberShaderRaytracingProgram::UberShaderRaytracingProgram(ID3D12Device *pDevice, DxilShaderPatcher &dxilShaderPatcher, const StateObjectCollection &stateObjectCollection) :
        m_DxilShaderPatcher(dxilShaderPatcher)
    {
        UINT numLibraries = (UINT)stateObjectCollection.m_dxilLibraries.size();

        UINT numShaders = 0;
        for (auto &lib : stateObjectCollection.m_dxilLibraries)
        {
            numShaders += lib.NumExports;
        }

        std::vector<DxilLibraryInfo> librariesInfo;
        std::vector<LPCWSTR> exportNames;

        std::vector<CComPtr<IDxcBlob>> patchedBlobList;

        UINT cbvSrvUavHandleSize = pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        UINT samplerHandleSize = pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        
        ViewKey SRVViewsList[FallbackLayerNumDescriptorHeapSpacesPerView];
        UINT SRVsUsed = 0;
        ViewKey UAVViewsList[FallbackLayerNumDescriptorHeapSpacesPerView];
        UINT UAVsUsed = 0;
        
        for (UINT i = 0; i < numLibraries; i++)
        {
            auto &library = stateObjectCollection.m_dxilLibraries[i];
            if (library.NumExports > 0)
            {
                DxilLibraryInfo outputLibInfo((void *)library.DXILLibrary.pShaderBytecode, (UINT)library.DXILLibrary.BytecodeLength);
                CComPtr<IDxcBlob> pOutputBlob;
                for (UINT exportIndex = 0; exportIndex < library.NumExports; exportIndex++)
                {
                    std::wstring exportName = library.pExports[exportIndex].Name;
                    auto pShaderAssociation = stateObjectCollection.m_shaderAssociations.find(exportName);
                    
                    if (pShaderAssociation == stateObjectCollection.m_shaderAssociations.end())
                    {
                        for (auto &hitgroup : stateObjectCollection.m_hitGroups)
                        {
                            LPCWSTR imports[] = {
                                hitgroup.second.ClosestHitShaderImport,
                                hitgroup.second.AnyHitShaderImport,
                                hitgroup.second.IntersectionShaderImport
                            };
                            for(auto hitgroupImport : imports)
                            {
                                if (hitgroupImport && exportName == std::wstring(hitgroupImport))
                                {
                                    pShaderAssociation = stateObjectCollection.m_shaderAssociations.find(hitgroup.first);
                                }
                            }
                        }
                    }

                    if (pShaderAssociation != stateObjectCollection.m_shaderAssociations.end() && pShaderAssociation->second.m_pRootSignature)
                    {
                        CComPtr<ID3D12VersionedRootSignatureDeserializer> pDeserializer;
                        ShaderInfo shaderInfo;
                        shaderInfo.pRootSignatureDesc = GetDescFromRootSignature(pShaderAssociation->second.m_pRootSignature, pDeserializer);
                        shaderInfo.pSRVRegisterSpaceArray = SRVViewsList;
                        shaderInfo.pNumSRVSpaces = &SRVsUsed;
                        shaderInfo.pUAVRegisterSpaceArray = UAVViewsList;
                        shaderInfo.pNumUAVSpaces = &UAVsUsed;

                        if (GetNumParameters(*shaderInfo.pRootSignatureDesc) > 0)
                        {
                            shaderInfo.SamplerDescriptorSizeInBytes = samplerHandleSize;
                            shaderInfo.SrvCbvUavDescriptorSizeInBytes = cbvSrvUavHandleSize;
                            shaderInfo.ShaderRecordIdentifierSizeInBytes = sizeof(ShaderIdentifier);
                            shaderInfo.ExportName = exportName.c_str();

                            CComPtr<IDxcBlob> pPatchedBlob;
                            m_DxilShaderPatcher.PatchShaderBindingTables(
                                (const BYTE *)outputLibInfo.pByteCode,
                                (UINT)outputLibInfo.BytecodeLength,
                                &shaderInfo,
                                &pPatchedBlob);

                            pOutputBlob = pPatchedBlob;
                            outputLibInfo = DxilLibraryInfo(pOutputBlob->GetBufferPointer(), pOutputBlob->GetBufferSize());
                        }
                    }
                }
                patchedBlobList.push_back(pOutputBlob);
                librariesInfo.emplace_back(outputLibInfo);

            }

            for(UINT exportIndex = 0; exportIndex < library.NumExports; exportIndex++)
              exportNames.push_back(library.pExports[exportIndex].Name);
        }

        {
            auto &traversalShader = stateObjectCollection.m_traversalShader.DXILLibrary;
            librariesInfo.emplace_back((void *)traversalShader.pShaderBytecode, traversalShader.BytecodeLength);
            exportNames.push_back(L"Fallback_TraceRay");
        }

        {
            librariesInfo.emplace_back((void *)g_pStateMachineLib, ARRAYSIZE(g_pStateMachineLib));
        }

        std::vector<FallbackLayer::StateIdentifier> stateIdentifiers;
        CComPtr<IDxcBlob> pLinkedBlob;
        UINT stackSize = (UINT)stateObjectCollection.m_pipelineStackSize;
        if (stackSize == 0 && (stateObjectCollection.IsUsingAnyHit || stateObjectCollection.IsUsingIntersection))
        {
            // TODO: The stack size used by the traversal shader is high when it's split by a continuation from the
            // Intersection shader or the Anyhit. Currently setting a higher hard-coded value, this can go-away
            // once API-specified stack-sizes are supported
            stackSize = 2048;
        }

        m_DxilShaderPatcher.LinkShaders(stackSize, librariesInfo, exportNames, stateIdentifiers, &pLinkedBlob);

        for (size_t i = 0; i < exportNames.size(); ++i)
        {
            m_ExportNameToShaderIdentifier[exportNames[i]] = { stateIdentifiers[i], 0 };
        }

        for (auto &hitGroupMapEntry : stateObjectCollection.m_hitGroups)
        {
            auto closestHitName = hitGroupMapEntry.second.ClosestHitShaderImport;
            auto anyHitName = hitGroupMapEntry.second.AnyHitShaderImport;
            auto intersectionName = hitGroupMapEntry.second.IntersectionShaderImport;

            ShaderIdentifier shaderId = {};
            shaderId.StateId = GetStateIdentfier(closestHitName);
            shaderId.AnyHitId = GetStateIdentfier(anyHitName);
            shaderId.IntersectionShaderId = GetStateIdentfier(intersectionName);

            auto hitGroupName = hitGroupMapEntry.first;
            m_ExportNameToShaderIdentifier[hitGroupName] = shaderId;
        }

        CompilePSO(
            pDevice, 
            CD3DX12_SHADER_BYTECODE(pLinkedBlob->GetBufferPointer(), pLinkedBlob->GetBufferSize()), 
            stateObjectCollection, 
            &m_pRayTracePSO);
        
        UINT sizeOfParamterStart = sizeof(m_patchRootSignatureParameterStart);
        ThrowFailure(stateObjectCollection.m_pGlobalRootSignature->GetPrivateData(
            FallbackLayerPatchedParameterStartGUID,
            &sizeOfParamterStart,
            &m_patchRootSignatureParameterStart),
            L"Root signatures in a state object must be created through "
            L"Fallback Layer-specific interaces. Either use RaytracingDevice::D3D12SerializeRootSignature "
            L"or RaytracingDevice::D3D12SerializeFallbackRootSignature and create with "
            L"RaytracingDevice::CreateRootSignature");
    }

    ShaderIdentifier *UberShaderRaytracingProgram::GetShaderIdentifier(LPCWSTR pExportName)
    {
        auto pEntry = m_ExportNameToShaderIdentifier.find(pExportName);
        if (pEntry == m_ExportNameToShaderIdentifier.end())
        {
            return nullptr;
        }
        else
        {
            // Handing out this pointer is safe because the map is read-only at this point
            return &pEntry->second;
        }
    }

    void UberShaderRaytracingProgram::DispatchRays(
        ID3D12GraphicsCommandList *pCommandList, 
        ID3D12DescriptorHeap *pSrvCbvUavDescriptorHeap,
        ID3D12DescriptorHeap *pSamplerDescriptorHeap,
        const std::unordered_map<UINT, WRAPPED_GPU_POINTER> &boundAccelerationStructures,
        const D3D12_FALLBACK_DISPATCH_RAYS_DESC &desc)
    {
        assert(pSrvCbvUavDescriptorHeap);
        pCommandList->SetComputeRootDescriptorTable(m_patchRootSignatureParameterStart + CbvSrvUavDescriptorHeapAliasedTables, pSrvCbvUavDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

        if (pSamplerDescriptorHeap)
        {
            pCommandList->SetComputeRootDescriptorTable(m_patchRootSignatureParameterStart + SamplerDescriptorHeapAliasedTables, pSamplerDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
        }
        if (desc.HitGroupTable.StartAddress)
        {
            pCommandList->SetComputeRootShaderResourceView(m_patchRootSignatureParameterStart + HitGroupRecord, desc.HitGroupTable.StartAddress);
        }
        if (desc.MissShaderTable.StartAddress)
        {
            pCommandList->SetComputeRootShaderResourceView(m_patchRootSignatureParameterStart + MissShaderRecord, desc.MissShaderTable.StartAddress);
        }
        if (desc.RayGenerationShaderRecord.StartAddress)
        {
            pCommandList->SetComputeRootShaderResourceView(m_patchRootSignatureParameterStart + RayGenShaderRecord, desc.RayGenerationShaderRecord.StartAddress);
        }
        if (desc.CallableShaderTable.StartAddress)
        {
            pCommandList->SetComputeRootShaderResourceView(m_patchRootSignatureParameterStart + CallableShaderRecord, desc.CallableShaderTable.StartAddress);
        }

        DispatchRaysConstants constants;
        constants.RayDispatchDimensionsWidth = desc.Width;
        constants.RayDispatchDimensionsHeight = desc.Height;
        constants.HitGroupShaderRecordStride = static_cast<UINT>(desc.HitGroupTable.StrideInBytes);
        constants.MissShaderRecordStride = static_cast<UINT>(desc.MissShaderTable.StrideInBytes);
        constants.SrvCbvUavDescriptorHeapStart = pSrvCbvUavDescriptorHeap->GetGPUDescriptorHandleForHeapStart().ptr;
        constants.SamplerDescriptorHeapStart = pSamplerDescriptorHeap ? pSamplerDescriptorHeap->GetGPUDescriptorHandleForHeapStart().ptr : 0;
        
        pCommandList->SetComputeRoot32BitConstants(m_patchRootSignatureParameterStart + DispatchConstants, SizeOfInUint32(DispatchRaysConstants), &constants, 0);

        UINT entriesAdded = 0;
        std::vector<WRAPPED_GPU_POINTER> AccelerationStructuresEntries(boundAccelerationStructures.size());
        for (auto &entry : boundAccelerationStructures)
        {
            AccelerationStructuresEntries[entriesAdded++] = entry.second;
        }

        pCommandList->SetComputeRoot32BitConstants(
            m_patchRootSignatureParameterStart + AccelerationStructuresList, (UINT)(AccelerationStructuresEntries.size() * (SizeOfInUint32(*AccelerationStructuresEntries.data()))), AccelerationStructuresEntries.data(), 0);

#ifdef DEBUG
        m_pPredispatchCallback(pCommandList, m_patchRootSignatureParameterStart);
#endif

        UINT dispatchWidth = DivideAndRoundUp<UINT>(desc.Width, THREAD_GROUP_WIDTH);
        UINT dispatchHeight = DivideAndRoundUp<UINT>(desc.Height, THREAD_GROUP_HEIGHT);

        pCommandList->SetPipelineState(m_pRayTracePSO);
        pCommandList->Dispatch(dispatchWidth, dispatchHeight, 1);
    }
}
