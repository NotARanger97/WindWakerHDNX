// Real Latte microcode -> Cemu GLSL -> SPIR-V for the Vulkan backend.
#include "shaders.h"
#include "mods/cemu_pack.h"
#include "mods/shader_interface.h"
#include "graphic_pack_hash.h"
#include "api.h"
#include "draw_options.h"
#include "exact_state_memo.h"
#include "word_cache.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteShaderAssembly.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/ISA/LatteInstructions.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "gx2/gx2.h"
#include "gx2/gx2_cmd.h"
#include "gx2/gx2_regs.h"
#ifdef __SWITCH__
#include "gx2/shader_program_writes.h"
#endif
#include "ppc.h"
#include "runtime.h"
#include "util/helpers/StringBuf.h"
#define XXH_INLINE_ALL
#include "../../../third_party/xxhash/xxhash.h"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <memory>
#include <malloc.h>
#include <filesystem>
#include <glslang/build_info.h>
#include "platform/host.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <future>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_map>
#include <unordered_set>

LatteDecompilerShader* FinishDecompiledShader(LatteDecompilerOutput_t& output);
LatteFetchShader* LatteShaderRecompiler_createFetchShader(LatteFetchShader::CacheHash hash,
    uint32* regs, uint32* code, uint32 size);

namespace gfxvk::vk {
using Latte::REGADDR;
namespace {
// Variant key -> translation. Variants whose translations are identical (Shader::output) share one
// Shader, keyed by the first of them: thousands of register combinations map to ~1k distinct outputs,
// and pipelines (keyed by Shader::key) are shared with them.
std::unordered_map<uint64_t, Shader*> shaders;
std::vector<std::unique_ptr<Shader>> shaderStore;  // owns every Shader in shaders
struct OutputHash { size_t operator()(const std::array<uint64_t,2>& o) const { return size_t(o[0]); } };
std::unordered_map<std::array<uint64_t,2>, Shader*, OutputHash> shaderOutputs;  // ready translations
std::unordered_map<uint64_t, LatteFetchShader*> fetchShaders;
std::unordered_map<const LatteFetchShader*, uint64_t> fetchKeys;  // reverse of fetchShaders
struct ProgramHash { uint64_t hash = 0, frame = ~uint64_t{0}, global = 0; };
#ifdef __SWITCH__
// Content validation of watched program ranges (revalidate_programs): a copy of each hashed range's
// bytes, and the ranges on each guest page. Render thread only.
std::unordered_map<uint64_t, std::vector<uint8_t>> programBytes;
std::unordered_map<uint32_t, std::vector<uint64_t>> programsByPage;
#endif
std::unordered_map<uint64_t, ProgramHash> programHashes;
struct LastProgramHash {
    uint64_t range = 0;
    ProgramHash value;
};
std::array<LastProgramHash, 3> lastProgramHashes;
std::array<LastProgramHash, 256> directProgramHashes;  // program_hash; cleared with the map
uint64_t lastProgramFrame = ~uint64_t{0};
struct LastShader {
    const uint32_t* regs = nullptr;
    LatteFetchShader* fetch = nullptr;
    uint64_t frame = 0, generation = 0, fsKey = 0;
    uint32_t primitive = 0;
    Shader* shader = nullptr;
    uint32_t address = 0, size = 0;
};
LastShader lastShaders[2];
struct LastFetch {
    uint64_t frame = ~uint64_t{0}, key = 0;
    uint32_t address = 0, size = 0, count = 0;
    bool compact = false;
    std::array<uint32_t, 2> stepRates{};
    LatteFetchShader* fetch = nullptr;
    uint32_t programSize = 0;
};
LastFetch lastFetch;
#ifdef __SWITCH__
std::array<LastFetch, 64> fetchRangeMemo;
#endif
ShaderStats stats;
ExactStateMemo<105+5*LATTE_NUM_MAX_TEX_UNITS> stateMemo;
// Same full-byte word mixer used by the working Metal backend. memcpy keeps
// unaligned microcode/state safe; the tail still includes every remaining byte.
uint64_t hash_bytes(const void* bytes, size_t size, uint64_t hash = 0x9E3779B97F4A7C15ull) {
    const auto* p = static_cast<const uint8_t*>(bytes);
    size_t i=0;
    for (; i+8<=size; i+=8) {
        uint64_t word;
        memcpy(&word,p+i,8);
        hash=(hash^word)*0xFF51AFD7ED558CCDull;
        hash^=hash>>32;
    }
    for (; i<size; ++i) hash=(hash^p[i])*0x100000001B3ull;
    return hash^(hash>>29);
}
// Disk entries retain exact GLSL to verify hash collisions and translator changes.
// Bump for compilation defaults, translator/key changes or input-layout changes.
constexpr char cacheRecipe[] = "WWVKSC03:inputs1:glsl450:vk1.1:spv1.3:noopt:resources1:"
    GLSLANG_VERSION_FLAVOR;
constexpr size_t maxCacheBytes = 128u * 1024u * 1024u;
constexpr size_t maxSourceBytes = 4u * 1024u * 1024u;
constexpr size_t maxSpirvWords = 1024u * 1024u;
constexpr size_t maxCacheEntries = 65536, maxCacheVariants = 1u << 20;  // variants include aliases
using TranslationState = std::array<uint32_t,105+5*LATTE_NUM_MAX_TEX_UNITS>;
// Intern exact state separately: many programs share these words. The hot
// pair key is only five words, rather than duplicating ~1 KiB for every pair.
WordCache<uint64_t> pairStates;
WordCache<ShaderPair> pairMemo;
// translate_pair's direct-mapped memo by program ranges; cleared with pairMemo (state ids and Shader
// pointers it holds are recycled/destroyed there)
struct RangePair { uint64_t vs = 0, ps = 0, state = 0, fsKey = 0, epoch = ~uint64_t{0}; uintptr_t fetch = 0; ShaderPair pair; };
std::array<RangePair, 1024> rangePairs;
uint64_t nextPairStateId = 0;
size_t pairMemoCount = 0;
struct PairState {
    const uint32_t* regs = nullptr;
    uint64_t generation = 0, id = 0;
    uint32_t primitive = 0;
};
PairState pairState;
// The analyzer also annotates framebuffer aliases (unused by Vulkan), and the
// raw fetch parser can annotate Metal strides in dual-renderer builds. Preserve
// those reads too, without changing the established Vulkan variant key.
using TranslationAnnotations = std::array<uint32_t,34+7*LATTE_NUM_MAX_TEX_UNITS>;
constexpr uint32_t maxProgramBytes = 0x100000, maxFetchBytes = 0x1000;
struct TranslationInputs {
    uint64_t key = 0;
    TranslationState state{}; // includes the masked primitive type
    TranslationAnnotations annotations{};
    std::vector<uint8_t> program, fetch;
    uint32_t fetchSizeReg = 0;
    std::array<uint32_t,2> stepRates{};
};
struct DiskShader {
    std::string source;
    std::shared_ptr<const std::vector<uint32_t>> words;  // shared by every variant using it
    bool vertex;
    bool fromDisk = false;
    std::vector<TranslationInputs> variants;
    // variants whose translation equals one of the variants above: {variant key, that variant's key}
    std::vector<std::pair<uint64_t,uint64_t>> aliases;
};
std::unordered_map<uint64_t, DiskShader> diskShaders;
std::unordered_set<uint64_t> diskVariants;
std::string diskPath;
bool diskInitialized = false, diskDirty = false;
uint64_t diskChangedFrame = 0, diskAttemptFrame = 0, diskRevision = 0;
size_t diskBytes = 40;
// Journal (spirv.journal next to spirv.bin): periodic saves append only what changed since the last
// save. Rewriting the whole cache each time (47 MB after a few hours of play) cost the render thread
// ~30 ms for the snapshot copy, plus a second full copy of the cache in the writer.
//   header: "WWVKSJ02" ("WWVKSJ01" is read too: no alias records), cache fingerprint, base id (payload
//           checksum of the spirv.bin it extends, 0 when there was none)
//   blocks: payload length (8), payload checksum (8), records:
//     0 entry:   key 8, stage 4, source length 4, word count 4, source, SPIR-V words (replaces the entry)
//     1 variant: entry key 8, then the SC03 variant fields
//     2 alias:   entry key 8, variant key 8, key of the entry's variant with the same translation 8
// Loading applies whole blocks in order and stops at the first damaged one (a save cut off by closing
// the game). A full save (shutdown, boot compaction) writes spirv.bin and deletes the journal.
std::string journalPath;
std::vector<uint8_t> journalPending;  // records since the last save or snapshot
uint64_t journalBase = 0;
bool journalBroken = false;           // a failed append: full saves until one succeeds
bool oldFormat = false;               // SC03 spirv.bin or SJ01 journal loaded: rewrite as SC04 at boot
size_t journalBytes = 0, journalRecords = 0;
constexpr size_t journalHeaderBytes = 24, compactJournalBytes = 16u * 1024u * 1024u;
size_t input_bytes(const TranslationInputs& input) {
    return 28+sizeof(input.state)+sizeof(input.annotations)+input.program.size()+input.fetch.size();
}
size_t entry_bytes(const DiskShader& entry) {
    size_t bytes=24+entry.source.size()+entry.words->size()*4;
    for(const auto& input:entry.variants) bytes+=input_bytes(input);
    return bytes+4+16*entry.aliases.size();
}
uint64_t cache_checksum(const uint8_t* p, size_t n) {
    uint64_t h=14695981039346656037ull;
    for(size_t i=0;i<n;++i) h=(h^p[i])*1099511628211ull;
    return h;
}
uint64_t cache_fingerprint() {
    return cache_checksum(reinterpret_cast<const uint8_t*>(cacheRecipe),sizeof(cacheRecipe)) ^
        (uint64_t(GLSLANG_VERSION_MAJOR)<<48) ^ (uint64_t(GLSLANG_VERSION_MINOR)<<32) ^
        (uint64_t(GLSLANG_VERSION_PATCH)<<16);
}
// Key of a cache entry: its GLSL (XXH3: the byte-wise FNV used before cost ~0.05 ms of every
// translation), stage and compile recipe. Recomputed from the stored source when a cache loads, so
// files with older keys need no migration; loadedKeys maps their keys for the journal's records.
uint64_t spirv_key(const char* glsl, size_t size, bool vertex) {
    static const uint64_t fingerprint=cache_fingerprint();
    return XXH3_64bits(glsl,size) ^ fingerprint ^ (vertex ? 0x9E3779B97F4A7C15ull : 0xD1B54A32D192ED03ull);
}
std::unordered_map<uint64_t,uint64_t> loadedKeys;  // boot only: key stored in the file -> spirv_key
uint64_t loaded_key(uint64_t key) {
    auto it=loadedKeys.find(key);
    return it!=loadedKeys.end() ? it->second : key;
}
void cache_put(std::vector<uint8_t>& out,uint64_t value,size_t bytes) {
    for(size_t i=0;i<bytes;++i) out.push_back(uint8_t(value>>(i*8)));
}
bool cache_take(const std::vector<uint8_t>& in,size_t& offset,uint64_t& value,size_t bytes) {
    if(offset>in.size() || bytes>in.size()-offset) return false;
    value=0; for(size_t i=0;i<bytes;++i) value|=uint64_t(in[offset++])<<(i*8);
    return true;
}
bool cache_spirv_valid(const std::vector<uint32_t>& words) {
    if(words.size()<5 || words.size()>maxSpirvWords || words[0]!=0x07230203 ||
       words[1]!=0x00010300 || !words[3] || words[3]>0x400000 || words[4]!=0) return false;
    for(size_t i=5;i<words.size();) {
        size_t length=words[i]>>16;
        if(!length || length>words.size()-i) return false;
        i+=length;
    }
    return words.size()>5;
}
uint32_t input_word(const std::vector<uint8_t>& bytes, size_t offset) {
    uint32_t word; memcpy(&word,bytes.data()+offset,4); return word;
}
// The adapted parsers do not bounds-check clause/literal reads. Do not replay
// programs whose translation could depend on bytes outside the saved input.
bool program_inputs_complete(const std::vector<uint8_t>& bytes) {
    if(bytes.empty() || (bytes.size()&7)) return false;
    auto fits=[&](uint64_t offset,uint64_t size) {
        return offset<=bytes.size() && size<=bytes.size()-offset;
    };
    std::vector<uint32_t> calls{0};
    std::unordered_set<uint32_t> seen;
    for(size_t root=0;root<calls.size();++root) {
        for(uint64_t cf=uint64_t(calls[root])*8;fits(cf,8);cf+=8) {
            uint32_t w0=input_word(bytes,size_t(cf)), w1=input_word(bytes,size_t(cf)+4);
            uint32_t op=(w1>>23)&0x7F;
            if(op<0x40) {
                if(op==GPU7_CF_INST_TEX) {
                    uint32_t count=((w1>>10)&7) | ((w1>>16)&8);
                    if(!fits(uint64_t(w0)*8,uint64_t(count+1)*16)) return false;
                } else if(op==GPU7_CF_INST_CALL) {
                    if(!fits(uint64_t(w0)*8,8)) return false;
                    if(seen.insert(w0).second) calls.push_back(w0);
                }
                // Main stops at EOP; the subroutine parser stops only at RETURN.
                if((root==0 && (w1&(1u<<21))) || (root!=0 && op==GPU7_CF_INST_RETURN)) break;
            } else {
                uint32_t aluOp=(w1>>26)&15;
                if(aluOp!=8 && aluOp!=9 && aluOp!=10 && aluOp!=11 && aluOp!=14 && aluOp!=15) continue;
                uint64_t address=uint64_t(w0&0x3FFFFF)*8;
                uint32_t count=((w1>>18)&0x7F)+1, literalMask=0;
                if(!fits(address,uint64_t(count)*8)) return false;
                for(uint32_t i=0;i<count;) {
                    uint32_t a0=input_word(bytes,size_t(address)+i*8);
                    uint32_t a1=input_word(bytes,size_t(address)+i*8+4); ++i;
                    if(GPU7_ALU_SRC_IS_LITERAL(a0&0x1FF)) literalMask|=1u<<((a0>>10)&3);
                    if(GPU7_ALU_SRC_IS_LITERAL((a0>>13)&0x1FF)) literalMask|=1u<<((a0>>23)&3);
                    if(((a1>>13)&31)>=8 && GPU7_ALU_SRC_IS_LITERAL(a1&0x1FF))
                        literalMask|=1u<<((a1>>10)&3);
                    if(a0&0x80000000) {
                        if(literalMask) {
                            uint32_t literals=(literalMask&12) ? 2 : 1;
                            if(!fits(address+uint64_t(i)*8,literals*8)) return false;
                            i+=literals;
                        }
                        literalMask=0;
                    }
                }
            }
        }
    }
    return true;
}
bool fetch_inputs_complete(const std::vector<uint8_t>& bytes) {
    if(bytes.size()<16) return false;
    if(!memcmp(bytes.data(),"WWFS",4)) return true; // compact length/header checked separately
    if(bytes.size()&15) return false;
    uint32_t first=input_word(bytes,0), second=input_word(bytes,4);
    if(!(first&1) && first<=0x30 && (second&~((3u<<10)|(1u<<19)))==0x01800000) {
        for(size_t cf=0;cf+8<=bytes.size();cf+=8) {
            uint32_t w0=input_word(bytes,cf), w1=input_word(bytes,cf+4), op=(w1>>23)&0x7F;
            if(op==LatteCFInstruction::INST_VTX_TC) {
                uint64_t offset=uint64_t(w0)*8;
                uint64_t length=((((w1>>10)&7) | ((w1>>16)&8))+1)*16;
                if(offset>bytes.size() || length>bytes.size()-offset) return false;
            }
            if(op==LatteCFInstruction::INST_RETURN || (w1&(1u<<21))) break;
        }
    } else {
        for(size_t word=0;word<bytes.size()/4;) {
            uint32_t op=input_word(bytes,word*4)&31;
            if(op==1 && bytes.size()/4-word<4) return false;
            word+=(op==1 || op==2) ? 4 : 1;
        }
    }
    return true;
}
// SC02 (no translation inputs): same records without the variant list, keyed with the SC02 recipe's
// fingerprint. Its SPIR-V stays valid (same compile options): migrate the entries, re-keyed for SC03,
// so the first session after the upgrade decompiles but never runs glslang again.
constexpr char cacheRecipeSC02[] = "WWVKSC02:glsl450:vk1.1:spv1.3:noopt:resources1:" GLSLANG_VERSION_FLAVOR;
uint64_t cache_fingerprint_sc02() {
    return cache_checksum(reinterpret_cast<const uint8_t*>(cacheRecipeSC02),sizeof(cacheRecipeSC02)) ^
        (uint64_t(GLSLANG_VERSION_MAJOR)<<48) ^ (uint64_t(GLSLANG_VERSION_MINOR)<<32) ^
        (uint64_t(GLSLANG_VERSION_PATCH)<<16);
}
bool decode_disk_cache_sc02(const std::vector<uint8_t>& file,
                           std::unordered_map<uint64_t,DiskShader>& result) {
    if(file.size()<40 || file.size()>maxCacheBytes || memcmp(file.data(),"WWVKSC02",8)) return false;
    size_t pos=8; uint64_t fingerprint,length,checksum,count;
    if(!cache_take(file,pos,fingerprint,8) || fingerprint!=cache_fingerprint_sc02() ||
       !cache_take(file,pos,length,8) || length!=file.size()-40 ||
       !cache_take(file,pos,checksum,8) || checksum!=cache_checksum(file.data()+40,file.size()-40) ||
       !cache_take(file,pos,count,8) || count>maxCacheEntries) return false;
    std::unordered_map<uint64_t,DiskShader> decoded;
    for(uint64_t i=0;i<count;++i) {
        uint64_t key,stage,sourceLength,wordCount;
        if(!cache_take(file,pos,key,8) || !cache_take(file,pos,stage,4) || stage>1 ||
           !cache_take(file,pos,sourceLength,4) || !sourceLength || sourceLength>maxSourceBytes ||
           !cache_take(file,pos,wordCount,4) || wordCount<5 || wordCount>maxSpirvWords ||
           sourceLength>file.size()-pos || wordCount>(file.size()-pos-sourceLength)/4) return false;
        DiskShader entry;
        entry.vertex=stage!=0;
        entry.fromDisk=true;
        entry.source.assign(reinterpret_cast<const char*>(file.data()+pos),size_t(sourceLength));
        pos+=sourceLength;
        {
            std::vector<uint32_t> words;
            words.reserve(size_t(wordCount));
            for(uint64_t j=0;j<wordCount;++j) {
                uint64_t word; if(!cache_take(file,pos,word,4)) return false;
                words.push_back(uint32_t(word));
            }
            if(!cache_spirv_valid(words)) return false;
            entry.words=std::make_shared<const std::vector<uint32_t>>(std::move(words));
        }
        const uint64_t loaded=spirv_key(entry.source.data(),entry.source.size(),entry.vertex);
        if(!decoded.emplace(loaded,std::move(entry)).second) return false;
    }
    if(pos!=file.size()) return false;
    result=std::move(decoded); return true;
}
// One variant record (SC03 and journal): key, program/fetch sizes, fetch size register, step rates,
// state, annotations, program bytes, fetch bytes.
bool take_variant(const std::vector<uint8_t>& file, size_t& pos, bool vertex, TranslationInputs& input) {
    uint64_t programSize,fetchSize,value;
    if(!cache_take(file,pos,input.key,8) ||
       !cache_take(file,pos,programSize,4) || !programSize || programSize>maxProgramBytes || (programSize&7) ||
       !cache_take(file,pos,fetchSize,4) || fetchSize>maxFetchBytes ||
       (vertex ? !fetchSize : fetchSize!=0) ||
       !cache_take(file,pos,value,4)) return false;
    input.fetchSizeReg=uint32_t(value);
    for(auto& rate:input.stepRates) {
        if(!cache_take(file,pos,value,4)) return false;
        rate=uint32_t(value);
    }
    for(auto& word:input.state) {
        if(!cache_take(file,pos,value,4)) return false;
        word=uint32_t(value);
    }
    for(auto& word:input.annotations) {
        if(!cache_take(file,pos,value,4)) return false;
        word=uint32_t(value);
    }
    if(programSize>file.size()-pos || fetchSize>file.size()-pos-programSize) return false;
    input.program.assign(file.begin()+pos,file.begin()+pos+programSize); pos+=programSize;
    input.fetch.assign(file.begin()+pos,file.begin()+pos+fetchSize); pos+=fetchSize;
    // Fetch headers use guest big endian; raw Latte instructions use host words.
    bool compact=fetchSize>=16 && !memcmp(input.fetch.data(),"WWFS",4);
    if(vertex) {
        if(compact) {
            uint32_t n=(uint32_t(input.fetch[4])<<24) | (uint32_t(input.fetch[5])<<16) |
                (uint32_t(input.fetch[6])<<8) | input.fetch[7];
            if(n>64 || fetchSize!=16+n*16) return false;
        } else if(fetchSize<16 || (fetchSize&15) ||
                  (uint32_t(input.fetchSizeReg<<3)!=fetchSize)) return false;
    }
    return true;
}
void put_variant(std::vector<uint8_t>& out, const TranslationInputs& input) {
    cache_put(out,input.key,8);
    cache_put(out,input.program.size(),4); cache_put(out,input.fetch.size(),4);
    cache_put(out,input.fetchSizeReg,4);
    for(uint32_t rate:input.stepRates) cache_put(out,rate,4);
    for(uint32_t word:input.state) cache_put(out,word,4);
    for(uint32_t word:input.annotations) cache_put(out,word,4);
    out.insert(out.end(),input.program.begin(),input.program.end());
    out.insert(out.end(),input.fetch.begin(),input.fetch.end());
}
// One SPIR-V record without its variants (SC03 adds the variant count before the source).
bool take_spirv(const std::vector<uint8_t>& file, size_t& pos, uint64_t sourceLength, uint64_t wordCount,
                DiskShader& entry) {
    if(sourceLength>file.size()-pos || wordCount>(file.size()-pos-sourceLength)/4) return false;
    entry.source.assign(reinterpret_cast<const char*>(file.data()+pos),size_t(sourceLength));
    pos+=sourceLength;
    std::vector<uint32_t> words;
    words.reserve(size_t(wordCount));
    for(uint64_t j=0;j<wordCount;++j) {
        uint64_t word; if(!cache_take(file,pos,word,4)) return false;
        words.push_back(uint32_t(word));
    }
    if(!cache_spirv_valid(words)) return false;
    entry.words=std::make_shared<const std::vector<uint32_t>>(std::move(words));
    return true;
}
bool decode_disk_cache(const std::vector<uint8_t>& file,
                      std::unordered_map<uint64_t,DiskShader>& result) {
    // 8-byte schema, fingerprint, payload length, checksum, record count.
    // SC02 has no input recipes. Reject it atomically and rebuild during play.
    // SC04 adds each entry's aliases after its variants; SC03 (none) is still read.
    if(file.size()<40 || file.size()>maxCacheBytes ||
       (memcmp(file.data(),"WWVKSC03",8) && memcmp(file.data(),"WWVKSC04",8))) return false;
    const bool aliases=!memcmp(file.data(),"WWVKSC04",8);
    size_t pos=8; uint64_t fingerprint,length,checksum,count;
    if(!cache_take(file,pos,fingerprint,8) || fingerprint!=cache_fingerprint() ||
       !cache_take(file,pos,length,8) || length!=file.size()-40 ||
       !cache_take(file,pos,checksum,8) || checksum!=cache_checksum(file.data()+40,file.size()-40) ||
       !cache_take(file,pos,count,8) || count>maxCacheEntries) return false;
    std::unordered_map<uint64_t,DiskShader> decoded;
    std::unordered_set<uint64_t> variants;
    for(uint64_t i=0;i<count;++i) {
        uint64_t key,stage,sourceLength,wordCount,variantCount;
        if(!cache_take(file,pos,key,8) || !cache_take(file,pos,stage,4) || stage>1 ||
           !cache_take(file,pos,sourceLength,4) || !sourceLength || sourceLength>maxSourceBytes ||
           !cache_take(file,pos,wordCount,4) || wordCount<5 || wordCount>maxSpirvWords ||
           !cache_take(file,pos,variantCount,4) || variantCount>maxCacheVariants-variants.size()) return false;
        DiskShader entry;
        entry.vertex=stage!=0;
        entry.fromDisk=true;
        if(!take_spirv(file,pos,sourceLength,wordCount,entry)) return false;
        for(uint64_t j=0;j<variantCount;++j) {
            TranslationInputs input;
            if(!take_variant(file,pos,entry.vertex,input) || !variants.insert(input.key).second) return false;
            // Incomplete recipes are skipped during replay, not trusted merely
            // because the file checksum is valid. Canonical SPIR-V stays usable.
            entry.variants.push_back(std::move(input));
        }
        uint64_t aliasCount=0;
        if(aliases && (!cache_take(file,pos,aliasCount,4) || aliasCount>maxCacheVariants-variants.size())) return false;
        for(uint64_t j=0;j<aliasCount;++j) {
            uint64_t alias,target;
            if(!cache_take(file,pos,alias,8) || !cache_take(file,pos,target,8) ||
               std::none_of(entry.variants.begin(),entry.variants.end(),[&](const auto& v) { return v.key==target; }) ||
               !variants.insert(alias).second) return false;
            entry.aliases.push_back({alias,target});
        }
        const uint64_t loaded=spirv_key(entry.source.data(),entry.source.size(),entry.vertex);
        if(loaded!=key) loadedKeys[key]=loaded;
        if(!decoded.emplace(loaded,std::move(entry)).second) return false;
    }
    if(pos!=file.size()) return false;
    result=std::move(decoded); return true;
}
// Applies the journal's intact blocks to diskShaders (boot: after the base file).
void load_journal() {
    std::error_code ec;
    auto length=std::filesystem::file_size(journalPath,ec);
    if(ec || length<journalHeaderBytes || length>maxCacheBytes) return;
    std::vector<uint8_t> file(static_cast<size_t>(length));
    FILE* f=fopen(journalPath.c_str(),"rb");
    if(!f) return;
    const bool read=fread(file.data(),1,file.size(),f)==file.size();
    fclose(f);
    size_t pos=8; uint64_t fingerprint,base;
    if(!read || (memcmp(file.data(),"WWVKSJ01",8) && memcmp(file.data(),"WWVKSJ02",8)) ||
       !cache_take(file,pos,fingerprint,8) ||
       fingerprint!=cache_fingerprint() || !cache_take(file,pos,base,8) || base!=journalBase) return;
    size_t blocks=0;
    while(pos<file.size()) {
        uint64_t bytes,checksum;
        if(!cache_take(file,pos,bytes,8) || !cache_take(file,pos,checksum,8) || bytes>file.size()-pos ||
           checksum!=cache_checksum(file.data()+pos,size_t(bytes))) break;
        // parse the whole block before applying any of it
        std::vector<std::pair<uint64_t,DiskShader>> entries;
        std::vector<std::pair<uint64_t,TranslationInputs>> inputs;
        struct Alias { uint64_t entry, alias, target; };
        std::vector<Alias> aliases;
        std::vector<std::pair<uint8_t,size_t>> order;  // record type, index
        const std::vector<uint8_t> block(file.begin()+pos,file.begin()+pos+bytes);
        size_t at=0; bool ok=true;
        while(ok && at<block.size()) {
            const uint8_t type=block[at++];
            uint64_t key;
            if(!cache_take(block,at,key,8)) { ok=false; break; }
            if(type==0) {
                uint64_t stage,sourceLength,wordCount;
                DiskShader entry;
                ok=cache_take(block,at,stage,4) && stage<=1 && cache_take(block,at,sourceLength,4) &&
                   sourceLength && sourceLength<=maxSourceBytes && cache_take(block,at,wordCount,4) &&
                   wordCount>=5 && wordCount<=maxSpirvWords;
                entry.vertex=stage!=0; entry.fromDisk=true;
                ok=ok && take_spirv(block,at,sourceLength,wordCount,entry);
                if(ok) {
                    const uint64_t loaded=spirv_key(entry.source.data(),entry.source.size(),entry.vertex);
                    if(loaded!=key) loadedKeys[key]=loaded;
                    order.push_back({0,entries.size()}); entries.push_back({loaded,std::move(entry)});
                }
            } else if(type==1) {
                key=loaded_key(key);
                // the variant's stage is its entry's: an entry earlier in this block or already loaded
                bool vertex=false, found=false;
                for(auto it=entries.rbegin();it!=entries.rend();++it)
                    if(it->first==key) { vertex=it->second.vertex; found=true; break; }
                if(!found) {
                    auto e=diskShaders.find(key);
                    if(e!=diskShaders.end()) { vertex=e->second.vertex; found=true; }
                }
                TranslationInputs input;
                ok=take_variant(block,at,vertex,input);
                if(ok && found) { order.push_back({1,inputs.size()}); inputs.push_back({key,std::move(input)}); }
            } else if(type==2) {
                key=loaded_key(key);
                uint64_t alias,target;
                ok=cache_take(block,at,alias,8) && cache_take(block,at,target,8);
                if(ok) { order.push_back({2,aliases.size()}); aliases.push_back({key,alias,target}); }
            } else ok=false;
        }
        if(!ok) break;
        for(auto [type,index]:order) {
            if(type==0) {
                auto& [key,entry]=entries[index];
                auto old=diskShaders.find(key);
                size_t oldBytes=0;
                if(old!=diskShaders.end()) {
                    oldBytes=entry_bytes(old->second);
                    for(const auto& input:old->second.variants) diskVariants.erase(input.key);
                    for(const auto& alias:old->second.aliases) diskVariants.erase(alias.first);
                } else if(diskShaders.size()>=maxCacheEntries) continue;
                diskBytes=diskBytes-oldBytes+entry_bytes(entry);
                diskShaders.insert_or_assign(key,std::move(entry));
            } else if(type==2) {
                const auto& a=aliases[index];
                auto e=diskShaders.find(a.entry);
                if(e==diskShaders.end() || diskVariants.size()>=maxCacheVariants ||
                   std::none_of(e->second.variants.begin(),e->second.variants.end(),
                                [&](const auto& v) { return v.key==a.target; }) ||
                   !diskVariants.insert(a.alias).second) continue;
                diskBytes+=16;
                e->second.aliases.push_back({a.alias,a.target});
            } else {
                auto& [key,input]=inputs[index];
                auto e=diskShaders.find(key);
                if(e==diskShaders.end() || diskVariants.size()>=maxCacheVariants || !diskVariants.insert(input.key).second)
                    continue;
                diskBytes+=input_bytes(input);
                e->second.variants.push_back(std::move(input));
            }
            ++journalRecords;
        }
        pos+=bytes; ++blocks;
    }
    journalBytes=pos;
    if(journalRecords && !memcmp(file.data(),"WWVKSJ01",8)) oldFormat=true;  // appends start an SJ02 journal
    if(pos<file.size()) {
        // drop the damaged tail now: blocks appended after it would never be read
        bool cut=false;
        if(FILE* out=fopen(journalPath.c_str(),"wb")) {
            cut=fwrite(file.data(),1,pos,out)==pos;
            cut=fclose(out)==0 && cut;
        }
        if(!cut) journalBroken=true;
    }
    fprintf(stderr,"[vulkan] shader cache journal: %zu blocks, %zu records, %.1f MiB%s\n",blocks,journalRecords,
            pos/1048576.0,pos<file.size() ? " (damaged tail ignored)" : "");
}
void initialize_disk_cache() {
    if(diskInitialized) return;
    diskInitialized=true;
    auto start=std::chrono::steady_clock::now();
    try {
        const char* overridePath=getenv("WWHD_VK_SHADER_CACHE");
        const char* general=getenv("WWHD_SHADER_CACHE");
        if((overridePath && !strcmp(overridePath,"0")) ||
           (!overridePath && general && !strcmp(general,"0"))) return;
        std::string dir=overridePath ? overridePath : host::config_dir()+"/shadercache/vulkan-shaders";
        if(dir.empty()) return;
        diskPath=dir+"/spirv.bin";
        journalPath=dir+"/spirv.journal";
        [&] {
            std::error_code ec;
            auto length=std::filesystem::file_size(diskPath,ec);
            if(ec || length<40 || length>maxCacheBytes) return;
            std::vector<uint8_t> bytes(static_cast<size_t>(length));
            FILE* file=fopen(diskPath.c_str(),"rb");
            if(!file) return;
            bool read=fread(bytes.data(),1,bytes.size(),file)==bytes.size() && fgetc(file)==EOF && !ferror(file);
            fclose(file);
            if(read && decode_disk_cache(bytes,diskShaders)) {
                oldFormat=!memcmp(bytes.data(),"WWVKSC03",8);
                for(const auto& [key,entry]:diskShaders) {
                    for(const auto& input:entry.variants) diskVariants.insert(input.key);
                    for(const auto& alias:entry.aliases) diskVariants.insert(alias.first);
                }
                ++stats.diskLoads; diskBytes=bytes.size();
                size_t pos=24; cache_take(bytes,pos,journalBase,8);  // the payload checksum
            } else if(read && decode_disk_cache_sc02(bytes,diskShaders)) {
                // Saved again as SC03 once gameplay records translation inputs (diskDirty).
                ++stats.diskLoads; diskBytes=bytes.size();
                journalBroken=true;  // the next save rewrites the whole cache
                fprintf(stderr,"[vulkan] migrated %zu SPIR-V entries from the SC02 shader cache\n",diskShaders.size());
            }
        }();
        if(!journalBroken) load_journal();
        if(!loadedKeys.empty()) oldFormat=true;  // rewritten with the current keys at boot
        loadedKeys.clear();
    } catch(...) {
        diskShaders.clear(); diskVariants.clear(); diskBytes=40;
        journalBroken=true; journalBytes=journalRecords=0;
    }
    stats.diskLoadNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-start).count();
}

uint64_t program_validation(uint64_t frame) {
#ifdef __SWITCH__
    if (frame != ~uint64_t{0}) return gx2::shaderProgramWrites.generation();
#endif
    return frame;
}
uint64_t program_hash(uint32_t address, uint32_t size, uint64_t frame, size_t stage) {
    // Exact per-stage stamps make forward frame/epoch changes self-invalidating.
    // Clear on standalone calls/rewinds, which can revalidate the shared map
    // while another stage's old stamp would otherwise become current again.
    if (frame == ~uint64_t{0} || (lastProgramFrame != ~uint64_t{0} && frame < lastProgramFrame)) {
        lastProgramHashes = {};
    }
    lastProgramFrame = frame;
    // Include size: the same address can refer to different program ranges.
    const uint64_t range = (uint64_t(address) << 32) | size;
    auto& last = lastProgramHashes[stage];
    // Same validation stamp as the map (frame on desktop, write epoch on
    // Switch). Standalone calls always hash bytes immediately.
    if (frame != ~uint64_t{0} && last.range == range && last.value.frame == frame)
        return last.value.hash;
    // Direct-mapped copies of map entries, under the same validation stamp: alternating programs
    // skip the map lookup. A stale stamp falls through to the map, which revalidates the bytes.
    auto& slot = directProgramHashes[(range ^ (range >> 13) ^ (range >> 37)) & 255];
    if (frame != ~uint64_t{0} && slot.range == range && slot.value.frame == frame) {
        last = slot;
        return slot.value.hash;
    }
    auto& entry = programHashes[range];
#ifdef __SWITCH__
    // Switch: an epoch bump from revalidate_programs marks exactly the changed programs stale
    // (frame = ~0); every other entry's bytes were checked against its copy and are still current.
    // Only a global invalidation (or a stale/new entry) needs the bytes hashed again. Without this,
    // each of the 2-3 real program changes per frame rehashed and recopied every program in use.
    const uint64_t global = gx2::shaderProgramWrites.global_generation();
    if (frame != ~uint64_t{0} && entry.frame != frame && entry.frame != ~uint64_t{0} && entry.global == global)
        entry.frame = frame;
#endif
    if (frame == ~uint64_t{0} || entry.frame != frame) {
#ifdef __SWITCH__
        // Publish before reading bytes. A flush during hashing advances the
        // epoch and forces revalidation on the next lookup.
        gx2::shaderProgramWrites.watch(address, size);
        {
            auto [copy, inserted] = programBytes.try_emplace(range);
            copy->second.assign(ppc_ptr(address), ppc_ptr(address) + size);
            if (inserted)
                for (uint64_t page = address >> 12; page <= (uint64_t(address) + size - 1) >> 12; ++page)
                    programsByPage[uint32_t(page)].push_back(range);
        }
#endif
        entry.hash = hash_bytes(ppc_ptr(address), size);
        entry.frame = frame;
#ifdef __SWITCH__
        entry.global = global;
#endif
    }
    last = {range, entry};
    slot = last;
    return entry.hash;
}
#ifdef __SWITCH__
}  // namespace
// Before each command batch: programs on pages flushed since the last call are compared with their
// copies; only a real change advances the write epoch (and with it every program/shader memo).
void revalidate_programs() {
    bool changed = false;
    gx2::shaderProgramWrites.take_dirty([&](uint32_t page) {
        auto it = programsByPage.find(page);
        if (it == programsByPage.end()) return;
        for (uint64_t range : it->second) {
            auto copy = programBytes.find(range);
            if (copy == programBytes.end()) continue;
            const uint32_t address = uint32_t(range >> 32), size = uint32_t(range);
            if (copy->second.size() != size || memcmp(copy->second.data(), ppc_ptr(address), size)) {
                copy->second.assign(ppc_ptr(address), ppc_ptr(address) + size);
                if (auto entry = programHashes.find(range); entry != programHashes.end()) entry->second.frame = ~uint64_t{0};
                changed = true;
            }
        }
    });
    if (changed) { gx2::shaderProgramWrites.invalidate(); gx2::shaderProgramWrites.contentBumps.fetch_add(1); }
}
namespace {
#endif
// Hash all state affecting the GLSL analyzer. Buffer addresses are excluded from
// shader variants: unlike Metal framebuffer-fetch, Vulkan always samples images.
template<bool restore, size_t N>
size_t translation_state(uint32_t* regs, std::array<uint32_t,N>& state, bool vertex) {
    // 105 fixed words includes all four optional streamout strides, plus two
    // texture words and three sampler words per unit. Keep the original order.
    size_t count=0;
    auto put = [&](uint32_t first,uint32_t length) {
        if constexpr(restore) memcpy(regs+first,state.data()+count,length*sizeof(uint32_t));
        else memcpy(state.data()+count,regs+first,length*sizeof(uint32_t));
        count+=length;
    };
    auto masked = [&](uint32_t reg,uint32_t mask) {
        // Only masked bits belong to the translation key. Restoring zeros in
        // the other bits is equivalent for the Vulkan analyzer/emitter.
        if constexpr(restore) regs[reg]=state[count]&mask;
        else state[count]=regs[reg]&mask;
        ++count;
    };
    put(mmSQ_VTX_SEMANTIC_0, 32);
    put(mmSPI_VS_OUT_ID_0, 10);
    put(mmSPI_VS_OUT_CONFIG, 1); put(mmPA_CL_VS_OUT_CNTL, 1);
    put(mmSPI_PS_IN_CONTROL_0, 2); put(mmSPI_PS_INPUT_CNTL_0, 32);
    // Primitive mode changes point-size emission; point-sprite interpolation
    // changes fragment inputs. Streamout enable also affects shader resources.
    masked(REGADDR::VGT_PRIMITIVE_TYPE,0x3F);
    masked(mmSPI_INTERP_CONTROL_0,1u<<1); put(mmVGT_STRMOUT_EN,1);
    if (regs[mmVGT_STRMOUT_EN])
        for (uint32_t buffer = 0; buffer < 4; ++buffer)
            put(mmVGT_STRMOUT_VTX_STRIDE_0 + buffer * 4, 1);
    put(REGADDR::VGT_GS_MODE, 1); put(REGADDR::SQ_CONFIG, 1); put(mmCB_SHADER_MASK, 1);
    put(mmCB_SHADER_CONTROL, 1); put(mmDB_SHADER_CONTROL, 1);
    put(mmSPI_INPUT_Z, 1); put(REGADDR::SX_ALPHA_TEST_CONTROL, 1);
    // Clip enable and depth compare/write functions belong to pipeline state;
    // only viewport transform, half-Z and attachment enable affect translation.
    masked(REGADDR::PA_CL_VTE_CNTL,0x3F);
    masked(REGADDR::PA_CL_CLIP_CNTL,1u<<19);
    masked(REGADDR::DB_DEPTH_CONTROL,0x83);
    put(REGADDR::CB_COLOR_CONTROL, 1); put(REGADDR::CB_TARGET_MASK, 1);
    put(mmCB_COLOR0_INFO, 8);
    uint32_t base = vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
    for (uint32_t t = 0; t < LATTE_NUM_MAX_TEX_UNITS; ++t) {
        auto* w = regs + base + t * 7;
        if constexpr(restore) { w[0]=state[count]&7; w[4]=state[count]&0x300; }
        else state[count]=(w[0]&7) | (w[4]&0x300);
        ++count;
        masked(base+t*7+1,0x3F00000);
    }
    for (uint32_t t = 0; t < LATTE_NUM_MAX_TEX_UNITS * 3; ++t) {
        masked(REGADDR::SQ_TEX_SAMPLER_WORD0_0+t*3,0xF8000000);
    }
    return count;
}
template<bool restore>
void translation_annotations(uint32_t* regs, TranslationAnnotations& words, bool vertex) {
    size_t count=0;
    auto put=[&](uint32_t first,uint32_t length) {
        if constexpr(restore) memcpy(regs+first,words.data()+count,length*4);
        else memcpy(words.data()+count,regs+first,length*4);
        count+=length;
    };
    put(mmCB_COLOR0_BASE,8); put(mmCB_COLOR0_SIZE,8);
    put(REGADDR::PA_SC_GENERIC_SCISSOR_TL,2);
    put(vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS,
        7*LATTE_NUM_MAX_TEX_UNITS);
    for(uint32_t b=0;b<16;++b) put(mmSQ_VTX_ATTRIBUTE_BLOCK_START+b*7+2,1);
}
uint64_t state_hash(const uint32_t* regs, uint64_t hash, bool vertex, bool cacheLast) {
    TranslationState state;
    size_t count=translation_state<false>(const_cast<uint32_t*>(regs),state,vertex);
    ++stats.stateHashLookups;
    static const bool enabled = [] {
        const char* value = getenv("WWHD_VK_SHADER_STATE_MEMO");
        return value && !strcmp(value,"1");
    }();
    uint64_t result;
    if(enabled && cacheLast && stateMemo.find(vertex,state.data(),count,hash,result)) {
        ++stats.stateHashMemoHits;
        return result;
    }
    stats.stateHashBytes += count*sizeof(uint32_t);
    result = hash_bytes(state.data(),count*sizeof(uint32_t),hash);
    if(enabled && cacheLast) stateMemo.remember(vertex,state.data(),count,hash,result);
    return result;
}
// journal records of cache changes (spirv.journal); a record that cannot be completed is dropped whole
void journal_entry(uint64_t key, const DiskShader& entry) {
    if(diskPath.empty()) return;
    const size_t mark=journalPending.size();
    try {
        journalPending.push_back(0);
        cache_put(journalPending,key,8); cache_put(journalPending,entry.vertex,4);
        cache_put(journalPending,entry.source.size(),4); cache_put(journalPending,entry.words->size(),4);
        journalPending.insert(journalPending.end(),entry.source.begin(),entry.source.end());
        const size_t at=journalPending.size();
        journalPending.resize(at+entry.words->size()*4);
        for(size_t i=0;i<entry.words->size();++i) {
            const uint32_t w=(*entry.words)[i];
            for(int b=0;b<4;++b) journalPending[at+i*4+b]=uint8_t(w>>(b*8));
        }
    } catch(...) { journalPending.resize(mark); journalBroken=true; }
}
void journal_variant(uint64_t key, const TranslationInputs& input) {
    if(diskPath.empty()) return;
    const size_t mark=journalPending.size();
    try {
        journalPending.push_back(1);
        cache_put(journalPending,key,8);
        put_variant(journalPending,input);
    } catch(...) { journalPending.resize(mark); journalBroken=true; }
}
// The inputs that reproduce `shader`'s translation, read from the registers and guest memory now.
bool capture_inputs(const uint32_t* regs, const Shader& shader, LatteFetchShader* fetch, uint64_t fsKey,
                    TranslationInputs& input) try {
    input.key=shader.key;
    size_t count=translation_state<false>(const_cast<uint32_t*>(regs),input.state,shader.vertex);
    translation_annotations<false>(const_cast<uint32_t*>(regs),input.annotations,shader.vertex);
    uint32_t start=shader.vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS;
    uint32_t address=regs[start]<<8, size=regs[start+1]<<3;
    input.program.assign(ppc_ptr(address),ppc_ptr(address)+size);
    if(!program_inputs_complete(input.program)) return false;
    uint64_t capturedFetchKey=0;
    if(shader.vertex) {
        address=regs[mmSQ_PGM_START_FS]<<8;
        if(!address) return false;
        input.fetchSizeReg=regs[mmSQ_PGM_START_FS+1];
        input.stepRates={regs[REGADDR::VGT_INSTANCE_STEP_RATE_0],regs[REGADDR::VGT_INSTANCE_STEP_RATE_1]};
        bool compact=ld32(address)==0x57574653;
        uint32_t n=compact ? ld32(address+4) : 0;
        if(compact && n>64) return false;
        size=compact ? 16+n*16 : input.fetchSizeReg<<3;
        if(!size || size>maxFetchBytes || uint64_t(address)+size>0x100000000ull) return false;
        if(!compact && (size<16 || (size&15))) return false;
        input.fetch.assign(ppc_ptr(address),ppc_ptr(address)+size);
        if(!fetch_inputs_complete(input.fetch)) return false;
        capturedFetchKey=hash_bytes(input.fetch.data(),input.fetch.size());
        if(!compact) capturedFetchKey=hash_bytes(input.stepRates.data(),sizeof(input.stepRates),capturedFetchKey);
        auto parsed=fetchShaders.find(capturedFetchKey);
        if(capturedFetchKey!=fsKey || parsed==fetchShaders.end() || parsed->second!=fetch) return false;
    }
    // Capture only bytes that really reproduce this variant, even if a framed
    // program memo was stale or a caller supplied a fetch from other registers.
    uint64_t base=hash_bytes(input.program.data(),input.program.size()) ^ (shader.vertex ? 0x1111 : 0x2222);
    uint64_t key=hash_bytes(input.state.data(),count*4,base) ^ (shader.vertex ? capturedFetchKey*31 : 0);
    return key==input.key;
} catch(...) { return false; }
void store_inputs(uint64_t entryKey, DiskShader& entry, TranslationInputs input, uint64_t frame) try {
    const uint64_t key=input.key;
    const size_t bytes=input_bytes(input);
    if(diskPath.empty() || diskVariants.contains(key) || diskVariants.size()>=maxCacheVariants ||
       bytes>maxCacheBytes-diskBytes) return;
    diskVariants.insert(key);
    try { journal_variant(entryKey,input); entry.variants.push_back(std::move(input)); }
    catch(...) { diskVariants.erase(key); throw; }
    diskBytes+=bytes; ++diskRevision; diskDirty=true; diskChangedFrame=frame;
} catch(...) { /* Input recording is optional; rendering must still succeed. */ }
void record_translation(uint64_t entryKey, DiskShader& entry, const uint32_t* regs, const Shader& shader,
                        LatteFetchShader* fetch, uint64_t fsKey, uint64_t frame) {
    if(diskPath.empty() || diskVariants.contains(shader.key) || diskVariants.size()>=maxCacheVariants) return;
    TranslationInputs input;
    if(capture_inputs(regs,shader,fetch,fsKey,input)) store_inputs(entryKey,entry,std::move(input),frame);
}
bool has_variant(const DiskShader& entry, uint64_t key) {
    return std::any_of(entry.variants.begin(),entry.variants.end(),[&](const auto& v) { return v.key==key; });
}
// A variant whose translation equals the entry's recorded variant `target`: 16 bytes instead of its inputs.
void record_alias(uint64_t entryKey, DiskShader& entry, uint64_t alias, uint64_t target, uint64_t frame) try {
    if(diskPath.empty() || diskVariants.contains(alias) || diskVariants.size()>=maxCacheVariants ||
       diskBytes+16>maxCacheBytes || !has_variant(entry,target)) return;
    const size_t mark=journalPending.size();
    journalPending.push_back(2);
    cache_put(journalPending,entryKey,8); cache_put(journalPending,alias,8); cache_put(journalPending,target,8);
    try { entry.aliases.push_back({alias,target}); diskVariants.insert(alias); }
    catch(...) { journalPending.resize(mark); throw; }
    diskBytes+=16; ++diskRevision; diskDirty=true; diskChangedFrame=frame;
} catch(...) {}
void free_decompiler(LatteDecompilerShader* shader) {
    if (!shader) return;
    delete shader->strBuf_shaderSource;
    delete shader;
}
}

