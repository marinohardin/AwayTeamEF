/*
===========================================================================
AwayTeamEF VR: OpenXR session lifecycle for the Vulkan renderer (EF_VR only).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

- The OpenXR loader is opened at runtime (dlopen / LoadLibrary), so the VR build links on
  machines without one and falls back to flat when it's missing. On the Quest the loader
  comes from the APK (libopenxr_loader.so); EF_OPENXR_LOADER overrides the path.
- Graphics: XR_KHR_vulkan_enable2. The runtime creates the VkInstance and VkDevice
  (VR_CreateVulkanInstance / VR_CreateVulkanDevice) and names the physical device.
- Views: primary stereo, one swapchain per eye.
- Space: STAGE when the runtime has it, else LOCAL.
- Frames: xrWaitFrame + xrBeginFrame in VR_BeginFrame, one projection layer in VR_EndFrame.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#ifdef __ANDROID__
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#endif

#include "vr.h"

#define XR_USE_GRAPHICS_API_VULKAN
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define VR_MAX_SWAPCHAIN_IMAGES 8

// ---------------------------------------------------------------------------
// printing
// ---------------------------------------------------------------------------

static vrPrintFunc_t vr_print;

void VR_SetPrint( vrPrintFunc_t print ) {
	vr_print = print;
}

static void VR_Log( int isError, const char *fmt, ... ) {
	char msg[1024];
	va_list ap;

	va_start( ap, fmt );
	vsnprintf( msg, sizeof( msg ), fmt, ap );
	va_end( ap );

	if ( vr_print ) {
		vr_print( isError, msg );
	} else {
		fprintf( isError ? stderr : stdout, "%s", msg );
	}
}

// ---------------------------------------------------------------------------
// loader and entry points
// ---------------------------------------------------------------------------

static void *vr_loaderLib;
static PFN_xrGetInstanceProcAddr qxrGetInstanceProcAddr;

#define XR_FUNCS_GLOBAL \
	XR_FN( xrEnumerateInstanceExtensionProperties ) \
	XR_FN( xrCreateInstance )

#define XR_FUNCS_INSTANCE \
	XR_FN( xrDestroyInstance ) \
	XR_FN( xrResultToString ) \
	XR_FN( xrGetInstanceProperties ) \
	XR_FN( xrGetSystem ) \
	XR_FN( xrGetSystemProperties ) \
	XR_FN( xrEnumerateViewConfigurations ) \
	XR_FN( xrEnumerateViewConfigurationViews ) \
	XR_FN( xrEnumerateReferenceSpaces ) \
	XR_FN( xrCreateReferenceSpace ) \
	XR_FN( xrDestroySpace ) \
	XR_FN( xrLocateSpace ) \
	XR_FN( xrCreateSession ) \
	XR_FN( xrDestroySession ) \
	XR_FN( xrBeginSession ) \
	XR_FN( xrEndSession ) \
	XR_FN( xrRequestExitSession ) \
	XR_FN( xrPollEvent ) \
	XR_FN( xrEnumerateSwapchainFormats ) \
	XR_FN( xrCreateSwapchain ) \
	XR_FN( xrDestroySwapchain ) \
	XR_FN( xrEnumerateSwapchainImages ) \
	XR_FN( xrAcquireSwapchainImage ) \
	XR_FN( xrWaitSwapchainImage ) \
	XR_FN( xrReleaseSwapchainImage ) \
	XR_FN( xrWaitFrame ) \
	XR_FN( xrBeginFrame ) \
	XR_FN( xrEndFrame ) \
	XR_FN( xrLocateViews ) \
	XR_FN( xrGetVulkanGraphicsRequirements2KHR ) \
	XR_FN( xrCreateVulkanInstanceKHR ) \
	XR_FN( xrGetVulkanGraphicsDevice2KHR ) \
	XR_FN( xrCreateVulkanDeviceKHR )

#define XR_FN( name ) static PFN_##name q##name;
XR_FUNCS_GLOBAL
XR_FUNCS_INSTANCE
#undef XR_FN

#ifdef __ANDROID__
static PFN_xrInitializeLoaderKHR qxrInitializeLoaderKHR;
static void *vr_javaVM;
static void *vr_activity;

void VR_SetAndroidContext( void *javaVM, void *activity ) {
	vr_javaVM = javaVM;
	vr_activity = activity;
}
#endif

static void *VR_OpenLibrary( const char *name ) {
#ifdef _WIN32
	return (void *)LoadLibraryA( name );
#else
	return dlopen( name, RTLD_NOW | RTLD_LOCAL );
#endif
}

static void *VR_LibrarySymbol( void *lib, const char *name ) {
#ifdef _WIN32
	return (void *)GetProcAddress( (HMODULE)lib, name );
#else
	return dlsym( lib, name );
#endif
}

static void VR_CloseLibrary( void *lib ) {
#ifdef _WIN32
	FreeLibrary( (HMODULE)lib );
#else
	dlclose( lib );
#endif
}

static int VR_LoadLoader( void ) {
	static const char *names[] = {
#if defined( _WIN32 )
		"openxr_loader.dll",
#elif defined( __APPLE__ )
		"libopenxr_loader.dylib", "libopenxr_loader.1.dylib",
#elif defined( __ANDROID__ )
		"libopenxr_loader.so",
#else
		"libopenxr_loader.so.1", "libopenxr_loader.so",
#endif
		NULL
	};
	const char *custom = getenv( "EF_OPENXR_LOADER" );
	int i;

	if ( qxrGetInstanceProcAddr ) {
		return 1;
	}

	if ( custom && *custom ) {
		vr_loaderLib = VR_OpenLibrary( custom );
	}
	for ( i = 0; !vr_loaderLib && names[i]; i++ ) {
		vr_loaderLib = VR_OpenLibrary( names[i] );
	}
	if ( !vr_loaderLib ) {
		VR_Log( 0, "VR: no OpenXR loader found, staying flat\n" );
		return 0;
	}

	qxrGetInstanceProcAddr = (PFN_xrGetInstanceProcAddr)VR_LibrarySymbol( vr_loaderLib, "xrGetInstanceProcAddr" );
	if ( !qxrGetInstanceProcAddr ) {
		VR_Log( 1, "VR: the OpenXR loader has no xrGetInstanceProcAddr\n" );
		VR_CloseLibrary( vr_loaderLib );
		vr_loaderLib = NULL;
		return 0;
	}
	return 1;
}

static int VR_LoadFunctions( XrInstance instance ) {
	int ok = 1;
#define XR_FN( name ) \
	if ( XR_FAILED( qxrGetInstanceProcAddr( instance, #name, (PFN_xrVoidFunction *)&q##name ) ) || !q##name ) { \
		VR_Log( 1, "VR: missing OpenXR entry point %s\n", #name ); \
		ok = 0; \
	}
	if ( instance == XR_NULL_HANDLE ) {
		XR_FUNCS_GLOBAL
	} else {
		XR_FUNCS_INSTANCE
	}
#undef XR_FN
	return ok;
}

static void VR_ClearFunctions( void ) {
#define XR_FN( name ) q##name = NULL;
	XR_FUNCS_INSTANCE
#undef XR_FN
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------

typedef struct {
	XrSwapchain			handle;
	uint32_t			width, height;
	int64_t				format;
	uint32_t			imageCount;
	XrSwapchainImageVulkan2KHR images[VR_MAX_SWAPCHAIN_IMAGES];
	uint32_t			acquiredIndex;
	int					acquired;
} vrSwapchain_t;

static struct {
	XrInstance			instance;
	XrSystemId			systemId;
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
	XrGraphicsRequirementsVulkan2KHR graphicsReqs;

	XrSession			session;
	XrSessionState		sessionState;
	int					sessionRunning;
	XrSpace				space;
	XrReferenceSpaceType spaceType;

	uint32_t			viewCount;
	XrViewConfigurationView configViews[VR_MAX_EYES];
	vrSwapchain_t		swapchains[VR_MAX_EYES];

	// current frame
	int					frameBegun;		// xrBeginFrame called, xrEndFrame due
	int					frameActive;	// views located and images acquired: render stereo
	XrFrameState		frameState;
	XrView				views[VR_MAX_EYES];
	XrViewState			viewState;

	// head origin for VR_GetEyeOffset
	int					recenterPending;
	float				headOrigin[3];
} vr;

static const char *VR_ResultString( XrResult res ) {
	static char buf[XR_MAX_RESULT_STRING_SIZE];

	if ( qxrResultToString && vr.instance != XR_NULL_HANDLE &&
		XR_SUCCEEDED( qxrResultToString( vr.instance, res, buf ) ) ) {
		return buf;
	}
	snprintf( buf, sizeof( buf ), "XrResult %d", (int)res );
	return buf;
}

#define VR_CHECK( call, what ) \
	do { \
		XrResult _r = ( call ); \
		if ( XR_FAILED( _r ) ) { \
			VR_Log( 1, "VR: %s failed: %s\n", what, VR_ResultString( _r ) ); \
			return 0; \
		} \
	} while ( 0 )

int VR_Initialized( void ) {
	return vr.instance != XR_NULL_HANDLE && vr.systemId != XR_NULL_SYSTEM_ID;
}

// ---------------------------------------------------------------------------
// instance and system
// ---------------------------------------------------------------------------

static int VR_HasExtension( const XrExtensionProperties *props, uint32_t count, const char *name ) {
	uint32_t i;
	for ( i = 0; i < count; i++ ) {
		if ( strcmp( props[i].extensionName, name ) == 0 ) {
			return 1;
		}
	}
	return 0;
}

int VR_Init( PFN_vkGetInstanceProcAddr getInstanceProcAddr ) {
	XrExtensionProperties *exts;
	uint32_t extCount = 0, i;
	const char *enabled[4];
	uint32_t enabledCount = 0;
	XrInstanceCreateInfo ici;
	XrSystemGetInfo sgi;
	XrInstanceProperties iprops;
	XrSystemProperties sprops;
	XrResult res;
#ifdef __ANDROID__
	XrInstanceCreateInfoAndroidKHR androidInfo;
#endif

	if ( VR_Initialized() ) {
		return 1;
	}

	vr.vkGetInstanceProcAddr = getInstanceProcAddr;

	if ( !VR_LoadLoader() ) {
		return 0;
	}

#ifdef __ANDROID__
	if ( XR_SUCCEEDED( qxrGetInstanceProcAddr( XR_NULL_HANDLE, "xrInitializeLoaderKHR",
			(PFN_xrVoidFunction *)&qxrInitializeLoaderKHR ) ) && qxrInitializeLoaderKHR ) {
		XrLoaderInitInfoAndroidKHR li;

		if ( !vr_javaVM || !vr_activity ) {
			VR_Log( 1, "VR: VR_SetAndroidContext was not called\n" );
			return 0;
		}
		memset( &li, 0, sizeof( li ) );
		li.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
		li.applicationVM = vr_javaVM;
		li.applicationContext = vr_activity;
		VR_CHECK( qxrInitializeLoaderKHR( (const XrLoaderInitInfoBaseHeaderKHR *)&li ), "xrInitializeLoaderKHR" );
	}
#endif

	if ( !VR_LoadFunctions( XR_NULL_HANDLE ) ) {
		return 0;
	}

	res = qxrEnumerateInstanceExtensionProperties( NULL, 0, &extCount, NULL );
	if ( XR_FAILED( res ) || extCount == 0 ) {
		VR_Log( 0, "VR: no OpenXR runtime (%d), staying flat\n", (int)res );
		return 0;
	}
	exts = (XrExtensionProperties *)calloc( extCount, sizeof( *exts ) );
	for ( i = 0; i < extCount; i++ ) {
		exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	}
	res = qxrEnumerateInstanceExtensionProperties( NULL, extCount, &extCount, exts );
	if ( XR_FAILED( res ) || !VR_HasExtension( exts, extCount, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME ) ) {
		VR_Log( 0, "VR: the OpenXR runtime has no %s, staying flat\n", XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME );
		free( exts );
		return 0;
	}
	enabled[enabledCount++] = XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
#ifdef __ANDROID__
	if ( VR_HasExtension( exts, extCount, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME ) ) {
		enabled[enabledCount++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
	}
#endif
	free( exts );

	memset( &ici, 0, sizeof( ici ) );
	ici.type = XR_TYPE_INSTANCE_CREATE_INFO;
	strncpy( ici.applicationInfo.applicationName, "AwayTeamEF", XR_MAX_APPLICATION_NAME_SIZE - 1 );
	ici.applicationInfo.applicationVersion = 1;
	strncpy( ici.applicationInfo.engineName, "AwayTeamEF", XR_MAX_ENGINE_NAME_SIZE - 1 );
	ici.applicationInfo.engineVersion = 1;
	// 1.0 keeps older runtimes happy; everything used here is core 1.0 or KHR
	ici.applicationInfo.apiVersion = XR_MAKE_VERSION( 1, 0, 34 );
	ici.enabledExtensionCount = enabledCount;
	ici.enabledExtensionNames = enabled;
#ifdef __ANDROID__
	memset( &androidInfo, 0, sizeof( androidInfo ) );
	androidInfo.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	androidInfo.applicationVM = vr_javaVM;
	androidInfo.applicationActivity = vr_activity;
	ici.next = &androidInfo;
#endif

	res = qxrCreateInstance( &ici, &vr.instance );
	if ( XR_FAILED( res ) ) {
		VR_Log( 0, "VR: xrCreateInstance failed (%d), staying flat\n", (int)res );
		vr.instance = XR_NULL_HANDLE;
		return 0;
	}

	if ( !VR_LoadFunctions( vr.instance ) ) {
		VR_Shutdown();
		return 0;
	}

	memset( &iprops, 0, sizeof( iprops ) );
	iprops.type = XR_TYPE_INSTANCE_PROPERTIES;
	if ( XR_SUCCEEDED( qxrGetInstanceProperties( vr.instance, &iprops ) ) ) {
		VR_Log( 0, "VR: OpenXR runtime %s %u.%u.%u\n", iprops.runtimeName,
			XR_VERSION_MAJOR( iprops.runtimeVersion ), XR_VERSION_MINOR( iprops.runtimeVersion ),
			XR_VERSION_PATCH( iprops.runtimeVersion ) );
	}

	memset( &sgi, 0, sizeof( sgi ) );
	sgi.type = XR_TYPE_SYSTEM_GET_INFO;
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	res = qxrGetSystem( vr.instance, &sgi, &vr.systemId );
	if ( XR_FAILED( res ) ) {
		VR_Log( 0, "VR: no headset (%s), staying flat\n", VR_ResultString( res ) );
		VR_Shutdown();
		return 0;
	}

	memset( &sprops, 0, sizeof( sprops ) );
	sprops.type = XR_TYPE_SYSTEM_PROPERTIES;
	if ( XR_SUCCEEDED( qxrGetSystemProperties( vr.instance, vr.systemId, &sprops ) ) ) {
		VR_Log( 0, "VR: system %s, max swapchain %ux%u\n", sprops.systemName,
			sprops.graphicsProperties.maxSwapchainImageWidth, sprops.graphicsProperties.maxSwapchainImageHeight );
	}

	// stereo view configuration, and the recommended eye-buffer size
	vr.viewCount = 0;
	res = qxrEnumerateViewConfigurationViews( vr.instance, vr.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
		0, &vr.viewCount, NULL );
	if ( XR_FAILED( res ) || vr.viewCount != VR_MAX_EYES ) {
		VR_Log( 1, "VR: no primary stereo view configuration (%u views)\n", vr.viewCount );
		VR_Shutdown();
		return 0;
	}
	for ( i = 0; i < VR_MAX_EYES; i++ ) {
		memset( &vr.configViews[i], 0, sizeof( vr.configViews[i] ) );
		vr.configViews[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	}
	res = qxrEnumerateViewConfigurationViews( vr.instance, vr.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
		VR_MAX_EYES, &vr.viewCount, vr.configViews );
	if ( XR_FAILED( res ) ) {
		VR_Log( 1, "VR: xrEnumerateViewConfigurationViews failed: %s\n", VR_ResultString( res ) );
		VR_Shutdown();
		return 0;
	}
	VR_Log( 0, "VR: eye buffer %ux%u recommended\n",
		vr.configViews[0].recommendedImageRectWidth, vr.configViews[0].recommendedImageRectHeight );

	// required before xrCreateVulkanInstanceKHR
	memset( &vr.graphicsReqs, 0, sizeof( vr.graphicsReqs ) );
	vr.graphicsReqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR;
	res = qxrGetVulkanGraphicsRequirements2KHR( vr.instance, vr.systemId, &vr.graphicsReqs );
	if ( XR_FAILED( res ) ) {
		VR_Log( 1, "VR: xrGetVulkanGraphicsRequirements2KHR failed: %s\n", VR_ResultString( res ) );
		VR_Shutdown();
		return 0;
	}
	VR_Log( 0, "VR: Vulkan %u.%u to %u.%u supported by the runtime\n",
		XR_VERSION_MAJOR( vr.graphicsReqs.minApiVersionSupported ), XR_VERSION_MINOR( vr.graphicsReqs.minApiVersionSupported ),
		XR_VERSION_MAJOR( vr.graphicsReqs.maxApiVersionSupported ), XR_VERSION_MINOR( vr.graphicsReqs.maxApiVersionSupported ) );

	vr.recenterPending = 1;
	return 1;
}

void VR_Shutdown( void ) {
	VR_DestroySession();

	if ( vr.instance != XR_NULL_HANDLE && qxrDestroyInstance ) {
		qxrDestroyInstance( vr.instance );
	}
	vr.instance = XR_NULL_HANDLE;
	vr.systemId = XR_NULL_SYSTEM_ID;
	VR_ClearFunctions();
	// the loader stays open: some runtimes don't survive being unloaded and reloaded
}

// ---------------------------------------------------------------------------
// Vulkan hooks
// ---------------------------------------------------------------------------

VkResult VR_CreateVulkanInstance( const VkInstanceCreateInfo *info, VkInstance *instance ) {
	XrVulkanInstanceCreateInfoKHR ci;
	VkInstanceCreateInfo vci = *info;
	VkApplicationInfo app;
	uint32_t minApi;
	VkResult vkres = VK_ERROR_INITIALIZATION_FAILED;
	XrResult res;

	if ( !VR_Initialized() ) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	// raise the Vulkan API version to what the runtime needs (the renderer asks for 1.0)
	minApi = VK_MAKE_VERSION( XR_VERSION_MAJOR( vr.graphicsReqs.minApiVersionSupported ),
		XR_VERSION_MINOR( vr.graphicsReqs.minApiVersionSupported ), 0 );
	if ( minApi < VK_API_VERSION_1_1 ) {
		minApi = VK_API_VERSION_1_1;
	}
	if ( info->pApplicationInfo ) {
		app = *info->pApplicationInfo;
	} else {
		memset( &app, 0, sizeof( app ) );
		app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	}
	if ( app.apiVersion < minApi ) {
		app.apiVersion = minApi;
	}
	vci.pApplicationInfo = &app;

	memset( &ci, 0, sizeof( ci ) );
	ci.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
	ci.systemId = vr.systemId;
	ci.pfnGetInstanceProcAddr = vr.vkGetInstanceProcAddr;
	ci.vulkanCreateInfo = &vci;
	ci.vulkanAllocator = NULL;

	res = qxrCreateVulkanInstanceKHR( vr.instance, &ci, instance, &vkres );
	if ( XR_FAILED( res ) ) {
		VR_Log( 1, "VR: xrCreateVulkanInstanceKHR failed: %s\n", VR_ResultString( res ) );
		return vkres != VK_SUCCESS ? vkres : VK_ERROR_INITIALIZATION_FAILED;
	}
	return vkres;
}

VkPhysicalDevice VR_GetVulkanPhysicalDevice( VkInstance instance ) {
	XrVulkanGraphicsDeviceGetInfoKHR gi;
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	XrResult res;

	if ( !VR_Initialized() ) {
		return VK_NULL_HANDLE;
	}

	memset( &gi, 0, sizeof( gi ) );
	gi.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
	gi.systemId = vr.systemId;
	gi.vulkanInstance = instance;
	res = qxrGetVulkanGraphicsDevice2KHR( vr.instance, &gi, &pd );
	if ( XR_FAILED( res ) ) {
		VR_Log( 1, "VR: xrGetVulkanGraphicsDevice2KHR failed: %s\n", VR_ResultString( res ) );
		return VK_NULL_HANDLE;
	}
	return pd;
}

VkResult VR_CreateVulkanDevice( VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *info, VkDevice *device ) {
	XrVulkanDeviceCreateInfoKHR ci;
	VkResult vkres = VK_ERROR_INITIALIZATION_FAILED;
	XrResult res;

	if ( !VR_Initialized() ) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	memset( &ci, 0, sizeof( ci ) );
	ci.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
	ci.systemId = vr.systemId;
	ci.pfnGetInstanceProcAddr = vr.vkGetInstanceProcAddr;
	ci.vulkanPhysicalDevice = physicalDevice;
	ci.vulkanCreateInfo = info;
	ci.vulkanAllocator = NULL;

	res = qxrCreateVulkanDeviceKHR( vr.instance, &ci, device, &vkres );
	if ( XR_FAILED( res ) ) {
		VR_Log( 1, "VR: xrCreateVulkanDeviceKHR failed: %s\n", VR_ResultString( res ) );
		return vkres != VK_SUCCESS ? vkres : VK_ERROR_INITIALIZATION_FAILED;
	}
	return vkres;
}

// ---------------------------------------------------------------------------
// session, space, swapchains
// ---------------------------------------------------------------------------

// The game's colours are already gamma-encoded, and the renderer copies them into the eye
// images with vkCmdBlitImage, which converts formats. A UNORM eye image keeps the values as
// they are; whether the compositor then treats them as sRGB is runtime-specific and to be
// checked on the headset (docs/vr/RENDERING.md, "Colour space").
static int64_t VR_PickSwapchainFormat( void ) {
	static const int64_t preferred[] = {
		VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
		VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
	};
	int64_t formats[64];
	uint32_t count = 0, i, j;

	if ( XR_FAILED( qxrEnumerateSwapchainFormats( vr.session, 64, &count, formats ) ) || count == 0 ) {
		return VK_FORMAT_UNDEFINED;
	}
	for ( i = 0; i < sizeof( preferred ) / sizeof( preferred[0] ); i++ ) {
		for ( j = 0; j < count; j++ ) {
			if ( formats[j] == preferred[i] ) {
				return formats[j];
			}
		}
	}
	return formats[0];	// the runtime's own favourite
}

static int VR_CreateSwapchains( void ) {
	int64_t format = VR_PickSwapchainFormat();
	uint32_t eye, i;

	if ( format == VK_FORMAT_UNDEFINED ) {
		VR_Log( 1, "VR: no swapchain formats\n" );
		return 0;
	}

	for ( eye = 0; eye < VR_MAX_EYES; eye++ ) {
		vrSwapchain_t *sc = &vr.swapchains[eye];
		XrSwapchainCreateInfo ci;

		memset( &ci, 0, sizeof( ci ) );
		ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
		// the renderer blits into the image today; drawing into it directly comes later
		ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		ci.format = format;
		ci.sampleCount = 1;
		ci.width = vr.configViews[eye].recommendedImageRectWidth;
		ci.height = vr.configViews[eye].recommendedImageRectHeight;
		ci.faceCount = 1;
		ci.arraySize = 1;
		ci.mipCount = 1;
		VR_CHECK( qxrCreateSwapchain( vr.session, &ci, &sc->handle ), "xrCreateSwapchain" );

		sc->width = ci.width;
		sc->height = ci.height;
		sc->format = format;
		sc->acquired = 0;

		sc->imageCount = 0;
		VR_CHECK( qxrEnumerateSwapchainImages( sc->handle, 0, &sc->imageCount, NULL ), "xrEnumerateSwapchainImages" );
		if ( sc->imageCount > VR_MAX_SWAPCHAIN_IMAGES ) {
			VR_Log( 1, "VR: %u swapchain images, more than %d\n", sc->imageCount, VR_MAX_SWAPCHAIN_IMAGES );
			return 0;
		}
		for ( i = 0; i < sc->imageCount; i++ ) {
			memset( &sc->images[i], 0, sizeof( sc->images[i] ) );
			sc->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
		}
		VR_CHECK( qxrEnumerateSwapchainImages( sc->handle, sc->imageCount, &sc->imageCount,
			(XrSwapchainImageBaseHeader *)sc->images ), "xrEnumerateSwapchainImages" );
	}

	VR_Log( 0, "VR: eye swapchains %ux%u, format %d, %u images\n", vr.swapchains[0].width,
		vr.swapchains[0].height, (int)format, vr.swapchains[0].imageCount );
	return 1;
}

static int VR_CreateSpace( void ) {
	XrReferenceSpaceType types[16];
	XrReferenceSpaceCreateInfo ci;
	uint32_t count = 0, i;
	int haveStage = 0;

	if ( XR_SUCCEEDED( qxrEnumerateReferenceSpaces( vr.session, 16, &count, types ) ) ) {
		for ( i = 0; i < count; i++ ) {
			if ( types[i] == XR_REFERENCE_SPACE_TYPE_STAGE ) {
				haveStage = 1;
			}
		}
	}
	vr.spaceType = haveStage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;

	memset( &ci, 0, sizeof( ci ) );
	ci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	ci.referenceSpaceType = vr.spaceType;
	ci.poseInReferenceSpace.orientation.w = 1.0f;
	if ( XR_FAILED( qxrCreateReferenceSpace( vr.session, &ci, &vr.space ) ) && haveStage ) {
		// STAGE listed but not usable yet (no boundary set up): LOCAL always exists
		vr.spaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		ci.referenceSpaceType = vr.spaceType;
		VR_CHECK( qxrCreateReferenceSpace( vr.session, &ci, &vr.space ), "xrCreateReferenceSpace" );
	}
	if ( vr.space == XR_NULL_HANDLE ) {
		VR_Log( 1, "VR: no reference space\n" );
		return 0;
	}
	VR_Log( 0, "VR: reference space %s\n", vr.spaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL" );
	return 1;
}

int VR_CreateSession( VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
	uint32_t queueFamilyIndex, uint32_t queueIndex ) {
	XrGraphicsBindingVulkan2KHR binding;
	XrSessionCreateInfo ci;

	if ( !VR_Initialized() ) {
		return 0;
	}
	if ( vr.session != XR_NULL_HANDLE ) {
		return 1;
	}

	memset( &binding, 0, sizeof( binding ) );
	binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR;
	binding.instance = instance;
	binding.physicalDevice = physicalDevice;
	binding.device = device;
	binding.queueFamilyIndex = queueFamilyIndex;
	binding.queueIndex = queueIndex;

	memset( &ci, 0, sizeof( ci ) );
	ci.type = XR_TYPE_SESSION_CREATE_INFO;
	ci.next = &binding;
	ci.systemId = vr.systemId;

	if ( XR_FAILED( qxrCreateSession( vr.instance, &ci, &vr.session ) ) ) {
		VR_Log( 1, "VR: xrCreateSession failed\n" );
		vr.session = XR_NULL_HANDLE;
		return 0;
	}
	vr.sessionState = XR_SESSION_STATE_UNKNOWN;
	vr.sessionRunning = 0;

	if ( !VR_CreateSpace() || !VR_CreateSwapchains() ) {
		VR_DestroySession();
		return 0;
	}

	VR_Log( 0, "VR: session created, waiting for the runtime to start it\n" );
	return 1;
}

void VR_DestroySession( void ) {
	int eye;

	if ( vr.session == XR_NULL_HANDLE ) {
		return;
	}

	for ( eye = 0; eye < VR_MAX_EYES; eye++ ) {
		vrSwapchain_t *sc = &vr.swapchains[eye];
		if ( sc->handle != XR_NULL_HANDLE ) {
			if ( sc->acquired ) {
				qxrReleaseSwapchainImage( sc->handle, NULL );
			}
			qxrDestroySwapchain( sc->handle );
		}
		memset( sc, 0, sizeof( *sc ) );
	}

	if ( vr.space != XR_NULL_HANDLE ) {
		qxrDestroySpace( vr.space );
		vr.space = XR_NULL_HANDLE;
	}
	if ( vr.sessionRunning ) {
		qxrEndSession( vr.session );
	}
	qxrDestroySession( vr.session );

	vr.session = XR_NULL_HANDLE;
	vr.sessionRunning = 0;
	vr.sessionState = XR_SESSION_STATE_UNKNOWN;
	vr.frameBegun = 0;
	vr.frameActive = 0;
}

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------

static void VR_HandleSessionState( XrSessionState state ) {
	vr.sessionState = state;

	switch ( state ) {
	case XR_SESSION_STATE_READY:
		if ( !vr.sessionRunning ) {
			XrSessionBeginInfo bi;
			memset( &bi, 0, sizeof( bi ) );
			bi.type = XR_TYPE_SESSION_BEGIN_INFO;
			bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			if ( XR_SUCCEEDED( qxrBeginSession( vr.session, &bi ) ) ) {
				vr.sessionRunning = 1;
				VR_Log( 0, "VR: session running\n" );
			} else {
				VR_Log( 1, "VR: xrBeginSession failed\n" );
			}
		}
		break;
	case XR_SESSION_STATE_STOPPING:
		if ( vr.sessionRunning ) {
			qxrEndSession( vr.session );
			vr.sessionRunning = 0;
			VR_Log( 0, "VR: session stopped\n" );
		}
		break;
	case XR_SESSION_STATE_EXITING:
	case XR_SESSION_STATE_LOSS_PENDING:
		// the headset went away or the user quit from the system menu; stay flat until restart
		VR_Log( 0, "VR: session ending (state %d)\n", (int)state );
		break;
	default:
		break;
	}
}

static void VR_PollEvents( void ) {
	XrEventDataBuffer ev;

	for ( ;; ) {
		memset( &ev, 0, sizeof( ev ) );
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		if ( qxrPollEvent( vr.instance, &ev ) != XR_SUCCESS ) {
			break;
		}
		switch ( ev.type ) {
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
			VR_HandleSessionState( ( (XrEventDataSessionStateChanged *)&ev )->state );
			break;
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
			vr.recenterPending = 1;
			break;
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			VR_Log( 1, "VR: OpenXR instance loss pending\n" );
			break;
		default:
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// frames
// ---------------------------------------------------------------------------

static int VR_AcquireEyeImages( void ) {
	int eye;

	for ( eye = 0; eye < VR_MAX_EYES; eye++ ) {
		vrSwapchain_t *sc = &vr.swapchains[eye];
		XrSwapchainImageWaitInfo wi;

		if ( XR_FAILED( qxrAcquireSwapchainImage( sc->handle, NULL, &sc->acquiredIndex ) ) ) {
			return 0;
		}
		sc->acquired = 1;

		memset( &wi, 0, sizeof( wi ) );
		wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
		wi.timeout = XR_INFINITE_DURATION;
		if ( XR_FAILED( qxrWaitSwapchainImage( sc->handle, &wi ) ) ) {
			return 0;
		}
	}
	return 1;
}

static void VR_UpdateHeadOrigin( void ) {
	int i;

	if ( !vr.recenterPending ) {
		return;
	}
	if ( !( vr.viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT ) ) {
		return;
	}
	for ( i = 0; i < 3; i++ ) {
		// midpoint of the two eyes
		vr.headOrigin[i] = 0.5f * ( ( &vr.views[0].pose.position.x )[i] + ( &vr.views[1].pose.position.x )[i] );
	}
	vr.recenterPending = 0;
}

int VR_BeginFrame( void ) {
	XrFrameWaitInfo wi;
	XrFrameBeginInfo bi;
	XrViewLocateInfo li;
	uint32_t viewCount = 0;
	int eye;

	vr.frameActive = 0;

	if ( vr.session == XR_NULL_HANDLE ) {
		return 0;
	}

	if ( vr.frameBegun ) {
		// the last frame never reached VR_EndFrame (an error unwound it): close it empty
		VR_EndFrame( 0 );
	}

	VR_PollEvents();

	if ( !vr.sessionRunning ) {
		return 0;
	}

	memset( &wi, 0, sizeof( wi ) );
	wi.type = XR_TYPE_FRAME_WAIT_INFO;
	memset( &vr.frameState, 0, sizeof( vr.frameState ) );
	vr.frameState.type = XR_TYPE_FRAME_STATE;
	if ( XR_FAILED( qxrWaitFrame( vr.session, &wi, &vr.frameState ) ) ) {
		return 0;
	}

	memset( &bi, 0, sizeof( bi ) );
	bi.type = XR_TYPE_FRAME_BEGIN_INFO;
	if ( XR_FAILED( qxrBeginFrame( vr.session, &bi ) ) ) {
		return 0;
	}
	vr.frameBegun = 1;

	if ( !vr.frameState.shouldRender ) {
		return 0;	// VR_EndFrame submits an empty frame
	}

	memset( &li, 0, sizeof( li ) );
	li.type = XR_TYPE_VIEW_LOCATE_INFO;
	li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	li.displayTime = vr.frameState.predictedDisplayTime;
	li.space = vr.space;
	memset( &vr.viewState, 0, sizeof( vr.viewState ) );
	vr.viewState.type = XR_TYPE_VIEW_STATE;
	for ( eye = 0; eye < VR_MAX_EYES; eye++ ) {
		memset( &vr.views[eye], 0, sizeof( vr.views[eye] ) );
		vr.views[eye].type = XR_TYPE_VIEW;
	}
	if ( XR_FAILED( qxrLocateViews( vr.session, &li, &vr.viewState, VR_MAX_EYES, &viewCount, vr.views ) ) ||
		viewCount != VR_MAX_EYES ||
		!( vr.viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT ) ) {
		return 0;	// no tracking this frame: submit nothing rather than a wrong view
	}

	VR_UpdateHeadOrigin();

	if ( !VR_AcquireEyeImages() ) {
		return 0;
	}

	vr.frameActive = 1;
	return 1;
}

int VR_FrameActive( void ) {
	return vr.frameActive;
}

int VR_GetEyeView( int eye, vrEyeView_t *view ) {
	const XrView *v;

	if ( !vr.frameActive || eye < 0 || eye >= VR_MAX_EYES ) {
		return 0;
	}
	v = &vr.views[eye];
	view->position[0] = v->pose.position.x;
	view->position[1] = v->pose.position.y;
	view->position[2] = v->pose.position.z;
	view->orientation[0] = v->pose.orientation.x;
	view->orientation[1] = v->pose.orientation.y;
	view->orientation[2] = v->pose.orientation.z;
	view->orientation[3] = v->pose.orientation.w;
	view->angleLeft = v->fov.angleLeft;
	view->angleRight = v->fov.angleRight;
	view->angleUp = v->fov.angleUp;
	view->angleDown = v->fov.angleDown;
	return 1;
}

void VR_GetEyeOffset( int eye, float offset[3] ) {
	if ( !vr.frameActive || eye < 0 || eye >= VR_MAX_EYES ) {
		offset[0] = offset[1] = offset[2] = 0.0f;
		return;
	}
	offset[0] = vr.views[eye].pose.position.x - vr.headOrigin[0];
	offset[1] = vr.views[eye].pose.position.y - vr.headOrigin[1];
	offset[2] = vr.views[eye].pose.position.z - vr.headOrigin[2];
}

void VR_Recenter( void ) {
	vr.recenterPending = 1;
}

int VR_GetEyeImage( int eye, vrEyeImage_t *image ) {
	const vrSwapchain_t *sc;

	if ( !vr.frameActive || eye < 0 || eye >= VR_MAX_EYES ) {
		return 0;
	}
	sc = &vr.swapchains[eye];
	if ( !sc->acquired ) {
		return 0;
	}
	image->image = sc->images[sc->acquiredIndex].image;
	image->format = (VkFormat)sc->format;
	image->width = sc->width;
	image->height = sc->height;
	return 1;
}

void VR_EndFrame( int eyesWritten ) {
	XrCompositionLayerProjectionView projViews[VR_MAX_EYES];
	XrCompositionLayerProjection layer;
	const XrCompositionLayerBaseHeader *layers[1];
	XrFrameEndInfo ei;
	int eye, submit;

	if ( vr.session == XR_NULL_HANDLE || !vr.frameBegun ) {
		vr.frameActive = 0;
		return;
	}

	submit = vr.frameActive && eyesWritten;
	for ( eye = 0; eye < VR_MAX_EYES; eye++ ) {
		vrSwapchain_t *sc = &vr.swapchains[eye];
		if ( sc->acquired ) {
			qxrReleaseSwapchainImage( sc->handle, NULL );
			sc->acquired = 0;
		} else {
			submit = 0;
		}
	}

	memset( &ei, 0, sizeof( ei ) );
	ei.type = XR_TYPE_FRAME_END_INFO;
	ei.displayTime = vr.frameState.predictedDisplayTime;
	ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

	if ( submit ) {
		for ( eye = 0; eye < VR_MAX_EYES; eye++ ) {
			memset( &projViews[eye], 0, sizeof( projViews[eye] ) );
			projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projViews[eye].pose = vr.views[eye].pose;
			projViews[eye].fov = vr.views[eye].fov;
			projViews[eye].subImage.swapchain = vr.swapchains[eye].handle;
			projViews[eye].subImage.imageRect.offset.x = 0;
			projViews[eye].subImage.imageRect.offset.y = 0;
			projViews[eye].subImage.imageRect.extent.width = (int32_t)vr.swapchains[eye].width;
			projViews[eye].subImage.imageRect.extent.height = (int32_t)vr.swapchains[eye].height;
			projViews[eye].subImage.imageArrayIndex = 0;
		}
		memset( &layer, 0, sizeof( layer ) );
		layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
		layer.space = vr.space;
		layer.viewCount = VR_MAX_EYES;
		layer.views = projViews;
		layers[0] = (const XrCompositionLayerBaseHeader *)&layer;
		ei.layerCount = 1;
		ei.layers = layers;
	} else {
		ei.layerCount = 0;
		ei.layers = NULL;
	}

	if ( XR_FAILED( qxrEndFrame( vr.session, &ei ) ) ) {
		VR_Log( 1, "VR: xrEndFrame failed\n" );
	}

	vr.frameBegun = 0;
	vr.frameActive = 0;
}
