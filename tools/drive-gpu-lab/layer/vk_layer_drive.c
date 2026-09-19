// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "dgl_protocol.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define DGL_MAX_INSTANCES 8
#define DGL_MAX_DEVICES 8
#define DGL_MAX_PHYS 32
#define DGL_MAX_QUEUES 64
#define DGL_MAX_ALLOCATIONS 16384

typedef struct instance_ctx {
    void *key;
    VkInstance instance;
    PFN_vkGetInstanceProcAddr next_gipa;
    PFN_GetPhysicalDeviceProcAddr next_gpdpa;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices;
    PFN_vkGetPhysicalDeviceProperties get_physical_device_properties;
    PFN_vkGetPhysicalDeviceProperties2 get_physical_device_properties2;
} instance_ctx;

typedef struct physical_ctx {
    VkPhysicalDevice physical;
    VkInstance instance;
} physical_ctx;

typedef struct device_ctx {
    void *key;
    VkDevice device;
    PFN_vkGetDeviceProcAddr next_gdpa;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkGetDeviceQueue get_device_queue;
    PFN_vkGetDeviceQueue2 get_device_queue2;
    PFN_vkQueuePresentKHR queue_present;
    PFN_vkQueueSubmit queue_submit;
    PFN_vkQueueSubmit2 queue_submit2;
    PFN_vkQueueSubmit2KHR queue_submit2_khr;
    PFN_vkCreateSwapchainKHR create_swapchain;
    PFN_vkDestroySwapchainKHR destroy_swapchain;
    PFN_vkAllocateMemory allocate_memory;
    PFN_vkFreeMemory free_memory;
    volatile LONG64 allocated_bytes;
    volatile LONG64 peak_allocated_bytes;
    volatile LONG64 submits;
    volatile LONG64 frames;
    volatile LONG64 last_qpc;
} device_ctx;

typedef struct queue_ctx {
    VkQueue queue;
    device_ctx *device;
} queue_ctx;

typedef struct allocation_ctx {
    VkDeviceMemory memory;
    device_ctx *device;
    VkDeviceSize size;
} allocation_ctx;

static CRITICAL_SECTION g_lock;
static instance_ctx g_instances[DGL_MAX_INSTANCES];
static physical_ctx g_phys[DGL_MAX_PHYS];
static device_ctx g_devices[DGL_MAX_DEVICES];
static queue_ctx g_queues[DGL_MAX_QUEUES];
static allocation_ctx g_allocations[DGL_MAX_ALLOCATIONS];
static PFN_vkGetInstanceProcAddr g_fallback_gipa = NULL;
static PFN_GetPhysicalDeviceProcAddr g_fallback_gpdpa = NULL;
static INIT_ONCE g_udp_once = INIT_ONCE_STATIC_INIT;
static SOCKET g_udp = INVALID_SOCKET;
static struct sockaddr_in g_udp_dst;
static volatile LONG64 g_udp_seq = 0;
static LARGE_INTEGER g_qpc_freq;

static void *dispatch_key(const void *dispatchable) {
    return dispatchable ? *(void * const *)dispatchable : NULL;
}

static BOOL CALLBACK udp_init_once(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    WSADATA wd;
    u_long nonblock = 1;
    char port_buf[32];
    unsigned port = DGL_DEFAULT_UDP_PORT;
    (void)once; (void)param; (void)ctx;
    if (GetEnvironmentVariableA("DRIVE_G720_PROBE_PORT", port_buf, (DWORD)sizeof(port_buf)) > 0) {
        unsigned v = (unsigned)strtoul(port_buf, NULL, 10);
        if (v > 0 && v <= 65535) port = v;
    }
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return TRUE;
    g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp == INVALID_SOCKET) return TRUE;
    ioctlsocket(g_udp, FIONBIO, &nonblock);
    memset(&g_udp_dst, 0, sizeof(g_udp_dst));
    g_udp_dst.sin_family = AF_INET;
    g_udp_dst.sin_port = htons((u_short)port);
    g_udp_dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    QueryPerformanceFrequency(&g_qpc_freq);
    return TRUE;
}

