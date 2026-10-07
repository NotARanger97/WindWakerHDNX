// CPU-only regression: compile with the vendored Cemu GLSL sources/glslang.
// Include the implementation to exercise the private binary decoder and replay
// guards without adding test entry points to the renderer's public API.
#include "gfx/vulkan/shaders.cpp"
#undef HLE
#define HLE(lib, name) static void test_##lib##_##name(Cpu* c)
#include "gx2/gx2_resources.cpp"
#include "Cafe/HW/Latte/ISA/LatteInstructions.h"
#include <fstream>
#include <random>
#include <stdexcept>

std::unique_ptr<Renderer> g_renderer;
void cemu_shim_log(const std::string& msg) { fprintf(stderr,"%s\n",msg.c_str()); }
namespace gfxvk { void reset_draw_state_cache() {} }
namespace mem {
uint32_t host_alloc(uint32_t size, uint32_t align) {
    static uint32_t next=kHostStart;
    next=(next+align-1)&~(align-1);
    uint32_t address=next; next+=size;
    if(next>=kFixedStart) throw std::runtime_error("test scratch exhausted");
    memset(ptr(address),0,size);
    return address;
}
}
#ifdef __SWITCH__
// Exercise the actual watched-page/memo branches on the desktop test host.
extern "C" { uint8_t* const g_ppc_mem_base=reinterpret_cast<uint8_t*>(0x200000000000ull); }
#endif

