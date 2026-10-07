#include "packages.h"
#include "manager.h"
#include "mods.h"
#include "mod_archive.h"
#include "content.h"
#include "cemu_pack.h"
#include "../platform/host.h"
#include "wwhd_mod.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace mods::packages {
namespace {
namespace fs=std::filesystem;
using json::Value;
struct Requirement {std::string id,version;};
struct Manifest {
    std::string id,name,version,author,description,kind,binary,problem,fingerprint; // fingerprint: SHA-256 of the native library
    std::vector<Requirement> dependencies;
    std::vector<std::string> conflicts;
    std::vector<Option> options;
    std::map<std::string,bool> settings;
    content::Files files;
    std::shared_ptr<cemu::Pack> graphics;
};
struct Record {Manifest manifest;fs::path path;bool active=false,loading=false;std::string status,error;Value startup_config;};
std::mutex mutex;
std::map<std::string,Record> records;
Value database;
fs::path root;
bool ready=false;
std::string last_problem;
std::atomic<bool> dirty{false},running{false},profile_changed{false};
ReadMemory read_memory=nullptr;WriteMemory write_memory=nullptr;
void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
bool id_ok(const std::string& s){return !s.empty()&&s.size()<=64&&s[0]!='.'&&std::all_of(s.begin(),s.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.';});}
std::array<unsigned,3> version(const std::string& s) {
    std::array<unsigned,3> result{};size_t p=0;
    for(int i=0;i<3;i++){size_t end=i==2?s.size():s.find('.',p);require(end!=std::string::npos&&end>p,"Version must be major.minor.patch");auto parse=std::from_chars(s.data()+p,s.data()+end,result[i]);require(parse.ec==std::errc()&&parse.ptr==s.data()+end,"Invalid version");p=end+1;}
    return result;
}
std::string string_field(const Value& v,const char* key,bool optional=false,size_t limit=8192){const auto& f=v.get(key);if(optional&&f.type==Value::Null)return {};require(f.type==Value::String&&f.text.size()<=limit&&f.text.find('\0')==std::string::npos,"Invalid field: "+std::string(key));return f.text;}
// Streaming SHA-256 (FIPS 180-4) of a native library: the confirmation is bound to these exact bytes.
std::string sha256_file(const fs::path& p){
    static constexpr uint32_t k[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    auto rotr=[](uint32_t x,int n){return (x>>n)|(x<<(32-n));};
    auto block=[&](const unsigned char* b){
        uint32_t w[64],v[8];for(int i=0;i<16;i++)w[i]=uint32_t(b[4*i])<<24|uint32_t(b[4*i+1])<<16|uint32_t(b[4*i+2])<<8|uint32_t(b[4*i+3]);
        for(int i=16;i<64;i++)w[i]=w[i-16]+(rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3))+w[i-7]+(rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10));
        std::copy(h,h+8,v);
        for(int i=0;i<64;i++){uint32_t t1=v[7]+(rotr(v[4],6)^rotr(v[4],11)^rotr(v[4],25))+((v[4]&v[5])^(~v[4]&v[6]))+k[i]+w[i],t2=(rotr(v[0],2)^rotr(v[0],13)^rotr(v[0],22))+((v[0]&v[1])^(v[0]&v[2])^(v[1]&v[2]));std::copy_backward(v,v+7,v+8);v[4]+=t1;v[0]=t1+t2;}
        for(int i=0;i<8;i++)h[i]+=v[i];};
    std::ifstream f(p,std::ios::binary);require(bool(f),"Cannot read native library");
    std::vector<unsigned char> buffer(1<<16);unsigned char pending[128];size_t used=0;uint64_t total=0;
    while(f){f.read(reinterpret_cast<char*>(buffer.data()),std::streamsize(buffer.size()));size_t n=size_t(f.gcount());total+=n;
        for(size_t i=0;i<n;){size_t take=std::min<size_t>(64-used,n-i);std::copy_n(buffer.data()+i,take,pending+used);used+=take;i+=take;if(used==64){block(pending);used=0;}}}
    require(f.eof(),"Cannot read native library");
    pending[used++]=0x80;size_t end=used<=56?64:128;std::fill(pending+used,pending+end,0);
    for(int i=0;i<8;i++)pending[end-1-i]=uint8_t((total*8)>>(8*i));
    block(pending);if(end==128)block(pending+64);
    std::string hex;const char* digits="0123456789abcdef";for(uint32_t word:h)for(int shift=28;shift>=0;shift-=4)hex+=digits[(word>>shift)&15];
    return hex;
}
std::string read_text(const fs::path& p){require(fs::is_regular_file(p)&&!fs::is_symlink(p)&&fs::file_size(p)<=1024*1024,"Missing or oversized JSON file: "+p.filename().string());std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
bool valid_option(const Option& o,const Value& v){
    if(o.type=="bool")return v.type==Value::Bool;
    if(o.type=="number")return v.type==Value::Number&&std::isfinite(v.number)&&v.number>=o.minimum&&v.number<=o.maximum;
    if(o.type=="string")return v.type==Value::String&&v.text.size()<=1024&&v.text.find('\0')==std::string::npos;
    if(o.type=="enum")return v.type==Value::String&&std::find(o.choices.begin(),o.choices.end(),v.text)!=o.choices.end();
    return false;
}
Manifest manifest(const fs::path& path){
    auto v=json::parse(read_text(path/"manifest.json"));require(v.type==Value::Object,"Manifest must be an object");
    require(v.get("format_version").type==Value::Number&&v.get("format_version").number==1,"Unsupported manifest format");
    Manifest m;m.id=string_field(v,"id",false,64);require(id_ok(m.id)&&!manager::find(m.id),"Invalid or reserved mod ID");
    m.name=string_field(v,"name",false,128);require(!m.name.empty(),"Mod name is empty");
    m.version=string_field(v,"version",false,64);version(m.version);
    m.author=string_field(v,"author",true,256);m.description=string_field(v,"description",true);
    m.kind=string_field(v,"kind",false,32);require(m.kind=="native"||m.kind=="settings"||m.kind=="content"||m.kind=="cemu","Unsupported mod kind");
    if(string_field(v,"game_id",false,64)!=kGameId)m.problem="This package targets another game";
    auto minimum=string_field(v,"minimum_manager_version",true,64);
    if(!minimum.empty()&&version(minimum)>version(kManagerVersion))m.problem="Requires mod manager "+minimum;
    if(m.kind=="native") {
        require(v.get("abi_version").type==Value::Number&&v.get("abi_version").number==1,"Unsupported native mod ABI");
        const auto& binaries=v.get("binaries");require(binaries.type==Value::Object&&!binaries.object.empty(),"Native mod has no binaries");
        for(const auto& [platform,b]:binaries.object)require(b.type==Value::String&&archive::relative_path(b.text),"Invalid native library path");
        m.binary=binaries.get(platform_key()).string();
        if(m.binary.empty()&&m.problem.empty())m.problem="No binary for "+platform_key();
        else if(m.problem.empty()&&(!fs::is_regular_file(path/m.binary)||fs::is_symlink(path/m.binary)))m.problem="Native library is missing";
        if(m.problem.empty())m.fingerprint=sha256_file(path/m.binary);
    } else if(m.kind=="cemu") {
        auto folder=string_field(v,"cemu_dir",false,512);
        require(folder.empty()||archive::relative_path(folder),"Invalid Cemu directory");
        auto checked=path;for(const auto& part:fs::path(folder)){checked/=part;require(!fs::is_symlink(checked),"Cemu paths may not use symlinks");}
        m.graphics=std::make_shared<cemu::Pack>(cemu::parse(path/folder));
        auto schema=cemu::options(*m.graphics);if(v.get("options").type==Value::Null)v["options"]=schema;else require(v.get("options")==schema,"Cemu preset options do not match rules.txt");
    } else if(m.kind=="content") {
        auto folder=string_field(v,"content_dir",false,512);
        require(archive::relative_path(folder),"Invalid content directory");
        auto checked=path;for(const auto& part:fs::path(folder)){checked/=part;require(!fs::is_symlink(checked),"Content paths may not use symlinks");}
        m.files=content::index(path/folder);
    } else {
        const auto& settings=v.get("settings");require(settings.type==Value::Object&&!settings.object.empty(),"Settings mod has no settings");
        for(const auto& [id,b]:settings.object){require(manager::find(id)&&b.type==Value::Bool,"Unknown built-in setting: "+id);m.settings[id]=b.boolean;}
    }
    const auto& deps=v.get("dependencies");require(deps.type==Value::Null||deps.type==Value::Array,"Dependencies must be an array");
    for(const auto& dep:deps.array){Requirement r;r.id=string_field(dep,"id",false,80);r.version=string_field(dep,"minimum_version",true,64);if(r.version.empty())r.version="0.0.0";version(r.version);require(id_ok(r.id)||(r.id.starts_with("builtin:")&&manager::find(r.id.substr(8))),"Invalid dependency ID");m.dependencies.push_back(r);}
    const auto& conflicts=v.get("conflicts");require(conflicts.type==Value::Null||conflicts.type==Value::Array,"Conflicts must be an array");
    for(const auto& c:conflicts.array){require(c.type==Value::String&&(id_ok(c.text)||(c.text.starts_with("builtin:")&&manager::find(c.text.substr(8)))),"Invalid conflict ID");m.conflicts.push_back(c.text);}
    const auto& options=v.get("options");require(options.type==Value::Null||(options.type==Value::Array&&options.array.size()<=32),"Invalid options");std::set<std::string> option_ids;
    for(const auto& option:options.array){
        Option o;o.id=string_field(option,"id",false,64);require(id_ok(o.id)&&option_ids.insert(o.id).second,"Invalid or duplicate option ID");o.name=string_field(option,"name",false,128);o.description=string_field(option,"description",true);o.type=string_field(option,"type",false,32);o.default_value=option.get("default");
        if(o.type=="number"){const auto& lo=option.get("min");const auto& hi=option.get("max");const auto& step=option.get("step");require(lo.type==Value::Number&&hi.type==Value::Number&&lo.number<=hi.number,"Invalid numeric bounds");o.minimum=lo.number;o.maximum=hi.number;o.step=step.type==Value::Number?step.number:1;require(o.step>0&&std::isfinite(o.step),"Invalid numeric step");}
        if(o.type=="enum"){const auto& choices=option.get("choices");require(choices.type==Value::Array&&!choices.array.empty()&&choices.array.size()<=64,"Invalid enum choices");for(const auto& c:choices.array){require(c.type==Value::String&&c.text.size()<=128,"Invalid enum value");o.choices.push_back(c.text);}}
        require(valid_option(o,o.default_value),"Invalid default for "+o.id);o.value=o.default_value;m.options.push_back(o);
    }
    if(m.kind=="content")require(m.dependencies.empty()&&m.options.empty(),"Content packages do not support dependencies or runtime options yet");
    return m;
}
Value& profile(){return database["profiles"][database.get("active").string("Default")];}
bool wanted(const std::string& id){const auto& v=profile().get("enabled").get(id);return v.type==Value::Bool&&v.boolean;}
// Test aid: WWHD_TEST_TRUST_NATIVE_MODS=id[,id...] pre-confirms packages, only in isolated test runs
// (WWHD_NO_HOST_INPUT plus an explicit WWHD_MOD_MANAGER_DIR). Nothing is written to profiles.json.
bool test_trusted(const std::string& id){
    const char* list=std::getenv("WWHD_TEST_TRUST_NATIVE_MODS");if(!list||!std::getenv("WWHD_NO_HOST_INPUT")||!std::getenv("WWHD_MOD_MANAGER_DIR"))return false;
    for(std::string_view rest=list;!rest.empty();){auto comma=rest.find(',');if(rest.substr(0,comma)==id)return true;if(comma==std::string_view::npos)break;rest.remove_prefix(comma+1);}
    return false;
}
// Settings presets never ask; native code runs only after the player confirmed this exact library.
bool confirmed(const Record& r){const auto& m=r.manifest;return m.kind!="native"||test_trusted(m.id)||(!m.fingerprint.empty()&&database.get("native_trust").get(m.id).string()==m.fingerprint);}
const char* kUnconfirmed="Not loaded: it contains native code you have not confirmed. Enable it again to review.";
Value config(const Manifest& m){Value out;out.type=Value::Object;for(const auto& o:m.options){const auto& saved=profile().get("config").get(m.id).get(o.id);out[o.id]=valid_option(o,saved)?saved:o.default_value;}return out;}
void save(){if(!ready)return;fs::create_directories(root);auto tmp=root/"profiles.json.tmp";std::ofstream f(tmp,std::ios::binary|std::ios::trunc);f<<json::dump(database)<<'\n';f.close();require(bool(f)&&host::replace_file(tmp.string(),(root/"profiles.json").string()),"Cannot save mod profiles");}
void defaults(){database=Value{};database["format_version"]=1;database["active"]="Default";auto& p=profile();p["enabled"].type=Value::Object;for(const auto& e:manager::entries())p["builtins"][e.id]=e.enabled();p["builtin_options"]["direct-camera.speed"]=double(camera_speed());p["builtin_options"]["mouse-camera.sensitivity"]=double(mouse_sensitivity());}
void scan(){records.clear();if(!ready)return;fs::create_directories(root/"Mods");for(const auto& e:fs::directory_iterator(root/"Mods")){if(!e.is_directory()||e.is_symlink()||!id_ok(e.path().filename().string()))continue;Record r;r.path=e.path();try{r.manifest=manifest(e.path());require(r.manifest.id==e.path().filename(),"Folder and manifest IDs differ");}catch(const std::exception& ex){r.manifest.id=e.path().filename().string();r.manifest.name=r.manifest.id;r.manifest.problem=ex.what();}auto id=r.manifest.id;records.emplace(id,std::move(r));}}
std::vector<std::string> order(const std::set<std::string>& enabled){
    std::map<std::string,int> mark;std::vector<std::string> result;
    std::function<void(const std::string&)> visit=[&](const std::string& id){require(mark[id]!=1,"Dependency cycle at "+id);if(mark[id]==2)return;mark[id]=1;auto it=records.find(id);require(it!=records.end(),"Missing dependency: "+id);const auto& m=it->second.manifest;require(m.problem.empty(),m.name+": "+m.problem);for(const auto& dep:m.dependencies){if(dep.id.starts_with("builtin:")){auto* e=manager::find(dep.id.substr(8));require(e&&version("1.0.0")>=version(dep.version),"Built-in dependency version is unavailable");continue;}auto d=records.find(dep.id);require(d!=records.end(),"Missing dependency: "+dep.id);require(version(d->second.manifest.version)>=version(dep.version),"Dependency "+dep.id+" needs version "+dep.version);require(enabled.contains(dep.id),"Dependency is disabled: "+dep.id);visit(dep.id);}mark[id]=2;result.push_back(id);};
    for(const auto& id:enabled)visit(id);return result;
}
std::set<std::string> enabled_set(){std::set<std::string> result;for(const auto& [id,r]:records)if(wanted(id))result.insert(id);return result;}
void validate_conflicts(const std::set<std::string>& enabled,const Value* planned=nullptr){
    auto builtin_on=[&](const std::string& id){return planned?planned->get(id).boolean:manager::find(id)->enabled();};
    std::map<std::string,std::string> setting_owners,file_owners;
    std::vector<cemu::Selection> graphics;
    for(const auto& id:enabled){const auto& m=records.at(id).manifest;if(m.graphics)graphics.push_back({id,*m.graphics,config(m)});}
    cemu::validate(graphics);
    for(const auto& id:enabled){const auto& m=records.at(id).manifest;for(const auto& [file,path]:m.files){auto [it,inserted]=file_owners.emplace(file,id);require(inserted,"Content file conflict: "+file+" between "+id+" and "+it->second);}for(const auto& dep:m.dependencies)if(dep.id.starts_with("builtin:"))require(builtin_on(dep.id.substr(8)),"Built-in dependency is disabled: "+dep.id);for(const auto& conflict:m.conflicts){bool on=conflict.starts_with("builtin:")?builtin_on(conflict.substr(8)):enabled.contains(conflict);require(!on,m.name+" conflicts with "+conflict);}for(const auto& [setting,on]:m.settings){auto [it,inserted]=setting_owners.emplace(setting,id);require(inserted,m.name+" overlaps a setting from "+it->second);}}
}
template<class Fn> bool operation(std::string& error,Fn fn){try{std::lock_guard guard(mutex);require(ready,"Mod manager storage is unavailable");fn();error.clear();return true;}catch(const std::exception& e){error=e.what();return false;}}
struct Context {std::string id,path,status;Value config;WWHDModHostV1 host{};};
struct Live {std::unique_ptr<Context> context;WWHDModV1 api{};void* library=nullptr;bool initialized=false;std::map<std::string,bool> previous;std::string kind;};
std::map<std::string,Live> live; // game thread exclusively
std::vector<std::string> live_order;
const Value& option(void* c,const char* id){return static_cast<Context*>(c)->config.get(id?id:"");}
void unload(Live& item){if(item.initialized&&item.api.on_unload)item.api.on_unload(item.api.instance);for(const auto& [id,on]:item.previous)if(const auto* e=manager::find(id))e->apply(on);if(item.library){
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(item.library));
#elif !defined(__SWITCH__)
    dlclose(item.library);
#endif
}item.library=nullptr;}
void load(Live& item,const Record& record,const Value& configuration){
    item.kind=record.manifest.kind;item.context=std::make_unique<Context>();auto& c=*item.context;c.id=record.manifest.id;c.path=record.path.string();c.config=configuration;
    if(item.kind=="settings"){for(const auto& [id,on]:record.manifest.settings){auto* e=manager::find(id);item.previous[id]=e->enabled();
        if(e->restart_required){std::lock_guard guard(mutex);const auto& saved=profile().get("builtins").get(id);if(saved.type==Value::Bool)item.previous[id]=saved.boolean;}
        e->apply(on);}return;}
    auto path=record.path/record.manifest.binary;
#ifdef _WIN32
    item.library=LoadLibraryW(path.wstring().c_str());require(item.library,"Cannot load native mod library");auto init=reinterpret_cast<WWHDModInitV1>(GetProcAddress(static_cast<HMODULE>(item.library),"wwhd_mod_init_v1"));
#elif defined(__SWITCH__)
    // homebrew cannot load native code at run time: settings and content mods only
    WWHDModInitV1 init=nullptr;(void)path;
#else
    item.library=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);if(!item.library){const char* reason=dlerror();throw std::runtime_error(reason?reason:"Cannot load native library");}auto init=reinterpret_cast<WWHDModInitV1>(dlsym(item.library,"wwhd_mod_init_v1"));
#endif
    require(init,"Native library has no wwhd_mod_init_v1 entry point");
    c.host={sizeof(WWHDModHostV1),1,&c,kGameId,c.path.c_str(),
        [](void*,uint32_t a,void* b,size_t n){return read_memory?read_memory(a,b,n):0;},
        [](void*,uint32_t a,const void* b,size_t n){return write_memory?write_memory(a,b,n):0;},
        [](void* p,const char* s){fprintf(stderr,"[mod:%s] %s\n",static_cast<Context*>(p)->id.c_str(),s?s:"");},
        [](void* p,const char* s){auto* ctx=static_cast<Context*>(p);ctx->status=s?std::string(s).substr(0,1024):"";},
        [](void* p,const char* id){return option(p,id).text.c_str();},
        [](void* p,const char* id){return option(p,id).number;},
        [](void* p,const char* id){return int(option(p,id).boolean);}};
    item.api.size=sizeof(WWHDModV1);item.api.abi_version=1;
    require(init(&c.host,&item.api)!=0,"Native mod initialization failed");require(item.api.size>=sizeof(WWHDModV1)&&item.api.abi_version==1,"Native library ABI does not match");item.initialized=true;
}
}

