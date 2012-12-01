/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2012 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_config.h"

#if SDL_VIDEO_RENDER_PSP 

#include "SDL_hints.h"
#include "../SDL_sysrender.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pspge.h>
#include <stdarg.h>
#include <stdlib.h>

#include "SDL_pspvram.h"



/* PSP renderer implementation, based on the PGE  */


extern int SDL_RecreateWindow(SDL_Window * window, Uint32 flags);


static SDL_Renderer *PSP_CreateRenderer(SDL_Window * window, Uint32 flags);
static void PSP_WindowEvent(SDL_Renderer * renderer,
                             const SDL_WindowEvent *event);
static int PSP_CreateTexture(SDL_Renderer * renderer, SDL_Texture * texture);
static int PSP_UpdateTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                              const SDL_Rect * rect, const void *pixels,
                              int pitch);
static int PSP_LockTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                            const SDL_Rect * rect, void **pixels, int *pitch);
static void PSP_UnlockTexture(SDL_Renderer * renderer,
                               SDL_Texture * texture);
static int PSP_SetRenderTarget(SDL_Renderer * renderer,
                                 SDL_Texture * texture);
static int PSP_UpdateViewport(SDL_Renderer * renderer);
static int PSP_RenderClear(SDL_Renderer * renderer);
static int PSP_RenderDrawPoints(SDL_Renderer * renderer,
                                 const SDL_FPoint * points, int count);
static int PSP_RenderDrawLines(SDL_Renderer * renderer,
                                const SDL_FPoint * points, int count);
static int PSP_RenderFillRects(SDL_Renderer * renderer,
                                const SDL_FRect * rects, int count);
static int PSP_RenderCopy(SDL_Renderer * renderer, SDL_Texture * texture,
                           const SDL_Rect * srcrect,
                           const SDL_FRect * dstrect);
static int PSP_RenderReadPixels(SDL_Renderer * renderer, const SDL_Rect * rect,
                    Uint32 pixel_format, void * pixels, int pitch);
static int PSP_RenderCopyEx(SDL_Renderer * renderer, SDL_Texture * texture,
                         const SDL_Rect * srcrect, const SDL_FRect * dstrect,
                         const double angle, const SDL_FPoint *center, const SDL_RendererFlip flip);
static void PSP_RenderPresent(SDL_Renderer * renderer);
static void PSP_DestroyTexture(SDL_Renderer * renderer,
                                SDL_Texture * texture);
static void PSP_DestroyRenderer(SDL_Renderer * renderer);

/*
SDL_RenderDriver PSP_RenderDriver = {
    PSP_CreateRenderer,
    {
     "PSP",
     (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE),
     1,
     {SDL_PIXELFORMAT_ABGR8888},
     0,
     0}
};
*/
SDL_RenderDriver PSP_RenderDriver = {
	.CreateRenderer = PSP_CreateRenderer,
    .info = {
		.name = "PSP",
		.flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE,
		.num_texture_formats = 4,
		.texture_formats = { [0] = SDL_PIXELFORMAT_BGR565,
							 					 [1] = SDL_PIXELFORMAT_ABGR1555,
							 					 [2] = SDL_PIXELFORMAT_ABGR4444,
							 					 [3] = SDL_PIXELFORMAT_ABGR8888,
		},
		.max_texture_width = 512,
		.max_texture_height = 512,
     }
};

#define PSP_SCREEN_WIDTH	480
#define PSP_SCREEN_HEIGHT	272

#define PSP_FRAME_BUFFER_WIDTH	512
#define PSP_FRAME_BUFFER_SIZE	(PSP_FRAME_BUFFER_WIDTH*PSP_SCREEN_HEIGHT)

static unsigned int __attribute__((aligned(64))) DisplayList[2][262144/4];
static const ScePspIMatrix4 DitherMatrix =	{{0,  8,  2, 10},
																						 {12,  4, 14,  6},
																						 {3,  11,  1,  9},
																						 {15,  7, 13,  5}};
																						 	
#define COL5650(r,g,b,a)	((r>>3) | ((g>>2)<<5) | ((b>>3)<<11))
#define COL5551(r,g,b,a)	((r>>3) | ((g>>3)<<5) | ((b>>3)<<10) | (a>0?0x7000:0))
#define COL4444(r,g,b,a)	((r>>4) | ((g>>4)<<4) | ((b>>4)<<8) | ((a>>4)<<12))
#define COL8888(r,g,b,a)	((r) | ((g)<<8) | ((b)<<16) | ((a)<<24))
	

