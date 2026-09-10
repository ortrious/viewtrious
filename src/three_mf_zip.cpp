#include "three_mf_zip.h"

#include "miniz_tinfl.h"

#include <windows.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace {
constexpr uint64_t kMaxPackageBytes = 1024ull*1024ull*1024ull;
constexpr uint64_t kMaxEntryBytes = 256ull*1024ull*1024ull;
constexpr uint32_t kLocalHeader = 0x04034b50u, kCentralHeader = 0x02014b50u, kEndOfCentralDirectory = 0x06054b50u;
uint16_t Read16(const unsigned char* bytes) { return uint16_t(bytes[0])|uint16_t(bytes[1])<<8; }
uint32_t Read32(const unsigned char* bytes) { return uint32_t(bytes[0])|uint32_t(bytes[1])<<8|uint32_t(bytes[2])<<16|uint32_t(bytes[3])<<24; }
struct Entry { uint16_t flags=0, method=0; uint32_t compressed=0, uncompressed=0, localOffset=0; };

bool Extract(const std::vector<unsigned char>& package, const Entry& entry, std::vector<unsigned char>& output, std::wstring& error) {
    if((entry.flags&1)!=0 || entry.method!=0 && entry.method!=8 || entry.uncompressed>kMaxEntryBytes || entry.compressed>kMaxEntryBytes || entry.localOffset>package.size() || package.size()-entry.localOffset<30 || Read32(package.data()+entry.localOffset)!=kLocalHeader){error=L"The 3MF package contains an unsupported or malformed ZIP entry.";return false;}
    const size_t header=entry.localOffset, data=header+30ull+Read16(package.data()+header+26)+Read16(package.data()+header+28); if(data>package.size() || entry.compressed>package.size()-data){error=L"The 3MF package contains a truncated ZIP entry.";return false;}
    output.resize(entry.uncompressed); if(entry.method==0){if(entry.compressed!=entry.uncompressed){error=L"The 3MF package has an invalid stored ZIP entry.";return false;}std::copy_n(package.data()+data,entry.uncompressed,output.data());return true;}
    const size_t result=tinfl_decompress_mem_to_mem(output.data(),output.size(),package.data()+data,entry.compressed,0);if(result==TINFL_DECOMPRESS_MEM_TO_MEM_FAILED||result!=output.size()){error=L"The 3MF package contains invalid DEFLATE data.";return false;}return true;
}
bool ReadPackage(const std::wstring& path, std::unordered_map<std::string,Entry>& entries, std::vector<unsigned char>& package, std::wstring& error) {
    std::error_code ec; const uint64_t size=std::filesystem::file_size(path,ec);if(ec||size<22){error=L"The file is not a valid 3MF package.";return false;}if(size>kMaxPackageBytes){error=L"The 3MF package exceeds the 1 GB safety limit.";return false;}std::ifstream input(path,std::ios::binary);package.resize(size_t(size));if(!input.read(reinterpret_cast<char*>(package.data()),std::streamsize(package.size()))){error=L"The 3MF package could not be read.";return false;}
    const size_t searchStart=package.size()>22+0xffff?package.size()-(22+0xffff):0;size_t eocd=package.size();for(size_t pos=package.size()-22;;--pos){if(Read32(package.data()+pos)==kEndOfCentralDirectory){eocd=pos;break;}if(pos==searchStart)break;}if(eocd==package.size()){error=L"The file is not a valid ZIP/3MF package.";return false;}
    const uint16_t count=Read16(package.data()+eocd+10);const uint32_t directorySize=Read32(package.data()+eocd+12),directoryOffset=Read32(package.data()+eocd+16);if(count==0xffff||directorySize==0xffffffffu||directoryOffset==0xffffffffu||directoryOffset>package.size()||directorySize>package.size()-directoryOffset){error=L"ZIP64 or malformed 3MF packages are unsupported.";return false;}
    size_t cursor=directoryOffset;for(uint16_t i=0;i<count;++i){if(cursor>package.size()||package.size()-cursor<46||Read32(package.data()+cursor)!=kCentralHeader){error=L"The 3MF central directory is malformed.";return false;}const uint16_t nameLength=Read16(package.data()+cursor+28),extraLength=Read16(package.data()+cursor+30),commentLength=Read16(package.data()+cursor+32);const size_t record=46ull+nameLength+extraLength+commentLength;if(record>package.size()-cursor){error=L"The 3MF central directory is truncated.";return false;}const uint32_t compressed=Read32(package.data()+cursor+20),uncompressed=Read32(package.data()+cursor+24),localOffset=Read32(package.data()+cursor+42);if(compressed==0xffffffffu||uncompressed==0xffffffffu||localOffset==0xffffffffu){error=L"ZIP64 3MF entries are unsupported.";return false;}std::string name(reinterpret_cast<const char*>(package.data()+cursor+46),nameLength);entries.emplace(std::move(name),Entry{Read16(package.data()+cursor+8),Read16(package.data()+cursor+10),compressed,uncompressed,localOffset});cursor+=record;}
    return true;
}
std::wstring Attribute(IXmlReader* reader, const wchar_t* name) {
    if(reader->MoveToFirstAttribute()!=S_OK)return {};do{const wchar_t* local=nullptr;const wchar_t* value=nullptr;if(SUCCEEDED(reader->GetLocalName(&local,nullptr))&&SUCCEEDED(reader->GetValue(&value,nullptr))&&local&&value&&wcscmp(local,name)==0){reader->MoveToElement();return value;}}while(reader->MoveToNextAttribute()==S_OK);reader->MoveToElement();return {};
}
bool RelationshipModelPart(const std::unordered_map<std::string,Entry>& entries, const std::vector<unsigned char>& package, std::string& modelName, std::wstring& error) {
    const auto relationships=entries.find("_rels/.rels");if(relationships==entries.end())return true;std::vector<unsigned char> xml;if(!Extract(package,relationships->second,xml,error))return false;ComPtr<IStream> stream=SHCreateMemStream(xml.data(),static_cast<UINT>(xml.size()));ComPtr<IXmlReader> reader;if(!stream||FAILED(CreateXmlReader(IID_PPV_ARGS(&reader),nullptr))||FAILED(reader->SetInput(stream.Get()))){error=L"The 3MF package relationships could not be read.";return false;}XmlNodeType type{};while(reader->Read(&type)==S_OK)if(type==XmlNodeType_Element){const wchar_t* local=nullptr;if(FAILED(reader->GetLocalName(&local,nullptr))||!local)continue;if(wcscmp(local,L"Relationship")!=0)continue;const std::wstring relationshipType=Attribute(reader.Get(),L"Type");const std::wstring target=Attribute(reader.Get(),L"Target");if(relationshipType.find(L"3dmodel")==std::wstring::npos||target.empty())continue;modelName.clear();for(wchar_t character:target){if(character>0x7f){error=L"The 3MF relationship target is unsupported.";return false;}modelName.push_back(static_cast<char>(character));}while(!modelName.empty()&&modelName.front()=='/')modelName.erase(modelName.begin());return true;}return true;
}
}

