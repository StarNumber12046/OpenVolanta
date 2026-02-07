#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <strsafe.h>
#include <chrono>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")

#include "include/SimConnect.h"
#include "include/SimConnectDynamic.h"

#define POLLING_INTERVAL_MS 100
#define METERS_TO_FT 3.28084

// Define SimConnect function pointers
PfSimConnect_Open pSimConnect_Open = NULL;
PfSimConnect_Close pSimConnect_Close = NULL;
PfSimConnect_CallDispatch pSimConnect_CallDispatch = NULL;
PfSimConnect_AddToDataDefinition pSimConnect_AddToDataDefinition = NULL;
PfSimConnect_RequestDataOnSimObject pSimConnect_RequestDataOnSimObject = NULL;

HANDLE  hSimConnect = NULL;
SOCKET tcp_sock = INVALID_SOCKET;
struct sockaddr_in tcp_addr;

enum DATA_DEFINE_ID {
    DEFINITION_POSITION,
    DEFINITION_AIRCRAFT
};

enum DATA_REQUEST_ID {
    REQUEST_POSITION,
    REQUEST_AIRCRAFT
};

struct StructPosition {
    double  latitude;           // PLANE LATITUDE, degrees
    double  longitude;          // PLANE LONGITUDE, degrees
    double  altitude;           // PLANE ALTITUDE, feet
    double  altitude_agl;       // PLANE ALT ABOVE GROUND, feet
    double  pitch;              // PLANE PITCH DEGREES, degrees
    double  bank;               // PLANE BANK DEGREES, degrees
    double  heading_true;       // PLANE HEADING DEGREES TRUE, degrees
    double  ground_speed;       // GROUND VELOCITY, knots
    double  vertical_speed;     // VERTICAL SPEED, feet/minute
    double  fuel_weight;        // FUEL TOTAL QUANTITY WEIGHT, kilograms
    double  transponder_code;   // TRANSPONDER CODE:1, number
    double  on_ground;          // SIM ON GROUND, boolean (0 or 1)
    double  is_slew_active;     // IS SLEW ACTIVE, boolean (0 or 1)
    double  frame_rate;         // FRAME RATE, number
    double  sim_rate;           // SIMULATION RATE, number
    double  autopilot_master;   // AUTOPILOT MASTER, boolean
    double  engine_combustion;  // GENERAL ENG COMBUSTION:1, boolean
    double  parking_brake;      // BRAKE PARKING POSITION, position (0-1)
    double  wind_speed;         // AMBIENT WIND VELOCITY, knots
    double  wind_direction;     // AMBIENT WIND DIRECTION, degrees
};

struct StructAircraft {
    char    title[256];         // TITLE
    char    model[256];         // ATC MODEL
    char    type[256];          // ATC TYPE
    char    registration[256];  // ATC ID
};

void SetupTCPSocket()
{
    if (tcp_sock != INVALID_SOCKET) {
        closesocket(tcp_sock);
        tcp_sock = INVALID_SOCKET;
    }

#ifdef _WIN32
    static bool wsaInitialized = false;
    if (!wsaInitialized) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        wsaInitialized = true;
    }
#endif

    printf("OpenVolanta: Setting up TCP socket\n");
    tcp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcp_sock == INVALID_SOCKET) {
        printf("Error creating socket: %d\n", WSAGetLastError());
        return;
    }

    memset(&tcp_addr, 0, sizeof(tcp_addr));
    tcp_addr.sin_family = AF_INET;
    tcp_addr.sin_port = htons(6746);
    inet_pton(AF_INET, "127.0.0.1", &tcp_addr.sin_addr);
    
    // Set non-blocking mode
    u_long mode = 1;
    ioctlsocket(tcp_sock, FIONBIO, &mode);
    
    int result = connect(tcp_sock, (struct sockaddr*)&tcp_addr, sizeof(tcp_addr));
    if (result < 0) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            printf("OpenVolanta: Unable to connect to Volanta (Error: %d)\n", err);
        }
    }
}