typedef struct
{
	    
	unsigned int displayListId ;	
	void* frontbuffer ;
	void* depthbuffer ;
	void* backbuffer ;
	void* framebuffer ;
	unsigned int initialized ;
	unsigned int displayListAvail ;
	unsigned int psm ;
	unsigned int bpp ;
	
	unsigned int vsync;
	
	unsigned int currentColor;
	int 				 currentBlendMode;
		
} PSP_RenderData;

enum MemoryLocation
{
	PSP_RAM,
	PSP_VRAM
};

typedef struct
{
	void						*data;				/**< Image data. */
	unsigned int		size;								/**< Size of data in bytes. */
	unsigned int		width;							/**< Image width. */
	unsigned int		height;							/**< Image height. */
	unsigned int		textureWidth;				/**< Texture width (power of two). */
	unsigned int		textureHeight;			/**< Texture height (power of two). */
	unsigned int		bits;								/**< Image bits per pixel. */
	unsigned int		format;							/**< Image format - one of ::pgePixelFormat. */
	char						swizzled;							/**< Is image swizzled. */
	enum MemoryLocation	location;					/**< One of ::pgeMemoryLocation. */
	
} PSP_TextureData;

typedef struct
{	
	float	x, y, z;
} VertV;


typedef struct
{
	float	u, v;
	float	x, y, z;
	
} VertTV;

/*
static void debugOut(const char *text)
{
	int fd = sceIoOpen("debug.txt", PSP_O_WRONLY|PSP_O_CREAT|PSP_O_APPEND, 0777);
	
	sceIoWrite(fd, text, strlen(text));
	
	sceIoClose(fd);
}
*/

// Return next power of 2
static int 
TextureNextPow2(unsigned int w)
{
		if(w == 0)
			return 0;
	
		unsigned int n = 2;
	
		while(w > n)
			n <<= 1;
	
		return n;
}

// Return next multiple of 8 (needed for swizzling)
static int 
TextureNextMul8(unsigned int w)
{
		return((w+7)&~0x7);
}

static int
GetScaleQuality(void)
{
    const char *hint = SDL_GetHint(SDL_HINT_RENDER_SCALE_QUALITY);

    if (!hint || *hint == '0' || SDL_strcasecmp(hint, "nearest") == 0) {
        return GU_NEAREST; // GU_NEAREST good for tile-map
    } else {
        return GU_LINEAR; // GU_LINEAR good for scaling
    }
}

static int
PixelFormatToPSPFMT(Uint32 format)
{
    switch (format) {
    case SDL_PIXELFORMAT_BGR565:
        return GU_PSM_5650;
    case SDL_PIXELFORMAT_ABGR1555:
        return GU_PSM_5551;
    case SDL_PIXELFORMAT_ABGR4444:
        return GU_PSM_4444;
    case SDL_PIXELFORMAT_ABGR8888:
        return GU_PSM_8888;        
    default:    	
        return GU_PSM_8888;
    }
}
/*
static Uint32
PSPFMTToPixelFormat(int format)
{
    switch (format) {
    case GU_PSM_5650:
        return SDL_PIXELFORMAT_BGR565;
    case GU_PSM_5551:
        return SDL_PIXELFORMAT_ABGR1555;
    case GU_PSM_4444:
        return SDL_PIXELFORMAT_ABGR4444;
    case GU_PSM_8888:
        return SDL_PIXELFORMAT_ABGR8888; 
    default:
        return SDL_PIXELFORMAT_ABGR8888;
    }
}
*/

void 
StartDrawing(SDL_Renderer * renderer)
{	
	PSP_RenderData *data = (PSP_RenderData *) renderer->driverdata;
	if(data->displayListAvail)
		return;

	sceGuStart(GU_DIRECT, DisplayList[data->displayListId]);
	data->displayListId ^= 1;
	data->displayListAvail = 1;
}

void 
EndDrawing(SDL_Renderer * renderer)
{
	PSP_RenderData *data = (PSP_RenderData *) renderer->driverdata;
	if(!data->displayListAvail)
		return;
	
	data->displayListAvail = 0;
	sceGuFinish();
	sceGuSync(0,0);
}