static void emit_json(const char *json) {
    if (!json) return;
    InitOnceExecuteOnce(&g_udp_once, udp_init_once, NULL, NULL);
    if (g_udp != INVALID_SOCKET) {
        (void)sendto(g_udp, json, (int)strlen(json), 0,
                     (const struct sockaddr *)&g_udp_dst, (int)sizeof(g_udp_dst));
    }
}

static void safe_gpu_name(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    if (!cap) return;
    while (src && *src && i + 1 < cap) {
        char c = *src++;
        if (c == '"' || c == '\\' || (unsigned char)c < 0x20) c = '_';
        dst[i++] = c;
    }
    dst[i] = '\0';
}

static instance_ctx *find_instance(VkInstance instance) {
    void *key = dispatch_key(instance);
    int i;
    for (i = 0; i < DGL_MAX_INSTANCES; ++i) if (g_instances[i].key == key && key) return &g_instances[i];
    return NULL;
}

static instance_ctx *single_instance_fallback(void) {
    int i;
    instance_ctx *only = NULL;
    for (i = 0; i < DGL_MAX_INSTANCES; ++i) {
        if (!g_instances[i].key) continue;
        if (only) return NULL;
        only = &g_instances[i];
    }
    return only;
}

static VkInstance instance_for_physical(VkPhysicalDevice physical) {
    int i;
    for (i = 0; i < DGL_MAX_PHYS; ++i) if (g_phys[i].physical == physical) return g_phys[i].instance;
    {
        instance_ctx *one = single_instance_fallback();
        return one ? one->instance : VK_NULL_HANDLE;
    }
}

static device_ctx *find_device_by_dispatch(const void *handle) {
    void *key = dispatch_key(handle);
    int i;
    for (i = 0; i < DGL_MAX_DEVICES; ++i) if (g_devices[i].key == key && key) return &g_devices[i];
    return NULL;
}

static device_ctx *find_device_for_queue(VkQueue queue) {
    int i;
    if (!queue) return NULL;
    for (i = 0; i < DGL_MAX_QUEUES; ++i)
        if (g_queues[i].queue == queue && g_queues[i].device) return g_queues[i].device;
    return find_device_by_dispatch(queue);
}

static void register_queue(VkQueue queue, device_ctx *device) {
    int i, empty = -1;
    if (!queue || !device) return;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < DGL_MAX_QUEUES; ++i) {
        if (g_queues[i].queue == queue) { g_queues[i].device = device; LeaveCriticalSection(&g_lock); return; }
        if (empty < 0 && !g_queues[i].queue) empty = i;
    }
    if (empty >= 0) { g_queues[empty].queue = queue; g_queues[empty].device = device; }
    LeaveCriticalSection(&g_lock);
}

static void unregister_device_queues(device_ctx *device) {
    int i;
    for (i = 0; i < DGL_MAX_QUEUES; ++i) {
        if (g_queues[i].device == device) { g_queues[i].queue = VK_NULL_HANDLE; g_queues[i].device = NULL; }
    }
}


static void emit_memory(device_ctx *device) {
    char json[384];
    LONG64 allocated, peak;
    if (!device) return;
    allocated = InterlockedCompareExchange64(&device->allocated_bytes, 0, 0);
    peak = InterlockedCompareExchange64(&device->peak_allocated_bytes, 0, 0);
    snprintf(json, sizeof(json),
             "{\"v\":%d,\"source\":\"vklayer\",\"type\":\"memory\",\"seq\":%lld,"
             "\"allocated_bytes\":%lld,\"peak_allocated_bytes\":%lld}",
             DGL_PROTOCOL_VERSION, (long long)InterlockedIncrement64(&g_udp_seq),
             (long long)allocated, (long long)peak);
    emit_json(json);
}