void select_renderer() { g_renderer = std::make_unique<VulkanRenderer>(); }

std::vector<uint32_t> compile_glsl(const std::string& source, bool vertex, std::string* error) {
    static std::once_flag init;
    static bool initialized = false;
    std::call_once(init, [] { initialized = glslang::InitializeProcess(); });
    if (error) error->clear();
    auto fail = [&](const std::string& reason) { if (error) *error = reason; return std::vector<uint32_t>{}; };
    if (!initialized) return fail("glslang initialization failed");
    EShLanguage stage = vertex ? EShLangVertex : EShLangFragment;
    glslang::TShader shader(stage);
    const char* text = source.c_str();
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, false, messages))
        return fail(std::string(shader.getInfoLog()) + shader.getInfoDebugLog());
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) return fail(program.getInfoLog());
    std::vector<uint32_t> words;
    glslang::SpvOptions options;
    options.disableOptimizer = true; // no SPIRV-Tools runtime dependency
    glslang::GlslangToSpv(*program.getIntermediate(stage), words, &options);
    if (words.empty()) return fail("glslang emitted empty SPIR-V");
    return words;
}

// Asynchronous SPIR-V compiles (WWHD_VK_ASYNC_SHADERS, on by default on Switch; "0" turns it off).
// glslang takes ~11 ms per shader on the Switch CPU, and the first sight of an area needs hundreds:
// the render thread stalled for seconds. New GLSL is compiled by two low-priority workers instead;
// draws using its shaders are skipped until the render thread completes them (poll_shader_compiles).
// Workers see only their job.
namespace {
bool async_shaders() {
    static const bool enabled = draw_option_enabled("WWHD_VK_ASYNC_SHADERS");  // Switch: on unless "0"
    return enabled;
}
struct CompileJob {
    uint64_t spirvKey = 0;
    bool vertex = false;
    std::string glsl;
    std::vector<uint32_t> words;
    std::string error;
    uint64_t ns = 0;
};
std::mutex compileMutex;
std::condition_variable compileReady;
std::deque<std::shared_ptr<CompileJob>> compileQueue, compileDone;
std::atomic<bool> compileFinished{false};  // a job is in compileDone (polled without the lock)
// render thread: the shaders waiting for each GLSL's compile
struct PendingShader {
    Shader* shader;
    uint64_t fsKey, frame;
    bool captured;
    TranslationInputs input;  // captured at translation: the registers are gone at completion
};
struct PendingCompile { std::string glsl; std::vector<PendingShader> shaders; };
std::unordered_map<uint64_t, PendingCompile> pendingCompiles;
size_t pendingShaders = 0;
// stands in for a translation postponed by the per-frame budget: never stored, so the next lookup
// translates the variant (draws skip it like a compiling shader)
Shader deferredShader = [] { Shader s; s.pending = true; return s; }();
uint64_t translateFrame = ~uint64_t{0}, translateNs = 0;
void compile_worker() {
    host::background_thread();
    for (;;) {
        std::shared_ptr<CompileJob> job;
        {
            std::unique_lock<std::mutex> lock(compileMutex);
            compileReady.wait(lock, [] { return !compileQueue.empty(); });
            job = std::move(compileQueue.front());
            compileQueue.pop_front();
        }
        const auto start = std::chrono::steady_clock::now();
        try { job->words = compile_glsl(job->glsl, job->vertex, &job->error); }
        catch (...) { job->words.clear(); job->error = "glslang threw"; }
        job->ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
        std::lock_guard<std::mutex> lock(compileMutex);
        compileDone.push_back(std::move(job));
        compileFinished.store(true, std::memory_order_release);
    }
}
void submit_compile(uint64_t spirvKey, bool vertex, const std::string& glsl) {
    static std::once_flag workers;
    std::call_once(workers, [] {
        for (int i = 0; i < 2; ++i) new std::thread(compile_worker);  // process lifetime, never joined
    });
    auto job = std::make_shared<CompileJob>();
    job->spirvKey = spirvKey; job->vertex = vertex; job->glsl = glsl;
    {
        std::lock_guard<std::mutex> lock(compileMutex);
        compileQueue.push_back(std::move(job));
    }
    compileReady.notify_one();
}
}  // namespace

