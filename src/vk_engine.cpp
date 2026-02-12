//> includes
#include "vk_engine.h"

#include "SDL.h"
#include <SDL_vulkan.h>

// --- other includes --- //
#include <vk_initializers.h>
#include <vk_types.h>
#include <vk_images.h>

// boostrap library
#include "VkBootstrap.h"
#include <array>
#include <chrono>
#include <thread>

// Vulkan Memory Allocator
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"

constexpr bool bUseValidationLayers{ true };

VulkanEngine* loadedEngine = nullptr;

VulkanEngine& VulkanEngine::Get() { return *loadedEngine; }

/// vkbootstrap follows this loose order for initialization:
/// 1. create class object to build or initially set up the vulkan handle/object, and use flags on the class object to set up the vulkan handle/object.
/// 2. create a vkb struct of that handle to hold its data, and assign it to the class object (mentioned above) with all of its flags.
/// 3. store the vulkan handle in a separate vulkan handle by accessing the vkb struct's fields. 


void VulkanEngine::init()
{
    // only one engine initialization is allowed with the application.
    assert(loadedEngine == nullptr);
    loadedEngine = this;

    // We initialize SDL and create a window with it.
    SDL_Init(SDL_INIT_VIDEO);

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN);

    _window = SDL_CreateWindow(
        "Vulkan Engine",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        _windowExtent.width,
        _windowExtent.height,
        window_flags);

    init_vulkan();

    init_swapchain();

    init_commands();

    init_sync_structures();

    // everything went fine
    _isInitialized = true;
}

