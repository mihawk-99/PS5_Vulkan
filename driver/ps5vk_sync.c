/*
 * PS5 Vulkan driver - sync objects: fences, semaphores and events.
 * Copyright (C) 2026 Mihawk-99
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Milestone 5 Phase B5 (docs/M5_PHASE_B.md). A binary sync object is a flag
 * under a mutex and condition variable, set and cleared on the CPU; a
 * submission waits for its input syncs on the CPU before its streams run.
 * After lavapipe's lvp_pipe_sync (Mesa, MIT), without its gallium fences.
 *
 * R69: a submission's last step is not waited for (ps5vk_queue.c), so the
 * syncs it signals are set with the marker value that step writes: a wait on
 * one waits for the flag and then for the GPU to reach that value. A wait by a
 * later submission to the same queue skips the GPU part, since the queue's
 * submissions run in order.
 */

#include "ps5vk_private.h"

#include <assert.h>
#include <time.h>

#include "util/os_time.h"
#include "util/timespec.h"

static struct ps5vk_sync *
ps5vk_sync_of(struct vk_sync *sync)
{
   assert(sync->type == &ps5vk_sync_type);
   return container_of(sync, struct ps5vk_sync, base);
}

static VkResult
ps5vk_sync_init(struct vk_device *device, struct vk_sync *vk_sync, uint64_t initial_value)
{
   struct ps5vk_sync *const sync = ps5vk_sync_of(vk_sync);
   if (mtx_init(&sync->lock, mtx_plain) != thrd_success)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   if (cnd_init(&sync->changed) != thrd_success) {
      mtx_destroy(&sync->lock);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   sync->signaled = initial_value != 0;
   sync->queue = NULL;
   sync->value = 0;
   return VK_SUCCESS;
}

static void
ps5vk_sync_finish(struct vk_device *device, struct vk_sync *vk_sync)
{
   (void)device;
   struct ps5vk_sync *const sync = ps5vk_sync_of(vk_sync);
   cnd_destroy(&sync->changed);
   mtx_destroy(&sync->lock);
}

static void
ps5vk_sync_set(struct ps5vk_sync *sync, bool signaled, struct ps5vk_queue *queue, uint32_t value)
{
   mtx_lock(&sync->lock);
   sync->signaled = signaled;
   sync->queue = signaled ? queue : NULL;
   sync->value = value;
   cnd_broadcast(&sync->changed);
   mtx_unlock(&sync->lock);
}

void
ps5vk_sync_signal_pending(struct vk_sync *vk_sync, struct ps5vk_queue *queue, uint32_t value)
{
   ps5vk_sync_set(ps5vk_sync_of(vk_sync), true, queue, value);
}

bool
ps5vk_sync_pending_on(struct vk_sync *vk_sync, const struct ps5vk_queue *queue)
{
   if (vk_sync->type != &ps5vk_sync_type)
      return false;
   struct ps5vk_sync *const sync = ps5vk_sync_of(vk_sync);
   mtx_lock(&sync->lock);
   const bool pending = sync->signaled && sync->queue == queue;
   mtx_unlock(&sync->lock);
   return pending;
}

static VkResult
ps5vk_sync_signal(struct vk_device *device, struct vk_sync *vk_sync, uint64_t value)
{
   (void)device;
   assert(value == 0);
   ps5vk_sync_set(ps5vk_sync_of(vk_sync), true, NULL, 0);
   return VK_SUCCESS;
}

static VkResult
ps5vk_sync_reset(struct vk_device *device, struct vk_sync *vk_sync)
{
   (void)device;
   ps5vk_sync_set(ps5vk_sync_of(vk_sync), false, NULL, 0);
   return VK_SUCCESS;
}

static VkResult
ps5vk_sync_move(struct vk_device *device, struct vk_sync *vk_dst, struct vk_sync *vk_src)
{
   (void)device;
   struct ps5vk_sync *const src = ps5vk_sync_of(vk_src);
   mtx_lock(&src->lock);
   const bool signaled = src->signaled;
   struct ps5vk_queue *const queue = src->queue;
   const uint32_t value = src->value;
   src->signaled = false;
   src->queue = NULL;
   cnd_broadcast(&src->changed);
   mtx_unlock(&src->lock);
   ps5vk_sync_set(ps5vk_sync_of(vk_dst), signaled, queue, value);
   return VK_SUCCESS;
}

static VkResult
ps5vk_sync_wait(struct vk_device *device, struct vk_sync *vk_sync, uint64_t wait_value,
                enum vk_sync_wait_flags wait_flags, uint64_t abs_timeout_ns)
{
   (void)wait_value;
   (void)wait_flags;
   struct ps5vk_sync *const sync = ps5vk_sync_of(vk_sync);
   VkResult result = VK_SUCCESS;

   mtx_lock(&sync->lock);
   while (!sync->signaled) {
      const uint64_t now_ns = os_time_get_nano();
      if (now_ns >= abs_timeout_ns) {
         result = VK_TIMEOUT;
         break;
      }
      int ret;
      struct timespec now_ts;
      struct timespec abs_ts;
      /* C11 condition variables time out on CLOCK_REALTIME, Vulkan timeouts
       * are CLOCK_MONOTONIC: convert the remaining time, then re-check the
       * monotonic clock instead of trusting the wait's own timeout. */
      if (abs_timeout_ns >= (uint64_t)INT64_MAX || timespec_get(&now_ts, TIME_UTC) == 0 ||
          timespec_add_nsec(&abs_ts, &now_ts, abs_timeout_ns - now_ns))
         ret = cnd_wait(&sync->changed, &sync->lock);
      else
         ret = cnd_timedwait(&sync->changed, &sync->lock, &abs_ts);
      if (ret == thrd_error) {
         result = vk_errorf(device, VK_ERROR_UNKNOWN, "waiting on a sync object failed");
         break;
      }
   }
   struct ps5vk_queue *const queue = result == VK_SUCCESS ? sync->queue : NULL;
   const uint32_t value = sync->value;
   mtx_unlock(&sync->lock);
   if (queue == NULL)
      return result;
   /* Signalled by a submission: done once the GPU has run it. */
   result = ps5vk_queue_wait_value(queue, value, abs_timeout_ns);
   if (result == VK_SUCCESS) {
      mtx_lock(&sync->lock);
      if (sync->queue == queue && sync->value == value)
         sync->queue = NULL;
      mtx_unlock(&sync->lock);
   }
   return result;
}

const struct vk_sync_type ps5vk_sync_type = {
   .size = sizeof(struct ps5vk_sync),
   .features = VK_SYNC_FEATURE_BINARY | VK_SYNC_FEATURE_GPU_WAIT |
               VK_SYNC_FEATURE_GPU_MULTI_WAIT | VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_RESET | VK_SYNC_FEATURE_CPU_SIGNAL,
   .init = ps5vk_sync_init,
   .finish = ps5vk_sync_finish,
   .signal = ps5vk_sync_signal,
   .reset = ps5vk_sync_reset,
   .move = ps5vk_sync_move,
   .wait = ps5vk_sync_wait,
};

/* ------------- events: the same flag with Vulkan's event rules -------------
 *
 * An event is unsignaled when it is created, set and reset by the host
 * (vkSetEvent, vkResetEvent) or by a submission (vkCmdSetEvent,
 * vkCmdResetEvent), read by vkGetEventStatus and waited for by
 * vkCmdWaitEvents. Recording one of the three commands records a split with an
 * action on it -- the record the copies use, with nothing to copy
 * (driver/ps5vk_private.h) -- which the queue reaches at its place in the
 * command order: the words before it have run and completed before the flag is
 * touched, which is Vulkan's own wording for when a set or a reset takes
 * effect, and the words after a wait are submitted only once its flag is set.
 * A wait therefore sees a set from earlier in the same submission, from an
 * earlier submission or from the host, which is what an event is for
 * (ps5vk_queue.c runs the actions).
 *
 * The memory barriers vkCmdWaitEvents carries are the split itself: the queue
 * flushes the render targets and the CPU caches at every step boundary, which
 * is the barrier a render-to-texture split already relies on (Phase C4). A
 * wait for an event nothing ever sets loses the device after two seconds with
 * its own message, the shape the completion marker poll has, rather than
 * hanging.
 *
 * Console runs still owed (docs/M5_PHASE_C.md): the battery records a set, a
 * reset and a wait and reads the flag back, which is the same evidence the
 * CPU-side transfers get, so nothing here needed a probe first.
 */

static VkResult
ps5vk_event_set(struct ps5vk_event *event, bool signaled)
{
   mtx_lock(&event->lock);
   event->signaled = signaled;
   mtx_unlock(&event->lock);
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_CreateEvent(VkDevice _device, const VkEventCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator, VkEvent *pEvent)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   /* VK_EVENT_CREATE_DEVICE_ONLY_BIT is 1.3, so what reaches here is an
    * ordinary host-synchronized event: nothing in the flags changes the flag
    * below. */
   (void)pCreateInfo;
   struct ps5vk_event *const event =
      vk_object_zalloc(&device->vk, pAllocator, sizeof(*event), VK_OBJECT_TYPE_EVENT);
   if (event == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   if (mtx_init(&event->lock, mtx_plain) != thrd_success) {
      vk_object_free(&device->vk, pAllocator, event);
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   /* A new event is unsignaled. */
   event->signaled = false;
   *pEvent = ps5vk_event_to_handle(event);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_DestroyEvent(VkDevice _device, VkEvent _event, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(ps5vk_device, device, _device);
   VK_FROM_HANDLE(ps5vk_event, event, _event);
   if (event == NULL)
      return;
   mtx_destroy(&event->lock);
   vk_object_free(&device->vk, pAllocator, event);
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_GetEventStatus(VkDevice _device, VkEvent _event)
{
   VK_FROM_HANDLE(ps5vk_event, event, _event);
   (void)_device;
   mtx_lock(&event->lock);
   const bool signaled = event->signaled;
   mtx_unlock(&event->lock);
   return signaled ? VK_EVENT_SET : VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_SetEvent(VkDevice _device, VkEvent _event)
{
   VK_FROM_HANDLE(ps5vk_event, event, _event);
   (void)_device;
   return ps5vk_event_set(event, true);
}

VKAPI_ATTR VkResult VKAPI_CALL
ps5vk_ResetEvent(VkDevice _device, VkEvent _event)
{
   VK_FROM_HANDLE(ps5vk_event, event, _event);
   (void)_device;
   return ps5vk_event_set(event, false);
}

/* Records one event action where the command buffer's words have reached: a
 * split with the action on it and nothing to copy, which is the record the
 * queue runs when it reaches that place (driver/ps5vk_queue.c). */
static void
ps5vk_cmd_buffer_record_event(struct ps5vk_cmd_buffer *cmd_buffer, struct ps5vk_event *event,
                              enum ps5vk_event_action action)
{
   struct ps5vk_memory_copy *const record =
      util_dynarray_grow(&cmd_buffer->copies, struct ps5vk_memory_copy, 1);
   if (record == NULL) {
      ps5vk_cmd_buffer_refuse(cmd_buffer, VK_ERROR_OUT_OF_HOST_MEMORY,
                              "no memory to record an event command");
      return;
   }
   *record = (struct ps5vk_memory_copy){
      .after_words = (uint32_t)util_dynarray_num_elements(&cmd_buffer->words, uint32_t),
      .event = event,
      .event_action = (uint8_t)action,
   };
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdSetEvent(VkCommandBuffer commandBuffer, VkEvent _event, VkPipelineStageFlags stageMask)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_event, event, _event);
   /* The stage mask names which stages have to have completed first; a split
    * is a whole step boundary, so every stage has. */
   (void)stageMask;
   if (vk_command_buffer_has_error(&cmd_buffer->vk) || event == NULL)
      return;
   ps5vk_cmd_buffer_record_event(cmd_buffer, event, PS5VK_EVENT_ACTION_SET);
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdResetEvent(VkCommandBuffer commandBuffer, VkEvent _event, VkPipelineStageFlags stageMask)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   VK_FROM_HANDLE(ps5vk_event, event, _event);
   (void)stageMask;
   if (vk_command_buffer_has_error(&cmd_buffer->vk) || event == NULL)
      return;
   ps5vk_cmd_buffer_record_event(cmd_buffer, event, PS5VK_EVENT_ACTION_RESET);
}

VKAPI_ATTR void VKAPI_CALL
ps5vk_CmdWaitEvents(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent *pEvents,
                    VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
                    uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
                    uint32_t bufferMemoryBarrierCount,
                    const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                    uint32_t imageMemoryBarrierCount,
                    const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   VK_FROM_HANDLE(ps5vk_cmd_buffer, cmd_buffer, commandBuffer);
   /* The barriers are the split's own: the queue flushes the targets and the
    * CPU caches at the step boundary the wait falls on. */
   (void)srcStageMask;
   (void)dstStageMask;
   (void)memoryBarrierCount;
   (void)pMemoryBarriers;
   (void)bufferMemoryBarrierCount;
   (void)pBufferMemoryBarriers;
   (void)imageMemoryBarrierCount;
   (void)pImageMemoryBarriers;
   if (vk_command_buffer_has_error(&cmd_buffer->vk))
      return;
   for (uint32_t i = 0; i < eventCount; i++) {
      VK_FROM_HANDLE(ps5vk_event, event, pEvents[i]);
      if (event == NULL)
         continue;
      ps5vk_cmd_buffer_record_event(cmd_buffer, event, PS5VK_EVENT_ACTION_WAIT);
   }
}