DescriptorRankPlan make_descriptor_rank_plan(const LatteDecompilerShaderResourceMapping& mapping,
                                             const LatteDecompilerShader& shader) {
    DescriptorRankPlan plan;
    static_assert(17 + LATTE_NUM_MAX_TEX_UNITS < DescriptorRankPlan::unused);
    struct Binding { int value; uint8_t kind, slot; };
    std::array<Binding, 17 + LATTE_NUM_MAX_TEX_UNITS> bindings{};
    size_t count = 0;
    auto add = [&](int value, uint8_t kind, uint8_t slot) {
        if (value >= 0) bindings[count++] = {value, kind, slot};
    };
    for (uint8_t i = 0; i < 16; ++i) add(mapping.uniformBuffersBindingPoint[i], 0, i);
    add(mapping.uniformVarsBufferBindingPoint, 1, 0);
    for (uint8_t i = 0; i < LATTE_NUM_MAX_TEX_UNITS; ++i)
        add(mapping.textureUnitToBindingPoint[i], 2, i);
    // Unexpected duplicate texture writes retain the original preparation/sort.
    std::array<bool, LATTE_NUM_MAX_TEX_UNITS> seen{};
    if (shader.textureUnitListCount < 0 || shader.textureUnitListCount > LATTE_NUM_MAX_TEX_UNITS)
        return plan;
    for (int i = 0; i < shader.textureUnitListCount; ++i) {
        const auto unit = shader.textureUnitList[i];
        if (unit >= LATTE_NUM_MAX_TEX_UNITS) return plan;
        if (mapping.textureUnitToBindingPoint[unit] >= 0) {
            if (seen[unit]) return plan;
            seen[unit] = true;
        }
    }
    std::sort(bindings.begin(), bindings.begin() + count,
              [](const auto& a, const auto& b) { return a.value < b.value; });
    for (size_t rank = 0; rank < count; ++rank) {
        if (rank && bindings[rank-1].value == bindings[rank].value) return DescriptorRankPlan{};
        const auto& binding = bindings[rank];
        if (binding.kind == 0) {
            plan.blocks[binding.slot] = uint8_t(rank);
            plan.uniformRanks[plan.uniformCount++] = uint8_t(rank);
        } else if (binding.kind == 1) {
            plan.support = uint8_t(rank);
            plan.uniformRanks[plan.uniformCount++] = uint8_t(rank);
        }
        else plan.textures[binding.slot] = uint8_t(rank);
    }
    plan.count = uint8_t(count);
    plan.valid = true;
    return plan;
}

