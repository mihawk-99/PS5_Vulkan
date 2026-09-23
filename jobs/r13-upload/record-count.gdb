# R13 host witness: metadata is bounded by regions, not image rows.
# Run after tools/check-driver.sh c4_texture; see this directory's README.
set pagination off
set confirm off
break ps5vk_CmdCopyMemoryToImageKHR
run
set $upload_cb = (struct ps5vk_cmd_buffer *)commandBuffer
set $before = $upload_cb->copies.size
set $upload_info = pCopyMemoryInfo
set $regions = pCopyMemoryInfo->regionCount
finish
set $records = ($upload_cb->copies.size - $before) / sizeof(struct ps5vk_memory_copy)
printf "upload metadata: %u records for %u regions, %u bytes per record\n", $records, $regions, sizeof(struct ps5vk_memory_copy)
if $records != $regions
  quit 1
end
disable 1
set $uploads = 1
while $uploads < 64
  call (void) ps5vk_CmdCopyMemoryToImageKHR((VkCommandBuffer)$upload_cb, $upload_info)
  set $uploads = $uploads + 1
end
set $records = ($upload_cb->copies.size - $before) / sizeof(struct ps5vk_memory_copy)
printf "repeated uploads: %u records for %u regions; metadata capacity %u bytes\n", $records, $regions * $uploads, $upload_cb->copies.capacity
if $records != $regions * $uploads
  quit 1
end
if $upload_cb->copies.capacity >= 32768
  quit 1
end
quit 0
