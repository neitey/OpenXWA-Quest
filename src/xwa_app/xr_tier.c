#include "xr_tier.h"

#include "aeron/log.h"

#include <SDL3/SDL_stdinc.h>

#include <stdlib.h>
#include <string.h>

#if defined(__unix__)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <unistd.h>
#define XWA_XR_TIER_PROBE 1
#endif

XwaXrTier XwaXrTier_Classify(const char* gpu_name, int ram_mb) {
	XwaXrTier tier = XWA_XR_TIER_UNKNOWN;
	if (gpu_name && gpu_name[0]) {
		/* "Adreno (TM) 650" (Android), "Turnip Adreno (TM) 750" (Mesa). */
		const char* adreno = strstr(gpu_name, "Adreno");
		if (adreno) {
			const char* p = adreno + 6;
			while (*p && (*p < '0' || *p > '9')) {
				p++;
			}
			if (*p) {
				const long model = strtol(p, NULL, 10);
				if (model >= 750) {
					tier = XWA_XR_TIER_HIGH;
				} else if (model >= 700) {
					tier = XWA_XR_TIER_MID;
				} else if (model > 0) {
					tier = XWA_XR_TIER_LOW;
				}
			}
		}
	}
	/* Quest 2 class memory: the budget follows the RAM too. */
	if (ram_mb > 0 && ram_mb < 7000) {
		tier = XWA_XR_TIER_LOW;
	}
	return tier;
}

const char* XwaXrTier_Name(XwaXrTier tier) {
	switch (tier) {
	case XWA_XR_TIER_LOW:
		return "low";
	case XWA_XR_TIER_MID:
		return "mid";
	case XWA_XR_TIER_HIGH:
		return "high";
	default:
		return "unknown";
	}
}

long XwaXrTier_EyePixels(XwaXrTier tier) {
	/* Quest 3 at its default 1.1 scale (1848 x 1936, 3.6 MP) is the measured
	 * reference; the others follow their GPU's power relative to it. */
	switch (tier) {
	case XWA_XR_TIER_LOW:
		/* Adreno 650, about 60 % of Quest 3's GPU: Quest 2's recommended
		 * size (1440 x 1584); a Pico 4 renders about 1520 x 1520. */
		return 2300000;
	case XWA_XR_TIER_HIGH:
		/* Adreno 750 (Steam Frame, 2160 x 2160 panels): room for rendering
		 * above the panel size (SteamVR recommends 1728 x 1728 at 72 Hz; 150 %
		 * of that is 2592 x 2592). */
		return 6800000;
	default:
		/* Quest 3 stays as it is; Pico 4 Ultra (1920 x 1920 recommended)
		 * renders about 1920 x 1920 instead of 2110 x 2110. */
		return 3700000;
	}
}

#if XWA_XR_TIER_PROBE
/* The Vulkan device's name, from a throwaway instance on the system loader
 * (not the foveation shim). */
