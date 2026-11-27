# GPU->CPU readback of a mapped (display) surface: Vulkan sync audit
Library /tmp/wt-integ2 @ 345f14e. Read-only. All paths below are under src/. Line numbers are as of that HEAD (they differ a little from the "~" hints in the request).

## 0. Verdict
Short answer: the chain has ONE spec-level hole and several logic-level ways to hand the guest stale or partial data. wbverify cannot see any of them.

1. Spec hole (inferred from the spec, not reproduced): no barrier in the chain makes the image->staging copy writes available/visible to the HOST. Every barrier is ALL_COMMANDS -> ALL_COMMANDS. A vkWaitForFences does not fill the gap. The staging buffer can therefore be stale on a driver that does not flush on submission completion. In practice most drivers do flush, so this is a latent bug, not a known reproducer.
2. wbverify (ms_wb_verify, protocol/arm64_resources.c:490) compares guest pages against ms->stage_map, which is the thing that was just written to the guest from stage_map (:521). It is a write/read round-trip check. It never compares against the GPU image. "equal" therefore proves nothing about the GPU->stage leg or about what the guest sees through its own mapping.

## 1. The chain, with file:line

### 1.1 Render end
- protocol/arm64_exec_decode.c:297: after the last segment, lagfx_a64_exec_end(p).
- protocol/arm64_exec_run.c:1511-1517: exec_end calls pass_end(r) (:1514), then lagfx_a64_res_end(x) (:1517).
- pass_end, protocol/arm64_exec_run.c:757-776: lagfx_translate_render_end (translate/render_encoder.c:458, vkCmdEndRendering, only if render_pass_active). It then adds a SHADER_WRITE barrier if storage was written (:759-763). Finally it calls lagfx_a64_res_target_end per colour target (:765).
- target_end, protocol/arm64_resources.c:1444-1455: VkImageMemoryBarrier, src COLOR_ATTACHMENT_OUTPUT / COLOR_ATTACHMENT_WRITE, dst ALL_COMMANDS / (TRANSFER_READ|TRANSFER_WRITE|SHADER_READ|SHADER_WRITE|COLOR_ATTACHMENT_WRITE). Layout goes from COLOR_ATTACHMENT_OPTIMAL (or 1000232000 RENDERING_LOCAL_READ when have_local_read) to GENERAL.
- Result: the dynamic render IS ended (vkCmdEndRendering) before any copy is recorded. The render->transfer dependency is correct. The access mask contains TRANSFER_READ and the dst stage ALL_COMMANDS covers TRANSFER.

### 1.2 Image -> staging copy (recorded in the same command buffer)
- protocol/arm64_resources.c:1204-1214 (lagfx_a64_res_end): for flag 4, or flag 2 with LAGFX_ARM64_MAPPED_WRITEBACK=1, it calls ms_record_down (:1211-1212).
- ms_record_down, :478-487: barrier(cb); vkCmdCopyImageToBuffer; barrier(cb). This goes through translate/blit_encoder.c:445-447.
- The copy is done with the image in VK_IMAGE_LAYOUT_GENERAL. There is NO transition to TRANSFER_SRC_OPTIMAL. GENERAL is a legal srcImageLayout for vkCmdCopyImageToBuffer.
- barrier(), :195-199: one global VkMemoryBarrier, srcStage ALL_COMMANDS, dstStage ALL_COMMANDS, srcAccess MEMORY_WRITE, dstAccess MEMORY_READ|MEMORY_WRITE.
  - This orders render -> copy (correct).
  - After the copy it orders copy -> end of command buffer only. The dst stage does not include PIPELINE_STAGE_HOST. The dst access does not include HOST_READ. There is no VkBufferMemoryBarrier on the staging buffer.
- Dedupe: ms_record_down returns early if (down_epoch == m->epoch && down_pending) (:479). That is fine inside one command buffer.

### 1.3 Barrier after the copy / host visibility
- The last barrier is the one at :484 (and again at :1214, res_end). Both are ALL_COMMANDS/MEMORY_WRITE -> ALL_COMMANDS/MEMORY_READ|WRITE.
- Spec: HOST_READ is only valid with the HOST stage. MEMORY_READ at stage ALL_COMMANDS does not include HOST_READ. HOST_BIT is a pseudo-stage that no recorded command executes in (from memory of the spec, see section 3; not confirmed by my fetch).
- Spec: vkWaitForFences does not close this. See section 3.

