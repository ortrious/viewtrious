#include "three_mf_loader.h"
#include "three_mf_zip.h"

#include <windows.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <cwchar>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr uint64_t kMaxVertices = 30'000'000ull;
constexpr uint64_t kMaxTriangles = 10'000'000ull;
constexpr uint32_t kMaxObjects = 100'000;
constexpr uint32_t kMaxComponentDepth = 64;

struct SourceTriangle { uint32_t first = 0, second = 0, third = 0; };
struct SourceComponent { uint32_t objectId = 0; Matrix4 transform = Matrix4::Identity(); };
struct SourceObject { uint32_t id = 0; std::vector<Float3> vertices; std::vector<SourceTriangle> triangles; std::vector<SourceComponent> components; };
struct BuildItem { uint32_t objectId = 0; Matrix4 transform = Matrix4::Identity(); };
struct ParsedModel { std::unordered_map<uint32_t, SourceObject> objects; std::vector<BuildItem> buildItems; std::wstring unit = L"millimeter"; double scaleMillimeters = 1.0; };

bool Finite(Float3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
Float3 Sub(Float3 a, Float3 b) { return { a.x-b.x, a.y-b.y, a.z-b.z }; }
Float3 Cross(Float3 a, Float3 b) { return { a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x }; }
float Dot(Float3 a, Float3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
float Length(Float3 a) { return std::sqrt(Dot(a,a)); }

Matrix4 Multiply(const Matrix4& a, const Matrix4& b) {
    Matrix4 result{};
    for (int row = 0; row < 4; ++row) for (int column = 0; column < 4; ++column)
        for (int i = 0; i < 4; ++i) result.m[row*4+column] += a.m[row*4+i] * b.m[i*4+column];
    return result;
}
Float3 TransformPoint(Float3 point, const Matrix4& transform) {
    return { point.x*transform.m[0]+point.y*transform.m[4]+point.z*transform.m[8]+transform.m[12],
             point.x*transform.m[1]+point.y*transform.m[5]+point.z*transform.m[9]+transform.m[13],
             point.x*transform.m[2]+point.y*transform.m[6]+point.z*transform.m[10]+transform.m[14] };
}

bool ParseUnsigned(const std::wstring& text, uint32_t& value) {
    if (text.empty()) return false; wchar_t* end = nullptr; errno = 0; const unsigned long parsed = std::wcstoul(text.c_str(), &end, 10);
    if (errno || end != text.c_str()+text.size() || parsed > std::numeric_limits<uint32_t>::max()) return false;
    value = static_cast<uint32_t>(parsed); return true;
}
bool ParseFloat(const std::wstring& text, float& value) {
    if (text.empty()) return false; wchar_t* end = nullptr; errno = 0; const double parsed = std::wcstod(text.c_str(), &end);
    if (errno || end != text.c_str()+text.size() || !std::isfinite(parsed) || parsed < -std::numeric_limits<float>::max() || parsed > std::numeric_limits<float>::max()) return false;
    value = static_cast<float>(parsed); return true;
}
bool ParseTransform(const std::wstring& text, Matrix4& transform) {
    transform = Matrix4::Identity(); if (text.empty()) return true;
    double values[12]{}; const wchar_t* cursor = text.c_str();
    for (double& value : values) { while (*cursor && std::iswspace(*cursor)) ++cursor; wchar_t* end = nullptr; errno = 0; value = std::wcstod(cursor, &end); if (end == cursor || errno || !std::isfinite(value)) return false; cursor = end; }
    while (*cursor && std::iswspace(*cursor)) ++cursor; if (*cursor) return false;
    transform.m[0]=float(values[0]); transform.m[1]=float(values[1]); transform.m[2]=float(values[2]); transform.m[12]=float(values[3]);
    transform.m[4]=float(values[4]); transform.m[5]=float(values[5]); transform.m[6]=float(values[6]); transform.m[13]=float(values[7]);
    transform.m[8]=float(values[8]); transform.m[9]=float(values[9]); transform.m[10]=float(values[10]); transform.m[14]=float(values[11]);
    return true;
}

std::wstring Attribute(IXmlReader* reader, const wchar_t* name) {
    if (reader->MoveToFirstAttribute() != S_OK) return {};
    do { const wchar_t* local = nullptr; const wchar_t* value = nullptr; if (SUCCEEDED(reader->GetLocalName(&local, nullptr)) && SUCCEEDED(reader->GetValue(&value, nullptr)) && local && value && wcscmp(local, name) == 0) { reader->MoveToElement(); return value; } } while (reader->MoveToNextAttribute() == S_OK);
    reader->MoveToElement(); return {};
}
bool ParseUnit(const std::wstring& unit, double& scale) {
    if (unit.empty() || unit == L"millimeter") { scale=1.0; return true; }
    if (unit == L"micron") { scale=.001; return true; } if (unit == L"centimeter") { scale=10.; return true; }
    if (unit == L"meter") { scale=1000.; return true; } if (unit == L"inch") { scale=25.4; return true; }
    if (unit == L"foot") { scale=304.8; return true; } return false;
}

bool ParseModelXml(IStream* stream, ParsedModel& model, std::wstring& error) {
    ComPtr<IXmlReader> reader; if (FAILED(CreateXmlReader(IID_PPV_ARGS(&reader), nullptr)) || FAILED(reader->SetInput(stream))) { error=L"The 3MF model XML could not be read."; return false; }
    uint32_t currentObject = 0; bool haveModel = false; XmlNodeType type{};
    while (reader->Read(&type) == S_OK) {
        if (type == XmlNodeType_Element) {
            const wchar_t* local = nullptr; if (FAILED(reader->GetLocalName(&local, nullptr)) || !local) { error=L"The 3MF XML is malformed."; return false; }
            if (wcscmp(local,L"model") == 0) { if (haveModel) { error=L"The 3MF contains multiple model roots."; return false; } haveModel=true; const std::wstring unit=Attribute(reader.Get(),L"unit"); if (!unit.empty()) model.unit=unit; if (!ParseUnit(model.unit,model.scaleMillimeters)) { error=L"The 3MF declares an unsupported model unit."; return false; } }
            else if (wcscmp(local,L"object") == 0) { uint32_t id=0; if (!ParseUnsigned(Attribute(reader.Get(),L"id"),id) || id==0 || model.objects.size()>=kMaxObjects || model.objects.contains(id)) { error=L"The 3MF contains an invalid object ID."; return false; } model.objects.emplace(id,SourceObject{id}); currentObject=id; }
            else if (wcscmp(local,L"vertex") == 0 && currentObject) { auto object=model.objects.find(currentObject); Float3 vertex{}; if (object==model.objects.end() || !ParseFloat(Attribute(reader.Get(),L"x"),vertex.x) || !ParseFloat(Attribute(reader.Get(),L"y"),vertex.y) || !ParseFloat(Attribute(reader.Get(),L"z"),vertex.z) || !Finite(vertex) || object->second.vertices.size()>=kMaxVertices) { error=L"The 3MF contains an invalid vertex."; return false; } object->second.vertices.push_back(vertex); }
            else if (wcscmp(local,L"triangle") == 0 && currentObject) { auto object=model.objects.find(currentObject); SourceTriangle triangle{}; if (object==model.objects.end() || !ParseUnsigned(Attribute(reader.Get(),L"v1"),triangle.first) || !ParseUnsigned(Attribute(reader.Get(),L"v2"),triangle.second) || !ParseUnsigned(Attribute(reader.Get(),L"v3"),triangle.third) || object->second.triangles.size()>=kMaxTriangles) { error=L"The 3MF contains an invalid triangle."; return false; } object->second.triangles.push_back(triangle); }
            else if (wcscmp(local,L"component") == 0 && currentObject) { auto object=model.objects.find(currentObject); SourceComponent component{}; if (object==model.objects.end() || !ParseUnsigned(Attribute(reader.Get(),L"objectid"),component.objectId) || component.objectId==0 || !ParseTransform(Attribute(reader.Get(),L"transform"),component.transform)) { error=L"The 3MF contains an invalid component transform."; return false; } object->second.components.push_back(component); }
            else if (wcscmp(local,L"item") == 0) { BuildItem item{}; if (!ParseUnsigned(Attribute(reader.Get(),L"objectid"),item.objectId) || item.objectId==0 || !ParseTransform(Attribute(reader.Get(),L"transform"),item.transform)) { error=L"The 3MF contains an invalid build item transform."; return false; } model.buildItems.push_back(item); }
        } else if (type == XmlNodeType_EndElement) { const wchar_t* local=nullptr; if (SUCCEEDED(reader->GetLocalName(&local,nullptr)) && local && wcscmp(local,L"object")==0) currentObject=0; }
    }
    if (!haveModel || model.objects.empty() || model.buildItems.empty()) { error=L"The 3MF contains no usable build geometry."; return false; }
    for (const auto& [id, object] : model.objects) for (const SourceTriangle& triangle : object.triangles) if (triangle.first>=object.vertices.size() || triangle.second>=object.vertices.size() || triangle.third>=object.vertices.size()) { error=L"The 3MF contains a triangle with an invalid vertex index."; return false; }
    return true;
}

bool AppendTriangle(MeshGeometry& mesh, ModelBounds& bounds, bool& hasBounds, Float3 a, Float3 b, Float3 c) {
    if (!Finite(a)||!Finite(b)||!Finite(c)||mesh.indices.size()/3>=kMaxTriangles||mesh.positions.size()>std::numeric_limits<uint32_t>::max()-3) return false;
    Float3 normal=Cross(Sub(b,a),Sub(c,a)); const float length=Length(normal); if (!std::isfinite(length)) return false; if (length<=1e-20f) return true; normal={normal.x/length,normal.y/length,normal.z/length}; const uint32_t first=uint32_t(mesh.positions.size());
    for (const Float3 point : {a,b,c}) { mesh.positions.push_back(point); mesh.normals.push_back(normal); mesh.indices.push_back(uint32_t(mesh.indices.size())); if(!hasBounds){bounds.minimum=bounds.maximum=point;hasBounds=true;}else{bounds.minimum.x=std::min(bounds.minimum.x,point.x);bounds.minimum.y=std::min(bounds.minimum.y,point.y);bounds.minimum.z=std::min(bounds.minimum.z,point.z);bounds.maximum.x=std::max(bounds.maximum.x,point.x);bounds.maximum.y=std::max(bounds.maximum.y,point.y);bounds.maximum.z=std::max(bounds.maximum.z,point.z);} }
    (void)first; return true;
}
void BuildSnapPlanes(ModelDocument& document) { const auto& mesh=document.geometries.front(); const Float3 span=Sub(document.bounds.maximum,document.bounds.minimum); const float scale=std::max(1e-5f,Length(span)), normalStep=.002f, offsetStep=scale*1e-4f; std::unordered_map<std::string,uint32_t> ids; const size_t count=mesh.indices.size()/3; document.triangleSnapPlanes.resize(count); for(size_t triangle=0;triangle<count;++triangle){const Float3 normal=mesh.normals[mesh.indices[triangle*3]];const float offset=Dot(normal,mesh.positions[mesh.indices[triangle*3]]);const auto q=[](float v,float step){return int(std::lround(v/step));};const std::string key=std::to_string(q(normal.x,normalStep))+":"+std::to_string(q(normal.y,normalStep))+":"+std::to_string(q(normal.z,normalStep))+":"+std::to_string(q(offset,offsetStep));auto [entry,added]=ids.emplace(key,uint32_t(document.snapPlanes.size()));if(added)document.snapPlanes.push_back({normal,offset,{}});document.triangleSnapPlanes[triangle]=entry->second;document.snapPlanes[entry->second].triangles.push_back(uint32_t(triangle));} }

bool FlattenObject(const ParsedModel& source, uint32_t objectId, const Matrix4& transform, uint32_t buildItemIndex, uint32_t rootObjectId, uint32_t depth, std::unordered_set<uint32_t>& stack, MeshGeometry& mesh, ModelBounds& bounds, bool& hasBounds, std::vector<ModelInstanceRange>& ranges, std::wstring& error) {
    if (depth>kMaxComponentDepth || stack.contains(objectId)) { error=L"The 3MF contains cyclic or excessively nested components."; return false; }
    const auto found=source.objects.find(objectId); if(found==source.objects.end()) { error=L"The 3MF references a missing object."; return false; }
    stack.insert(objectId); const SourceObject& object=found->second; const uint32_t firstTriangle=uint32_t(mesh.indices.size()/3);
    for(const SourceTriangle& triangle:object.triangles){const auto point=[&](uint32_t index){Float3 p=TransformPoint(object.vertices[index],transform);return Float3{float(p.x*source.scaleMillimeters),float(p.y*source.scaleMillimeters),float(p.z*source.scaleMillimeters)};};if(!AppendTriangle(mesh,bounds,hasBounds,point(triangle.first),point(triangle.second),point(triangle.third))){error=L"The 3MF geometry is invalid or exceeds the model limit.";stack.erase(objectId);return false;}}
    const uint32_t ownTriangles=uint32_t(mesh.indices.size()/3)-firstTriangle; if(ownTriangles)ranges.push_back({rootObjectId,buildItemIndex,firstTriangle,ownTriangles});
    for(const SourceComponent& component:object.components)if(!FlattenObject(source,component.objectId,Multiply(component.transform,transform),buildItemIndex,rootObjectId,depth+1,stack,mesh,bounds,hasBounds,ranges,error)){stack.erase(objectId);return false;}
    stack.erase(objectId); return true;
}

}

ThreeMfLoadResult LoadThreeMfDocument(const std::wstring& path) {
    std::vector<unsigned char> xml; std::wstring partPath,error; if(!ReadThreeMfModelXml(path,xml,partPath,error))return {nullptr,std::move(error)}; ComPtr<IStream> stream=SHCreateMemStream(xml.data(),static_cast<UINT>(xml.size())); if(!stream)return {nullptr,L"The 3MF model part could not be read."};
    ParsedModel source; if(!ParseModelXml(stream.Get(),source,error)) return {nullptr,std::move(error)};
    MeshGeometry mesh; ModelBounds bounds{}; bool hasBounds=false; std::vector<ModelInstanceRange> ranges; for(uint32_t index=0;index<source.buildItems.size();++index){std::unordered_set<uint32_t> stack;if(!FlattenObject(source,source.buildItems[index].objectId,source.buildItems[index].transform,index,source.buildItems[index].objectId,0,stack,mesh,bounds,hasBounds,ranges,error))return {nullptr,std::move(error)};}
    if(mesh.indices.empty()||!hasBounds) return {nullptr,L"The 3MF build contains no usable triangles."}; auto document=std::make_shared<ModelDocument>(); document->bounds=bounds; document->geometries.push_back(std::move(mesh)); document->instances.push_back({0,Matrix4::Identity()}); document->sourceFormat=ModelSourceFormat::ThreeMf; document->sourceUnit=source.unit; document->unitScaleMillimeters=source.scaleMillimeters; document->metersPerUnit=.001; document->instanceRanges=std::move(ranges); BuildSnapPlanes(*document);
#if defined(_DEBUG)
    wchar_t message[512]{}; swprintf_s(message,L"Viewtrious 3MF: part=%s unit=%s scale=%.6g objects=%zu build=%zu instances=%zu vertices=%zu triangles=%zu\n",partPath.c_str(),document->sourceUnit.c_str(),document->unitScaleMillimeters,source.objects.size(),source.buildItems.size(),document->instanceRanges.size(),document->geometries.front().positions.size(),document->geometries.front().indices.size()/3); OutputDebugStringW(message);
#endif
    return {std::move(document),L""};
}
