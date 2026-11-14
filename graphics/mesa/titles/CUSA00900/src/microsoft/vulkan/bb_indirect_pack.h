/* A slab lives on internal_bufs until Vulkan resets the completed command
 * buffer. Slots never overlap; overflow allocates a new slab with the same
 * original lifetime. No new CPU readback and no cross-frame state assumptions. */
static void bb_ifast_transition(struct dzn_cmd_buffer* c,D3D12_RESOURCE_STATES next){
 const D3D12_RESOURCE_STATES old=c->bb_arg_state;if(old==next)return;
 if(c->enhanced_barriers){
  dzn_cmd_buffer_buffer_barrier(c,c->bb_arg_slab,
    old==D3D12_RESOURCE_STATE_COPY_DEST?D3D12_BARRIER_SYNC_COPY:D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
    next==D3D12_RESOURCE_STATE_COPY_DEST?D3D12_BARRIER_SYNC_COPY:D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
    old==D3D12_RESOURCE_STATE_COPY_DEST?D3D12_BARRIER_ACCESS_COPY_DEST:D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,
    next==D3D12_RESOURCE_STATE_COPY_DEST?D3D12_BARRIER_ACCESS_COPY_DEST:D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
 }else dzn_cmd_buffer_queue_transition_barriers(c,c->bb_arg_slab,0,1,old,next,DZN_QUEUE_TRANSITION_FLUSH);
 c->bb_arg_state=next;
}
static bool bb_ifast_pack(struct dzn_cmd_buffer* c,ID3D12Resource* src,uint64_t source_offset,
 unsigned whole_bytes,unsigned constants_offset,unsigned constants_bytes,ID3D12Resource** out,uint64_t* offset){
 const unsigned bytes=whole_bytes+constants_bytes;
 if(!c->bb_arg_slab||c->bb_arg_cursor+bytes>65536){
  ID3D12Resource* slab=NULL;
  if(dzn_cmd_buffer_alloc_internal_buf(c,65536,DZN_INTERNAL_BUF_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST,0,&slab,NULL)!=VK_SUCCESS)return false;
  c->bb_arg_slab=slab;c->bb_arg_cursor=0;c->bb_arg_state=D3D12_RESOURCE_STATE_COPY_DEST;atomic_fetch_add(&bb_ifast_slabs,1);
 }
 bb_ifast_transition(c,D3D12_RESOURCE_STATE_COPY_DEST);
 if(c->enhanced_barriers)dzn_cmd_buffer_buffer_barrier(c,src,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_ACCESS_COPY_SOURCE);
 else dzn_cmd_buffer_queue_transition_barriers(c,src,0,1,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_COPY_SOURCE,DZN_QUEUE_TRANSITION_FLUSH);
 const uint64_t at=c->bb_arg_cursor;
 ID3D12GraphicsCommandList1_CopyBufferRegion(c->cmdlist,c->bb_arg_slab,at,src,source_offset+constants_offset,constants_bytes);
 ID3D12GraphicsCommandList1_CopyBufferRegion(c->cmdlist,c->bb_arg_slab,at+constants_bytes,src,source_offset,whole_bytes);
 if(c->enhanced_barriers)dzn_cmd_buffer_buffer_barrier(c,src,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_COPY_SOURCE,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
 else dzn_cmd_buffer_queue_transition_barriers(c,src,0,1,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,DZN_QUEUE_TRANSITION_FLUSH);
 bb_ifast_transition(c,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
 *out=c->bb_arg_slab;*offset=at;c->bb_arg_cursor=ALIGN_POT(at+bytes,4);return true;
}