### 1.4 Submit and fence wait
- submit_fence, :163-174: one vkQueueSubmit on vk->graphics_queue with a fresh fence. No semaphores.
- res_end, :1216-1223: lagfx_vk_wait_bounded(x->vk, x->fence, 0) -> vkWaitForFences (vulkan/command.c:440-448), default 2000 ms (LAGFX_GPU_WAIT_MS / LAGFX_VK_WAIT_MS).
  - OK: exec_finish (:1173) runs, which does ms_finish_down and so reads stage_map.
  - TIMEOUT: x->pending is set, PENDING is returned, the stamp is deferred (exec_run.c:1524-1530), and the next poll runs exec_finish (:1197-1201, :1245-1255).
- Single queue, so there is no queue-family ownership issue. Buffers are SHARING_MODE_EXCLUSIVE (:132).

### 1.5 Staging allocation
- make_buf, :129-146: mem_type(...HOST_VISIBLE | HOST_COHERENT) (:135). mem_type (:123-128) returns the FIRST type with both bits. It does not prefer HOST_CACHED, does not avoid DEVICE_LOCAL, and gives no fallback if there is no coherent type (the allocation fails, :137 -> ms creation fails -> mapped_unsupported).
- Persistent map: vkMapMemory(WHOLE_SIZE) at :139, once, at creation. It lives until vkFreeMemory (free_buf, :152-155; "freeing unmaps"). Never unmapped between execs, which is legal.
- Stage buffer for a surface: :561, size w*h*bpp, usage TRANSFER_SRC|TRANSFER_DST. It is memset to 0 at creation (:141). The same buffer serves both directions (guest->image upload in ms_pull and image->guest in ms_record_down).
- vkInvalidateMappedMemoryRanges: not called anywhere in protocol/arm64_resources.c. grep over src/ finds invalidate/flush only in vulkan/pipeline.c and vulkan/cursor.c, which are unrelated. That is acceptable ONLY because non-coherent types are never selected. Coherency removes the invalidate. It does NOT remove the need for a device->host availability operation (see 3).

### 1.6 Layout / contents between execs
- The image is created UNDEFINED (:390) and is moved to GENERAL and zero-cleared once at creation (:403-410, submit_wait).
- Every use afterwards is GENERAL -> attachment layout -> GENERAL (target_begin :1425-1442, target_end :1444-1455). oldLayout is always GENERAL, never UNDEFINED, so the layout transitions themselves do not discard contents.
- loadOp comes from the guest's pass descriptor (protocol/arm64_exec_run.c:831-839): Metal 2 -> CLEAR, 1 -> LOAD, 0 -> DONT_CARE. storeOp is always STORE.
  - DONT_CARE means the render-area contents are undefined in Vulkan terms if the pass does not cover every pixel. The same is true in Metal, but a Vulkan driver (tiler) may actually scramble them. These bytes are copied to the guest.
  - The image is left in GENERAL after the exec, ready for the next frame. No re-transition is required.
- Depth/stencil images stay GENERAL throughout (:1457-1484).

### 1.7 fbfetch / input attachment passes
- exec_run.c:742-752: inside the render pass, a VkMemoryBarrier COLOR_ATTACHMENT_WRITE -> INPUT_ATTACHMENT_READ|COLOR_ATTACHMENT_READ|WRITE, BY_REGION, stages COLOR_ATTACHMENT_OUTPUT|FRAGMENT_SHADER on both sides. This needs dynamicRenderingLocalRead. If have_local_read is false the draw is skipped and counted (:726).
- This barrier is local to the pass. The pass-end barrier (target_end) then covers the render -> transfer dependency exactly like a non-fbfetch pass. So fbfetch passes need nothing extra for the readback, and are followed by the right barrier. I did not check whether the by-region subpass-self-dependency validity rules hold on every driver.

## 2. Ordering relative to the guest completion stamp and the display swap

