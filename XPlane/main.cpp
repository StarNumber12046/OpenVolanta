// Downloaded from https://developer.x-plane.com/code-sample/hello-world-sdk-3/
#pragma comment(lib, "ws2_32.lib")
#if IBM
	#include <winsock2.h>
    #include <ws2tcpip.h>
#endif
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMDataAccess.h"
#include "XPLMUtilities.h"
#include "XPLMProcessing.h"
#include "XPLMPlugin.h"
#include "XPLMPlanes.h"
#include <regex>
#include <algorithm>
#include <string.h>
#if LIN
	#include <GL/gl.h>
#elif __GNUC__
	#include <OpenGL/gl.h>
#else
	#include <GL/gl.h>
#endif
#include <cstdio>

#ifndef XPLM300
	#error This is made to be compiled against the XPLM300 SDK
#endif

#define METERS_TO_FT 3.28084

XPLMDataRef dr_lat, dr_lon, dr_alt_amsl, dr_alt_agl;
XPLMDataRef dr_pitch, dr_bank, dr_heading;
XPLMDataRef dr_gs, dr_vs;
XPLMDataRef dr_fuel_kg;
XPLMDataRef dr_gravity;
XPLMDataRef dr_transponder;
XPLMDataRef dr_on_ground;
XPLMDataRef dr_slew, dr_paused, dr_replay;
XPLMDataRef dr_fps;
XPLMDataRef dr_taccel;
XPLMDataRef dr_ap_engaged;
XPLMDataRef dr_eng_running;
XPLMDataRef dr_parking_brake;
XPLMDataRef dr_wind_speed, dr_wind_dir;
XPLMDataRef livery_path;

XPLMDataRef acf_icao, acf_reg;

SOCKET tcp_sock;
struct sockaddr_in tcp_addr;

bool extract_registration_to_buffer(
    const std::string& livery_name,
    char* output_buffer,
    size_t buffer_size)
{
    // Ensure the output buffer is null-terminated immediately in case of failure
    if (buffer_size > 0) {
        output_buffer[0] = '\0';
    }
    else {
        return false; // Cannot write to a zero-size buffer
    }

    // 1. Define the Regular Expression (Same as disassembly)
    const std::regex registration_regex(
        "[A-Z]-[A-Z]{4}|"           // e.g., G-ABCD
        "([A-Z]|[1-9]){2}-[A-Z]{3}|"        // e.g., EI-GJK, 9H-QDU
        "N[0-9]{1,5}[A-Z]{0,2}",    // e.g., N12345, N1A, N12AB
        std::regex::ECMAScript
    );

    // 2. Define the Match Results Container
    std::smatch match_results;

    // 3. Perform the Regex Search
    if (std::regex_search(livery_name, match_results, registration_regex)) {

        // The entire matched registration is at index 0
        const std::string registration = match_results[0].str();

        // 4. Copy the result to the C-style buffer

        // Determine the length to copy, ensuring it doesn't exceed the buffer size - 1
        size_t copy_len = (((registration.length()) < (buffer_size - 1)) ? (registration.length()) : (buffer_size - 1));

        // Copy the characters
        std::strncpy(output_buffer, registration.c_str(), copy_len);

        // Explicitly null-terminate the string in the buffer
        output_buffer[copy_len] = '\0';

        return true;
    }

    // No match found, buffer is already null-terminated by the initial check
    return false;
}

