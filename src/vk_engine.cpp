//> includes
#include "vk_engine.h"

#include "SDL2/SDL.h"
#include <SDL2/SDL_vulkan.h>

// --- other includes --- //
#include <vk_initializers.h>
#include <vk_types.h>
#include <vk_images.h>
#include <vk_pipelines.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_vulkan.h"

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

    init_descriptors();

    init_pipelines();
    
    init_imgui();

    init_default_data();

    // everything went fine
    _isInitialized = true;

    fmt::println("Engine has been initialized successfully.\n");
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

    // initializing the (immediate) command structures for use with imgui

    VK_CHECK(vkCreateCommandPool(_device, &commandPoolInfo, nullptr, &_immediateCommandPool));

    // allocate the command buffer for immediate submits
    VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(_immediateCommandPool, 1);

    VK_CHECK(vkAllocateCommandBuffers(_device, &cmdAllocInfo, &_immediateCommandBuffer));

    _mainDeletionQueue.push_function([=]() {vkDestroyCommandPool(_device, _immediateCommandPool, nullptr); });

}

void VulkanEngine::init_sync_structures()
{
    // create synchronization structures
    // one fence to know/control when the GPU has finished rendering the frame,
    // and 2 sempahores to synchronize rendering with the swapchain (1 semaphore to wait on the swapchain image request, then another for waiting until we finish drawing so we can present).
    // we want the fence to start signalled so we can wait on it on the first frame.
    VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
    VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();

    _renderSemaphores.resize(_swapchainImages.size()); // resizing here, will zero-initialize the vector properly, so once vkCreateSemaphore is run, it'll overwrite the values with real vulkan semaphore handles.

    /// SEMAPHORES MUST BE PER-SWAPCHAIN-IMAGE!
    // this is a bit confusing, but take it slow. 
        // _renderSemaphores is a zero-initialized vector; i.e., it does not have any vkSemaphore's stored within in it. so how can we loop through the vector and run vkCreateSemaphore on it?
        // the reason why is because as long as the size of the vector is known and we zero-initialized every element in the array (we did this by using .resize() above),
        // we can run vkCreateSemaphore() on those zero-initialized elements and it will return the handle for a semaphore for us. Vulkan will basically GIVE us a valid semaphore
        // if we run vkCreateSemaphore() (or rather, it will overwrite a valid handle to that memory address; as we pass in the address of the semaphore), even on random values. 
        // The VkSemaphore object itself is actually just a handle! When we call VkCreateSemaphore, it'll handle the allocation and back-end business and give us a handle, which we 
        // store inside of a VkSemaphore object/handle. So when we had <VkSemaphore _renderSemaphore> in the <FrameData> struct before, that wasn't an actual object (like a class you 
        // would expect), it was just a handle which points to the GPU semaphore or whatever back-end shenannigans are happening (after you run VkCreateSemaphore).
        // Basically: VkSemaphore is just a variable to hold the handle/memory address of the actual semaphore that lives inside Vulkan (which you store in the VkSemaphore after using vkCreateSemaphore()).
        // So we don't have to actually STORE VkSemaphore's in the vector! 
    for (auto& sema : _renderSemaphores)
    {
        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &sema));
    }

    // for each frame, create the fence and semaphores
    for (int i = 0 ; i < FRAME_OVERLAP ; i++)
    {
        
        VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_frames[i]._renderFence));

        VK_CHECK(vkCreateSemaphore(_device, &semaphoreCreateInfo, nullptr, &_frames[i]._swapchainSemaphore));
    }

    VK_CHECK(vkCreateFence(_device, &fenceCreateInfo, nullptr, &_immediateFence));
    _mainDeletionQueue.push_function([=]() { vkDestroyFence(_device, _immediateFence, nullptr); });
}

