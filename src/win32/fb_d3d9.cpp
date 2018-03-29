/*
** fb_d3d9.cpp
** Code to let ZDoom use Direct3D 9 as a simple framebuffer
**
**---------------------------------------------------------------------------
** Copyright 1998-2011 Randy Heit
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
** This file does _not_ implement hardware-acclerated 3D rendering. It is
** just a means of getting the pixel data to the screen in a more reliable
** method on modern hardware by copying the entire frame to a texture,
** drawing that to the screen, and presenting.
**
** That said, it does implement hardware-accelerated 2D rendering.
*/

// HEADER FILES ------------------------------------------------------------

#ifdef _DEBUG
#define D3D_DEBUG_INFO
#endif
#define DIRECT3D_VERSION 0x0900
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>

#include <stdio.h>

#include "doomtype.h"

#include "c_dispatch.h"
#include "templates.h"
#include "i_system.h"
#include "i_video.h"
#include "i_input.h"
#include "v_video.h"
#include "v_pfx.h"
#include "stats.h"
#include "doomerrors.h"
#include "r_data/r_translate.h"
#include "f_wipe.h"
#include "sbar.h"
#include "win32iface.h"
#include "win32swiface.h"
#include "doomstat.h"
#include "v_palette.h"
#include "w_wad.h"
#include "textures.h"
#include "r_data/colormaps.h"
#include "SkylineBinPack.h"
#include "swrenderer/scene/r_light.h"

// MACROS ------------------------------------------------------------------

// The number of points for the vertex buffer.
#define NUM_VERTS		10240

// The number of indices for the index buffer.
#define NUM_INDEXES		((NUM_VERTS * 6) / 4)

// The number of quads we can batch together.
#define MAX_QUAD_BATCH	(NUM_INDEXES / 6)

// TYPES -------------------------------------------------------------------

class D3DTex : public FNativeTexture
{
public:
	D3DTex(FTexture *tex, FTextureFormat fmt, D3DFB *fb, bool wrapping);
	~D3DTex();

	//D3DFB::PackedTexture *Box;

	IDirect3DTexture9 *Tex;
	D3DFORMAT Format;


	D3DTex **Prev;
	D3DTex *Next;

	bool IsGray;

	bool Create(D3DFB *fb, bool wrapping);
	bool Update();
	bool CheckWrapping(bool wrapping);
	D3DFORMAT GetTexFormat();
};

class D3DPal : public FNativePalette
{
public:
	D3DPal(FRemapTable *remap, D3DFB *fb);
	~D3DPal();

	D3DPal **Prev;
	D3DPal *Next;

	IDirect3DTexture9 *Tex;
	D3DCOLOR BorderColor;

	bool Update();

	FRemapTable *Remap;
	int RoundedPaletteSize;
};

// EXTERNAL FUNCTION PROTOTYPES --------------------------------------------

// PUBLIC FUNCTION PROTOTYPES ----------------------------------------------

void DoBlending (const PalEntry *from, PalEntry *to, int count, int r, int g, int b, int a);

// PRIVATE FUNCTION PROTOTYPES ---------------------------------------------

// EXTERNAL DATA DECLARATIONS ----------------------------------------------

extern HWND Window;
extern IVideo *Video;
extern BOOL AppActive;
extern int SessionState;
extern bool VidResizing;

EXTERN_CVAR (Bool, fullscreen)
EXTERN_CVAR (Float, Gamma)
EXTERN_CVAR (Bool, vid_vsync)
EXTERN_CVAR (Float, transsouls)
EXTERN_CVAR (Int, vid_refreshrate)

extern IDirect3D9 *D3D;

extern cycle_t BlitCycles;

// PRIVATE DATA DEFINITIONS ------------------------------------------------

const char *const D3DFB::ShaderNames[D3DFB::NUM_SHADERS] =
{
	"NormalColor.pso",
	"NormalColorPal.pso",
	"NormalColorD.pso",
	"NormalColorPalD.pso",
	"NormalColorInv.pso",
	"NormalColorPalInv.pso",
	"NormalColorOpaq.pso",
	"NormalColorPalOpaq.pso",
	"NormalColorInvOpaq.pso",
	"NormalColorPalInvOpaq.pso",

	"AlphaTex.pso",
	"PalAlphaTex.pso",
	"Stencil.pso",
	"PalStencil.pso",

	"VertexColor.pso",

	"SpecialColormap.pso",
	"SpecialColorMapPal.pso",

	"BurnWipe.pso",
	"GammaCorrection.pso",
};

// PUBLIC DATA DEFINITIONS -------------------------------------------------