std::string platform_key(){
#ifdef _WIN32
    std::string key="windows";
#elif defined(__APPLE__)
    std::string key="macos";
#elif defined(__ANDROID__)
    std::string key="android";
#else
    std::string key="linux";
#endif
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    return key+"-arm64";
#else
    return key+"-x86_64";
#endif
}
void initialize(){
    std::lock_guard guard(mutex);if(ready)return;const char* override=std::getenv("WWHD_MOD_MANAGER_DIR");if(std::getenv("WWHD_NO_HOST_INPUT")&&!override)return;
    root=override?fs::path(override):fs::path(host::config_dir())/"ModManager";ready=true;defaults();
    try{fs::create_directories(root);if(fs::exists(root/"profiles.json")){auto saved=json::parse(read_text(root/"profiles.json"));require(saved.get("format_version").type==Value::Number&&saved.get("format_version").number==1,"Unsupported profile format");require(saved.get("profiles").type==Value::Object&&!saved.get("profiles").object.empty(),"Invalid profiles");require(saved.get("active").type==Value::String&&saved.get("profiles").object.contains(saved.get("active").text),"Invalid active profile");database=std::move(saved);}scan();
        for(const auto& e:manager::entries()){const auto& v=profile().get("builtins").get(e.id);if(!std::getenv(e.startup_env)&&v.type==Value::Bool)e.apply(v.boolean);}
        const auto& options=profile().get("builtin_options");auto speed=options.get("direct-camera.speed"),sens=options.get("mouse-camera.sensitivity");if(!std::getenv("WWHD_MOD_CAMERA_SPEED")&&speed.type==Value::Number&&speed.number>=.5&&speed.number<=2)set_camera_speed(speed.number);if(!std::getenv("WWHD_MOD_MOUSE_SENS")&&sens.type==Value::Number&&sens.number>=.08&&sens.number<=.3)set_mouse_sensitivity(sens.number);
        // Next-launch (restart_required) settings must be selected before the game starts.
        try { auto enabled=enabled_set();auto sequence=order(enabled);validate_conflicts(enabled);
            content::Files files;
            for(const auto& id:sequence){auto& record=records.at(id);if(record.manifest.kind=="content"){
                files.insert(record.manifest.files.begin(),record.manifest.files.end());record.active=true;
                record.status=std::to_string(record.manifest.files.size())+" replacement files";
            }}
            std::vector<cemu::Selection> graphics;
            for(const auto& id:sequence){auto& record=records.at(id);if(record.manifest.graphics&&(record.manifest.graphics->shaders.empty()||cemu::vulkan())){
                record.startup_config=config(record.manifest);graphics.push_back({id,*record.manifest.graphics,record.startup_config});
                record.active=true;record.status=std::to_string(record.manifest.graphics->textures.size())+" texture rules, "+std::to_string(record.manifest.graphics->shaders.size())+" shader candidates";
            }}
            cemu::activate(graphics);
            content::activate(std::move(files));
            for(const auto& id:sequence)for(const auto& [setting,on]:records.at(id).manifest.settings) {
            const auto* entry=manager::find(setting);
            if(entry->restart_required&&!std::getenv(entry->startup_env))entry->apply(on);
        }} catch(const std::exception& e) {last_problem=e.what();}
        dirty=true;
    }catch(const std::exception& e){last_problem=e.what();ready=false;records.clear();fprintf(stderr,"[mod-manager] %s\n",e.what());}
}
std::string directory(){std::lock_guard guard(mutex);return ready?(root/"Mods").string():"";}
std::vector<View> list(){std::lock_guard guard(mutex);std::vector<View> out;for(const auto& [id,r]:records){const auto& m=r.manifest;View v;v.id=id;v.name=m.name;v.version=m.version;v.author=m.author;v.description=m.description;v.kind=m.kind;v.restart_required=m.kind=="content"||m.kind=="cemu";v.enabled=wanted(id);v.active=r.active;v.compatible=m.problem.empty();v.native_confirmed=confirmed(r);v.reason=m.problem.empty()?r.error:m.problem;v.status=r.status;if(m.graphics){auto diagnostics=cemu::runtime_status(id);if(!diagnostics.empty())v.status+=". "+diagnostics;}v.options=m.options;auto cfg=config(m);v.pending_restart=v.restart_required&&(v.enabled!=v.active||(m.graphics&&v.active&&!(r.startup_config==cfg)));if(m.graphics&&!m.graphics->shaders.empty()&&!cemu::vulkan()){v.active=false;v.compatible=false;v.reason="GLSL shader packs require Vulkan; choose it in Graphics and restart";}for(auto& o:v.options)o.value=cfg.get(o.id);for(const auto& dep:m.dependencies)v.dependencies.push_back(dep.id+">="+dep.version);v.conflicts=m.conflicts;out.push_back(std::move(v));}return out;}
bool install(const std::string& source,std::string& error){return operation(error,[&]{
    auto nonce=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());auto stage=root/(".stage-"+nonce),backup=root/(".backup-"+nonce);fs::path target;bool backed=false,moved=false;
    try{if(fs::is_regular_file(source)&&content::known_pack(fs::path(source).filename().string())){
        require(!fs::is_symlink(source)&&fs::file_size(source)<=128ull*1024*1024,"Invalid or oversized replacement pack");
        std::ifstream input(source,std::ios::binary);char magic[4]{};input.read(magic,4);require(std::string(magic,4)=="SARC"||std::string(magic,4)=="Yaz0","Replacement pack is not a SARC/Yaz0 archive");
        fs::create_directories(stage);fs::copy_file(source,stage/fs::path(source).filename());
    }else archive::stage(fs::path(source),stage);if(!fs::exists(stage/"manifest.json")){
        bool graphics=false;for(const auto& file:fs::recursive_directory_iterator(stage))if(file.is_regular_file()){
            auto name=file.path().filename().string();if(name.ends_with("_vs.txt")||name.ends_with("_ps.txt"))graphics=true;
            if(name=="rules.txt"){auto text=read_text(file.path());for(char& c:text)if(c>='A'&&c<='Z')c+='a'-'A';if(text.find("[preset]")!=std::string::npos||text.find("[textureredefine]")!=std::string::npos||text.find("[default]")!=std::string::npos)graphics=true;}
        }
        if(graphics)cemu::import_legacy(stage,fs::path(source).filename().string());else content::import_legacy(stage,fs::path(source).filename().string());
    }auto m=manifest(stage);require(m.problem.empty()||m.problem.starts_with("No binary for")||m.problem.starts_with("GLSL shader packs require Vulkan"),m.problem);target=root/"Mods"/m.id;auto it=records.find(m.id);require(it==records.end()||(!wanted(m.id)&&!it->second.active&&!it->second.loading),it!=records.end()&&(it->second.manifest.kind=="content"||it->second.manifest.kind=="cemu")?"Disable this content mod and restart before updating":"Disable this mod and wait for it to unload before updating");fs::create_directories(target.parent_path());if(fs::exists(target)){fs::rename(target,backup);backed=true;}fs::rename(stage,target);moved=true;m=manifest(target);Record r;r.path=target;r.manifest=std::move(m);auto installed_id=r.manifest.id;records[installed_id]=std::move(r);if(backed){std::error_code cleanup;fs::remove_all(backup,cleanup);}dirty=true;
    }catch(...){if(moved)fs::remove_all(target);if(backed)fs::rename(backup,target);if(fs::exists(stage))fs::remove_all(stage);throw;}
});}
bool remove(const std::string& id,std::string& error){return operation(error,[&]{auto it=records.find(id);require(it!=records.end(),"Mod not found");require(!wanted(id)&&!it->second.active&&!it->second.loading,(it->second.manifest.kind=="content"||it->second.manifest.kind=="cemu")?"Disable this content mod and restart before removing":"Disable this mod and wait for it to unload before removing");for(const auto& [other,r]:records)if(wanted(other))for(const auto& d:r.manifest.dependencies)require(d.id!=id,r.manifest.name+" depends on this mod");auto previous=database;for(auto& [name,p]:database["profiles"].object){p["enabled"].object.erase(id);p["config"].object.erase(id);}database["native_trust"].object.erase(id);try{save();}catch(...){database=previous;throw;}fs::remove_all(it->second.path);records.erase(it);dirty=true;});}
bool enable(const std::string& id,bool on,std::string& error){std::vector<std::string> builtin_dependencies;bool ok=operation(error,[&]{require(records.contains(id),"Mod not found");auto enabled=enabled_set();if(on){
    std::set<std::string> visiting;
    std::function<void(const std::string&)> add=[&](const std::string& current){require(!visiting.contains(current),"Dependency cycle at "+current);require(records.contains(current),"Missing dependency: "+current);const auto& graphics=records.at(current).manifest.graphics;if(graphics&&!graphics->shaders.empty())require(cemu::vulkan(),"GLSL shader packs require Vulkan; choose it in Graphics and restart");if(enabled.contains(current))return;visiting.insert(current);for(const auto& d:records.at(current).manifest.dependencies){if(d.id.starts_with("builtin:"))builtin_dependencies.push_back(d.id.substr(8));else add(d.id);}visiting.erase(current);require(confirmed(records.at(current)),records.at(current).manifest.name+" contains native code that has not been confirmed");enabled.insert(current);};add(id);
    }else enabled.erase(id);order(enabled);auto planned=profile().get("builtins");for(const auto& entry:manager::entries())planned[entry.id]=entry.enabled();for(const auto& dependency:builtin_dependencies)planned[dependency]=true;validate_conflicts(enabled,&planned);auto previous=database;profile()["builtins"]=planned;for(const auto& [key,r]:records)profile()["enabled"][key]=enabled.contains(key);try{save();}catch(...){database=previous;throw;}for(const auto& key:enabled)records.at(key).error.clear();dirty=true;});if(ok)for(const auto& key:builtin_dependencies)manager::set_enabled(key,true);return ok;}
