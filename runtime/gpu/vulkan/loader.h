#pragma once
// Vulkan through function pointers, for a build that decides at run time
// which implementation to talk to.
//
// A desktop build links the loader and calls vkSomething directly. Android
// cannot: the driver may be one the player imported as a zip (Mesa's Turnip,
// runtime/android/driver.cpp), which the system's loader knows nothing about,
// and the platform's own libvulkan.so exports only the core entry points --
// everything with a KHR or EXT on the end has to be asked for by name anyway.
//
// So every entry point the runtime calls is a pointer with the same name as
// the function, filled in from vkGetInstanceProcAddr. The call sites do not
// change: `vkCmdDraw(...)` is a call through a pointer instead of a call to
// the loader's stub. The list is generated (tools/gen_vulkan_loader.py) from
// the calls the sources actually make.
//
// Device-level entry points are loaded again from vkGetDeviceProcAddr once
// the device exists. That skips the loader's dispatch trampoline on every
// draw call, which is a measurable share of a frame on a phone where the
// renderer submits thousands of them.
//
// This header is force-included into the Vulkan sources by CMake, so it is
// the first thing every one of them sees.
#if defined(PES17_VULKAN_DYNAMIC) && defined(PES17_HAVE_VULKAN)

#ifndef VK_NO_PROTOTYPES
#   define VK_NO_PROTOTYPES 1
#endif
#if defined(__SWITCH__) && !defined(VK_USE_PLATFORM_VI_NN)
#   define VK_USE_PLATFORM_VI_NN 1
#endif

#include <vulkan/vulkan.h>