void VulkanEngine::init_descriptors()
{
    // create a descriptor pool that will hold 10 sets with 1 image each.
    std::vector<DescriptorAllocator::PoolSizeRatio> sizes =
    {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 } // in the PoolSizeRatio struct, we initialize the type and ratio.
    };
    
    globalDescriptorAllocator.init_pool(_device, 10, sizes);

    // make the descriptor set layout for our compute draw
    {
        // the descriptor layout is a layout with only 1 binding at binding number 0, of type VK_DESCRIPTOR_TYPE_STORAGE_IMAGE (matching the pool).
        DescriptorLayoutBuilder builder;
        builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        _drawImageDescriptorLayout = builder.build(_device, VK_SHADER_STAGE_COMPUTE_BIT);
    }

    // allocate a descriptor set for our draw image.
    _drawImageDescriptors = globalDescriptorAllocator.allocate(_device, _drawImageDescriptorLayout);

    // a struct that specifies descriptor image information. this will hold the image data we want to bind.
    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    imgInfo.imageView = _drawImage.imageView;

    VkWriteDescriptorSet drawImageWrite{};
    drawImageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    drawImageWrite.pNext = nullptr;

    drawImageWrite.dstBinding = 0; // this specifies in the descriptor set, which descriptor binding we are writing to (using its binding index).
    drawImageWrite.dstSet = _drawImageDescriptors; // the descriptor set we're writing to.
    drawImageWrite.descriptorCount = 1;
    drawImageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    drawImageWrite.pImageInfo = &imgInfo;

    vkUpdateDescriptorSets(_device, 1, &drawImageWrite, 0, nullptr);

    // ensure that both the descriptor allocator and the new layout get cleaned up properly.
    _mainDeletionQueue.push_function([&]()
        {
            globalDescriptorAllocator.destroy_pool(_device);

            vkDestroyDescriptorSetLayout(_device, _drawImageDescriptorLayout, nullptr);
        });

}

void VulkanEngine::init_pipelines()
{
    /// Compute Pipelines
    init_background_pipelines();

    /// Graphics Pipelines
    init_triangle_pipeline();
    init_mesh_pipelines();
}

void VulkanEngine::init_background_pipelines()
{
    VkPipelineLayoutCreateInfo computeLayout{};
    computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    computeLayout.pNext = nullptr;
    computeLayout.pSetLayouts = &_drawImageDescriptorLayout;
    computeLayout.setLayoutCount = 1;

    VkPushConstantRange pushConstant{};
    pushConstant.offset = 0;
    pushConstant.size = sizeof(ComputePushConstants);
    pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    computeLayout.pPushConstantRanges = &pushConstant;
    computeLayout.pushConstantRangeCount = 1;

    VK_CHECK(vkCreatePipelineLayout(_device, &computeLayout, nullptr, &_gradientPipelineLayout));

    VkShaderModule gradientShader{};
    if (!vkutil::load_shader_module((_shaderPath + "gradient_color.comp.spv").c_str(), _device, &gradientShader))
    {
        fmt::print("Error when building the compute shader.\n");
    }

    VkShaderModule skyShader{};
    if (!vkutil::load_shader_module((_shaderPath + "sky.comp.spv").c_str(), _device, &skyShader))
    {
        fmt::print("Error when building the compute shader.\n");
    }

    VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stageInfo.pNext = nullptr;
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.module = gradientShader;
    stageInfo.pName = "main";

    VkComputePipelineCreateInfo computePipelineCreateInfo{};
    computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    computePipelineCreateInfo.pNext = nullptr;
    computePipelineCreateInfo.layout = _gradientPipelineLayout;
    computePipelineCreateInfo.stage = stageInfo;

    ComputeEffect gradient;
    gradient.pipelineLayout = _gradientPipelineLayout;
    gradient.name = "gradient";
    gradient.pcData = {};

    // default colours
    gradient.pcData.data1 = glm::vec4(1, 0, 0, 1);
    gradient.pcData.data2 = glm::vec4(0, 0, 1, 1);

    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &gradient.pipeline));

    // change the shader module only to create the sky shader (so we can reuse the computePipelineCreateInfo struct for the other pipeline)
    computePipelineCreateInfo.stage.module = skyShader;

    ComputeEffect sky;
    sky.pipelineLayout = _gradientPipelineLayout;
    sky.name = "sky";
    sky.pcData = {};
    // default sky parameters
    sky.pcData.data1 = glm::vec4(0.1, 0.2, 0.4, 0.97);

    VK_CHECK(vkCreateComputePipelines(_device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &sky.pipeline));

    // add the 2 background effects (ComputeEffect's) into the array
    backgroundEffects.push_back(gradient);
    backgroundEffects.push_back(sky);

    // destroy structures properly
    vkDestroyShaderModule(_device, gradientShader, nullptr);
    vkDestroyShaderModule(_device, skyShader, nullptr);
    _mainDeletionQueue.push_function([=]()
        {
            vkDestroyPipelineLayout(_device, _gradientPipelineLayout, nullptr);
            vkDestroyPipeline(_device, sky.pipeline, nullptr);
            vkDestroyPipeline(_device, gradient.pipeline, nullptr);

        });








}

