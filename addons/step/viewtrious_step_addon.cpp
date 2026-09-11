#include "model_importer_abi.h"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
constexpr double kLinearDeflectionMillimeters = .1, kAngularDeflectionRadians = .5;
struct Topology { occ::handle<TDocStd_Document> document; };
struct ResultStore {
    std::vector<ViewtriousImporterFloat3> positions, normals;
    std::vector<uint32_t> indices, faceIds;
    std::vector<ViewtriousImporterRange> ranges;
    ViewtriousImporterBounds bounds{}; bool hasBounds=false;
    std::wstring error;
    void Extend(ViewtriousImporterFloat3 value) { if(!hasBounds){bounds.minimum=bounds.maximum=value;hasBounds=true;return;} bounds.minimum.x=std::min(bounds.minimum.x,value.x);bounds.minimum.y=std::min(bounds.minimum.y,value.y);bounds.minimum.z=std::min(bounds.minimum.z,value.z);bounds.maximum.x=std::max(bounds.maximum.x,value.x);bounds.maximum.y=std::max(bounds.maximum.y,value.y);bounds.maximum.z=std::max(bounds.maximum.z,value.z); }
};
struct ProgressReporter {
    ViewtriousImporterProgressCallback callback = nullptr;
    void* context = nullptr;
    float last = 0.0f;
    void Report(float value) { if (!callback) return; value = std::clamp(value, last, 1.0f); if (value == last) return; last = value; callback(context, value); }
};
float Dot(ViewtriousImporterFloat3 a,ViewtriousImporterFloat3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
ViewtriousImporterFloat3 Sub(ViewtriousImporterFloat3 a,ViewtriousImporterFloat3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
ViewtriousImporterFloat3 Cross(ViewtriousImporterFloat3 a,ViewtriousImporterFloat3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
bool AppendTriangle(ResultStore& store, ViewtriousImporterBounds& local, bool& hasLocal, ViewtriousImporterFloat3 a,ViewtriousImporterFloat3 b,ViewtriousImporterFloat3 c,uint32_t faceId){auto n=Cross(Sub(b,a),Sub(c,a));const float l=std::sqrt(Dot(n,n));if(!std::isfinite(l))return false;if(l<=1e-20f)return true;n={n.x/l,n.y/l,n.z/l};for(auto point:{a,b,c}){store.positions.push_back(point);store.normals.push_back(n);store.indices.push_back(uint32_t(store.indices.size()));store.Extend(point);if(!hasLocal){local.minimum=local.maximum=point;hasLocal=true;}else{local.minimum.x=std::min(local.minimum.x,point.x);local.minimum.y=std::min(local.minimum.y,point.y);local.minimum.z=std::min(local.minimum.z,point.z);local.maximum.x=std::max(local.maximum.x,point.x);local.maximum.y=std::max(local.maximum.y,point.y);local.maximum.z=std::max(local.maximum.z,point.z);}}store.faceIds.push_back(faceId);return true;}
bool AppendInstance(const TDF_Label& label,const occ::handle<XCAFDoc_ShapeTool>& shapes,ResultStore& store,uint32_t id){const TopoDS_Shape shape=shapes->GetShape(label);if(shape.IsNull())return true;BRepMesh_IncrementalMesh tess(shape,kLinearDeflectionMillimeters,false,kAngularDeflectionRadians,true);const uint32_t firstVertex=uint32_t(store.positions.size()),firstTriangle=uint32_t(store.indices.size()/3);ViewtriousImporterBounds bounds{};bool hasBounds=false;uint32_t faceId=uint32_t(store.faceIds.size());for(TopExp_Explorer exp(shape,TopAbs_FACE);exp.More();exp.Next(),++faceId){const TopoDS_Face face=TopoDS::Face(exp.Current());TopLoc_Location location;const auto triangulation=BRep_Tool::Triangulation(face,location);if(triangulation.IsNull())continue;for(int i=1;i<=triangulation->NbTriangles();++i){const auto& tri=triangulation->Triangle(i);int ia,ib,ic;tri.Get(ia,ib,ic);auto point=[&](int index){const gp_Pnt p=triangulation->Node(index).Transformed(location.Transformation());return ViewtriousImporterFloat3{float(p.X()),float(p.Y()),float(p.Z())};};auto a=point(ia),b=point(ib),c=point(ic);if(face.Orientation()==TopAbs_REVERSED)std::swap(b,c);if(!AppendTriangle(store,bounds,hasBounds,a,b,c,faceId))return false;}}const uint32_t triangles=uint32_t(store.indices.size()/3)-firstTriangle;if(triangles)store.ranges.push_back({id,id,firstVertex,uint32_t(store.positions.size())-firstVertex,firstTriangle,triangles,bounds});return true;}
uint32_t CountLeafLabels(const TDF_Label& label,const occ::handle<XCAFDoc_ShapeTool>& shapes){NCollection_Sequence<TDF_Label> children;if(!shapes->GetComponents(label,children,false))return 1;uint32_t count=0;for(int i=1;i<=children.Length();++i)count+=CountLeafLabels(children.Value(i),shapes);return count;}
bool AppendLabel(const TDF_Label& label,const occ::handle<XCAFDoc_ShapeTool>& shapes,ResultStore& store,uint32_t& id,uint32_t total,uint32_t& completed,ProgressReporter& progress){NCollection_Sequence<TDF_Label> children;if(shapes->GetComponents(label,children,false)){for(int i=1;i<=children.Length();++i)if(!AppendLabel(children.Value(i),shapes,store,id,total,completed,progress))return false;return true;}if(!AppendInstance(label,shapes,store,id++))return false;++completed;progress.Report(.35f+.60f*float(completed)/std::max(1u,total));return true;}
void FillResult(ResultStore* store,void* context,double meters,ViewtriousImporterResult* result){result->structSize=sizeof(*result);result->apiVersion=kViewtriousModelImporterApiVersion;result->positions=store->positions.data();result->normals=store->normals.data();result->indices=store->indices.data();result->ranges=store->ranges.data();result->triangleCadFaceIds=store->faceIds.data();result->positionCount=uint32_t(store->positions.size());result->indexCount=uint32_t(store->indices.size());result->rangeCount=uint32_t(store->ranges.size());result->triangleCadFaceIdCount=uint32_t(store->faceIds.size());result->bounds=store->bounds;result->metersPerUnit=meters;result->resultOwner=store;result->documentContext=context;result->error=store->error.c_str();}
bool LoadStep(const wchar_t* path,ViewtriousImporterProgressCallback callback,void* context,ViewtriousImporterResult* result){if(!result||result->structSize!=sizeof(*result)||result->apiVersion!=kViewtriousModelImporterApiVersion)return false;ProgressReporter progress{callback,context};auto store=std::make_unique<ResultStore>();auto topology=std::make_unique<Topology>();STEPCAFControl_Reader reader;reader.SetNameMode(true);reader.SetColorMode(true);reader.SetLayerMode(true);progress.Report(.02f);const std::u8string utf8=std::filesystem::path(path?path:L"").u8string();const std::string filename(reinterpret_cast<const char*>(utf8.data()),utf8.size());if(filename.empty()||reader.ReadFile(filename.c_str())!=IFSelect_RetDone){store->error=L"The STEP file could not be read.";FillResult(store.release(),nullptr,.001,result);return false;}progress.Report(.20f);topology->document=new TDocStd_Document("MDTV-XCAF");if(!reader.Transfer(topology->document)){store->error=L"The STEP file contains no transferable CAD geometry.";FillResult(store.release(),nullptr,.001,result);return false;}progress.Report(.35f);const auto shapes=XCAFDoc_DocumentTool::ShapeTool(topology->document->Main());NCollection_Sequence<TDF_Label> roots;shapes->GetFreeShapes(roots);uint32_t total=0;for(int i=1;i<=roots.Length();++i)total+=CountLeafLabels(roots.Value(i),shapes);uint32_t id=0,completed=0;for(int i=1;i<=roots.Length();++i)if(!AppendLabel(roots.Value(i),shapes,*store,id,total,completed,progress)){store->error=L"The STEP model could not be tessellated.";FillResult(store.release(),nullptr,.001,result);return false;}progress.Report(.95f);if(store->indices.empty()||!store->hasBounds){store->error=L"The STEP file contains no usable tessellated faces.";FillResult(store.release(),nullptr,.001,result);return false;}double meters=.001;XCAFDoc_DocumentTool::GetLengthUnit(topology->document,meters);progress.Report(.98f);FillResult(store.release(),topology.release(),meters,result);progress.Report(1.0f);return true;}
void ReleaseResult(ViewtriousImporterResult* result){if(!result)return;delete reinterpret_cast<ResultStore*>(result->resultOwner);*result={};}
void ReleaseContext(void* context){delete reinterpret_cast<Topology*>(context);}
}

extern "C" bool ViewtriousModelImporter_GetApi(uint32_t requestedVersion, ViewtriousModelImporterApi* api){if(!api||requestedVersion!=kViewtriousModelImporterApiVersion||api->structSize!=sizeof(*api))return false;*api={sizeof(*api),kViewtriousModelImporterApiVersion,&LoadStep,&ReleaseResult,&ReleaseContext};return true;}
