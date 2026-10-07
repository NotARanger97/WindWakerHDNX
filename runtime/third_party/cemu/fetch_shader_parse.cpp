// Fetch shader microcode parser.
// Adapted from Cemu src/Cafe/HW/Latte/Core/FetchShader.cpp (cache registration removed).
// Copyright (c) Cemu contributors. Licensed under the Mozilla Public License 2.0 (see LICENSE.txt).
#include "Cafe/HW/Latte/Core/LatteConst.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerInstructions.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/ISA/LatteInstructions.h"
#ifdef ENABLE_METAL
#include "Cafe/HW/Latte/Renderer/Metal/LatteToMtl.h"
#else
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#endif

uint32 LatteShaderRecompiler_getAttributeSize(Latte::E_HWFMT format)
{
	if (format == Latte::E_HWFMT::HWFMT_32_32_32_32 || format == Latte::E_HWFMT::HWFMT_32_32_32_32_FLOAT)
		return 4 * 4;
	else if (format == Latte::E_HWFMT::HWFMT_32_32_32 || format == Latte::E_HWFMT::HWFMT_32_32_32_FLOAT)
		return 3 * 4;
	else if (format == Latte::E_HWFMT::HWFMT_32_32 || format == Latte::E_HWFMT::HWFMT_32_32_FLOAT)
		return 2 * 4;
	else if (format == Latte::E_HWFMT::HWFMT_32 || format == Latte::E_HWFMT::HWFMT_32_FLOAT)
		return 1 * 4;
	else if (format == Latte::E_HWFMT::HWFMT_16_16_16_16 || format == Latte::E_HWFMT::HWFMT_16_16_16_16_FLOAT)
		return 4 * 2;
	else if (format == Latte::E_HWFMT::HWFMT_16_16 || format == Latte::E_HWFMT::HWFMT_16_16_FLOAT)
		return 2 * 2;
	else if (format == Latte::E_HWFMT::HWFMT_16 || format == Latte::E_HWFMT::HWFMT_16_FLOAT)
		return 1 * 2;
	else if (format == Latte::E_HWFMT::HWFMT_8_8_8_8)
		return 4 * 1;
	else if (format == Latte::E_HWFMT::HWFMT_8_8)
		return 2 * 1;
	else if (format == Latte::E_HWFMT::HWFMT_8)
		return 1 * 1;
	else if (format == Latte::E_HWFMT::HWFMT_2_10_10_10)
		return 4;
	else
		cemu_assert_unimplemented();
	return 0;
}

uint32 LatteShaderRecompiler_getAttributeAlignment(Latte::E_HWFMT format)
{
	if (format == Latte::E_HWFMT::HWFMT_32_32_32_32 || format == Latte::E_HWFMT::HWFMT_32_32_32_32_FLOAT)
		return 4;
	else if (format == Latte::E_HWFMT::HWFMT_32_32_32 || format == Latte::E_HWFMT::HWFMT_32_32_32_FLOAT)
		return 4;
	else if (format == Latte::E_HWFMT::HWFMT_32_32 || format == Latte::E_HWFMT::HWFMT_32_32_FLOAT)
		return 4;
	else if (format == Latte::E_HWFMT::HWFMT_32 || format == Latte::E_HWFMT::HWFMT_32_FLOAT)
		return 4;
	else if (format == Latte::E_HWFMT::HWFMT_16_16_16_16 || format == Latte::E_HWFMT::HWFMT_16_16_16_16_FLOAT)
		return 2;
	else if (format == Latte::E_HWFMT::HWFMT_16_16 || format == Latte::E_HWFMT::HWFMT_16_16_FLOAT)
		return 2;
	else if (format == Latte::E_HWFMT::HWFMT_16 || format == Latte::E_HWFMT::HWFMT_16_FLOAT)
		return 2;
	else if (format == Latte::E_HWFMT::HWFMT_8_8_8_8)
		return 1;
	else if (format == Latte::E_HWFMT::HWFMT_8_8)
		return 1;
	else if (format == Latte::E_HWFMT::HWFMT_8)
		return 1;
	else if (format == Latte::E_HWFMT::HWFMT_2_10_10_10)
		return 4;
	else
		cemu_assert_unimplemented();
	return 4;
}