// Keep our dispatch slots distinct from the public vk* functions exported by
// the statically linked Mesa loaderless driver. All runtime call sites use
// these slots; switch_driver.cpp still obtains the real GIPA entry point.
#define vkCreateInstance pes17_dispatch_vkCreateInstance
#define vkEnumerateInstanceExtensionProperties pes17_dispatch_vkEnumerateInstanceExtensionProperties
#define vkEnumerateInstanceVersion pes17_dispatch_vkEnumerateInstanceVersion
#define vkCreateAndroidSurfaceKHR pes17_dispatch_vkCreateAndroidSurfaceKHR
#define vkCreateViSurfaceNN pes17_dispatch_vkCreateViSurfaceNN
#define vkCreateDevice pes17_dispatch_vkCreateDevice
#define vkDestroyDevice pes17_dispatch_vkDestroyDevice
#define vkDestroyInstance pes17_dispatch_vkDestroyInstance
#define vkDestroySurfaceKHR pes17_dispatch_vkDestroySurfaceKHR
#define vkEnumerateDeviceExtensionProperties pes17_dispatch_vkEnumerateDeviceExtensionProperties
#define vkEnumeratePhysicalDevices pes17_dispatch_vkEnumeratePhysicalDevices
#define vkGetDeviceProcAddr pes17_dispatch_vkGetDeviceProcAddr
#define vkGetPhysicalDeviceFeatures pes17_dispatch_vkGetPhysicalDeviceFeatures
#define vkGetPhysicalDeviceFeatures2 pes17_dispatch_vkGetPhysicalDeviceFeatures2
#define vkGetPhysicalDeviceFormatProperties pes17_dispatch_vkGetPhysicalDeviceFormatProperties
#define vkGetPhysicalDeviceMemoryProperties pes17_dispatch_vkGetPhysicalDeviceMemoryProperties
#define vkGetPhysicalDeviceProperties pes17_dispatch_vkGetPhysicalDeviceProperties
#define vkGetPhysicalDeviceProperties2 pes17_dispatch_vkGetPhysicalDeviceProperties2
#define vkGetPhysicalDeviceQueueFamilyProperties pes17_dispatch_vkGetPhysicalDeviceQueueFamilyProperties
#define vkGetPhysicalDeviceQueueFamilyProperties2 pes17_dispatch_vkGetPhysicalDeviceQueueFamilyProperties2
#define vkGetPhysicalDeviceSurfaceCapabilitiesKHR pes17_dispatch_vkGetPhysicalDeviceSurfaceCapabilitiesKHR
#define vkGetPhysicalDeviceSurfaceFormatsKHR pes17_dispatch_vkGetPhysicalDeviceSurfaceFormatsKHR
#define vkGetPhysicalDeviceSurfacePresentModesKHR pes17_dispatch_vkGetPhysicalDeviceSurfacePresentModesKHR
#define vkGetPhysicalDeviceSurfaceSupportKHR pes17_dispatch_vkGetPhysicalDeviceSurfaceSupportKHR
#define vkAcquireNextImageKHR pes17_dispatch_vkAcquireNextImageKHR
#define vkAllocateCommandBuffers pes17_dispatch_vkAllocateCommandBuffers
#define vkAllocateDescriptorSets pes17_dispatch_vkAllocateDescriptorSets
#define vkAllocateMemory pes17_dispatch_vkAllocateMemory
#define vkBeginCommandBuffer pes17_dispatch_vkBeginCommandBuffer
#define vkBindBufferMemory pes17_dispatch_vkBindBufferMemory
#define vkBindImageMemory pes17_dispatch_vkBindImageMemory
#define vkCmdBeginQuery pes17_dispatch_vkCmdBeginQuery
#define vkCmdBeginRenderPass pes17_dispatch_vkCmdBeginRenderPass
#define vkCmdBindDescriptorSets pes17_dispatch_vkCmdBindDescriptorSets
#define vkCmdBindIndexBuffer pes17_dispatch_vkCmdBindIndexBuffer
#define vkCmdBindPipeline pes17_dispatch_vkCmdBindPipeline
#define vkCmdBlitImage pes17_dispatch_vkCmdBlitImage
#define vkCmdClearColorImage pes17_dispatch_vkCmdClearColorImage
#define vkCmdClearDepthStencilImage pes17_dispatch_vkCmdClearDepthStencilImage
#define vkCmdCopyBufferToImage pes17_dispatch_vkCmdCopyBufferToImage
#define vkCmdCopyImage pes17_dispatch_vkCmdCopyImage
#define vkCmdCopyImageToBuffer pes17_dispatch_vkCmdCopyImageToBuffer
#define vkCmdDispatch pes17_dispatch_vkCmdDispatch
#define vkCmdDraw pes17_dispatch_vkCmdDraw
#define vkCmdDrawIndexed pes17_dispatch_vkCmdDrawIndexed
#define vkCmdEndQuery pes17_dispatch_vkCmdEndQuery
#define vkCmdEndRenderPass pes17_dispatch_vkCmdEndRenderPass
#define vkCmdPipelineBarrier pes17_dispatch_vkCmdPipelineBarrier
#define vkCmdPushConstants pes17_dispatch_vkCmdPushConstants
#define vkCmdResetQueryPool pes17_dispatch_vkCmdResetQueryPool
#define vkCmdResolveImage pes17_dispatch_vkCmdResolveImage
#define vkCmdSetDepthBias pes17_dispatch_vkCmdSetDepthBias
#define vkCmdSetScissor pes17_dispatch_vkCmdSetScissor
#define vkCmdSetStencilCompareMask pes17_dispatch_vkCmdSetStencilCompareMask
#define vkCmdSetStencilReference pes17_dispatch_vkCmdSetStencilReference
#define vkCmdSetStencilWriteMask pes17_dispatch_vkCmdSetStencilWriteMask
#define vkCmdSetViewport pes17_dispatch_vkCmdSetViewport
#define vkCmdUpdateBuffer pes17_dispatch_vkCmdUpdateBuffer
#define vkCmdWriteTimestamp pes17_dispatch_vkCmdWriteTimestamp
#define vkCreateBuffer pes17_dispatch_vkCreateBuffer
#define vkCreateCommandPool pes17_dispatch_vkCreateCommandPool
#define vkCreateComputePipelines pes17_dispatch_vkCreateComputePipelines
#define vkCreateDescriptorPool pes17_dispatch_vkCreateDescriptorPool
#define vkCreateDescriptorSetLayout pes17_dispatch_vkCreateDescriptorSetLayout
#define vkCreateFence pes17_dispatch_vkCreateFence
#define vkCreateFramebuffer pes17_dispatch_vkCreateFramebuffer
#define vkCreateGraphicsPipelines pes17_dispatch_vkCreateGraphicsPipelines
#define vkCreateImage pes17_dispatch_vkCreateImage
#define vkCreateImageView pes17_dispatch_vkCreateImageView
#define vkCreatePipelineCache pes17_dispatch_vkCreatePipelineCache
#define vkCreatePipelineLayout pes17_dispatch_vkCreatePipelineLayout
#define vkCreateQueryPool pes17_dispatch_vkCreateQueryPool
#define vkCreateRenderPass pes17_dispatch_vkCreateRenderPass
#define vkCreateSampler pes17_dispatch_vkCreateSampler
#define vkCreateSemaphore pes17_dispatch_vkCreateSemaphore
#define vkCreateShaderModule pes17_dispatch_vkCreateShaderModule
#define vkCreateSwapchainKHR pes17_dispatch_vkCreateSwapchainKHR
#define vkDestroyBuffer pes17_dispatch_vkDestroyBuffer
#define vkDestroyCommandPool pes17_dispatch_vkDestroyCommandPool
#define vkDestroyDescriptorPool pes17_dispatch_vkDestroyDescriptorPool
#define vkDestroyDescriptorSetLayout pes17_dispatch_vkDestroyDescriptorSetLayout
#define vkDestroyFence pes17_dispatch_vkDestroyFence
#define vkDestroyFramebuffer pes17_dispatch_vkDestroyFramebuffer
#define vkDestroyImage pes17_dispatch_vkDestroyImage
#define vkDestroyImageView pes17_dispatch_vkDestroyImageView
#define vkDestroyPipeline pes17_dispatch_vkDestroyPipeline
#define vkDestroyPipelineCache pes17_dispatch_vkDestroyPipelineCache
#define vkDestroyPipelineLayout pes17_dispatch_vkDestroyPipelineLayout
#define vkDestroyQueryPool pes17_dispatch_vkDestroyQueryPool
#define vkDestroyRenderPass pes17_dispatch_vkDestroyRenderPass
#define vkDestroySampler pes17_dispatch_vkDestroySampler
#define vkDestroySemaphore pes17_dispatch_vkDestroySemaphore
#define vkDestroyShaderModule pes17_dispatch_vkDestroyShaderModule
#define vkDestroySwapchainKHR pes17_dispatch_vkDestroySwapchainKHR
#define vkDeviceWaitIdle pes17_dispatch_vkDeviceWaitIdle
#define vkEndCommandBuffer pes17_dispatch_vkEndCommandBuffer
#define vkFreeDescriptorSets pes17_dispatch_vkFreeDescriptorSets
#define vkFreeMemory pes17_dispatch_vkFreeMemory
#define vkGetBufferMemoryRequirements pes17_dispatch_vkGetBufferMemoryRequirements
#define vkGetDeviceQueue pes17_dispatch_vkGetDeviceQueue
#define vkGetFenceStatus pes17_dispatch_vkGetFenceStatus
#define vkGetImageMemoryRequirements pes17_dispatch_vkGetImageMemoryRequirements
#define vkGetPipelineCacheData pes17_dispatch_vkGetPipelineCacheData
#define vkGetQueryPoolResults pes17_dispatch_vkGetQueryPoolResults
#define vkGetSwapchainImagesKHR pes17_dispatch_vkGetSwapchainImagesKHR
#define vkMapMemory pes17_dispatch_vkMapMemory
#define vkQueuePresentKHR pes17_dispatch_vkQueuePresentKHR
#define vkQueueSubmit pes17_dispatch_vkQueueSubmit
#define vkResetCommandBuffer pes17_dispatch_vkResetCommandBuffer
#define vkResetCommandPool pes17_dispatch_vkResetCommandPool
#define vkResetFences pes17_dispatch_vkResetFences
#define vkUnmapMemory pes17_dispatch_vkUnmapMemory
#define vkUpdateDescriptorSets pes17_dispatch_vkUpdateDescriptorSets
#define vkWaitForFences pes17_dispatch_vkWaitForFences