CVAR(Bool, d3d_antilag, true, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
CVAR(Bool, vid_hwaalines, true, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)

// CODE --------------------------------------------------------------------

//==========================================================================
//
// D3DFB - Constructor
//
//==========================================================================

D3DFB::D3DFB (UINT adapter, int width, int height, bool bgra, bool fullscreen)
	: BaseWinFB (width, height, bgra)
{
	D3DPRESENT_PARAMETERS d3dpp;

	LastHR = 0;

	Adapter = adapter;
	D3DDevice = NULL;
	VertexBuffer = NULL;
	IndexBuffer = NULL;
	FBTexture = NULL;
	TempRenderTexture = NULL;
	RenderTexture[0] = NULL;
	RenderTexture[1] = NULL;
	InitialWipeScreen = NULL;
	ScreenshotTexture = NULL;
	ScreenshotSurface = NULL;
	FinalWipeScreen = NULL;
	PaletteTexture = NULL;
	GammaTexture = NULL;
	FrontCopySurface = NULL;
	for (int i = 0; i < NUM_SHADERS; ++i)
	{
		Shaders[i] = NULL;
	}
	GammaShader = NULL;
	BlockSurface[0] = NULL;
	BlockSurface[1] = NULL;
	VSync = vid_vsync;
	BlendingRect.left = 0;
	BlendingRect.top = 0;
	BlendingRect.right = FBWidth;
	BlendingRect.bottom = FBHeight;
	In2D = 0;
	Palettes = NULL;
	Textures = NULL;
	GatheringWipeScreen = false;
	ScreenWipe = NULL;
	InScene = false;
	QuadExtra = new BufferedTris[MAX_QUAD_BATCH];
	Atlases = NULL;
	PixelDoubling = 0;
	SkipAt = -1;
	CurrRenderTexture = 0;
	RenderTextureToggle = 0;

	Gamma = 1.0;
	FlashColor0 = 0;
	FlashColor1 = 0xFFFFFFFF;
	FlashColor = 0;
	FlashAmount = 0;

	NeedGammaUpdate = false;
	NeedPalUpdate = false;

	RenderBuffer = new DSimpleCanvas(width, height, bgra);

	memcpy(SourcePalette, GPalette.BaseColors, sizeof(PalEntry)*256);

	Windowed = !(static_cast<Win32Video *>(Video)->GoFullscreen(fullscreen));

	TrueHeight = height;
	if (fullscreen)
	{
		for (Win32Video::ModeInfo *mode = static_cast<Win32Video *>(Video)->m_Modes; mode != NULL; mode = mode->next)
		{
			if (mode->width == Width && mode->height == Height)
			{
				TrueHeight = mode->realheight;
				PixelDoubling = mode->doubling;
				break;
			}
		}
	}
	// Offset from top of screen to top of letterboxed screen
	LBOffsetI = (TrueHeight - Height) / 2;
	LBOffset = float(LBOffsetI);

	FillPresentParameters(&d3dpp, fullscreen, VSync);

	HRESULT hr;

	LOG("CreateDevice attempt 1 hwvp\n");
	if (FAILED(hr = D3D->CreateDevice(Adapter, D3DDEVTYPE_HAL, Window,
		D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &d3dpp, &D3DDevice)) &&
		(hr != D3DERR_DEVICELOST || D3DDevice == NULL))
	{
		LOG2("CreateDevice returned hr %08x dev %p; attempt 2 swvp\n", hr, D3DDevice);
		if (FAILED(D3D->CreateDevice(Adapter, D3DDEVTYPE_HAL, Window,
			D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &d3dpp, &D3DDevice)) &&
			(hr != D3DERR_DEVICELOST || D3DDevice == NULL))
		{
			if (d3dpp.FullScreen_RefreshRateInHz != 0)
			{
				d3dpp.FullScreen_RefreshRateInHz = 0;
				LOG2("CreateDevice returned hr %08x dev %p; attempt 3 (hwvp, default Hz)\n", hr, D3DDevice);
				if (FAILED(hr = D3D->CreateDevice(Adapter, D3DDEVTYPE_HAL, Window,
					D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &d3dpp, &D3DDevice)) &&
					(hr != D3DERR_DEVICELOST || D3DDevice == NULL))
				{
					LOG2("CreateDevice returned hr %08x dev %p; attempt 4 (swvp, default Hz)\n", hr, D3DDevice);
					if (FAILED(D3D->CreateDevice(Adapter, D3DDEVTYPE_HAL, Window,
						D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &d3dpp, &D3DDevice)) &&
						hr != D3DERR_DEVICELOST)
					{
						D3DDevice = NULL;
					}
				}
			}
		}
	}
	LOG2("Final CreateDevice returned HR %08x and device %p\n", hr, D3DDevice);
	LastHR = hr;
	if (D3DDevice != NULL)
	{
		D3DADAPTER_IDENTIFIER9 adapter_id;
		D3DDEVICE_CREATION_PARAMETERS create_params;

		if (FAILED(hr = D3DDevice->GetDeviceCaps(&DeviceCaps)))
		{
			memset(&DeviceCaps, 0, sizeof(DeviceCaps));
		}
		if (SUCCEEDED(hr = D3DDevice->GetCreationParameters(&create_params)) &&
			SUCCEEDED(hr = D3D->GetAdapterIdentifier(create_params.AdapterOrdinal, 0, &adapter_id)))
		{
			// NVidia's drivers lie, claiming they don't support
			// antialiased lines when, really, they do.
			if (adapter_id.VendorId == 0x10de)
			{
				DeviceCaps.LineCaps |= D3DLINECAPS_ANTIALIAS;
			}
			// ATI's drivers apparently also lie, so screw this caps bit.
		}
		CreateResources();
		SetInitialState();
	}
}

//==========================================================================
//
// D3DFB - Destructor
//
//==========================================================================

D3DFB::~D3DFB ()
{
	ReleaseResources();
	SAFE_RELEASE( D3DDevice );
	delete[] QuadExtra;
}

//==========================================================================
//
// D3DFB :: SetInitialState
//
// Called after initial device creation and reset, when everything is set
// to D3D's defaults.
//
//==========================================================================

void D3DFB::SetInitialState()
{
	AlphaBlendEnabled = FALSE;
	AlphaBlendOp = D3DBLENDOP_ADD;
	AlphaSrcBlend = D3DBLEND(0);
	AlphaDestBlend = D3DBLEND(0);

	CurPixelShader = NULL;
	memset(Constant, 0, sizeof(Constant));

	for (unsigned i = 0; i < countof(Texture); ++i)
	{
		Texture[i] = NULL;
		D3DDevice->SetSamplerState(i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		D3DDevice->SetSamplerState(i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		if (i > 1)
		{
			// Set linear filtering for the SM14 gamma texture.
			D3DDevice->SetSamplerState(i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		}
	}

	NeedGammaUpdate = true;
	NeedPalUpdate = true;
	OldRenderTarget = NULL;

	// This constant is used for grayscaling weights (.xyz) and color inversion (.w)
	float weights[4] = { 77/256.f, 143/256.f, 37/256.f, 1 };
	D3DDevice->SetPixelShaderConstantF(PSCONST_Weights, weights, 1);

	// D3DRS_ALPHATESTENABLE defaults to FALSE
	// D3DRS_ALPHAREF defaults to 0
	D3DDevice->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_NOTEQUAL);
	AlphaTestEnabled = FALSE;

	CurBorderColor = 0;

	// Clear to black, just in case it wasn't done already.
	D3DDevice->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0,0,0), 0, 0);
}

//==========================================================================
//
// D3DFB :: FillPresentParameters
//
//==========================================================================

void D3DFB::FillPresentParameters (D3DPRESENT_PARAMETERS *pp, bool fullscreen, bool vsync)
{
	memset (pp, 0, sizeof(*pp));
	pp->Windowed = !fullscreen;
	pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp->BackBufferWidth = Width << PixelDoubling;
	pp->BackBufferHeight = TrueHeight << PixelDoubling;
	pp->BackBufferFormat = fullscreen ? D3DFMT_A8R8G8B8 : D3DFMT_UNKNOWN;
	pp->BackBufferCount = 1;
	pp->hDeviceWindow = Window;
	pp->PresentationInterval = vsync ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
	if (fullscreen)
	{
		pp->FullScreen_RefreshRateInHz = vid_refreshrate;
	}
}

//==========================================================================
//
// D3DFB :: CreateResources
//
//==========================================================================

bool D3DFB::CreateResources()
{
	Atlases = NULL;
	if (!Windowed)
	{
		// Remove the window border in fullscreen mode
		SetWindowLong (Window, GWL_STYLE, WS_POPUP|WS_VISIBLE|WS_SYSMENU);
	}
	else
	{
		// Resize the window to match desired dimensions
		RECT rect = { 0, 0, Width, Height };
		AdjustWindowRectEx(&rect, WS_VISIBLE|WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW);
		int sizew = rect.right - rect.left;
		int sizeh = rect.bottom - rect.top;
		LOG2 ("Resize window to %dx%d\n", sizew, sizeh);
		VidResizing = true;
		// Make sure the window has a border in windowed mode
		SetWindowLong(Window, GWL_STYLE, WS_VISIBLE|WS_OVERLAPPEDWINDOW);
		if (GetWindowLong(Window, GWL_EXSTYLE) & WS_EX_TOPMOST)
		{
			// Direct3D 9 will apparently add WS_EX_TOPMOST to fullscreen windows,
			// and removing it is a little tricky. Using SetWindowLongPtr to clear it
			// will not do the trick, but sending the window behind everything will.
			SetWindowPos(Window, HWND_BOTTOM, 0, 0, sizew, sizeh,
				SWP_DRAWFRAME | SWP_NOCOPYBITS | SWP_NOMOVE);
			SetWindowPos(Window, HWND_TOP, 0, 0, 0, 0, SWP_NOCOPYBITS | SWP_NOMOVE | SWP_NOSIZE);
		}
		else
		{
			SetWindowPos(Window, NULL, 0, 0, sizew, sizeh,
				SWP_DRAWFRAME | SWP_NOCOPYBITS | SWP_NOMOVE | SWP_NOZORDER);
		}
		I_RestoreWindowedPos();
		VidResizing = false;
	}
	if (!LoadShaders())
	{
		return false;
	}
	if (!CreateFBTexture() ||
		!CreatePaletteTexture())
	{
		return false;
	}
	if (!CreateVertexes(NUM_VERTS, NUM_INDEXES))
	{
		return false;
	}
	CreateBlockSurfaces();
	return true;
}

//==========================================================================
//
// D3DFB :: LoadShaders
//
// Returns true if all required shaders were loaded. (Gamma and burn wipe
// are the only ones not considered "required".)
//
//==========================================================================

bool D3DFB::LoadShaders()
{
	static const char models[][4] = { "30/", "20/" };
	FString shaderdir, shaderpath;
	unsigned model, i;
	int lump;

	// We determine the best available model simply by trying them all in
	// order of decreasing preference.
	for (model = 0; model < countof(models); ++model)
	{
		shaderdir = "shaders/d3d/sm";
		shaderdir += models[model];
		for (i = 0; i < NUM_SHADERS; ++i)
		{
			shaderpath = shaderdir;
			shaderpath += ShaderNames[i];
			lump = Wads.CheckNumForFullName(shaderpath);
			if (lump >= 0)
			{
				FMemLump data = Wads.ReadLump(lump);
				if (FAILED(D3DDevice->CreatePixelShader((DWORD *)data.GetMem(), &Shaders[i])) &&
					i < SHADER_BurnWipe)
				{
					break;
				}
			}
		}
		if (i == NUM_SHADERS)
		{ // Success!
			return true;
		}
		// Failure. Release whatever managed to load (which is probably nothing.)
		for (i = 0; i < NUM_SHADERS; ++i)
		{
			SAFE_RELEASE( Shaders[i] );
		}
	}
	return false;
}

//==========================================================================
//
// D3DFB :: ReleaseResources
//
//==========================================================================

void D3DFB::ReleaseResources ()
{
	I_SaveWindowedPos ();
	KillNativeTexs();
	KillNativePals();
	ReleaseDefaultPoolItems();
	SAFE_RELEASE( ScreenshotSurface );
	SAFE_RELEASE( ScreenshotTexture );
	SAFE_RELEASE( PaletteTexture );
	for (int i = 0; i < NUM_SHADERS; ++i)
	{
		SAFE_RELEASE( Shaders[i] );
	}
	GammaShader = NULL;
	if (ScreenWipe != NULL)
	{
		delete ScreenWipe;
		ScreenWipe = NULL;
	}
	GatheringWipeScreen = false;
}

//==========================================================================
//
// D3DFB :: ReleaseDefaultPoolItems
//
// Free resources created with D3DPOOL_DEFAULT.
//
//==========================================================================

void D3DFB::ReleaseDefaultPoolItems()
{
	SAFE_RELEASE( FBTexture );
	SAFE_RELEASE( FinalWipeScreen );
	SAFE_RELEASE( RenderTexture[0] );
	SAFE_RELEASE( RenderTexture[1] );
	SAFE_RELEASE( InitialWipeScreen );
	SAFE_RELEASE( VertexBuffer );
	SAFE_RELEASE( IndexBuffer );
	SAFE_RELEASE( BlockSurface[0] );
	SAFE_RELEASE( BlockSurface[1] );
	SAFE_RELEASE( FrontCopySurface );
}

//==========================================================================
//
// D3DFB :: Reset
//
//==========================================================================

bool D3DFB::Reset ()
{
	D3DPRESENT_PARAMETERS d3dpp;

	ReleaseDefaultPoolItems();
	FillPresentParameters (&d3dpp, !Windowed, VSync);
	if (!SUCCEEDED(D3DDevice->Reset (&d3dpp)))
	{
		if (d3dpp.FullScreen_RefreshRateInHz != 0)
		{
			d3dpp.FullScreen_RefreshRateInHz = 0;
			if (!SUCCEEDED(D3DDevice->Reset (&d3dpp)))
			{
				return false;
			}
		}
		else
		{
			return false;
		}
	}
	LOG("Device was reset\n");
	if (!CreateFBTexture() || !CreateVertexes(NUM_VERTS, NUM_INDEXES))
	{
		return false;
	}
	CreateBlockSurfaces();
	SetInitialState();
	return true;
}

//==========================================================================
//
// D3DFB :: CreateBlockSurfaces
//
// Create blocking surfaces for antilag. It's okay if these can't be
// created; antilag just won't work.
//
//==========================================================================

void D3DFB::CreateBlockSurfaces()
{
	BlockNum = 0;
	if (SUCCEEDED(D3DDevice->CreateOffscreenPlainSurface(16, 16, D3DFMT_A8R8G8B8,
		D3DPOOL_DEFAULT, &BlockSurface[0], 0)))
	{
		if (FAILED(D3DDevice->CreateOffscreenPlainSurface(16, 16, D3DFMT_A8R8G8B8,
			D3DPOOL_DEFAULT, &BlockSurface[1], 0)))
		{
			BlockSurface[0]->Release();
			BlockSurface[0] = NULL;
		}
	}
}

//==========================================================================
//
// D3DFB :: KillNativePals
//
// Frees all native palettes.
//
//==========================================================================

void D3DFB::KillNativePals()
{
	while (Palettes != NULL)
	{
		delete Palettes;
	}
}

//==========================================================================
//
// D3DFB :: KillNativeTexs
//
// Frees all native textures.
//
//==========================================================================

void D3DFB::KillNativeTexs()
{
	while (Textures != NULL)
	{
		delete Textures;
	}
}

//==========================================================================
//
// D3DFB :: CreateFBTexture
//
// Creates the "Framebuffer" texture. With the advent of hardware-assisted
// 2D, this is something of a misnomer now. The FBTexture is only used for
// uploading the software 3D image to video memory so that it can be drawn
// to the real frame buffer.
//
// It also creates the TempRenderTexture, since this seemed like a
// convenient place to do so.
//
//==========================================================================

bool D3DFB::CreateFBTexture ()
{
	FBFormat = IsBgra() ? D3DFMT_A8R8G8B8 : D3DFMT_L8;

	if (FAILED(D3DDevice->CreateTexture(Width, Height, 1, D3DUSAGE_DYNAMIC, FBFormat, D3DPOOL_DEFAULT, &FBTexture, NULL)))
	{
		int pow2width, pow2height, i;

		for (i = 1; i < Width; i <<= 1) {} pow2width = i;
		for (i = 1; i < Height; i <<= 1) {} pow2height = i;

		if (FAILED(D3DDevice->CreateTexture(pow2width, pow2height, 1, D3DUSAGE_DYNAMIC, FBFormat, D3DPOOL_DEFAULT, &FBTexture, NULL)))
		{
			return false;
		}
		else
		{
			FBWidth = pow2width;
			FBHeight = pow2height;
		}
	}
	else
	{
		FBWidth = Width;
		FBHeight = Height;
	}
	RenderTextureToggle = 0;
	RenderTexture[0] = NULL;
	RenderTexture[1] = NULL;
	if (FAILED(D3DDevice->CreateTexture(FBWidth, FBHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &RenderTexture[0], NULL)))
	{
		return false;
	}
	if (Windowed || PixelDoubling)
	{
		// Windowed or pixel doubling: Create another render texture so we can flip between them.
		RenderTextureToggle = 1;
		if (FAILED(D3DDevice->CreateTexture(FBWidth, FBHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &RenderTexture[1], NULL)))
		{
			return false;
		}
	}
	else
	{
		// Fullscreen and not pixel doubling: Create a render target to have the back buffer copied to.
		if (FAILED(D3DDevice->CreateRenderTarget(Width, Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &FrontCopySurface, NULL)))
		{
			return false;
		}
	}
	// Initialize the TempRenderTextures to black.
	for (int i = 0; i <= RenderTextureToggle; ++i)
	{
		IDirect3DSurface9 *surf;
		if (SUCCEEDED(RenderTexture[i]->GetSurfaceLevel(0, &surf)))
		{
			D3DDevice->ColorFill(surf, NULL, D3DCOLOR_XRGB(0,0,0));
			surf->Release();
		}
	}
	TempRenderTexture = RenderTexture[0];
	CurrRenderTexture = 0;
	return true;
}

//==========================================================================
//
// D3DFB :: CreatePaletteTexture
//
//==========================================================================

bool D3DFB::CreatePaletteTexture ()
{
	if (FAILED(D3DDevice->CreateTexture (256, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &PaletteTexture, NULL)))
	{
		return false;
	}
	return true;
}

//==========================================================================
//
// D3DFB :: CreateVertexes
//
//==========================================================================

bool D3DFB::CreateVertexes (int numverts, int numindices)
{
	SAFE_RELEASE(VertexBuffer);
	SAFE_RELEASE(IndexBuffer);
	NumVertices = numverts;
	NumIndices = numindices;
	if (FAILED(D3DDevice->CreateVertexBuffer(sizeof(FBVERTEX)*numverts, 
		D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_FBVERTEX, D3DPOOL_DEFAULT, &VertexBuffer, NULL)))
	{
		return false;
	}
	if (FAILED(D3DDevice->CreateIndexBuffer(sizeof(uint16_t)*numindices,
		D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_DEFAULT, &IndexBuffer, NULL)))
	{
		return false;
	}
	return true;
}

//==========================================================================
//
// D3DFB :: CalcFullscreenCoords
//
//==========================================================================

void D3DFB::CalcFullscreenCoords (FBVERTEX verts[4], bool viewarea_only, bool can_double, D3DCOLOR color0, D3DCOLOR color1) const
{
	float offset = OldRenderTarget != NULL ? 0 : LBOffset;
	float top = offset - 0.5f;
	float texright = float(Width) / float(FBWidth);
	float texbot = float(Height) / float(FBHeight);
	float mxl, mxr, myt, myb, tmxl, tmxr, tmyt, tmyb;

	if (viewarea_only)
	{ // Just calculate vertices for the viewarea/BlendingRect
		mxl = float(BlendingRect.left) - 0.5f;
		mxr = float(BlendingRect.right) - 0.5f;
		myt = float(BlendingRect.top) + top;
		myb = float(BlendingRect.bottom) + top;
		tmxl = float(BlendingRect.left) / float(Width) * texright;
		tmxr = float(BlendingRect.right) / float(Width) * texright;
		tmyt = float(BlendingRect.top) / float(Height) * texbot;
		tmyb = float(BlendingRect.bottom) / float(Height) * texbot;
	}
	else
	{ // Calculate vertices for the whole screen
		mxl = -0.5f;
		mxr = float(Width << (can_double ? PixelDoubling : 0)) - 0.5f;
		myt = top;
		myb = float(Height << (can_double ? PixelDoubling : 0)) + top;
		tmxl = 0;
		tmxr = texright;
		tmyt = 0;
		tmyb = texbot;
	}

	//{   mxl, myt, 0, 1, 0, 0xFFFFFFFF,    tmxl,    tmyt },
	//{   mxr, myt, 0, 1, 0, 0xFFFFFFFF,    tmxr,    tmyt },
	//{   mxr, myb, 0, 1, 0, 0xFFFFFFFF,    tmxr,    tmyb },
	//{   mxl, myb, 0, 1, 0, 0xFFFFFFFF,    tmxl,    tmyb },

	verts[0].x = mxl;
	verts[0].y = myt;
	verts[0].z = 0;
	verts[0].rhw = 1;
	verts[0].color0 = color0;
	verts[0].color1 = color1;
	verts[0].tu = tmxl;
	verts[0].tv = tmyt;

	verts[1].x = mxr;
	verts[1].y = myt;
	verts[1].z = 0;
	verts[1].rhw = 1;
	verts[1].color0 = color0;
	verts[1].color1 = color1;
	verts[1].tu = tmxr;
	verts[1].tv = tmyt;

	verts[2].x = mxr;
	verts[2].y = myb;
	verts[2].z = 0;
	verts[2].rhw = 1;
	verts[2].color0 = color0;
	verts[2].color1 = color1;
	verts[2].tu = tmxr;
	verts[2].tv = tmyb;

	verts[3].x = mxl;
	verts[3].y = myb;
	verts[3].z = 0;
	verts[3].rhw = 1;
	verts[3].color0 = color0;
	verts[3].color1 = color1;
	verts[3].tu = tmxl;
	verts[3].tv = tmyb;
}

//==========================================================================
//
// D3DFB :: GetPageCount
//
//==========================================================================

int D3DFB::GetPageCount ()
{
	return 1;
}

//==========================================================================
//
// D3DFB :: IsFullscreen
//
//==========================================================================

bool D3DFB::IsFullscreen ()
{
	return !Windowed;
}

//==========================================================================
//
// D3DFB :: Update
//
// When In2D == 0: Copy buffer to screen and present
// When In2D == 1: Copy buffer to screen but do not present
// When In2D == 2: Set up for 2D drawing but do not draw anything
// When In2D == 3: Present and set In2D to 0
//
//==========================================================================

void D3DFB::Update ()
{
	if (In2D == 3)
	{
		if (InScene)
		{
			DrawRateStuff();
			EndQuadBatch();		// Make sure all batched primitives are drawn.
			In2D = 0;
			Flip();
		}
		In2D = 0;
		return;
	}

	if (In2D == 0)
	{
		DrawRateStuff();
	}

	if (NeedGammaUpdate)
	{
		float psgamma[4];
		float igamma;

		NeedGammaUpdate = false;
		igamma = 1 / Gamma;
		if (!Windowed)
		{
			D3DGAMMARAMP ramp;

			for (int i = 0; i < 256; ++i)
			{
				ramp.blue[i] = ramp.green[i] = ramp.red[i] = WORD(65535.f * powf(i / 255.f, igamma));
			}
			LOG("SetGammaRamp\n");
			D3DDevice->SetGammaRamp(0, D3DSGR_CALIBRATE, &ramp);
		}
		else
		{
			if (igamma != 1)
			{
				UpdateGammaTexture(igamma);
				GammaShader = Shaders[SHADER_GammaCorrection];
			}
			else
			{
				GammaShader = NULL;
			}
		}
		psgamma[2] = psgamma[1] = psgamma[0] = igamma;
		psgamma[3] = 0.5;		// For SM14 version
		D3DDevice->SetPixelShaderConstantF(PSCONST_Gamma, psgamma, 1);
	}
	
	if (NeedPalUpdate)
	{
		UploadPalette();
	}

	BlitCycles.Reset();
	BlitCycles.Clock();

	HRESULT hr = D3DDevice->TestCooperativeLevel();
	if (FAILED(hr) && (hr != D3DERR_DEVICENOTRESET || !Reset()))
	{
		Sleep(1);
		return;
	}
	Draw3DPart(In2D <= 1);
	if (In2D == 0)
	{
		Flip();
	}

	BlitCycles.Unclock();
	//LOG1 ("cycles = %d\n", BlitCycles);

#if 0
	Buffer = NULL;
#endif
	UpdatePending = false;
}

//==========================================================================
//
// D3DFB :: Flip
//
//==========================================================================

void D3DFB::Flip()
{
	assert(InScene);

	DrawLetterbox();
	DoWindowedGamma();
	D3DDevice->EndScene();

	CopyNextFrontBuffer();

	// Attempt to counter input lag.
	if (d3d_antilag && BlockSurface[0] != NULL)
	{
		D3DLOCKED_RECT lr;
		volatile int dummy;
		D3DDevice->ColorFill(BlockSurface[BlockNum], NULL, D3DCOLOR_ARGB(0xFF,0,0x20,0x50));
		BlockNum ^= 1;
		if (!FAILED((BlockSurface[BlockNum]->LockRect(&lr, NULL, D3DLOCK_READONLY))))
		{
			dummy = *(int *)lr.pBits;
			BlockSurface[BlockNum]->UnlockRect();
		}
	}
	// Limiting the frame rate is as simple as waiting for the timer to signal this event.
	I_FPSLimit();
	D3DDevice->Present(NULL, NULL, NULL, NULL);
	InScene = false;

	if (RenderTextureToggle)
	{
		// Flip the TempRenderTexture to the other one now.
		CurrRenderTexture ^= RenderTextureToggle;
		TempRenderTexture = RenderTexture[CurrRenderTexture];
	}

	if (Windowed)
	{
		RECT box;
		GetClientRect(Window, &box);
		if (box.right > 0 && box.bottom > 0 && (Width != box.right || Height != box.bottom))
		{
			RenderBuffer->Resize(box.right, box.bottom);

			TrueHeight = Height;
			PixelDoubling = 0;
			LBOffsetI = 0;
			LBOffset = 0.0f;
			Reset();

			V_OutputResized(Width, Height);
		}
	}
}

//==========================================================================
//
// D3DFB :: CopyNextFrontBuffer
//
// Duplicates the contents of the back buffer that will become the front
// buffer upon Present into FrontCopySurface so that we can get the
// contents of the display without wasting time in GetFrontBufferData().
//
//==========================================================================

void D3DFB::CopyNextFrontBuffer()
{
	IDirect3DSurface9 *backbuff;

	if (Windowed || PixelDoubling)
	{
		// Windowed mode or pixel doubling: TempRenderTexture has what we want
		SAFE_RELEASE( FrontCopySurface );
		if (SUCCEEDED(TempRenderTexture->GetSurfaceLevel(0, &backbuff)))
		{
			FrontCopySurface = backbuff;
		}
	}
	else
	{
		// Fullscreen, not pixel doubled: The back buffer has what we want,
		// but it might be letter boxed.
		if (SUCCEEDED(D3DDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuff)))
		{
			RECT srcrect = { 0, LBOffsetI, Width, LBOffsetI + Height };
			D3DDevice->StretchRect(backbuff, &srcrect, FrontCopySurface, NULL, D3DTEXF_NONE);
			backbuff->Release();
		}
	}
}

