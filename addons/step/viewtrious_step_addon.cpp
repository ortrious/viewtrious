#include "model_importer_abi.h"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Poly_Triangulation.hxx>
#include <Quantity_ColorRGBA.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XCAFPrs.hxx>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr double kLinearDeflectionMillimeters = .1, kAngularDeflectionRadians = .5;
constexpr uint32_t kNoIndex = UINT32_MAX;
struct Topology { occ::handle<TDocStd_Document> document; };
struct ResultStore {
    std::vector<ViewtriousImporterFloat3> positions, normals;
    std::vector<ViewtriousImporterFloat4> colors;
    std::vector<uint8_t> colorFlags;
    std::vector<uint32_t> indices, faceIds, hierarchyChildIndices;
    std::vector<std::vector<uint32_t>> hierarchyChildren;
    std::vector<ViewtriousImporterRange> ranges;
    std::vector<ViewtriousImporterHierarchyNode> hierarchy;
    std::string hierarchyNamesUtf8;
    ViewtriousImporterBounds bounds{}; bool hasBounds = false;
    std::wstring error;
    void Extend(ViewtriousImporterFloat3 value) {
        if (!hasBounds) { bounds.minimum = bounds.maximum = value; hasBounds = true; return; }
        bounds.minimum.x = std::min(bounds.minimum.x, value.x); bounds.minimum.y = std::min(bounds.minimum.y, value.y); bounds.minimum.z = std::min(bounds.minimum.z, value.z);
        bounds.maximum.x = std::max(bounds.maximum.x, value.x); bounds.maximum.y = std::max(bounds.maximum.y, value.y); bounds.maximum.z = std::max(bounds.maximum.z, value.z);
    }
};
struct ProgressReporter {
    ViewtriousImporterProgressCallback callback = nullptr; void* context = nullptr; float last = 0.0f;
    void Report(float value) { if (!callback) return; value = std::clamp(value, last, 1.0f); if (value == last) return; last = value; callback(context, value); }
};
float Dot(ViewtriousImporterFloat3 a, ViewtriousImporterFloat3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
ViewtriousImporterFloat3 Sub(ViewtriousImporterFloat3 a, ViewtriousImporterFloat3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
ViewtriousImporterFloat3 Cross(ViewtriousImporterFloat3 a, ViewtriousImporterFloat3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }

std::string LabelNameUtf8(const TDF_Label& label) {
    occ::handle<TDataStd_Name> name;
    if (label.IsNull() || !label.FindAttribute(TDataStd_Name::GetID(), name) || name.IsNull()) return {};
    TCollection_ExtendedString value = name->Get(); value.LeftAdjust(); value.RightAdjust(); if (value.Length() == 0) return {};
    std::string utf8(static_cast<size_t>(value.LengthOfCString()) + 1, '\0'); Standard_PCharacter buffer = utf8.data(); const int length = value.ToUTF8CString(buffer); utf8.resize(std::max(0, length)); return utf8;
}
TDF_Label ReferredLabel(const TDF_Label& label) { TDF_Label referred; return XCAFDoc_ShapeTool::GetReferredShape(label, referred) ? referred : label; }
std::string DisplayName(const TDF_Label& occurrence, const TDF_Label& definition, bool assembly, uint32_t ordinal) {
    if (std::string name = LabelNameUtf8(occurrence); !name.empty() && name.rfind("=>[", 0) != 0) return name;
    if (std::string name = LabelNameUtf8(definition); !name.empty() && name != "ASSEMBLY" && name.rfind("=>[", 0) != 0) return name;
    return std::string(assembly ? "Assembly " : "Part ") + std::to_string(ordinal + 1);
}
using StyleMap = NCollection_IndexedDataMap<TopoDS_Shape, XCAFPrs_Style, TopTools_ShapeMapHasher>;
StyleMap CollectFaceStyles(const TDF_Label& occurrence, const TopLoc_Location& parentLocation) {
    StyleMap settings, faceStyles;
    XCAFPrs::CollectStyleSettings(occurrence, parentLocation, settings);
    // Match OCCT's own style dispatcher: specific face styles win; styles on
    // containing shells/solids/compounds fill only faces not styled already.
    for (int type = TopAbs_FACE; type >= TopAbs_COMPOUND; --type) for (StyleMap::Iterator setting(settings); setting.More(); setting.Next()) {
        const TopoDS_Shape& styledShape = setting.Key(); if (styledShape.ShapeType() != type) continue;
        if (type == TopAbs_FACE) { if (XCAFPrs_Style* existing = faceStyles.ChangeSeek(styledShape)) *existing = setting.Value(); else faceStyles.Add(styledShape, setting.Value()); continue; }
        for (TopExp_Explorer faces(styledShape, TopAbs_FACE); faces.More(); faces.Next()) if (!faceStyles.Contains(faces.Current())) faceStyles.Add(faces.Current(), setting.Value());
    }
    return faceStyles;
}
bool StyleColor(const StyleMap& styles, const TopoDS_Face& face, ViewtriousImporterFloat4& output) {
    const XCAFPrs_Style* style = styles.Seek(face); if (!style || !style->IsVisible()) return false;
    Quantity_ColorRGBA color;
    if (!style->Material().IsNull()) color = style->Material()->BaseColor();
    else if (style->IsSetColorSurf()) color = style->GetColorSurfRGBA();
    else if (style->IsSetColorCurv()) color = Quantity_ColorRGBA(style->GetColorCurv());
    else return false;
    const Quantity_Color& rgb = color.GetRGB(); output = {float(rgb.Red()),float(rgb.Green()),float(rgb.Blue()),color.Alpha()}; return true;
}
bool AppendTriangle(ResultStore& store, ViewtriousImporterBounds& local, bool& hasLocal, ViewtriousImporterFloat3 a, ViewtriousImporterFloat3 b, ViewtriousImporterFloat3 c, uint32_t faceId, ViewtriousImporterFloat4 color, bool authored) {
    auto normal=Cross(Sub(b,a),Sub(c,a)); const float length=std::sqrt(Dot(normal,normal)); if(!std::isfinite(length))return false;if(length<=1e-20f)return true;normal={normal.x/length,normal.y/length,normal.z/length};
    for(auto point:{a,b,c}){store.positions.push_back(point);store.normals.push_back(normal);store.colors.push_back(color);store.colorFlags.push_back(authored?kViewtriousImporterColorAuthored:0);store.indices.push_back(uint32_t(store.indices.size()));store.Extend(point);if(!hasLocal){local.minimum=local.maximum=point;hasLocal=true;}else{local.minimum.x=std::min(local.minimum.x,point.x);local.minimum.y=std::min(local.minimum.y,point.y);local.minimum.z=std::min(local.minimum.z,point.z);local.maximum.x=std::max(local.maximum.x,point.x);local.maximum.y=std::max(local.maximum.y,point.y);local.maximum.z=std::max(local.maximum.z,point.z);}}
    store.faceIds.push_back(faceId);return true;
}
bool AppendInstance(const TopoDS_Shape& shape,const StyleMap& styles,ResultStore& store,uint32_t nodeIndex){if(shape.IsNull())return true;BRepMesh_IncrementalMesh tess(shape,kLinearDeflectionMillimeters,false,kAngularDeflectionRadians,true);const uint32_t firstVertex=uint32_t(store.positions.size()),firstTriangle=uint32_t(store.indices.size()/3);ViewtriousImporterBounds bounds{};bool hasBounds=false;uint32_t faceId=uint32_t(store.faceIds.size());for(TopExp_Explorer explorer(shape,TopAbs_FACE);explorer.More();explorer.Next(),++faceId){const TopoDS_Face face=TopoDS::Face(explorer.Current());TopLoc_Location location;const auto triangulation=BRep_Tool::Triangulation(face,location);if(triangulation.IsNull())continue;ViewtriousImporterFloat4 color{0,0,0,1};const bool authored=StyleColor(styles,face,color);for(int i=1;i<=triangulation->NbTriangles();++i){const auto& triangle=triangulation->Triangle(i);int ia,ib,ic;triangle.Get(ia,ib,ic);const auto point=[&](int index){const gp_Pnt p=triangulation->Node(index).Transformed(location.Transformation());return ViewtriousImporterFloat3{float(p.X()),float(p.Y()),float(p.Z())};};auto a=point(ia),b=point(ib),c=point(ic);if(face.Orientation()==TopAbs_REVERSED)std::swap(b,c);if(!AppendTriangle(store,bounds,hasBounds,a,b,c,faceId,color,authored))return false;}}const uint32_t triangles=uint32_t(store.indices.size()/3)-firstTriangle;if(triangles)store.ranges.push_back({nodeIndex,nodeIndex,firstVertex,uint32_t(store.positions.size())-firstVertex,firstTriangle,triangles,bounds});return true;}
uint32_t CountLeaves(const TDF_Label& label,const occ::handle<XCAFDoc_ShapeTool>& shapes){NCollection_Sequence<TDF_Label> children;const TDF_Label definition=ReferredLabel(label);if(!shapes->GetComponents(definition,children,false))return 1;uint32_t count=0;for(int i=1;i<=children.Length();++i)count+=CountLeaves(children.Value(i),shapes);return count;}
bool AppendOccurrence(const TDF_Label& occurrence,const TopoDS_Shape& shape,uint32_t parentIndex,const occ::handle<XCAFDoc_ShapeTool>& shapes,const StyleMap& styles,ResultStore& store,uint32_t& completed,uint32_t total,ProgressReporter& progress){const TDF_Label definition=ReferredLabel(occurrence);NCollection_Sequence<TDF_Label> children;const bool assembly=shapes->GetComponents(definition,children,false);const uint32_t nodeIndex=uint32_t(store.hierarchy.size());ViewtriousImporterHierarchyNode node{parentIndex,0,0,kNoIndex,0,uint32_t(store.hierarchyNamesUtf8.size()),0,assembly?kViewtriousImporterHierarchyAssembly:0};const std::string name=DisplayName(occurrence,definition,assembly,nodeIndex);node.nameLength=uint32_t(name.size());store.hierarchyNamesUtf8+=name;store.hierarchy.push_back(node);store.hierarchyChildren.emplace_back();if(assembly){for(int i=1;i<=children.Length();++i){const uint32_t childIndex=uint32_t(store.hierarchy.size());const TopoDS_Shape childShape=shapes->GetShape(children.Value(i)).Moved(shape.Location());if(!AppendOccurrence(children.Value(i),childShape,nodeIndex,shapes,styles,store,completed,total,progress))return false;store.hierarchyChildren[nodeIndex].push_back(childIndex);}}else{const uint32_t firstRange=uint32_t(store.ranges.size());if(!AppendInstance(shape,styles,store,nodeIndex))return false;const uint32_t rangeCount=uint32_t(store.ranges.size())-firstRange;if(rangeCount){store.hierarchy[nodeIndex].flags|=kViewtriousImporterHierarchyRenderable;store.hierarchy[nodeIndex].firstRangeIndex=firstRange;store.hierarchy[nodeIndex].rangeCount=rangeCount;}++completed;progress.Report(.35f+.60f*float(completed)/std::max(1u,total));}return true;}
void FinalizeHierarchy(ResultStore& store){store.hierarchyChildIndices.clear();for(uint32_t index=0;index<store.hierarchy.size();++index){auto& node=store.hierarchy[index];node.firstChildIndex=uint32_t(store.hierarchyChildIndices.size());node.childCount=uint32_t(store.hierarchyChildren[index].size());store.hierarchyChildIndices.insert(store.hierarchyChildIndices.end(),store.hierarchyChildren[index].begin(),store.hierarchyChildren[index].end());}}
void FillResult(ResultStore* store,void* context,double meters,ViewtriousImporterResult* result){result->structSize=sizeof(*result);result->apiVersion=kViewtriousModelImporterApiVersion;result->positions=store->positions.data();result->normals=store->normals.data();result->vertexColors=store->colors.data();result->vertexColorFlags=store->colorFlags.data();result->indices=store->indices.data();result->ranges=store->ranges.data();result->triangleCadFaceIds=store->faceIds.data();result->hierarchyNodes=store->hierarchy.data();result->hierarchyChildIndices=store->hierarchyChildIndices.data();result->hierarchyNamesUtf8=store->hierarchyNamesUtf8.data();result->positionCount=uint32_t(store->positions.size());result->indexCount=uint32_t(store->indices.size());result->rangeCount=uint32_t(store->ranges.size());result->triangleCadFaceIdCount=uint32_t(store->faceIds.size());result->vertexColorCount=uint32_t(store->colors.size());result->hierarchyNodeCount=uint32_t(store->hierarchy.size());result->hierarchyChildIndexCount=uint32_t(store->hierarchyChildIndices.size());result->hierarchyNamesUtf8Count=uint32_t(store->hierarchyNamesUtf8.size());result->bounds=store->bounds;result->metersPerUnit=meters;result->resultOwner=store;result->documentContext=context;result->error=store->error.c_str();}
bool LoadStep(const wchar_t* path,ViewtriousImporterProgressCallback callback,void* context,ViewtriousImporterResult* result){if(!result||result->structSize!=sizeof(*result)||result->apiVersion!=kViewtriousModelImporterApiVersion)return false;ProgressReporter progress{callback,context};auto store=std::make_unique<ResultStore>();auto topology=std::make_unique<Topology>();STEPCAFControl_Reader reader;reader.SetNameMode(true);reader.SetColorMode(true);reader.SetLayerMode(true);progress.Report(.02f);const std::u8string utf8=std::filesystem::path(path?path:L"").u8string();const std::string filename(reinterpret_cast<const char*>(utf8.data()),utf8.size());if(filename.empty()||reader.ReadFile(filename.c_str())!=IFSelect_RetDone){store->error=L"The STEP file could not be read.";FillResult(store.release(),nullptr,.001,result);return false;}progress.Report(.20f);topology->document=new TDocStd_Document("MDTV-XCAF");if(!reader.Transfer(topology->document)){store->error=L"The STEP file contains no transferable CAD geometry.";FillResult(store.release(),nullptr,.001,result);return false;}progress.Report(.35f);const auto shapes=XCAFDoc_DocumentTool::ShapeTool(topology->document->Main());NCollection_Sequence<TDF_Label> roots;shapes->GetFreeShapes(roots);uint32_t total=0,completed=0;for(int i=1;i<=roots.Length();++i)total+=CountLeaves(roots.Value(i),shapes);const TopLoc_Location rootLocation;for(int i=1;i<=roots.Length();++i){const StyleMap styles=CollectFaceStyles(roots.Value(i),rootLocation);if(!AppendOccurrence(roots.Value(i),shapes->GetShape(roots.Value(i)),kNoIndex,shapes,styles,*store,completed,total,progress)){store->error=L"The STEP model could not be tessellated.";FillResult(store.release(),nullptr,.001,result);return false;}}FinalizeHierarchy(*store);progress.Report(.95f);if(store->indices.empty()||!store->hasBounds){store->error=L"The STEP file contains no usable tessellated faces.";FillResult(store.release(),nullptr,.001,result);return false;}double meters=.001;XCAFDoc_DocumentTool::GetLengthUnit(topology->document,meters);progress.Report(.98f);FillResult(store.release(),topology.release(),meters,result);progress.Report(1.0f);return true;}
void ReleaseResult(ViewtriousImporterResult* result){if(!result)return;delete reinterpret_cast<ResultStore*>(result->resultOwner);*result={};}
void ReleaseContext(void* context){delete reinterpret_cast<Topology*>(context);}
}
extern "C" bool ViewtriousModelImporter_GetApi(uint32_t requestedVersion,ViewtriousModelImporterApi* api){if(!api||requestedVersion!=kViewtriousModelImporterApiVersion||api->structSize!=sizeof(*api))return false;*api={sizeof(*api),kViewtriousModelImporterApiVersion,&LoadStep,&ReleaseResult,&ReleaseContext};return true;}
