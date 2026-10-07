// Single source of deferred entry points, signatures, and pointer ownership.
// Read by gen_vk_switch.py; expanded into public wrappers by vk_record_thread.cpp.
// Add a recursively deep copy policy for every pointer when extending this list.
// VK_RECORD_CALL(return, name, (parameters), (direct arguments), (owned arguments))

VK_RECORD_CALL(VkResult, vkBeginCommandBuffer,
    (VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo),
    (commandBuffer, pBeginInfo),
    (commandBuffer, vkrecord::array(pBeginInfo, 1)))

VK_RECORD_CALL(void, vkCmdBeginRendering,
    (VkCommandBuffer commandBuffer, const VkRenderingInfo* pRenderingInfo),
    (commandBuffer, pRenderingInfo),
    (commandBuffer, vkrecord::array(pRenderingInfo, 1)))

VK_RECORD_CALL(void, vkCmdBindDescriptorSets,
    (VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets),
    (commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets),
    (commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, vkrecord::array(pDescriptorSets, descriptorSetCount), dynamicOffsetCount, vkrecord::array(pDynamicOffsets, dynamicOffsetCount)))

VK_RECORD_CALL(void, vkCmdBindIndexBuffer,
    (VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkIndexType indexType),
    (commandBuffer, buffer, offset, indexType),
    (commandBuffer, buffer, offset, indexType))

VK_RECORD_CALL(void, vkCmdBindPipeline,
    (VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline),
    (commandBuffer, pipelineBindPoint, pipeline),
    (commandBuffer, pipelineBindPoint, pipeline))

VK_RECORD_CALL(void, vkCmdBindVertexBuffers,
    (VkCommandBuffer commandBuffer, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets),
    (commandBuffer, firstBinding, bindingCount, pBuffers, pOffsets),
    (commandBuffer, firstBinding, bindingCount, vkrecord::array(pBuffers, bindingCount), vkrecord::array(pOffsets, bindingCount)))

VK_RECORD_CALL(void, vkCmdBlitImage,
    (VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageBlit* pRegions, VkFilter filter),
    (commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions, filter),
    (commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, vkrecord::array(pRegions, regionCount), filter))

VK_RECORD_CALL(void, vkCmdClearColorImage,
    (VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearColorValue* pColor, uint32_t rangeCount, const VkImageSubresourceRange* pRanges),
    (commandBuffer, image, imageLayout, pColor, rangeCount, pRanges),
    (commandBuffer, image, imageLayout, vkrecord::array(pColor, 1), rangeCount, vkrecord::array(pRanges, rangeCount)))

VK_RECORD_CALL(void, vkCmdClearDepthStencilImage,
    (VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearDepthStencilValue* pDepthStencil, uint32_t rangeCount, const VkImageSubresourceRange* pRanges),
    (commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges),
    (commandBuffer, image, imageLayout, vkrecord::array(pDepthStencil, 1), rangeCount, vkrecord::array(pRanges, rangeCount)))

VK_RECORD_CALL(void, vkCmdCopyBuffer,
    (VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, uint32_t regionCount, const VkBufferCopy* pRegions),
    (commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions),
    (commandBuffer, srcBuffer, dstBuffer, regionCount, vkrecord::array(pRegions, regionCount)))

VK_RECORD_CALL(void, vkCmdCopyBufferToImage,
    (VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkBufferImageCopy* pRegions),
    (commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions),
    (commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, vkrecord::array(pRegions, regionCount)))

VK_RECORD_CALL(void, vkCmdCopyImage,
    (VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage, VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageCopy* pRegions),
    (commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions),
    (commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, vkrecord::array(pRegions, regionCount)))

VK_RECORD_CALL(void, vkCmdCopyImageToBuffer,
    (VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkBuffer dstBuffer, uint32_t regionCount, const VkBufferImageCopy* pRegions),
    (commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, pRegions),
    (commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, vkrecord::array(pRegions, regionCount)))

VK_RECORD_CALL(void, vkCmdDraw,
    (VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance),
    (commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance),
    (commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance))

VK_RECORD_CALL(void, vkCmdDrawIndexed,
    (VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance),
    (commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance),
    (commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance))