//==========================================================================
//
// D3DFB :: Draw3DPart
//
// The software 3D part, to be exact.
//
//==========================================================================

void D3DFB::Draw3DPart(bool copy3d)
{
	if (copy3d)
	{
		RECT texrect = { 0, 0, Width, Height };
		D3DLOCKED_RECT lockrect;

		if ((FBWidth == Width && FBHeight == Height &&
			SUCCEEDED(FBTexture->LockRect (0, &lockrect, NULL, D3DLOCK_DISCARD))) ||
			SUCCEEDED(FBTexture->LockRect (0, &lockrect, &texrect, 0)))
		{
			auto MemBuffer = RenderBuffer->GetPixels();
			auto Pitch = RenderBuffer->GetPitch();

			if (IsBgra() && FBFormat == D3DFMT_A8R8G8B8)
			{

				if (lockrect.Pitch == Pitch * sizeof(uint32_t) && Pitch == Width)
				{
					memcpy(lockrect.pBits, MemBuffer, Width * Height * sizeof(uint32_t));
				}
				else
				{
					uint32_t *dest = (uint32_t *)lockrect.pBits;
					uint32_t *src = (uint32_t*)MemBuffer;
					for (int y = 0; y < Height; y++)
					{
						memcpy(dest, src, Width * sizeof(uint32_t));
						dest = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(dest) + lockrect.Pitch);
						src += Pitch;
					}
				}
			}
			else if (!IsBgra() && FBFormat == D3DFMT_L8)
			{
				if (lockrect.Pitch == Pitch && Pitch == Width)
				{
					memcpy(lockrect.pBits, MemBuffer, Width * Height);
				}
				else
				{
					uint8_t *dest = (uint8_t *)lockrect.pBits;
					uint8_t *src = RenderBuffer->GetPixels();
					for (int y = 0; y < Height; y++)
					{
						memcpy(dest, src, Width);
						dest = reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(dest) + lockrect.Pitch);
						src += RenderBuffer->GetPitch();
					}
				}
			}
			else
			{
				memset(lockrect.pBits, 0, lockrect.Pitch * Height);
			}
			FBTexture->UnlockRect (0);
		}
	}
	InScene = true;
	D3DDevice->BeginScene();
	D3DDevice->SetRenderState(D3DRS_ANTIALIASEDLINEENABLE, vid_hwaalines);
	assert(OldRenderTarget == NULL);
	if (TempRenderTexture != NULL &&
		((Windowed && TempRenderTexture != FinalWipeScreen) || GatheringWipeScreen || PixelDoubling))
	{
		IDirect3DSurface9 *targetsurf;
		if (SUCCEEDED(TempRenderTexture->GetSurfaceLevel(0, &targetsurf)))
		{
			if (SUCCEEDED(D3DDevice->GetRenderTarget(0, &OldRenderTarget)))
			{
				if (FAILED(D3DDevice->SetRenderTarget(0, targetsurf)))
				{
					// Setting the render target failed.
				}
			}
			targetsurf->Release();
		}
	}

	SetTexture(0, FBTexture);
	SetPaletteTexture(PaletteTexture, 256, BorderColor);
	D3DDevice->SetFVF (D3DFVF_FBVERTEX);
	memset(Constant, 0, sizeof(Constant));
	SetAlphaBlend(D3DBLENDOP(0));
	EnableAlphaTest(FALSE);
	if (IsBgra())
		SetPixelShader(Shaders[SHADER_NormalColor]);
	else
		SetPixelShader(Shaders[SHADER_NormalColorPal]);
	if (copy3d)
	{
		FBVERTEX verts[4];
		D3DCOLOR color0, color1;
		auto map = swrenderer::CameraLight::Instance()->ShaderColormap();
		if (map == NULL)
		{
			color0 = 0;
			color1 = 0xFFFFFFF;
		}
		else
		{
			color0 = D3DCOLOR_COLORVALUE(map->ColorizeStart[0]/2, map->ColorizeStart[1]/2, map->ColorizeStart[2]/2, 0);
			color1 = D3DCOLOR_COLORVALUE(map->ColorizeEnd[0]/2, map->ColorizeEnd[1]/2, map->ColorizeEnd[2]/2, 1);
			if (IsBgra())
				SetPixelShader(Shaders[SHADER_SpecialColormap]);
			else
				SetPixelShader(Shaders[SHADER_SpecialColormapPal]);
		}
		CalcFullscreenCoords(verts, true, false, color0, color1);
		D3DDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, verts, sizeof(FBVERTEX));
	}
	if (IsBgra())
		SetPixelShader(Shaders[SHADER_NormalColor]);
	else
		SetPixelShader(Shaders[SHADER_NormalColorPal]);
}

