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