extern "C"
{
    // The one that is not loaded from anything else: it comes out of the
    // library, and everything else comes out of it.
    extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddrFn;

#define PES17_VK_GLOBAL(name)   extern PFN_##name name;
#define PES17_VK_INSTANCE(name) extern PFN_##name name;
#define PES17_VK_DEVICE(name)   extern PFN_##name name;
#include "vulkan_functions.inc"
#undef PES17_VK_GLOBAL
#undef PES17_VK_INSTANCE
#undef PES17_VK_DEVICE
}

#include <string>

namespace vk::loader
{
    // Opens the implementation -- the player's driver where there is one,
    // the system's otherwise -- and loads what can be loaded without an
    // instance. Safe to call more than once; only the first does anything.
    bool Open(std::string& error);
    bool Opened();

    // After vkCreateInstance and after vkCreateDevice. Entry points an
    // extension owns come back null when the extension is not there, which
    // is what the callers already check for.
    void LoadInstance(VkInstance instance);
    bool LoadDevice(VkDevice device);
    // The device is about to be destroyed: its entry points go back to the
    // instance's, which stay valid.
    void ForgetDevice();
    // The instance is gone: reset instance/device dispatch to global entry points.
    void ForgetInstance();

    // What was opened, for the log ("the system's libvulkan.so", a driver's
    // file name).
    const char* Description();
    // How many of the entry points the runtime wants this implementation has.
    // A driver missing one the renderer needs would otherwise crash on a null
    // pointer somewhere far from here.
    uint32_t Missing();
}

#endif  // PES17_VULKAN_DYNAMIC && PES17_HAVE_VULKAN