int 
TextureSwizzle(SDL_Texture *texture)
{
	PSP_TextureData *psp_texture = (PSP_TextureData *) texture->driverdata;
	if(psp_texture->swizzled)
		return 1;
	
	int bytewidth = psp_texture->textureWidth*(psp_texture->bits>>3);
	int height = psp_texture->size / bytewidth;

	int rowblocks = (bytewidth>>4);
	int rowblocksadd = (rowblocks-1)<<7;
	unsigned int blockaddress = 0;
	unsigned int *src = (unsigned int*) psp_texture->data;

	unsigned char *data = NULL;
	
	if(psp_texture->location == PSP_VRAM)
		data = (void*) VramAlloc(psp_texture->size);
	else
		data = malloc(psp_texture->size);


	int j;

	for(j = 0; j < height; j++, blockaddress += 16)
	{
		unsigned int *block;

		if(psp_texture->location == PSP_VRAM)
			block = (unsigned int*)((unsigned int)&data[blockaddress]|0x40000000);
		else
			block = (unsigned int*)&data[blockaddress];

		int i;

		for(i = 0; i < rowblocks; i++)
		{
			*block++ = *src++;
			*block++ = *src++;
			*block++ = *src++;
			*block++ = *src++;
			block += 28;
		}

		if((j & 0x7) == 0x7)
			blockaddress += rowblocksadd;
	}

	if(psp_texture->location == PSP_VRAM)
		VramFree(psp_texture->data);
	else
		free(psp_texture->data);
	
	psp_texture->data = data;

	psp_texture->swizzled = 1;

	return 1;
}