static void account_allocation(device_ctx *device, VkDeviceMemory memory, VkDeviceSize size) {
    int i, empty = -1;
    LONG64 now, peak;
    if (!device || !memory) return;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < DGL_MAX_ALLOCATIONS; ++i) {
        if (!g_allocations[i].memory && empty < 0) empty = i;
        if (g_allocations[i].memory == memory) { empty = i; break; }
    }
    if (empty >= 0) {
        g_allocations[empty].memory = memory;
        g_allocations[empty].device = device;
        g_allocations[empty].size = size;
        now = InterlockedExchangeAdd64(&device->allocated_bytes, (LONG64)size) + (LONG64)size;
        for (;;) {
            peak = InterlockedCompareExchange64(&device->peak_allocated_bytes, 0, 0);
            if (now <= peak || InterlockedCompareExchange64(&device->peak_allocated_bytes, now, peak) == peak) break;
        }
    }
    LeaveCriticalSection(&g_lock);
    emit_memory(device);
}

static void account_free(device_ctx *device, VkDeviceMemory memory) {
    int i;
    VkDeviceSize size = 0;
    if (!device || !memory) return;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < DGL_MAX_ALLOCATIONS; ++i) {
        if (g_allocations[i].memory == memory && g_allocations[i].device == device) {
            size = g_allocations[i].size;
            memset(&g_allocations[i], 0, sizeof(g_allocations[i]));
            break;
        }
    }
    if (size) InterlockedExchangeAdd64(&device->allocated_bytes, -(LONG64)size);
    LeaveCriticalSection(&g_lock);
    emit_memory(device);
}

static void cleanup_device_allocations(device_ctx *device) {
    int i;
    if (!device) return;
    for (i = 0; i < DGL_MAX_ALLOCATIONS; ++i) {
        if (g_allocations[i].device == device) memset(&g_allocations[i], 0, sizeof(g_allocations[i]));
    }
    InterlockedExchange64(&device->allocated_bytes, 0);
}

static void emit_device_lost(const char *operation, VkResult result) {
    char json[384];
    snprintf(json, sizeof(json),
             "{\"v\":%d,\"source\":\"vklayer\",\"type\":\"device_lost\",\"seq\":%lld,"
             "\"operation\":\"%s\",\"result\":%d}",
             DGL_PROTOCOL_VERSION, (long long)InterlockedIncrement64(&g_udp_seq),
             operation ? operation : "unknown", (int)result);
    emit_json(json);
}

static VkLayerInstanceCreateInfo *instance_chain_info(const VkInstanceCreateInfo *ci, VkLayerFunction func) {
    const VkBaseInStructure *p = (const VkBaseInStructure *)ci->pNext;
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO) {
            VkLayerInstanceCreateInfo *info = (VkLayerInstanceCreateInfo *)(uintptr_t)p;
            if (info->function == func) return info;
        }
        p = p->pNext;
    }
    return NULL;
}

static VkLayerDeviceCreateInfo *device_chain_info(const VkDeviceCreateInfo *ci, VkLayerFunction func) {
    const VkBaseInStructure *p = (const VkBaseInStructure *)ci->pNext;
    while (p) {
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) {
            VkLayerDeviceCreateInfo *info = (VkLayerDeviceCreateInfo *)(uintptr_t)p;
            if (info->function == func) return info;
        }
        p = p->pNext;
    }
    return NULL;
}

static void add_physical_mapping(VkInstance instance, uint32_t count, const VkPhysicalDevice *phys) {
    uint32_t j;
    int i;
    EnterCriticalSection(&g_lock);
    for (j = 0; j < count; ++j) {
        int existing = 0;
        for (i = 0; i < DGL_MAX_PHYS; ++i) {
            if (g_phys[i].physical == phys[j]) { g_phys[i].instance = instance; existing = 1; break; }
        }
        if (!existing) {
            for (i = 0; i < DGL_MAX_PHYS; ++i) if (!g_phys[i].physical) {
                g_phys[i].physical = phys[j]; g_phys[i].instance = instance; break;
            }
        }
    }
    LeaveCriticalSection(&g_lock);
}

VKAPI_ATTR VkResult VKAPI_CALL dgl_vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator,
                                                     VkInstance *pInstance);