void VulkanEngine::draw_background(VkCommandBuffer cmdBuff)
{
    ComputeEffect& effect = backgroundEffects[currentBackgroundEffect];

    // bind the background compute pipeline.
    vkCmdBindPipeline(cmdBuff, VK_PIPELINE_BIND_POINT_COMPUTE, effect.pipeline);

    // bind the descriptor set containing the draw image for the compute pipeline
    vkCmdBindDescriptorSets(cmdBuff, VK_PIPELINE_BIND_POINT_COMPUTE, _gradientPipelineLayout, 0, 1, &_drawImageDescriptors, 0, nullptr);

    ComputePushConstants pc;
    pc.data1 = glm::vec4(1, 0, 0, 1);
    pc.data2 = glm::vec4(0, 0, 1, 1);

    // update the values of push constants
    vkCmdPushConstants(cmdBuff, _gradientPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &effect.pcData);

    // execute the compute pipeline dispatch. we are using a 16x16 workgroup size, so we need to divide by it; as that will tell us how many workgroups we have for our resolution.
    vkCmdDispatch(cmdBuff, std::ceil(_drawExtent.width / 16.0), std::ceil(_drawExtent.height / 16.0), 1);
}

void VulkanEngine::immediate_submit(std::function<void(VkCommandBuffer cmd)>&& function)
{
    VK_CHECK(vkResetFences(_device, 1, &_immediateFence));
    VK_CHECK(vkResetCommandBuffer(_immediateCommandBuffer, 0));

    VkCommandBuffer cmd = _immediateCommandBuffer;

    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

    function(cmd); // record the function(s) into the command buffer

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cmdInfo = vkinit::command_buffer_submit_info(cmd);
    VkSubmitInfo2 submitInfo = vkinit::submit_info(&cmdInfo, nullptr, nullptr);

    // submit command buffer to the queue and execute it.
    // _renderFence will now block until the graphics commands finish execution.
    VK_CHECK(vkQueueSubmit2(_graphicsQueue, 1, &submitInfo, _immediateFence));

    // we can wait for the fence at the very end of the function, as we don't call it again and again (unlike the <draw> function below). 
    VK_CHECK(vkWaitForFences(_device, 1, &_immediateFence, true, 9999999999));
}

void VulkanEngine::init_imgui()
{
    // 1: Create a descriptor pool for IMGUI
    //  the size of the pool is very oversized, but it's copied from the imgui demo itself.
    VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 } };

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000;
    pool_info.poolSizeCount = (uint32_t)std::size(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;

    VkDescriptorPool imguiPool;
    VK_CHECK(vkCreateDescriptorPool(_device, &pool_info, nullptr, &imguiPool));

    // 2: Initialize the imgui library

    // this initializes the core structures of imgui
    ImGui::CreateContext();

    // this initializes imgui for SDL
    ImGui_ImplSDL2_InitForVulkan(_window);

    // this initializes imgui for Vulkan
    ImGui_ImplVulkan_InitInfo init_info{};
    init_info.Instance = _instance;
    init_info.PhysicalDevice = _chosenGPU;
    init_info.Device = _device;
    init_info.Queue = _graphicsQueue;
    init_info.DescriptorPool = imguiPool;
    init_info.MinImageCount = 3;
    init_info.ImageCount = 3;
    init_info.UseDynamicRendering = true;

    // dynamic rendering parameters for imgui to use
    init_info.PipelineRenderingCreateInfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    init_info.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init_info.PipelineRenderingCreateInfo.pColorAttachmentFormats = &_swapchainImageFormat;

    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    ImGui_ImplVulkan_Init(&init_info);

    ImGui_ImplVulkan_CreateFontsTexture();

    // add the destruction functions for the imgui structures
    _mainDeletionQueue.push_function([=]() {
        ImGui_ImplVulkan_Shutdown(); 
        vkDestroyDescriptorPool(_device, imguiPool, nullptr); 
        });
}

