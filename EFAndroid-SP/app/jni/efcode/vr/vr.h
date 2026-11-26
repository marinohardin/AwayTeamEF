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
              VR_CreateSession( ... )              session, spaces, eye swapchains
  per frame   VR_BeginFrame()                      events, xrWaitFrame/xrBeginFrame, views,
                                                   acquires the eye images; 1 = render stereo
              VR_GetEyeView( eye, &view )          pose + asymmetric FOV of eye 0 (left) / 1
              VR_GetEyeImage( eye, &image )        the VkImage to fill for that eye
              VR_EndFrame( written )               releases the images, xrEndFrame
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

// pose of one eye in the session's reference space (STAGE, or LOCAL as a fallback),
// in OpenXR's own convention: metres, +X right, +Y up, -Z forward
typedef struct {
	float	position[3];
	float	orientation[4];		// quaternion x, y, z, w
	// asymmetric field of view, radians, as XrFovf: left and down are normally negative
	float	angleLeft, angleRight, angleUp, angleDown;
} vrEyeView_t;

typedef struct {
	VkImage		image;			// in VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; leave it in that layout
	VkFormat	format;
	uint32_t	width, height;
} vrEyeImage_t;

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

// session: after the device and its queue exist. Returns 0 on failure (VR stays off).
int			VR_CreateSession( VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
				uint32_t queueFamilyIndex, uint32_t queueIndex );
void		VR_DestroySession( void );
void		VR_Shutdown( void );

// per frame. VR_BeginFrame returns 1 when this frame's eyes are to be rendered: the session is
// running, the runtime wants the frame and both eye images are acquired. VR_EndFrame must follow
// every VR_BeginFrame (whatever it returned) once the frame's command buffer has been submitted;
// eyesWritten = 0 when the renderer dropped the frame (the runtime then gets no layers).
int			VR_BeginFrame( void );
int			VR_FrameActive( void );		// between a VR_BeginFrame that returned 1 and VR_EndFrame
int			VR_GetEyeView( int eye, vrEyeView_t *view );
int			VR_GetEyeImage( int eye, vrEyeImage_t *image );
void		VR_EndFrame( int eyesWritten );

// head-origin reset: the current head position becomes the origin of the eye offsets that
// VR_GetEyeOffset returns (done once automatically, on the first tracked frame)
void		VR_Recenter( void );
// eye position relative to the recentred head origin, OpenXR axes, metres
void		VR_GetEyeOffset( int eye, float offset[3] );

#ifdef __cplusplus
}
#endif

#endif // EF_VR_H