Immediate mode (LAGFX_ARM64_MAPPED_WRITEBACK=1, or flag 4 in any mode)
1. exec_end -> res_end submits, waits.
2. exec_finish -> ms_finish_down (:511-527) writes stage_map to the guest pages via shell->write_memory (surface_writeback.c:61-105), then runs wbverify if LAGFX_WB_VERIFY=1.
3. exec_end returns DONE; lagfx_arm64_submit_end (protocol/arm64_completion.c:262-277) completes the stamp at once when nwork==0 and no earlier entry is queued: complete_stamp_slot (stamp.c:100-107), which calls complete_stamp_slot_noirq and raises the IRQ.
Order: GPU done -> guest pages written -> stamp. Correct. If the wait times out, the stamp is held back until res_poll -> exec_finish (the write-back included) via pending_wait (arm64_exec_run.c:1499-1507), then retire_locked (arm64_completion.c:~236-257) calls complete_stamp_slot_noirq. Also correct.

Swap mode (LAGFX_ARM64_MAPPED_WRITEBACK=swap, flag 2 without flag 4)
1. exec_finish only marks ms->owed = true (:1177). No copy is recorded in the exec (:1211-1212 gate on flag 4 or mode 1).
2. The stamp completes immediately (as above). The guest sees "done" while its pages still hold the OLD pixels.
3. The write-back happens at the next display op (0x01/0x02/0x08): dispatchers/channel_compute_dispatcher.c:120 -> lagfx_a64_res_display_swap (:1318). It records a NEW command buffer (epoch++, ms_record_down per owed surface), submits, waits (bounded), then swap_done -> ms_finish_down (:1291-1298), and only then forwards display_command to the shell (:121-128).
Order: stamp -> (guest CPU may read here) -> swap copy -> guest pages -> shell. The gap between stamp and display op is a window where the guest has a completed-stamp surface that is not yet written. If display_swap hits a pending exec / a timed-out swap fence, the surfaces stay owed (swap_deferred, :1323-1327, :1343-1348) and the display command is still forwarded: the shell shows one frame late.

## 3. Comparison with the Vulkan docs

Fetched OK:
- docs.vulkan.org/guide/latest/synchronization_examples.html. Section "Transfer Dependencies" -> "CPU read back of data written by a compute shader" (the only readback example on the page; there is no section literally called "Transfer/readback" or "Host reads of GPU written data"). It records a global memory barrier srcStage COMPUTE_SHADER / srcAccess SHADER_WRITE -> dstStage HOST / dstAccess HOST_READ, then submits with a fence. The page then says: wait on the fence; "If the memory is host coherent, skip this step - otherwise invalidation is necessary" (vkInvalidateMappedMemoryRanges).
  - Applied here: the analogue is srcStage TRANSFER / TRANSFER_WRITE -> dstStage HOST / HOST_READ after the copy. This code uses ALL_COMMANDS/MEMORY_* on both sides. The invalidate is correctly skipped (coherent only). The HOST barrier is missing.
- docs.vulkan.org/spec/latest/chapters/synchronization.html (the page is ~298k chars; I read three ~100k slices, so the range up to ~300k was covered).
  - Section "Fences": the first access scope "includes all memory access performed by the device"; the second access scope is empty; for vkWaitForFences the second synchronization scope is the host operation that observes the signal. The excerpt returned by the fetch tool states plainly that waiting does not itself make device writes host visible: "the access scope of a memory dependency defined by a fence only includes device access". So a fence wait gives an execution dependency, not a device->host memory dependency.
  - Section "Access Types" table: HOST_READ is supported only by the HOST stage. HOST_COHERENT: host-domain writes are made visible automatically; vkInvalidateMappedMemoryRanges is required otherwise ("must be called to guarantee that writes which are available to the host domain are made visible to host operations"). Coherent therefore covers visibility from the host domain, not availability from the device.
  - Section "Memory Barriers / Buffer Memory Barriers": the destination access mask including HOST_READ triggers a memory domain operation. The fetch result was cut off in the middle of that sentence ("If the destination access mask includes VK_ACCESS_HOST_WRITE_BIT or VK_ACCESS_HOST_READ_BIT,") so I could not read the rest verbatim.
  - Section "Wait Idle Operations": vkQueueWaitIdle is equivalent to submitting a fence and waiting; this says nothing extra about visibility.
  - Section "Host Write Ordering Guarantees": this is the host->device direction only (host writes are visible to a vkQueueSubmit). It has no bearing on device->host.

