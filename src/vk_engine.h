// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.

#pragma once

#include <vk_types.h>


/// <summary>
/// ok dude, i'm not gonna lie... i'm lost with what the hell this deletion queue thingo is (sorta). lambdas, polymorphic function wrappers (std::function), functors... 
/// i'm out of my depth rn
/// 
/// basically, we create a deque, which is a first in last out queue. we fill this deque with std::function's, which let you store, copy, and invoke any callable object (like lambdas). 
/// we store a lambda in each std::function. this will allow us to free/delete parts of the engine, which we add to the deque, by calling the lambda through the std::function. 
/// we can use push_function to push a part of the engine (like semaphores, just for an example) onto the deque (so we can free it later). and once we want to free all of the resources 
/// in the deque (which are vulkan objects/handles we added/pushed), we just use flush().
/// </summary>
struct DeletionQueue 
{
	// create a deque (supports first in last out) of std::function's, which store lambdas. we do this so we can store callbacks with some data with it (using the lambdas).
	std::deque<std::function<void()>> deletors;

	// create a function to push a lamba std::function onto the deque.
	void push_function(std::function<void()>&& function)
	{
		deletors.push_back(function);
	}

	void flush() 
	{
		// reverse iterate the deletion queue to execute all the functions. rbegin is a reverse iterator to the beginning, rend is the end.
		for (auto it = deletors.rbegin(); it != deletors.rend(); it++)
		{
			(*it)(); // call functors
		}
	
		deletors.clear();
	}
};


struct FrameData
{
	// we have a command pool and main command buffer for each "frame", because we're double buffering.
	// so, we have 2 command pools/command buffers, 1 for each frame (so we can double buffer). 
	VkCommandPool _commandPool{};
	VkCommandBuffer _mainCommandBuffer{};

	// we need a semaphore for the swapchain, so the GPU can request an image from the swapchain. our rendering commands will wait for the swapchain semaphore.
	// we also have a semaphore for rendering that controls presenting the image to the OS once drawing finishes (we wait until we finish drawing and then present the image).
	// and also a fence for rendering, cause we need to know when the GPU is done rendering (we wait for the draw commands of a given frame to be finished).
	// so:
		// 1. Request an image for the swapchain (semaphore 1, which will be signalled when we get the image).
		// 2. Wait until we're done drawing so we can present the image (semaphore 2, waits on semaphore 1 until it's signalled; meaning, semaphore 2 will wait until we got an image from the swapchain).
		// 3. Alert the CPU that the GPU is finished rendering (fence 1).
	VkSemaphore _swapchainSemaphore, _renderSemaphore{};
	VkFence _renderFence{};

	// we add a deletion queue to each frame (in flight), as the lifetime of the objects that each frame has (command pool/buffer and semaphores/fence) is different to the rest of the 
	// engine (and to the other frame). thus, we delete the stuff associated with each frame (in flight) at a different time than everything else, so we give each frame the deletion queue.
	// this allows us to delete objects next frame after they are used.
	DeletionQueue _deletionQueue;
};


constexpr unsigned int FRAME_OVERLAP{ 2 };


class VulkanEngine {
public:

	bool _isInitialized{ false };
	int _frameNumber {0};
	bool stop_rendering{ false };
	VkExtent2D _windowExtent{ 1700 , 900 };

	/// --- vulkan initialization members --- ///

	VkInstance _instance; // Vulkan library handle.
	VkDebugUtilsMessengerEXT _debugMessenger; // Vulkan debug output handle.
	VkPhysicalDevice _chosenGPU; // GPU chosen as the default device
	VkDevice _device; // Vulkan device for commands
	VkSurfaceKHR _surface; // Vulkan window surface

	VkSwapchainKHR _swapchain;
	VkFormat _swapchainImageFormat;
	
	// vkimage is a handle to the actual image object to use as texture or render to. imageview is a wrapper for that image.
	// we need a vector/list of the swapchain images and image views we'll be using.
	std::vector<VkImage> _swapchainImages;
	std::vector<VkImageView> _swapchainImageViews;
	VkExtent2D _swapchainExtent;

	/// --- vulkan command members --- ///
	
	FrameData _frames[FRAME_OVERLAP]; // create array of FrameData structures (each struct is the data for a frame)

	FrameData& get_current_frame() { return _frames[_frameNumber % FRAME_OVERLAP]; }; // return the current frame. lowkey i still get confused when we return references (lifetimes)...

	VkQueue _graphicsQueue; // queues aren't really created objects, they're more like a handle to something that already exists as part of the VkInstance.
	uint32_t _graphicsQueueFamily;

	// this is a forward declaration apparently...
	struct SDL_Window* _window{ nullptr };

	static VulkanEngine& Get();

	DeletionQueue _mainDeletionQueue;

	VmaAllocator _allocator;

	AllocatedImage _drawImage;
	VkExtent2D _drawExtent;

	//initializes everything in the engine
	void init();

	//shuts down the engine
	void cleanup();

	//draw loop
	void draw();

	//run main loop
	void run();

	void draw_background(VkCommandBuffer cmd);

private:

	void init_vulkan();
	void init_swapchain();
	void init_commands();
	void init_sync_structures();
	void create_swapchain(uint32_t width, uint32_t height);
	void destroy_swapchain();
};





