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
- Views: primary stereo, one swapchain per eye; the renderer draws straight into them.
- Space: STAGE when the runtime has it, else LOCAL; VIEW for head-locked layers.
- Layers (VR_EndFrame): the world (projection, fresh or frozen), a dimmer quad, and the 2D
  canvas quad (HUD, menus, console, cutscenes). docs/vr/RENDERING.md has the reasons.
- Display: XR_FB_display_refresh_rate, when present, asks for the wanted rate (72 Hz on a Quest 2).
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

#include <math.h>
#ifndef _WIN32
#include <time.h>
#endif

#include "vr_local.h"	// controller input (vr_input.c)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// printing
// ---------------------------------------------------------------------------

static vrPrintFunc_t vr_print;

void VR_SetPrint( vrPrintFunc_t print ) {
	vr_print = print;
}

void VR_Log( int isError, const char *fmt, ... ) {	// also used by vr_input.c
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
	int64_t				format;			// the swapchain's own format
	int64_t				viewFormat;		// what the renderer renders or copies through
	uint32_t			imageCount;
	XrSwapchainImageVulkan2KHR images[VR_MAX_SWAPCHAIN_IMAGES];
	uint32_t			acquiredIndex;
	int					acquired;
	int					released;		// an image has been released at least once: submittable
} vrSwapchain_t;

typedef struct {
	int					placed;
	float				yaw;			// radians about +Y; 0 looks down -Z
	float				position[3];	// in vr.space
	int					turning;		// lazy follow: catching up with the head
} vrQuadAnchor_t;

static struct {
	XrInstance			instance;
	XrSystemId			systemId;
	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
	XrGraphicsRequirementsVulkan2KHR graphicsReqs;
	int					haveRefreshRate;	// XR_FB_display_refresh_rate enabled
	vrOptions_t			options;

	XrSession			session;
	XrSessionState		sessionState;
	int					sessionRunning;
	XrSpace				space;
	XrReferenceSpaceType spaceType;
	XrSpace				viewSpace;			// head-locked layers

	uint32_t			viewCount;
	XrViewConfigurationView configViews[VR_MAX_EYES];
	vrSwapchain_t		swapchains[VR_MAX_EYES];
	vrSwapchain_t		canvas;				// the 2D layer
	vrSwapchain_t		dimmer;				// translucent black, over a frozen world
	float				dimmerAlpha;		// what the dimmer image holds
	float				dimmerPending;		// what the renderer fills it with this frame

	// current frame
	int					frameBegun;		// xrBeginFrame called, xrEndFrame due
	int					frameActive;	// views located: render
	XrFrameState		frameState;
	XrView				views[VR_MAX_EYES];
	XrViewState			viewState;

	// the views of the last fresh projection, for re-showing it frozen
	int					frozenValid;
	XrView				frozenViews[VR_MAX_EYES];

	vrQuadAnchor_t		anchors[3];		// by vrQuadMode_t
	int					lastQuadMode;	// -1 = no quad last frame

	// head origin for VR_GetEyeOffset
	int					recenterPending;
	unsigned			recenterCount;		// recentres done (vr_move.c watches it)
	float				headOrigin[3];
	float				debugHead[3];		// vr_debugHead: added to the located views
	int					debugTurnOn;		// vr_debugHeadTurn: a rotation applied to the located views
	float				debugTurn[4];		// x y z w, OpenXR axes

	// comfort (vr_move.c): the wall fade (the dimmer) and the movement vignette
	float				comfortFade;
	float				comfortVignette;
	vrSwapchain_t		vignette;			// filled once with a radial mask (vr_move: vignette)
	int					vignetteState;		// 0 empty, 1 acquired for its fill this frame, 2 filled

	// ending: our own exit request (shutdown, vid_restart) vs the runtime ending the session
	int					exitRequested;		// we called xrRequestExitSession
	vrExitReason_t		exitReason;			// the runtime ended the session or lost the headset
	int					disabled;			// after a runtime-side end: VR_Init stays flat for this run
} vr;