void VulkanEngine::init_vulkan()
{
                                                                            /// --- vulkan instance initialization --- ///


    vkb::InstanceBuilder builder;

    auto inst_ret = builder.set_app_name("Vulkan Application")
        .request_validation_layers(bUseValidationLayers)
        .use_default_debug_messenger()
        .require_api_version(1, 3, 0)
        .build();

    vkb::Instance vkb_inst = inst_ret.value();

    // grab the instance
    _instance = vkb_inst.instance;
    _debugMessenger = vkb_inst.debug_messenger;


                                                                         /// --- physical & logical device initialization --- ///


    SDL_Vulkan_CreateSurface(_window, _instance, &_surface);

    // vulkan 1.3 features 
    // get features of vulkan 1.3 physical device, and then turn on dynamic rendering and the upgraded version of the sync functions.
    VkPhysicalDeviceVulkan13Features features{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    features.dynamicRendering = true;
    features.synchronization2 = true;

    // vulkan 1.2 features. 
    // bufferDeviceAddress lets us use GPU pointers without binding buffers, descriptorIndexing gives us bindless textures. so, no binding here!  
    VkPhysicalDeviceVulkan12Features features12{ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    features12.bufferDeviceAddress = true;
    features12.descriptorIndexing = true;

    // use vkbootstrap to select a gpu.
    // the PhysicalDeviceSelector is a class (that needs to be initialized with a VkInstance, as we need an instance to select a GPU) that we can use many flags (member functions)
    // to select the physical device we want. we store the selected physical device in a vkb::PhysicalDevice struct that holds all the data of the physical device.
    // the same concept applies for the logical device.
    // we want a gpu that can write to the SDL surface and supports vulkan 1.3 with the correct features. 
    // we pass the features structures so we tell vkbootstrap to find a gpu with those features.
    vkb::PhysicalDeviceSelector selector{ vkb_inst };
    vkb::PhysicalDevice physicalDevice = selector
        .set_minimum_version(1, 3)
        .set_required_features_13(features)
        .set_required_features_12(features12)
        .set_surface(_surface)
        .select()
        .value();

    // create the final vulkan device (we already got the physical device from above). 
    // we make a deviceBuilder object so we can use the physical device to build the logical device.
    vkb::DeviceBuilder deviceBuilder{ physicalDevice };

    vkb::Device vkbDevice = deviceBuilder.build().value();

    // get the VkDevice handle used in the rest of a vulkan application
    // the vkbDevice and physicalDevice are both structs which hold all the data. we just access the .device and .physical_device fields to get the actual vulkan objects we need
    // ctrl + left click the device and physical_device fields to really see.
    _device = vkbDevice.device;
    _chosenGPU = physicalDevice.physical_device;


                                                                                /// --- queue initialization --- ///


    // use vkbootstrap to get a Graphics queue (the queue type is graphics; meaning it'll accept graphics commands). 
    // We request a graphics queue from the logical device, and then also get the queue family index (which queue family we're using; we create the queue from this queue family).
    _graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    _graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();


                                                                                 /// --- VMA initialization --- ///

    // initialize the memory allocator
    VmaAllocatorCreateInfo allocatorInfo {};
    allocatorInfo.physicalDevice = _chosenGPU;
    allocatorInfo.device = _device;
    allocatorInfo.instance = _instance;
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vmaCreateAllocator(&allocatorInfo, &_allocator);

    // make sure to push the memory allocator object onto the main deletion queue (so we can free it). the lambda that will be stored in the std::function is just the vmaDestroyAllocator(). 
    // basically, we'll store vmaDestroyAllocator() into the lambda, and then store that in std::function, which is in the deque. for std::function, when we use the lambda, it'll call 
    // vmaDestroyAllocator(). we just store the destruction function into the main deletion queue (the deque).
    // so, when the engine exits, the memory allocator gets cleared.
    _mainDeletionQueue.push_function([&]() { vmaDestroyAllocator(_allocator); });

}

void VulkanEngine::create_swapchain(uint32_t width, uint32_t height)
{
    // same principles as the init_vulkan() function really. vkbootstrap is very straight forward. love it.
    
                                                                                /// --- swap-chain creation --- ///
    vkb::SwapchainBuilder swapchainBuilder{ _chosenGPU, _device, _surface };

    _swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;

    vkb::Swapchain vkbSwapchain = swapchainBuilder
        //.use_default_format_selection()
        .set_desired_format(VkSurfaceFormatKHR{ .format = _swapchainImageFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
        // use vsync present mode
        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
        .set_desired_extent(width, height)
        .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        .build()
        .value();

    // store swapchain, its extent, and its related images. basically just store all swapchain stuff in member variables of the VulkanEngine class.
    _swapchainExtent = vkbSwapchain.extent;
    _swapchain = vkbSwapchain.swapchain;
    _swapchainImages = vkbSwapchain.get_images().value();
    _swapchainImageViews = vkbSwapchain.get_image_views().value();
}

void VulkanEngine::destroy_swapchain()
{
    vkDestroySwapchainKHR(_device, _swapchain, nullptr);

    // destroy swapchain resources
    for (int i = 0 ; i < _swapchainImageViews.size() ; i++)
    {
        vkDestroyImageView(_device, _swapchainImageViews[i], nullptr);
    }
}

void VulkanEngine::init_swapchain()
{
    create_swapchain(_windowExtent.width, _windowExtent.height);


                                                                                        /// --- image creation --- ///


    // the draw image size will match the window size.
    VkExtent3D drawImageExtent =
    {
        _windowExtent.width,
        _windowExtent.height,
        1
    };

    // hardcoding the draw format to 32 bit float
    _drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    _drawImage.imageExtent = drawImageExtent;

    // information about how we're going to use the image
    VkImageUsageFlags drawImageUsages{};
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // allows us to copy from the image
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT; // allows us to write to the image
    drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT; // allows the compute shader to write to the image
    drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // allows us to use the graphics pipeline to draw geometry onto the image

    VkImageCreateInfo rimg_info = vkinit::image_create_info(_drawImage.imageFormat, drawImageUsages, drawImageExtent);

    // for the draw image, we want to allocate it from GPU local memory.
    // by doing this, we ensure that VMA allocates the memory on the GPU's vram (ensuring the fastest access) instead of using the upload heap and copying it over from the CPU. 
    VmaAllocationCreateInfo rimg_allocinfo{};
    rimg_allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    rimg_allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // allocate and create the image
    vmaCreateImage(_allocator, &rimg_info, &rimg_allocinfo, &_drawImage.image, &_drawImage.allocation, nullptr);

    // build an image-view for the draw image to use for rendering
    VkImageViewCreateInfo rview_info = vkinit::imageview_create_info(_drawImage.imageFormat, _drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);

    VK_CHECK(vkCreateImageView(_device, &rview_info, nullptr, &_drawImage.imageView));

    // add to deletion queue.
    _mainDeletionQueue.push_function([=]()
        {
            vkDestroyImageView(_device, _drawImage.imageView, nullptr);
            vmaDestroyImage(_allocator, _drawImage.image, _drawImage.allocation);
        });
}

void VulkanEngine::init_commands()
{
                                                                    /// --- command pool creation & command buffer allocation --- ///

    
    // create a command pool for commands submitted to the graphics queue.
    // this is the creation info struct we need to actually create the command pools (see for loop).
    // we also want the pool to allow for resetting of individual command buffers.
  
    // we use this abstraction function to make struct creation easier.
    // the flag (second parameter) tells vulkan we're resetting individual command buffers made from the pool.
    // pass in the queue family so the command pool can create commands compatible with any queue from the family we'll use.
    VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(_graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

    for (int i = 0 ; i < FRAME_OVERLAP; i++)
    {
        // create the command pools for each frame
        VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_frames[i]._commandPool));

        // allocate the default command buffer that we will use for rendering, from the command pool we just created above. 
        VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_frames[i]._commandPool, 1);
  

        VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_frames[i]._mainCommandBuffer));
    }
}

