// Standalone GPU arithmetic regression. Build/run via legacy_effects_validation.py.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr) { if (FAILED(hr)) { std::fprintf(stderr,"D3D12 failure: %08lx\n",hr); std::exit(1); } }
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    ComPtr<ID3D12Debug> debug; check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    ComPtr<ID3D12Device> device; check(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info; check(device.As(&info));
    std::ifstream f(argv[1],std::ios::binary); std::vector<char> code((std::istreambuf_iterator<char>(f)),{});
    if(code.empty()) return 2;
    D3D12_ROOT_PARAMETER param={};param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rd={};rd.NumParameters=1;rd.pParameters=&param;
    ComPtr<ID3DBlob> blob,errors;check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors));
    ComPtr<ID3D12RootSignature> root;check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd={};pd.pRootSignature=root.Get();pd.CS={code.data(),code.size()};
    ComPtr<ID3D12PipelineState> pipeline;check(device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pipeline)));
    constexpr unsigned count=27, bytes=count*4*sizeof(float);
    auto buffer=[&](D3D12_HEAP_TYPE type) {
        D3D12_HEAP_PROPERTIES hp={};hp.Type=type;
        D3D12_RESOURCE_DESC d={};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(type==D3D12_HEAP_TYPE_DEFAULT)d.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_DEFAULT?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)));return r;
    };
    auto output=buffer(D3D12_HEAP_TYPE_DEFAULT), readback=buffer(D3D12_HEAP_TYPE_READBACK);
    D3D12_COMMAND_QUEUE_DESC qd={};ComPtr<ID3D12CommandQueue> queue;check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),pipeline.Get(),IID_PPV_ARGS(&list)));
    list->SetComputeRootSignature(root.Get());list->SetComputeRootUnorderedAccessView(0,output->GetGPUVirtualAddress());list->Dispatch(1,1,1);
    D3D12_RESOURCE_BARRIER barrier={};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&barrier);
    list->CopyResource(readback.Get(),output.Get());check(list->Close());ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));check(queue->Signal(fence.Get(),1));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return 1;check(fence->SetEventOnCompletion(1,event));
    if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)return 1;CloseHandle(event);
    const float expected[count][4]={
        {1,1,1,1},{.3f,.6f,.4f,1},{.2f,.3f,.4f,1},{0,0,-1,1},{0,0,-1,1},
        {.2f,.4f,.6f,1},{.125f,.25f,.375f,1},{-.01f,.02f,0,1},{-.01f,.04f,0,1},{0,0,0,1},
        {.375f,.625f,.2f,1},{.25f,std::sqrt(.5f),0,1},{1,.7f,.4f,1},{.04f,1,1,1},
        {.2f,2,8,1},{.788675135f,1.0f/6561.0f,0,1},{.5f,0,0,1},
        {1,1,1,1},{.25f,.5f,1,1},{.2f,2,8,1},{.4f,0,.5f,1},
        {1,0,0,1},{1,0,0,1},{-1,0,0,1},{0,0,1,1},{1,1,0,1},{.25f,1,0,1}};
    D3D12_RANGE range={0,bytes};void* mapped=nullptr;check(readback->Map(0,&range,&mapped));auto values=static_cast<float*>(mapped);
    bool ok=true;for(unsigned i=0;i<count;i++)for(unsigned j=0;j<4;j++)if(!std::isfinite(values[i*4+j])||std::abs(values[i*4+j]-expected[i][j])>2e-5f){std::fprintf(stderr,"Case %u channel %u: got %g expected %g\n",i,j,values[i*4+j],expected[i][j]);ok=false;}
    readback->Unmap(0,nullptr);
    for(UINT64 i=0;i<info->GetNumStoredMessages();i++){SIZE_T size=0;check(info->GetMessage(i,nullptr,&size));std::vector<char> storage(size);auto m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());check(info->GetMessage(i,m,&size));if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::fprintf(stderr,"%s\n",m->pDescription);ok=false;}}
    if(ok)std::puts("PASS: 27 production shader cases; zero D3D12 validation errors");return ok?0:1;
}