void::VulkanEngine::draw_imgui(VkCommandBuffer cmd, VkImageView targetImageView)
{

    /// a rendering attachment is simply the image that we use during rendering. it is NOT it's own separate object; it is simply an image we already have (via a VkImageView), but used for rendering. 
    /// we're just changing our use of it. We take our <VkImageView> that we want to render to (and eventually present to the screen), which in this case, are our swapchain images, and we use these 
    /// <VkImageView>'s as attachments. This specifies HOW we're doing to use our images during rendering, as there are many, many uses for images, so we have to specify that we want to use this as a (colour)
    /// attachment and thus output target for our rendering operations notice how we call <draw_imgui> with the swapchain image as the <targetImageView>; so the render target (the image we want to render to) 
    /// is the swapchain image. Also, we attach a <VkImageView> and not a <VkImage> as an attachment, because Vulkan needs some important information that only the <VkImageView> has and not <VkImage>.
    /// 
    /// TLDR: attachments are just images but used in a specific role for rendering. It's just an image but used in rendering. So we're using our swapchain images here as a colour attachment, so we can render
    /// colour onto the image. That's it.
    VkRenderingAttachmentInfo colourAttachment = vkinit::attachment_info(targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(_swapchainExtent, &colourAttachment, nullptr);

    vkCmdBeginRendering(cmd, &renderInfo);

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

    vkCmdEndRendering(cmd);
}

void VulkanEngine::init_triangle_pipeline()
{
    VkShaderModule triangleFragShader;
    if (!vkutil::load_shader_module((_shaderPath + "colored_triangle.frag.spv").c_str(), _device, &triangleFragShader)) {
        fmt::print("Error when building the triangle fragment shader module.\n");
    }
    else {
        fmt::print("Triangle fragment shader succesfully loaded.\n");
    }

    VkShaderModule triangleVertexShader;
    if (!vkutil::load_shader_module((_shaderPath + "colored_triangle.vert.spv").c_str(), _device, &triangleVertexShader)) {
        fmt::print("Error when building the triangle vertex shader module.\n");
    }
    else {
        fmt::print("Triangle vertex shader succesfully loaded.\n");
    }

    //build the pipeline layout that controls the inputs/outputs of the shader
    //we are not using descriptor sets or other systems yet (as we're using vertex pulling - no vertex attributes are used), so no need to use anything other than empty default
    VkPipelineLayoutCreateInfo pipeline_layout_info = vkinit::pipeline_layout_create_info();
    VK_CHECK(vkCreatePipelineLayout(_device, &pipeline_layout_info, nullptr, &_trianglePipelineLayout));

    PipelineBuilder pipelineBuilder;


    //use the triangle layout we created
    pipelineBuilder._pipelineLayout = _trianglePipelineLayout;
    //connecting the vertex and pixel shaders to the pipeline
    pipelineBuilder.set_shaders(triangleVertexShader, triangleFragShader);
    //it will draw triangles
    pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    //filled triangles
    pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    //no backface culling
    pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    //no multisampling
    pipelineBuilder.set_multisampling_none();
    //no blending
    pipelineBuilder.disable_blending();
    //no depth testing
    pipelineBuilder.disable_depthtest();

    //connect the image format we will draw into, from draw image
    pipelineBuilder.set_color_attachment_format(_drawImage.imageFormat);
    pipelineBuilder.set_depth_format(VK_FORMAT_UNDEFINED);

    //finally build the pipeline
    _trianglePipeline = pipelineBuilder.build_pipeline(_device);

    //clean structures
    vkDestroyShaderModule(_device, triangleFragShader, nullptr);
    vkDestroyShaderModule(_device, triangleVertexShader, nullptr);

    _mainDeletionQueue.push_function([&]() {
        vkDestroyPipelineLayout(_device, _trianglePipelineLayout, nullptr);
        vkDestroyPipeline(_device, _trianglePipeline, nullptr);
        });
}

void VulkanEngine::draw_geometry(VkCommandBuffer cmdBuff)
{
    //begin a render pass  connected to our draw image
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(_drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    VkRenderingInfo renderInfo = vkinit::rendering_info(_drawExtent, &colorAttachment, nullptr);
    vkCmdBeginRendering(cmdBuff, &renderInfo);

    vkCmdBindPipeline(cmdBuff, VK_PIPELINE_BIND_POINT_GRAPHICS, _trianglePipeline);

    //set dynamic viewport and scissor.
    // we do this here, as we're using dynamic state for the viewport and scissor, so we set them right before drawing. 
    VkViewport viewport {};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = _drawExtent.width;
    viewport.height = _drawExtent.height;
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;

    vkCmdSetViewport(cmdBuff, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent.width = _drawExtent.width;
    scissor.extent.height = _drawExtent.height;

    vkCmdSetScissor(cmdBuff, 0, 1, &scissor);

    //launch a draw command to draw 3 vertices
    vkCmdDraw(cmdBuff, 3, 1, 0, 0);

    // bind the pipeline to render a mesh
    vkCmdBindPipeline(cmdBuff, VK_PIPELINE_BIND_POINT_GRAPHICS, _meshPipeline);

    // congigure our push constants before we send them to the GPU
    GPUDrawPushConstants push_constants;
    push_constants.worldMatrix = glm::mat4{ 1.f };
    push_constants.vertexBuffer = rectangle.vertexBufferAddress;

    // Send the vertex buffer address & world transformation matrix to the GPU via push constants. 
    // Also bind the index buffer.
    vkCmdPushConstants(cmdBuff, _meshPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GPUDrawPushConstants), &push_constants);
    vkCmdBindIndexBuffer(cmdBuff, rectangle.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    vkCmdDrawIndexed(cmdBuff, 6, 1, 0, 0, 0);

    vkCmdEndRendering(cmdBuff);
}

AllocatedBuffer VulkanEngine::create_buffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
{
    // allocate buffer
    VkBufferCreateInfo bufferInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.pNext = nullptr;
    bufferInfo.size = allocSize;

    bufferInfo.usage = usage;

    VmaAllocationCreateInfo vmaallocInfo = {};
    vmaallocInfo.usage = memoryUsage;
    vmaallocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT; // don't really get how this specific allocation works but whatevah
    AllocatedBuffer newBuffer;

    // allocate the buffer
    VK_CHECK(vmaCreateBuffer(_allocator, &bufferInfo, &vmaallocInfo, &newBuffer.buffer, &newBuffer.allocation,
        &newBuffer.info));

    return newBuffer;
}

void VulkanEngine::destroy_buffer(const AllocatedBuffer& buffer)
{
    vmaDestroyBuffer(_allocator, buffer.buffer, buffer.allocation);
}

GPUMeshBuffers VulkanEngine::uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices)
{
    const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
    const size_t indexBufferSize = indices.size() * sizeof(uint32_t);

    GPUMeshBuffers newSurface;

    //create vertex buffer
    newSurface.vertexBuffer = create_buffer(vertexBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    //find the address of the vertex buffer
    VkBufferDeviceAddressInfo deviceAdressInfo{ .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer = newSurface.vertexBuffer.buffer };
    newSurface.vertexBufferAddress = vkGetBufferDeviceAddress(_device, &deviceAdressInfo);

    //create index buffer
    newSurface.indexBuffer = create_buffer(indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_GPU_ONLY);

    // as the vertexBuffer and indexBuffer are both GPU only buffers, we cannot write to them from the CPU.
    // so, we create this temporary buffer (the staging buffer), which can be written to by the CPU. 
    // This allows us to first write the memory on the staging buffer, and then copy it over to the GPU buffers (vertexBuffer & indexBuffer).
    AllocatedBuffer staging = create_buffer(vertexBufferSize + indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

    void* data = staging.allocation->GetMappedData();

    // copy vertex buffer
    memcpy(data, vertices.data(), vertexBufferSize);
    // copy index buffer
    // since the staging buffer holds both the vertex and index buffers, we add the vertexBufferSize to get the position in memory for the index buffer (from 0 to vertexBufferSize, it's the vertex buffer, 
    // so past vertexBufferSize 
    memcpy((char*)data + vertexBufferSize, indices.data(), indexBufferSize);

    // Send the command (via immediate submit) to the GPU to copy the buffers. 
    immediate_submit([&](VkCommandBuffer cmd) {
        VkBufferCopy vertexCopy{ 0 };
        vertexCopy.dstOffset = 0;
        vertexCopy.srcOffset = 0;
        vertexCopy.size = vertexBufferSize;

        vkCmdCopyBuffer(cmd, staging.buffer, newSurface.vertexBuffer.buffer, 1, &vertexCopy);

        VkBufferCopy indexCopy{ 0 };
        indexCopy.dstOffset = 0;
        indexCopy.srcOffset = vertexBufferSize;
        indexCopy.size = indexBufferSize;

        vkCmdCopyBuffer(cmd, staging.buffer, newSurface.indexBuffer.buffer, 1, &indexCopy);
        });

    destroy_buffer(staging);

    return newSurface;
}

void VulkanEngine::init_mesh_pipelines()
{
    VkShaderModule triangleFragShader;
    if (!vkutil::load_shader_module((_shaderPath + "colored_triangle.frag.spv").c_str(), _device, &triangleFragShader)) {
        fmt::print("Error when building the mesh triangle fragment shader module.\n");
    }
    else {
        fmt::print("Mesh triangle fragment shader succesfully loaded.\n");
    }

    VkShaderModule triangleVertexShader;
    if (!vkutil::load_shader_module((_shaderPath + "colored_triangle_mesh.vert.spv").c_str(), _device, &triangleVertexShader)) {
        fmt::print("Error when building the mesh triangle vertex shader module.\n");
    }
    else {
        fmt::print("Mesh triangle vertex shader succesfully loaded.\n");
    }

    VkPushConstantRange bufferRange{};
    bufferRange.offset = 0;
    bufferRange.size = sizeof(GPUDrawPushConstants);
    bufferRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkPipelineLayoutCreateInfo pipeline_layout_info = vkinit::pipeline_layout_create_info();
    pipeline_layout_info.pPushConstantRanges = &bufferRange;
    pipeline_layout_info.pushConstantRangeCount = 1;

    VK_CHECK(vkCreatePipelineLayout(_device, &pipeline_layout_info, nullptr, &_meshPipelineLayout));

    PipelineBuilder pipelineBuilder;

    //use the triangle layout we created
    pipelineBuilder._pipelineLayout = _meshPipelineLayout;
    //connecting the vertex and pixel shaders to the pipeline
    pipelineBuilder.set_shaders(triangleVertexShader, triangleFragShader);
    //it will draw triangles
    pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    //filled triangles
    pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
    //no backface culling
    pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
    //no multisampling
    pipelineBuilder.set_multisampling_none();
    //no blending
    pipelineBuilder.disable_blending();

    pipelineBuilder.disable_depthtest();

    //connect the image format we will draw into, from draw image
    pipelineBuilder.set_color_attachment_format(_drawImage.imageFormat);
    pipelineBuilder.set_depth_format(VK_FORMAT_UNDEFINED);

    //finally build the pipeline
    _meshPipeline = pipelineBuilder.build_pipeline(_device);

    //clean structures
    vkDestroyShaderModule(_device, triangleFragShader, nullptr);
    vkDestroyShaderModule(_device, triangleVertexShader, nullptr);

    _mainDeletionQueue.push_function([&]() {
        vkDestroyPipelineLayout(_device, _meshPipelineLayout, nullptr);
        vkDestroyPipeline(_device, _meshPipeline, nullptr);
        });
}

void VulkanEngine::init_default_data()
{
    std::array<Vertex, 4> rect_vertices;

    rect_vertices[0].position = { 0.5,-0.5, 0 };
    rect_vertices[1].position = { 0.5,0.5, 0 };
    rect_vertices[2].position = { -0.5,-0.5, 0 };
    rect_vertices[3].position = { -0.5,0.5, 0 };

    rect_vertices[0].color = { 0,0, 0,1 };
    rect_vertices[1].color = { 0.5,0.5,0.5 ,1 };
    rect_vertices[2].color = { 1,0, 0,1 };
    rect_vertices[3].color = { 0,1, 0,1 };

    std::array<uint32_t, 6> rect_indices;

    rect_indices[0] = 0;
    rect_indices[1] = 1;
    rect_indices[2] = 2;

    rect_indices[3] = 2;
    rect_indices[4] = 1;
    rect_indices[5] = 3;

    // convert the vertices and indices into buffers.
    rectangle = uploadMesh(rect_indices, rect_vertices);

    //delete the rectangle data on engine shutdown
    _mainDeletionQueue.push_function([&]() {
        destroy_buffer(rectangle.indexBuffer);
        destroy_buffer(rectangle.vertexBuffer);
        });
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
    /// 7. end command buffer recording.
    /// 8. submit the command buffer (to a queue).
    /// 9. wait for _renderSemaphore so we know when rendering has finished.
    /// 10. present the image!!

    
                                                                                /// --- fences & command buffer setup --- ///



    // wait until the GPU has finished rendering the last frame. Timeout of 1 second.
    // we always want to wait for the fences at the very start of this function, because we'll call it again and again (we have to wait for the last frame).
    VK_CHECK(vkWaitForFences(_device, 1, &get_current_frame()._renderFence, true, 1000000000));

    get_current_frame()._deletionQueue.flush();

    VK_CHECK(vkResetFences(_device, 1, &get_current_frame()._renderFence));

    // request image (i.e., the function returns the index of the next available presentable image) from the swapchain
    // we pass in the swapchain semaphore so we can sync other operations with the swapchain when we have an image ready to render.
    // basically, we use the swapchain semaphore to wait until we get the next swapchain image. it becomes signalled when we get one.
    // uint32_t swapchainImageIndex{};
    VK_CHECK(vkAcquireNextImageKHR(_device, _swapchain, 1000000000, get_current_frame()._swapchainSemaphore, nullptr, &_swapchainImageIndex));

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

    // transition the image into a <VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL> so we can render geometry onto the image. It is possible to use the general layout when rendering geometry, but using 
    // that results in lower performance and validation layer warnings. 
    vkutil::transition_image(cmdBuff, _drawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    draw_geometry(cmdBuff);

    // transition the draw image and the swapchain image into their correct transfer layouts
    // we transition the draw image from a general layout (which we wrote to above), to a source transfer layout, which means we can copy the draw image onto some other image.
    // we transition the swapchain image from undefined ("don't care") to a destination transfer layout, which means we can write to the swapchain image using another image.
    // this allows us to copy over the drawImage we wrote to, onto the swapchain image so we can present it.
    vkutil::transition_image(cmdBuff, _drawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkutil::transition_image(cmdBuff, _swapchainImages[_swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    // execute a copy from the draw image into the swapchain
    vkutil::copy_image_to_image(cmdBuff, _drawImage.image, _swapchainImages[_swapchainImageIndex], _drawExtent, _swapchainExtent);

    // set swapchain image layout to Attachment Optimal so we can draw it
    vkutil::transition_image(cmdBuff, _swapchainImages[_swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    // draw imgui into the swapchain image
    draw_imgui(cmdBuff, _swapchainImageViews[_swapchainImageIndex]);

    // set swapchain image layout to Present so we can show it on the screen
    vkutil::transition_image(cmdBuff, _swapchainImages[_swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

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
    VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, _renderSemaphores[_swapchainImageIndex]);

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
    presentInfo.pWaitSemaphores = &_renderSemaphores[_swapchainImageIndex];

    presentInfo.waitSemaphoreCount = 1;

    presentInfo.pImageIndices = &_swapchainImageIndex;
 
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

            // send SDL event to imgui for handling
            ImGui_ImplSDL2_ProcessEvent(&e);

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

        // make new frames for imgui
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        if (ImGui::Begin("background"))
        {
            ComputeEffect& selected = backgroundEffects[currentBackgroundEffect];
            
            ImGui::Text("Selected effect: ", selected.name);
        
            ImGui::SliderInt("Effect Index", &currentBackgroundEffect, 0, backgroundEffects.size() - 1);
            
            ImGui::InputFloat4("data1", (float*)&selected.pcData.data1);
            ImGui::InputFloat4("data2", (float*)&selected.pcData.data2);
            ImGui::InputFloat4("data3", (float*)&selected.pcData.data3);
            ImGui::InputFloat4("data4", (float*)&selected.pcData.data4);
        }

      
        ImGui::End();

        // some imgui UI that we can use for testing
      //   ImGui::ShowDemoWindow();

        // make imgui calculate the internal draw structures (we have to draw it to the screen ourselves; this function just makes imgui do the magical calculations in the backend that we can use).
        ImGui::Render();

        draw();
    }
}



void VulkanEngine::cleanup()
{
    if (_isInitialized) {

        // make sure the GPU has stopped doing its things
        vkDeviceWaitIdle(_device);

        // free per-frame structures and deletion queue.
        for (int i = 0; i < FRAME_OVERLAP; i++)
        {
            vkDestroyCommandPool(_device, _frames[i]._commandPool, nullptr); // don't have to destroy any individual command buffer. destroying command pool will delete of its all buffers.

            //destroy sync objects
            vkDestroyFence(_device, _frames[i]._renderFence, nullptr);
            vkDestroySemaphore(_device, _frames[i]._swapchainSemaphore, nullptr);

            // free all objects from both frames.
            _frames[i]._deletionQueue.flush();
        }

        // delete the render semaphores separately. the amount of semaphores we have is the same as the amount of swapchain images we have.
        for (int i = 0; i < _swapchainImages.size(); i++)
        {
            vkDestroySemaphore(_device, _renderSemaphores[i], nullptr);
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