NOT fetched / not confirmed verbatim (cited from memory, mark as inferred):
- The definition of VK_ACCESS_2_MEMORY_READ_BIT ("treated as equivalent to setting all READ access flags valid where it is used"). My own reading is that at stage ALL_COMMANDS this does not include HOST_READ, because HOST_READ is valid only with the HOST stage.
- The wording that ALL_COMMANDS does not include the HOST stage. The fetch did not return the definitions of ALL_COMMANDS or of the HOST stage (the tool answer said so explicitly).
- The sentence in "Memory Barriers" about HOST_READ in the destination access mask, quoted above in truncated form.
- Anything under "Availability, Visibility" memory domain operations beyond what is listed above.

Conclusion from the docs: the guide's pattern for a coherent readback is barrier(... -> HOST, HOST_READ) + fence wait; this code has the fence wait but not the barrier. This is a spec violation (inferred, since the "not included" wording was not fetched). It will usually work because the driver flushes at the end of the command buffer or at fence signal.

## 4. Cases where stale/partial data can reach the guest while wbverify says "equal"
wbverify (arm64_resources.c:490-505, vulkan/surface_writeback.c:108-132) reads the guest pages through shell->read_memory and compares them with stage_map. It is skipped for bpp != 4 (RGhA / half-float surfaces, :493), is capped at LAGFX_WBV_MAX_COMPARES, logs at most 40 lines, and only runs after a successful write (:521-523).

A. Missing HOST_READ barrier (section 1.3): stage_map is stale for the host-visible view; the stale bytes are then written to the guest and compared to themselves. Equal. (Spec-level; no driver observed.)
B. down_pending left true after a failed submit: res_end (:1216-1222) frees x without clearing down_pending on its surfaces (swap mode clears it at :1343, immediate mode does not). A later flag-4 exec in which ms_record_down returns false (host_valid false, :479) still reaches ms_finish_down, which sees down_pending and writes the OLD stage_map to the guest. Equal, because verify compares stage with guest.
C. Surface rendered but host_valid false (initial pull failed: no descriptor, size change, :460-466): ms_record_down returns false, nothing is copied, nothing is written, and the guest keeps whatever it had. Not "stale staging", but stale guest. wbverify never runs.
D. ms_finish_down returns 0 when the descriptor is unreadable (:513-516) after clearing down_pending. The surface is NOT retried in immediate mode (in swap mode owed stays set). No verify is run. The guest has the old pixels.
E. Swap mode: stamp completes before the write-back (section 2). Any guest read of the surface in that window, a CPU screenshot, or a later exec with flag 1 on the same surface sees old data. ms_pull (:460-474) also resets ms->owed = false (:473) and uploads the guest's old pixels over the GPU image, so the GPU's frame is lost; verify later compares the guest's own old pixels with themselves.
F. Swap mode with a timed-out swap fence or a pending exec: swap_deferred; the display command is forwarded before the pixels are written (:1318-1349). wbverify runs later in swap_done (:1291-1298), by which time the shell may already have shown the stale frame.
G. Partial writes: guest pages absent (page-list entry without bit 0, surface_writeback.c:83-96) are skipped, and pages_skipped is counted. Verify zero-fills unreadable pages, so a mismatch is counted there. Pages present but mapped to other memory than the guest's actual view (see H) are not detected.
H. Layout/format errors are invisible: both the write and the verify use the same xfer layout (first_offset, row_bytes, page list). A wrong row_bytes/first_offset, B8G8R8A8 vs the guest's expectation, SRGB view, premultiplied alpha, or the RGhA conversion, is round-trip consistent. The same goes for what the guest's own CPU mapping sees (guest cache / mapping aliasing). wbverify only proves that write_memory + read_memory agree.
I. loadOp DONT_CARE (exec_run.c:831-839) with a partial render area (viewport/scissor smaller than the attachment, or an attachment size mismatch, :833): undefined pixels in the image are read back and written.
J. Image sharing: two objects with the same mapping id share one image and stage (mapped_shared, :569-571). If the surface was rendered through a different ref in a later epoch, the down copy captures whatever the image holds at that time. Fine for correctness, but "which frame" is determined by when the exec ends, not by the guest's intent.
K. Dedupe (:479): if down_pending is set from a previous recording in the same epoch, the second ms_record_down returns true without recording. In-exec this is fine. If a failed submit leaves it set (case B) and the epoch counter has not advanced, it would be skipped silently. Epoch is bumped by res_begin (:809), sync_to_guest (:1268) and display_swap (:1333), so this needs a very specific sequence. Not shown to occur.