void LatteShader_calculateFSKey(LatteFetchShader* fetchShader)
{
	uint64 key = 0;
	for (sint32 g = 0; g < fetchShader->bufferGroups.size(); g++)
	{
		LatteParsedFetchShaderBufferGroup& group = fetchShader->bufferGroups[g];
		for (sint32 f = 0; f < group.attribCount; f++)
		{
			LatteParsedFetchShaderAttribute* attrib = group.attrib + f;
			key += (uint64)attrib->endianSwap;
			key = std::rotl<uint64>(key, 3);
			key += (uint64)attrib->nfa;
			key = std::rotl<uint64>(key, 3);
			key += (uint64)(attrib->isSigned?1:0);
			key = std::rotl<uint64>(key, 1);
			key += (uint64)attrib->format;
			key = std::rotl<uint64>(key, 7);
			key += (uint64)attrib->fetchType;
			key = std::rotl<uint64>(key, 8);
			key += (uint64)attrib->ds[0];
			key = std::rotl<uint64>(key, 2);
			key += (uint64)attrib->ds[1];
			key = std::rotl<uint64>(key, 2);
			key += (uint64)attrib->ds[2];
			key = std::rotl<uint64>(key, 2);
			key += (uint64)attrib->ds[3];
			key = std::rotl<uint64>(key, 2);
			key += (uint64)(attrib->aluDivisor+1);
			key = std::rotl<uint64>(key, 2);
			key += (uint64)attrib->attributeBufferIndex;
			key = std::rotl<uint64>(key, 8);
			key += (uint64)attrib->semanticId;
			key = std::rotl<uint64>(key, 8);
			switch(g_renderer->GetType())
			{
#ifdef ENABLE_METAL
			case RendererAPI::Metal:
			{
			    key += (uint64)attrib->offset;
				key = std::rotl<uint64>(key, 7);
				break;
			}
#endif
			default:
			{
				key += (uint64)(attrib->offset & 3);
				key = std::rotl<uint64>(key, 2);
				break;
			}
			}
		}
	}
	// todo - also hash invalid buffer groups?

#ifdef ENABLE_METAL
	if (g_renderer->GetType() == RendererAPI::Metal)
	{
		for (sint32 g = 0; g < fetchShader->bufferGroups.size(); g++)
	{
			LatteParsedFetchShaderBufferGroup& group = fetchShader->bufferGroups[g];
			key += (uint64)group.attributeBufferIndex;
			key = std::rotl<uint64>(key, 5);
		}
	}
#endif

	fetchShader->key = key;
}

void LatteFetchShader::CalculateFetchShaderVkHash()
{
	// patched: Vulkan pipeline hash is unused here; a plain FNV-1a over the attribute layout replaces SHA-1
	// fields only: struct padding is not initialised by every parser, so hashing raw bytes made the
	// value differ between runs (it keys the persisted pipeline warm-up recipes)
	uint64 h = 1469598103934665603ull;
	auto mix = [&](uint64 v) { h = (h ^ v) * 1099511628211ull; };
	for (auto& group : bufferGroups)
	{
		mix(group.attributeBufferIndex);
		for (sint32 f = 0; f < group.attribCount; f++)
		{
			const auto& a = group.attrib[f];
			mix(a.attributeBufferIndex); mix(a.semanticId); mix((uint64)a.format); mix((uint64)a.fetchType);
			mix(a.nfa); mix(a.isSigned); mix((uint64)a.endianSwap);
			for (int k = 0; k < 4; k++) mix(a.ds[k]);
			mix((uint32)a.aluDivisor); mix(a.offset);
		}
	}
	this->vkPipelineHashFragment = h;
}