static int probe_gpu(char* name, size_t size) {
	static const char* const libs[] = { "libvulkan.so", "libvulkan.so.1" };
	void* lib = NULL;
	PFN_vkGetInstanceProcAddr gipa = NULL;
	PFN_vkCreateInstance create = NULL;
	VkInstance instance = VK_NULL_HANDLE;
	VkApplicationInfo app;
	VkInstanceCreateInfo info;
	void* symbol;
	int found = 0;

	name[0] = '\0';
	memset(&app, 0, sizeof app);
	memset(&info, 0, sizeof info);
	app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	for (size_t i = 0; i < SDL_arraysize(libs) && !lib; ++i) {
		lib = dlopen(libs[i], RTLD_NOW | RTLD_LOCAL);
	}
	if (!lib) {
		return 0;
	}
	symbol = dlsym(lib, "vkGetInstanceProcAddr");
	SDL_memcpy(&gipa, &symbol, sizeof gipa);
	create = gipa ? (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance") : NULL;
	app.pApplicationName = "OpenXWA probe";
	app.apiVersion = VK_API_VERSION_1_0;
	info.pApplicationInfo = &app;
	if (create && create(&info, NULL, &instance) == VK_SUCCESS) {
		PFN_vkEnumeratePhysicalDevices enumerate =
			(PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
		PFN_vkGetPhysicalDeviceProperties properties =
			(PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
		PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance");
		VkPhysicalDevice devices[4];
		uint32_t count = SDL_arraysize(devices);
		const VkResult listed = enumerate && properties ? enumerate(instance, &count, devices) : VK_ERROR_UNKNOWN;
		if ((listed == VK_SUCCESS || listed == VK_INCOMPLETE) && count > 0) {
			VkPhysicalDeviceProperties props;
			memset(&props, 0, sizeof props);
			properties(devices[0], &props);
			SDL_strlcpy(name, props.deviceName, size);
			found = name[0] != '\0';
		}
		if (destroy) {
			destroy(instance, NULL);
		}
	}
	/* The library stays loaded: the renderer opens it again right after, and
	 * some drivers do not like being unloaded. */
	return found;
}

static int ram_megabytes(void) {
	const long pages = sysconf(_SC_PHYS_PAGES);
	const long page = sysconf(_SC_PAGESIZE);
	if (pages <= 0 || page <= 0) {
		return 0;
	}
	return (int)((long long)pages * page / (1024 * 1024));
}
#else
static int probe_gpu(char* name, size_t size) {
	(void)size;
	name[0] = '\0';
	return 0;
}

static int ram_megabytes(void) { return 0; }
#endif

static int set_default(const char* key, const char* value) {
	if (SDL_getenv(key)) {
		return 0; /* the player's (or an earlier default's) */
	}
	SDL_setenv_unsafe(key, value, 1);
	return 1;
}

XwaXrTier XwaXrTier_ApplyDefaults(void) {
	char gpu[256];
	const int ram_mb = ram_megabytes();
	const char* forced = SDL_getenv("XWA_XR_TIER");
	XwaXrTier tier;

	probe_gpu(gpu, sizeof gpu);
	tier = XwaXrTier_Classify(gpu, ram_mb);
	if (forced && forced[0]) {
		if (SDL_strcasecmp(forced, "low") == 0) {
			tier = XWA_XR_TIER_LOW;
		} else if (SDL_strcasecmp(forced, "mid") == 0) {
			tier = XWA_XR_TIER_MID;
		} else if (SDL_strcasecmp(forced, "high") == 0) {
			tier = XWA_XR_TIER_HIGH;
		}
	}
	{
		char pixels[32];
		SDL_snprintf(pixels, sizeof pixels, "%ld", XwaXrTier_EyePixels(tier));
		set_default("AERON_XR_EYE_PIXELS", pixels);
	}
	if (tier == XWA_XR_TIER_LOW) {
		/* The most time per frame, and coarser shading at the view's edges
		 * (where the XR2 Gen 1 headsets' lenses blur anyway). */
		set_default("AERON_XR_REFRESH_RATE", "72");
		set_default("XWA_XR_FOVEATION", "medium");
	}
	Aeron_LogInfo("xwa.config", "device tier %s%s (GPU '%s', RAM %d MB): eye budget %s pixels, refresh %s Hz, "
								"foveation %s",
				  XwaXrTier_Name(tier), forced && forced[0] ? " (XWA_XR_TIER)" : "", gpu[0] ? gpu : "?", ram_mb,
				  SDL_getenv("AERON_XR_EYE_PIXELS"),
				  SDL_getenv("AERON_XR_REFRESH_RATE") ? SDL_getenv("AERON_XR_REFRESH_RATE") : "headset default",
				  SDL_getenv("XWA_XR_FOVEATION") ? SDL_getenv("XWA_XR_FOVEATION") : "low");
	return tier;
}
