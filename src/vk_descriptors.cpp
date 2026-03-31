#include <vk_descriptors.h>


/// what this whole file does, is first:
///		1. Add descriptor bindings to a list, so we can tie them up into a descriptor set later (DescriptorLayoutBuilder::add_binding).
///		2. Build the descriptor set layout so we can allocate descriptor sets from a descriptor pool, we do so by using the list of descriptor bindings we just made (dotpoint 1, 
///		   and also so we can make the compute pipeline, which requires a DS layout). (DescriptorLayoutBuilder::build)
///		3. Initialize the descriptor pool so we can allocate descriptor sets from it later. (DescriptorAllocator::init_pool)
///		4. Allocate the descriptor sets, using both the descriptor set layout we built (point 2) descriptor pool we initialized (point 3). (DescriptorAllocator::allocate)

/// So it's split between:
///		1. Add all of the descriptor bindings, and then build the descriptor layout.
///		2. Initialize the descriptor pool, and then allocate the descriptor sets.

// we'll create a function so we can add individual descriptor bindings to the <bindings> vector in the DescriptorLayoutBuilder struct.
void DescriptorLayoutBuilder::add_binding(uint32_t binding, VkDescriptorType type)
{
	VkDescriptorSetLayoutBinding newbind{};
	newbind.binding = binding;
	newbind.descriptorCount = 1;
	newbind.descriptorType = type;

	bindings.push_back(newbind);
}


void DescriptorLayoutBuilder::clear()
{
	bindings.clear();
}

// this will build the descriptor set layout, so we can both allocate the descriptor set(s) and also create the compute pipeline.
VkDescriptorSetLayout DescriptorLayoutBuilder::build(VkDevice device, VkShaderStageFlags shaderStages, void* pNext, VkDescriptorSetLayoutCreateFlags flags)
{
	// loop through bindings and add stage flags (idk what the xor operator is for either).
	for(auto& b : bindings)
	{
		b.stageFlags |= shaderStages;
	}

	// make the layout create info struct, bind it to the <bindings> vector full of the descriptor bindings. 
	VkDescriptorSetLayoutCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	info.pNext = pNext;

	info.pBindings = bindings.data();
	info.bindingCount = (uint32_t)bindings.size();
	info.flags = flags;

	// create a set layout, create it, and return it.
	VkDescriptorSetLayout set;
	VK_CHECK(vkCreateDescriptorSetLayout(device, &info, nullptr, &set));

	return set;
}

// we have to allocate the descriptors. we do so by creating a descriptor pool (which this function first initializes the pool).
// i don't exactly understand what the pool ratios are though.
void DescriptorAllocator::init_pool(VkDevice device, uint32_t maxSets, std::span<PoolSizeRatio> poolRatios)
{
	// a list of poolsizes. we add to this list by looping through each element the pool ratios struct, which holds the type of the descriptor and also the size ratio, and doing a push_back.

	std::vector<VkDescriptorPoolSize> poolSizes;
	for (PoolSizeRatio ratio : poolRatios)
	{
		poolSizes.push_back(VkDescriptorPoolSize{ .type = ratio.type, .descriptorCount = uint32_t(ratio.ratio * maxSets) });
	}

	VkDescriptorPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	pool_info.flags = 0;
	pool_info.maxSets = maxSets; // specifies the total amount of descriptor sets we can create from the pool
	pool_info.poolSizeCount = (uint32_t)poolSizes.size(); // specifies how many individual bindings of a given type are owned (how many bindings the descriptor sets have, I'm assuming).
	pool_info.pPoolSizes = poolSizes.data();

	vkCreateDescriptorPool(device, &pool_info, nullptr, &pool);
}

// this function just deletes/clears the descriptor sets that were allocated from their descriptor pool.
void DescriptorAllocator::clear_descriptors(VkDevice device)
{
	vkResetDescriptorPool(device, pool, 0);
}

// this deletes the actual descriptor pool itself.
void DescriptorAllocator::destroy_pool(VkDevice device)
{
	vkDestroyDescriptorPool(device, pool, nullptr);
}

// here, we provide a function so we can allocate a descriptor set.
VkDescriptorSet DescriptorAllocator::allocate(VkDevice device, VkDescriptorSetLayout layout)
{
	VkDescriptorSetAllocateInfo allocInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	allocInfo.pNext = nullptr;
	allocInfo.descriptorPool = pool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &layout;

	VkDescriptorSet ds;
	VK_CHECK(vkAllocateDescriptorSets(device, &allocInfo, &ds));

	return ds;
}