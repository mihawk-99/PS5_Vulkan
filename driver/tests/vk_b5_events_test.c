/*
 * PS5 Vulkan driver - Phase B5 test: events.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B5 (docs/M5_REFERENCE.md); built and run through the
 * loader and directly by tools/check-driver.sh (see ps5vk_test.h).
 *
 * An event is a flag this process owns, and the driver reaches it from a
 * submission at a split point (driver/ps5vk_sync.c records the action,
 * driver/ps5vk_queue.c runs it), which is the mechanism the CPU-side transfers
 * already use. What this test proves is the part that mechanism is for: that
 * the flag follows the command order.
 *
 *   - vkCreateEvent makes an unsignaled event; vkGetEventStatus reads it;
 *     vkSetEvent and vkResetEvent set and clear it from the host.
 *   - vkCmdSetEvent and vkCmdResetEvent set and clear it as part of a
 *     submission, and two actions in one command buffer happen in the order
 *     they were recorded, so the last one is what the flag holds.
 *   - vkCmdWaitEvents is passed by a set earlier in the same command buffer and
 *     by a set in an earlier command buffer of the same submission.
 *   - a wait with nothing to pass it blocks: the last case sets the event from
 *     another thread while the submission is inside vkQueueSubmit and measures
 *     how long that took, which is the one case that tells a wait that ran from
 *     a wait that was skipped.
 *
 * Every check is a read of the flag or of the clock, all of it on the PC: no
 * frame, no golden, and the PS5 build only links. The console run that records
 * the same three commands is still owed (docs/M5_PHASE_C.md).
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <time.h>

#include "ps5vk_test.h"

/* How long the helper thread waits before it sets the event, in milliseconds,
 * and the lower bound the blocked submission has to have taken: half of it,
 * which no scheduling accident reaches and no skipped wait passes. */
#define HOST_SET_DELAY_MS 150
#define BLOCKED_MINIMUM_NS (INT64_C(HOST_SET_DELAY_MS / 2) * INT64_C(1000000))

static VkInstance g_instance;
static VkDevice g_device;
static VkQueue g_queue;
static VkEvent g_event;

/* Submits the recorded command buffers and waits for them, so everything a
 * submission records has run by the time it returns. */
static VkResult
submit(VkFence fence, const VkCommandBuffer *buffers, uint32_t count)
{
   const VkSubmitInfo info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = count,
      .pCommandBuffers = buffers,
   };
   VkResult result = VK_FUNCTION(g_instance, QueueSubmit)(g_queue, 1, &info, fence);
   if (result != VK_SUCCESS)
      return result;
   result = VK_FUNCTION(g_instance, WaitForFences)(g_device, 1, &fence, VK_TRUE, UINT64_MAX);
   if (result != VK_SUCCESS)
      return result;
   return VK_FUNCTION(g_instance, ResetFences)(g_device, 1, &fence);
}

static void
begin_record(VkCommandBuffer command)
{
   const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   VK_FUNCTION(g_instance, BeginCommandBuffer)(command, &begin);
}

static void
end_record(VkCommandBuffer command)
{
   VK_FUNCTION(g_instance, EndCommandBuffer)(command);
}

/* Monotonic nanoseconds, for the one check that measures how long a submission
 * took; the test needs no other clock. */
static int64_t
now_ns(void)
{
   struct timespec at;
   clock_gettime(CLOCK_MONOTONIC, &at);
   return (int64_t)at.tv_sec * INT64_C(1000000000) + (int64_t)at.tv_nsec;
}

/* Sets the event from another thread after a delay: the submission waiting on
 * it can only finish once this has run. */
static void *
set_event_after_delay(void *unused)
{
   (void)unused;
   const struct timespec delay = {
      .tv_sec = HOST_SET_DELAY_MS / 1000,
      .tv_nsec = (long)(HOST_SET_DELAY_MS % 1000) * 1000000L,
   };
   nanosleep(&delay, NULL);
   VK_FUNCTION(g_instance, SetEvent)(g_device, g_event);
   return NULL;
}

