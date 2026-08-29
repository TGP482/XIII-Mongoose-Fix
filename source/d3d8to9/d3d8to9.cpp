/**
 * Copyright (C) 2015 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/d3d8to9#license
 */

#include "d3dx9.hpp"
#include "d3d8to9.hpp"
#include "embedded_d3dx9.hpp"

PFN_D3DXAssembleShader D3DXAssembleShader = nullptr;
PFN_D3DXDisassembleShader D3DXDisassembleShader = nullptr;
PFN_D3DXLoadSurfaceFromSurface D3DXLoadSurfaceFromSurface = nullptr;

#ifndef D3D8TO9NOLOG
 // Very simple logging for the purpose of debugging only.
std::ofstream LOG;
#endif

extern "C" HRESULT WINAPI ValidatePixelShader(const DWORD* pPixelShader, const D3DCAPS8* pCaps, BOOL ReturnErrors, char** pErrorsString)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "ValidatePixelShader " << "(" << pPixelShader << ", " << pCaps << ", " << ReturnErrors << ", " << pErrorsString << ")' ..." << std::endl;
#endif

	HRESULT hr = E_FAIL;
	const char* errorMessage = "";

	if (!pPixelShader)
	{
		errorMessage = "Invalid code pointer.\n";
	}
	else
	{
		switch (*pPixelShader)
		{
		case D3DPS_VERSION(1, 0):
		case D3DPS_VERSION(1, 1):
		case D3DPS_VERSION(1, 2):
		case D3DPS_VERSION(1, 3):
		case D3DPS_VERSION(1, 4):
			if (pCaps && *pPixelShader > pCaps->PixelShaderVersion)
			{
				errorMessage = "Shader version not supported by caps.\n";
				break;
			}
			hr = S_OK;
			break;

		default:
			errorMessage = "Unsupported shader version.\n";
		}
	}

	if (!ReturnErrors)
	{
		errorMessage = "";
	}

	if (pErrorsString)
	{
		const size_t size = strlen(errorMessage) + 1;

		*pErrorsString = (char*) HeapAlloc(GetProcessHeap(), 0, size);
		if (*pErrorsString)
		{
			memcpy(*pErrorsString, errorMessage, size);
		}
	}

	return hr;
}

extern "C" HRESULT WINAPI ValidateVertexShader(const DWORD* pVertexShader, const DWORD* pVertexDecl, const D3DCAPS8* pCaps, BOOL ReturnErrors, char** pErrorsString)
{
	UNREFERENCED_PARAMETER(pVertexDecl);

#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "ValidateVertexShader " << "(" << pVertexShader << ", " << pVertexDecl << ", " << pCaps << ", " << ReturnErrors << ", " << pErrorsString << ")' ..." << std::endl;
#endif

	HRESULT hr = E_FAIL;
	const char* errorMessage = "";

	if (!pVertexShader)
	{
		errorMessage = "Invalid code pointer.\n";
	}
	else
	{
		switch (*pVertexShader)
		{
		case D3DVS_VERSION(1, 0):
		case D3DVS_VERSION(1, 1):
			if (pCaps && *pVertexShader > pCaps->VertexShaderVersion)
			{
				errorMessage = "Shader version not supported by caps.\n";
				break;
			}
			hr = S_OK;
			break;

		default:
			errorMessage = "Unsupported shader version.\n";
		}
	}

	if (!ReturnErrors)
	{
		errorMessage = "";
	}

	if (pErrorsString)
	{
		const size_t size = strlen(errorMessage) + 1;

		*pErrorsString = (char*) HeapAlloc(GetProcessHeap(), 0, size);
		if (*pErrorsString)
		{
			memcpy(*pErrorsString, errorMessage, size);
		}
	}

	return hr;
}

extern "C" void WINAPI DebugSetMute()
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "DebugSetMute" << "(" << ")' ..." << std::endl;
#endif
}

extern "C" IDirect3D8 *WINAPI Direct3DCreate8(UINT SDKVersion)
{
#ifndef D3D8TO9NOLOG
	static bool LogMessageFlag = true;

	if (!LOG.is_open())
	{
		LOG.open("d3d8.log", std::ios::trunc);
	}

	if (!LOG.is_open() && LogMessageFlag)
	{
		LogMessageFlag = false;
		MessageBox(nullptr, TEXT("Failed to open debug log file \"d3d8.log\"!"), nullptr, MB_ICONWARNING);
	}

	LOG << "Redirecting '" << "Direct3DCreate8" << "(" << SDKVersion << ")' ..." << std::endl;
	LOG << "> Passing on to 'Direct3DCreate9':" << std::endl;
#endif

	IDirect3D9 *const d3d = Direct3DCreate9(D3D_SDK_VERSION);

	if (d3d == nullptr)
	{
		return nullptr;
	}

	// Load D3DX. Both redistributables are baked into the asi, so nothing has to be installed.
	InitEmbeddedD3DX();

	return new Direct3D8(d3d);
}