//==========================================================================
//
// D3DFB :: DrawLetterbox
//
// Draws the black bars at the top and bottom of the screen for letterboxed
// modes.
//
//==========================================================================

void D3DFB::DrawLetterbox()
{
	if (LBOffsetI != 0)
	{
		D3DRECT rects[2] = { { 0, 0, Width, LBOffsetI }, { 0, Height + LBOffsetI, Width, TrueHeight } };
		D3DDevice->Clear (2, rects, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0,0,0), 1.f, 0);
	}
}

//==========================================================================
//
// D3DFB :: DoWindowedGamma
//
// Draws the render target texture to the real back buffer using a gamma-
// correcting pixel shader.
//
//==========================================================================

void D3DFB::DoWindowedGamma()
{
	if (OldRenderTarget != NULL)
	{
		FBVERTEX verts[4];

		CalcFullscreenCoords(verts, false, true, 0, 0xFFFFFFFF);
		D3DDevice->SetRenderTarget(0, OldRenderTarget);
		D3DDevice->SetFVF(D3DFVF_FBVERTEX);
		SetTexture(0, TempRenderTexture);
		SetPixelShader(Windowed && GammaShader ? GammaShader : Shaders[SHADER_NormalColor]);
		SetAlphaBlend(D3DBLENDOP(0));
		EnableAlphaTest(FALSE);
		D3DDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 2, verts, sizeof(FBVERTEX));
		OldRenderTarget->Release();
		OldRenderTarget = NULL;
	}
}

