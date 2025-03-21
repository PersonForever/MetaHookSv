#include "stdafx.h"
#include <cstdlib>

ULONG WINAPI GetMemoryModuleFileSize(HMEMORYMODULE hModule)
{
	auto pModule = MapMemoryModuleHandle(hModule);
	if (pModule)
	{
		return pModule->dwImageMemorySize;
	}

	return 0;
}

ULONG WINAPI GetMemoryModuleSize(HMEMORYMODULE hModule)
{
	auto pModule = MapMemoryModuleHandle(hModule);
	if (pModule)
	{
		return pModule->dwImageMemorySize;
	}

	return 0;
}

HMEMORYMODULE WINAPI LoadLibraryMemoryExW(
	_In_ PVOID BufferAddress,
	_In_ SIZE_T BufferSize,
	_Out_opt_ DWORD * OutImageSize,
	_In_opt_ LPCWSTR DllBaseName,
	_In_opt_ LPCWSTR DllFullName,
	_In_ DWORD Flags) {
	HMEMORYMODULE hMemoryModule = nullptr;
	NTSTATUS status = LdrLoadDllMemoryExW(&hMemoryModule, OutImageSize, nullptr, Flags, BufferAddress, BufferSize, DllBaseName, DllFullName);
	if (!NT_SUCCESS(status) || status == STATUS_IMAGE_MACHINE_TYPE_MISMATCH) {
		SetLastError(RtlNtStatusToDosError(status));
	}
	return hMemoryModule;
}

HMEMORYMODULE WINAPI LoadLibraryMemoryExA(
	_In_ PVOID BufferAddress,
	_In_ SIZE_T BufferSize,
	_Out_opt_ DWORD* OutImageSize,
	_In_opt_ LPCSTR DllBaseName,
	_In_opt_ LPCSTR DllFullName,
	_In_ DWORD Flags) {
	LPWSTR _DllName = nullptr, _DllFullName = nullptr;
	size_t size;
	HMEMORYMODULE result = nullptr;
	HANDLE heap = NtCurrentPeb()->ProcessHeap;

	do {
		if (DllBaseName) {
			size = strlen(DllBaseName) + 1;
			_DllName = (LPWSTR)RtlAllocateHeap(heap, 0, sizeof(WCHAR) * size);

			if (!_DllName) {
				RtlNtStatusToDosError(STATUS_INSUFFICIENT_RESOURCES);
				break;
			}

			mbstowcs_s(nullptr, _DllName, size, DllBaseName, size);
		}

		if (DllFullName) {
			size = strlen(DllFullName) + 1;
			_DllFullName = (LPWSTR)RtlAllocateHeap(heap, 0, sizeof(WCHAR) * size);

			if (!_DllFullName) {
				RtlNtStatusToDosError(STATUS_INSUFFICIENT_RESOURCES);
				break;
			}

			mbstowcs_s(nullptr, _DllFullName, size, DllFullName, size);
		}

		result = LoadLibraryMemoryExW(BufferAddress, BufferSize, OutImageSize, _DllName, _DllFullName, 0);
	} while (false);

	RtlFreeHeap(heap, 0, _DllName);
	RtlFreeHeap(heap, 0, _DllFullName);
	return result;
}

HMEMORYMODULE WINAPI LoadLibraryMemory(_In_ PVOID BufferAddress, _In_ SIZE_T BufferSize) {
	return LoadLibraryMemoryExW(BufferAddress, BufferSize, nullptr, nullptr, nullptr, 0);
}

BOOL WINAPI FreeLibraryMemory(_In_ HMEMORYMODULE hMemoryModule) {
	NTSTATUS status = LdrUnloadDllMemory(hMemoryModule);
	if (!NT_SUCCESS(status)) {
		SetLastError(RtlNtStatusToDosError(status));
		return FALSE;
	}
	return TRUE;
}