void FindDatarefs() {
	dr_lat = XPLMFindDataRef("sim/flightmodel/position/latitude");
	dr_lon = XPLMFindDataRef("sim/flightmodel/position/longitude");
	dr_alt_amsl = XPLMFindDataRef("sim/flightmodel/position/elevation");
	dr_alt_agl = XPLMFindDataRef("sim/flightmodel/position/y_agl");

	dr_pitch = XPLMFindDataRef("sim/flightmodel/position/theta");
	dr_bank = XPLMFindDataRef("sim/flightmodel/position/phi");
	dr_heading = XPLMFindDataRef("sim/flightmodel/position/psi");

	dr_gs = XPLMFindDataRef("sim/flightmodel/position/groundspeed");
	dr_vs = XPLMFindDataRef("sim/flightmodel/position/vh_ind_fpm");

	dr_fuel_kg = XPLMFindDataRef("sim/flightmodel/weight/m_fuel_total");
	dr_gravity = XPLMFindDataRef("sim/physics/gravity_normal");

	dr_transponder = XPLMFindDataRef("sim/cockpit/radios/transponder_code");
	dr_on_ground = XPLMFindDataRef("sim/flightmodel/failures/onground_any");
	dr_slew = XPLMFindDataRef("sim/operation/override/override_planepath");
	dr_paused = XPLMFindDataRef("sim/time/paused");
	dr_replay = XPLMFindDataRef("sim/operation/prefs/replay_mode");

	dr_fps = XPLMFindDataRef("sim/graphics/view/framerate_period");
	dr_taccel = XPLMFindDataRef("sim/time/time_accel");

	dr_ap_engaged = XPLMFindDataRef("sim/cockpit/autopilot/autopilot_mode");
	dr_eng_running = XPLMFindDataRef("sim/flightmodel/engine/ENGN_running");
	dr_parking_brake = XPLMFindDataRef("sim/cockpit2/controls/parking_brake_ratio");

	dr_wind_speed = XPLMFindDataRef("sim/weather/wind_speed_kt");
	dr_wind_dir = XPLMFindDataRef("sim/weather/wind_direction_degt");

	acf_icao = XPLMFindDataRef("sim/aircraft/view/acf_ICAO");
    acf_reg = XPLMFindDataRef("sim/aircraft/view/acf_tailnum");

    livery_path = XPLMFindDataRef("sim/aircraft/view/acf_livery_path");
}

void HandleAircraftLoad() {

}


void SetupTCPSocket()
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

	XPLMDebugString("OpenVolanta: Setting up TCP socket\n");
    tcp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    memset(&tcp_addr, 0, sizeof(tcp_addr));
    tcp_addr.sin_family = AF_INET;
    tcp_addr.sin_port = htons(6746);
    tcp_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    u_long mode = 1;
    ioctlsocket(tcp_sock, FIONBIO, &mode);
    int result = connect(tcp_sock, (struct sockaddr*)&tcp_addr, sizeof(tcp_addr));
    if (result < 0) {
		XPLMDebugString("OpenVolanta: Unable to connect to Volanta\n");
    }
}

float SendPosition(
	float                inElapsedSinceLastCall,
	float                inElapsedTimeSinceLastFlightLoop,
	int                  inCounter,
	void* inRefcon)
{
    double lat = XPLMGetDatad(dr_lat);
    double lon = XPLMGetDatad(dr_lon);
    double altA = XPLMGetDatad(dr_alt_amsl);
    double altG = XPLMGetDatad(dr_alt_agl);

    float pitch = XPLMGetDataf(dr_pitch);
    float bank = XPLMGetDataf(dr_bank);
    float hdg = XPLMGetDataf(dr_heading);

    float gs = XPLMGetDataf(dr_gs);
    float vs = XPLMGetDataf(dr_vs);

    float fuel = XPLMGetDataf(dr_fuel_kg);
    float grav = XPLMGetDataf(dr_gravity);

    int   xpndr = XPLMGetDatai(dr_transponder);
    int   og = XPLMGetDatai(dr_on_ground);

    int   slew = XPLMGetDatai(dr_slew);
    int   paused = XPLMGetDatai(dr_paused);
    int   replay = XPLMGetDatai(dr_replay);

    // float fps = 1.0f / XPLMGetDataf(dr_fps);
	float fps = 144; // doesnt seem to update properly, so just fake it
    float taccel = XPLMGetDataf(dr_taccel);

    int   ap_eng = XPLMGetDatai(dr_ap_engaged);
    int   eng_run = XPLMGetDatai(dr_eng_running);
    float brake = XPLMGetDataf(dr_parking_brake);

    float windS = XPLMGetDataf(dr_wind_speed);
    float windD = XPLMGetDataf(dr_wind_dir);

    // Build JSON
    char json[1024];
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
        "\"sim_abbreviation\":\"xp12\","
        "\"sim_version\":\"12.320\","
        "\"wind_speed\":%.6f,"
        "\"wind_direction\":%.6f"
        "}}",
        altA * METERS_TO_FT, 
        altG * METERS_TO_FT, 
        lat, 
        lon,
        pitch, bank, hdg,
        gs, vs, fuel, grav,
        xpndr,
        og ? "true" : "false",
        slew ? "true" : "false",
        paused ? "true" : "false",
        replay ? "true" : "false",
        fps, taccel,
        ap_eng ? "true" : "false",
        eng_run ? "true" : "false",
        brake > 0.5f ? "true" : "false",
        windS, windD
    );

    int sent = send(tcp_sock, json, (int)strlen(json), 0);
    if (sent < 0) {
		SetupTCPSocket();  // Try to reconnect
        XPLMDebugString(json); // Log the failure
    }
	return 0.1f;  // Run again in .1 seconds
}