int
main(void)
{
   test_begin("B5 events");
   VkPhysicalDevice physical = VK_NULL_HANDLE;
   if (!test_create_device(&g_instance, &physical, &g_device)) {
      test_destroy_device(g_instance, g_device);
      return test_finish();
   }
   VK_FUNCTION(g_instance, GetDeviceQueue)(g_device, 0, 0, &g_queue);

   const VkEventCreateInfo event_info = {.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
   check(VK_FUNCTION(g_instance, CreateEvent)(g_device, &event_info, NULL, &g_event) == VK_SUCCESS &&
            g_event != VK_NULL_HANDLE,
         "an event");
   if (g_event == VK_NULL_HANDLE)
      goto finish;
   check(VK_FUNCTION(g_instance, GetEventStatus)(g_device, g_event) == VK_EVENT_RESET,
         "a new event is unsignaled");
   check(VK_FUNCTION(g_instance, SetEvent)(g_device, g_event) == VK_SUCCESS &&
            VK_FUNCTION(g_instance, GetEventStatus)(g_device, g_event) == VK_EVENT_SET,
         "vkSetEvent sets it from the host");
   check(VK_FUNCTION(g_instance, ResetEvent)(g_device, g_event) == VK_SUCCESS &&
            VK_FUNCTION(g_instance, GetEventStatus)(g_device, g_event) == VK_EVENT_RESET,
         "vkResetEvent clears it from the host");

   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = 0,
   };
   VkCommandPool pool = VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, CreateCommandPool)(g_device, &pool_info, NULL, &pool) != VK_SUCCESS) {
      check(false, "a command pool");
      goto finish;
   }
   const VkCommandBufferAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 2,
   };
   VkCommandBuffer commands[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
   const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence = VK_NULL_HANDLE;
   if (VK_FUNCTION(g_instance, AllocateCommandBuffers)(g_device, &allocate_info, commands) !=
           VK_SUCCESS ||
       VK_FUNCTION(g_instance, CreateFence)(g_device, &fence_info, NULL, &fence) != VK_SUCCESS) {
      check(false, "two command buffers and a fence");
      goto destroy_pool;
   }

   /* A set and a reset recorded in a submission: the flag is the command's. */
   begin_record(commands[0]);
   VK_FUNCTION(g_instance, CmdSetEvent)(commands[0], g_event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
   end_record(commands[0]);
   VkResult result = submit(fence, &commands[0], 1);
   check(result == VK_SUCCESS &&
            VK_FUNCTION(g_instance, GetEventStatus)(g_device, g_event) == VK_EVENT_SET,
         "vkCmdSetEvent sets the event where it is recorded in the submission");

   begin_record(commands[0]);
   VK_FUNCTION(g_instance, CmdResetEvent)(commands[0], g_event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
   end_record(commands[0]);
   result = submit(fence, &commands[0], 1);
   check(result == VK_SUCCESS &&
            VK_FUNCTION(g_instance, GetEventStatus)(g_device, g_event) == VK_EVENT_RESET,
         "vkCmdResetEvent clears it");

   /* Both in one command buffer: the split order is the record order. */
   begin_record(commands[0]);
   VK_FUNCTION(g_instance, CmdSetEvent)(commands[0], g_event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
   VK_FUNCTION(g_instance, CmdResetEvent)(commands[0], g_event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
   end_record(commands[0]);
   result = submit(fence, &commands[0], 1);
   check(result == VK_SUCCESS &&
            VK_FUNCTION(g_instance, GetEventStatus)(g_device, g_event) == VK_EVENT_RESET,
         "two event commands in one command buffer run in the order they were recorded");

   /* A wait passed by a set earlier in the same command buffer. */
   begin_record(commands[0]);
   VK_FUNCTION(g_instance, CmdSetEvent)(commands[0], g_event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
   VK_FUNCTION(g_instance, CmdWaitEvents)(commands[0], 1, &g_event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL, 0,
                                          NULL);
   end_record(commands[0]);
   result = submit(fence, &commands[0], 1);
   check(result == VK_SUCCESS &&
            VK_FUNCTION(g_instance, GetEventStatus)(g_device, g_event) == VK_EVENT_SET,
         "a wait passes when the same command buffer set the event before it");

   /* A wait passed by a set in an earlier command buffer of the submission. */
   VK_FUNCTION(g_instance, ResetEvent)(g_device, g_event);
   begin_record(commands[0]);
   VK_FUNCTION(g_instance, CmdSetEvent)(commands[0], g_event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
   end_record(commands[0]);
   begin_record(commands[1]);
   VK_FUNCTION(g_instance, CmdWaitEvents)(commands[1], 1, &g_event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL, 0,
                                          NULL);
   end_record(commands[1]);
   result = submit(fence, commands, 2);
   check(result == VK_SUCCESS &&
            VK_FUNCTION(g_instance, GetEventStatus)(g_device, g_event) == VK_EVENT_SET,
         "a wait passes when an earlier command buffer of the submission set the event");

   /* A wait with nothing to pass it: another thread sets the event while the
    * submission is inside vkQueueSubmit, and the time the submission took says
    * the wait really waited. */
   VK_FUNCTION(g_instance, ResetEvent)(g_device, g_event);
   begin_record(commands[0]);
   VK_FUNCTION(g_instance, CmdWaitEvents)(commands[0], 1, &g_event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, NULL, 0, NULL, 0,
                                          NULL);
   end_record(commands[0]);
   pthread_t setter;
   const bool started = pthread_create(&setter, NULL, set_event_after_delay, NULL) == 0;
   const int64_t before = now_ns();
   result = submit(fence, &commands[0], 1);
   const int64_t elapsed = now_ns() - before;
   if (started)
      pthread_join(setter, NULL);
   check(started && result == VK_SUCCESS &&
            VK_FUNCTION(g_instance, GetEventStatus)(g_device, g_event) == VK_EVENT_SET,
         "a wait is passed by vkSetEvent from another thread");
   check(elapsed >= BLOCKED_MINIMUM_NS,
         "the wait blocked until the host set the event (the submission took at least 75 ms)");

   VK_FUNCTION(g_instance, DestroyFence)(g_device, fence, NULL);
destroy_pool:
   VK_FUNCTION(g_instance, DestroyCommandPool)(g_device, pool, NULL);
finish:
   if (g_event != VK_NULL_HANDLE)
      VK_FUNCTION(g_instance, DestroyEvent)(g_device, g_event, NULL);
   test_destroy_device(g_instance, g_device);
   return test_finish();
}