SDL_Renderer *
PSP_CreateRenderer(SDL_Window * window, Uint32 flags)
{

    SDL_Renderer *renderer;
    PSP_RenderData *data;
		int pixelformat;
    renderer = (SDL_Renderer *) SDL_calloc(1, sizeof(*renderer));
    if (!renderer) {
        SDL_OutOfMemory();
        return NULL;
    }

    data = (PSP_RenderData *) SDL_calloc(1, sizeof(*data));
    if (!data) {
        PSP_DestroyRenderer(renderer);
        SDL_OutOfMemory();
        return NULL;
    }
    
		
    renderer->WindowEvent = PSP_WindowEvent;
    renderer->CreateTexture = PSP_CreateTexture;
    renderer->UpdateTexture = PSP_UpdateTexture;
    renderer->LockTexture = PSP_LockTexture;
    renderer->UnlockTexture = PSP_UnlockTexture;
    renderer->SetRenderTarget = PSP_SetRenderTarget;
    renderer->UpdateViewport = PSP_UpdateViewport;
    renderer->RenderClear = PSP_RenderClear;
    renderer->RenderDrawPoints = PSP_RenderDrawPoints;
    renderer->RenderDrawLines = PSP_RenderDrawLines;
    renderer->RenderFillRects = PSP_RenderFillRects;
    renderer->RenderCopy = PSP_RenderCopy;
    renderer->RenderReadPixels = PSP_RenderReadPixels;
    renderer->RenderCopyEx = PSP_RenderCopyEx;
    renderer->RenderPresent = PSP_RenderPresent;
    renderer->DestroyTexture = PSP_DestroyTexture;
    renderer->DestroyRenderer = PSP_DestroyRenderer;
    renderer->info = PSP_RenderDriver.info;
    renderer->info.flags = SDL_RENDERER_ACCELERATED;
    renderer->driverdata = data;
    renderer->window = window;
    
		if (data->initialized != 0)
			return 0;
		data->initialized = 1;
		
    if (flags & SDL_RENDERER_PRESENTVSYNC) {
        data->vsync = 1;
    } else {
        data->vsync = 0;
    }
    	
		pixelformat=PixelFormatToPSPFMT(SDL_GetWindowPixelFormat(window));
		switch(pixelformat)
		{
			case GU_PSM_4444:
			case GU_PSM_5650:
			case GU_PSM_5551:
				data->frontbuffer = VramRelativePointer(VramAlloc(PSP_FRAME_BUFFER_SIZE<<1));
				data->backbuffer = VramRelativePointer(VramAlloc(PSP_FRAME_BUFFER_SIZE<<1));
				data->depthbuffer = VramRelativePointer(VramAlloc(PSP_FRAME_BUFFER_SIZE<<1));
				data->bpp = 2;
				data->psm = pixelformat;
				break;
			default:
				data->frontbuffer = VramRelativePointer(VramAlloc(PSP_FRAME_BUFFER_SIZE<<2));
				data->backbuffer = VramRelativePointer(VramAlloc(PSP_FRAME_BUFFER_SIZE<<2));
				data->depthbuffer = VramRelativePointer(VramAlloc(PSP_FRAME_BUFFER_SIZE<<1));
				data->bpp = 4;
				data->psm = GU_PSM_8888;
				break;
		}
		data->framebuffer = VramAbsolutePointer(data->frontbuffer);
		
		sceGuInit();
		// setup GU
		sceGuStart(GU_DIRECT, DisplayList[data->displayListId]);
		sceGuDrawBuffer(data->psm, data->frontbuffer, PSP_FRAME_BUFFER_WIDTH);
		sceGuDispBuffer(512, 512, data->backbuffer, PSP_FRAME_BUFFER_WIDTH);
		sceGuDepthBuffer(data->depthbuffer, PSP_FRAME_BUFFER_WIDTH);
	
		sceGuOffset(2048 - (PSP_SCREEN_WIDTH>>1), 2048 - (PSP_SCREEN_HEIGHT>>1));
		sceGuViewport(2048, 2048, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
	
		// Scissoring
		sceGuScissor(0, 0, PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
		sceGuEnable(GU_SCISSOR_TEST);
	
		// Backface culling
		sceGuFrontFace(GU_CCW);
		sceGuEnable(GU_CULL_FACE);
	
		// Depth test
		sceGuDepthRange(65535, 0);
		sceGuDepthFunc(GU_GEQUAL);
		sceGuDisable(GU_DEPTH_TEST);
		//sceGuDepthMask(GU_TRUE);		// disable z-writes
	
		sceGuEnable(GU_CLIP_PLANES);
	
		// Texturing
		sceGuEnable(GU_TEXTURE_2D);
		sceGuShadeModel(GU_SMOOTH);
		sceGuTexWrap(GU_REPEAT, GU_REPEAT);
		sceGuTexFilter(GU_LINEAR,GU_LINEAR);
		sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
		sceGuTexEnvColor(0xFFFFFFFF);
		sceGuColor(0xFFFFFFFF);
		sceGuAmbientColor(0xFFFFFFFF);
		sceGuTexOffset(0.0f, 0.0f);
		sceGuTexScale(1.0f, 1.0f);

		// Blending
		sceGuEnable(GU_BLEND);
		sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	/*
		if(data->bpp < 4)
		{
			sceGuSetDither(&DitherMatrix);
			sceGuEnable(GU_DITHER);
		}
	*/
		// Projection
		sceGumMatrixMode(GU_PROJECTION);
		sceGumLoadIdentity();
		sceGumPerspective(60.0f, 480.0f/272.0f, 1.0f, 1000.0f);
		
		sceGumMatrixMode(GU_VIEW);
		sceGumLoadIdentity();
		
		sceGumMatrixMode(GU_MODEL);
		sceGumLoadIdentity();
		
		sceGuClearColor(0x0);
		sceGuClear(GU_COLOR_BUFFER_BIT);
		
		sceGuFinish();
		sceGuSync(0,0);
	
		sceDisplayWaitVblankStartCB();
		data->framebuffer = VramAbsolutePointer(sceGuSwapBuffers());
		sceGuDisplay(1);	
		
    return renderer;
}

static void
PSP_WindowEvent(SDL_Renderer * renderer, const SDL_WindowEvent *event)
{

}


static int
PSP_CreateTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
//		PSP_RenderData *renderdata = (PSP_RenderData *) renderer->driverdata;
		PSP_TextureData* psp_texture = (PSP_TextureData*) malloc(sizeof(PSP_TextureData));
		int location = PSP_RAM;
		if(!psp_texture)
			return -1;
	
		psp_texture->swizzled = 0;
		psp_texture->width = texture->w;
		psp_texture->height = texture->h;
		psp_texture->textureHeight = TextureNextPow2(texture->h);
		psp_texture->textureWidth = TextureNextPow2(texture->w);
		psp_texture->format = PixelFormatToPSPFMT(texture->format);
		psp_texture->location = location;

	
		switch(psp_texture->format)
		{
			case GU_PSM_5650:
			case GU_PSM_5551:
			case GU_PSM_4444:
		//	case GU_PSM_T16:
				psp_texture->bits = 16;
				break;
				
			case GU_PSM_8888:
		//	case GU_PSM_T32:
				psp_texture->bits = 32;
				break;
			/*
			case GU_PSM_T8:
				psp_texture->bits = 8;
				break;
	
			case GU_PSM_T4:
				psp_texture->bits = 4;
				break;
			*/
			default:
				return -1;
		}
	
		psp_texture->size = psp_texture->textureWidth*TextureNextMul8(psp_texture->height)*(psp_texture->bits>>3);
	
		if(location == PSP_RAM)
		{
			psp_texture->data = malloc(psp_texture->size);
			
			if(!psp_texture->data)
			{
				free(psp_texture);
				return -1;
			}
		}
		else
		{
			psp_texture->data = VramAlloc(psp_texture->size);
			
			if(!psp_texture->data)
			{
				free(psp_texture);
				return -1;
			}
		}
	
		memset(psp_texture->data, 0xFF, psp_texture->size);

    texture->driverdata = psp_texture;
    
    return 0;
}

static int TextureMode = 3;

void 
TextureActivate(SDL_Texture * texture)
{		
	PSP_TextureData *psp_texture = (PSP_TextureData *) texture->driverdata;
	int scaleMode = GetScaleQuality();
	
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexWrap(GU_REPEAT, GU_REPEAT);
//	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexFilter(scaleMode, scaleMode); // GU_NEAREST good for tile-map
																			  // GU_LINEAR good for scaling
	sceGuTexFunc(TextureMode, GU_TCC_RGBA);
	sceGuTexEnvColor(0xFFFFFFFF);
	sceGuColor(0xFFFFFFFF);
	sceGuAmbientColor(0xFFFFFFFF);
	sceGuTexOffset(0.0f, 0.0f);
	sceGuTexScale(1.0f/(float)psp_texture->textureWidth, 1.0f/(float)psp_texture->textureHeight);

	sceGuTexMode(psp_texture->format, 0, 0, psp_texture->swizzled);
	sceGuTexImage(0, psp_texture->textureWidth, psp_texture->textureHeight, psp_texture->textureWidth, psp_texture->data);
}


static int
PSP_UpdateTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                   const SDL_Rect * rect, const void *pixels, int pitch)
{
		PSP_TextureData *psp_texture = (PSP_TextureData *) texture->driverdata;

 		char *new_pixels = NULL;
    int w = TextureNextPow2(rect->w);
    int h = TextureNextPow2(rect->h);
		
    if (rect->w <= 0 || rect->h <= 0)
        return 0;
        
		if (w != rect->w || h != rect->h) {
			/// Allocate a temporary surface and copy pixels into it while
			//	 enlarging the pitch. 
			const char * src;
			char *dst;
			int new_pitch = (psp_texture->bits>>3) * w;
			int i;
	
			new_pixels = malloc( w*TextureNextMul8(rect->h)*(psp_texture->bits>>3));
	//		memset(texture->data, 0xFF, texture->size);
			
			if (!new_pixels)
				return SDL_ENOMEM;
	
			src = pixels;
			dst = new_pixels;
			for (i=0; i<rect->h; i++) {
				memcpy(dst, src, pitch);
				src += pitch;
				dst += new_pitch;
			}
		}

/*
	if(swizzle)
	{
		if(!TextureSwizzle(texture))
		{
			debugOut("14\n");
			free(texture);
			return -1;
		}
	}
*/


		psp_texture->width = rect->w;							
		psp_texture->height = rect->h;							
		psp_texture->textureWidth = TextureNextPow2(rect->w);				
		psp_texture->textureHeight = TextureNextPow2(rect->h);
		psp_texture->data= (void*)(new_pixels? new_pixels : pixels);


		sceKernelDcacheWritebackAll();
	
  	TextureSwizzle(texture);

		sceKernelDcacheWritebackAll();
	
		if (new_pixels)
			free(new_pixels);

		
    return 0;
}