XPLMFlightLoopID gFlightLoop = NULL;

void CreateMyFlightLoop() {
	XPLMCreateFlightLoop_t params;
	params.structSize = sizeof(params);
	params.phase = xplm_FlightLoop_Phase_AfterFlightModel; // Usually safest
	params.callbackFunc = SendPosition;
	params.refcon = NULL;

	gFlightLoop = XPLMCreateFlightLoop(&params);
}

PLUGIN_API int XPluginStart(
							char *		outName,
							char *		outSig,
							char *		outDesc)
{
	strcpy(outName, "OpenVolanta");
	strcpy(outSig, "starnumber.openvolanta");
	strcpy(outDesc, "A drop-in replacement plugin for Volanta");
	SetupTCPSocket();
	FindDatarefs();
	CreateMyFlightLoop();
	XPLMScheduleFlightLoop(gFlightLoop, -1, 1);
    
	return 1;
}

PLUGIN_API void	XPluginStop(void)
{
	XPLMDestroyFlightLoop(gFlightLoop);
}

PLUGIN_API void XPluginDisable(void) {}
PLUGIN_API int  XPluginEnable(void)  { return 1; }
PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFrom, int inMsg, void * inParam) {
	char output[256];
	snprintf(output, sizeof(output), "OpenVolanta: Received message %d from plugin %d - param: %d\n", inMsg, inFrom, (int)inParam);
    XPLMDebugString(output);
	if (inMsg == XPLM_MSG_LIVERY_LOADED) {
        char icao[256];
        int icaoLength = XPLMGetDatab(acf_icao, NULL, 0, 0);
        if (icaoLength > 0 && icaoLength <= 40) {
            XPLMGetDatab(acf_icao, &icao, 0, icaoLength);
            icao[39] = '\0';
        }
		char reg[256];
		char current_livery_path[256];
        int liveryLength = XPLMGetDatab(livery_path, NULL, 0, 0);
        if (liveryLength > 0 && liveryLength <= 255) {
            XPLMGetDatab(livery_path, &current_livery_path, 0, liveryLength);
            current_livery_path[255] = '\0';
        }
        XPLMDebugString("OpenVolanta: Livery path: ");
		XPLMDebugString(current_livery_path);

		bool success = extract_registration_to_buffer(current_livery_path, reg, sizeof(reg));
        if (!success) {
            // Fallback to acf_tailnum dataref
            reg[0] = '\0';
            int length = XPLMGetDatab(acf_reg, NULL, 0, 0);
            if (length > 0 && length <= 40) {
                XPLMGetDatab(acf_reg, &reg, 0, length);
                icao[39] = '\0';
            }
        }
        else {
            XPLMDebugString("OpenVolanta: Successfully extracted registration from livery name\n");
			XPLMDebugString(reg);
        }
		XPLMDebugString("OpenVolanta: Extracted registration: ");
        XPLMDebugString("OpenVolanta: Plane loaded, sending plane info to Volanta\n");
        char json[1024];
        snprintf(json, sizeof(json),
            "{\"type\":\"STREAM\",\"name\":\"AIRCRAFT_UPDATE\",\"data\":{\"title\":\"\",\"type\":\"%s\",\"model\":\"%s\",\"registration\":\"%s\",\"airline\":\"\"}}",
            icao,
            icao,
            reg
        );
        int sent = send(tcp_sock, json, (int)strlen(json), 0);
        if (sent < 0) {
            SetupTCPSocket();  // Try to reconnect
            XPLMDebugString("OpenVolanta: Failed to send aircraft update\n");
            XPLMDebugString(json); // Log the failure
        }
        else {
            XPLMDebugString("OpenVolanta: Sent aircraft update\n");
        }
    }
}