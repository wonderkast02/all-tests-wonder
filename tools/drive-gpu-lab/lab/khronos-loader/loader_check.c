/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static void utf8_path(const char *key, const wchar_t *value)
{
    char buf[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, value, -1, buf, (int)sizeof(buf),
                                NULL, NULL);
    if (n <= 0) snprintf(buf, sizeof(buf), "<conversion-failed>");
    printf("%s=%s\n", key, buf);
}

static int join_path(wchar_t *dst, size_t cap, const wchar_t *dir,
                     const wchar_t *leaf)
{
    int n = _snwprintf_s(dst, cap, _TRUNCATE, L"%ls\\%ls", dir, leaf);
    return n >= 0;
}

static void module_path(const char *key, HMODULE module)
{
    wchar_t path[4096];
    DWORD n;
    if (!module) {
        printf("%s=<not-loaded>\n", key);
        return;
    }
    n = GetModuleFileNameW(module, path, (DWORD)ARRAYSIZE(path));
    if (!n || n >= ARRAYSIZE(path)) {
        printf("%s=<GetModuleFileNameW-failed:%lu>\n", key,
               (unsigned long)GetLastError());
        return;
    }
    utf8_path(key, path);
}

static int finish(int pass, VkInstance instance, VkDevice device,
                  PFN_vkDestroyInstance destroy_instance,
                  PFN_vkDestroyDevice destroy_device)
{
    if (device && destroy_device) destroy_device(device, NULL);
    if (instance && destroy_instance) destroy_instance(instance, NULL);

    printf("RESULT=%s\n", pass ? "PASS" : "FAIL");
    fflush(stdout);
    fflush(stderr);

    MessageBoxW(NULL,
                pass
                    ? L"Khronos Loader Lab: PASS\n\nVeja loader-check-result.txt."
                    : L"Khronos Loader Lab: FAIL\n\nVeja loader-check-result.txt.",
                L"Drive GPU Lab", pass ? MB_OK | MB_ICONINFORMATION
                                      : MB_OK | MB_ICONERROR);
    return pass ? 0 : 1;
}