void SendToVolanta(const char* json) {
    if (tcp_sock == INVALID_SOCKET) {
        SetupTCPSocket();
    }

    if (tcp_sock != INVALID_SOCKET) {
        int sent = send(tcp_sock, json, (int)strlen(json), 0);
        if (sent < 0) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                // Connection lost or error
                SetupTCPSocket(); 
                // Don't log every failure to avoid spam, or log only if verbose
                // printf("Failed to send: %s\n", json);
            }
        }
    }
}

void CALLBACK MyDispatchProcRD(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext)
{
    switch (pData->dwID)
    {
        case SIMCONNECT_RECV_ID_SIMOBJECT_DATA:
        {
            SIMCONNECT_RECV_SIMOBJECT_DATA* pTabData = (SIMCONNECT_RECV_SIMOBJECT_DATA*)pData;

            if (pTabData->dwRequestID == REQUEST_POSITION)
            {
                StructPosition* pS = (StructPosition*)&pTabData->dwData;
                
                char json[2048];
                snprintf(json, sizeof(json),
                    "{\"type\":\"STREAM\",\"name\":\"POSITION_UPDATE\",\"data\":{"
                    "\"altitude_amsl\":%.6f,"
                    "\"altitude_agl\":%.6f,"
                    "\"latitude\":%.6f,"
                    "\"longitude\":%.6f,"
                    "\"pitch\":%.6f,"
                    "\"bank\":%.6f,"
                    "\"heading_true\":%.6f,"
                    "\"ground_speed\":%.6f,"
                    "\"vertical_speed\":%.6f,"
                    "\"fuel_kg\":%.6f,"
                    "\"gravity\":%.6f,"
                    "\"transponder\":\"%04d\","
                    "\"on_ground\":%s,"
                    "\"slew\":%s,"
                    "\"paused\":%s,"
                    "\"in_replay_mode\":%s,"
                    "\"fps\":%.6f,"
                    "\"time_acceleration\":%.6f,"
                    "\"autopilot_engaged\":%s,"
                    "\"engines_running\":%s,"
                    "\"parking_brake\":%s,"
                    "\"sim_abbreviation\":\"msfs\","
                    "\"sim_version\":\"11.0\"," 
                    "\"wind_speed\":%.6f,"
                    "\"wind_direction\":%.6f"
                    "}}",
                    pS->altitude, 
                    pS->altitude_agl, 
                    pS->latitude, 
                    pS->longitude,
                    pS->pitch, pS->bank, pS->heading_true,
                    pS->ground_speed, pS->vertical_speed, pS->fuel_weight, 1.0,
                    (int)pS->transponder_code,
                    (pS->on_ground > 0.5) ? "true" : "false",
                    (pS->is_slew_active > 0.5) ? "true" : "false",
                    (pS->sim_rate < 0.001) ? "true" : "false",
                    "false", // replay
                    pS->frame_rate, pS->sim_rate,
                    (pS->autopilot_master > 0.5) ? "true" : "false",
                    (pS->engine_combustion > 0.5) ? "true" : "false",
                    (pS->parking_brake > 0.1) ? "true" : "false", // Parking brake is 0.0 to 1.0
                    pS->wind_speed, pS->wind_direction
                );

                SendToVolanta(json);
            }
            else if (pTabData->dwRequestID == REQUEST_AIRCRAFT)
            {
                StructAircraft* pS = (StructAircraft*)&pTabData->dwData;
                printf("\n[Event] Aircraft Changed: %s (%s)\n", pS->title, pS->registration); 
                
                char json[2048];
                snprintf(json, sizeof(json),
                    "{\"type\":\"STREAM\",\"name\":\"AIRCRAFT_UPDATE\",\"data\":{\"title\":\"%s\",\"type\":\"%s\",\"model\":\"%s\",\"registration\":\"%s\",\"airline\":\"\"}}",
                    pS->title,
                    pS->type,
                    pS->model,
                    pS->registration
                );
                
                SendToVolanta(json);
            }
            break;
        }

        case SIMCONNECT_RECV_ID_QUIT:
        {
            printf("\nQuit received.");
            break;
        }

        case SIMCONNECT_RECV_ID_EXCEPTION:
        {
            SIMCONNECT_RECV_EXCEPTION* pObjData = (SIMCONNECT_RECV_EXCEPTION*)pData;
            printf("\nException received: %d", pObjData->dwException);
            break;
        }

        default:
            break;
    }
}

