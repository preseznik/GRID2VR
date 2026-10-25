#include "flicker_capture.h"
#include "sign_visibility.h"
#include <wrl/client.h>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;
namespace f=grid2vr::flicker;
int main(int argc,char** argv) {
    if(argc!=2) return 1;
    const std::filesystem::path root=argv[1];std::filesystem::create_directories(root);
    const auto folder=root/std::to_string(GetCurrentProcessId());std::filesystem::create_directories(folder);
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level;
    const auto library=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto create=reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(library,"D3D11CreateDevice"));
    if(!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context))) return 2;
    std::array<ComPtr<ID3D11Texture2D>,3> textures;
    const std::array<DXGI_FORMAT,3> formats{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM};
    // R, G, B, including source channel conversion and ten-bit unpacking.
    const std::array<uint32_t,3> pixels{0xff0000ff,0xff00ff00,0xfff00000};
    for(unsigned eye=0;eye<3;++eye) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=1;desc.ArraySize=desc.MipLevels=1;
        desc.Format=formats[eye];desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA data{&pixels[eye],4,0};
        if(FAILED(device->CreateTexture2D(&desc,&data,&textures[eye])))return 3;
    }
    ComPtr<ID3D11RenderTargetView> target;if(FAILED(device->CreateRenderTargetView(textures[0].Get(),nullptr,&target)))return 4;
    auto* raw=target.Get();context->OMSetRenderTargets(1,&raw,nullptr);
    const D3D11_VIEWPORT viewport{3,4,5,6,0,1};context->RSSetViewports(1,&viewport);
    ComPtr<ID3D11Buffer> instance,alpha,object;
    D3D11_BUFFER_DESC bufferDesc{};bufferDesc.ByteWidth=64;bufferDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    if(FAILED(device->CreateBuffer(&bufferDesc,nullptr,&instance)))return 15;
    bufferDesc.ByteWidth=16;bufferDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    const std::array<uint32_t,4> alphaWords{0x3f000000,0,0,0};D3D11_SUBRESOURCE_DATA alphaData{alphaWords.data(),0,0};
    if(FAILED(device->CreateBuffer(&bufferDesc,&alphaData,&alpha)))return 16;
    std::array<uint8_t,448> objectBytes{};
    for(unsigned i=0;i<objectBytes.size();++i)objectBytes[i]=static_cast<uint8_t>(i);
    bufferDesc.ByteWidth=static_cast<UINT>(objectBytes.size());
    D3D11_SUBRESOURCE_DATA objectData{objectBytes.data(),0,0};
    if(FAILED(device->CreateBuffer(&bufferDesc,&objectData,&object)))return 22;
    auto* objectBinding=object.Get();context->VSSetConstantBuffers(0,1,&objectBinding);
    auto* stream=instance.Get();const UINT stride=16,offset=0,lastOffset=16;
    context->IASetVertexBuffers(1,1,&stream,&stride,&offset);context->IASetVertexBuffers(31,1,&stream,&stride,&lastOffset);
    auto* constants=alpha.Get();context->PSSetConstantBuffers(5,1,&constants);
    ComPtr<ID3D11DepthStencilState> equalDepth;
    D3D11_DEPTH_STENCIL_DESC depthDesc{};depthDesc.DepthEnable=TRUE;depthDesc.DepthFunc=D3D11_COMPARISON_EQUAL;
    depthDesc.FrontFace=depthDesc.BackFace={D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};
    if(FAILED(device->CreateDepthStencilState(&depthDesc,&equalDepth)))return 17;
    context->OMSetDepthStencilState(equalDepth.Get(),7);
    std::ofstream(folder/L"flicker-capture.request") << "capture";
    f::Configure(folder);f::Poll();f::Begin(1);
    if(f::Capturing() || std::filesystem::exists(folder/L"flicker-draws.jsonl"))return 5;
    std::ofstream(folder/L"flicker-diagnostic.request") << "enabled";
    f::Configure(folder);f::Poll();
    std::thread wrong([]{f::Begin(1);});wrong.join();if(f::Capturing())return 6;
    const std::array<ID3D11Texture2D*,3> images{textures[0].Get(),textures[1].Get(),textures[2].Get()};
    for(uint64_t scene=1;scene<=10;++scene) {
        f::Begin(scene);
        if(f::Capturing()!= (scene<=8))return 7;
        if(scene==1) {
            f::Begin(scene); // duplicate boundary does not consume another sample
            for(unsigned i=0;i<4098;++i) {
                if(i==1)f::Upload(alpha.Get(),alphaWords.data(),16);
                f::Draw(context.Get(),0,6,"Indexed",std::array<UINT,3>{3,0,0},1,2,{});
            }
        }
        if(scene==2)f::Draw(context.Get(),0,6,"Indexed",std::array<UINT,3>{3,0,0},1,2,{}); // older upload stays labelled
        if(scene==3) {
            context->OMSetDepthStencilState(nullptr,0);
            f::Draw(context.Get(),0,6,"Indexed",std::array<UINT,3>{3,0,0},1,2,{});
            context->OMSetDepthStencilState(equalDepth.Get(),7);
        }
        if(scene==4) {
            f::Draw(context.Get(),0,6,"Indexed",std::array<UINT,3>{3,0,0},0xc90ec9a844625ee6ull,2,{}); // unknown upload
            f::Upload(object.Get(),objectBytes.data(),static_cast<UINT>(objectBytes.size()));
            f::Draw(context.Get(),1,6,"Indexed",std::array<UINT,3>{3,0,0},0xc90ec9a844625ee6ull,2,{});
            f::Upload(object.Get(),objectBytes.data(),352); // must not expose an earlier timer/matrix tail
            f::Draw(context.Get(),2,6,"Indexed",std::array<UINT,3>{3,0,0},0xc90ec9a844625ee6ull,2,{});
            f::Upload(object.Get(),objectBytes.data(),static_cast<UINT>(objectBytes.size()));
        }
        if(scene==5)f::Draw(context.Get(),0,6,"Indexed",std::array<UINT,3>{3,0,0},0xc90ec9a844625ee6ull,2,{}); // stale data explicitly labelled
        f::Complete(context.Get(),scene+100,scene,scene!=2,images);
    }
    std::ofstream(folder/L"flicker-capture.request") << "again";f::Poll();f::Begin(11);
    if(f::Capturing())return 8;
    ComPtr<ID3D11RenderTargetView> after;context->OMGetRenderTargets(1,&after,nullptr);
    D3D11_VIEWPORT actual{};UINT count=1;context->RSGetViewports(&count,&actual);
    if(after!=target || count!=1 || memcmp(&actual,&viewport,sizeof(actual)))return 9;
    ComPtr<ID3D11Buffer> bound;UINT actualStride{},actualOffset{},reference{};
    context->IAGetVertexBuffers(31,1,&bound,&actualStride,&actualOffset);
    if(bound!=instance || actualStride!=stride || actualOffset!=lastOffset)return 18;
    bound.Reset();context->PSGetConstantBuffers(5,1,&bound);if(bound!=alpha)return 19;
    bound.Reset();context->VSGetConstantBuffers(0,1,&bound);if(bound!=object)return 23;
    ComPtr<ID3D11DepthStencilState> depthAfter;context->OMGetDepthStencilState(&depthAfter,&reference);
    if(depthAfter!=equalDepth || reference!=7)return 20;
    for(unsigned eye=0;eye<3;++eye) {
        const auto path=folder/("stage-1-flicker-eye"+std::to_string(eye)+".ppm");
        std::ifstream file(path,std::ios::binary);std::string data((std::istreambuf_iterator<char>(file)),{});
        if(data.size()!=14 || data.substr(0,11)!="P6\n1 1\n255\n")return 10;
        for(unsigned c=0;c<3;++c)if(static_cast<unsigned char>(data[11+c])!=(c==eye?255:0))return 11;
    }
    if(std::filesystem::exists(folder/L"stage-2-flicker-eye0.ppm") || std::filesystem::exists(folder/L"stage-9-flicker-eye0.ppm"))return 12;
    std::ifstream file(folder/L"flicker-draws.jsonl");std::string line;unsigned draws=0,scenes=0,imagesSaved=0;bool end=false,truncated=false;
    unsigned knownAlpha=0,unknownAlpha=0,staleAlpha=0,defaultDepth=0,equalTests=0,instanceRecords=0;
    unsigned objectFields=0,knownObjects=0,unknownObjects=0,shortObjects=0,staleObjects=0;
    const auto streamId=std::to_string(uintptr_t(instance.Get())),alphaId=std::to_string(uintptr_t(alpha.Get()));
    while(std::getline(file,line)) {
        draws+=line.find("\"type\":\"draw\"")!=std::string::npos;
        scenes+=line.find("\"type\":\"scene\"")!=std::string::npos;
        imagesSaved+=line.find("\"status\":\"saved\"")!=std::string::npos;
        end|=line.find("\"type\":\"end\"")!=std::string::npos;
        truncated|=line.find("\"truncated\":[1,0,0]")!=std::string::npos;
        if(line.find("\"type\":\"draw\"")==std::string::npos)continue;
        instanceRecords+=line.find("\"instanceStreams\":[[1,"+streamId+",16,0],[31,"+streamId+",16,16]]")!=std::string::npos;
        const bool known=line.find(",16,1056964608,0,0,0]")!=std::string::npos;
        knownAlpha+=known;unknownAlpha+=line.find("\"pcb5\":["+alphaId+",0,0,0,0]")!=std::string::npos;
        staleAlpha+=known && line.find("\"scene\":2")!=std::string::npos && line.find(",1,16,1056964608,0,0,0]")!=std::string::npos;
        defaultDepth+=line.find("\"depthTest\":[1,1,2]")!=std::string::npos;
        equalTests+=line.find("\"depthTest\":[1,0,3]")!=std::string::npos;
        if(line.find("\"objectInputs\":")!=std::string::npos) {
            ++objectFields;
            const bool hasTimer=line.find("\"_timer\":\"64656667\"")!=std::string::npos;
            knownObjects+=hasTimer;
            unknownObjects+=line.find("\"objectInputs\":{\"known\":0")!=std::string::npos;
            shortObjects+=line.find("\"winddirection\":\"505152535455565758595a5b5c5d5e5f\",\"_timer\":null,\"perInstanceModelMatrix\":null")!=std::string::npos;
            staleObjects+=hasTimer && line.find("\"scene\":5")!=std::string::npos && line.find("\"objectInputs\":{\"known\":1,\"scene\":4")!=std::string::npos;
        }
    }
    if(draws!=4102 || scenes!=8 || imagesSaved!=21 || !end || !truncated)return 13;
    if(instanceRecords!=4102 || knownAlpha!=4101 || unknownAlpha!=1 || staleAlpha!=1 || defaultDepth!=1 || equalTests!=4101)return 21;
    if(objectFields!=4 || knownObjects!=2 || unknownObjects!=1 || shortObjects!=1 || staleObjects!=1)return 24;
    file.close();
    // Automated mode keeps the causal object/alpha/state observations while
    // omitting bulky whole-buffer stamps. It must not alter any binding.
    std::ofstream(folder/L"flicker-compact.request") << "compact";
    f::Configure(folder);f::Poll();f::Begin(20);
    f::Upload(object.Get(),objectBytes.data(),static_cast<UINT>(objectBytes.size()));
    f::Draw(context.Get(),0,6,"Indexed",std::array<UINT,3>{3,0,0},0xc90ec9a844625ee6ull,2,{});
    f::AfterDraw(context.Get());
    f::Complete(context.Get(),120,20,true,images);
    std::ifstream compactFile(folder/L"flicker-draws.jsonl");
    std::string compactLog((std::istreambuf_iterator<char>(compactFile)),{});compactFile.close();
    for(const char* key:{"\"wholeBufferStamps\":false","\"objectInputs\":","\"pcb5\":","\"instanceStreams\":","\"depthTest\":"})
        if(compactLog.find(key)==std::string::npos)return 25;
    if(compactLog.find("\"vcb\":")!=std::string::npos || compactLog.find("\"pcb\":")!=std::string::npos)return 26;
    bound.Reset();context->VSGetConstantBuffers(0,1,&bound);if(bound!=object)return 27;
    after.Reset();context->OMGetRenderTargets(1,&after,nullptr);if(after!=target)return 28;
    // Actual GPU sample counts and buffer bytes, without replacing the game draw.
    const char* code="float4 main(float3 p:POSITION):SV_Position{return float4(p,1);}";
    ComPtr<ID3DBlob> bytecode;
    if(FAILED(D3DCompile(code,strlen(code),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&bytecode,nullptr)))return 29;
    ComPtr<ID3D11VertexShader> vs;
    if(FAILED(device->CreateVertexShader(bytecode->GetBufferPointer(),bytecode->GetBufferSize(),nullptr,&vs)))return 30;
    const D3D11_INPUT_ELEMENT_DESC element{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    ComPtr<ID3D11InputLayout> layout;
    if(FAILED(device->CreateInputLayout(&element,1,bytecode->GetBufferPointer(),bytecode->GetBufferSize(),&layout)))return 31;
    f::RememberLayout(layout.Get(),&element,1);context->IASetInputLayout(layout.Get());
    const float points[]{-1,-1,0, -1,3,0, 3,-1,0};D3D11_SUBRESOURCE_DATA pointsData{points,0,0};
    bufferDesc.ByteWidth=sizeof(points);bufferDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    ComPtr<ID3D11Buffer> vertices;if(FAILED(device->CreateBuffer(&bufferDesc,&pointsData,&vertices)))return 32;
    auto* verticesRaw=vertices.Get();const UINT pointStride=12;context->IASetVertexBuffers(0,1,&verticesRaw,&pointStride,&offset);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vs.Get(),nullptr,0);
    context->OMSetDepthStencilState(nullptr,0);context->RSSetState(nullptr);
    const D3D11_VIEWPORT onePixel{0,0,1,1,0,1};context->RSSetViewports(1,&onePixel);
    const char* pixelCode="float4 main():SV_Target{return float4(1,0,0,1);}";
    bytecode.Reset();if(FAILED(D3DCompile(pixelCode,strlen(pixelCode),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&bytecode,nullptr)))return 33;
    ComPtr<ID3D11PixelShader> ps;if(FAILED(device->CreatePixelShader(bytecode->GetBufferPointer(),bytecode->GetBufferSize(),nullptr,&ps)))return 34;
    context->PSSetShader(ps.Get(),nullptr,0);
    std::ofstream(folder/L"flicker-capture.request") << "fixture";
    f::Configure(folder);f::Poll();f::Begin(30);
    f::Draw(context.Get(),0,5,"Draw",std::array<UINT,2>{3,0},0xc90ec9a844625ee6ull,2,{});
    context->Draw(3,0);f::AfterDraw(context.Get());
    ComPtr<ID3D11Query> nativeQuery;const D3D11_QUERY_DESC nativeQueryDesc{D3D11_QUERY_OCCLUSION,0};
    if(FAILED(device->CreateQuery(&nativeQueryDesc,&nativeQuery)))return 38;
    const UINT64 nativeSamples=123;
    f::NativeQuery(nativeQuery.Get(),0,"get",0x1234,S_OK,&nativeSamples,sizeof(nativeSamples));
    f::NativeQuery(nativeQuery.Get(),0,"get",0x1234,S_FALSE,&nativeSamples,sizeof(nativeSamples));
    f::DepthClear(nullptr,0,D3D11_CLEAR_DEPTH,1,0);
    f::Complete(context.Get(),130,30,true,images);
    std::ifstream sampleFile(folder/L"flicker-draws.jsonl");std::string sampleLog((std::istreambuf_iterator<char>(sampleFile)),{});sampleFile.close();
    for(const char* expected:{"\"type\":\"layout\"","[\"POSITION\",0,6,0,0,0,0]","\"status\":\"ok\"","\"hr\":0,\"passed\":1","\"surface\":","\"viewport\":[1,0,0,1,1,0,1]"})
        if(sampleLog.find(expected)==std::string::npos)return 35;
    if(sampleLog.find("\"hr\":0,\"bytes\":8,\"hex\":\"7b00000000000000\"")==std::string::npos ||
       sampleLog.find("\"hr\":1,\"bytes\":8}")==std::string::npos || sampleLog.find("\"type\":\"clearDepth\"")==std::string::npos)return 39;
    ComPtr<ID3D11InputLayout> layoutAfter;context->IAGetInputLayout(&layoutAfter);if(layoutAfter!=layout)return 36;
    bound.Reset();context->IAGetVertexBuffers(0,1,&bound,&actualStride,&actualOffset);if(bound!=vertices || actualStride!=12 || actualOffset)return 37;
    // Capture only shader-consumed alpha sampling metadata, preserving the
    // existing sampler, view and resource LOD. Use non-default mip settings.
    D3D11_TEXTURE2D_DESC alphaTextureDesc{};alphaTextureDesc.Width=alphaTextureDesc.Height=4;
    alphaTextureDesc.ArraySize=1;alphaTextureDesc.MipLevels=3;alphaTextureDesc.SampleDesc.Count=1;
    alphaTextureDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;alphaTextureDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> alphaTexture;
    if(FAILED(device->CreateTexture2D(&alphaTextureDesc,nullptr,&alphaTexture)))return 40;
    D3D11_SHADER_RESOURCE_VIEW_DESC alphaViewDesc{};alphaViewDesc.Format=alphaTextureDesc.Format;
    alphaViewDesc.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;alphaViewDesc.Texture2D={1,2};
    ComPtr<ID3D11ShaderResourceView> alphaView;
    if(FAILED(device->CreateShaderResourceView(alphaTexture.Get(),&alphaViewDesc,&alphaView)))return 41;
    auto* alphaViewRaw=alphaView.Get();context->PSSetShaderResources(0,1,&alphaViewRaw);
    D3D11_SAMPLER_DESC alphaSamplerDesc{};alphaSamplerDesc.Filter=D3D11_FILTER_ANISOTROPIC;
    alphaSamplerDesc.AddressU=alphaSamplerDesc.AddressV=alphaSamplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    alphaSamplerDesc.MipLODBias=.75f;alphaSamplerDesc.MaxAnisotropy=8;alphaSamplerDesc.ComparisonFunc=D3D11_COMPARISON_NEVER;
    alphaSamplerDesc.MinLOD=1;alphaSamplerDesc.MaxLOD=3;
    ComPtr<ID3D11SamplerState> alphaSampler;
    if(FAILED(device->CreateSamplerState(&alphaSamplerDesc,&alphaSampler)))return 42;
    auto* alphaSamplerRaw=alphaSampler.Get();context->PSSetSamplers(5,1,&alphaSamplerRaw);
    std::ofstream(folder/L"flicker-capture.request") << "fixture";
    f::Configure(folder);f::Poll();f::Begin(40);
    f::Draw(context.Get(),0,6,"Draw",std::array<UINT,2>{3,0},1,0x938d132c1aa95cbeull,{});
    f::Complete(context.Get(),140,40,true,images);
    std::ifstream alphaFile(folder/L"flicker-draws.jsonl");std::string alphaLog((std::istreambuf_iterator<char>(alphaFile)),{});alphaFile.close();
    for(const char* expected:{"\"slot\":5,\"bound\":1,\"descriptor\":[85,3,3,3,1061158912,8,1,0,0,0,0,1065353216,1077936128]",
        "\"mostDetailedMip\":1,\"mipLevels\":2", "\"resourceMinLodBits\":0,\"texture\":[4,4,3,1,28]"})
        if(alphaLog.find(expected)==std::string::npos)return 43;
    ComPtr<ID3D11SamplerState> samplerAfter;context->PSGetSamplers(5,1,&samplerAfter);
    ComPtr<ID3D11ShaderResourceView> alphaViewAfter;context->PSGetShaderResources(0,1,&alphaViewAfter);
    if(samplerAfter!=alphaSampler || alphaViewAfter!=alphaView || context->GetResourceMinLOD(alphaTexture.Get())!=0)return 44;
    // A braking-marker mesh can use an index slice and a signed base vertex.
    // Capture only its referenced bytes, not the whole shared geometry buffer.
    std::array<float,12> markerPoints{};memcpy(markerPoints.data()+3,points,sizeof(points));
    bufferDesc.ByteWidth=sizeof(markerPoints);bufferDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA markerData{markerPoints.data(),0,0};ComPtr<ID3D11Buffer> markerVertex,markerIndex;
    if(FAILED(device->CreateBuffer(&bufferDesc,&markerData,&markerVertex)))return 45;
    std::array<uint16_t,58> markerIndices{};
    for(unsigned i=4;i<markerIndices.size();++i)markerIndices[i]=uint16_t((i-4)%3);
    bufferDesc.ByteWidth=sizeof(markerIndices);bufferDesc.BindFlags=D3D11_BIND_INDEX_BUFFER;
    markerData.pSysMem=markerIndices.data();if(FAILED(device->CreateBuffer(&bufferDesc,&markerData,&markerIndex)))return 46;
    auto* markerBinding=markerVertex.Get();context->IASetVertexBuffers(0,1,&markerBinding,&pointStride,&offset);
    context->IASetIndexBuffer(markerIndex.Get(),DXGI_FORMAT_R16_UINT,0);
    const float blue[]{0,0,1,1};context->ClearRenderTargetView(target.Get(),blue);
    std::ofstream(folder/L"flicker-capture.request") << "marker";
    f::Configure(folder);f::Poll();f::Begin(50);
    f::Draw(context.Get(),0,6,"Indexed",std::array<UINT,3>{54,4,1},0xd946f27a1c896b78ull,0x938d132c1aa95cbeull,{});
    context->DrawIndexed(54,4,1);f::AfterDraw(context.Get());
    f::FinishPass(context.Get(),50,0,textures[0].Get());f::Complete(context.Get(),150,50,true,images);
    std::ifstream markerFile(folder/L"flicker-draws.jsonl");std::string markerLog((std::istreambuf_iterator<char>(markerFile)),{});markerFile.close();
    for(const char* expected:{"\"meshInputs\":", "\"bytes\":108,\"resourceBytes\":116,\"offset\":8,\"status\":\"ok\"",
        "\"vertexFirst\":1", "\"bytes\":36,\"resourceBytes\":48,\"offset\":12,\"status\":\"ok\"",
        "\"type\":\"meshPixels\"", "\"changedPixels\":1,\"retainedPixels\":1", "\"finalSourceSame\":true"})
        if(markerLog.find(expected)==std::string::npos)return 47;
    bound.Reset();context->IAGetVertexBuffers(0,1,&bound,&actualStride,&actualOffset);
    if(bound!=markerVertex || actualStride!=pointStride || actualOffset)return 48;
    // Instance transforms come from the actual consumed stream, with nonzero
    // binding and start-instance offsets. No cached CPU upload is supplied.
    const char* instanceCode="float4 main(float3 p:POSITION,float4 a:TANGENT1,float4 b:TANGENT2,float4 c:TANGENT3,float4 d:TANGENT4):SV_Position{return float4(p.x*a.xyz+p.y*b.xyz+p.z*c.xyz+d.xyz,1);}";
    bytecode.Reset();if(FAILED(D3DCompile(instanceCode,strlen(instanceCode),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&bytecode,nullptr)))return 49;
    D3D11_INPUT_ELEMENT_DESC instanceElements[]{element,
        {"TANGENT",1,DXGI_FORMAT_R16G16B16A16_FLOAT,1,0,D3D11_INPUT_PER_INSTANCE_DATA,1},
        {"TANGENT",2,DXGI_FORMAT_R16G16B16A16_FLOAT,1,8,D3D11_INPUT_PER_INSTANCE_DATA,1},
        {"TANGENT",3,DXGI_FORMAT_R16G16B16A16_FLOAT,1,16,D3D11_INPUT_PER_INSTANCE_DATA,1},
        {"TANGENT",4,DXGI_FORMAT_R32G32B32A32_FLOAT,1,24,D3D11_INPUT_PER_INSTANCE_DATA,1}};
    ComPtr<ID3D11InputLayout> instanceLayout;
    if(FAILED(device->CreateInputLayout(instanceElements,5,bytecode->GetBufferPointer(),bytecode->GetBufferSize(),&instanceLayout)))return 50;
    f::RememberLayout(instanceLayout.Get(),instanceElements,5);context->IASetInputLayout(instanceLayout.Get());
    std::array<uint8_t,328> instanceData{};for(size_t i=0;i<instanceData.size();++i)instanceData[i]=uint8_t(i);
    bufferDesc.ByteWidth=UINT(instanceData.size());bufferDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA instanceInit{instanceData.data(),0,0};ComPtr<ID3D11Buffer> transforms;
    if(FAILED(device->CreateBuffer(&bufferDesc,&instanceInit,&transforms)))return 51;
    auto* transformBinding=transforms.Get();const UINT transformStride=40,transformOffset=8;
    context->IASetVertexBuffers(1,1,&transformBinding,&transformStride,&transformOffset);
    std::ofstream(folder/L"flicker-capture.request") << "instances";
    f::Configure(folder);f::Poll();f::Begin(60);
    constexpr uint64_t instanceVs=0x13dd94df551eb6d9ull,instancePs=0x938d132c1aa95cbeull;
    f::Draw(context.Get(),0,6,"IndexedInstanced",std::array<UINT,5>{6,2,4,1,3},instanceVs,instancePs,{});f::AfterDraw(context.Get());
    f::Draw(context.Get(),0,6,"IndexedInstanced",std::array<UINT,5>{6,65,4,1,3},instanceVs,instancePs,{});f::AfterDraw(context.Get());
    f::Draw(context.Get(),0,6,"IndexedInstanced",std::array<UINT,5>{6,2,4,1,UINT_MAX},instanceVs,instancePs,{});f::AfterDraw(context.Get());
    instanceElements[1].InstanceDataStepRate=2;f::RememberLayout(instanceLayout.Get(),instanceElements,5);
    f::Draw(context.Get(),0,6,"IndexedInstanced",std::array<UINT,5>{6,2,4,1,3},instanceVs,instancePs,{});f::AfterDraw(context.Get());
    f::Complete(context.Get(),160,60,true,images);
    std::ifstream instanceFile(folder/L"flicker-draws.jsonl");std::string instanceLog((std::istreambuf_iterator<char>(instanceFile)),{});instanceFile.close();
    for(const char* expected:{"\"instanceInputs\":{\"status\":\"ok\"", "\"firstElement\":3,\"elementCount\":2,\"stride\":40,\"bindingOffset\":8,\"stepRate\":1",
        "\"bytes\":80,\"resourceBytes\":328,\"offset\":128,\"status\":\"ok\"", "\"hex\":\"80818283848586878889",
        "\"status\":\"unsupported_count\"", "\"status\":\"unsupported_binding\"", "\"status\":\"unsupported_layout\""})
        if(instanceLog.find(expected)==std::string::npos)return 52;
    bound.Reset();context->IAGetVertexBuffers(1,1,&bound,&actualStride,&actualOffset);
    if(bound!=transforms || actualStride!=40 || actualOffset!=8)return 53;
    layoutAfter.Reset();context->IAGetInputLayout(&layoutAfter);if(layoutAfter!=instanceLayout)return 54;
    // The optional timing trigger requires fresh native model/camera uploads
    // and an accepted pair; an eye, stale data or failed pair cannot arm it.
    std::array<uint8_t,128> targetCamera{};std::array<float,16> targetVp{};
    targetVp[14]=35;targetVp[15]=70;memcpy(targetCamera.data()+64,targetVp.data(),64);
    bufferDesc.ByteWidth=128;bufferDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA targetCameraInit{targetCamera.data(),0,0};ComPtr<ID3D11Buffer> targetCameraBuffer;
    if(FAILED(device->CreateBuffer(&bufferDesc,&targetCameraInit,&targetCameraBuffer)))return 55;
    auto* targetCameraRaw=targetCameraBuffer.Get();context->VSSetConstantBuffers(3,1,&targetCameraRaw);
    const float targetPosition[]{89.756325f,26.448267f,-379.425201f};
    memcpy(objectBytes.data()+192,targetPosition,12);
    std::ofstream(folder/L"flicker-sign-target.request") << "target";
    std::ofstream(folder/L"flicker-capture.request") << "target";
    f::Configure(folder);f::Poll();
    for(uint64_t scene=70;scene<=73;++scene) {
        f::Begin(scene);if(f::Capturing() || !f::Observing())return 56;
        if(scene!=70) {
            f::Upload(object.Get(),objectBytes.data(),UINT(objectBytes.size()));
            f::Upload(targetCameraBuffer.Get(),targetCamera.data(),UINT(targetCamera.size()));
        }
        f::Draw(context.Get(),scene==71?1:0,6,"Indexed",std::array<UINT,3>{126,0,0},0xd946f27a1c896b78ull,instancePs,{});
        if(f::Capturing())return 57;
        f::Complete(context.Get(),scene+100,scene,scene!=72,images);
    }
    f::Begin(74);if(!f::Capturing())return 58;
    f::Complete(context.Get(),174,74,true,images);
    std::ifstream triggerFile(folder/L"flicker-draws.jsonl");std::string triggerLog((std::istreambuf_iterator<char>(triggerFile)),{});triggerFile.close();
    if(triggerLog.find("\"type\":\"signTrigger\",\"scene\":73,\"target\":0")==std::string::npos ||
        triggerLog.find("\"type\":\"scene\",\"scene\":74,\"sample\":1")==std::string::npos)return 59;
    // Visibility comparisons can capture a sideways view without waiting for
    // a forward-facing sign draw that the candidate may correctly exclude.
    std::filesystem::remove(folder/L"flicker-sign-target.request");
    std::ofstream(folder/L"native-visibility-control.request") << "control";
    std::ofstream(folder/L"flicker-capture.request") << "capture";
    f::Configure(folder);f::Poll();f::Begin(80);
    if(!f::Capturing())return 60;
    grid2vr::signvisibility::Snapshot(180,80,0,"visibility-before",0,0);
    f::Complete(context.Get(),180,80,true,images);
    for(uint64_t scene=81;scene<88;++scene) {f::Begin(scene);f::Complete(context.Get(),scene+100,scene,true,images);}
    std::ifstream motionFile(folder/L"sign-visibility.jsonl");std::string motionLog((std::istreambuf_iterator<char>(motionFile)),{});motionFile.close();
    if(motionLog.find("\"type\":\"snapshot\",\"frame\":180")==std::string::npos)return 61;
    // Crowded routes must retain complete triples without storing every draw.
    // The marker alone cannot activate this reduced diagnostic outside the
    // isolated visibility/unattended path.
    std::ofstream(folder/L"flicker-visibility-only.request") << "visibility";
    std::ofstream(folder/L"flicker-capture.request") << "capture";
    f::Configure(folder);f::Poll();f::Begin(90);
    if(f::Capturing())return 62;
    std::ofstream(folder/L"tracked-visibility.request") << "tracked";
    std::ofstream(folder/L"flicker-unattended.request") << "unattended";
    f::Configure(folder);f::Poll();
    for(uint64_t scene=90;scene<98;++scene) {
        f::Begin(scene);if(!f::Capturing())return 63;
        for(unsigned pass=0;pass<3;++pass)for(unsigned i=0;i<5000;++i) {
            // A null context proves this path never inspects/binds GPU state.
            f::Draw(nullptr,pass,6,"Indexed",std::array<UINT,3>{126,0,0},0,0,{});
            f::AfterDraw(nullptr);
        }
        f::Complete(context.Get(),scene+100,scene,true,images);
    }
    std::ifstream reducedFile(folder/L"flicker-draws.jsonl");std::string reducedLog((std::istreambuf_iterator<char>(reducedFile)),{});reducedFile.close();
    if(reducedLog.size()>16384 || reducedLog.find("\"captureScope\":\"visibility\"")==std::string::npos ||
        reducedLog.find("\"type\":\"draw\"")!=std::string::npos || reducedLog.find("\"type\":\"end\",\"scenes\":8")==std::string::npos)return 64;
    size_t cursor=0;unsigned completions=0,saved=0;
    while((cursor=reducedLog.find("\"draws\":[5000,5000,5000],\"truncated\":[0,0,0]",cursor))!=std::string::npos){++completions;++cursor;}
    cursor=0;while((cursor=reducedLog.find("\"status\":\"saved\"",cursor))!=std::string::npos){++saved;++cursor;}
    if(completions!=8 || saved!=24)return 65;
    std::filesystem::remove(folder/L"tracked-visibility.request");
    std::ofstream(folder/L"flicker-capture.request") << "native control";
    f::Configure(folder);f::Poll();f::Begin(100);
    if(!f::Capturing())return 66; // native control + unattended markers remain
    f::Configure(root); // close the log before removing only this fixture
    if(std::filesystem::canonical(folder).parent_path()!=std::filesystem::canonical(root) || std::filesystem::is_symlink(folder))return 14;
    std::filesystem::remove_all(folder);
    puts("Bounded opt-in source capture preserves bindings/pixels, rejects partial pairs, and cannot rearm.");
}