LatteFetchShader* get_fetch_shader(const uint32_t* regs, uint64_t* keyOut, uint64_t frame) {
    ++stats.fetchLookups;
    if (frame == ~uint64_t{0}) reset_draw_state_cache();
    if (keyOut) *keyOut = 0;
    static const bool memo = draw_option_enabled("WWHD_VK_FETCH_MEMO");
    const bool cacheLast = memo && frame != ~uint64_t{0};
    frame = program_validation(frame);
    if (!cacheLast) {
        lastFetch = {};
#ifdef __SWITCH__
        if (frame == ~uint64_t{0}) fetchRangeMemo = {};
#endif
    }
    uint32_t address = regs[mmSQ_PGM_START_FS] << 8;
    if (!address) return nullptr;
    const uint32_t programSize = regs[mmSQ_PGM_START_FS + 1] << 3;
    const std::array<uint32_t, 2> stepRates = {
        regs[REGADDR::VGT_INSTANCE_STEP_RATE_0], regs[REGADDR::VGT_INSTANCE_STEP_RATE_1]};
#ifdef __SWITCH__
    // The tracked epoch covers the compact header too, so a warm hit does not
    // touch guest bytes. Register size/divisors are checked even for compact FS.
    if (cacheLast && lastFetch.fetch && lastFetch.frame == frame &&
        lastFetch.address == address && lastFetch.programSize == programSize && lastFetch.stepRates == stepRates) {
        ++stats.fetchLastHits;
        if (keyOut) *keyOut = lastFetch.key;
        return lastFetch.fetch;
    }
    auto& rangeMemo = fetchRangeMemo[(address / 256 ^ address / 8192 ^ programSize) & 63];
    if (cacheLast && rangeMemo.fetch && rangeMemo.frame == frame &&
        rangeMemo.address == address && rangeMemo.programSize == programSize && rangeMemo.stepRates == stepRates) {
        lastFetch = rangeMemo;
        ++stats.fetchRangeHits;
        if (keyOut) *keyOut = rangeMemo.key;
        return rangeMemo.fetch;
    }
    gx2::shaderProgramWrites.watch(address, 16);
#endif
    bool compact = ld32(address) == 0x57574653;
    uint32_t count = compact ? ld32(address + 4) : 0;
    if (compact && count > 64) return nullptr;
    uint32_t size = compact ? 16 + count * 16 : programSize;
    if (!size || size > 0x1000 || uint64_t(address) + size > 0x100000000ull) return nullptr;
    // Raw fetch parsing resolves indexed instance divisors from these two
    // registers. Compact programs encode divisors in their own byte stream.
    // Desktop keeps fresh header reads and once-per-frame body validation.
    // Switch has already checked the epoch before reading the header on misses.
    if (cacheLast && lastFetch.fetch && lastFetch.frame == frame &&
        lastFetch.address == address && lastFetch.size == size &&
        lastFetch.compact == compact && lastFetch.count == count && lastFetch.stepRates == stepRates &&
        lastFetch.programSize == programSize) {
        ++stats.fetchLastHits;
        if (keyOut) *keyOut = lastFetch.key;
        return lastFetch.fetch;
    }
    uint64_t key = program_hash(address, size, frame, 2);
    if (!compact) key = hash_bytes(stepRates.data(), sizeof(stepRates), key);
    if (keyOut) *keyOut = key;
    auto remember = [&](LatteFetchShader* fetch) {
        if (cacheLast && fetch) {
            lastFetch = {frame, key, address, size, count, compact, stepRates, fetch, programSize};
#ifdef __SWITCH__
            rangeMemo = lastFetch;
#endif
        }
        return fetch;
    };
    if (auto it = fetchShaders.find(key); it != fetchShaders.end()) return remember(it->second);
    auto* fetch = compact ? gx2::build_fetch_shader(address)
        : LatteShaderRecompiler_createFetchShader(key, const_cast<uint32_t*>(regs),
                                                  reinterpret_cast<uint32_t*>(ppc_ptr(address)), size);
    fetchShaders.emplace(key, fetch);
    if (fetch) fetchKeys.emplace(fetch, key);
    return remember(fetch);
}