//==========================================================================
//
// D3DFB :: UpdateGammaTexture
//
// Updates the gamma texture used by the PS14 shader. We only use the first
// half of the texture so that we needn't worry about imprecision causing
// it to grab from the border.
//
//==========================================================================

void D3DFB::UpdateGammaTexture(float igamma)
{
	D3DLOCKED_RECT lockrect;

	if (GammaTexture != NULL && SUCCEEDED(GammaTexture->LockRect(0, &lockrect, NULL, 0)))
	{
		uint8_t *pix = (uint8_t *)lockrect.pBits;
		for (int i = 0; i <= 128; ++i)
		{
			pix[i*4+2] = pix[i*4+1] = pix[i*4] = uint8_t(255.f * powf(i / 128.f, igamma));
			pix[i*4+3] = 255;
		}
		GammaTexture->UnlockRect(0);
	}
}

void D3DFB::UploadPalette ()
{
	D3DLOCKED_RECT lockrect;

	if (SUCCEEDED(PaletteTexture->LockRect(0, &lockrect, NULL, 0)))
	{
		uint8_t *pix = (uint8_t *)lockrect.pBits;
		int i;

		for (i = 0; i < 256; ++i, pix += 4)
		{
			pix[0] = SourcePalette[i].b;
			pix[1] = SourcePalette[i].g;
			pix[2] = SourcePalette[i].r;
			pix[3] = (i == 0 ? 0 : 255);
			// To let masked textures work, the first palette entry's alpha is 0.
		}
		PaletteTexture->UnlockRect(0);
		BorderColor = D3DCOLOR_XRGB(SourcePalette[255].r, SourcePalette[255].g, SourcePalette[255].b);
	}
}

PalEntry *D3DFB::GetPalette ()
{
	return SourcePalette;
}

void D3DFB::UpdatePalette ()
{
	NeedPalUpdate = true;
}

bool D3DFB::SetGamma (float gamma)
{
	LOG1 ("SetGamma %g\n", gamma);
	Gamma = gamma;
	NeedGammaUpdate = true;
	return true;
}

bool D3DFB::SetFlash (PalEntry rgb, int amount)
{
	FlashColor = rgb;
	FlashAmount = amount;

	// Fill in the constants for the pixel shader to do linear interpolation between the palette and the flash:
	float r = rgb.r / 255.f, g = rgb.g / 255.f, b = rgb.b / 255.f, a = amount / 256.f;
	FlashColor0 = D3DCOLOR_COLORVALUE(r * a, g * a, b * a, 0);
	a = 1 - a;
	FlashColor1 = D3DCOLOR_COLORVALUE(a, a, a, 1);
	return true;
}

void D3DFB::GetFlash (PalEntry &rgb, int &amount)
{
	rgb = FlashColor;
	amount = FlashAmount;
}

void D3DFB::GetFlashedPalette (PalEntry pal[256])
{
	memcpy (pal, SourcePalette, 256*sizeof(PalEntry));
	if (FlashAmount)
	{
		DoBlending (pal, pal, 256, FlashColor.r, FlashColor.g, FlashColor.b, FlashAmount);
	}
}

void D3DFB::SetVSync (bool vsync)
{
	if (VSync != vsync)
	{
		VSync = vsync;
		Reset();
	}
}