static int
PSP_LockTexture(SDL_Renderer * renderer, SDL_Texture * texture,
                 const SDL_Rect * rect, void **pixels, int *pitch)
{

    return 0;
}

static void
PSP_UnlockTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{

}

static int
PSP_SetRenderTarget(SDL_Renderer * renderer, SDL_Texture * texture)
{

    return 0;
}

static int
PSP_UpdateViewport(SDL_Renderer * renderer)
{
/*
    sceGuViewport(renderer->viewport.x, renderer->viewport.y,
               renderer->viewport.w, renderer->viewport.h);

    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumOrtho(0.0f,renderer->viewport.w,renderer->viewport.h, 0.0f, 0.0, 1.0);
*/
    return 0;
}
/*
static int
PSP_SetColor(SDL_Renderer * renderer)
{
		PSP_RenderData *data = (PSP_RenderData *) renderer->driverdata;	
    Uint32 color;
    Uint8 r = renderer->r;
    Uint8 g = renderer->g;
    Uint8 b = renderer->b;
    Uint8 a = renderer->a;
		switch(data->psm)
		{
			case GU_PSM_4444:
				color = ((r>>4) | ((g>>4)<<4) | ((b>>4)<<8) | ((a>>4)<<12));
				break;								
			case GU_PSM_5650:
				color = ((r>>3) | ((g>>2)<<5) | ((b>>3)<<11));
				break;								
			case GU_PSM_5551:
				color = ((r>>3) | ((g>>3)<<5) | ((b>>3)<<10) | (a>0?0x7000:0));
				break;				
      case GU_PSM_8888:	
      	color = ((r) | ((g)<<8) | ((b)<<16) | ((a)<<24));			
      	break;      	
			default:
				color = ((r) | ((g)<<8) | ((b)<<16) | ((a)<<24));
				break;
		}		
//		sceGuColor(color);
		if (color != data->currentColor){
    		data->currentColor = color;
  	}
  	return color;
}


static void
PSP_SetBlendMode(SDL_Renderer * renderer, int blendMode)
{
		PSP_RenderData *data = (PSP_RenderData *) renderer->driverdata;	
			
    if (blendMode != data-> currentBlendMode) {
        switch (blendMode) {
        case SDL_BLENDMODE_NONE:
        		sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);        
						sceGuDisable(GU_BLEND);
            break;
        case SDL_BLENDMODE_BLEND:
        		sceGuTexFunc(GU_TFX_MODULATE , GU_TCC_RGBA);
						sceGuEnable(GU_BLEND);
						sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0 );
            break;
        case SDL_BLENDMODE_ADD:
        		sceGuTexFunc(GU_TFX_MODULATE , GU_TCC_RGBA);
						sceGuEnable(GU_BLEND);
						sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0x00FFFFFF );
            break;
        case SDL_BLENDMODE_MOD:
        		sceGuTexFunc(GU_TFX_MODULATE , GU_TCC_RGBA);
						sceGuEnable(GU_BLEND);
						sceGuBlendFunc( GU_ADD, GU_FIX, GU_SRC_COLOR, 0, 0);
            break;
        }
        data->currentBlendMode = blendMode;
    }
}
*/