// Field by field (struct padding is not hashed): the GLSL and every decompiler output the renderer can
// read. Two hashes with different algorithms, so equal signatures mean equal outputs in practice.
std::array<uint64_t,2> output_signature(const Shader& shader, uint64_t fsKey) {
    uint64_t a=0x9E3779B97F4A7C15ull, b=14695981039346656037ull;
    auto add=[&](const void* p, size_t n) {
        a=hash_bytes(p,n,a);
        const auto* c=static_cast<const uint8_t*>(p);
        for(size_t i=0;i<n;++i) b=(b^c[i])*1099511628211ull;
        b=(b^n)*1099511628211ull;  // lengths separate adjacent fields
    };
    auto val=[&](auto v) { add(&v,sizeof v); };
    const LatteDecompilerShader& d=*shader.dec;
    val(shader.vertex); val(shader.vertex ? fsKey : 0);
    const XXH128_hash_t text=XXH3_128bits(shader.glsl.data(),shader.glsl.size());
    val(text.low64); val(text.high64); val(uint64_t(shader.glsl.size()));
    add(&shader.mapping,sizeof shader.mapping);    // sint8 fields only
    add(&shader.uniforms,sizeof shader.uniforms);  // sint32 fields only
    val(uint32_t(d.shaderType)); val(d.baseHash); val(d.hasError); val(d.isCustomShader);
    val(uint32_t(d.list_quickBufferList.size()));
    for(const auto& q:d.list_quickBufferList) { val(uint32_t(q.index)); val(uint32_t(q.size)); }
    val(d.textureUnitListCount);
    for(int i=0;i<d.textureUnitListCount;++i) {
        const uint8_t unit=d.textureUnitList[i];
        val(unit); val(d.textureRenderTargetIndex[unit]);
    }
    add(d.textureUnitDim,sizeof d.textureUnitDim); add(d.textureIsIntegerFormat,sizeof d.textureIsIntegerFormat);
    val(d.uniformMode);
    val(uint32_t(d.list_remappedUniformEntries.size()));
    for(const auto& e:d.list_remappedUniformEntries) { val(e.isRegister); val(e.kcacheBankId); val(e.index); val(e.mappedIndex); }
    val(uint64_t(d.textureUnitMask2.to_ullong()));
    add(d.textureUnitSamplerAssignment,sizeof d.textureUnitSamplerAssignment);
    add(d.textureUsesDepthCompare,sizeof d.textureUsesDepthCompare);
    val(d.pixelColorOutputMask); val(d.depthMask); val(d.ringParameterCount); val(d.ringParameterCountFromPrevStage);
    val(uint64_t(d.streamoutBufferWriteMask.to_ullong())); val(d.hasStreamoutBufferWrite); val(d.outputParameterMask);
    add(&d.resourceMapping,sizeof d.resourceMapping);
    const auto& u=d.uniform;
    val(u.loc_remapped); val(u.loc_uniformRegister); val(u.count_uniformRegister); val(u.loc_windowSpaceToClipSpaceTransform);
    val(u.loc_alphaTestRef); val(u.loc_pointSize); val(u.loc_fragCoordScale);
    add(u.loc_framebufferFetchSize,sizeof u.loc_framebufferFetchSize);
    val(uint32_t(u.list_ufTexRescale.size()));
    for(const auto& t:u.list_ufTexRescale) { val(t.texUnit); val(t.uniformLocation); }
    val(u.loc_verticesPerInstance); add(u.loc_streamoutBufferBase,sizeof u.loc_streamoutBufferBase); val(u.uniformRangeSize);
    val(uint32_t(d.list_remappedUniformEntries_register.size()));
    for(const auto& e:d.list_remappedUniformEntries_register) { val(e.indexOffset); val(e.mappedIndexOffset); }
    val(uint32_t(d.list_remappedUniformEntries_bufferGroups.size()));
    for(const auto& g:d.list_remappedUniformEntries_bufferGroups) {
        val(g.bufferId); val(g.kcacheBankIdOffset); val(uint32_t(g.entries.size()));
        for(const auto& e:g.entries) { val(e.indexOffset); val(e.mappedIndexOffset); }
    }
    val(uint32_t(d.m_shaderStateCacheKeys.size()));
    for(uint64_t k:d.m_shaderStateCacheKeys) val(k);
    return {a,b};
}