void D3DFB::NewRefreshRate ()
{
	if (!Windowed)
	{
		Reset();
	}
}

void D3DFB::SetBlendingRect(int x1, int y1, int x2, int y2)
{
	BlendingRect.left = x1;
	BlendingRect.top = y1;
	BlendingRect.right = x2;
	BlendingRect.bottom = y2;
}

//==========================================================================
//
// D3DFB :: GetScreenshotBuffer
//
// Returns a pointer into a surface holding the current screen data.
//
//==========================================================================

void D3DFB::GetScreenshotBuffer(const uint8_t *&buffer, int &pitch, ESSType &color_type, float &gamma)
{
	D3DLOCKED_RECT lrect;

	buffer = NULL;
	if ((ScreenshotTexture = GetCurrentScreen()) != NULL)
	{
		if (FAILED(ScreenshotTexture->GetSurfaceLevel(0, &ScreenshotSurface)))
		{
			ScreenshotTexture->Release();
			ScreenshotTexture = NULL;
		}
		else if (FAILED(ScreenshotSurface->LockRect(&lrect, NULL, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)))
		{
			ScreenshotSurface->Release();
			ScreenshotSurface = NULL;
			ScreenshotTexture->Release();
			ScreenshotTexture = NULL;
		}
		else
		{
			buffer = (const uint8_t *)lrect.pBits;
			pitch = lrect.Pitch;
			color_type = SS_BGRA;
			gamma = Gamma;
		}
	}
}

//==========================================================================
//
// D3DFB :: ReleaseScreenshotBuffer
//
//==========================================================================

void D3DFB::ReleaseScreenshotBuffer()
{
	if (ScreenshotSurface != NULL)
	{
		ScreenshotSurface->UnlockRect();
		ScreenshotSurface->Release();
		ScreenshotSurface = NULL;
	}
	SAFE_RELEASE( ScreenshotTexture );
}

//==========================================================================
//
// D3DFB :: GetCurrentScreen
//
// Returns a texture containing the pixels currently visible on-screen.
//
//==========================================================================

IDirect3DTexture9 *D3DFB::GetCurrentScreen(D3DPOOL pool)
{
	IDirect3DTexture9 *tex;
	IDirect3DSurface9 *surf;
	D3DSURFACE_DESC desc;
	HRESULT hr;

	assert(pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_DEFAULT);

	if (FrontCopySurface == NULL || FAILED(FrontCopySurface->GetDesc(&desc)))
	{
		return NULL;
	}
	if (pool == D3DPOOL_SYSTEMMEM)
	{
		hr = D3DDevice->CreateTexture(desc.Width, desc.Height, 1, 0, desc.Format, D3DPOOL_SYSTEMMEM, &tex, NULL);
	}
	else
	{
		hr = D3DDevice->CreateTexture(FBWidth, FBHeight, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &tex, NULL);
	}
	if (FAILED(hr))
	{
		return NULL;
	}
	if (FAILED(tex->GetSurfaceLevel(0, &surf)))
	{
		tex->Release();
		return NULL;
	}
	if (pool == D3DPOOL_SYSTEMMEM)
	{
		// Video -> System memory : use GetRenderTargetData
		hr = D3DDevice->GetRenderTargetData(FrontCopySurface, surf);
	}
	else
	{
		// Video -> Video memory : use StretchRect
		RECT destrect = { 0, 0, Width, Height };
		hr = D3DDevice->StretchRect(FrontCopySurface, NULL, surf, &destrect, D3DTEXF_POINT);
	}
	surf->Release();
	if (FAILED(hr))
	{
		tex->Release();
		return NULL;
	}
	return tex;
}

/**************************************************************************/
/*                                  2D Stuff                              */
/**************************************************************************/


//==========================================================================
//
// D3DTex Constructor
//
//==========================================================================

D3DTex::D3DTex(FTexture *tex, FTextureFormat fmt, D3DFB *fb, bool wrapping)
	: FNativeTexture(tex, fmt)
{
	// Attach to the texture list for the D3DFB
	Next = fb->Textures;
	if (Next != NULL)
	{
		Next->Prev = &Next;
	}
	Prev = &fb->Textures;
	fb->Textures = this;

	IsGray = false;
	Format = GetTexFormat();

	Create(fb, wrapping);
}

//==========================================================================
//
// D3DTex Destructor
//
//==========================================================================

D3DTex::~D3DTex()
{
	// Detach from the texture list
	*Prev = Next;
	if (Next != NULL)
	{
		Next->Prev = Prev;
	}
	SAFE_RELEASE(Tex);

}

//==========================================================================
//
// D3DTex :: CheckWrapping
//
// Returns true if the texture is compatible with the specified wrapping
// mode.
//
//==========================================================================

bool D3DTex::CheckWrapping(bool wrapping)
{
	return true;	// we no longer use atlases
}

//==========================================================================
//
// D3DTex :: Create
//
// Creates an IDirect3DTexture9 for the texture and copies the image data
// to it. Note that unlike FTexture, this image is row-major.
//
//==========================================================================

bool D3DTex::Create(D3DFB *fb, bool wrapping)
{
	if (SUCCEEDED(fb->D3DDevice->CreateTexture(mGameTex->GetWidth(), mGameTex->GetHeight(), 1, 0, Format, D3DPOOL_MANAGED, &Tex, NULL)))
	{
		return Update();
	}
	return false;
}

//==========================================================================
//
// D3DTex :: Update
//
// Copies image data from the underlying FTexture to the D3D texture.
//
//==========================================================================

bool D3DTex::Update()
{
	D3DSURFACE_DESC desc;
	D3DLOCKED_RECT lrect;
	RECT rect;
	uint8_t *dest;

	assert(mGameTex != NULL);

	if (FAILED(Tex->GetLevelDesc(0, &desc)))
	{
		return false;
	}
	rect = { 0, 0, (LONG)desc.Width, (LONG)desc.Height };
	if (FAILED(Tex->LockRect(0, &lrect, &rect, 0)))
	{
		return false;
	}
	dest = (uint8_t *)lrect.pBits;

	mGameTex->FillBuffer(dest, lrect.Pitch, mGameTex->GetHeight(), mFormat);
	Tex->UnlockRect(0);
	return true;
}

//==========================================================================
//
// D3DTex :: GetTexFormat
//
// Returns the texture format that would best fit this texture.
//
//==========================================================================

D3DFORMAT D3DTex::GetTexFormat()
{
	IsGray = false;

	switch (mFormat)
	{
	case TEX_Pal:	return D3DFMT_L8;
	case TEX_Gray:	IsGray = true; return D3DFMT_L8;
	case TEX_RGB:	return D3DFMT_A8R8G8B8;
	default:		I_FatalError ("GameTex->GetFormat() returned invalid format.");
	}
	return D3DFMT_A8R8G8B8;
}

//==========================================================================
//
// D3DPal Constructor
//
//==========================================================================

D3DPal::D3DPal(FRemapTable *remap, D3DFB *fb)
	: Tex(NULL), Remap(remap)
{
	int count;

	// Attach to the palette list for the D3DFB
	Next = fb->Palettes;
	if (Next != NULL)
	{
		Next->Prev = &Next;
	}
	Prev = &fb->Palettes;
	fb->Palettes = this;

	int pow2count;

	// Round up to the nearest power of 2.
	for (pow2count = 1; pow2count < remap->NumEntries; pow2count <<= 1)
	{ }
	count = pow2count;
	BorderColor = 0;
	RoundedPaletteSize = count;
	if (SUCCEEDED(fb->D3DDevice->CreateTexture(count, 1, 1, 0, 
		D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &Tex, NULL)))
	{
		if (!Update())
		{
			Tex->Release();
			Tex = NULL;
		}
	}
}

//==========================================================================
//
// D3DPal Destructor
//
//==========================================================================

D3DPal::~D3DPal()
{
	SAFE_RELEASE( Tex );
	// Detach from the palette list
	*Prev = Next;
	if (Next != NULL)
	{
		Next->Prev = Prev;
	}
	// Remove link from the remap table
	if (Remap != NULL)
	{
		Remap->Native = NULL;
	}
}

//==========================================================================
//
// D3DPal :: Update
//
// Copies the palette to the texture.
//
//==========================================================================