static int
PSP_RenderClear(SDL_Renderer * renderer)
{					
		//start list
		StartDrawing(renderer);
		int color = renderer->a << 24 | renderer->b << 16 | renderer->g << 8 | renderer->r;		
					//clear screen fixed
		sceGuClearColor(color);
		sceGuClearDepth(0);
		sceGuClear(GU_COLOR_BUFFER_BIT|GU_DEPTH_BUFFER_BIT|GU_FAST_CLEAR_BIT);

    return 0;
}

static int
PSP_RenderDrawPoints(SDL_Renderer * renderer, const SDL_FPoint * points,
                      int count)
{
		int color = renderer->a << 24 | renderer->b << 16 | renderer->g << 8 | renderer->r;
		int i;
		StartDrawing(renderer);		
		VertV* vertices = (VertV*)sceGuGetMemory(count*sizeof(VertV)); 
		
    for (i = 0; i < count; ++i) {
				vertices[i].x = points[i].x;
				vertices[i].y = points[i].y;
				vertices[i].z = 0.0f;
		}
		sceGuDisable(GU_TEXTURE_2D);
		sceGuColor(color);		
		sceGuShadeModel(GU_FLAT);
		sceGuDrawArray(GU_POINTS, GU_VERTEX_32BITF|GU_TRANSFORM_2D, count, 0, vertices);
		sceGuShadeModel(GU_SMOOTH);
		sceGuEnable(GU_TEXTURE_2D);			

    return 0;
}

static int
PSP_RenderDrawLines(SDL_Renderer * renderer, const SDL_FPoint * points,
                     int count)
{
		int color = renderer->a << 24 | renderer->b << 16 | renderer->g << 8 | renderer->r;
		int i;
		StartDrawing(renderer);
		VertV* vertices = (VertV*)sceGuGetMemory(count*sizeof(VertV)); 
		
    for (i = 0; i < count; ++i) {
				vertices[i].x = points[i].x;
				vertices[i].y = points[i].y;
				vertices[i].z = 0.0f;
		}

		sceGuDisable(GU_TEXTURE_2D);
		sceGuColor(color);
		sceGuShadeModel(GU_FLAT);
		sceGuDrawArray(GU_LINE_STRIP, GU_VERTEX_32BITF|GU_TRANSFORM_2D, count, 0, vertices);
		sceGuShadeModel(GU_SMOOTH);
		sceGuEnable(GU_TEXTURE_2D);
	 
    return 0;
}

static int
PSP_RenderFillRects(SDL_Renderer * renderer, const SDL_FRect * rects,
                     int count)
{
		int color = renderer->a << 24 | renderer->b << 16 | renderer->g << 8 | renderer->r;
		int i;	
		StartDrawing(renderer);
		
    for (i = 0; i < count; ++i) {
        const SDL_FRect *rect = &rects[i];
				VertV* vertices = (VertV*)sceGuGetMemory((sizeof(VertV)<<1));
				vertices[0].x = rect->x;
				vertices[0].y = rect->y;
				vertices[0].z = 0.0f;
		
				vertices[1].x = rect->x + rect->w;
				vertices[1].y = rect->y + rect->h;
				vertices[1].z = 0.0f;
				
				sceGuDisable(GU_TEXTURE_2D);
				sceGuColor(color);			
				sceGuShadeModel(GU_FLAT);
				sceGuDrawArray(GU_SPRITES, GU_VERTEX_32BITF|GU_TRANSFORM_2D, 2, 0, vertices);
				sceGuShadeModel(GU_SMOOTH);
				sceGuEnable(GU_TEXTURE_2D);
    }
		
    return 0;
}


