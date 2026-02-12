
#pragma once 

#include <vulkan/vulkan.h>
#include "vk_initializers.h"

namespace vkutil 
{

	// we use a memory image barrier in order to transition the swapchain image formats, into a writable format. We then write to the image, and transition it back to a readable format
	// so we can display it to the screen.
	
	// stage and access masks, i'm fairly sure, essentially just control how the pipeline barrier stops different parts of the GPU. check the documentation!
	void transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout currentLayout, VkImageLayout newLayout);

	void copy_image_to_image(VkCommandBuffer cmd, VkImage source, VkImage destination, VkExtent2D srcSize, VkExtent2D dstSize);
};