bool D3DPal::Update()
{
	D3DLOCKED_RECT lrect;

	assert(Tex != NULL);

	if (FAILED(Tex->LockRect(0, &lrect, NULL, 0)))
	{
		return false;
	}
	auto buff = (D3DCOLOR *)lrect.pBits;
	auto pal = Remap->Palette;

	auto maxidx = MIN(Remap->NumEntries, 256);

	for (int i = 0; i < maxidx; ++i)
	{
		buff[i] = D3DCOLOR_ARGB(pal[i].a, pal[i].r, pal[i].g, pal[i].b);
	}
	BorderColor = D3DCOLOR_ARGB(pal[maxidx].a, pal[maxidx-1].r, pal[maxidx-1].g, pal[maxidx-1].b);

	Tex->UnlockRect(0);
	return true;
}

//==========================================================================
//
// D3DFB :: Begin2D
//
// Begins 2D mode drawing operations. In particular, DrawTexture is
// rerouted to use Direct3D instead of the software renderer.
//
//==========================================================================

bool D3DFB::Begin2D(bool copy3d)
{
	Super::Begin2D(copy3d);
	if (In2D)
	{
		return true;
	}
	In2D = 2 - copy3d;
	Update();
	In2D = 3;

	return true;
}

//==========================================================================
//
// D3DFB :: DrawBlendingRect
//
// Call after Begin2D to blend the 3D view.
//
//==========================================================================

void D3DFB::DrawBlendingRect()
{
	Dim(FlashColor, FlashAmount / 256.f, viewwindowx, viewwindowy, viewwidth, viewheight);
}

//==========================================================================
//
// D3DFB :: CreateTexture
//
// Returns a native texture that wraps a FTexture.
//
//==========================================================================

FNativeTexture *D3DFB::CreateTexture(FTexture *gametex, FTextureFormat fmt, bool wrapping)
{
	D3DTex *tex = new D3DTex(gametex, fmt, this, wrapping);
	return tex;
}

//==========================================================================
//
// D3DFB :: CreatePalette
//
// Returns a native texture that contains a palette.
//
//==========================================================================

FNativePalette *D3DFB::CreatePalette(FRemapTable *remap)
{
	D3DPal *tex = new D3DPal(remap, this);
	if (tex->Tex == NULL)
	{
		delete tex;
		return NULL;
	}
	return tex;
}

//==========================================================================
//
// D3DFB :: Draw2D
//
//==========================================================================

static D3DBLENDOP OpToD3D(int op)
{
	switch (op)
	{
		// STYLEOP_None can never get here.
	default:
		return D3DBLENDOP_ADD;
	case STYLEOP_Sub:
		return D3DBLENDOP_SUBTRACT;
	case STYLEOP_RevSub:
		return D3DBLENDOP_REVSUBTRACT;
	}
}


void D3DFB::Draw2D()
{
	auto &vertices = m2DDrawer.mVertices;
	auto &indices = m2DDrawer.mIndices;
	auto &commands = m2DDrawer.mData;

	auto vc = vertices.Size();
	auto ic = indices.Size();
	if (vc > NumVertices || ic > NumIndices)
	{
		// We got more vertices than the current buffer can take so resize it.
		if (!CreateVertexes(MAX(vc, NumVertices), MAX(ic, NumIndices)))
		{
			I_FatalError("Unable to resize vertex buffer");
		}
	}
	IndexBuffer->Lock(0, 0, (void **)&IndexData, D3DLOCK_DISCARD);
	memcpy(IndexData, &indices[0], sizeof(*IndexData) * ic);
	IndexBuffer->Unlock();
	VertexBuffer->Lock(0, 0, (void **)&VertexData, D3DLOCK_DISCARD);
	auto yoffs = GatheringWipeScreen ? 0.5f : 0.5f - LBOffset;

	for (auto &vt : vertices)
	{
		VertexData->x = vt.x;
		VertexData->y = vt.y + yoffs;
		VertexData->z = vt.z;
		VertexData->rhw = 1;
		VertexData->color0 = vt.color0;
		VertexData->color1 = 0;
		VertexData->tu = vt.u;
		VertexData->tv = vt.v;
	}
	VertexBuffer->Unlock();
	D3DDevice->SetStreamSource(0, VertexBuffer, 0, sizeof(FBVERTEX));
	D3DDevice->SetIndices(IndexBuffer);

	D3DDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER);
	D3DDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER);
	bool uv_wrapped = false;
	bool scissoring = false;
	EnableAlphaTest(true);

	for (auto &cmd : commands)
	{
		//Set blending mode
		SetAlphaBlend(OpToD3D(cmd.mRenderStyle.BlendOp), GetStyleAlpha(cmd.mRenderStyle.SrcAlpha), GetStyleAlpha(cmd.mRenderStyle.DestAlpha));
		int index = -1;

		if (cmd.mTexture == nullptr)
		{
			index = SHADER_VertexColor;
		}
		else
		{
			// set texture wrapping
			bool uv_should_wrap = !!(cmd.mFlags & F2DDrawer::DTF_Wrap);
			if (uv_wrapped != uv_should_wrap)
			{
				DWORD mode = uv_should_wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_BORDER;
				uv_wrapped = uv_should_wrap;
				D3DDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, mode);
				D3DDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, mode);
			}

			auto textype = cmd.mTexture->GetFormat();	// This never returns TEX_Gray.
			if (cmd.mTranslation) textype = TEX_Pal;	// Translation requires a paletted texture, regardless of the source format.

			if (cmd.mFlags & F2DDrawer::DTF_SpecialColormap)
			{
				index = textype == TEX_Pal ? SHADER_SpecialColormapPal : SHADER_SpecialColormap;
				SetConstant(PSCONST_Color1, cmd.mColor1.r / 510.f, cmd.mColor1.g / 510.f, cmd.mColor1.b / 510.f, 0);
				SetConstant(PSCONST_Color2, cmd.mColor1.r / 510.f, cmd.mColor1.g / 510.f, cmd.mColor1.b / 510.f, 0);
			}
			else
			{
				SetConstant(PSCONST_Desaturation, cmd.mDesaturate / 255.f, (255 - cmd.mDesaturate) / 255.f, 0, 0);
				SetConstant(PSCONST_Color1, cmd.mColor1.r / 255.f, cmd.mColor1.g / 255.f, cmd.mColor1.b / 255.f, 0);
				switch (cmd.mDrawMode)
				{
				default:
				case F2DDrawer::DTM_Normal:
					if (cmd.mDesaturate) index = textype == TEX_Pal ? SHADER_NormalColorPalD : SHADER_NormalColorD;
					else  index = textype == TEX_Pal ? SHADER_NormalColorPal : SHADER_NormalColor;
					break;

				case F2DDrawer::DTM_Invert:
					index = textype == TEX_Pal ? SHADER_NormalColorPalInv : SHADER_NormalColorInv;
					break;

				case F2DDrawer::DTM_InvertOpaque:
					index = textype == TEX_Pal ? SHADER_NormalColorPalInvOpaq : SHADER_NormalColorInvOpaq;
					break;

				case F2DDrawer::DTM_AlphaTexture:
					index = textype == TEX_Pal ? SHADER_PalAlphaTex : SHADER_AlphaTex;
					break;

				case F2DDrawer::DTM_Opaque:
					index = textype == TEX_Pal ? SHADER_NormalColorPalOpaq : SHADER_NormalColorOpaq;
					break;

				case F2DDrawer::DTM_Stencil:
					index = textype == TEX_Pal ? SHADER_PalStencil : SHADER_Stencil;
					break;

				}
			}

			auto tex = cmd.mTexture;
			D3DTex *d3dtex = static_cast<D3DTex *>(tex->GetNative(textype, uv_should_wrap));
			if (d3dtex == nullptr) continue;
			SetTexture(0, d3dtex->Tex);

			if (textype == TEX_Pal)
			{
				if (!cmd.mTranslation)
				{
					SetPaletteTexture(PaletteTexture, 256, BorderColor);
				}
				else
				{
					auto ptex = static_cast<D3DPal*>(cmd.mTranslation->GetNative());
					if (ptex != nullptr)
					{
						SetPaletteTexture(ptex->Tex, ptex->RoundedPaletteSize, ptex->BorderColor);
					}
				}
			}
		}
		if (index == -1) continue;
		SetPixelShader(Shaders[index]);

		if (cmd.mFlags & F2DDrawer::DTF_Scissor)
		{
			scissoring = true;
			RECT scissor = { cmd.mScissor[0], cmd.mScissor[1] + LBOffsetI, cmd.mScissor[2], cmd.mScissor[3] + LBOffsetI };
			D3DDevice->SetScissorRect(&scissor);
			D3DDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
		}
		else if (scissoring) D3DDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, false);

		switch (cmd.mType)
		{
		case F2DDrawer::DrawTypeTriangles:
			D3DDevice->DrawPrimitive(D3DPT_TRIANGLELIST, cmd.mVertIndex, cmd.mVertCount);
			break;

		case F2DDrawer::DrawTypeLines:
			D3DDevice->DrawPrimitive(D3DPT_LINELIST, cmd.mVertIndex, cmd.mVertCount);
			break;

		case F2DDrawer::DrawTypePoints:
			D3DDevice->DrawPrimitive(D3DPT_POINTLIST, cmd.mVertIndex, cmd.mVertCount);
			break;

		}
	}
	if (uv_wrapped)
	{
		D3DDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER);
		D3DDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER);
	}
}