void VulkanEngine::init_sync_structures()
{
    // create synchronization structures
    // one fence to know/control when the GPU has finished rendering the frame,
    // and 2 sempahores to synchronize rendering with the swapchain (1 semaphore to wait on the swapchain image request, then another for waiting until we finish drawing so we can present).
    // we want the fence to start signalled so we can wait on it on the first frame.
    VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
    VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();

    // for each frame, create the fence and semaphores
    for (int i = 0 ; i < FRAME_OVERLAP ; i++)
    {
        
        VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_frames[i]._renderFence));

        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &_frames[i]._swapchainSemaphore));
        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &_frames[i]._renderSemaphore));

    }
}

void VulkanEngine::cleanup()
{
    if (_isInitialized) {

        // make sure the GPU has stopped doing its things
        vkDeviceWaitIdle(_device);

        // free per-frame structures and deletion queue.
        for (int i = 0 ; i < FRAME_OVERLAP; i++)
        {
            vkDestroyCommandPool(_device, _frames[i]._commandPool, nullptr); // don't have to destroy any individual command buffer. destroying command pool will delete of its all buffers.
       
            //destroy sync objects
            vkDestroyFence(_device, _frames[i]._renderFence, nullptr);
            vkDestroySemaphore(_device, _frames[i]._renderSemaphore, nullptr);
            vkDestroySemaphore(_device, _frames[i]._swapchainSemaphore, nullptr);

            // free all objects from both frames.
            _frames[i]._deletionQueue.flush();
        }

        // flush the global deletion queue
        _mainDeletionQueue.flush();

        destroy_swapchain();

        vkDestroySurfaceKHR(_instance, _surface, nullptr);
        vkDestroyDevice(_device, nullptr);

        vkb::destroy_debug_utils_messenger(_instance, _debugMessenger);
        vkDestroyInstance(_instance, nullptr);

        SDL_DestroyWindow(_window);
    }

    // clear engine pointer
    loadedEngine = nullptr;
}




