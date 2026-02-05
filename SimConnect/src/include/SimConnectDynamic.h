#pragma once
#include <windows.h>
#include "SimConnect.h"

typedef HRESULT (WINAPI *PfSimConnect_Open)(HANDLE*, LPCSTR, HWND, DWORD, HANDLE, DWORD);
typedef HRESULT (WINAPI *PfSimConnect_Close)(HANDLE);
typedef HRESULT (WINAPI *PfSimConnect_CallDispatch)(HANDLE, DispatchProc, void*);
typedef HRESULT (WINAPI *PfSimConnect_AddToDataDefinition)(HANDLE, SIMCONNECT_DATA_DEFINITION_ID, const char*, const char*, SIMCONNECT_DATATYPE, float, DWORD);
typedef HRESULT (WINAPI *PfSimConnect_RequestDataOnSimObject)(HANDLE, SIMCONNECT_DATA_REQUEST_ID, SIMCONNECT_DATA_DEFINITION_ID, SIMCONNECT_OBJECT_ID, SIMCONNECT_PERIOD, SIMCONNECT_DATA_REQUEST_FLAG, DWORD, DWORD, DWORD);

extern PfSimConnect_Open pSimConnect_Open;
extern PfSimConnect_Close pSimConnect_Close;
extern PfSimConnect_CallDispatch pSimConnect_CallDispatch;
extern PfSimConnect_AddToDataDefinition pSimConnect_AddToDataDefinition;
extern PfSimConnect_RequestDataOnSimObject pSimConnect_RequestDataOnSimObject;

inline bool LoadSimConnect() {
    HMODULE hSimConnectDll = LoadLibrary(TEXT("SimConnect.dll"));
    if (!hSimConnectDll) return false;

    pSimConnect_Open = (PfSimConnect_Open)GetProcAddress(hSimConnectDll, "SimConnect_Open");
    pSimConnect_Close = (PfSimConnect_Close)GetProcAddress(hSimConnectDll, "SimConnect_Close");
    pSimConnect_CallDispatch = (PfSimConnect_CallDispatch)GetProcAddress(hSimConnectDll, "SimConnect_CallDispatch");
    pSimConnect_AddToDataDefinition = (PfSimConnect_AddToDataDefinition)GetProcAddress(hSimConnectDll, "SimConnect_AddToDataDefinition");
    pSimConnect_RequestDataOnSimObject = (PfSimConnect_RequestDataOnSimObject)GetProcAddress(hSimConnectDll, "SimConnect_RequestDataOnSimObject");

    return (pSimConnect_Open && pSimConnect_Close && pSimConnect_CallDispatch && pSimConnect_AddToDataDefinition && pSimConnect_RequestDataOnSimObject);
}