static Shader* translate_impl(const uint32_t* regs, bool vertex, LatteFetchShader* fetch, uint64_t fsKey,
                             uint64_t frame, uint64_t stateGeneration,
                             const DiskShader* expected = nullptr, uint64_t expectedKey = 0) {
    ++stats.lookups;
    auto& last = lastShaders[vertex ? 0 : 1];
    uint32_t primitive = regs[REGADDR::VGT_PRIMITIVE_TYPE] & 0x3F;
    bool cacheLast = frame != ~uint64_t{0} && stateGeneration != ~uint64_t{0};
    const uint64_t validation = program_validation(frame);
    uint32_t start = vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS;
    uint32_t address = regs[start] << 8, size = regs[start + 1] << 3;
    if (!cacheLast) { reset_draw_state_cache(); last = {}; stateMemo.reset(); pairState.regs = nullptr; }
    if (cacheLast && last.shader && last.regs == regs && last.frame == validation &&
        last.address == address && last.size == size &&
        last.generation == stateGeneration &&
        (!vertex || (last.fsKey == fsKey && last.fetch == fetch)) && last.primitive == primitive) {
        ++stats.lastHits;
        return last.shader;
    }
    auto remember = [&](Shader* shader) {
        if (cacheLast) last = {regs, fetch, validation, stateGeneration, fsKey, primitive, shader, address, size};
        return shader;
    };
    if (!address || !size || size > 0x100000 || uint64_t(address) + size > 0x100000000ull) return nullptr;
    uint64_t base = program_hash(address, size, validation, vertex ? 0 : 1) ^ (vertex ? 0x1111 : 0x2222);
    uint64_t key = state_hash(regs, base, vertex, cacheLast) ^ (vertex ? fsKey * 31 : 0);
    if(vertex&&mods::cemu::has_shaders())key^=uint64_t(regs[REGADDR::PA_CL_VTE_CNTL])*0x9E3779B97F4A7C15ull;
    // Replay never trusts the saved key as an insertion key.
    if(expected && (key!=expectedKey || vertex!=expected->vertex)) return nullptr;
    if (auto it = shaders.find(key); it != shaders.end()) {
        ++stats.variantHits;
        return remember(it->second);
    }
    // Async mode: at most WWHD_VK_TRANSLATE_BUDGET_MS (default 6) of new translations per frame. A new
    // area brings hundreds of variants at once (~0.3 ms each); the rest are translated in the next
    // frames and their draws wait, instead of one frame taking 100-200 ms.
    if (!expected && cacheLast && async_shaders()) {
        static const uint64_t budget = [] {
            const char* value = getenv("WWHD_VK_TRANSLATE_BUDGET_MS");
            return uint64_t((value ? atof(value) : 6.0) * 1e6);
        }();
        if (frame != translateFrame) { translateFrame = frame; translateNs = 0; }
        if (translateNs >= budget) { ++stats.deferredTranslations; return &deferredShader; }
    }
    auto started = std::chrono::steady_clock::now();
    struct CompileTimer {
        std::chrono::steady_clock::time_point start;
        ~CompileTimer() {
            translateNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
            ++stats.compiles;
            stats.compileNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count();
        }
    } timer{started};
    auto owned = std::make_unique<Shader>();
    struct OwnedDecompiler {
        std::unique_ptr<Shader>& owned;
        ~OwnedDecompiler() { if(owned) free_decompiler(owned->dec); }
    } cleanup{owned};
    Shader* shader = owned.get();
    shader->key = key; shader->vertex = vertex;
    // Normal lookups memoize errors as before. Replay retains only verified,
    // successful translations, so a rejected recipe cannot poison gameplay.
    if(!expected) { shaderStore.push_back(std::move(owned)); shaders.emplace(key, shader); remember(shader); }
    if (!g_renderer || g_renderer->GetType() != RendererAPI::Vulkan) {
        shader->error = "Vulkan renderer was not selected before shader translation"; return expected ? nullptr : shader;
    }
    if ((regs[Latte::REGADDR::VGT_GS_MODE] & 3) != 0) {
        shader->error = "geometry shaders are not implemented by this Vulkan backend"; return expected ? nullptr : shader;
    }
    if (vertex && !fetch) { shader->error = "vertex shader has no fetch program"; return expected ? nullptr : shader; }
    LatteShader_UpdatePSInputs(const_cast<uint32_t*>(regs));
    LatteDecompilerOptions options;
    uint64_t packBase=0;
    if(mods::cemu::has_shaders()) {
        packBase=cemu_pack_hash::base(ppc_ptr(address),size,regs,vertex,fetch);
        options.legacyGraphicPackUniforms=!vertex&&mods::cemu::legacy_pixel_uniforms(packBase);
    }
    const uint64_t decompilerBase=mods::cemu::has_shaders()?packBase:base;
    LatteDecompilerOutput_t output{};
    if (vertex) LatteDecompiler_DecompileVertexShader(decompilerBase, const_cast<uint32_t*>(regs),
        ppc_ptr(address), size, fetch, options, &output);
    else LatteDecompiler_DecompilePixelShader(decompilerBase, const_cast<uint32_t*>(regs),
        ppc_ptr(address), size, options, &output);
    if (!output.shader || output.shader->hasError || !output.shader->strBuf_shaderSource) {
        free_decompiler(output.shader); shader->error = "Latte GLSL translation failed"; return expected ? nullptr : shader;
    }
    shader->dec = FinishDecompiledShader(output);
    shader->mapping = output.resourceMappingVK;
    shader->descriptorRanks = make_descriptor_rank_plan(shader->mapping, *shader->dec);
    shader->uniforms = output.uniformOffsetsVK;
    shader->glsl = shader->dec->strBuf_shaderSource->c_str();
    if (vertex) {
        // Depth-only and shaded variants must rasterize identical positions.
        // Match Metal's [[invariant]] position output for multipass depth tests.
        auto main = shader->glsl.find("void main(");
        if (main != std::string::npos)
            shader->glsl.insert(main, "invariant gl_Position;\n");
    }
    if(mods::cemu::has_shaders()) {
        auto packAux=cemu_pack_hash::auxiliary(*shader->dec,regs,vertex);
        auto custom=mods::cemu::shader_source(packBase,packAux,vertex);
        if(!custom.empty()) {
            std::string error;
            auto replacement=compile_glsl(custom,vertex,&error);
            std::string originalError;
            auto original=compile_glsl(shader->glsl,vertex,&originalError);
            bool accepted=!replacement.empty()&&!original.empty()&&
                mods::cemu::compatible_shader_interface(original,replacement,error);
            if(accepted)shader->glsl=std::move(custom);
            if(error.empty()&&!accepted)error=originalError.empty()?"Shader compilation failed":originalError;
            mods::cemu::report_shader(packBase,packAux,vertex,accepted,error);
            fprintf(stderr,"[cemu-pack] %016llx_%016llx_%s %s%s\n",
                (unsigned long long)packBase,(unsigned long long)packAux,vertex?"vs":"ps",
                accepted?"applied":"rejected: ",accepted?"":error.c_str());
        }
    }
    if(expected && shader->glsl!=expected->source) return nullptr;
    stats.decompileNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-started).count();
    initialize_disk_cache();
    // Guest variants retain distinct metadata, but identical final GLSL shares
    // compilation even with persistence disabled. Exact source checks collisions.
    uint64_t spirvKey=spirv_key(shader->glsl.data(),shader->glsl.size(),vertex);
    auto cached=diskShaders.find(spirvKey);
    if(cached!=diskShaders.end() && cached->second.vertex==vertex && cached->second.source==shader->glsl) {
        shader->spirv=cached->second.words;
        if(cached->second.fromDisk) ++stats.diskHits;
        else ++stats.spirvReuseHits;
    } else {
        if(expected) return nullptr; // boot replay never falls back to glslang or mutates diskShaders
        if(async_shaders()) {
            // one compile per GLSL: every variant translated to it while it runs waits for it
            auto& pending=pendingCompiles[spirvKey];
            if(pending.shaders.empty()) { pending.glsl=shader->glsl; submit_compile(spirvKey,vertex,shader->glsl); }
            if(pending.glsl==shader->glsl) {  // else a checksum collision: compile here as before
                PendingShader waiting{shader,fsKey,frame,false,{}};
                if(!diskPath.empty() && !diskVariants.contains(key) && diskVariants.size()<maxCacheVariants)
                    waiting.captured=capture_inputs(regs,*shader,fetch,fsKey,waiting.input);
                pending.shaders.push_back(std::move(waiting));
                ++pendingShaders;
                shader->pending=true;
                // the GLSL stays for completion (signature, cache entry); the decompiler's copy goes
                if (shader->dec) { delete shader->dec->strBuf_shaderSource; shader->dec->strBuf_shaderSource = nullptr; }
                return shader;
            }
        }
        auto compileStart=std::chrono::steady_clock::now();
        shader->spirv = std::make_shared<const std::vector<uint32_t>>(compile_glsl(shader->glsl, vertex, &shader->error));
        ++stats.spirvCompiles;
        stats.spirvCompileNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-compileStart).count();
        if(shader->glsl.size()<=maxSourceBytes &&
           cache_spirv_valid(*shader->spirv) && diskShaders.size()<maxCacheEntries) {
            size_t oldBytes=cached==diskShaders.end() ? 0 : entry_bytes(cached->second);
            size_t newBytes=24+shader->glsl.size()+shader->spirv->size()*4;
            if(newBytes<=maxCacheBytes-(diskBytes-oldBytes)) {
                if(cached!=diskShaders.end())
                    for(const auto& input:cached->second.variants) diskVariants.erase(input.key);
                auto [inserted,_]=diskShaders.insert_or_assign(spirvKey,DiskShader{shader->glsl,shader->spirv,vertex,false});
                journal_entry(spirvKey,inserted->second);
                diskBytes=diskBytes-oldBytes+newBytes;
                ++diskRevision;
                diskDirty=!diskPath.empty();
                diskChangedFrame=frame;
            }
        }
    }
    // An earlier translation with the same output serves this variant too (and keeps its Shader::key,
    // so pipelines are shared).
    Shader* same=nullptr;
    if(shader->ready()) {
        shader->output=output_signature(*shader,fsKey);
        auto [it,inserted]=shaderOutputs.try_emplace(shader->output,shader);
        if(!inserted) same=it->second;
    }
    if(expected) {
        if(!shader->ready()) return nullptr;
        if(same) { shaders.emplace(key,same); return same; }  // `owned` and its decompiler are freed
        shaders.emplace(key,shader); shaderStore.push_back(std::move(owned));
    } else if(shader->ready()) {
        // Identical GLSL can have many distinct translation keys/metadata.
        auto entry=diskShaders.find(spirvKey);
        if(entry!=diskShaders.end() && entry->second.vertex==vertex && entry->second.source==shader->glsl) {
            if(same && has_variant(entry->second,same->key))
                record_alias(spirvKey,entry->second,key,same->key,frame);
            else
                record_translation(spirvKey,entry->second,regs,*shader,fetch,fsKey,frame);
        }
        if(same) {
            shaders[key]=same;
            remember(same);
            if(!shaderStore.empty() && shaderStore.back().get()==shader) {
                free_decompiler(shader->dec);
                shaderStore.pop_back();
            }
            return same;
        }
    }
    if (!shader->error.empty()) fprintf(stderr, "[vulkan] shader %08X: %s\n", address, shader->error.c_str());
    // The GLSL text is only needed during translation (cache lookup/recording): free both copies
    // (thousands of variants stay resident after the boot warm-up).
    std::string().swap(shader->glsl);
    if (shader->dec) { delete shader->dec->strBuf_shaderSource; shader->dec->strBuf_shaderSource = nullptr; }
    return shader;
}

void poll_shader_compiles(uint64_t frame) {
    if(!compileFinished.load(std::memory_order_acquire)) return;
    std::deque<std::shared_ptr<CompileJob>> done;
    {
        std::lock_guard<std::mutex> lock(compileMutex);
        done.swap(compileDone);
        compileFinished.store(false, std::memory_order_relaxed);
    }
    for(auto& job:done) {
        auto it=pendingCompiles.find(job->spirvKey);
        if(it==pendingCompiles.end()) continue;
        ++stats.spirvCompiles; stats.spirvCompileNs+=job->ns;
        auto words=std::make_shared<const std::vector<uint32_t>>(std::move(job->words));
        // the cache entry, as the synchronous path makes it
        DiskShader* entry=nullptr;
        if(job->error.empty() && job->glsl.size()<=maxSourceBytes && cache_spirv_valid(*words) &&
           diskShaders.size()<maxCacheEntries && !diskShaders.count(job->spirvKey)) {
            const size_t newBytes=28+job->glsl.size()+words->size()*4;
            if(newBytes<=maxCacheBytes-diskBytes) {
                auto [inserted,_]=diskShaders.insert_or_assign(job->spirvKey,DiskShader{job->glsl,words,job->vertex,false});
                journal_entry(job->spirvKey,inserted->second);
                diskBytes+=newBytes;
                ++diskRevision; diskDirty=!diskPath.empty(); diskChangedFrame=frame;
                entry=&inserted->second;
            }
        }
        for(auto& waiting:it->second.shaders) {
            Shader* shader=waiting.shader;
            shader->spirv=words;
            shader->error=job->error;
            shader->pending=false;
            --pendingShaders;
            if(shader->ready()) {
                // Draws already hold this Shader: it stays itself even when its output equals another's.
                shader->output=output_signature(*shader,waiting.fsKey);
                auto [same,inserted]=shaderOutputs.try_emplace(shader->output,shader);
                if(entry && waiting.captured) {
                    if(!inserted && has_variant(*entry,same->second->key))
                        record_alias(job->spirvKey,*entry,shader->key,same->second->key,frame);
                    else store_inputs(job->spirvKey,*entry,std::move(waiting.input),frame);
                }
            } else fprintf(stderr,"[vulkan] shader %016llX: %s\n",(unsigned long long)shader->key,shader->error.c_str());
            std::string().swap(shader->glsl);
        }
        pendingCompiles.erase(it);
    }
}
size_t pending_shader_compiles() { return pendingShaders; }

Shader* translate(const uint32_t* regs, bool vertex, LatteFetchShader* fetch, uint64_t fsKey,
                  uint64_t frame, uint64_t stateGeneration) {
    return translate_impl(regs,vertex,fetch,fsKey,frame,stateGeneration);
}