int __cdecl _tmain(int argc, _TCHAR* argv[])
{
    HRESULT hr;

    printf("Loading SimConnect library...\n");
    if (!LoadSimConnect()) {
        printf("Failed to load SimConnect.dll. Make sure the simulator is installed or the DLL is in the same directory.\n");
        return 1;
    }

    printf("Connecting to SimConnect...\n");

    if (SUCCEEDED(pSimConnect_Open(&hSimConnect, "OpenVolanta SimConnect Client", NULL, 0, 0, 0)))
    {
        printf("Connected to SimConnect!\n");
        SetupTCPSocket();

        // Set up Position Definition
        // Note: Default arguments must be supplied for dynamic function calls
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "PLANE LATITUDE", "degrees", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "PLANE LONGITUDE", "degrees", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "PLANE ALTITUDE", "feet", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "PLANE ALT ABOVE GROUND", "feet", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "PLANE PITCH DEGREES", "degrees", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "PLANE BANK DEGREES", "degrees", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "PLANE HEADING DEGREES TRUE", "degrees", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "GROUND VELOCITY", "knots", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "VERTICAL SPEED", "feet/minute", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "FUEL TOTAL QUANTITY WEIGHT", "kilograms", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "TRANSPONDER CODE:1", "number", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "SIM ON GROUND", "bool", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "IS SLEW ACTIVE", "bool", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "FRAME RATE", "number", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "SIMULATION RATE", "number", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "AUTOPILOT MASTER", "bool", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "GENERAL ENG COMBUSTION:1", "bool", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "BRAKE PARKING POSITION", "position", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "AMBIENT WIND VELOCITY", "knots", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_POSITION, "AMBIENT WIND DIRECTION", "degrees", SIMCONNECT_DATATYPE_FLOAT64, 0.0f, SIMCONNECT_UNUSED);


        // Set up Aircraft Definition
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_AIRCRAFT, "TITLE", NULL, SIMCONNECT_DATATYPE_STRING256, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_AIRCRAFT, "ATC MODEL", NULL, SIMCONNECT_DATATYPE_STRING256, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_AIRCRAFT, "ATC TYPE", NULL, SIMCONNECT_DATATYPE_STRING256, 0.0f, SIMCONNECT_UNUSED);
        hr = pSimConnect_AddToDataDefinition(hSimConnect, DEFINITION_AIRCRAFT, "ATC ID", NULL, SIMCONNECT_DATATYPE_STRING256, 0.0f, SIMCONNECT_UNUSED);


        // Request Aircraft Data - Only when changed
        hr = pSimConnect_RequestDataOnSimObject(hSimConnect, REQUEST_AIRCRAFT, DEFINITION_AIRCRAFT, SIMCONNECT_OBJECT_ID_USER, SIMCONNECT_PERIOD_SIM_FRAME, SIMCONNECT_DATA_REQUEST_FLAG_CHANGED, 0, 0, 0);

        printf("Monitoring aircraft changes and position every %dms...\n", POLLING_INTERVAL_MS);

        // Main Loop
        auto last_request_time = std::chrono::steady_clock::now();
        bool running = true;

        while (running)
        {
            // Process incoming messages
            pSimConnect_CallDispatch(hSimConnect, MyDispatchProcRD, NULL);

            // Handle Position Polling
            auto current_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_request_time).count();

            if (elapsed >= POLLING_INTERVAL_MS)
            {
                // Request Position Data Once
                hr = pSimConnect_RequestDataOnSimObject(hSimConnect, REQUEST_POSITION, DEFINITION_POSITION, SIMCONNECT_OBJECT_ID_USER, SIMCONNECT_PERIOD_ONCE, SIMCONNECT_DATA_REQUEST_FLAG_DEFAULT, 0, 0, 0);
                last_request_time = current_time;
            }

            // Sleep to avoid burning CPU, but short enough to handle dispatch
            Sleep(10);
        }

        hr = pSimConnect_Close(hSimConnect);
    }
    else
    {
        printf("Failed to connect to SimConnect. Ensure the simulator is running.\n");
    }

    if (tcp_sock != INVALID_SOCKET) {
        closesocket(tcp_sock);
    }
    WSACleanup();

    return 0;
}