int wmain(void)
{
    wchar_t exe_path[4096], dir[4096], loader_path[4096];
    wchar_t layer_manifest[4096], icd_manifest[4096];
    DWORD n;
    wchar_t *slash;
    HMODULE loader = NULL, layer_module = NULL, winevulkan_module = NULL;
    PFN_vkEnumerateInstanceVersion p_enum_version = NULL;
    PFN_vkEnumerateInstanceLayerProperties p_enum_layers = NULL;
    PFN_vkCreateInstance p_create_instance = NULL;
    PFN_vkGetInstanceProcAddr p_gipa = NULL;
    PFN_vkGetDeviceProcAddr p_gdpa = NULL;
    PFN_vkDestroyInstance p_destroy_instance = NULL;
    PFN_vkEnumeratePhysicalDevices p_enum_phys = NULL;
    PFN_vkGetPhysicalDeviceProperties p_get_props = NULL;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties p_get_qprops = NULL;
    PFN_vkCreateDevice p_create_device = NULL;
    PFN_vkDestroyDevice p_destroy_device = NULL;
    VkLayerProperties *layers = NULL;
    VkPhysicalDevice *phys = NULL;
    VkQueueFamilyProperties *qprops = NULL;
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkResult vr = VK_ERROR_INITIALIZATION_FAILED;
    uint32_t loader_version = VK_API_VERSION_1_0;
    uint32_t layer_count = 0, phys_count = 0, qcount = 0, qindex = UINT32_MAX;
    uint32_t i;
    int layer_found = 0, pass = 0;
    const char *layer_name = "VK_LAYER_DRIVE_G720_telemetry";

    n = GetModuleFileNameW(NULL, exe_path, (DWORD)ARRAYSIZE(exe_path));
    if (!n || n >= ARRAYSIZE(exe_path)) return 2;
    wcsncpy_s(dir, ARRAYSIZE(dir), exe_path, _TRUNCATE);
    slash = wcsrchr(dir, L'\\');
    if (!slash) slash = wcsrchr(dir, L'/');
    if (!slash) return 2;
    *slash = L'\0';

    if (!SetCurrentDirectoryW(dir)) return 2;
    {
        FILE *redirected = NULL;
        if (freopen_s(&redirected, "loader-check-result.txt", "w", stdout) != 0)
            return 2;
        redirected = NULL;
        if (freopen_s(&redirected, "loader-check-result.txt", "a", stderr) != 0)
            return 2;
    }

    printf("DRIVE_GPU_LAB_KHRONOS_LOADER_CHECK=1\n");
    utf8_path("LAB_DIR", dir);

    if (!join_path(loader_path, ARRAYSIZE(loader_path), dir, L"vulkan-1.dll") ||
        !join_path(layer_manifest, ARRAYSIZE(layer_manifest), dir,
                   L"VK_LAYER_DRIVE_G720_telemetry.json") ||
        !join_path(icd_manifest, ARRAYSIZE(icd_manifest), dir,
                   L"winevulkan-icd.json")) {
        printf("PATH_BUILD=FAIL\n");
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);
    }

    utf8_path("KHRONOS_LOADER_PATH", loader_path);
    utf8_path("LAYER_MANIFEST", layer_manifest);
    utf8_path("ICD_MANIFEST", icd_manifest);

    printf("SYSTEM32_WINEVULKAN_FILE=%s\n",
           GetFileAttributesW(L"C:\\windows\\system32\\winevulkan.dll") !=
                   INVALID_FILE_ATTRIBUTES
               ? "PRESENT"
               : "NOT_VISIBLE_AS_FILE");

    if (!SetEnvironmentVariableW(L"VK_LAYER_PATH", dir) ||
        !SetEnvironmentVariableW(L"VK_INSTANCE_LAYERS",
                                 L"VK_LAYER_DRIVE_G720_telemetry") ||
        !SetEnvironmentVariableW(L"VK_DRIVER_FILES", icd_manifest) ||
        !SetEnvironmentVariableW(L"VK_ICD_FILENAMES", icd_manifest) ||
        !SetEnvironmentVariableW(L"VK_LOADER_DEBUG", L"all")) {
        printf("ENV_SETUP=FAIL error=%lu\n", (unsigned long)GetLastError());
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);
    }
    printf("ENV_SETUP=PASS\n");

    loader = LoadLibraryW(loader_path);
    if (!loader) {
        printf("LOAD_KHRONOS_LOADER=FAIL error=%lu\n",
               (unsigned long)GetLastError());
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);
    }
    printf("LOAD_KHRONOS_LOADER=PASS\n");
    module_path("KHRONOS_LOADER_MODULE", loader);

    p_enum_version = (PFN_vkEnumerateInstanceVersion)GetProcAddress(
        loader, "vkEnumerateInstanceVersion");
    p_enum_layers = (PFN_vkEnumerateInstanceLayerProperties)GetProcAddress(
        loader, "vkEnumerateInstanceLayerProperties");
    p_create_instance = (PFN_vkCreateInstance)GetProcAddress(
        loader, "vkCreateInstance");
    p_gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(
        loader, "vkGetInstanceProcAddr");
    p_gdpa = (PFN_vkGetDeviceProcAddr)GetProcAddress(
        loader, "vkGetDeviceProcAddr");

    if (!p_enum_layers || !p_create_instance || !p_gipa || !p_gdpa) {
        printf("LOADER_EXPORTS=FAIL enum_layers=%d create_instance=%d gipa=%d gdpa=%d\n",
               p_enum_layers != NULL, p_create_instance != NULL, p_gipa != NULL,
               p_gdpa != NULL);
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);
    }
    printf("LOADER_EXPORTS=PASS\n");

    if (p_enum_version) {
        vr = p_enum_version(&loader_version);
        printf("vkEnumerateInstanceVersion=%d api=%u.%u.%u\n", (int)vr,
               VK_API_VERSION_MAJOR(loader_version),
               VK_API_VERSION_MINOR(loader_version),
               VK_API_VERSION_PATCH(loader_version));
    } else {
        printf("vkEnumerateInstanceVersion=<not-exported>; assuming 1.0\n");
    }

    vr = p_enum_layers(&layer_count, NULL);
    printf("LAYER_COUNT_QUERY_RESULT=%d\nLAYER_COUNT=%u\n", (int)vr,
           layer_count);
    if (vr != VK_SUCCESS || layer_count == 0) {
        printf("LAYER_ENUMERATION=FAIL\n");
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);
    }

    layers = (VkLayerProperties *)calloc(layer_count, sizeof(*layers));
    if (!layers) {
        printf("ALLOC_LAYERS=FAIL\n");
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);
    }
    vr = p_enum_layers(&layer_count, layers);
    printf("LAYER_ENUM_RESULT=%d\n", (int)vr);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE) {
        free(layers);
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);
    }
    for (i = 0; i < layer_count; ++i) {
        printf("LAYER[%u]=%s spec=%u impl=%u\n", i, layers[i].layerName,
               layers[i].specVersion, layers[i].implementationVersion);
        if (strcmp(layers[i].layerName, layer_name) == 0) layer_found = 1;
    }
    free(layers);
    layers = NULL;
    printf("TARGET_LAYER_FOUND=%s\n", layer_found ? "YES" : "NO");
    if (!layer_found)
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);

    {
        VkApplicationInfo app = {0};
        VkInstanceCreateInfo ci = {0};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "G720VkLoaderCheck";
        app.applicationVersion = 1;
        app.pEngineName = "Drive GPU Lab";
        app.engineVersion = 1;
        app.apiVersion = VK_API_VERSION_1_0;

        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer_name;

        vr = p_create_instance(&ci, NULL, &instance);
        printf("vkCreateInstance=%d\n", (int)vr);
        if (vr != VK_SUCCESS || !instance)
            return finish(0, instance, device, p_destroy_instance,
                          p_destroy_device);
    }

    layer_module = GetModuleHandleW(L"G720VkLayer.dll");
    winevulkan_module = GetModuleHandleW(L"winevulkan.dll");
    module_path("G720_LAYER_MODULE", layer_module);
    module_path("WINEVULKAN_MODULE", winevulkan_module);
    printf("G720_LAYER_LOADED=%s\n", layer_module ? "YES" : "NO");
    printf("WINEVULKAN_LOADED=%s\n", winevulkan_module ? "YES" : "NO");

    p_destroy_instance = (PFN_vkDestroyInstance)p_gipa(
        instance, "vkDestroyInstance");
    p_enum_phys = (PFN_vkEnumeratePhysicalDevices)p_gipa(
        instance, "vkEnumeratePhysicalDevices");
    p_get_props = (PFN_vkGetPhysicalDeviceProperties)p_gipa(
        instance, "vkGetPhysicalDeviceProperties");
    p_get_qprops = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)p_gipa(
        instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    p_create_device = (PFN_vkCreateDevice)p_gipa(
        instance, "vkCreateDevice");

    if (!p_destroy_instance || !p_enum_phys || !p_get_props || !p_get_qprops ||
        !p_create_device) {
        printf("INSTANCE_FUNCTIONS=FAIL\n");
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);
    }

    vr = p_enum_phys(instance, &phys_count, NULL);
    printf("PHYSICAL_DEVICE_COUNT_QUERY=%d count=%u\n", (int)vr, phys_count);
    if (vr != VK_SUCCESS || phys_count == 0)
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);

    phys = (VkPhysicalDevice *)calloc(phys_count, sizeof(*phys));
    if (!phys)
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);
    vr = p_enum_phys(instance, &phys_count, phys);
    printf("PHYSICAL_DEVICE_ENUM=%d count=%u\n", (int)vr, phys_count);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE) {
        free(phys);
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);
    }

    for (i = 0; i < phys_count; ++i) {
        VkPhysicalDeviceProperties props;
        memset(&props, 0, sizeof(props));
        p_get_props(phys[i], &props);
        printf("PHYSICAL_DEVICE[%u]=%s api=%u.%u.%u driver=%u vendor=0x%04x device=0x%04x\n",
               i, props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
               VK_API_VERSION_MINOR(props.apiVersion),
               VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion,
               props.vendorID, props.deviceID);
    }

    p_get_qprops(phys[0], &qcount, NULL);
    printf("QUEUE_FAMILY_COUNT=%u\n", qcount);
    if (!qcount) {
        free(phys);
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);
    }
    qprops = (VkQueueFamilyProperties *)calloc(qcount, sizeof(*qprops));
    if (!qprops) {
        free(phys);
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);
    }
    p_get_qprops(phys[0], &qcount, qprops);
    for (i = 0; i < qcount; ++i) {
        printf("QUEUE[%u]=count:%u flags:0x%x\n", i, qprops[i].queueCount,
               qprops[i].queueFlags);
        if (qprops[i].queueCount > 0 &&
            (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            qindex = i;
            break;
        }
        if (qindex == UINT32_MAX && qprops[i].queueCount > 0) qindex = i;
    }
    free(qprops);
    qprops = NULL;

    if (qindex == UINT32_MAX) {
        free(phys);
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);
    }

    {
        float priority = 1.0f;
        VkDeviceQueueCreateInfo qci = {0};
        VkDeviceCreateInfo dci = {0};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = qindex;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;

        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;

        vr = p_create_device(phys[0], &dci, NULL, &device);
        printf("vkCreateDevice=%d queue_family=%u\n", (int)vr, qindex);
    }
    free(phys);
    phys = NULL;

    if (vr != VK_SUCCESS || !device)
        return finish(0, instance, device, p_destroy_instance, p_destroy_device);

    p_destroy_device = (PFN_vkDestroyDevice)p_gdpa(
        device, "vkDestroyDevice");
    printf("DESTROY_DEVICE_RESOLVED=%s\n",
           p_destroy_device ? "YES" : "NO");

    pass = layer_module != NULL && winevulkan_module != NULL &&
           p_destroy_device != NULL;
    return finish(pass, instance, device, p_destroy_instance, p_destroy_device);
}