ShaderPair translate_pair(const uint32_t* regs, LatteFetchShader* fetch, uint64_t fsKey,
                          uint64_t frame, uint64_t stateGeneration, bool tracked) {
    static const bool enabled = draw_option_enabled("WWHD_VK_SHADER_PAIR_MEMO");
    if (!enabled || frame == ~uint64_t{0} || stateGeneration == ~uint64_t{0})
        return {translate(regs,true,fetch,fsKey,frame,stateGeneration),
                translate(regs,false,fetch,fsKey,frame,stateGeneration)};
    ++stats.pairLookups;
    const uint32_t vsAddress = regs[mmSQ_PGM_START_VS] << 8, vsSize = regs[mmSQ_PGM_START_VS+1] << 3;
    const uint32_t psAddress = regs[mmSQ_PGM_START_PS] << 8, psSize = regs[mmSQ_PGM_START_PS+1] << 3;
    auto valid = [](uint32_t address, uint32_t size) {
        return address && size && size <= maxProgramBytes && uint64_t(address)+size <= 0x100000000ull;
    };
    if (!valid(vsAddress,vsSize) || !valid(psAddress,psSize))
        return {translate(regs,true,fetch,fsKey,frame,stateGeneration),
                translate(regs,false,fetch,fsKey,frame,stateGeneration)};
    const uint64_t validation = program_validation(frame);
    tracked = tracked && regs == gx2::regs();
    const uint32_t primitive = regs[REGADDR::VGT_PRIMITIVE_TYPE]&0x3F;
    if (tracked && pairState.regs == regs && pairState.generation == gx2::drawStateGenerations.translation &&
        pairState.primitive == primitive) {
        ++stats.pairStateHits;
    } else {
        std::array<uint32_t,105+7*LATTE_NUM_MAX_TEX_UNITS> words;
        size_t count = translation_state<false>(const_cast<uint32_t*>(regs),words,true);
        for (uint32_t t=0; t<LATTE_NUM_MAX_TEX_UNITS; ++t) {
            const auto* w = regs + REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS + t*7;
            words[count++] = (w[0]&7) | (w[4]&0x300);
            words[count++] = w[1]&0x03F00000;
        }
        // Sparse, indexed nonzero words are an exact serialization: omitted
        // words are zero and the count distinguishes optional streamout words.
        std::array<uint64_t,1+105+7*LATTE_NUM_MAX_TEX_UNITS> key;
        size_t keyCount=0; key[keyCount++]=count;
        for (size_t i=0; i<count; ++i)
            if (words[i]) key[keyCount++]=(uint64_t(i)<<32)|words[i];
        const std::span<const uint64_t> stateKey{key.data(),keyCount};
        uint64_t id=pairStates.find(stateKey);
        if (!id) {
            if (nextPairStateId >= 2048) {
                pairStates.clear(); pairMemo.clear(); rangePairs.fill({}); pairMemoCount=0; nextPairStateId=0;
            }
            id=++nextPairStateId; pairStates.insert(stateKey,id);
        }
        pairState.id=id;
        pairState.regs=tracked ? regs : nullptr;
        pairState.generation=gx2::drawStateGenerations.translation; pairState.primitive=primitive;
    }
    // Direct-mapped memo by program ranges: while the validation epoch is unchanged, no watched program
    // byte changed (revalidate_programs bumps it), so equal ranges hold equal programs and the pair is
    // the same. Skips both program_hash lookups and the content-keyed memo on most calls.
    const uint64_t vsRange = (uint64_t(vsAddress) << 32) | vsSize, psRange = (uint64_t(psAddress) << 32) | psSize;
    const uintptr_t fetchId = reinterpret_cast<uintptr_t>(fetch);
    auto& direct = rangePairs[(vsRange * 0x9E3779B97F4A7C15ull ^ psRange * 0xC2B2AE3D27D4EB4Full ^ pairState.id * 31 ^ fsKey) >> 54];
    if (direct.epoch == validation && direct.vs == vsRange && direct.ps == psRange && direct.state == pairState.id &&
        direct.fsKey == fsKey && direct.fetch == fetchId && direct.pair.vs) {
        ++stats.pairHits;
        return direct.pair;
    }
    const uint64_t vsProgram = program_hash(vsAddress,vsSize,validation,0);
    const uint64_t psProgram = program_hash(psAddress,psSize,validation,1);
    auto rememberRange = [&](const ShaderPair& pair) {
        if (program_validation(frame) == validation) direct = {vsRange, psRange, pairState.id, fsKey, validation, fetchId, pair};
    };
    const std::array<uint64_t,5> identity{pairState.id,vsProgram,psProgram,fsKey,
                                       uint64_t(reinterpret_cast<uintptr_t>(fetch))};
    // The state ID was interned with exact word equality; program content and
    // fetch identity retain the established validation/key rules.
    if (auto pair=pairMemo.find(identity); pair.vs && program_validation(frame)==validation) {
        ++stats.pairHits;
        rememberRange(pair);
        return pair;
    }
    ShaderPair pair{translate(regs,true,fetch,fsKey,frame,stateGeneration),
                    translate(regs,false,fetch,fsKey,frame,stateGeneration)};
    if (pair.vs && pair.ps && pair.vs->ready() && pair.ps->ready() && program_validation(frame)==validation) {
        if (pairMemoCount >= 8192) { pairMemo.clear(); rangePairs.fill({}); pairMemoCount=0; }
        pairMemo.insert(identity,pair); ++pairMemoCount;
        rememberRange(pair);
    }
    return pair;
}

void warm_up_shader_cache() {
    // main.cpp: device and guest memory exist, but no game/render thread runs yet.
    static bool attempted=false;
    if(attempted) return;
    attempted=true;
    auto started=std::chrono::steady_clock::now();
    {   // temporary files of cache saves cut off by closing the game (each up to the cache size)
        std::error_code ec;
        for(const auto& dir:{host::config_dir()+"/shadercache",host::config_dir()+"/shadercache/vulkan-shaders"})
            for(std::filesystem::directory_iterator it(dir,ec),end;!ec && it!=end;it.increment(ec))
                if(it->path().filename().string().find(".tmp")!=std::string::npos) {
                    std::error_code removeError;
                    std::filesystem::remove(it->path(),removeError);
                }
    }
    initialize_disk_cache();
    // (an old-format cache is rewritten after the warm-up instead, with its variants folded into aliases)
    if(!oldFormat && (journalBytes>compactJournalBytes || (journalBroken && !diskShaders.empty()))) {
        // boot, no game thread yet: fold the journal into spirv.bin (a full rewrite, seconds on an SD card)
        auto compactStart=std::chrono::steady_clock::now();
        const size_t folded=journalBytes;
        diskDirty=true;
        const bool saved=save_shader_cache();
        fprintf(stderr,"[vulkan] shader cache compaction: %.1f MiB journal into spirv.bin %s, %.0f ms\n",
                folded/1048576.0,saved ? "done" : "FAILED",
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-compactStart).count());
    }
    const char* enabled=getenv("WWHD_VK_SHADER_WARMUP");
    if(enabled && !strcmp(enabled,"0")) return;
    constexpr auto budget=std::chrono::seconds(30);
    // Admission: measured heap growth of the warm-up (WWHD_VK_WARMUP_MB, default 256 MiB). The old
    // per-variant estimate was ~6x pessimistic (152 MiB estimated for 26 MiB measured, 13.5k variants).
    const size_t admissionBytes=[] {
        const char* e=getenv("WWHD_VK_WARMUP_MB");
        return (e ? size_t(atoi(e)) : size_t(256))*1024u*1024u;
    }();
    const size_t heapBefore=mallinfo().uordblks;
    size_t warmed=0, skipped=0, visited=0, aliased=0, recorded=0;
    for(const auto& [spirvKey,entry]:diskShaders) recorded+=entry.variants.size();
    size_t retainedBytes=0;
    bool timedOut=false, memoryLimited=false;
    try {
        if(!diskVariants.empty()) {
            // Bump allocator cannot free: allocate once, reuse for every recipe.
            // Standalone hashing (~0 frame) always rereads these overwritten bytes.
            static uint32_t scratch=mem::host_alloc(maxProgramBytes+maxFetchBytes,256);
            std::vector<uint32_t> regs(gx2::kNumRegs);
            for(const auto& [spirvKey,entry]:diskShaders) {
                for(const auto& input:entry.variants) {
                    if(std::chrono::steady_clock::now()-started>=budget) { timedOut=true; break; }
                    // Admission estimate per retained variant: SPIR-V is shared with the
                    // disk entry and the GLSL text is freed after translation; metadata
                    // (remapped-uniform vectors, decompiler output, parsed fetch data) remains.
                    size_t bytes=8*input.program.size()+
                        sizeof(Shader)+sizeof(LatteDecompilerShader)+4*input.fetch.size()+4096;
                    if(visited%256==0 && size_t(mallinfo().uordblks)>heapBefore+admissionBytes) memoryLimited=true;
                    if(memoryLimited) { ++visited; ++skipped; continue; }
                    retainedBytes+=bytes;
                    ++visited;
                    if(!program_inputs_complete(input.program) ||
                       (entry.vertex && !fetch_inputs_complete(input.fetch))) { ++skipped; continue; }
                    std::fill(regs.begin(),regs.end(),0);
                    auto state=input.state;
                    translation_state<true>(regs.data(),state,entry.vertex);
                    auto annotations=input.annotations;
                    translation_annotations<true>(regs.data(),annotations,entry.vertex);
                    uint32_t start=entry.vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS;
                    regs[start]=scratch>>8;
                    regs[start+1]=uint32_t(input.program.size())>>3;
                    memcpy(ppc_ptr(scratch),input.program.data(),input.program.size());
                    uint64_t fsKey=0;
                    LatteFetchShader* fetch=nullptr;
                    if(entry.vertex) {
                        uint32_t fsAddress=scratch+maxProgramBytes;
                        regs[mmSQ_PGM_START_FS]=fsAddress>>8;
                        regs[mmSQ_PGM_START_FS+1]=input.fetchSizeReg;
                        regs[REGADDR::VGT_INSTANCE_STEP_RATE_0]=input.stepRates[0];
                        regs[REGADDR::VGT_INSTANCE_STEP_RATE_1]=input.stepRates[1];
                        memcpy(ppc_ptr(fsAddress),input.fetch.data(),input.fetch.size());
                        fetch=get_fetch_shader(regs.data(),&fsKey);
                        if(!fetch) { ++skipped; continue; }
                    }
                    auto* shader=translate_impl(regs.data(),entry.vertex,fetch,fsKey,
                                               ~uint64_t{0},~uint64_t{0},&entry,input.key);
                    if(shader && shader->ready() && find_shader(input.key)==shader) ++warmed;
                    else ++skipped;
                }
                if(timedOut) break;
                for(const auto& [alias,target]:entry.aliases) {
                    Shader* shader=find_shader(target);
                    if(shader && shaders.emplace(alias,shader).second) ++aliased;
                }
            }
        }
    } catch(...) { ++skipped; }
    // Retain content-keyed Shader/fetch objects, never scratch-address stamps.
    // Real programs still call program_hash/watch at their own addresses; a DC
    // store rehashes them normally and a changed program selects a different key.
    reset_shader_memoization();
    double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    fprintf(stderr,"[vulkan] shader warm-up: %zu/%zu warmed, %zu skipped, %zu unvisited, %.1f ms%s%s; "
            "estimate %zu MiB, heap %+lld MiB (%zu MiB in use)\n",
            warmed,recorded,skipped,recorded-visited,ms,
            timedOut ? " (30 s budget)" : "",memoryLimited ? " (admission budget)" : "",
            retainedBytes>>20,(long long)((long long)mallinfo().uordblks-(long long)heapBefore)/(1<<20),size_t(mallinfo().uordblks)>>20);
    // Variants recorded in full whose translation equals another recorded variant's (older caches,
    // or two variants first seen in one session) become aliases of it.
    size_t folded=0;
    if(!timedOut && !memoryLimited) {
        for(auto& [spirvKey,entry]:diskShaders) {
            std::unordered_map<uint64_t,uint64_t> target;  // folded variant -> the variant it equals
            for(const auto& input:entry.variants) {
                const Shader* shader=find_shader(input.key);
                if(shader && shader->key!=input.key && has_variant(entry,shader->key)) target[input.key]=shader->key;
            }
            if(target.empty()) continue;
            std::vector<TranslationInputs> kept;
            for(auto& input:entry.variants) {
                if(!target.count(input.key)) { kept.push_back(std::move(input)); continue; }
                entry.aliases.push_back({input.key,target[input.key]});
                diskBytes-=input_bytes(input)-16;
                ++folded;
            }
            entry.variants=std::move(kept);
            // a target is a variant translated with its own key, so it is never folded; earlier aliases of a
            // folded variant follow it to its target
            for(auto& alias:entry.aliases)
                if(auto t=target.find(alias.second); t!=target.end()) alias.second=t->second;
        }
    }
    if(folded || oldFormat) {
        auto saveStart=std::chrono::steady_clock::now();
        diskDirty=true;
        const bool saved=save_shader_cache();
        fprintf(stderr,"[vulkan] shader cache: %zu variants folded into aliases, rewritten %s (%.1f MiB), %.0f ms\n",
                folded,saved ? "done" : "FAILED",diskBytes/1048576.0,
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-saveStart).count());
    }
    fprintf(stderr,"[vulkan] shader warm-up: %zu aliases, %zu distinct translations\n",aliased,shaderStore.size());
}

Shader* find_shader(uint64_t key) {
    auto it=shaders.find(key);
    return it!=shaders.end() && it->second && it->second->ready() ? it->second : nullptr;
}
LatteFetchShader* find_fetch_shader(uint64_t fsKey) {
    auto it=fetchShaders.find(fsKey);
    return it!=fetchShaders.end() ? it->second : nullptr;
}
bool fetch_shader_key(const LatteFetchShader* fetch, uint64_t& fsKey) {
    auto it=fetchKeys.find(fetch);
    if(it==fetchKeys.end()) return false;
    fsKey=it->second;
    return true;
}

