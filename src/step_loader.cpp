#include "step_loader.h"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopLoc_Location.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <unordered_map>

namespace {
constexpr uint64_t kMaxVertices = 30'000'000ull;
constexpr uint64_t kMaxTriangles = 10'000'000ull;
constexpr double kLinearDeflectionMillimeters = 0.1;
constexpr double kAngularDeflectionRadians = 0.5;

struct StepTopology { occ::handle<TDocStd_Document> document; };

Float3 Sub(Float3 a, Float3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
Float3 Cross(Float3 a, Float3 b) { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
float Dot(Float3 a, Float3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
float Length(Float3 a) { return std::sqrt(Dot(a, a)); }
bool Finite(Float3 value) { return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z); }
std::string Utf8(const std::wstring& value) {
    const std::u8string converted = std::filesystem::path(value).u8string();
    return {reinterpret_cast<const char*>(converted.data()), converted.size()};
}
void Extend(ModelBounds& bounds, bool& hasBounds, Float3 value) {
    if (!hasBounds) { bounds.minimum = bounds.maximum = value; hasBounds = true; return; }
    bounds.minimum.x = std::min(bounds.minimum.x, value.x); bounds.minimum.y = std::min(bounds.minimum.y, value.y); bounds.minimum.z = std::min(bounds.minimum.z, value.z);
    bounds.maximum.x = std::max(bounds.maximum.x, value.x); bounds.maximum.y = std::max(bounds.maximum.y, value.y); bounds.maximum.z = std::max(bounds.maximum.z, value.z);
}
bool AppendTriangle(MeshGeometry& mesh, ModelBounds& bounds, bool& hasBounds, Float3 a, Float3 b, Float3 c, uint32_t faceId, std::vector<uint32_t>& faceIds) {
    if (!Finite(a) || !Finite(b) || !Finite(c) || mesh.indices.size()/3 >= kMaxTriangles || mesh.positions.size() > std::numeric_limits<uint32_t>::max()-3) return false;
    Float3 normal = Cross(Sub(b,a), Sub(c,a)); const float length = Length(normal);
    if (!std::isfinite(length)) return false;
    if (length <= 1e-20f) return true;
    normal = {normal.x/length, normal.y/length, normal.z/length};
    for (Float3 point : {a,b,c}) { mesh.positions.push_back(point); mesh.normals.push_back(normal); mesh.indices.push_back(uint32_t(mesh.indices.size())); Extend(bounds, hasBounds, point); }
    faceIds.push_back(faceId); return true;
}
void BuildSnapPlanes(ModelDocument& document) {
    const auto& mesh = document.geometries.front(); const Float3 span = Sub(document.bounds.maximum, document.bounds.minimum); const float scale = std::max(1e-5f, Length(span)), normalStep = .002f, offsetStep = scale*1e-4f;
    std::unordered_map<std::string,uint32_t> ids; document.triangleSnapPlanes.resize(mesh.indices.size()/3);
    for (size_t triangle=0; triangle<mesh.indices.size()/3; ++triangle) { const Float3 normal=mesh.normals[mesh.indices[triangle*3]]; const float offset=Dot(normal,mesh.positions[mesh.indices[triangle*3]]); const auto q=[](float v,float step){return int(std::lround(v/step));}; const std::string key=std::to_string(q(normal.x,normalStep))+":"+std::to_string(q(normal.y,normalStep))+":"+std::to_string(q(normal.z,normalStep))+":"+std::to_string(q(offset,offsetStep)); auto [entry,added]=ids.emplace(key,uint32_t(document.snapPlanes.size())); if(added) document.snapPlanes.push_back({normal,offset,{}}); document.triangleSnapPlanes[triangle]=entry->second; document.snapPlanes[entry->second].triangles.push_back(uint32_t(triangle)); }
}
bool AppendInstance(const TDF_Label& label, const occ::handle<XCAFDoc_ShapeTool>& shapeTool, MeshGeometry& mesh, ModelBounds& documentBounds, bool& hasDocumentBounds, std::vector<ModelInstanceRange>& ranges, std::vector<uint32_t>& faceIds, uint32_t instanceId, std::wstring& error) {
    const TopoDS_Shape shape = shapeTool->GetShape(label); if (shape.IsNull()) return true;
    BRepMesh_IncrementalMesh tessellator(shape, kLinearDeflectionMillimeters, false, kAngularDeflectionRadians, true);
    const uint32_t firstVertex = uint32_t(mesh.positions.size()), firstTriangle = uint32_t(mesh.indices.size()/3); ModelBounds bounds{}; bool hasBounds = false; uint32_t faceId = uint32_t(faceIds.size());
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More(); explorer.Next(), ++faceId) {
        const TopoDS_Face face = TopoDS::Face(explorer.Current()); TopLoc_Location location; const occ::handle<Poly_Triangulation> triangulation=BRep_Tool::Triangulation(face,location); if(triangulation.IsNull()) continue;
        for (int triangleIndex=1; triangleIndex<=triangulation->NbTriangles(); ++triangleIndex) { const Poly_Triangle& triangle=triangulation->Triangle(triangleIndex); int ia,ib,ic; triangle.Get(ia,ib,ic); gp_Pnt pa=triangulation->Node(ia).Transformed(location.Transformation()), pb=triangulation->Node(ib).Transformed(location.Transformation()), pc=triangulation->Node(ic).Transformed(location.Transformation()); Float3 a{float(pa.X()),float(pa.Y()),float(pa.Z())},b{float(pb.X()),float(pb.Y()),float(pb.Z())},c{float(pc.X()),float(pc.Y()),float(pc.Z())}; if(face.Orientation()==TopAbs_REVERSED) std::swap(b,c); if(!AppendTriangle(mesh,bounds,hasBounds,a,b,c,faceId,faceIds)){error=L"The STEP model is invalid or exceeds the model safety limit.";return false;} }
    }
    const uint32_t triangleCount=uint32_t(mesh.indices.size()/3)-firstTriangle; if(triangleCount){ranges.push_back({instanceId,instanceId,firstVertex,uint32_t(mesh.positions.size())-firstVertex,firstTriangle,triangleCount,bounds}); if(!hasDocumentBounds){documentBounds=bounds;hasDocumentBounds=true;}else{Extend(documentBounds,hasDocumentBounds,bounds.minimum);Extend(documentBounds,hasDocumentBounds,bounds.maximum);}}
    return true;
}
bool AppendLabel(const TDF_Label& label, const occ::handle<XCAFDoc_ShapeTool>& shapeTool, MeshGeometry& mesh, ModelBounds& bounds, bool& hasBounds, std::vector<ModelInstanceRange>& ranges, std::vector<uint32_t>& faceIds, uint32_t& nextId, std::wstring& error) {
    NCollection_Sequence<TDF_Label> components; if (shapeTool->GetComponents(label, components, false)) { for(int i=1;i<=components.Length();++i) if(!AppendLabel(components.Value(i),shapeTool,mesh,bounds,hasBounds,ranges,faceIds,nextId,error)) return false; return true; }
    return AppendInstance(label,shapeTool,mesh,bounds,hasBounds,ranges,faceIds,nextId++,error);
}
}

StepLoadResult LoadStepDocument(const std::wstring& path) {
    STEPCAFControl_Reader reader; reader.SetNameMode(true); reader.SetColorMode(true); reader.SetLayerMode(true);
    const std::string utf8Path=Utf8(path); if(utf8Path.empty() || reader.ReadFile(utf8Path.c_str()) != IFSelect_RetDone) return {nullptr,L"The STEP file could not be read."};
    auto topology=std::make_shared<StepTopology>(); topology->document=new TDocStd_Document("MDTV-XCAF"); if(!reader.Transfer(topology->document)) return {nullptr,L"The STEP file contains no transferable CAD geometry."};
    const occ::handle<XCAFDoc_ShapeTool> shapeTool=XCAFDoc_DocumentTool::ShapeTool(topology->document->Main()); NCollection_Sequence<TDF_Label> roots; shapeTool->GetFreeShapes(roots);
    MeshGeometry mesh; ModelBounds bounds{}; bool hasBounds=false; std::vector<ModelInstanceRange> ranges; std::vector<uint32_t> faceIds; uint32_t nextId=0; std::wstring error;
    for(int i=1;i<=roots.Length();++i) if(!AppendLabel(roots.Value(i),shapeTool,mesh,bounds,hasBounds,ranges,faceIds,nextId,error)) return {nullptr,std::move(error)};
    if(mesh.indices.empty() || !hasBounds || ranges.empty()) return {nullptr,L"The STEP file contains no usable tessellated faces."};
    double metersPerUnit=.001; XCAFDoc_DocumentTool::GetLengthUnit(topology->document,metersPerUnit);
    auto document=std::make_shared<ModelDocument>(); document->bounds=bounds; document->geometries.push_back(std::move(mesh)); document->instances.push_back({0,Matrix4::Identity()}); document->sourceFormat=ModelSourceFormat::Step; document->sourceUnit=L"STEP"; document->unitScaleMillimeters=metersPerUnit*1000.0; document->metersPerUnit=metersPerUnit; document->instanceRanges=std::move(ranges); document->triangleCadFaceIds=std::move(faceIds); document->cadTopology=std::static_pointer_cast<void>(topology); BuildSnapPlanes(*document); return {std::move(document),L""};
}