VK_RECORD_CALL(void, vkCmdEndRendering,
    (VkCommandBuffer commandBuffer),
    (commandBuffer),
    (commandBuffer))

VK_RECORD_CALL(void, vkCmdPipelineBarrier,
    (VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers, uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier* pImageMemoryBarriers),
    (commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount, pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers, imageMemoryBarrierCount, pImageMemoryBarriers),
    (commandBuffer, srcStageMask, dstStageMask, dependencyFlags, memoryBarrierCount, vkrecord::array(pMemoryBarriers, memoryBarrierCount), bufferMemoryBarrierCount, vkrecord::array(pBufferMemoryBarriers, bufferMemoryBarrierCount), imageMemoryBarrierCount, vkrecord::array(pImageMemoryBarriers, imageMemoryBarrierCount)))

VK_RECORD_CALL(void, vkCmdPushConstants,
    (VkCommandBuffer commandBuffer, VkPipelineLayout layout, VkShaderStageFlags stageFlags, uint32_t offset, uint32_t size, const void* pValues),
    (commandBuffer, layout, stageFlags, offset, size, pValues),
    (commandBuffer, layout, stageFlags, offset, size, vkrecord::bytes(pValues, size)))

VK_RECORD_CALL(void, vkCmdResetQueryPool,
    (VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount),
    (commandBuffer, queryPool, firstQuery, queryCount),
    (commandBuffer, queryPool, firstQuery, queryCount))

VK_RECORD_CALL(void, vkCmdSetBlendConstants,
    (VkCommandBuffer commandBuffer, const float blendConstants[4]),
    (commandBuffer, blendConstants),
    (commandBuffer, vkrecord::array(blendConstants, 4)))

VK_RECORD_CALL(void, vkCmdSetScissor,
    (VkCommandBuffer commandBuffer, uint32_t firstScissor, uint32_t scissorCount, const VkRect2D* pScissors),
    (commandBuffer, firstScissor, scissorCount, pScissors),
    (commandBuffer, firstScissor, scissorCount, vkrecord::array(pScissors, scissorCount)))

VK_RECORD_CALL(void, vkCmdSetStencilReference,
    (VkCommandBuffer commandBuffer, VkStencilFaceFlags faceMask, uint32_t reference),
    (commandBuffer, faceMask, reference),
    (commandBuffer, faceMask, reference))

VK_RECORD_CALL(void, vkCmdSetViewport,
    (VkCommandBuffer commandBuffer, uint32_t firstViewport, uint32_t viewportCount, const VkViewport* pViewports),
    (commandBuffer, firstViewport, viewportCount, pViewports),
    (commandBuffer, firstViewport, viewportCount, vkrecord::array(pViewports, viewportCount)))

VK_RECORD_CALL(void, vkCmdWriteTimestamp,
    (VkCommandBuffer commandBuffer, VkPipelineStageFlagBits pipelineStage, VkQueryPool queryPool, uint32_t query),
    (commandBuffer, pipelineStage, queryPool, query),
    (commandBuffer, pipelineStage, queryPool, query))

VK_RECORD_CALL(VkResult, vkEndCommandBuffer,
    (VkCommandBuffer commandBuffer),
    (commandBuffer),
    (commandBuffer))

VK_RECORD_CALL(VkResult, vkQueuePresentKHR,
    (VkQueue queue, const VkPresentInfoKHR* pPresentInfo),
    (queue, pPresentInfo),
    (queue, vkrecord::array(pPresentInfo, 1)))

VK_RECORD_CALL(VkResult, vkQueueSubmit,
    (VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence),
    (queue, submitCount, pSubmits, fence),
    (queue, submitCount, vkrecord::array(pSubmits, submitCount), fence))

VK_RECORD_CALL(VkResult, vkQueueSubmit2,
    (VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence),
    (queue, submitCount, pSubmits, fence),
    (queue, submitCount, vkrecord::array(pSubmits, submitCount), fence))

VK_RECORD_CALL(VkResult, vkResetCommandBuffer,
    (VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags),
    (commandBuffer, flags),
    (commandBuffer, flags))