bool ReadThreeMfModelXml(const std::wstring& path, std::vector<unsigned char>& xml, std::wstring& modelPartPath, std::wstring& error) {
    std::unordered_map<std::string,Entry> entries;std::vector<unsigned char> package;if(!ReadPackage(path,entries,package,error))return false;
    std::string modelName;if(!RelationshipModelPart(entries,package,modelName,error))return false;if(!modelName.empty()&&!entries.contains(modelName)){error=L"The 3MF package relationship references a missing model part.";return false;}if(modelName.empty())for(const auto& [name,entry]:entries)if(name.size()>6&&name.ends_with(".model")){if(!modelName.empty()){error=L"The 3MF package has multiple model parts without a resolvable primary part.";return false;}modelName=name;}
    if(modelName.empty()){error=L"The 3MF package has no model part.";return false;}const auto model=entries.find(modelName);if(model==entries.end()||!Extract(package,model->second,xml,error))return false;modelPartPath.assign(modelName.begin(),modelName.end());return true;
}

bool ReadThreeMfModelXmlPart(const std::wstring& path, const std::wstring& modelPartPath, std::vector<unsigned char>& xml, std::wstring& error) {
    std::string partName; partName.reserve(modelPartPath.size());
    for (wchar_t character : modelPartPath) { if (character > 0x7f) { error=L"The 3MF referenced model part is unsupported."; return false; } partName.push_back(static_cast<char>(character)); }
    while (!partName.empty() && partName.front() == '/') partName.erase(partName.begin());
    if (partName.empty() || partName.find("..") != std::string::npos) { error=L"The 3MF referenced model part path is invalid."; return false; }
    std::unordered_map<std::string,Entry> entries; std::vector<unsigned char> package;
    if (!ReadPackage(path, entries, package, error)) return false;
    const auto part = entries.find(partName);
    if (part == entries.end()) { error=L"The 3MF component references a missing model part."; return false; }
    return Extract(package, part->second, xml, error);
}