static PFN_xrEnumerateDisplayRefreshRatesFB qxrEnumerateDisplayRefreshRatesFB;
static PFN_xrRequestDisplayRefreshRateFB qxrRequestDisplayRefreshRateFB;

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
	const char *enabled[8];
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
	if ( vr.disabled ) {
		VR_Log( 0, "VR: the headset session ended earlier in this run, staying flat\n" );
		return 0;
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
	vr.haveRefreshRate = VR_HasExtension( exts, extCount, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME );
	if ( vr.haveRefreshRate ) {
		enabled[enabledCount++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
	}
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
	qxrEnumerateDisplayRefreshRatesFB = NULL;
	qxrRequestDisplayRefreshRateFB = NULL;
	if ( vr.haveRefreshRate ) {
		qxrGetInstanceProcAddr( vr.instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&qxrEnumerateDisplayRefreshRatesFB );
		qxrGetInstanceProcAddr( vr.instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&qxrRequestDisplayRefreshRateFB );
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

	// controllers: actions and bindings belong to the instance (no input is no reason to stay flat)
	VR_Input_Init( vr.instance, qxrGetInstanceProcAddr );

	vr.recenterPending = 1;
	if ( vr.options.resolutionScale <= 0.0f ) {
		vr.options.resolutionScale = 1.0f;
	}
	return 1;
}

void VR_SetOptions( const vrOptions_t *options ) {
	vr.options = *options;
	if ( !( vr.options.resolutionScale >= 0.25f ) ) {
		vr.options.resolutionScale = 1.0f;
	}
}

/*
The whole OpenXR side goes, while the Vulkan device and instance it made are still alive:
the session is ended (VR_DestroySession), then the instance. Some runtimes (the Meta XR
Simulator) free their own objects on the device, such as its queue wrappers, as late as
xrDestroyInstance or just after it, so vkDestroyDevice must come after this.
*/
void VR_Shutdown( void ) {
	VR_DestroySession();
	VR_Input_Shutdown();

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
// swapchains
// ---------------------------------------------------------------------------

/*
Colour: the game's colours are gamma-encoded already (the renderer never linearises). An sRGB
swapchain is what compositors expect, so it is created with MUTABLE_FORMAT and the renderer
writes it through a UNORM view (or copies raw bits into it): the stored bytes are the game's own,
and the compositor decodes them as sRGB, which is right. A UNORM swapchain is the fallback; a
compositor then treats the values as linear and the picture looks washed out.
*/
static VkFormat VR_UnormTwin( VkFormat format ) {
	switch ( format ) {
	case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
	case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
	default: return format;
	}
}

static VkFormat VR_SrgbTwin( VkFormat format ) {
	switch ( format ) {
	case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
	case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
	default: return format;
	}
}

// the first of prefs the runtime offers, or VK_FORMAT_UNDEFINED
static int64_t VR_PickFormat( const int64_t *prefs, int count ) {
	int64_t formats[64];
	uint32_t n = 0, j;
	int i;

	if ( XR_FAILED( qxrEnumerateSwapchainFormats( vr.session, 64, &n, formats ) ) ) {
		return VK_FORMAT_UNDEFINED;
	}
	for ( i = 0; i < count; i++ ) {
		for ( j = 0; j < n; j++ ) {
			if ( formats[j] == prefs[i] ) {
				return formats[j];
			}
		}
	}
	return VK_FORMAT_UNDEFINED;
}

static void VR_DestroySwapchain( vrSwapchain_t *sc ) {
	if ( sc->handle != XR_NULL_HANDLE ) {
		if ( sc->acquired ) {
			qxrReleaseSwapchainImage( sc->handle, NULL );
		}
		qxrDestroySwapchain( sc->handle );
	}
	memset( sc, 0, sizeof( *sc ) );
}

static int VR_CreateSwapchain( vrSwapchain_t *sc, const char *what, uint32_t width, uint32_t height,
	int64_t format, XrSwapchainUsageFlags usage ) {
	XrSwapchainCreateInfo ci;
	uint32_t i;
	XrResult res;

	memset( sc, 0, sizeof( *sc ) );
	sc->viewFormat = VR_UnormTwin( (VkFormat)format );
	if ( sc->viewFormat != format ) {
		usage |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
	}

	memset( &ci, 0, sizeof( ci ) );
	ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	ci.usageFlags = usage;
	ci.format = format;
	ci.sampleCount = 1;
	ci.width = width;
	ci.height = height;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;
	res = qxrCreateSwapchain( vr.session, &ci, &sc->handle );
	if ( XR_FAILED( res ) ) {
		VR_Log( 1, "VR: xrCreateSwapchain (%s) failed: %s\n", what, VR_ResultString( res ) );
		sc->handle = XR_NULL_HANDLE;
		return 0;
	}
	sc->width = width;
	sc->height = height;
	sc->format = format;

	if ( XR_FAILED( qxrEnumerateSwapchainImages( sc->handle, 0, &sc->imageCount, NULL ) ) ||
		sc->imageCount == 0 || sc->imageCount > VR_MAX_SWAPCHAIN_IMAGES ) {
		VR_Log( 1, "VR: %s swapchain has %u images (at most %d)\n", what, sc->imageCount, VR_MAX_SWAPCHAIN_IMAGES );
		VR_DestroySwapchain( sc );
		return 0;
	}
	for ( i = 0; i < sc->imageCount; i++ ) {
		sc->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR;
	}
	if ( XR_FAILED( qxrEnumerateSwapchainImages( sc->handle, sc->imageCount, &sc->imageCount,
		(XrSwapchainImageBaseHeader *)sc->images ) ) ) {
		VR_Log( 1, "VR: xrEnumerateSwapchainImages (%s) failed\n", what );
		VR_DestroySwapchain( sc );
		return 0;
	}
	VR_Log( 0, "VR: %s swapchain %ux%u, format %d (rendered as %d), %u images\n", what, width, height,
		(int)format, (int)sc->viewFormat, sc->imageCount );
	return 1;
}

static void VR_FillInfo( const vrSwapchain_t *sc, vrSwapchainInfo_t *info ) {
	uint32_t i;

	memset( info, 0, sizeof( *info ) );
	info->width = sc->width;
	info->height = sc->height;
	info->imageFormat = (VkFormat)sc->format;
	info->viewFormat = (VkFormat)sc->viewFormat;
	info->imageCount = sc->imageCount;
	for ( i = 0; i < sc->imageCount; i++ ) {
		info->images[i] = sc->images[i].image;
	}
}

static int VR_CreateEyeSwapchains( void ) {
	static const int64_t prefs[] = {
		VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
		VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
	};
	const int64_t format = VR_PickFormat( prefs, sizeof( prefs ) / sizeof( prefs[0] ) );
	uint32_t eye;

	if ( format == VK_FORMAT_UNDEFINED ) {
		VR_Log( 1, "VR: the runtime has no 8-bit RGBA swapchain format\n" );
		return 0;
	}
	if ( format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM ) {
		VR_Log( 0, "VR: no sRGB eye format; colours may look washed out\n" );
	}

	for ( eye = 0; eye < VR_MAX_EYES; eye++ ) {
		const XrViewConfigurationView *cv = &vr.configViews[eye];
		uint32_t w = (uint32_t)( cv->recommendedImageRectWidth * vr.options.resolutionScale + 0.5f );
		uint32_t h = (uint32_t)( cv->recommendedImageRectHeight * vr.options.resolutionScale + 0.5f );

		w = ( w + 7 ) & ~7u;	// tile-friendly
		h = ( h + 7 ) & ~7u;
		if ( w > cv->maxImageRectWidth ) w = cv->maxImageRectWidth;
		if ( h > cv->maxImageRectHeight ) h = cv->maxImageRectHeight;
		if ( w < 64 ) w = 64;
		if ( h < 64 ) h = 64;

		if ( !VR_CreateSwapchain( &vr.swapchains[eye], eye ? "right eye" : "left eye", w, h, format,
				XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
				( vr.options.debugCapture ? XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT : 0 ) ) ) {
			return 0;
		}
	}

	// the dimmer: small, filled with one colour by the renderer (vkCmdClearColorImage)
	if ( !VR_CreateSwapchain( &vr.dimmer, "dimmer", 16, 16, format,
			XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT ) ) {
		VR_Log( 0, "VR: no dimmer layer\n" );
	}
	vr.dimmerAlpha = 0.0f;
	vr.dimmerPending = 0.0f;

	// the movement vignette: a radial mask, filled once by the renderer
	if ( !VR_CreateSwapchain( &vr.vignette, "vignette", VR_VIGNETTE_SIZE, VR_VIGNETTE_SIZE, format,
			XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT ) ) {
		VR_Log( 0, "VR: no vignette layer\n" );
	}
	vr.vignetteState = 0;
	return 1;
}

int VR_GetEyeSwapchain( int eye, vrSwapchainInfo_t *info ) {
	if ( eye < 0 || eye >= VR_MAX_EYES || vr.swapchains[eye].handle == XR_NULL_HANDLE ) {
		return 0;
	}
	VR_FillInfo( &vr.swapchains[eye], info );
	return 1;
}

int VR_GetDimmerSwapchain( vrSwapchainInfo_t *info ) {
	if ( vr.dimmer.handle == XR_NULL_HANDLE ) {
		return 0;
	}
	VR_FillInfo( &vr.dimmer, info );
	return 1;
}

int VR_SetCanvas( uint32_t width, uint32_t height, VkFormat colorFormat, vrSwapchainInfo_t *info ) {
	int64_t prefs[2];
	int64_t format;

	if ( vr.session == XR_NULL_HANDLE ) {
		return 0;
	}
	prefs[0] = VR_SrgbTwin( colorFormat );
	prefs[1] = colorFormat;
	format = VR_PickFormat( prefs, 2 );

	if ( vr.canvas.handle != XR_NULL_HANDLE ) {
		if ( vr.canvas.width == width && vr.canvas.height == height && vr.canvas.format == format ) {
			VR_FillInfo( &vr.canvas, info );
			return 1;
		}
		VR_DestroySwapchain( &vr.canvas );
	}
	if ( format == VK_FORMAT_UNDEFINED ) {
		VR_Log( 1, "VR: no swapchain format for the 2D layer (canvas format %d)\n", (int)colorFormat );
		return 0;
	}
	// filled with vkCmdCopyImage, a raw copy (the bits stay the game's gamma-encoded ones)
	if ( !VR_CreateSwapchain( &vr.canvas, "2D layer", width, height, format,
			XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT ) ) {
		return 0;
	}
	VR_FillInfo( &vr.canvas, info );
	return 1;
}

// ---------------------------------------------------------------------------
// session and spaces
// ---------------------------------------------------------------------------

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

	ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if ( XR_FAILED( qxrCreateReferenceSpace( vr.session, &ci, &vr.viewSpace ) ) ) {
		vr.viewSpace = XR_NULL_HANDLE;	// head-locked layers fall back to world-locked
	}
	VR_Log( 0, "VR: reference space %s\n", vr.spaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL" );
	return 1;
}

static void VR_SetRefreshRate( void ) {
	float rates[16], best = 0.0f;
	uint32_t n = 0, i;

	if ( !qxrEnumerateDisplayRefreshRatesFB || !qxrRequestDisplayRefreshRateFB || vr.options.refreshRate <= 0.0f ) {
		return;
	}
	if ( XR_FAILED( qxrEnumerateDisplayRefreshRatesFB( vr.session, 16, &n, rates ) ) || n == 0 ) {
		return;
	}
	for ( i = 0; i < n; i++ ) {
		if ( best == 0.0f || fabsf( rates[i] - vr.options.refreshRate ) < fabsf( best - vr.options.refreshRate ) ) {
			best = rates[i];
		}
	}
	if ( XR_SUCCEEDED( qxrRequestDisplayRefreshRateFB( vr.session, best ) ) ) {
		VR_Log( 0, "VR: display refresh rate %.0f Hz requested (%u offered)\n", best, n );
	}
}

int VR_CreateSession( VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
	uint32_t queueFamilyIndex, uint32_t queueIndex ) {
	XrGraphicsBindingVulkan2KHR binding;
	XrSessionCreateInfo ci;
	XrResult res;

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

	res = qxrCreateSession( vr.instance, &ci, &vr.session );
	if ( XR_FAILED( res ) ) {
		VR_Log( 1, "VR: xrCreateSession failed: %s\n", VR_ResultString( res ) );
		vr.session = XR_NULL_HANDLE;
		return 0;
	}
	vr.sessionState = XR_SESSION_STATE_UNKNOWN;
	vr.sessionRunning = 0;
	vr.frozenValid = 0;
	vr.lastQuadMode = -1;
	memset( vr.anchors, 0, sizeof( vr.anchors ) );

	if ( !VR_CreateSpace() || !VR_CreateEyeSwapchains() ) {
		VR_DestroySession();
		return 0;
	}
	VR_SetRefreshRate();
	VR_Input_AttachSession( vr.session );

	VR_Log( 0, "VR: session created, waiting for the runtime to start it\n" );
	return 1;
}

static void VR_PollEvents( void );

static double VR_Seconds( void ) {
#ifdef _WIN32
	return (double)GetTickCount64() / 1000.0;
#else
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static void VR_SleepMs( int ms ) {
#ifdef _WIN32
	Sleep( (DWORD)ms );
#else
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)( ms % 1000 ) * 1000000L;
	nanosleep( &ts, NULL );
#endif
}

/*
Ends a running session the way OpenXR wants: xrRequestExitSession, then the runtime moves
through STOPPING (VR_HandleSessionState calls xrEndSession there) to IDLE and EXITING. Until
STOPPING arrives the frame loop must keep going, so empty frames are submitted meanwhile.
Bounded: a runtime that doesn't answer (a paused Android activity, a dead simulator) gets
xrEndSession after the timeout anyway, and the session is destroyed regardless.
*/
static void VR_StopSession( void ) {
	double deadline;
	XrResult res;

	if ( vr.frameBegun ) {
		VR_EndFrame( NULL );	// a frame left open by an error
	}
	VR_PollEvents();	// the runtime may already be stopping
	if ( !vr.sessionRunning ) {
		return;
	}

	vr.exitRequested = 1;
	res = qxrRequestExitSession( vr.session );
	if ( XR_FAILED( res ) ) {
		VR_Log( 0, "VR: xrRequestExitSession: %s\n", VR_ResultString( res ) );
	}

	deadline = VR_Seconds() + 2.0;
	while ( XR_SUCCEEDED( res ) && vr.sessionRunning && VR_Seconds() < deadline ) {
		VR_PollEvents();
		if ( !vr.sessionRunning ) {
			break;
		}
		if ( vr.sessionState == XR_SESSION_STATE_SYNCHRONIZED || vr.sessionState == XR_SESSION_STATE_VISIBLE ||
			vr.sessionState == XR_SESSION_STATE_FOCUSED || vr.sessionState == XR_SESSION_STATE_READY ) {
			XrFrameWaitInfo wi;
			XrFrameBeginInfo bi;
			XrFrameEndInfo ei;

			memset( &wi, 0, sizeof( wi ) );
			wi.type = XR_TYPE_FRAME_WAIT_INFO;
			memset( &vr.frameState, 0, sizeof( vr.frameState ) );
			vr.frameState.type = XR_TYPE_FRAME_STATE;
			if ( XR_FAILED( qxrWaitFrame( vr.session, &wi, &vr.frameState ) ) ) {
				VR_SleepMs( 5 );
				continue;
			}
			memset( &bi, 0, sizeof( bi ) );
			bi.type = XR_TYPE_FRAME_BEGIN_INFO;
			if ( XR_FAILED( qxrBeginFrame( vr.session, &bi ) ) ) {
				continue;
			}
			memset( &ei, 0, sizeof( ei ) );
			ei.type = XR_TYPE_FRAME_END_INFO;
			ei.displayTime = vr.frameState.predictedDisplayTime;
			ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
			qxrEndFrame( vr.session, &ei );
		} else {
			VR_SleepMs( 5 );
		}
	}

	if ( vr.sessionRunning ) {
		res = qxrEndSession( vr.session );
		VR_Log( 0, "VR: the runtime did not stop the session in time; xrEndSession: %s\n", VR_ResultString( res ) );
		vr.sessionRunning = 0;
		return;
	}

	// after xrEndSession the runtime reports IDLE, then EXITING: wait briefly for EXITING
	deadline = VR_Seconds() + 0.5;
	while ( vr.sessionState != XR_SESSION_STATE_EXITING && vr.sessionState != XR_SESSION_STATE_LOSS_PENDING &&
		VR_Seconds() < deadline ) {
		VR_PollEvents();
		if ( vr.sessionState != XR_SESSION_STATE_EXITING ) {
			VR_SleepMs( 5 );
		}
	}
}

void VR_DestroySession( void ) {
	int eye;

	if ( vr.session == XR_NULL_HANDLE ) {
		return;
	}
	VR_StopSession();
	VR_Input_DetachSession();

	for ( eye = 0; eye < VR_MAX_EYES; eye++ ) {
		VR_DestroySwapchain( &vr.swapchains[eye] );
	}
	VR_DestroySwapchain( &vr.canvas );
	VR_DestroySwapchain( &vr.dimmer );
	VR_DestroySwapchain( &vr.vignette );
	vr.vignetteState = 0;

	if ( vr.viewSpace != XR_NULL_HANDLE ) {
		qxrDestroySpace( vr.viewSpace );
		vr.viewSpace = XR_NULL_HANDLE;
	}
	if ( vr.space != XR_NULL_HANDLE ) {
		qxrDestroySpace( vr.space );
		vr.space = XR_NULL_HANDLE;
	}
	qxrDestroySession( vr.session );
	VR_Log( 0, "VR: session destroyed\n" );

	vr.session = XR_NULL_HANDLE;
	vr.sessionRunning = 0;
	vr.sessionState = XR_SESSION_STATE_UNKNOWN;
	vr.exitRequested = 0;
	vr.frameBegun = 0;
	vr.frameActive = 0;
	vr.frozenValid = 0;
}

vrExitReason_t VR_GetExitReason( void ) {
	return vr.exitReason;
}

void VR_DebugEndSession( void ) {
	if ( vr.session == XR_NULL_HANDLE || !vr.sessionRunning ) {
		VR_Log( 0, "VR: no running session\n" );
		return;
	}
	// as if the user had quit from the headset's system menu: the runtime stops the session,
	// and it ends in EXITING without our own exit request
	VR_Log( 0, "VR: debug: asking the runtime to end the session\n" );
	qxrRequestExitSession( vr.session );
}

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------

static const char *VR_StateName( XrSessionState state ) {
	switch ( state ) {
	case XR_SESSION_STATE_IDLE: return "IDLE";
	case XR_SESSION_STATE_READY: return "READY";
	case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
	case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
	case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
	case XR_SESSION_STATE_STOPPING: return "STOPPING";
	case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
	case XR_SESSION_STATE_EXITING: return "EXITING";
	default: return "UNKNOWN";
	}
}

static void VR_HandleSessionState( XrSessionState state ) {
	vr.sessionState = state;
	VR_Log( 0, "VR: session state %s\n", VR_StateName( state ) );

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
		if ( vr.sessionRunning ) {	// LOSS_PENDING can come without STOPPING
			qxrEndSession( vr.session );
			vr.sessionRunning = 0;
		}
		if ( !vr.exitRequested && vr.exitReason == VR_EXIT_NONE ) {
			// the user quit from the headset's system menu, or the runtime lost the headset:
			// the renderer drops to flat (desktop) or quits (Quest)
			vr.exitReason = state == XR_SESSION_STATE_EXITING ? VR_EXIT_RUNTIME : VR_EXIT_LOST;
			vr.disabled = 1;
			VR_Log( 0, "VR: the runtime ended the session (%s)\n", VR_StateName( state ) );
		} else {
			VR_Log( 0, "VR: session ending\n" );
		}
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
			VR_Recenter();
			break;
		case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
			VR_Input_ProfileChanged();
			break;
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			VR_Log( 1, "VR: OpenXR instance loss pending\n" );
			if ( vr.exitReason == VR_EXIT_NONE ) {
				vr.exitReason = VR_EXIT_LOST;
				vr.disabled = 1;
			}
			break;
		default:
			break;
		}
	}
}

// ---------------------------------------------------------------------------
// frames
// ---------------------------------------------------------------------------

static int VR_Acquire( vrSwapchain_t *sc ) {
	XrSwapchainImageWaitInfo wi;

	if ( sc->handle == XR_NULL_HANDLE || sc->acquired ) {
		return sc->acquired;
	}
	if ( XR_FAILED( qxrAcquireSwapchainImage( sc->handle, NULL, &sc->acquiredIndex ) ) ) {
		return 0;
	}
	sc->acquired = 1;

	memset( &wi, 0, sizeof( wi ) );
	wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
	wi.timeout = XR_INFINITE_DURATION;
	if ( XR_FAILED( qxrWaitSwapchainImage( sc->handle, &wi ) ) ) {
		// still acquired: VR_EndFrame releases it
		return 0;
	}
	return 1;
}

static void VR_Release( vrSwapchain_t *sc ) {
	if ( sc->acquired ) {
		qxrReleaseSwapchainImage( sc->handle, NULL );
		sc->acquired = 0;
		sc->released = 1;
	}
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
	memset( vr.anchors, 0, sizeof( vr.anchors ) );	// the layers are placed again in front
	vr.recenterPending = 0;
	vr.recenterCount++;
}

// a quaternion product a * b (x y z w)
static void VR_QuatMul( const float *a, const float *b, float *out ) {
	out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
	out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
	out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
	out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

/*
vr_debugHeadTurn: turn the located views about the point between the eyes, as if the head had
turned, so effects can be checked with the head looking away from the body without a headset
*/
static void VR_ApplyDebugTurn( void ) {
	const float *q = vr.debugTurn;
	float mid[3], p[3], t[3], o[4], r[4];
	int eye;

	mid[0] = 0.5f * ( vr.views[0].pose.position.x + vr.views[1].pose.position.x );
	mid[1] = 0.5f * ( vr.views[0].pose.position.y + vr.views[1].pose.position.y );
	mid[2] = 0.5f * ( vr.views[0].pose.position.z + vr.views[1].pose.position.z );
	for ( eye = 0; eye < VR_MAX_EYES; eye++ ) {
		XrPosef *pose = &vr.views[eye].pose;

		o[0] = pose->orientation.x; o[1] = pose->orientation.y;
		o[2] = pose->orientation.z; o[3] = pose->orientation.w;
		VR_QuatMul( q, o, r );
		pose->orientation.x = r[0]; pose->orientation.y = r[1];
		pose->orientation.z = r[2]; pose->orientation.w = r[3];

		// v' = v + 2w (q x v) + 2 q x (q x v)
		p[0] = pose->position.x - mid[0];
		p[1] = pose->position.y - mid[1];
		p[2] = pose->position.z - mid[2];
		t[0] = 2.0f * ( q[1] * p[2] - q[2] * p[1] );
		t[1] = 2.0f * ( q[2] * p[0] - q[0] * p[2] );
		t[2] = 2.0f * ( q[0] * p[1] - q[1] * p[0] );
		pose->position.x = mid[0] + p[0] + q[3] * t[0] + ( q[1] * t[2] - q[2] * t[1] );
		pose->position.y = mid[1] + p[1] + q[3] * t[1] + ( q[2] * t[0] - q[0] * t[2] );
		pose->position.z = mid[2] + p[2] + q[3] * t[2] + ( q[0] * t[1] - q[1] * t[0] );
	}
}

int VR_BeginFrame( int wantEyes, float dimAlpha ) {
	XrFrameWaitInfo wi;
	XrFrameBeginInfo bi;
	XrViewLocateInfo li;
	uint32_t viewCount = 0;
	int eye;
	static int loggedViews;

	vr.frameActive = 0;

	if ( vr.session == XR_NULL_HANDLE ) {
		return 0;
	}

	if ( vr.frameBegun ) {
		// the last frame never reached VR_EndFrame (an error unwound it): close it empty
		VR_EndFrame( NULL );
	}

	VR_PollEvents();

	if ( !vr.sessionRunning ) {
		VR_Input_Sync( XR_NULL_HANDLE, 0, 0 );	// let go of the controls
		return 0;
	}

	memset( &wi, 0, sizeof( wi ) );
	wi.type = XR_TYPE_FRAME_WAIT_INFO;
	memset( &vr.frameState, 0, sizeof( vr.frameState ) );
	vr.frameState.type = XR_TYPE_FRAME_STATE;
	if ( XR_FAILED( qxrWaitFrame( vr.session, &wi, &vr.frameState ) ) ) {
		return 0;
	}
	VR_Input_Sync( vr.space, vr.frameState.predictedDisplayTime, vr.sessionState == XR_SESSION_STATE_FOCUSED );

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

	if ( vr.debugTurnOn ) {
		VR_ApplyDebugTurn();
	}
	if ( vr.debugHead[0] || vr.debugHead[1] || vr.debugHead[2] ) {
		for ( eye = 0; eye < VR_MAX_EYES; eye++ ) {	// vr_debugHead: a pretend head movement
			vr.views[eye].pose.position.x += vr.debugHead[0];
			vr.views[eye].pose.position.y += vr.debugHead[1];
			vr.views[eye].pose.position.z += vr.debugHead[2];
		}
	}

	VR_UpdateHeadOrigin();

	if ( !loggedViews ) {	// once: the runtime's eye fields of view and the eye separation
		const XrPosef *l = &vr.views[0].pose, *r = &vr.views[1].pose;
		float dx = r->position.x - l->position.x, dy = r->position.y - l->position.y;
		float dz = r->position.z - l->position.z;
		loggedViews = 1;
		for ( eye = 0; eye < VR_MAX_EYES; eye++ ) {
			const XrFovf *f = &vr.views[eye].fov;
			VR_Log( 0, "VR: eye %d fov left %.1f right %.1f up %.1f down %.1f deg\n", eye,
				f->angleLeft * 180.0 / M_PI, f->angleRight * 180.0 / M_PI,
				f->angleUp * 180.0 / M_PI, f->angleDown * 180.0 / M_PI );
		}
		VR_Log( 0, "VR: eye separation %.1f mm\n", sqrtf( dx * dx + dy * dy + dz * dz ) * 1000.0f );
	}

	if ( wantEyes ) {
		for ( eye = 0; eye < VR_MAX_EYES; eye++ ) {
			VR_Acquire( &vr.swapchains[eye] );
		}
	}
	VR_Acquire( &vr.canvas );

	vr.dimmerPending = 0.0f;
	if ( dimAlpha > 0.0f && fabsf( dimAlpha - vr.dimmerAlpha ) > 0.005f && VR_Acquire( &vr.dimmer ) ) {
		vr.dimmerPending = dimAlpha;
	}
	if ( vr.vignetteState == 0 && vr.comfortVignette > 0.0f && VR_Acquire( &vr.vignette ) ) {
		vr.vignetteState = 1;	// the renderer fills it this frame
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

void VR_GetHeadOrigin( float origin[3] ) {
	origin[0] = vr.headOrigin[0];
	origin[1] = vr.headOrigin[1];
	origin[2] = vr.headOrigin[2];
}

void VR_SetHeadOrigin( const float origin[3] ) {
	vr.headOrigin[0] = origin[0];
	vr.headOrigin[1] = origin[1];
	vr.headOrigin[2] = origin[2];
}

unsigned VR_GetRecenterCount( void ) {
	return vr.recenterCount;
}

void VR_SetDebugHeadTurn( const float quat[4] ) {
	if ( quat ) {
		vr.debugTurn[0] = quat[0];
		vr.debugTurn[1] = quat[1];
		vr.debugTurn[2] = quat[2];
		vr.debugTurn[3] = quat[3];
		vr.debugTurnOn = 1;
	} else {
		vr.debugTurnOn = 0;
	}
}

void VR_SetDebugHeadOffset( const float offset[3] ) {
	vr.debugHead[0] = offset[0];
	vr.debugHead[1] = offset[1];
	vr.debugHead[2] = offset[2];
}

void VR_SetComfort( float fade, float vignette ) {
	vr.comfortFade = fade < 0.0f ? 0.0f : fade > 1.0f ? 1.0f : fade;
	vr.comfortVignette = vignette < 0.0f ? 0.0f : vignette > 1.0f ? 1.0f : vignette;
}

float VR_GetComfortFade( void ) {
	return vr.comfortFade;
}

int VR_GetVignetteSwapchain( vrSwapchainInfo_t *info ) {
	if ( vr.vignette.handle == XR_NULL_HANDLE ) {
		return 0;
	}
	VR_FillInfo( &vr.vignette, info );
	return 1;
}

int VR_GetVignetteImageIndex( void ) {
	if ( !vr.frameActive || !vr.vignette.acquired || vr.vignetteState != 1 ) {
		return -1;
	}
	return (int)vr.vignette.acquiredIndex;
}

int VR_GetEyeImageIndex( int eye ) {
	if ( !vr.frameActive || eye < 0 || eye >= VR_MAX_EYES || !vr.swapchains[eye].acquired ) {
		return -1;
	}
	return (int)vr.swapchains[eye].acquiredIndex;
}

int VR_GetCanvasImageIndex( void ) {
	if ( !vr.frameActive || !vr.canvas.acquired ) {
		return -1;
	}
	return (int)vr.canvas.acquiredIndex;
}

int VR_GetDimmerImageIndex( float *alpha ) {
	if ( !vr.frameActive || !vr.dimmer.acquired || vr.dimmerPending <= 0.0f ) {
		return -1;
	}
	*alpha = vr.dimmerPending;
	return (int)vr.dimmer.acquiredIndex;
}

int VR_HaveFrozenEyes( void ) {
	return vr.frozenValid;
}

// ---------------------------------------------------------------------------
// layers
// ---------------------------------------------------------------------------

static void VR_YawQuat( float yaw, XrQuaternionf *q ) {
	q->x = 0.0f;
	q->y = sinf( yaw * 0.5f );
	q->z = 0.0f;
	q->w = cosf( yaw * 0.5f );
}

// the head: the eyes' midpoint and its yaw about +Y (0 = looking down -Z)
static void VR_HeadPose( float pos[3], float *yaw ) {
	const XrQuaternionf *q = &vr.views[0].pose.orientation;
	const float fx = -2.0f * ( q->x * q->z + q->w * q->y );
	const float fz = -( 1.0f - 2.0f * ( q->x * q->x + q->y * q->y ) );

	pos[0] = 0.5f * ( vr.views[0].pose.position.x + vr.views[1].pose.position.x );
	pos[1] = 0.5f * ( vr.views[0].pose.position.y + vr.views[1].pose.position.y );
	pos[2] = 0.5f * ( vr.views[0].pose.position.z + vr.views[1].pose.position.z );
	*yaw = ( fabsf( fx ) + fabsf( fz ) > 1e-4f ) ? atan2f( -fx, -fz ) : 0.0f;
}

static float VR_AngleDiff( float a, float b ) {
	float d = a - b;
	while ( d > (float)M_PI ) d -= 2.0f * (float)M_PI;
	while ( d < -(float)M_PI ) d += 2.0f * (float)M_PI;
	return d;
}

// where the quad goes this frame; returns the space to submit it in
static XrSpace VR_PlaceQuad( vrQuadMode_t mode, const vrQuadPlacement_t *p, XrPosef *pose ) {
	vrQuadAnchor_t *a = &vr.anchors[mode];
	float head[3], headYaw;

	if ( p->follow == 1 && vr.viewSpace != XR_NULL_HANDLE ) {
		pose->orientation.x = pose->orientation.y = pose->orientation.z = 0.0f;
		pose->orientation.w = 1.0f;
		pose->position.x = 0.0f;
		pose->position.y = p->height;
		pose->position.z = -p->distance;
		return vr.viewSpace;
	}

	VR_HeadPose( head, &headYaw );

	if ( !a->placed || (int)mode != vr.lastQuadMode ) {
		// placed in front of the head when the mode starts (a menu opens, a cutscene begins)
		a->placed = 1;
		a->yaw = headYaw;
		a->turning = 0;
		a->position[0] = head[0];
		a->position[1] = head[1];
		a->position[2] = head[2];
	}

	if ( p->follow == 2 ) {
		// lazy follow: the quad keeps still while the head looks around it, and swings round
		// (smoothly) once the head has turned more than followAngle away
		const float diff = VR_AngleDiff( headYaw, a->yaw );
		const float limit = p->followAngle * (float)M_PI / 180.0f;

		if ( fabsf( diff ) > limit ) {
			a->turning = 1;
		}
		if ( a->turning ) {
			a->yaw += diff * 0.08f;
			if ( fabsf( diff ) < 2.0f * (float)M_PI / 180.0f ) {
				a->turning = 0;
			}
		}
		// it moves with the head (leaning or stepping doesn't push the HUD away)
		a->position[0] = head[0];
		a->position[1] = head[1];
		a->position[2] = head[2];
	}

	VR_YawQuat( a->yaw, &pose->orientation );
	pose->position.x = a->position[0] - sinf( a->yaw ) * p->distance;
	pose->position.y = a->position[1] + p->height;
	pose->position.z = a->position[2] - cosf( a->yaw ) * p->distance;
	return vr.space;
}

// a line every ~10 seconds: the frame rate the runtime paced us at, and what was shown
static void VR_FrameStats( vrProjection_t projection, int layerCount ) {
	static XrTime start;
	static int frames, fresh, frozen, layers;

	if ( !vr.frameActive ) {
		return;
	}
	if ( frames == 0 ) {
		start = vr.frameState.predictedDisplayTime;
	}
	frames++;
	fresh += projection == VR_PROJ_FRESH;
	frozen += projection == VR_PROJ_FROZEN;
	layers += layerCount;
	if ( vr.frameState.predictedDisplayTime - start >= (XrTime)10000000000LL ) {
		const double secs = (double)( vr.frameState.predictedDisplayTime - start ) / 1e9;
		VR_Log( 0, "VR: %.1f fps over %.0f s (display period %.1f ms); world fresh %d%%, frozen %d%%, %.1f layers\n",
			frames / secs, secs, vr.frameState.predictedDisplayPeriod / 1e6, 100 * fresh / frames, 100 * frozen / frames,
			(double)layers / frames );
		frames = fresh = frozen = layers = 0;
	}
}

void VR_EndFrame( const vrFrameLayers_t *layers ) {
	XrCompositionLayerProjectionView projViews[VR_MAX_EYES];
	XrCompositionLayerProjection proj;
	XrCompositionLayerQuad dimQuad, quad, vigQuad;
	const XrCompositionLayerBaseHeader *list[4];
	XrFrameEndInfo ei;
	int eye, eyesAcquired, layerCount = 0;
	vrProjection_t projection = VR_PROJ_NONE;

	if ( vr.session == XR_NULL_HANDLE || !vr.frameBegun ) {
		vr.frameActive = 0;
		return;
	}

	eyesAcquired = vr.swapchains[0].acquired && vr.swapchains[1].acquired;
	for ( eye = 0; eye < VR_MAX_EYES; eye++ ) {
		VR_Release( &vr.swapchains[eye] );
	}
	if ( vr.canvas.acquired ) {
		VR_Release( &vr.canvas );
	}
	if ( vr.dimmer.acquired ) {
		VR_Release( &vr.dimmer );
		vr.dimmerAlpha = vr.dimmerPending;	// the renderer filled it this frame
	}
	if ( vr.vignette.acquired ) {
		VR_Release( &vr.vignette );
		vr.vignetteState = 2;	// filled once; the runtime keeps showing the last released image
	}

	if ( layers && vr.frameActive ) {
		projection = layers->projection;
		if ( projection == VR_PROJ_FRESH && !eyesAcquired ) {
			projection = VR_PROJ_NONE;
		}
		if ( projection == VR_PROJ_FROZEN && !vr.frozenValid ) {
			projection = VR_PROJ_NONE;
		}
	}
	if ( eyesAcquired && projection != VR_PROJ_FRESH ) {
		// released without being drawn: what they hold is no longer the last frame
		vr.frozenValid = 0;
	}

	if ( projection != VR_PROJ_NONE ) {
		const XrView *views = projection == VR_PROJ_FRESH ? vr.views : vr.frozenViews;

		for ( eye = 0; eye < VR_MAX_EYES; eye++ ) {
			memset( &projViews[eye], 0, sizeof( projViews[eye] ) );
			projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projViews[eye].pose = views[eye].pose;
			projViews[eye].fov = views[eye].fov;
			projViews[eye].subImage.swapchain = vr.swapchains[eye].handle;
			projViews[eye].subImage.imageRect.extent.width = (int32_t)vr.swapchains[eye].width;
			projViews[eye].subImage.imageRect.extent.height = (int32_t)vr.swapchains[eye].height;
		}
		memset( &proj, 0, sizeof( proj ) );
		proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
		proj.space = vr.space;
		proj.viewCount = VR_MAX_EYES;
		proj.views = projViews;
		list[layerCount++] = (const XrCompositionLayerBaseHeader *)&proj;

		if ( projection == VR_PROJ_FRESH ) {
			memcpy( vr.frozenViews, vr.views, sizeof( vr.frozenViews ) );
			vr.frozenValid = 1;
		}

		if ( layers->vignette && vr.vignetteState == 2 && vr.vignette.released && vr.comfortVignette > 0.01f ) {
			// head-locked; the mask's clear disc is VR_VIGNETTE_HOLE of its half-size, so the
			// quad's size sets how much of the view stays clear: from about 58 degrees off the
			// centre (nothing visible) at strength 0 to about 29 degrees at strength 1
			const float clearTan = 1.6f - 1.05f * vr.comfortVignette;
			memset( &vigQuad, 0, sizeof( vigQuad ) );
			vigQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
			vigQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			vigQuad.space = vr.viewSpace != XR_NULL_HANDLE ? vr.viewSpace : vr.space;
			vigQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			vigQuad.subImage.swapchain = vr.vignette.handle;
			vigQuad.subImage.imageRect.extent.width = (int32_t)vr.vignette.width;
			vigQuad.subImage.imageRect.extent.height = (int32_t)vr.vignette.height;
			vigQuad.pose.orientation.w = 1.0f;
			vigQuad.pose.position.z = -1.0f;
			vigQuad.size.width = 2.0f * clearTan / VR_VIGNETTE_HOLE;
			vigQuad.size.height = vigQuad.size.width;
			list[layerCount++] = (const XrCompositionLayerBaseHeader *)&vigQuad;
		}

		if ( layers->dim && vr.dimmer.released && vr.dimmerAlpha > 0.0f ) {
			// head-locked, large enough to cover the whole field of view
			memset( &dimQuad, 0, sizeof( dimQuad ) );
			dimQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
			dimQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			dimQuad.space = vr.viewSpace != XR_NULL_HANDLE ? vr.viewSpace : vr.space;
			dimQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			dimQuad.subImage.swapchain = vr.dimmer.handle;
			dimQuad.subImage.imageRect.extent.width = (int32_t)vr.dimmer.width;
			dimQuad.subImage.imageRect.extent.height = (int32_t)vr.dimmer.height;
			dimQuad.pose.orientation.w = 1.0f;
			dimQuad.pose.position.z = -1.0f;
			dimQuad.size.width = 8.0f;
			dimQuad.size.height = 8.0f;
			list[layerCount++] = (const XrCompositionLayerBaseHeader *)&dimQuad;
		}
	}

	if ( layers && vr.frameActive && layers->quad && vr.canvas.released ) {
		memset( &quad, 0, sizeof( quad ) );
		quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		// the canvas holds premultiplied colour (2D is drawn over transparent black)
		quad.layerFlags = layers->quadOpaque ? 0 : XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = vr.canvas.handle;
		quad.subImage.imageRect.extent.width = (int32_t)vr.canvas.width;
		quad.subImage.imageRect.extent.height = (int32_t)vr.canvas.height;
		quad.space = VR_PlaceQuad( layers->quadMode, &layers->placement, &quad.pose );
		quad.size.width = layers->placement.width;
		quad.size.height = layers->placement.width * (float)vr.canvas.height / (float)vr.canvas.width;
		list[layerCount++] = (const XrCompositionLayerBaseHeader *)&quad;
		vr.lastQuadMode = (int)layers->quadMode;
	} else {
		vr.lastQuadMode = -1;
	}

	memset( &ei, 0, sizeof( ei ) );
	ei.type = XR_TYPE_FRAME_END_INFO;
	ei.displayTime = vr.frameState.predictedDisplayTime;
	ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	ei.layerCount = vr.frameActive ? (uint32_t)layerCount : 0;
	ei.layers = layerCount ? list : NULL;

	{
		XrResult res = qxrEndFrame( vr.session, &ei );
		if ( XR_FAILED( res ) ) {
			static int warned;
			if ( warned++ < 5 ) {
				VR_Log( 1, "VR: xrEndFrame failed: %s\n", VR_ResultString( res ) );
			}
		}
	}

	VR_FrameStats( projection, layerCount );

	vr.frameBegun = 0;
	vr.frameActive = 0;
}
