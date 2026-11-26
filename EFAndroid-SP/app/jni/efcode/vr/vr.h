/*
===========================================================================
AwayTeamEF VR: the OpenXR side of the VR build (CMake option EF_VR).

This file is part of AwayTeamEF and is licensed under the GNU General
Public License, version 2 or (at your option) any later version.
===========================================================================

The renderer (renderervk) talks to OpenXR only through this interface. Nothing
here is compiled unless EF_VR is defined, and nothing runs unless the renderer
asks for it (cvar vr_enable), so flat builds never see it.

Order of calls, all from the render thread:

  startup     VR_Init( vkGetInstanceProcAddr )     XR instance + HMD system; 0 = stay flat
              VR_CreateVulkanInstance( ... )       instead of vkCreateInstance
              VR_GetVulkanPhysicalDevice( ... )    the GPU the runtime wants
              VR_CreateVulkanDevice( ... )         instead of vkCreateDevice
              VR_SetOptions( &options )            resolution scale, refresh rate (before the session)
              VR_CreateSession( ... )              session, spaces, eye swapchains
              VR_GetEyeSwapchain( eye, &info )     the eye images, to build framebuffers on
              VR_SetCanvas( w, h, format, &info )  (re)creates the 2D layer's swapchain
  per frame   VR_BeginFrame( wantEyes, dim )       events, xrWaitFrame/xrBeginFrame, views; acquires
                                                   the eye images (wantEyes) and the canvas image
              VR_GetEyeView( eye, &view )          pose + asymmetric FOV of eye 0 (left) / 1
              VR_GetEyeImageIndex( eye )           which eye image to render into (-1: none)
              VR_GetCanvasImageIndex()             which canvas image takes the 2D pass (-1: none)
              VR_GetDimmerImageIndex( &alpha )     a dimmer image to fill this frame (-1: none)
              VR_EndFrame( &layers )               releases the images, xrEndFrame with the layers
  teardown    VR_DestroySession()                  before vkDestroyDevice
              VR_Shutdown()                        before vkDestroyInstance (also destroys
                                                   the session if still there)
*/
#ifndef EF_VR_H
#define EF_VR_H

#include "../renderercommon/vulkan/vulkan.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VR_MAX_EYES 2
#define VR_MAX_SWAPCHAIN_IMAGES 8

// pose of one eye in the session's reference space (STAGE, or LOCAL as a fallback),
// in OpenXR's own convention: metres, +X right, +Y up, -Z forward
typedef struct {
	float	position[3];
	float	orientation[4];		// quaternion x, y, z, w
	// asymmetric field of view, radians, as XrFovf: left and down are normally negative
	float	angleLeft, angleRight, angleUp, angleDown;
} vrEyeView_t;

// one OpenXR swapchain, as the renderer sees it. Acquired images are in
// VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL and must be left in that layout.
typedef struct {
	uint32_t	width, height;
	VkFormat	imageFormat;	// what the compositor reads (normally an sRGB format)
	VkFormat	viewFormat;		// what to render or copy through: the UNORM twin of an sRGB image,
								// because the game's colours are already gamma-encoded
	uint32_t	imageCount;
	VkImage		images[VR_MAX_SWAPCHAIN_IMAGES];
} vrSwapchainInfo_t;

// set before VR_CreateSession
typedef struct {
	float		resolutionScale;	// times the runtime's recommended eye size (0.5 .. 1.5)
	float		refreshRate;		// display Hz to ask for (XR_FB_display_refresh_rate); 0 = runtime default
	int			debugCapture;		// eye images readable (TRANSFER_SRC), for vr_screenshot
} vrOptions_t;

// what VR_EndFrame submits
typedef enum {
	VR_PROJ_NONE,		// no world: black (main menu, loading)
	VR_PROJ_FRESH,		// the eye images rendered this frame
	VR_PROJ_FROZEN		// the last fresh eye images again, at their own poses (menus, cutscenes)
} vrProjection_t;

typedef enum {
	VR_QUAD_HUD,		// in-game HUD: lazy-follows the head's yaw
	VR_QUAD_MENU,		// menus and console: placed in front of the head when it opens
	VR_QUAD_THEATER		// cutscenes and cinematics: a large screen, placed when it starts
} vrQuadMode_t;

