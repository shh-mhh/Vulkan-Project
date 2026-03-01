#pragma once

#include <vk_types.h>

struct DescriptorLayoutBuilder 
{
	// VkDescriptorSetLayoutBinding is a configuration/info struct that we'll store into an array. these are all the bindings on the descriptor set, so we can use them to build the layout.
	std::vector<VkDescriptorSetLayoutBinding> bindings{};

	void add_binding(uint32_t binding, VkDescriptorType type);
	void clear();
	VkDescriptorSetLayout build(VkDevice device, VkShaderStageFlags shaderStages, void* pNext = nullptr, VkDescriptorSetLayoutCreateFlags flags = 0);

};

// we can allocate descriptors using a descriptor pool. descriptor pools need to be pre-initialized with some size and types of descriptors for it.
// think of descriptor pools as a memory allocator for some specific (types of) descriptors.
// we could have 1 very big descriptor pool to handle the entire engine, but with that, we would have to know what descriptors we will be using for everything ahead of time. 
// that's why, instead, we'll keep it simpler and have multiple descriptor pools for different parts of the project, and try to be more accurate with them.

// later, we could implement per frame descriptors, because when we reset a descriptor pool, it destroys all of the descriptor sets allocated from it.
// so if we have descriptors for each frame which are allocated dynamically (can be allocated/resized at runtime...), before we start the frame, we can reset the descriptor pool to free memory.
// this is fast. so, it could be a good optimization perhaps.
struct DescriptorAllocator
{
	struct PoolSizeRatio
	{
		VkDescriptorType type;
		float ratio{};
	};

	VkDescriptorPool pool;

	void init_pool(VkDevice device, uint32_t maxSets, std::span<PoolSizeRatio> poolRatios);
	void clear_descriptors(VkDevice device);
	void destroy_pool(VkDevice device);

	VkDescriptorSet allocate(VkDevice device, VkDescriptorSetLayout layout);
};