VKAPI_ATTR void VKAPI_CALL dgl_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL dgl_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pCount, VkPhysicalDevice *pDevices);
VKAPI_ATTR VkResult VKAPI_CALL dgl_vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
                                                   const VkAllocationCallbacks *pAllocator, VkDevice *pDevice);
VKAPI_ATTR void VKAPI_CALL dgl_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator);
VKAPI_ATTR void VKAPI_CALL dgl_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue);
VKAPI_ATTR void VKAPI_CALL dgl_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *pQueueInfo, VkQueue *pQueue);
VKAPI_ATTR VkResult VKAPI_CALL dgl_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);
VKAPI_ATTR VkResult VKAPI_CALL dgl_vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL dgl_vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits, VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL dgl_vkQueueSubmit2KHR(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits, VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL dgl_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                         const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain);
VKAPI_ATTR void VKAPI_CALL dgl_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                       const VkAllocationCallbacks *pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL dgl_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
                                                    const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory);
VKAPI_ATTR void VKAPI_CALL dgl_vkFreeMemory(VkDevice device, VkDeviceMemory memory,
                                             const VkAllocationCallbacks *pAllocator);

__declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName);
__declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName);
__declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_layerGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName);

VKAPI_ATTR VkResult VKAPI_CALL dgl_vkCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator,
                                                     VkInstance *pInstance) {
    VkLayerInstanceCreateInfo *chain = instance_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
    PFN_vkGetInstanceProcAddr next_gipa;
    PFN_GetPhysicalDeviceProcAddr next_gpdpa;
    PFN_vkCreateInstance next_create;
    VkResult result;
    int i;
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    next_gpdpa = chain->u.pLayerInfo->pfnNextGetPhysicalDeviceProcAddr;
    next_create = (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    result = next_create(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) return result;

    EnterCriticalSection(&g_lock);
    g_fallback_gipa = next_gipa;
    g_fallback_gpdpa = next_gpdpa;
    for (i = 0; i < DGL_MAX_INSTANCES; ++i) if (!g_instances[i].key) {
        g_instances[i].key = dispatch_key(*pInstance);
        g_instances[i].instance = *pInstance;
        g_instances[i].next_gipa = next_gipa;
        g_instances[i].next_gpdpa = next_gpdpa;
        g_instances[i].destroy_instance = (PFN_vkDestroyInstance)next_gipa(*pInstance, "vkDestroyInstance");
        g_instances[i].enumerate_physical_devices = (PFN_vkEnumeratePhysicalDevices)next_gipa(*pInstance, "vkEnumeratePhysicalDevices");
        g_instances[i].get_physical_device_properties = (PFN_vkGetPhysicalDeviceProperties)next_gipa(*pInstance, "vkGetPhysicalDeviceProperties");
        g_instances[i].get_physical_device_properties2 = (PFN_vkGetPhysicalDeviceProperties2)next_gipa(*pInstance, "vkGetPhysicalDeviceProperties2");
        if (!g_instances[i].get_physical_device_properties2)
            g_instances[i].get_physical_device_properties2 = (PFN_vkGetPhysicalDeviceProperties2)next_gipa(*pInstance, "vkGetPhysicalDeviceProperties2KHR");
        break;
    }
    LeaveCriticalSection(&g_lock);
    { char json[256]; snprintf(json, sizeof(json), "{\"v\":%d,\"source\":\"vklayer\",\"type\":\"instance\",\"seq\":%lld}", DGL_PROTOCOL_VERSION, (long long)InterlockedIncrement64(&g_udp_seq)); emit_json(json); }
    return result;
}

VKAPI_ATTR void VKAPI_CALL dgl_vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    instance_ctx copy = {0};
    int i;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < DGL_MAX_INSTANCES; ++i) if (g_instances[i].key == dispatch_key(instance)) {
        copy = g_instances[i]; memset(&g_instances[i], 0, sizeof(g_instances[i])); break;
    }
    for (i = 0; i < DGL_MAX_PHYS; ++i) if (g_phys[i].instance == instance) memset(&g_phys[i], 0, sizeof(g_phys[i]));
    LeaveCriticalSection(&g_lock);
    if (copy.destroy_instance) copy.destroy_instance(instance, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL dgl_vkEnumeratePhysicalDevices(VkInstance instance, uint32_t *pCount, VkPhysicalDevice *pDevices) {
    instance_ctx *ctx = find_instance(instance);
    VkResult r;
    uint32_t before = pCount ? *pCount : 0;
    (void)before;
    if (!ctx || !ctx->enumerate_physical_devices) return VK_ERROR_INITIALIZATION_FAILED;
    r = ctx->enumerate_physical_devices(instance, pCount, pDevices);
    if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && pDevices && pCount) add_physical_mapping(instance, *pCount, pDevices);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL dgl_vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
                                                   const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    VkLayerDeviceCreateInfo *chain = device_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
    PFN_vkGetInstanceProcAddr next_gipa;
    PFN_vkGetDeviceProcAddr next_gdpa;
    PFN_vkCreateDevice next_create;
    VkInstance instance = instance_for_physical(physicalDevice);
    instance_ctx *ictx = instance ? find_instance(instance) : NULL;
    VkResult result;
    device_ctx *dctx = NULL;
    int i;
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    next_gipa = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    next_gdpa = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    next_create = (PFN_vkCreateDevice)next_gipa(instance, "vkCreateDevice");
    if (!next_create) return VK_ERROR_INITIALIZATION_FAILED;
    chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;
    result = next_create(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) return result;

    EnterCriticalSection(&g_lock);
    for (i = 0; i < DGL_MAX_DEVICES; ++i) if (!g_devices[i].key) {
        dctx = &g_devices[i];
        memset(dctx, 0, sizeof(*dctx));
        dctx->key = dispatch_key(*pDevice);
        dctx->device = *pDevice;
        dctx->next_gdpa = next_gdpa;
        dctx->destroy_device = (PFN_vkDestroyDevice)next_gdpa(*pDevice, "vkDestroyDevice");
        dctx->get_device_queue = (PFN_vkGetDeviceQueue)next_gdpa(*pDevice, "vkGetDeviceQueue");
        dctx->get_device_queue2 = (PFN_vkGetDeviceQueue2)next_gdpa(*pDevice, "vkGetDeviceQueue2");
        dctx->queue_present = (PFN_vkQueuePresentKHR)next_gdpa(*pDevice, "vkQueuePresentKHR");
        dctx->queue_submit = (PFN_vkQueueSubmit)next_gdpa(*pDevice, "vkQueueSubmit");
        dctx->queue_submit2 = (PFN_vkQueueSubmit2)next_gdpa(*pDevice, "vkQueueSubmit2");
        dctx->queue_submit2_khr = (PFN_vkQueueSubmit2KHR)next_gdpa(*pDevice, "vkQueueSubmit2KHR");
        dctx->create_swapchain = (PFN_vkCreateSwapchainKHR)next_gdpa(*pDevice, "vkCreateSwapchainKHR");
        dctx->destroy_swapchain = (PFN_vkDestroySwapchainKHR)next_gdpa(*pDevice, "vkDestroySwapchainKHR");
        dctx->allocate_memory = (PFN_vkAllocateMemory)next_gdpa(*pDevice, "vkAllocateMemory");
        dctx->free_memory = (PFN_vkFreeMemory)next_gdpa(*pDevice, "vkFreeMemory");
        break;
    }
    LeaveCriticalSection(&g_lock);

    if (ictx && ictx->get_physical_device_properties) {
        VkPhysicalDeviceProperties props;
        VkPhysicalDeviceDriverProperties driver_props;
        VkPhysicalDeviceProperties2 props2;
        char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
        char driver_name[VK_MAX_DRIVER_NAME_SIZE];
        char driver_info[VK_MAX_DRIVER_INFO_SIZE];
        char json[2048];
        bool have_driver_props = false;
        memset(&props, 0, sizeof(props));
        memset(&driver_props, 0, sizeof(driver_props));
        memset(&props2, 0, sizeof(props2));
        ictx->get_physical_device_properties(physicalDevice, &props);
        if (ictx->get_physical_device_properties2) {
            driver_props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
            props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props2.pNext = &driver_props;
            ictx->get_physical_device_properties2(physicalDevice, &props2);
            have_driver_props = driver_props.driverName[0] != '\0' || driver_props.driverInfo[0] != '\0';
        }
        safe_gpu_name(name, sizeof(name), props.deviceName);
        safe_gpu_name(driver_name, sizeof(driver_name), have_driver_props ? driver_props.driverName : "");
        safe_gpu_name(driver_info, sizeof(driver_info), have_driver_props ? driver_props.driverInfo : "");
        snprintf(json, sizeof(json),
                 "{\"v\":%d,\"source\":\"vklayer\",\"type\":\"device\",\"seq\":%lld,"
                 "\"gpu_name\":\"%s\",\"vendor_id\":%u,\"device_id\":%u,"
                 "\"api_version\":%u,\"api_version_string\":\"%u.%u.%u\","
                 "\"driver_version\":%u,\"driver_version_string\":\"0x%08x\","
                 "\"driver_name\":\"%s\",\"driver_info\":\"%s\",\"driver_id\":%u,"
                 "\"conformance_version\":\"%u.%u.%u.%u\"}",
                 DGL_PROTOCOL_VERSION, (long long)InterlockedIncrement64(&g_udp_seq), name,
                 props.vendorID, props.deviceID, props.apiVersion,
                 VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
                 VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion, props.driverVersion,
                 driver_name, driver_info, have_driver_props ? (unsigned)driver_props.driverID : 0u,
                 have_driver_props ? driver_props.conformanceVersion.major : 0u,
                 have_driver_props ? driver_props.conformanceVersion.minor : 0u,
                 have_driver_props ? driver_props.conformanceVersion.subminor : 0u,
                 have_driver_props ? driver_props.conformanceVersion.patch : 0u);
        emit_json(json);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL dgl_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    device_ctx copy = {0};
    int i;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < DGL_MAX_DEVICES; ++i) if (g_devices[i].key == dispatch_key(device)) {
        device_ctx *slot = &g_devices[i];
        copy = *slot;
        unregister_device_queues(slot);
        cleanup_device_allocations(slot);
        memset(slot, 0, sizeof(*slot));
        break;
    }
    LeaveCriticalSection(&g_lock);
    if (copy.destroy_device) copy.destroy_device(device, pAllocator);
}

