/* Presentation diagnostics. Preserve vita2d's callback bytes and all GPU/display
 * dependencies. Only the opt-in probe adds an explicit fragment wait after
 * queue submission; ordinary tracing never waits on a notification. */
static uint64_t present_scene_end;
static SceGxmDisplayQueueCallback *present_callback;
static unsigned present_callback_size;
static bool present_wrapped, present_probe;
static atomic_uint present_submitted, present_completed;
typedef struct {
    uint64_t scene_end, queued;
    SceGxmNotification fence;
    unsigned frame;
    unsigned char original[64];
} present_payload;
int __real_sceGxmInitialize(const SceGxmInitializeParams *);
int __real_sceGxmPadHeartbeat(const SceGxmColorSurface *,SceGxmSyncObject *);
int __real_sceGxmDisplayQueueAddEntry(SceGxmSyncObject *,SceGxmSyncObject *,const void *);
static bool present_done(SceGxmNotification fence){
    return fence.address && (int32_t)(*fence.address-fence.value)>=0;
}
static void traced_display_callback(const void *data){
    const present_payload *p=data;
    uint64_t begin=sceKernelGetProcessTimeWide();
    bool done=present_done(p->fence);
    present_callback(p->original);
    uint64_t end=sceKernelGetProcessTimeWide();
    atomic_fetch_add_explicit(&present_completed,1,memory_order_release);
    /* Callback thread exclusively owns its statistics. No shared 64-bit writes. */
    static uint64_t latency, queue_latency, work;
    static unsigned samples,incomplete;
    latency+=begin-p->scene_end;queue_latency+=begin-p->queued;work+=end-begin;
    incomplete+=!done;
    if(++samples==60){
        VLOG("[PRESENT_CALLBACK] frame=%u scene_to_callback=%.2f queue_to_callback=%.2f work=%.2f incomplete=%u core=%d\n",
            p->frame,latency/60000.0,queue_latency/60000.0,work/60000.0,incomplete,sceKernelGetCpuId());
        samples=incomplete=0;latency=queue_latency=work=0;
    }
}
int __wrap_sceGxmInitialize(const SceGxmInitializeParams *params){
    SceGxmInitializeParams copy=*params;
    present_callback=params->displayQueueCallback;
    present_callback_size=params->displayQueueCallbackDataSize;
    present_wrapped=present_callback && present_callback_size<=sizeof(((present_payload *)0)->original);
    FILE *f=fopen("ux0:/data/raverace_gpu_probe.enable","rb");present_probe=f!=NULL;if(f)fclose(f);
    if(present_wrapped){copy.displayQueueCallback=traced_display_callback;copy.displayQueueCallbackDataSize=sizeof(present_payload);}
    VLOG("[PRESENT_TRACE] wrapped=%d probe=%d pending_limit=%u callback_bytes=%u flags=%08X\n",
        present_wrapped,present_probe,params->displayQueueMaxPendingCount,present_callback_size,params->flags);
    return __real_sceGxmInitialize(&copy);
}
static uint64_t present_probe_us,present_heartbeat_us;
static unsigned present_before,present_after;
int __wrap_sceGxmPadHeartbeat(const SceGxmColorSurface *surface,SceGxmSyncObject *sync){
    uint64_t begin=sceKernelGetProcessTimeWide();
    int r=__real_sceGxmPadHeartbeat(surface,sync);
    present_heartbeat_us+=sceKernelGetProcessTimeWide()-begin;
    return r;
}
int __wrap_sceGxmDisplayQueueAddEntry(SceGxmSyncObject *old,SceGxmSyncObject *next,const void *data){
    SceGxmNotification fence=frame_fence[frame_slot];
    uint64_t wait_begin=sceKernelGetProcessTimeWide();
    present_before+=present_done(fence);
    uint64_t begin=wait_begin;
    unsigned submitted=atomic_fetch_add_explicit(&present_submitted,1,memory_order_relaxed)+1;
    unsigned pending=submitted-atomic_load_explicit(&present_completed,memory_order_acquire);
    present_payload p={.scene_end=present_scene_end,.queued=begin,.fence=frame_fence[frame_slot],.frame=g_eng_frame};
    if(present_wrapped)memcpy(p.original,data,present_callback_size);
    int r=__real_sceGxmDisplayQueueAddEntry(old,next,present_wrapped?(const void *)&p:data);
    uint64_t end=sceKernelGetProcessTimeWide();
    bool ready_at_return=present_done(fence);
    if(present_probe && r>=0){
        if(!frame_fence_ready || !frame_fence_pending[frame_slot]){
            VLOG("[PRESENT_TRACE] missing probe fence\n");abort();
        }
        int result=sceGxmNotificationWait(&fence);
        if(result<0){VLOG("[PRESENT_TRACE] probe wait failed %08X\n",result);abort();}
    }
    present_after+=present_done(fence);
    present_probe_us+=sceKernelGetProcessTimeWide()-end;
    if(r<0){atomic_fetch_sub_explicit(&present_submitted,1,memory_order_relaxed);VLOG("[PRESENT_TRACE] queue failed %08X\n",r);}
    static uint64_t queue_us;
    static unsigned samples,done_after,max_pending;
    queue_us+=end-begin;done_after+=ready_at_return;if(pending>max_pending)max_pending=pending;
    if(++samples==60){
        VLOG("[PRESENT] frame=%u probe=%.2f heartbeat=%.2f queue=%.2f done_before=%u done_probe=%u done_queue=%u max_pending=%u\n",
            g_eng_frame,present_probe_us/60000.0,present_heartbeat_us/60000.0,queue_us/60000.0,
            present_before,present_after,done_after,max_pending);
        samples=done_after=max_pending=present_before=present_after=0;
        queue_us=present_probe_us=present_heartbeat_us=0;
    }
    return r;
}