#ifdef ENABLE_METAL
void LatteFetchShader::CheckIfVerticesNeedManualFetchMtl(uint32* contextRegister)
{
	for (sint32 g = 0; g < bufferGroups.size(); g++)
	{
	    LatteParsedFetchShaderBufferGroup& group = bufferGroups[g];
		uint32 bufferIndex = group.attributeBufferIndex;
		uint32 bufferBaseRegisterIndex = mmSQ_VTX_ATTRIBUTE_BLOCK_START + bufferIndex * 7;
		uint32 bufferStride = (contextRegister[bufferBaseRegisterIndex + 2] >> 11) & 0xFFFF;

  		if (bufferStride % 4 != 0)
  		    mtlFetchVertexManually = true;

  		for (sint32 f = 0; f < group.attribCount; f++)
  		{
  		    auto& attr = group.attrib[f];
  		    if (attr.offset + GetMtlVertexFormatSize(attr.format) > bufferStride)
 			    mtlFetchVertexManually = true;
  		}
	}
}
#endif

void _fetchShaderDecompiler_parseInstruction_VTX_SEMANTIC(LatteFetchShader* parsedFetchShader, uint32* contextRegister, const LatteClauseInstruction_VTX* instr)
{
	uint32 semanticId = instr->getFieldSEM_SEMANTIC_ID(); // location (attribute index inside shader)
	uint32 bufferId = instr->getField_BUFFER_ID(); // the index used for GX2SetAttribBuffer (+0xA0)
	LatteConst::VertexFetchType2 fetchType = instr->getField_FETCH_TYPE();
	auto srcSelX = instr->getField_SRC_SEL_X();
	auto dsx = instr->getField_DST_SEL(0);
	auto dsy = instr->getField_DST_SEL(1);
	auto dsz = instr->getField_DST_SEL(2);
	auto dsw = instr->getField_DST_SEL(3);
	auto dataFormat = instr->getField_DATA_FORMAT();
	uint32 offset = instr->getField_OFFSET();
	auto nfa = instr->getField_NUM_FORMAT_ALL();
	bool isSigned = instr->getField_FORMAT_COMP_ALL() == LatteClauseInstruction_VTX::FORMAT_COMP::COMP_SIGNED;
	auto endianSwap = instr->getField_ENDIAN_SWAP();

	uint32 attribSize = LatteShaderRecompiler_getAttributeSize(dataFormat);
	cemu_assert(attribSize > 0);
	uint32 offsetAfterAttrib = offset + attribSize;

	// get buffer
	cemu_assert_debug(bufferId >= 0xA0 && bufferId < 0xB0);
	uint32 bufferIndex = (bufferId - 0xA0);

	// get or add new attribute group (by buffer index)
	LatteParsedFetchShaderBufferGroup* attribGroup = nullptr;
	if (LatteFetchShader::isValidBufferIndex(bufferIndex))
	{
		auto bufferGroupItr = std::find_if(parsedFetchShader->bufferGroups.begin(), parsedFetchShader->bufferGroups.end(), [bufferIndex](LatteParsedFetchShaderBufferGroup& bufferGroup) {return bufferGroup.attributeBufferIndex == bufferIndex; });
		if (bufferGroupItr != parsedFetchShader->bufferGroups.end())
			attribGroup = &(*bufferGroupItr);
	}
	else
	{
		auto bufferGroupItr = std::find_if(parsedFetchShader->bufferGroupsInvalid.begin(), parsedFetchShader->bufferGroupsInvalid.end(), [bufferIndex](LatteParsedFetchShaderBufferGroup& bufferGroup) {return bufferGroup.attributeBufferIndex == bufferIndex; });
		if (bufferGroupItr != parsedFetchShader->bufferGroupsInvalid.end())
			attribGroup = &(*bufferGroupItr);
	}
	// create new group if none found
	if (attribGroup == nullptr)
	{
		if (LatteFetchShader::isValidBufferIndex(bufferIndex))
			attribGroup = &parsedFetchShader->bufferGroups.emplace_back();
		else
			attribGroup = &parsedFetchShader->bufferGroupsInvalid.emplace_back();

		parsedFetchShader->attributeBufferMask |= (1 << bufferIndex);
		attribGroup->attributeBufferIndex = bufferIndex;
		attribGroup->minOffset = offset;
		attribGroup->totalAttribRangeSize = offset;
	}
	// add attribute
	sint32 groupAttribIndex = attribGroup->attribCount;
	if (attribGroup->attribCount < (groupAttribIndex + 1))
	{
		cemu_assert(groupAttribIndex < 127);
		attribGroup->attribCount = (groupAttribIndex + 1);
		attribGroup->attrib = (LatteParsedFetchShaderAttribute*)realloc(attribGroup->attrib, sizeof(LatteParsedFetchShaderAttribute) * attribGroup->attribCount);
	}
	attribGroup->attrib[groupAttribIndex].semanticId = semanticId;
	attribGroup->attrib[groupAttribIndex].format = dataFormat;
	attribGroup->attrib[groupAttribIndex].fetchType = fetchType;
	attribGroup->attrib[groupAttribIndex].nfa = (uint8)nfa;
	attribGroup->attrib[groupAttribIndex].isSigned = isSigned;
	attribGroup->attrib[groupAttribIndex].offset = offset;
	attribGroup->attrib[groupAttribIndex].ds[0] = (uint8)dsx;
	attribGroup->attrib[groupAttribIndex].ds[1] = (uint8)dsy;
	attribGroup->attrib[groupAttribIndex].ds[2] = (uint8)dsz;
	attribGroup->attrib[groupAttribIndex].ds[3] = (uint8)dsw;
	attribGroup->attrib[groupAttribIndex].attributeBufferIndex = bufferIndex;
	attribGroup->attrib[groupAttribIndex].endianSwap = endianSwap;
	attribGroup->minOffset = (std::min)(attribGroup->minOffset, offset);
	attribGroup->totalAttribRangeSize = (std::max)(attribGroup->totalAttribRangeSize, offsetAfterAttrib);
	// get alu divisor
	if (srcSelX == LatteClauseInstruction_VTX::SRC_SEL::SEL_X)
	{
		cemu_assert_debug(fetchType != LatteConst::VertexFetchType2::INSTANCE_DATA); // aluDivisor 0 in combination with instanced data is not allowed?
		attribGroup->attrib[groupAttribIndex].aluDivisor = -1;
	}
	else if (srcSelX == LatteClauseInstruction_VTX::SRC_SEL::SEL_W)
	{
		cemu_assert_debug(fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA); // using constant divisor 1 with per-vertex data seems strange? (divisor is instance-only)
		// aluDivisor is constant 1
		attribGroup->attrib[groupAttribIndex].aluDivisor = 1;
	}
	else if (srcSelX == LatteClauseInstruction_VTX::SRC_SEL::SEL_Y)
	{
		// use alu divisor 1
		attribGroup->attrib[groupAttribIndex].aluDivisor = (sint32)contextRegister[Latte::REGADDR::VGT_INSTANCE_STEP_RATE_0];
		cemu_assert_debug(attribGroup->attrib[groupAttribIndex].aluDivisor > 0);
	}
	else if (srcSelX == LatteClauseInstruction_VTX::SRC_SEL::SEL_Z)
	{
		// use alu divisor 2
		attribGroup->attrib[groupAttribIndex].aluDivisor = (sint32)contextRegister[Latte::REGADDR::VGT_INSTANCE_STEP_RATE_1];
		cemu_assert_debug(attribGroup->attrib[groupAttribIndex].aluDivisor > 0);
	}
}