VKAPI_ATTR void VKAPI_CALL dgl_vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue) {
    device_ctx *ctx = find_device_by_dispatch(device);
    if (!ctx || !ctx->get_device_queue) return;
    ctx->get_device_queue(device, queueFamilyIndex, queueIndex, pQueue);
    if (pQueue && *pQueue) register_queue(*pQueue, ctx);
}

VKAPI_ATTR void VKAPI_CALL dgl_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *pQueueInfo, VkQueue *pQueue) {
    device_ctx *ctx = find_device_by_dispatch(device);
    if (!ctx || !ctx->get_device_queue2) return;
    ctx->get_device_queue2(device, pQueueInfo, pQueue);
    if (pQueue && *pQueue) register_queue(*pQueue, ctx);
}

VKAPI_ATTR VkResult VKAPI_CALL dgl_vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence) {
    device_ctx *ctx = find_device_for_queue(queue);
    VkResult r;
    if (!ctx || !ctx->queue_submit) return VK_ERROR_INITIALIZATION_FAILED;
    InterlockedExchangeAdd64(&ctx->submits, (LONG64)submitCount);
    r = ctx->queue_submit(queue, submitCount, pSubmits, fence);
    if (r == VK_ERROR_DEVICE_LOST) emit_device_lost("vkQueueSubmit", r);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL dgl_vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits, VkFence fence) {
    device_ctx *ctx = find_device_for_queue(queue);
    VkResult r;
    if (!ctx || !ctx->queue_submit2) return VK_ERROR_INITIALIZATION_FAILED;
    InterlockedExchangeAdd64(&ctx->submits, (LONG64)submitCount);
    r = ctx->queue_submit2(queue, submitCount, pSubmits, fence);
    if (r == VK_ERROR_DEVICE_LOST) emit_device_lost("vkQueueSubmit2", r);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL dgl_vkQueueSubmit2KHR(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits, VkFence fence) {
    device_ctx *ctx = find_device_for_queue(queue);
    VkResult r;
    if (!ctx || !ctx->queue_submit2_khr) return VK_ERROR_INITIALIZATION_FAILED;
    InterlockedExchangeAdd64(&ctx->submits, (LONG64)submitCount);
    r = ctx->queue_submit2_khr(queue, submitCount, pSubmits, fence);
    if (r == VK_ERROR_DEVICE_LOST) emit_device_lost("vkQueueSubmit2KHR", r);
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL dgl_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
    device_ctx *ctx = find_device_for_queue(queue);
    VkResult r;
    LARGE_INTEGER qpc;
    LONG64 prev, now, frame_ns = 0, submits, frame;
    char json[768];
    if (!ctx || !ctx->queue_present) return VK_ERROR_INITIALIZATION_FAILED;
    r = ctx->queue_present(queue, pPresentInfo);
    QueryPerformanceCounter(&qpc);
    now = qpc.QuadPart;
    prev = InterlockedExchange64(&ctx->last_qpc, now);
    if (prev && g_qpc_freq.QuadPart > 0)
        frame_ns = (LONG64)((long double)(now - prev) * 1000000000.0L / (long double)g_qpc_freq.QuadPart);
    submits = InterlockedExchange64(&ctx->submits, 0);
    frame = InterlockedIncrement64(&ctx->frames);
    snprintf(json, sizeof(json),
             "{\"v\":%d,\"source\":\"vklayer\",\"type\":\"frame\",\"seq\":%lld,"
             "\"frame\":%lld,\"frame_ns\":%lld,\"submits\":%lld,\"present_result\":%d}",
             DGL_PROTOCOL_VERSION, (long long)InterlockedIncrement64(&g_udp_seq),
             (long long)frame, (long long)frame_ns, (long long)submits, (int)r);
    emit_json(json);
    if (r == VK_ERROR_DEVICE_LOST) {
        snprintf(json, sizeof(json),
                 "{\"v\":%d,\"source\":\"vklayer\",\"type\":\"device_lost\",\"seq\":%lld,\"result\":%d}",
                 DGL_PROTOCOL_VERSION, (long long)InterlockedIncrement64(&g_udp_seq), (int)r);
        emit_json(json);
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL dgl_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                         const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain) {
    device_ctx *ctx = find_device_by_dispatch(device);
    VkResult r;
    char json[768];
    if (!ctx || !ctx->create_swapchain) return VK_ERROR_EXTENSION_NOT_PRESENT;
    r = ctx->create_swapchain(device, pCreateInfo, pAllocator, pSwapchain);
    snprintf(json, sizeof(json),
             "{\"v\":%d,\"source\":\"vklayer\",\"type\":\"swapchain\",\"seq\":%lld,"
             "\"result\":%d,\"width\":%u,\"height\":%u,\"min_images\":%u,\"format\":%d,\"present_mode\":%d}",
             DGL_PROTOCOL_VERSION, (long long)InterlockedIncrement64(&g_udp_seq), (int)r,
             pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height, pCreateInfo->minImageCount,
             (int)pCreateInfo->imageFormat, (int)pCreateInfo->presentMode);
    emit_json(json);
    return r;
}

VKAPI_ATTR void VKAPI_CALL dgl_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                       const VkAllocationCallbacks *pAllocator) {
    device_ctx *ctx = find_device_by_dispatch(device);
    if (ctx && ctx->destroy_swapchain) ctx->destroy_swapchain(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL dgl_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
                                                    const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory) {
    device_ctx *ctx = find_device_by_dispatch(device);
    VkResult r;
    if (!ctx || !ctx->allocate_memory) return VK_ERROR_INITIALIZATION_FAILED;
    r = ctx->allocate_memory(device, pAllocateInfo, pAllocator, pMemory);
    if (r == VK_SUCCESS && pMemory && *pMemory && pAllocateInfo)
        account_allocation(ctx, *pMemory, pAllocateInfo->allocationSize);
    else if (r == VK_ERROR_DEVICE_LOST)
        emit_device_lost("vkAllocateMemory", r);
    return r;
}

VKAPI_ATTR void VKAPI_CALL dgl_vkFreeMemory(VkDevice device, VkDeviceMemory memory,
                                             const VkAllocationCallbacks *pAllocator) {
    device_ctx *ctx = find_device_by_dispatch(device);
    if (!ctx || !ctx->free_memory) return;
    if (memory) account_free(ctx, memory);
    ctx->free_memory(device, memory, pAllocator);
}

__declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *pName) {
    instance_ctx *ctx;
    if (!pName) return NULL;
    if (!strcmp(pName, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(pName, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(pName, "vkCreateInstance")) return (PFN_vkVoidFunction)dgl_vkCreateInstance;
    if (!strcmp(pName, "vkDestroyInstance")) return (PFN_vkVoidFunction)dgl_vkDestroyInstance;
    if (!strcmp(pName, "vkEnumeratePhysicalDevices")) return (PFN_vkVoidFunction)dgl_vkEnumeratePhysicalDevices;
    if (!strcmp(pName, "vkCreateDevice")) return (PFN_vkVoidFunction)dgl_vkCreateDevice;
    ctx = instance ? find_instance(instance) : NULL;
    if (ctx && ctx->next_gipa) return ctx->next_gipa(instance, pName);
    if (g_fallback_gipa) return g_fallback_gipa(instance, pName);
    return NULL;
}

__declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char *pName) {
    device_ctx *ctx;
    if (!pName) return NULL;
    if (!strcmp(pName, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(pName, "vkDestroyDevice")) return (PFN_vkVoidFunction)dgl_vkDestroyDevice;
    if (!strcmp(pName, "vkGetDeviceQueue")) return (PFN_vkVoidFunction)dgl_vkGetDeviceQueue;
    if (!strcmp(pName, "vkGetDeviceQueue2")) return (PFN_vkVoidFunction)dgl_vkGetDeviceQueue2;
    if (!strcmp(pName, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)dgl_vkQueuePresentKHR;
    if (!strcmp(pName, "vkQueueSubmit")) return (PFN_vkVoidFunction)dgl_vkQueueSubmit;
    if (!strcmp(pName, "vkQueueSubmit2")) return (PFN_vkVoidFunction)dgl_vkQueueSubmit2;
    if (!strcmp(pName, "vkQueueSubmit2KHR")) return (PFN_vkVoidFunction)dgl_vkQueueSubmit2KHR;
    if (!strcmp(pName, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)dgl_vkCreateSwapchainKHR;
    if (!strcmp(pName, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)dgl_vkDestroySwapchainKHR;
    if (!strcmp(pName, "vkAllocateMemory")) return (PFN_vkVoidFunction)dgl_vkAllocateMemory;
    if (!strcmp(pName, "vkFreeMemory")) return (PFN_vkVoidFunction)dgl_vkFreeMemory;
    ctx = device ? find_device_by_dispatch(device) : NULL;
    if (ctx && ctx->next_gdpa) return ctx->next_gdpa(device, pName);
    return NULL;
}

__declspec(dllexport) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_layerGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName) {
    instance_ctx *ctx = instance ? find_instance(instance) : NULL;
    if (ctx && ctx->next_gpdpa) return ctx->next_gpdpa(instance, pName);
    if (g_fallback_gpdpa) return g_fallback_gpdpa(instance, pName);
    return NULL;
}

__declspec(dllexport) VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (pVersionStruct->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = vk_layerGetPhysicalDeviceProcAddr;
    return VK_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)hinst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        InitializeCriticalSection(&g_lock);
        QueryPerformanceFrequency(&g_qpc_freq);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_udp != INVALID_SOCKET) closesocket(g_udp);
        WSACleanup();
        DeleteCriticalSection(&g_lock);
    }
    return TRUE;
}