typedef struct {
	float		distance;		// metres in front of the head
	float		width;			// metres; the height follows the canvas' aspect
	float		height;			// metres above (+) or below (-) eye level
	int			follow;			// 0 world-locked where placed, 1 head-locked, 2 lazy yaw follow
	float		followAngle;	// lazy follow: degrees of head yaw before the quad turns after it
} vrQuadPlacement_t;

typedef struct {
	vrProjection_t	projection;
	int				quad;			// submit the canvas (it was written this frame)
	vrQuadMode_t	quadMode;
	int				quadOpaque;		// 1 = ignore the canvas' alpha (theater)
	vrQuadPlacement_t placement;	// for quadMode
	int				dim;			// draw the dimmer between the world and the quad
} vrFrameLayers_t;

// printing: the renderer passes a function that takes one finished line
typedef void ( *vrPrintFunc_t )( int isError, const char *msg );
void		VR_SetPrint( vrPrintFunc_t print );

#ifdef __ANDROID__
// the Android shell passes the JavaVM and the activity (jobject) before the renderer starts;
// OpenXR's Android loader and instance need both
void		VR_SetAndroidContext( void *javaVM, void *activity );
#endif

// startup: load the OpenXR loader, create the XR instance and find the head-mounted display.
// Returns 0 (and stays flat) when there is no loader, no runtime or no headset.
int			VR_Init( PFN_vkGetInstanceProcAddr getInstanceProcAddr );
int			VR_Initialized( void );		// VR_Init succeeded and VR_Shutdown has not run

// the Vulkan hooks: same results as vkCreateInstance / vkCreateDevice. The runtime adds the
// extensions it needs; apiVersion is raised to what the runtime requires.
VkResult	VR_CreateVulkanInstance( const VkInstanceCreateInfo *info, VkInstance *instance );
VkPhysicalDevice VR_GetVulkanPhysicalDevice( VkInstance instance );
VkResult	VR_CreateVulkanDevice( VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *info, VkDevice *device );

void		VR_SetOptions( const vrOptions_t *options );

// session: after the device and its queue exist. Returns 0 on failure (VR stays off).
int			VR_CreateSession( VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
				uint32_t queueFamilyIndex, uint32_t queueIndex );
void		VR_DestroySession( void );
void		VR_Shutdown( void );

// the eye swapchains (valid from VR_CreateSession to VR_DestroySession)
int			VR_GetEyeSwapchain( int eye, vrSwapchainInfo_t *info );
// the 2D layer's swapchain: its images are copied from the renderer's 2D canvas (colorFormat, a
// UNORM format), so it is made the same size, in the sRGB twin of that format when the runtime
// has it. Recreated when the size changes. Returns 0 when there is no session or no such format.
int			VR_SetCanvas( uint32_t width, uint32_t height, VkFormat colorFormat, vrSwapchainInfo_t *info );
// a tiny swapchain filled with translucent black, drawn over the world while a menu or a
// cutscene is up (the runtime may have no colour-scale extension)
int			VR_GetDimmerSwapchain( vrSwapchainInfo_t *info );

// per frame. VR_BeginFrame returns 1 when the runtime wants this frame rendered (session running,
// shouldRender, views located). wantEyes = 0 skips acquiring the eye images (menus and cutscenes
// re-show the last ones); dimAlpha is the dimmer's wanted opacity. VR_EndFrame must follow every
// VR_BeginFrame (whatever it returned) once the frame's command buffer has been submitted.
int			VR_BeginFrame( int wantEyes, float dimAlpha );
int			VR_FrameActive( void );		// between a VR_BeginFrame that returned 1 and VR_EndFrame
int			VR_GetEyeView( int eye, vrEyeView_t *view );
int			VR_GetEyeImageIndex( int eye );		// -1 when no eye image is acquired this frame
int			VR_GetCanvasImageIndex( void );		// -1 when no canvas image is acquired this frame
int			VR_GetDimmerImageIndex( float *alpha );	// -1 unless the dimmer must be (re)filled now
int			VR_HaveFrozenEyes( void );			// a fresh projection has been submitted since the last reset
void		VR_EndFrame( const vrFrameLayers_t *layers );

// head-origin reset: the current head position becomes the origin of the eye offsets that
// VR_GetEyeOffset returns (done once automatically, on the first tracked frame)
void		VR_Recenter( void );
// eye position relative to the recentred head origin, OpenXR axes, metres
void		VR_GetEyeOffset( int eye, float offset[3] );

#ifdef __cplusplus
}
#endif

#endif // EF_VR_H