std::vector<std::pair<std::string,std::string>> unconfirmed_native(const std::string& id){
    std::lock_guard guard(mutex);std::vector<std::pair<std::string,std::string>> result;if(!ready)return result;
    auto enabled=enabled_set();std::set<std::string> seen;
    // Same walk as enable(); missing dependencies and cycles are left for enable() to report.
    std::function<void(const std::string&)> visit=[&](const std::string& current){auto it=records.find(current);if(it==records.end()||enabled.contains(current)||!seen.insert(current).second)return;for(const auto& d:it->second.manifest.dependencies)if(!d.id.starts_with("builtin:"))visit(d.id);if(!confirmed(it->second))result.emplace_back(current,it->second.manifest.name);};
    visit(id);return result;
}
bool confirm_native(const std::string& id,std::string& error){return operation(error,[&]{auto it=records.find(id);require(it!=records.end(),"Mod not found");const auto& m=it->second.manifest;require(m.kind=="native"&&m.problem.empty()&&!m.fingerprint.empty(),"This package has no loadable native code");auto previous=database;database["native_trust"][id]=m.fingerprint;try{save();}catch(...){database=previous;throw;}});}
bool configure(const std::string& id,const std::string& option_id,const Value& value,std::string& error){return operation(error,[&]{require(records.contains(id),"Mod not found");auto& opts=records.at(id).manifest.options;auto it=std::find_if(opts.begin(),opts.end(),[&](const Option& o){return o.id==option_id;});require(it!=opts.end()&&valid_option(*it,value),"Invalid configuration value");auto previous=database;profile()["config"][id][option_id]=value;try{if(records.at(id).manifest.graphics)cemu::validate({{id,*records.at(id).manifest.graphics,config(records.at(id).manifest)}});if(wanted(id))validate_conflicts(enabled_set());save();}catch(...){database=previous;throw;}dirty=true;});}
void disable_all(){std::string ignored;operation(ignored,[&]{auto previous=database;for(const auto& [id,r]:records)profile()["enabled"][id]=false;try{save();}catch(...){database=previous;throw;}dirty=true;});manager::disable_all();}
std::vector<std::string> profiles(){std::lock_guard guard(mutex);std::vector<std::string> result;if(ready)for(const auto& [name,p]:database.get("profiles").object)result.push_back(name);return result;}
std::string current_profile(){std::lock_guard guard(mutex);return ready?database.get("active").string():"Default";}
bool create_profile(const std::string& name,std::string& error){return operation(error,[&]{require(!name.empty()&&name.size()<=64&&name.find('\0')==std::string::npos,"Invalid profile name");require(database.get("profiles").object.size()<64,"Profile limit reached");require(!database.get("profiles").object.contains(name),"Profile already exists");auto previous=database;Value copy=profile();for(const auto& e:manager::entries())copy["builtins"][e.id]=e.enabled();copy["builtin_options"]["direct-camera.speed"]=double(camera_speed());copy["builtin_options"]["mouse-camera.sensitivity"]=double(mouse_sensitivity());database["profiles"][name]=std::move(copy);try{save();}catch(...){database=previous;throw;}});}
bool select_profile(const std::string& name,std::string& error){Value chosen;bool ok=operation(error,[&]{require(database.get("profiles").object.contains(name),"Profile not found");auto previous=database;database["active"]=name;try{order(enabled_set());validate_conflicts(enabled_set(),&profile().get("builtins"));save();}catch(...){database=previous;throw;}chosen=profile();profile_changed=true;dirty=true;});if(ok){for(const auto& e:manager::entries()){const auto& v=chosen.get("builtins").get(e.id);manager::set_enabled(e.id,v.type==Value::Bool&&v.boolean);}auto speed=chosen.get("builtin_options").get("direct-camera.speed"),sens=chosen.get("builtin_options").get("mouse-camera.sensitivity");if(speed.type==Value::Number&&speed.number>=.5&&speed.number<=2)set_camera_speed(speed.number);if(sens.type==Value::Number&&sens.number>=.08&&sens.number<=.3)set_mouse_sensitivity(sens.number);}return ok;}
bool delete_profile(const std::string& name,std::string& error){return operation(error,[&]{require(name!=database.get("active").string(),"Switch profiles before deleting the active one");require(database.get("profiles").object.contains(name),"Profile not found");auto previous=database;database["profiles"].object.erase(name);try{save();}catch(...){database=previous;throw;}});}
void remember_builtin(const std::string& id,bool on){std::string error;operation(error,[&]{profile()["builtins"][id]=on;save();dirty=true;});}
void remember_option(const std::string& id,double value){std::string error;operation(error,[&]{profile()["builtin_options"][id]=value;save();});}
bool refresh(std::string& error){return operation(error,[&]{for(const auto& [id,r]:records)require(!r.active&&!r.loading&&!wanted(id),(r.manifest.kind=="content"||r.manifest.kind=="cemu")?"Disable content mods and restart before rescanning":"Disable installed mods before rescanning");scan();dirty=true;});}
void set_memory_access(ReadMemory read,WriteMemory write){read_memory=read;write_memory=write;}
void frame(uint64_t step){
    if(!dirty.load(std::memory_order_relaxed)&&!running.load(std::memory_order_relaxed))return;
    static uint64_t previous=~uint64_t(0);if(step==previous)return;previous=step;
    if(dirty.exchange(false)){
        std::map<std::string,std::pair<Record,Value>> desired;std::vector<std::string> sequence;std::set<std::string> unconfirmed;Value baseline;bool switching=false;
        {std::lock_guard guard(mutex);switching=profile_changed.exchange(false);baseline=profile().get("builtins");try{auto enabled=enabled_set();sequence=order(enabled);validate_conflicts(enabled);std::erase_if(sequence,[](const auto& id){return records.at(id).manifest.kind=="content"||records.at(id).manifest.kind=="cemu";});for(const auto& id:sequence){desired[id]={records.at(id),config(records.at(id).manifest)};records.at(id).loading=true;if(!confirmed(records.at(id)))unconfirmed.insert(id);}}catch(const std::exception& e){last_problem=e.what();for(auto& [id,r]:records)if(wanted(id))r.error=e.what();sequence.clear();desired.clear();}}
        for(auto it=live_order.rbegin();it!=live_order.rend();++it)if(!desired.contains(*it)||(switching&&live.at(*it).kind=="settings")){auto found=live.find(*it);if(found!=live.end()){unload(found->second);live.erase(found);}std::lock_guard guard(mutex);if(records.contains(*it)){records.at(*it).active=false;records.at(*it).status.clear();}}
        if(switching)for(const auto& entry:manager::entries()){const auto& value=baseline.get(entry.id);entry.apply(value.type==Value::Bool&&value.boolean);}
        live_order.clear();
        for(const auto& id:sequence){const auto& [record,cfg]=desired.at(id);bool deps_ok=true;for(const auto& dep:record.manifest.dependencies)if(!dep.id.starts_with("builtin:")&&!live.contains(dep.id)){std::lock_guard guard(mutex);if(!records.at(dep.id).active)deps_ok=false;}
            if(!deps_ok){std::lock_guard guard(mutex);records.at(id).loading=false;records.at(id).error="A dependency is unavailable (content dependencies may require restart)";continue;}
            auto it=live.find(id);
            // Profile switches, older profiles and updated libraries can name native code the player never
            // confirmed: it stays unloaded and disabled with a note, like a failed load; the checkbox asks.
            if(it==live.end()&&unconfirmed.contains(id)){fprintf(stderr,"[mod-manager] %s not loaded: native code not confirmed\n",id.c_str());std::lock_guard guard(mutex);records.at(id).loading=false;records.at(id).error=kUnconfirmed;profile()["enabled"][id]=false;try{save();}catch(...){}continue;}
            if(it==live.end()){Live item;try{load(item,record,cfg);live.emplace(id,std::move(item));fprintf(stderr,"[mod-manager] loaded %s (%s)\n",id.c_str(),record.manifest.kind.c_str());}catch(const std::exception& e){unload(item);std::lock_guard guard(mutex);records.at(id).loading=false;records.at(id).error=e.what();profile()["enabled"][id]=false;try{save();}catch(...){}continue;}}
            else if(!(it->second.context->config==cfg)){it->second.context->config=cfg;if(it->second.api.on_config_changed)it->second.api.on_config_changed(it->second.api.instance);}
            live_order.push_back(id);std::lock_guard guard(mutex);records.at(id).active=true;records.at(id).loading=false;
        }
        running=!live.empty();
    }
    for(const auto& id:live_order){auto& item=live.at(id);if(item.api.on_frame)item.api.on_frame(item.api.instance,step);if(item.context){std::lock_guard guard(mutex);if(records.contains(id))records.at(id).status=item.context->status;}}
}
}