namespace {
using DiskSnapshot = std::vector<std::pair<uint64_t,DiskShader>>;
// journal: an append (bytes appended; fresh: the file was started anew) or a full save (base: the new
// spirv.bin's id, the journal is gone)
struct DiskSaveResult { uint64_t revision=0, bytes=0, ns=0, base=0; bool saved=false, journal=false, fresh=false; };
std::future<DiskSaveResult> diskSaveJob;
DiskSnapshot snapshot_disk_shaders() {
    auto start=std::chrono::steady_clock::now();
    DiskSnapshot snapshot;
    snapshot.reserve(diskShaders.size());
    for(const auto& entry:diskShaders) snapshot.push_back(entry);
    stats.diskSnapshotNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now()-start).count();
    return snapshot;
}
// Worker receives only immutable owned data; no shader maps, metadata, stats,
// Vulkan objects, or renderer state are accessed from this thread.
DiskSaveResult write_shader_snapshot(DiskSnapshot snapshot,std::string path,std::string journal,uint64_t revision) {
    auto start=std::chrono::steady_clock::now();
    DiskSaveResult result; result.revision=revision;
    auto finish=[&] {
        result.ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-start).count();
        return result;
    };
    std::string temporary;
    try {
        std::vector<uint8_t> payload;
        size_t expected=0;
        for(const auto& [key,entry]:snapshot) {
            size_t bytes=entry_bytes(entry);
            if(bytes>maxCacheBytes-40-expected) return finish();
            expected+=bytes;
        }
        payload.reserve(expected);
        for(const auto& [key,entry]:snapshot) {
            cache_put(payload,key,8); cache_put(payload,entry.vertex,4);
            cache_put(payload,entry.source.size(),4); cache_put(payload,entry.words->size(),4);
            cache_put(payload,entry.variants.size(),4);
            payload.insert(payload.end(),entry.source.begin(),entry.source.end());
            for(uint32_t word:*entry.words) cache_put(payload,word,4);
            for(const auto& input:entry.variants) put_variant(payload,input);
            cache_put(payload,entry.aliases.size(),4);
            for(const auto& [alias,target]:entry.aliases) { cache_put(payload,alias,8); cache_put(payload,target,8); }
        }
        const size_t count=snapshot.size();
        DiskSnapshot().swap(snapshot);  // the copy is serialized: give its memory back before writing
        result.base=cache_checksum(payload.data(),payload.size());
        std::vector<uint8_t> header={'W','W','V','K','S','C','0','4'};
        cache_put(header,cache_fingerprint(),8); cache_put(header,payload.size(),8);
        cache_put(header,result.base,8);
        cache_put(header,count,8);
        std::error_code ec;
        auto parent=std::filesystem::path(path).parent_path();
        if(!parent.empty()) std::filesystem::create_directories(parent,ec);
        if(ec) return finish();
        temporary=path+".tmp-"+std::to_string(start.time_since_epoch().count());
        FILE* file=fopen(temporary.c_str(),"wb");
        if(!file) return finish();
        bool wrote=fwrite(header.data(),1,header.size(),file)==header.size() &&
                   fwrite(payload.data(),1,payload.size(),file)==payload.size();
        if(fclose(file)!=0) wrote=false;
        result.saved=wrote && host::replace_file(temporary,path);
        if(result.saved) {
            result.bytes=header.size()+payload.size();
            // everything in the journal is in the new spirv.bin (its base id no longer matches anyway)
            if(!journal.empty()) std::filesystem::remove(journal,ec);
        }
    } catch(...) { result.saved=false; }
    if(!result.saved && !temporary.empty()) { std::error_code ec; std::filesystem::remove(temporary,ec); }
    return finish();
}
// Appends one block of records to the journal; a journal for another base (or none) is started anew.
DiskSaveResult append_journal(std::vector<uint8_t> payload,std::string path,uint64_t base,uint64_t revision) {
    auto start=std::chrono::steady_clock::now();
    DiskSaveResult result; result.revision=revision; result.journal=true;
    try {
        std::vector<uint8_t> header={'W','W','V','K','S','J','0','2'};
        cache_put(header,cache_fingerprint(),8); cache_put(header,base,8);
        bool fresh=true;
        if(FILE* existing=fopen(path.c_str(),"rb")) {
            uint8_t head[journalHeaderBytes];
            fresh=fread(head,1,sizeof head,existing)!=sizeof head || memcmp(head,header.data(),sizeof head);
            fclose(existing);
        }
        std::vector<uint8_t> block;
        if(fresh) block=header;
        cache_put(block,payload.size(),8); cache_put(block,cache_checksum(payload.data(),payload.size()),8);
        if(FILE* file=fopen(path.c_str(),fresh ? "wb" : "ab")) {
            bool wrote=fwrite(block.data(),1,block.size(),file)==block.size() &&
                       fwrite(payload.data(),1,payload.size(),file)==payload.size();
            if(fclose(file)!=0) wrote=false;
            result.saved=wrote; result.fresh=fresh;
            result.bytes=block.size()+payload.size();
        }
    } catch(...) { result.saved=false; }
    result.ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
    return result;
}
bool finish_disk_save(bool wait) {
    if(!diskSaveJob.valid() || (!wait && diskSaveJob.wait_for(std::chrono::seconds(0))!=std::future_status::ready))
        return false;
    try {
        auto result=diskSaveJob.get();
        stats.diskSaveNs+=result.ns;
        if(result.saved) {
            ++stats.diskSaves; stats.diskSavedBytes=result.bytes;
            if(result.journal) journalBytes=(result.fresh ? 0 : journalBytes)+result.bytes;
            else { journalBase=result.base; journalBroken=false; journalBytes=0; }
            fprintf(stderr,"[vulkan cache] shader %s: %.1f KiB in %.0f ms (journal %.1f MiB)\n",
                    result.journal ? "journal append" : "full save",result.bytes/1024.0,result.ns/1e6,
                    journalBytes/1048576.0);
            diskDirty=diskRevision!=result.revision;
        } else {
            // the records of a failed save are only in memory now (a failed append may also have left
            // a torn block that would hide later ones): save everything next time
            diskDirty=true; journalBroken=true;
        }
        return result.saved;
    } catch(...) { diskDirty=true; journalBroken=true; return false; }
}
} // namespace
bool shader_cache_dirty() { return diskDirty; }
uint64_t shader_cache_changed_frame() { return diskChangedFrame; }
bool save_shader_cache() {
    // Explicit tools/shutdown contract is synchronous: join the old immutable
    // snapshot, then save any revisions compiled while that job was running.
    // A full save: spirv.bin with everything, no journal.
    bool saved=finish_disk_save(true);
    if(!diskDirty || diskPath.empty()) return saved;
    try {
        auto snapshot=snapshot_disk_shaders();
        journalPending.clear();  // in the snapshot
        auto result=write_shader_snapshot(std::move(snapshot),diskPath,journalPath,diskRevision);
        stats.diskSaveNs+=result.ns;
        if(result.saved) {
            diskDirty=false; ++stats.diskSaves; stats.diskSavedBytes=result.bytes;
            journalBase=result.base; journalBroken=false; journalBytes=0;
        } else journalBroken=true;
        return result.saved;
    } catch(...) { journalBroken=true; return false; }
}
void checkpoint_shader_cache(uint64_t frame) {
    finish_disk_save(false);
    if(diskSaveJob.valid() || !diskDirty || diskPath.empty() || diskChangedFrame==~uint64_t{0} ||
       frame<diskChangedFrame || frame-diskChangedFrame<120 ||
       frame<diskAttemptFrame || frame-diskAttemptFrame<120) return;
    diskAttemptFrame=frame;
    try {
        // the writers run as background threads: only in time the game and render threads leave idle
        if(!journalBroken) {
            if(journalPending.empty()) { diskDirty=false; return; }
            std::vector<uint8_t> payload;
            payload.swap(journalPending);
            diskSaveJob=std::async(std::launch::async,[payload=std::move(payload),path=journalPath,
                                                       base=journalBase,revision=diskRevision]() mutable {
                host::background_thread();
                return append_journal(std::move(payload),std::move(path),base,revision);
            });
            return;
        }
        auto snapshot=snapshot_disk_shaders();
        journalPending.clear();  // in the snapshot
        diskSaveJob=std::async(std::launch::async,[snapshot=std::move(snapshot),path=diskPath,journal=journalPath,
                                                   revision=diskRevision]() mutable {
            host::background_thread();
            return write_shader_snapshot(std::move(snapshot),std::move(path),std::move(journal),revision);
        });
    } catch(...) { journalBroken=true; /* Keep dirty for a bounded retry; cache failure is nonfatal. */ }
}

ShaderStats shader_stats() {
    auto result=stats;
    result.pairStates=nextPairStateId; result.pairEntries=pairMemoCount;
    result.pairCacheBytes=pairStates.bytes()+pairMemo.bytes();
    return result;
}

size_t support_uniform_size(const Shader& shader) {
    return size_t((std::max(shader.uniforms.offset_endOfBlock, 16) + 15) & ~15);
}
void pack_uniforms_into(const uint32_t* regs, const Shader& shader,
                        std::vector<uint8_t>& data, float scaleX, float scaleY) {
    data.resize(support_uniform_size(shader));
    pack_uniforms_raw(regs, shader, data.data(), scaleX, scaleY);
}
namespace {
void build_pack_plan(const Shader& shader, size_t total) {
    auto& plan = shader.packPlan;
    plan.built = true;
    const auto& offsets = shader.uniforms;
    auto add = [&](std::vector<UniformPackPlan::Run>& runs, int dst, uint32_t src, uint32_t bytes) {
        if (dst < 0 || size_t(dst) + bytes > total) return;
        if (!runs.empty()) {
            auto& last = runs.back();
            if (last.dst + last.bytes == uint32_t(dst) && last.src + last.bytes == src) { last.bytes += bytes; return; }
        }
        runs.push_back({uint32_t(dst), src, bytes});
    };
    if (offsets.offset_remapped >= 0) {
        for (const auto& entry : shader.dec->list_remappedUniformEntries_register)
            add(plan.regs, offsets.offset_remapped + entry.mappedIndexOffset, uint32_t(entry.indexOffset), 16);
        for (const auto& group : shader.dec->list_remappedUniformEntries_bufferGroups) {
            UniformPackPlan::Group g{uint32_t(group.kcacheBankIdOffset / 4), {}};
            for (const auto& entry : group.entries)
                add(g.runs, offsets.offset_remapped + entry.mappedIndexOffset, uint32_t(entry.indexOffset), 16);
            plan.groups.push_back(std::move(g));
        }
    }
    if (offsets.offset_uniformRegister >= 0)
        add(plan.regs, offsets.offset_uniformRegister, 0, uint32_t(offsets.count_uniformRegister) * 16);
    for (int unit = 0; unit < LATTE_NUM_MAX_TEX_UNITS; ++unit)
        if (offsets.offset_texScale[unit] >= 0 && size_t(offsets.offset_texScale[unit]) + 8 <= total)
            plan.texScales.push_back(offsets.offset_texScale[unit]);
}
}  // namespace
void pack_uniforms_raw(const uint32_t* regs, const Shader& shader, uint8_t* out,
                       float scaleX, float scaleY) {
    const auto& offsets = shader.uniforms;
    const size_t total = support_uniform_size(shader);
    // Reused memory may contain another shader's payload. Define every byte,
    // including gaps, padding, and remapped blocks with missing guest addresses.
    std::memset(out, 0, total);
    if (!shader.dec) return;
    if (!shader.packPlan.built) build_pack_plan(shader, total);
    const auto& plan = shader.packPlan;
    auto copy = [&](int offset, const void* src, size_t size) {
        if (offset >= 0 && size_t(offset) + size <= total) memcpy(out + offset, src, size);
    };
    auto put = [&](int offset, float value) { copy(offset, &value, sizeof value); };
    auto bitsf = [](uint32_t value) { float result; memcpy(&result, &value, 4); return result; };
    const uint8_t* alu = reinterpret_cast<const uint8_t*>(regs + mmSQ_ALU_CONSTANT0_0 + (shader.vertex ? 0x400 : 0));
    uint32_t blockBase = shader.vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
    for (const auto& run : plan.regs) memcpy(out + run.dst, alu + run.src, run.bytes);
    for (const auto& group : plan.groups) {
        uint32_t address = regs[blockBase + group.bankWord];
        if (!address) continue;
        for (const auto& run : group.runs) memcpy(out + run.dst, ppc_ptr(address + run.src), run.bytes);
    }
    put(offsets.offset_alphaTestRef, bitsf(regs[Latte::REGADDR::SX_ALPHA_REF]));
    float point = float(regs[Latte::REGADDR::PA_SU_POINT_SIZE] & 0xFFFF) / 8.0f;
    put(offsets.offset_pointSize, (point == 0 ? 0.125f : point) * scaleX);
    if (offsets.offset_windowSpaceToClipSpaceTransform >= 0) {
        float width = 2.0f * bitsf(regs[Latte::REGADDR::PA_CL_VPORT_XSCALE]);
        float height = -2.0f * bitsf(regs[Latte::REGADDR::PA_CL_VPORT_YSCALE]);
        put(offsets.offset_windowSpaceToClipSpaceTransform, width != 0 ? 2.0f / width : 0);
        put(offsets.offset_windowSpaceToClipSpaceTransform + 4, height != 0 ? 2.0f / height : 0);
    }
    if (offsets.offset_fragCoordScale >= 0) {
        float scale[] = {scaleX != 0 ? 1.0f / scaleX : 1.0f, scaleY != 0 ? 1.0f / scaleY : 1.0f, 0, 0};
        copy(offsets.offset_fragCoordScale, scale, sizeof scale);
    }
    static const float one[2] = {1, 1};  // surfaces currently preserve the guest texture dimensions
    for (int offset : plan.texScales) memcpy(out + offset, one, sizeof one);
}

std::vector<uint8_t> pack_uniforms(const uint32_t* regs, const Shader& shader,
                                   float scaleX, float scaleY) {
    std::vector<uint8_t> data;
    pack_uniforms_into(regs, shader, data, scaleX, scaleY);
    return data;
}

void reset_shader_memoization() {
    reset_draw_state_cache();
    stateMemo.reset();
    lastFetch = {};
    pairMemo.clear(); rangePairs.fill({}); pairStates.clear(); nextPairStateId=0; pairMemoCount=0;
    pairState.regs = nullptr;
#ifdef __SWITCH__
    fetchRangeMemo = {};
#endif
    programHashes.clear();
#ifdef __SWITCH__
    programBytes.clear();
    programsByPage.clear();
#endif
    directProgramHashes = {};
    lastProgramHashes = {};
    lastProgramFrame = ~uint64_t{0};
    lastShaders[0] = {}; lastShaders[1] = {};
}

void clear_shader_cache() {
    for (auto& shader : shaderStore) free_decompiler(shader->dec);
    shaders.clear(); shaderOutputs.clear(); shaderStore.clear();
    pendingCompiles.clear(); pendingShaders=0;  // their jobs complete into nothing
    reset_shader_memoization();
    // Fetch parsing allocations have shared interior pointers and no owning
    // destructor in the adapted Cemu parser. Keep its process-lifetime cache.
}
} // namespace gfxvk::vk
