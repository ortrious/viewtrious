#include "stl_loader.h"

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

namespace {
constexpr uint64_t kMaxFileBytes = 1024ull * 1024ull * 1024ull;
constexpr uint64_t kMaxTriangles = 10'000'000ull;
bool Finite(const Float3& value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
Float3 Sub(Float3 a, Float3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Float3 Cross(Float3 a, Float3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
float Length(Float3 a) { return std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); }
bool AppendTriangle(MeshGeometry& mesh, ModelBounds& bounds, bool& hasBounds, Float3 a, Float3 b, Float3 c) {
    if (!Finite(a) || !Finite(b) || !Finite(c)) return false;
    Float3 normal = Cross(Sub(b,a), Sub(c,a)); const float length = Length(normal); if (!std::isfinite(length) || length <= 1e-20f) return true;
    normal = {normal.x/length,normal.y/length,normal.z/length};
    if (mesh.positions.size() > kMaxTriangles * 3 - 3) return false;
    const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
    for (const Float3 v : {a,b,c}) { mesh.positions.push_back(v); mesh.normals.push_back(normal); mesh.indices.push_back(base + static_cast<uint32_t>(mesh.indices.size() - base));
        if (!hasBounds) { bounds.minimum=bounds.maximum=v; hasBounds=true; } else { bounds.minimum.x=std::min(bounds.minimum.x,v.x); bounds.minimum.y=std::min(bounds.minimum.y,v.y); bounds.minimum.z=std::min(bounds.minimum.z,v.z); bounds.maximum.x=std::max(bounds.maximum.x,v.x); bounds.maximum.y=std::max(bounds.maximum.y,v.y); bounds.maximum.z=std::max(bounds.maximum.z,v.z); }}
    return true;
}
float ReadFloat(const unsigned char* bytes) { float value; std::memcpy(&value, bytes, sizeof(value)); return value; }
uint32_t ReadU32(const unsigned char* bytes) { return uint32_t(bytes[0]) | (uint32_t(bytes[1])<<8) | (uint32_t(bytes[2])<<16) | (uint32_t(bytes[3])<<24); }
StlLoadResult MakeDocument(MeshGeometry&& mesh, ModelBounds bounds, bool hasBounds) { if (mesh.indices.empty() || !hasBounds) return {nullptr,L"The STL contains no usable triangles."}; auto document=std::make_shared<ModelDocument>(); document->bounds=bounds; document->geometries.push_back(std::move(mesh)); document->instances.push_back({0,Matrix4::Identity()}); return {std::move(document),L""}; }
StlLoadResult ParseBinary(const std::vector<unsigned char>& data) { const uint32_t count=ReadU32(data.data()+80); MeshGeometry mesh; ModelBounds bounds{}; bool hasBounds=false; for(uint32_t i=0;i<count;++i){const unsigned char* p=data.data()+84+size_t(i)*50; Float3 a{ReadFloat(p+12),ReadFloat(p+16),ReadFloat(p+20)},b{ReadFloat(p+24),ReadFloat(p+28),ReadFloat(p+32)},c{ReadFloat(p+36),ReadFloat(p+40),ReadFloat(p+44)}; if(!AppendTriangle(mesh,bounds,hasBounds,a,b,c)) return {nullptr,L"The binary STL contains invalid coordinates or exceeds the model limit."};} return MakeDocument(std::move(mesh),bounds,hasBounds); }
StlLoadResult ParseAscii(const std::string& text) { std::istringstream input(text); std::string word; MeshGeometry mesh; ModelBounds bounds{}; bool hasBounds=false, sawSolid=false; while(input>>word){if(word=="solid"){sawSolid=true; std::getline(input,word);} else if(word=="facet"){std::string normal,outer,loop,vertex,endloop,endfacet; Float3 vertices[3]; if(!(input>>normal) || normal!="normal") return {nullptr,L"Malformed ASCII STL facet."}; std::getline(input,word); if(!(input>>outer>>loop) || outer!="outer" || loop!="loop") return {nullptr,L"Malformed ASCII STL loop."}; for(auto& v:vertices) if(!(input>>vertex>>v.x>>v.y>>v.z) || vertex!="vertex") return {nullptr,L"Malformed ASCII STL vertex."}; if(!(input>>endloop) || endloop!="endloop" || !(input>>endfacet) || endfacet!="endfacet") return {nullptr,L"Malformed ASCII STL facet."}; if(!AppendTriangle(mesh,bounds,hasBounds,vertices[0],vertices[1],vertices[2])) return {nullptr,L"The ASCII STL contains invalid coordinates or exceeds the model limit."};} else if(word=="endsolid") break; else { std::getline(input,word); }} if(!sawSolid) return {nullptr,L"The file is not a valid ASCII STL."}; return MakeDocument(std::move(mesh),bounds,hasBounds); }
}

StlLoadResult LoadStlDocument(const std::wstring& path) {
    std::error_code ec; const uint64_t size=std::filesystem::file_size(path,ec); if(ec) return {nullptr,L"The STL file could not be read."}; if(size>kMaxFileBytes) return {nullptr,L"The STL exceeds the 1 GB safety limit."};
    std::ifstream input(path,std::ios::binary); std::vector<unsigned char> bytes(static_cast<size_t>(size)); if(!input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()))) return {nullptr,L"The STL file could not be read."};
    if(bytes.size()>=84){const uint64_t count=ReadU32(bytes.data()+80); const uint64_t expected=84ull+count*50ull; if(count<=kMaxTriangles && expected==bytes.size()) return ParseBinary(bytes);}
    return ParseAscii(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
}