## 5. Status
VERIFIED (read in the code at 345f14e):
- Render is ended with vkCmdEndRendering before any copy is recorded; target_end barrier src COLOR_ATTACHMENT_OUTPUT/WRITE -> dst ALL_COMMANDS with TRANSFER_READ.
- The copy uses GENERAL layout, with barrier() before and after; no TRANSFER_SRC transition; no buffer barrier; no HOST stage or HOST_READ anywhere in arm64_resources.c (only ALL_COMMANDS/MEMORY_*).
- Staging memory is HOST_VISIBLE|HOST_COHERENT (first matching type), persistently mapped, never invalidated; non-coherent is never used.
- Image is GENERAL between execs; oldLayout is never UNDEFINED after creation; contents depend on the guest's loadOp.
- Submit + bounded fence wait happen before ms_finish_down; in immediate mode the stamp completes after the guest write; in swap mode before it.
- wbverify compares guest pages with stage_map only.
INFERRED:
- The missing HOST dst barrier is a spec violation despite the fence wait (the fence's access scope is device-only per spec "Fences"; the guide's readback example uses a HOST_READ barrier); MEMORY_READ/ALL_COMMANDS does not include host access.
- Practical impact on real drivers is likely nil on coherent memory, but cannot be proven here.
- Cases B, E, F follow from reading the code paths; not exercised.
NOT CHECKED:
- No VM boot, no run, no driver testing, no validation layers (so I have no VUID output and no sync-validation result).
- The exact "HOST_BIT / ALL_COMMANDS / MEMORY_READ" definitions in the spec (see section 3).
- Thread model: whether display ops (dispatcher) and exec drain can run concurrently on different threads (a64_exec_trylock exists at arm64_completion.c:292-298; I did not trace every caller).
- The memory-type order on any actual device (whether the first coherent type is DEVICE_LOCAL/BAR, uncached, etc.).
- The shell side: how write_memory reaches the guest and whether the guest CPU needs cache maintenance.
- bpp 8 (RGhA) surfaces under wbverify (skipped by design).

## 6. Minimal fixes (description only, none applied)
1. After the last image->buffer copy (in ms_record_down, and the other flag-4 readback paths at :1127-1146 snapshots, :872, :927, :1498), add a barrier with srcStage TRANSFER / srcAccess TRANSFER_WRITE, dstStage HOST / dstAccess HOST_READ (a global VkMemoryBarrier or a VkBufferMemoryBarrier on ms->stage). Keep the fence wait. That is exactly the docs.vulkan.org pattern. Optionally do the same at the final barrier in res_end (:1214) so it covers every snapshot buffer.
2. In ms_record_down failure/early-return paths, and in res_end's submit-failure path (:1216-1222), reset ms->down_pending = false for every surface in x->res (swap mode already does at :1343). In ms_finish_down, also require that this exec recorded the copy (compare down_epoch with the epoch of the exec).
3. Make wbverify meaningful: in ms_wb_verify also read the GPU image through an independent path (a second vkCmdCopyImageToBuffer into a scratch buffer, with the HOST barrier) and compare it with stage_map. Or compute a hash of the image side in the fence-signalled cb. Without that, wbverify only checks write_memory/read_memory consistency.
4. Swap mode: either complete the stamp only after the owed surfaces are paid, or at least pay owed surfaces before any exec with flag 1 on the same mapping instead of letting ms_pull (:473) discard them; and do not forward the display command when the swap copy is pending (or count that as a "stale frame" failure).
5. If a failed ms_finish_down (descriptor unreadable, :513-516) must not lose the frame in immediate mode, set ms->owed = true so the next display/sync pays it.
6. Optional hardening: prefer a HOST_CACHED coherent type for staging readback buffers in make_buf (readbacks from uncached WC memory are slow, not wrong), and fall back to non-coherent + vkInvalidateMappedMemoryRanges.