void VulkanEngine::draw_background(VkCommandBuffer cmdBuff)
{

    // make a clear-colour from frame buffer. This will flash with a 120 frame period.
    VkClearColorValue clearValue{};
    float flash = std::abs(std::sin(_frameNumber / 120.0f));
    clearValue = { { 0.0f, 0.0f, flash, 1.0f} };

    // we specify the images' subresource range (what part of the image we want to clear) so we can pass it to vkCmdClearColorImage just below.
    VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);

    // clear image, write it to the swapchain image.
    vkCmdClearColorImage(cmdBuff, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);
}




void VulkanEngine::draw()
{
    /// we follow this (rough) procedure:
    /// 1. Wait for render fences (meaning, we wait until the GPU has finished executing its commands and we can start writing commands again).
    /// 2. Request for a new swapchain image to draw to. We provide a swapchain semaphore so the GPU can wait until we fetch a swapchain image.
    /// 3. Grab the command buffer (handle) for the current frame, and reset it (so we can reuse it).
    /// 4. Create the info struct for  the command buffer so we can start it.
    /// 5. Start the command buffer recording. 
    /// 6. We transition vulkan images      (the GPU stores images in different formats, we have to convert a swapchain image layout, which is the vulkan abstraction over these formats,
    ///                                     into a format that we are able to write/draw to, and then transition it again into a layout we can display... we do this using a pipeline 
    ///                                     barrier, which is in <vk_images.h>).


                                                                                /// --- fences & command buffer setup --- ///



    // wait until the GPU has finished rendering the last frame. Timeout of 1 second.
    // we always want to wait for the fences at the very start of this function, because we'll call it again and again (we have to wait for the last frame).
    VK_CHECK(vkWaitForFences(_device, 1, &get_current_frame()._renderFence, true, 1000000000));

    get_current_frame()._deletionQueue.flush();

    VK_CHECK(vkResetFences(_device, 1, &get_current_frame()._renderFence));

    // request image (an index) from the swapchain
    // we pass in the swapchain semaphore so we can sync other operations with the swapchain when we have an image ready to render.
    // basically, we use the swapchain semaphore to wait until we get the next swapchain image. it becomes signalled when we get one.
    uint32_t swapchainImageIndex{};
    VK_CHECK(vkAcquireNextImageKHR(_device, _swapchain, 1000000000, get_current_frame()._swapchainSemaphore, nullptr, &swapchainImageIndex));

    // naming it cmdBuff for shorter writing.
    // we grab the current frames command buffer
    // we can copy the command buffer handle no problem, because Vulkan handles are just a 64 bit handle/pointer, so its fine to copy them around, but remember that their actual data is handled by vulkan itself.
    VkCommandBuffer cmdBuff = get_current_frame()._mainCommandBuffer;

    // now that we are sure that the commands finished executing, we can safely reset the command buffer to begin recording again.
    // we are resetting the current frames' command buffer (it was recorded to on the prior frame).
    VK_CHECK(vkResetCommandBuffer(cmdBuff, 0));

    // get the info struct for the command buffer so we can start it. We will use this command buffer exactly once, so we want to let vulkan know that
    VkCommandBufferBeginInfo cmdBuffBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    _drawExtent.width = _drawImage.imageExtent.width;
    _drawExtent.height = _drawImage.imageExtent.height;

    // start the command buffer recording
    VK_CHECK(vkBeginCommandBuffer(cmdBuff, &cmdBuffBeginInfo));


                                                                                    /// --- image transitions --- ///


    // transition our main draw image into general layout so we can write into it
    // we will overwrite it all so we dont care about what was the older layout
        // the "currentLayout" is undefined (the "don't care" layout), but we want a "newLayout" that is general (writable/readable, general purpose).
    vkutil::transition_image(cmdBuff, _drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    
    // add the draw background commands to the buffer, meaning we write onto the image.
    draw_background(cmdBuff);

    // transition the draw image and the swapchain image into their correct transfer layouts
    // we transition the draw image from a general layout (which we wrote to above), to a source transfer layout, which means we can copy the draw image onto some other image.
    // we transition the swapchain image from undefined ("don't care") to a destination transfer layout, which means we can write to the swapchain image using another image.
    // this allows us to copy over the drawImage we wrote to, onto the swapchain image so we can present it.
    vkutil::transition_image(cmdBuff, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkutil::transition_image(cmdBuff, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // execute a copy from the draw image into the swapchain
    vkutil::copy_image_to_image(cmdBuff, _drawImage.image, _swapchainImages[swapchainImageIndex], _drawExtent, _swapchainExtent);

    // set swapchain image layout to Present so we can show it on the screen
    vkutil::transition_image(cmdBuff, _swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // finalize the command buffer (we can no longer add commands, but it now be executed).
    VK_CHECK(vkEndCommandBuffer(cmdBuff));


                                                                                    /// --- submit command buffer --- //
    

    // prepare the submission to the queue.
        // we get the submit info for the command buffer, and the render/swapchain semaphores. we combine them into a VkSubmitInfo2 struct to finally submit the cmdbuffer to the queue.
    // we want to wait on _presentSemaphore, as that semaphore is signalled when the swapchain is ready
    // we will signal the _renderSemaphore, to signal that rendering has finished (so we can then present the image).

    VkCommandBufferSubmitInfo cmdInfo = vkinit::command_buffer_submit_info(cmdBuff);

    // when we called vkAcquireNextImageKHR, we set this same swapchain semaphore to be signalled. by doing this, we make sure that the commands executed here won't begin until
    // the swapchain is ready (when we run vkQueueSubmit where we pass in the cmdInfo, signalInfo, and waitInfo which is packaged together into the <submit>, Vulkan will wait until
    // the swapchain semaphore is signalled (meaning we got back an image from the swapchain), and then afterwards the commands on the buffer will be run... i think).
    // _rendersemaphore will be signalled once the command buffer has been submitted and executed. this allows us to present the image to the screen (written in the next section below)
    VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR, get_current_frame()._swapchainSemaphore);
    VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, get_current_frame()._renderSemaphore);

    VkSubmitInfo2 submit = vkinit::submit_info(&cmdInfo, &signalInfo, &waitInfo);

    // submit command buffer to the queue and execute it.
    // _renderFence will now block until the graphic commands finish execution.
    VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submit, get_current_frame()._renderFence));


                                                                                        /// --- present image --- ///


    // prepare present
    // this will put the image we just rendered to into the visible window.
    // we want to wait on the _renderSemaphore for that, 
    // as its necessary that drawing commands have finished before the image is displayed to the user
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.pSwapchains = &_swapchain;
    presentInfo.swapchainCount = 1;

    // we wait until the rendering is finished, before we present the image to the screen. 
    presentInfo.pWaitSemaphores = &get_current_frame()._renderSemaphore;
    presentInfo.waitSemaphoreCount = 1;

    presentInfo.pImageIndices = &swapchainImageIndex;

    VK_CHECK(vkQueuePresentKHR(_graphicsQueue, &presentInfo));

    // increase the number of frames drawn
    _frameNumber++;


}

void VulkanEngine::run()
{
    SDL_Event e;
    bool bQuit = false;

    // main loop
    while (!bQuit) {
        // Handle events on queue
        while (SDL_PollEvent(&e) != 0) {
            // close the window when user alt-f4s or clicks the X button
            if (e.type == SDL_QUIT)
                bQuit = true;

            if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    stop_rendering = true;
                }
                if (e.window.event == SDL_WINDOWEVENT_RESTORED) {
                    stop_rendering = false;
                }
            }

            /// log some simple input.
            if (e.type == SDL_KEYDOWN)
            {
                if(e.key.keysym.sym == SDLK_SPACE)
                {
                    fmt::print("Whas good!");
                }
            }

            if(e.type == SDL_KEYDOWN)
            {
                
                if (e.key.keysym.sym == SDLK_ESCAPE) { bQuit = true; }
                
            }
        
        }

        // do not draw if we are minimized
        if (stop_rendering) {
            // throttle the speed to avoid the endless spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        draw();
    }
}