#define PI   3.14159265358979f

#define radToDeg(x) ((x)*180.f/PI)
#define degToRad(x) ((x)*PI/180.f)

float MathAbs(float x)
{
	float result;

	__asm__ volatile (
		"mtv      %1, S000\n"
		"vabs.s   S000, S000\n"
		"mfv      %0, S000\n"
	: "=r"(result) : "r"(x));

	return result;
}

void MathSincos(float r, float *s, float *c)
{
	__asm__ volatile (
		"mtv      %2, S002\n"
		"vcst.s   S003, VFPU_2_PI\n"
		"vmul.s   S002, S002, S003\n"
		"vrot.p   C000, S002, [s, c]\n"
		"mfv      %0, S000\n"
		"mfv      %1, S001\n"
	: "=r"(*s), "=r"(*c): "r"(r));
}

void Swap(float *a, float *b)
{
	float n=*a;
	*a = *b;
	*b = n;
}

static int
PSP_RenderCopy(SDL_Renderer * renderer, SDL_Texture * texture,
                const SDL_Rect * srcrect, const SDL_FRect * dstrect)
{
		float x, y, width, height;
		float u0, v0, u1, v1;
		unsigned char alpha;
		
		x = dstrect->x;
		y = dstrect->y;
		width = dstrect->w;
		height = dstrect->h;
	
		u0 = srcrect->x;
		v0 = srcrect->y;
		u1 = srcrect->x + srcrect->w;
		v1 = srcrect->y + srcrect->h;
		
		alpha = texture->a;
		
		StartDrawing(renderer);
		TextureActivate(texture);
		
//		pgeGfxSetBlendMode(PGE_BLEND_MODE_TRANSPARENT);		
//		sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
//		sceGuColor(color);

//		PSP_SetBlendMode(renderer, renderer->blendMode);
//		PSP_SetColor(renderer);
		if(alpha != 255)
		{
			sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
			sceGuColor(GU_RGBA(255, 255, 255, alpha));
		}

		if((MathAbs(u1) - MathAbs(u0)) < 64.0f)
		{
			VertTV* vertices = (VertTV*)sceGuGetMemory((sizeof(VertTV))<<1);

			vertices[0].u = u0;
			vertices[0].v = v0;
			vertices[0].x = x;
			vertices[0].y = y; 
			vertices[0].z = 0;

			vertices[1].u = u1;
			vertices[1].v = v1;
			vertices[1].x = x + width;
			vertices[1].y = y + height;
			vertices[1].z = 0;

			sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF|GU_VERTEX_32BITF|GU_TRANSFORM_2D, 2, 0, vertices);
		}
		else
		{
			float start, end;
			float curU = u0;
			float curX = x;
			float endX = x + width;
			float slice = 64.0f;
			float ustep = (u1 - u0)/width * slice;
		
			if(ustep < 0.0f)
				ustep = -ustep;

			for(start = 0, end = width; start < end; start += slice)
			{
				VertTV* vertices = (VertTV*)sceGuGetMemory((sizeof(VertTV))<<1);

				float polyWidth = ((curX + slice) > endX) ? (endX - curX) : slice;
				float sourceWidth = ((curU + ustep) > u1) ? (u1 - curU) : ustep;

				vertices[0].u = curU;
				vertices[0].v = v0;
				vertices[0].x = curX;
				vertices[0].y = y; 
				vertices[0].z = 0;

				curU += sourceWidth;
				curX += polyWidth;

				vertices[1].u = curU;
				vertices[1].v = v1;
				vertices[1].x = curX;
				vertices[1].y = (y + height);
				vertices[1].z = 0;

				sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF|GU_VERTEX_32BITF|GU_TRANSFORM_2D, 2, 0, vertices);
			}
		}
		
		if(alpha != 255)
			sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    return 0;
}

static int
PSP_RenderReadPixels(SDL_Renderer * renderer, const SDL_Rect * rect,
                    Uint32 pixel_format, void * pixels, int pitch)

{
		return 0;
}