static void require(bool value, const char* message) {
    if(!value) throw std::runtime_error(message);
}
using namespace gfxvk::vk;
static void state_roundtrip() {
    std::mt19937 random(42);
    for(bool vertex:{false,true}) for(bool streamout:{false,true}) for(int round=0;round<100;++round) {
        std::vector<uint32_t> regs(gx2::kNumRegs), replay(gx2::kNumRegs);
        for(auto& word:regs) word=random();
        regs[mmVGT_STRMOUT_EN]=streamout;
        TranslationState words{}, again{};
        size_t count=translation_state<false>(regs.data(),words,vertex);
        require(count==words.size()-(streamout ? 0 : 4),"wrong state word count");
        translation_state<true>(replay.data(),words,vertex);
        require(count==translation_state<false>(replay.data(),again,vertex) && words==again,
                "state masks/order did not round-trip");
        require(state_hash(regs.data(),123,vertex,false)==state_hash(replay.data(),123,vertex,false),
                "restored state changed hash");
        TranslationAnnotations annotations{}, copy{};
        translation_annotations<false>(regs.data(),annotations,vertex);
        translation_annotations<true>(replay.data(),annotations,vertex);
        translation_annotations<false>(replay.data(),copy,vertex);
        require(copy==annotations,"annotation registers did not round-trip");
    }
}
static void install_program(std::vector<uint32_t>& regs, bool vertex) {
    uint32_t address=mem::host_alloc(8,256), start=vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS;
    uint32_t code[]={vertex ? (1u<<13)|60 : 0,
        (0x28u<<23)|(1u<<21)|4|(4u<<3)|(4u<<6)|(5u<<9)}; // EXPORT_DONE constant (0,0,0,1)
    memcpy(ppc_ptr(address),code,sizeof(code));
    regs[start]=address>>8; regs[start+1]=1;
}
static void input_bounds() {
    auto bytes=[](std::initializer_list<uint32_t> words) {
        std::vector<uint8_t> result(words.size()*4);
        memcpy(result.data(),words.begin(),result.size()); return result;
    };
    require(!program_inputs_complete(bytes({2,1u<<23})),"TEX outside program accepted");
    require(!program_inputs_complete(bytes({2,0x12u<<23})),"CALL outside program accepted");
    auto alu=bytes({2,8u<<26,0,(0x28u<<23)|(1u<<21),0x80000000u|253u|(2u<<10),0,0,0,0,0});
    require(program_inputs_complete(alu),"in-range ALU literals rejected");
    alu.resize(24);
    require(!program_inputs_complete(alu),"ALU literals outside program accepted");
    require(!fetch_inputs_complete(bytes({2,0x01800000,0,0})),"fetch clause outside program accepted");
    require(fetch_inputs_complete(bytes({2,0x01800000,0,0x0A000000,1,0,0,0})),
            "in-range raw fetch clause rejected");
    // Legacy unknown words advance by one: a trailing semantic needs four words.
    require(!fetch_inputs_complete(bytes({0,0,0,1})),"truncated legacy attribute accepted");
}
static void install_fetch(std::vector<uint32_t>& regs, bool compact) {
    uint32_t address=mem::host_alloc(32,256);
    if(compact) {
        st32(address,0x57574653); st32(address+4,1);
        st32(address+16,0); st32(address+20,0);
        st32(address+24,2); st32(address+28,0x00010203);
        regs[mmSQ_PGM_START_FS+1]=4;
    } else {
        LatteClauseInstruction_VTX instruction{};
        instruction.setField_VTX_INST(LatteClauseInstruction_VTX::VTX_INST::_VTX_INST_SEMANTIC);
        instruction.setField_BUFFER_ID(0xA0);
        instruction.setField_FETCH_TYPE(LatteConst::VertexFetchType2::INSTANCE_DATA);
        instruction.setField_SRC_SEL_X(LatteClauseInstruction_VTX::SRC_SEL::SEL_Y);
        instruction.setField_DATA_FORMAT(Latte::E_HWFMT::HWFMT_32);
        memcpy(ppc_ptr(address),&instruction,16);
        regs[mmSQ_PGM_START_FS+1]=2;
    }
    regs[mmSQ_PGM_START_FS]=address>>8;
    regs[Latte::REGADDR::VGT_INSTANCE_STEP_RATE_0]=2;
    regs[Latte::REGADDR::VGT_INSTANCE_STEP_RATE_1]=3;
}
static Shader* lookup(std::vector<uint32_t>& regs, bool vertex, uint64_t frame=~uint64_t{0}) {
    uint64_t fsKey=0;
    auto* fetch=vertex ? get_fetch_shader(regs.data(),&fsKey,frame) : nullptr;
    auto* shader=translate(regs.data(),vertex,fetch,fsKey,frame);
    require(shader && shader->ready(),shader ? shader->error.c_str() : "null shader");
    return shader;
}
static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(path,std::ios::binary);
    return {(std::istreambuf_iterator<char>(file)),{}};
}
static void replace_word(std::vector<uint8_t>& file, size_t offset, uint64_t value, size_t bytes) {
    for(size_t i=0;i<bytes;++i) file[offset+i]=uint8_t(value>>(i*8));
}
static void update_checksum(std::vector<uint8_t>& file) {
    replace_word(file,24,cache_checksum(file.data()+40,file.size()-40),8);
}
int main(int argc, char** argv) {
    try {
        require(argc==2,"pass a disposable cache directory via argv and WWHD_VK_SHADER_CACHE");
        require(getenv("WWHD_VK_SHADER_CACHE") && std::string(getenv("WWHD_VK_SHADER_CACHE"))==argv[1],
                "test cache directory must match WWHD_VK_SHADER_CACHE");
        std::filesystem::remove(std::string(argv[1])+"/spirv.bin");
#ifdef _WIN32
        require(VirtualAlloc(PPC_MEM_BASE,0x100000000ull,MEM_RESERVE,PAGE_READWRITE)==PPC_MEM_BASE,"guest reserve");
        require(VirtualAlloc(mem::ptr(mem::kHostStart),mem::kFixedStart-mem::kHostStart,
                             MEM_COMMIT,PAGE_READWRITE)!=nullptr,"guest scratch commit");
#else
        require(mmap(PPC_MEM_BASE,0x100000000ull,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0)==PPC_MEM_BASE,
                "guest reserve");
#endif
        select_renderer(); state_roundtrip(); input_bounds();
        std::vector<std::pair<std::vector<uint32_t>,bool>> recipes;
        for(int kind=0;kind<3;++kind) {
            bool vertex=kind!=0;
            std::vector<uint32_t> regs(gx2::kNumRegs);
            regs[Latte::REGADDR::VGT_PRIMITIVE_TYPE]=4;
            regs[Latte::REGADDR::PA_CL_VTE_CNTL]=0x3F;
            regs[mmCB_SHADER_MASK]=regs[Latte::REGADDR::CB_TARGET_MASK]=15;
            regs[mmCB_SHADER_CONTROL]=1;
            std::fill_n(regs.data()+mmSQ_VTX_SEMANTIC_0,32,0xFF);
            regs[mmSQ_VTX_SEMANTIC_0]=0;
            install_program(regs,vertex);
            if(vertex) install_fetch(regs,kind==1);
            lookup(regs,vertex); recipes.emplace_back(regs,vertex);
            // Different key, same emitted source: both must survive persistence.
            regs[mmSQ_VTX_SEMANTIC_0+31]=17;
            lookup(regs,vertex); recipes.emplace_back(regs,vertex);
            if(kind==1) {
                auto points=regs;
                points[Latte::REGADDR::VGT_PRIMITIVE_TYPE]=1;
                require(lookup(points,true)->uniforms.offset_pointSize>=0,"point-size uniform not generated");
                recipes.emplace_back(points,true);
            }
            if(kind==2) {
                regs[Latte::REGADDR::VGT_INSTANCE_STEP_RATE_0]=7;
                lookup(regs,vertex); recipes.emplace_back(regs,vertex);
            }
        }
        require(diskVariants.size()==recipes.size(),"missing variant recipes sharing GLSL");
        std::vector<Shader> expected;
        for(auto& [regs,vertex]:recipes) { expected.push_back(*lookup(regs,vertex)); expected.back().dec=nullptr; }
        require(save_shader_cache(),"save failed");
        auto file=read_file(std::string(argv[1])+"/spirv.bin");
        require(file.size()==diskBytes,"disk byte accounting differs from serialization");
        std::unordered_map<uint64_t,DiskShader> decoded;
        require(decode_disk_cache(file,decoded),"cache failed to round-trip");
        auto damaged=file; damaged.back()^=1;
        require(!decode_disk_cache(damaged,decoded),"bad checksum accepted");
        damaged=file; damaged[7]='2';
        require(!decode_disk_cache(damaged,decoded),"old SC02 accepted");
        damaged=file; damaged.pop_back();
        require(!decode_disk_cache(damaged,decoded),"truncated cache accepted");
        size_t pos=40; uint64_t value,sourceBytes,spirvWords,variants;
        for(;;) {
            require(cache_take(file,pos,value,8) && cache_take(file,pos,value,4) &&
                    cache_take(file,pos,sourceBytes,4) && cache_take(file,pos,spirvWords,4) &&
                    cache_take(file,pos,variants,4),"test record header");
            if(variants>=2) break;
            pos+=sourceBytes+spirvWords*4;
            for(uint64_t i=0;i<variants;++i) {
                uint64_t programBytes,fetchBytes;
                require(cache_take(file,pos,value,8) && cache_take(file,pos,programBytes,4) &&
                        cache_take(file,pos,fetchBytes,4),"test input bounds");
                pos+=12+sizeof(TranslationState)+sizeof(TranslationAnnotations)+programBytes+fetchBytes;
            }
        }
        size_t firstInput=pos+sourceBytes+spirvWords*4;
        damaged=file; replace_word(damaged,firstInput+8,maxProgramBytes+8,4); update_checksum(damaged);
        require(!decode_disk_cache(damaged,decoded),"oversized program accepted with valid checksum");
        damaged=file; replace_word(damaged,pos-4,maxCacheEntries+1,4); update_checksum(damaged);
        require(!decode_disk_cache(damaged,decoded),"oversized variant count accepted");
        size_t cursor=firstInput; uint64_t firstKey,programBytes,fetchBytes;
        require(cache_take(file,cursor,firstKey,8) && cache_take(file,cursor,programBytes,4) &&
                cache_take(file,cursor,fetchBytes,4),"test input header");
        size_t secondInput=firstInput+28+sizeof(TranslationState)+sizeof(TranslationAnnotations)+programBytes+fetchBytes;
        damaged=file; replace_word(damaged,secondInput,firstKey,8); update_checksum(damaged);
        require(!decode_disk_cache(damaged,decoded),"duplicate translation key accepted");

        clear_shader_cache();
        auto& [regs,vertex]=recipes.front();
        // Replay the pixel recipe with its true key, but altered expected source.
        for(const auto& [key,pixel]:decoded) if(!pixel.vertex) {
            require(!translate_impl(regs.data(),false,nullptr,0,~uint64_t{0},~uint64_t{0},&pixel,0) &&
                    shaders.empty(),"key mismatch inserted a shader");
            auto mismatch=pixel; mismatch.source+="\n// mismatch";
            require(!translate_impl(regs.data(),false,nullptr,0,~uint64_t{0},~uint64_t{0},
                                    &mismatch,pixel.variants.front().key) && shaders.empty(),
                    "source mismatch poisoned shader map");
            break;
        }
        diskShaders.clear(); diskVariants.clear(); diskInitialized=false; diskBytes=40;
        fetchShaders.clear(); // force both real fetch parsers to run during warm-up
        auto compiles=stats.spirvCompiles;
        warm_up_shader_cache();
        if(const char* enabled=getenv("WWHD_VK_SHADER_WARMUP"); enabled && !strcmp(enabled,"0")) {
            require(shaders.empty() && stats.spirvCompiles==compiles,"disabled warm-up translated shaders");
            require(stats.diskLoads!=0,"disabled warm-up failed to load disk SPIR-V");
            auto* shader=lookup(recipes.front().first,false);
            require(shader->ready() && stats.spirvCompiles==compiles,"disabled warm-up lost normal disk reuse");
            clear_shader_cache();
            fprintf(stderr,"disabled shader warm-up regression passed\n");
            return 0;
        }
        require(shaders.size()==recipes.size(),"boot failed to warm every recipe");
        require(stats.spirvCompiles==compiles,"warm-up recompiled SPIR-V");
        require(programHashes.empty() && lastProgramFrame==~uint64_t{0},"scratch memo survived warm-up");
        auto translations=stats.compiles;
        size_t index=0;
        for(auto& [real,vs]:recipes) {
            uint32_t start=vs ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS;
            uint32_t relocated=mem::host_alloc(8,256);
            memcpy(ppc_ptr(relocated),ppc_ptr(real[start]<<8),8); real[start]=relocated>>8;
            if(vs) {
                uint32_t relocatedFetch=mem::host_alloc(32,256);
                memcpy(ppc_ptr(relocatedFetch),ppc_ptr(real[mmSQ_PGM_START_FS]<<8),32);
                real[mmSQ_PGM_START_FS]=relocatedFetch>>8;
            }
            auto* actual=lookup(real,vs,10);
            const auto& original=expected[index++];
            require(actual->glsl==original.glsl && actual->spirv==original.spirv &&
                    !memcmp(&actual->uniforms,&original.uniforms,sizeof(original.uniforms)) &&
                    !memcmp(&actual->mapping,&original.mapping,sizeof(original.mapping)),
                    "warm-up metadata differs from real translation");
            require(actual->descriptorRanks.blocks==original.descriptorRanks.blocks &&
                    actual->descriptorRanks.textures==original.descriptorRanks.textures &&
                    actual->descriptorRanks.support==original.descriptorRanks.support &&
                    actual->descriptorRanks.count==original.descriptorRanks.count &&
                    actual->descriptorRanks.valid==original.descriptorRanks.valid,
                    "warm-up descriptor rank plan differs");
        }
        require(stats.compiles==translations,"real relocated lookup missed warm shader");
        uint32_t address=regs[mmSQ_PGM_START_PS]<<8;
        reinterpret_cast<uint32_t*>(ppc_ptr(address))[1]^=1; // change an export constant
#ifdef __SWITCH__
        uint64_t epoch=gx2::shaderProgramWrites.generation();
        gx2::shaderProgramWrites.notify(address,8);
        require(gx2::shaderProgramWrites.generation()!=epoch,"real program was not watched");
#endif
        lookup(regs,false,11);
        require(stats.compiles==translations+1,"changed real program reused warmed shader");
        clear_shader_cache();
        fprintf(stderr,"shader warm-up regressions passed (%zu variants)\n",recipes.size());
        return 0;
    } catch(const std::exception& e) { fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