void _fetchShaderDecompiler_parseVTXClause(LatteFetchShader* parsedFetchShader, uint32* contextRegister, std::span<uint8> clauseCode, size_t numInstructions)
{
	const LatteClauseInstruction_VTX* instr = (LatteClauseInstruction_VTX*)clauseCode.data();
	const LatteClauseInstruction_VTX* end = instr + numInstructions;
	while (instr < end)
	{
		if (instr->getField_VTX_INST() == LatteClauseInstruction_VTX::VTX_INST::_VTX_INST_SEMANTIC)
		{
			_fetchShaderDecompiler_parseInstruction_VTX_SEMANTIC(parsedFetchShader, contextRegister, instr);
		}
		else
		{
			assert_dbg();
		}
		instr++;
	}
}

void _fetchShaderDecompiler_parseCF(LatteFetchShader* parsedFetchShader, uint32* contextRegister, std::span<uint8> programCode)
{
	size_t maxCountCFInstructions = programCode.size_bytes() / sizeof(LatteCFInstruction);
	const LatteCFInstruction* cfInstruction = (LatteCFInstruction*)programCode.data();
	const LatteCFInstruction* end = cfInstruction + maxCountCFInstructions;
	while (cfInstruction < end)
	{
		if (cfInstruction->getField_Opcode() == LatteCFInstruction::INST_VTX_TC)
		{
			auto vtxInstruction = cfInstruction->getParserIfOpcodeMatch<LatteCFInstruction_DEFAULT>();
			cemu_assert_debug(vtxInstruction->getField_COND() == LatteCFInstruction::CF_COND::CF_COND_ACTIVE);
			_fetchShaderDecompiler_parseVTXClause(parsedFetchShader, contextRegister, vtxInstruction->getClauseCode(programCode), vtxInstruction->getField_COUNT());
		}
		else if (cfInstruction->getField_Opcode() == LatteCFInstruction::INST_RETURN)
		{
			cemu_assert_debug(!cfInstruction->getField_END_OF_PROGRAM());
			return;
		}
		else
		{
			cemu_assert_debug(false); // unhandled / unexpected CF instruction
		}
		if (cfInstruction->getField_END_OF_PROGRAM())
		{
			cemu_assert_debug(false); // unusual for fetch shader? They should end with a return instruction
			break;
		}
		cfInstruction++;
	}
	cemu_assert_debug(false); // program must be terminated with an instruction that has EOP set?
}