static int
PSP_RenderCopyEx(SDL_Renderer * renderer, SDL_Texture * texture,
                const SDL_Rect * srcrect, const SDL_FRect * dstrect,
                const double angle, const SDL_FPoint *center, const SDL_RendererFlip flip)
{
		float x, y, width, height;
		float u0, v0, u1, v1;
		unsigned char alpha;
		float centerx, centery;
		
		x = dstrect->x;
		y = dstrect->y;
		width = dstrect->w;
		height = dstrect->h;
	
		u0 = srcrect->x;
		v0 = srcrect->y;
		u1 = srcrect->x + srcrect->w;
		v1 = srcrect->y + srcrect->h;
		
    centerx = center->x;
    centery = center->y;
    		
		alpha = texture->a;
		
		StartDrawing(renderer);
		TextureActivate(texture);
		
//		pgeGfxSetBlendMode(PGE_BLEND_MODE_TRANSPARENT);		
//		sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
//		sceGuColor(color);

//		PSP_SetBlendMode(renderer, renderer->blendMode);
//		PSP_SetColor(renderer);
		if(alpha != 255)
		{
			sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
			sceGuColor(GU_RGBA(255, 255, 255, alpha));
		}

//		x += width * 0.5f;
//		y += height * 0.5f;
		x += centerx;
		y += centery;		
		
		float c, s;
		
		MathSincos(degToRad(angle), &s, &c);
		
//		width *= 0.5f;
//		height *= 0.5f;
		width  -= centerx;
		height -= centery;		
		
		
		float cw = c*width;
		float sw = s*width;
		float ch = c*height;
		float sh = s*height;

		VertTV* vertices = (VertTV*)sceGuGetMemory(sizeof(VertTV)<<2);

		vertices[0].u = u0;
		vertices[0].v = v0;
		vertices[0].x = x - cw + sh;
		vertices[0].y = y - sw - ch;
		vertices[0].z = 0;
				
		vertices[1].u = u0;
		vertices[1].v = v1;
		vertices[1].x = x - cw - sh;
		vertices[1].y = y - sw + ch;
		vertices[1].z = 0;
		
		vertices[2].u = u1;
		vertices[2].v = v1;
		vertices[2].x = x + cw - sh;
		vertices[2].y = y + sw + ch;
		vertices[2].z = 0;
		
		vertices[3].u = u1;
		vertices[3].v = v0;
		vertices[3].x = x + cw + sh;
		vertices[3].y = y + sw - ch;
		vertices[3].z = 0;
		
    if (flip & SDL_FLIP_HORIZONTAL) {
				Swap(&vertices[0].v, &vertices[2].v);
				Swap(&vertices[1].v, &vertices[3].v);
  	}
    if (flip & SDL_FLIP_VERTICAL) {
				Swap(&vertices[0].u, &vertices[2].u);
				Swap(&vertices[1].u, &vertices[3].u);
  	}  	

		sceGuDrawArray(GU_TRIANGLE_FAN, GU_TEXTURE_32BITF|GU_VERTEX_32BITF|GU_TRANSFORM_2D, 4, 0, vertices);
				
		if(alpha != 255)
			sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
			
    return 0;
}

static void
PSP_RenderPresent(SDL_Renderer * renderer)
{
    PSP_RenderData *data = (PSP_RenderData *) renderer->driverdata;
    EndDrawing(renderer);
		if(data->vsync)
			sceDisplayWaitVblankStart();
	
		data->framebuffer = VramAbsolutePointer(sceGuSwapBuffers());
		
}

static void
PSP_DestroyTexture(SDL_Renderer * renderer, SDL_Texture * texture)
{
		PSP_RenderData *renderdata = (PSP_RenderData *) renderer->driverdata;
		PSP_TextureData *psp_texture = (PSP_TextureData *) texture->driverdata;
		
    if (renderdata == 0)
    	return;
    
		if(psp_texture == 0)
			return;
	
		if(psp_texture->data != 0)
		{
			if(psp_texture->location == PSP_VRAM)
				VramFree(psp_texture->data);
			else
				free(psp_texture->data);
		}
		free(texture);
		texture->driverdata = NULL;
}

static void
PSP_DestroyRenderer(SDL_Renderer * renderer)
{
    PSP_RenderData *data = (PSP_RenderData *) renderer->driverdata;
    if (data) {
				if (!data->initialized)
					return;
					
				StartDrawing(renderer);
				
				VramFree(VramAbsolutePointer(data->backbuffer));
				VramFree(VramAbsolutePointer(data->frontbuffer));
				data->initialized = 0;
				data->displayListAvail = 0;
				sceGuTerm();
        SDL_free(data);
    }
    SDL_free(renderer);
}

#endif /* SDL_VIDEO_RENDER_PSP */

/* vi: set ts=4 sw=4 expandtab: */