//==========================================================================
//
// D3DFB :: CheckQuadBatch
//
// Make sure there's enough room in the batch for one more set of triangles.
//
//==========================================================================

void D3DFB::CheckQuadBatch(int numtris, int numverts)
{
	if (QuadBatchPos == MAX_QUAD_BATCH ||
		VertexPos + numverts > NUM_VERTS ||
		IndexPos + numtris * 3 > NUM_INDEXES)
	{
		EndQuadBatch();
	}
	if (QuadBatchPos < 0)
	{
		BeginQuadBatch();
	}
}

//==========================================================================
//
// D3DFB :: BeginQuadBatch
//
// Locks the vertex buffer for quads and sets the cursor to 0.
//
//==========================================================================

void D3DFB::BeginQuadBatch()
{
	if (In2D < 2 || !InScene || QuadBatchPos >= 0)
	{
		return;
	}
	VertexBuffer->Lock(0, 0, (void **)&VertexData, D3DLOCK_DISCARD);
	IndexBuffer->Lock(0, 0, (void **)&IndexData, D3DLOCK_DISCARD);
	VertexPos = 0;
	IndexPos = 0;
	QuadBatchPos = 0;
	BatchType = BATCH_Quads;
}

//==========================================================================
//
// D3DFB :: EndQuadBatch
//
// Draws all the quads that have been batched up.
// This is still needed by the wiper and has been stripped off everything unneeded.
//
//==========================================================================

void D3DFB::EndQuadBatch()
{
	if (In2D < 2 || !InScene || BatchType != BATCH_Quads)
	{
		return;
	}
	BatchType = BATCH_None;
	VertexBuffer->Unlock();
	IndexBuffer->Unlock();
	if (QuadBatchPos == 0)
	{
		QuadBatchPos = -1;
		VertexPos = -1;
		IndexPos = -1;
		return;
	}
	D3DDevice->SetStreamSource(0, VertexBuffer, 0, sizeof(FBVERTEX));
	D3DDevice->SetIndices(IndexBuffer);
	int indexpos, vertpos;

	indexpos = vertpos = 0;
	for (int i = 0; i < QuadBatchPos; )
	{
		const BufferedTris *quad = &QuadExtra[i];
		int j;

		int startindex = indexpos;
		int startvertex = vertpos;

		indexpos += quad->NumTris * 3;
		vertpos += quad->NumVerts;

		// Quads with matching parameters should be done with a single
		// DrawPrimitive call.
		for (j = i + 1; j < QuadBatchPos; ++j)
		{
			const BufferedTris *q2 = &QuadExtra[j];
			if (quad->Texture != q2->Texture ||
				quad->Group1 != q2->Group1 ||
				quad->Palette != q2->Palette)
			{
				break;
			}
			indexpos += q2->NumTris * 3;
			vertpos += q2->NumVerts;
		}

		// Set the alpha blending
		SetAlphaBlend(D3DBLENDOP_ADD, D3DBLEND_ONE, D3DBLEND_ZERO);

		// Set the alpha test
		EnableAlphaTest(false);

		SetPixelShader(Shaders[SHADER_NormalColor]);

		// Set the texture
		if (quad->Texture != NULL)
		{
			SetTexture(0, quad->Texture);
		}

		// Draw the quad
		D3DDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0,
			startvertex,					// MinIndex
			vertpos - startvertex,			// NumVertices
			startindex,						// StartIndex
			(indexpos - startindex) / 3		// PrimitiveCount
			/*4 * i, 4 * (j - i), 6 * i, 2 * (j - i)*/);
		i = j;
	}
	QuadBatchPos = -1;
	VertexPos = -1;
	IndexPos = -1;
}

//==========================================================================
//
// D3DFB :: EndBatch
//
// Draws whichever type of primitive is currently being batched.
//
//==========================================================================

D3DBLEND D3DFB::GetStyleAlpha(int type)
{
	switch (type)
	{
	case STYLEALPHA_Zero:		return D3DBLEND_ZERO;
	case STYLEALPHA_One:		return D3DBLEND_ONE;
	case STYLEALPHA_Src:		return D3DBLEND_SRCALPHA;
	case STYLEALPHA_InvSrc:		return D3DBLEND_INVSRCALPHA;
	default:					return D3DBLEND_ZERO;
	}
}


void D3DFB::EnableAlphaTest(BOOL enabled)
{
	if (enabled != AlphaTestEnabled)
	{
		AlphaTestEnabled = enabled;
		D3DDevice->SetRenderState(D3DRS_ALPHATESTENABLE, enabled);
	}
}

void D3DFB::SetAlphaBlend(D3DBLENDOP op, D3DBLEND srcblend, D3DBLEND destblend)
{
	if (op == 0)
	{ // Disable alpha blend
		if (AlphaBlendEnabled)
		{
			AlphaBlendEnabled = FALSE;
			D3DDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		}
	}
	else
	{ // Enable alpha blend
		assert(srcblend != 0);
		assert(destblend != 0);

		if (!AlphaBlendEnabled)
		{
			AlphaBlendEnabled = TRUE;
			D3DDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		}
		if (AlphaBlendOp != op)
		{
			AlphaBlendOp = op;
			D3DDevice->SetRenderState(D3DRS_BLENDOP, op);
		}
		if (AlphaSrcBlend != srcblend)
		{
			AlphaSrcBlend = srcblend;
			D3DDevice->SetRenderState(D3DRS_SRCBLEND, srcblend);
		}
		if (AlphaDestBlend != destblend)
		{
			AlphaDestBlend = destblend;
			D3DDevice->SetRenderState(D3DRS_DESTBLEND, destblend);
		}
	}
}

void D3DFB::SetConstant(int cnum, float r, float g, float b, float a)
{
	if (Constant[cnum][0] != r ||
		Constant[cnum][1] != g ||
		Constant[cnum][2] != b ||
		Constant[cnum][3] != a)
	{
		Constant[cnum][0] = r;
		Constant[cnum][1] = g;
		Constant[cnum][2] = b;
		Constant[cnum][3] = a;
		D3DDevice->SetPixelShaderConstantF(cnum, Constant[cnum], 1);
	}
}

void D3DFB::SetPixelShader(IDirect3DPixelShader9 *shader)
{
	if (CurPixelShader != shader)
	{
		CurPixelShader = shader;
		D3DDevice->SetPixelShader(shader);
	}
}

void D3DFB::SetTexture(int tnum, IDirect3DTexture9 *texture)
{
	assert(unsigned(tnum) < countof(Texture));
	if (Texture[tnum] != texture)
	{
		Texture[tnum] = texture;
		D3DDevice->SetTexture(tnum, texture);
	}
}

void D3DFB::SetPaletteTexture(IDirect3DTexture9 *texture, int count, D3DCOLOR border_color)
{
	// The pixel shader receives color indexes in the range [0.0,1.0].
	// The palette texture is also addressed in the range [0.0,1.0],
	// HOWEVER the coordinate 1.0 is the right edge of the texture and
	// not actually the texture itself. We need to scale and shift
	// the palette indexes so they lie exactly in the center of each
	// texel. For a normal palette with 256 entries, that means the
	// range we use should be [0.5,255.5], adjusted so the coordinate
	// is still within [0.0,1.0].
	//
	// The constant register c2 is used to hold the multiplier in the
	// x part and the adder in the y part.
	float fcount = 1 / float(count);
	SetConstant(PSCONST_PaletteMod, 255 * fcount, 0.5f * fcount, 0, 0);
	SetTexture(1, texture);
}