// parse fetch shader and create LatteFetchShader object
// also registers the fs in the cache (s_fetchShaderByHash)
// can be assumed to be thread-safe, if called simultaneously on the same fetch shader only one shader will become registered. The others will be destroyed
LatteFetchShader* LatteShaderRecompiler_createFetchShader(LatteFetchShader::CacheHash fsHash, uint32* contextRegister, uint32* fsProgramCode, uint32 fsProgramSize)
{
	LatteFetchShader* newFetchShader = new LatteFetchShader();
	newFetchShader->m_cacheHash = fsHash;
	if( (fsProgramSize&0xF) != 0 )
		debugBreakpoint();
	uint32 index = 0;

	// if the first instruction is a CF instruction then parse shader properly
	// otherwise fall back to our broken legacy method (where we assumed fetch shaders had no CF program)
	// this workaround is required to make sure old shader caches dont break

	// from old fetch shader gen (CF part missing):
	//			{0x0000a001, 0x27961000, 0x00020000, 0x00000000}
	//			{0x0000a001, 0x2c151002, 0x00020000, 0x00000000, 0x0000a001, 0x068d1000, 0x0000000c, ...}
	//			{0x0000a001, 0x2c151000, 0x00020000, 0x00000000}
	//          {0x0300aa21, 0x28cd1006, 0x00000000, 0x00000000, 0x0300ab21, 0x28cd1007, 0x00000000, ...}

	// shaders shipped with games (e.g. BotW):
	//			{0x00000002, 0x01800400, 0x00000000, 0x8a000000, 0x1c00a001, 0x280d1000, 0x00090000, ...}
	//			{0x00000002, 0x01800000, 0x00000000, 0x8a000000, 0x1c00a001, 0x27961000, 0x000a0000, ...}
	//			{0x00000002, 0x01800c00, 0x00000000, 0x8a000000, 0x2c00a001, 0x2c151000, 0x000a0000, ...} // size 0x50
	//          {0x00000002, 0x01801000, 0x00000000, 0x8a000000, 0x1c00a001, 0x280d1000, 0x00090000, ...} // size 0x60
	//			{0x00000002, 0x01801c00, 0x00000000, 0x8a000000, 0x1c00a001, 0x280d1000, 0x00090000, ...} // size 0x90

	// our new implementation:
	//			{0x00000002, 0x01800400, 0x00000000, 0x8a000000, 0x0000a001, 0x2c151000, 0x00020000, ...}

	// for ALU instructions everything except the 01 is dynamic
	newFetchShader->bufferGroups.reserve(16);
	if (fsProgramSize == 0)
	{
		// empty fetch shader, seen in Minecraft
		// these only make sense when vertex shader does not call FS?
		LatteShader_calculateFSKey(newFetchShader);
		newFetchShader->CalculateFetchShaderVkHash();
#ifdef ENABLE_METAL
		newFetchShader->CheckIfVerticesNeedManualFetchMtl(contextRegister);
#endif
		return newFetchShader;
	}

	if ((fsProgramCode[0] & 1) == 0 && fsProgramCode[0] <= 0x30 && (fsProgramCode[1]&~((3 << 10)| (1 << 19))) == 0x01800000)
	{
		// very likely a CF instruction
		_fetchShaderDecompiler_parseCF(newFetchShader, contextRegister, { (uint8*)fsProgramCode, fsProgramSize });
	}
	else
	{
		while (index < (fsProgramSize / 4))
		{
			uint32 dword0 = fsProgramCode[index];
			uint32 opcode = dword0 & 0x1F;
			index++;
			if (opcode == VTX_INST_MEM)
			{
				// this might be the clause initialization instruction? (Seems to be the first instruction always)
				// todo - upon further investigation, it seems like fetch shaders also start with a CF program. Our implementation doesnt emit one right now
				uint32 opcode2 = (dword0 >> 8) & 7;

				index += 3;
			}
			else if (opcode == VTX_INST_SEMANTIC)
			{
				_fetchShaderDecompiler_parseInstruction_VTX_SEMANTIC(newFetchShader, contextRegister, (const LatteClauseInstruction_VTX*)(fsProgramCode + index - 1));
				index += 3;
			}
		}
	}
	newFetchShader->bufferGroups.shrink_to_fit();
	// calculate group information
	cemu_assert(newFetchShader->bufferGroups.size() <= Latte::GPU_LIMITS::NUM_VERTEX_BUFFERS);
	for (auto& bufferGroup : newFetchShader->bufferGroups)
	{
		bufferGroup.hasVtxIndexAccess = false;
		bufferGroup.hasInstanceIndexAccess = false;
		for(sint32 i=0; i<bufferGroup.attribCount; i++)
		{
			auto& attrib = bufferGroup.attrib[i];
			bufferGroup.hasVtxIndexAccess |= (attrib.fetchType == LatteConst::VERTEX_DATA);
			bufferGroup.hasInstanceIndexAccess |= (attrib.fetchType == LatteConst::INSTANCE_DATA);
		}
	}
	LatteShader_calculateFSKey(newFetchShader);
	newFetchShader->CalculateFetchShaderVkHash();
#ifdef ENABLE_METAL
	newFetchShader->CheckIfVerticesNeedManualFetchMtl(contextRegister);
#endif

	return newFetchShader;
}

