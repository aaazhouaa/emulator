// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/frontend/emu_window.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_surface.h"

namespace Vulkan {

namespace {

bool CanBlitToSwapchain(const vk::PhysicalDevice& physical_device, VkFormat format) {
    const VkFormatProperties props{physical_device.GetFormatProperties(format)};
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
}

[[nodiscard]] VkImageSubresourceLayers MakeImageSubresourceLayers() {
    return VkImageSubresourceLayers{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] VkImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                        s32 swapchain_height) {
    return VkImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            {
                {
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                {
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            {
                {
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                {
                    .x = swapchain_width,
                    .y = swapchain_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] VkImageCopy MakeImageCopy(u32 frame_width, u32 frame_height, u32 swapchain_width,
                                        u32 swapchain_height) {
    return VkImageCopy{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffset =
            {
                .x = 0,
                .y = 0,
                .z = 0,
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffset =
            {
                .x = 0,
                .y = 0,
                .z = 0,
            },
        .extent =
            {
                .width = std::min(frame_width, swapchain_width),
                .height = std::min(frame_height, swapchain_height),
                .depth = 1,
            },
    };
}

} // Anonymous namespace

PresentManager::PresentManager(const vk::Instance& instance_,
                               Core::Frontend::EmuWindow& render_window_, const Device& device_,
                               MemoryAllocator& memory_allocator_, Scheduler& scheduler_,
                               Swapchain& swapchain_, vk::SurfaceKHR& surface_)
    : instance{instance_}, render_window{render_window_}, device{device_},
      memory_allocator{memory_allocator_}, scheduler{scheduler_}, swapchain{swapchain_},
      surface{surface_}, blit_supported{CanBlitToSwapchain(device.GetPhysical(),
                                                           swapchain.GetImageViewFormat())},
      use_present_thread{Settings::values.async_presentation.GetValue()} {
    presented_surface = render_window.GetWindowInfo().render_surface;
    SetImageCount();
    UpdateFramePipelineLimit();

    auto& dld = device.GetLogical();
    cmdpool = dld.CreateCommandPool({
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags =
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.GetGraphicsFamily(),
    });
    auto cmdbuffers = cmdpool.Allocate(image_count);

    frames.resize(image_count);
    for (u32 i = 0; i < frames.size(); i++) {
        Frame& frame = frames[i];
        frame.cmdbuf = vk::CommandBuffer{cmdbuffers[i], device.GetDispatchLoader()};
        frame.render_ready = dld.CreateSemaphore({
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        });
        frame.present_done = dld.CreateFence({
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        });
        free_queue.push(&frame);
    }

    if (use_present_thread) {
        present_thread = std::jthread([this](std::stop_token token) { PresentThread(token); });
    }
}

PresentManager::~PresentManager() = default;

Frame* PresentManager::GetRenderFrame() {
    // Bound the whole Mailbox async pipeline before another frame can enter present_queue. Waiting
    // here preserves the fences and semaphores of frames that are already rendered or in flight.
    std::unique_lock lock{free_mutex};
    free_cv.wait(lock, [this] {
        return !free_queue.empty() &&
               (!limit_mailbox_frames || frames_in_flight < MAX_MAILBOX_FRAMES_IN_FLIGHT);
    });

    // Take the frame from the queue
    Frame* frame = free_queue.front();
    free_queue.pop();
    ++frames_in_flight;

    // Wait for the presentation to be finished so all frame resources are free
    frame->submitted = false;
    frame->present_done.Wait();
    frame->present_done.Reset();

    return frame;
}

void PresentManager::Present(Frame* frame) {
    if (!use_present_thread) {
        scheduler.WaitWorker();
        PresentFrame(frame);
        ReleaseRenderFrame(frame);
        return;
    }

    scheduler.Record([this, frame](vk::CommandBuffer) {
        std::unique_lock lock{queue_mutex};
        present_queue.push(frame);
        frame_cv.notify_one();
    });
}

void PresentManager::RecreateFrame(Frame* frame, u32 width, u32 height, VkFormat image_view_format,
                                   VkRenderPass rd) {
    auto& dld = device.GetLogical();

    frame->width = width;
    frame->height = height;

    // The frame is only ever viewed as image_view_format. Mutable without a format list lets the
    // driver assume any format; Turnip then stores the image linear (tu_image.cc).
    const VkFormat image_format = swapchain.GetImageFormat();
    const std::array view_formats{image_format, image_view_format};
    const VkImageFormatListCreateInfo format_list{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .pNext = nullptr,
        .viewFormatCount = static_cast<u32>(view_formats.size()),
        .pViewFormats = view_formats.data(),
    };
    const bool is_mutable = image_view_format != image_format;
    frame->image = memory_allocator.CreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = is_mutable && device.IsKhrImageFormatListSupported() ? &format_list : nullptr,
        .flags = is_mutable ? VkImageCreateFlags{VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT} : 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = image_format,
        .extent =
            {
                .width = width,
                .height = height,
                .depth = 1,
            },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });

    frame->image_view = dld.CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *frame->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_view_format,
        .components =
            {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    });

    const VkImageView image_view{*frame->image_view};
    frame->framebuffer = dld.CreateFramebuffer({
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderPass = rd,
        .attachmentCount = 1,
        .pAttachments = &image_view,
        .width = width,
        .height = height,
        .layers = 1,
    });
}

void PresentManager::WaitPresent() {
    if (!use_present_thread) {
        return;
    }

    // Wait for the present queue to be empty
    {
        std::unique_lock queue_lock{queue_mutex};
        frame_cv.wait(queue_lock, [this] { return present_queue.empty(); });
    }

    // The above condition will be satisfied when the last frame is taken from the queue.
    // To ensure that frame has been presented as well take hold of the swapchain
    // mutex.
    std::scoped_lock swapchain_lock{swapchain_mutex};
}

void PresentManager::PresentThread(std::stop_token token) {
    Common::SetCurrentThreadName("VulkanPresent");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    while (!token.stop_requested()) {
        std::unique_lock lock{queue_mutex};

        // Wait for presentation frames
        Common::CondvarWait(frame_cv, lock, token, [this] { return !present_queue.empty(); });
        if (token.stop_requested()) {
            return;
        }

        // Take the frame and notify anyone waiting
        Frame* frame = present_queue.front();
        present_queue.pop();
        frame_cv.notify_one();

        // By exchanging the lock ownership we take the swapchain lock
        // before the queue lock goes out of scope. This way the swapchain
        // lock in WaitPresent is guaranteed to occur after here.
        std::exchange(lock, std::unique_lock{swapchain_mutex});

        PresentFrame(frame);

        ReleaseRenderFrame(frame);
    }
}

void PresentManager::RecreateSwapchain(Frame* frame) {
    swapchain.Create(*surface, frame->width, frame->height);
    UpdateFramePipelineLimit();
}

void PresentManager::SetImageCount() {
    // We cannot have more than 7 images in flight at any given time.
    // FRAMES_IN_FLIGHT is 8, and the cache TICKS_TO_DESTROY is 8.
    // Mali drivers will give us 6.
    image_count = std::min<size_t>(swapchain.GetImageCount(), 7);
}

void PresentManager::UpdateFramePipelineLimit() {
    {
        std::scoped_lock lock{free_mutex};
#ifdef ANDROID
        limit_mailbox_frames = use_present_thread && swapchain.IsMailbox();
#else
        limit_mailbox_frames = false;
#endif
    }
    free_cv.notify_all();
}

void PresentManager::ReleaseRenderFrame(Frame* frame) {
    {
        std::scoped_lock lock{free_mutex};
        free_queue.push(frame);
        --frames_in_flight;
    }
    free_cv.notify_one();
}

void PresentManager::PresentFrame(Frame* frame) noexcept {
    try {
        CopyToSwapchain(frame);
    } catch (const std::exception& exception) {
        // Presenting runs on a std::jthread (async presentation) or on the GPU thread. An
        // exception escaping either of them reaches std::terminate and kills the process, which
        // is what a destroyed Android surface used to do.
        LOG_ERROR(Render_Vulkan, "Failed to present frame: {}", exception.what());
        DiscardFrame(frame);
    }
}

void PresentManager::DiscardFrame(Frame* frame) noexcept {
    if (frame->submitted) {
        // The frame was already on the queue waiting for render_ready, so that signal has been
        // consumed. Waiting on it again would never complete.
        LOG_DEBUG(Render_Vulkan, "Dropped frame {} after its submission", frame - frames.data());
        return;
    }

    // GetRenderFrame() waits on present_done and the next present waits on render_ready, which the
    // renderer already signaled. Retire both with an empty submission, otherwise the frame is never
    // reusable.
    const VkSemaphore render_semaphore{*frame->render_ready};
    static constexpr VkPipelineStageFlags wait_stage{VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 1U,
        .pWaitSemaphores = &render_semaphore,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 0,
        .pCommandBuffers = nullptr,
        .signalSemaphoreCount = 0U,
        .pSignalSemaphores = nullptr,
    };

    std::scoped_lock submit_lock{scheduler.submit_mutex};
    const VkResult result = device.GetGraphicsQueue().Submit(submit_info, *frame->present_done);
    if (result == VK_ERROR_DEVICE_LOST) {
        device.ReportLoss();
    }
    if (result != VK_SUCCESS) {
        // Without a signaled fence this frame can never be reused, but the device is failing
        // anyway.
        LOG_ERROR(Render_Vulkan, "Failed to retire a dropped frame: {}", string_VkResult(result));
    }
}

void PresentManager::CopyToSwapchain(Frame* frame) {
    // A surface that is gone for good keeps failing, so retrying has to be bounded. Without a
    // bound this loop spins on the present thread and the frame is never released.
    for (u32 attempt = 0; attempt < MAX_SURFACE_RECREATION_ATTEMPTS; ++attempt) {
        // Holds a reference on the native window for as long as the driver needs it, so the
        // frontend cannot destroy the surface midway through creating or presenting to it.
        const auto window_info = render_window.AcquireWindowInfo();
        SCOPE_EXIT {
            render_window.ReleaseRenderSurface(window_info.render_surface);
        };
        void* const window_surface = window_info.render_surface;

        if (window_surface == nullptr) {
            // The frontend destroyed its surface. Rebuilding it needs a live window handle,
            // without one the driver fails with VK_ERROR_INITIALIZATION_FAILED. Forget which
            // surface is in use too, so the next frame rebuilds the surface and the swapchain
            // instead of presenting to the destroyed one.
            LOG_DEBUG(Render_Vulkan, "Dropping frame, no surface to present to");
            presented_surface = nullptr;
            DiscardFrame(frame);
            return;
        }

        // The swapchain is bound to the native window it was created from, so a different window
        // (or a replacement for a destroyed one) needs a new surface and swapchain.
        const bool requires_recreation =
            surface_needs_recreation || window_surface != presented_surface;

        try {
            // Recreate surface and swapchain if needed.
            if (requires_recreation) {
                // The old surface has to outlive its swapchain: it is destroyed when this block
                // ends, which is after RecreateSwapchain() replaced the swapchain. Creating the new
                // surface first also means a failure leaves the old one intact.
                vk::SurfaceKHR new_surface = CreateSurface(instance, window_info);
                const vk::SurfaceKHR old_surface = std::move(surface);
                surface = std::move(new_surface);
                RecreateSwapchain(frame);
                presented_surface = window_surface;
                surface_needs_recreation = false;
            }

            // Draw to swapchain.
            return CopyToSwapchainImpl(frame);
        } catch (const vk::Exception& except) {
            if (except.GetResult() != VK_ERROR_SURFACE_LOST_KHR) {
                throw;
            }

            surface_needs_recreation = true;

            if (frame->submitted) {
                // The image copy already ran and consumed render_ready. Submitting again would wait
                // on a semaphore nobody signals any more, which hangs the queue; the fence of the
                // submit that did run will release this frame instead.
                LOG_DEBUG(Render_Vulkan, "Surface lost after submit, dropping this frame");
                return;
            }
        }
    }

    LOG_ERROR(Render_Vulkan, "Giving up on this frame, the surface could not be recreated");
    presented_surface = nullptr;
    DiscardFrame(frame);
}

void PresentManager::CopyToSwapchainImpl(Frame* frame) {

    // If the size of the incoming frames has changed, recreate the swapchain
    // to account for that.
    const bool is_suboptimal = swapchain.NeedsRecreation();
    const bool size_changed =
        swapchain.GetWidth() != frame->width || swapchain.GetHeight() != frame->height;
    if (is_suboptimal || size_changed) {
        RecreateSwapchain(frame);
    }

    while (swapchain.AcquireNextImage()) {
        RecreateSwapchain(frame);
    }

    const vk::CommandBuffer cmdbuf{frame->cmdbuf};
    cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });

    const VkImage image{swapchain.CurrentImage()};
    const VkExtent2D extent = swapchain.GetExtent();
    const std::array pre_barriers{
        VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        },
        VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = *frame->image,
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        },
    };
    const std::array post_barriers{
        VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        },
        VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = *frame->image,
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        },
    };

    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, {},
                           {}, {}, pre_barriers);

    if (blit_supported) {
        cmdbuf.BlitImage(*frame->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         MakeImageBlit(frame->width, frame->height, extent.width, extent.height),
                         VK_FILTER_LINEAR);
    } else {
        cmdbuf.CopyImage(*frame->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         MakeImageCopy(frame->width, frame->height, extent.width, extent.height));
    }

    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, {},
                           {}, {}, post_barriers);

    cmdbuf.End();

    const VkSemaphore present_semaphore = swapchain.CurrentPresentSemaphore();
    const VkSemaphore render_semaphore = swapchain.CurrentRenderSemaphore();
    const std::array wait_semaphores = {present_semaphore, *frame->render_ready};

    static constexpr std::array<VkPipelineStageFlags, 2> wait_stage_masks{
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
    };

    const VkSubmitInfo submit_info{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 2U,
        .pWaitSemaphores = wait_semaphores.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1,
        .pCommandBuffers = cmdbuf.address(),
        .signalSemaphoreCount = 1U,
        .pSignalSemaphores = &render_semaphore,
    };

    // Submit the image copy/blit to the swapchain
    {
        std::scoped_lock submit_lock{scheduler.submit_mutex};
        switch (const VkResult result =
                    device.GetGraphicsQueue().Submit(submit_info, *frame->present_done)) {
        case VK_SUCCESS:
            frame->submitted = true;
            break;
        case VK_ERROR_DEVICE_LOST:
            device.ReportLoss();
            [[fallthrough]];
        default:
            vk::Check(result);
            break;
        }
    }

    // Present
    swapchain.Present(render_semaphore);
}

} // namespace Vulkan
