package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"regexp"
	"sync"
	"syscall"
	"time"

	"golang.org/x/net/ipv4"
)

// Constants
const (
	MCAST_GRP          = "239.255.1.1"
	MCAST_PORT         = 49707
	VOLANTA_TCP_ADDR   = "127.0.0.1:6746"
	METERS_TO_FT       = 3.28084
	UPDATE_FREQ        = 10 // Hz
)

// Errors
var (
	ErrXPlaneIpNotFound          = fmt.Errorf("could not find any running XPlane instance in network")
	ErrXPlaneTimeout             = fmt.Errorf("XPlane timeout")
	ErrXPlaneVersionNotSupported = fmt.Errorf("XPlane version not supported")
)

// BeaconData holds information about the discovered X-Plane instance.
type BeaconData struct {
	IP            string
	Port          int
	Hostname      string
	XPlaneVersion int
	Role          int
}

// XPlaneUdp handles communication with X-Plane.
type XPlaneUdp struct {
	socket       *net.UDPConn
	destAddr     *net.UDPAddr
	datarefIdx   int
	datarefs     map[int]string
	xplaneValues map[string]float32
	valuesMutex  sync.RWMutex // Protects xplaneValues
	BeaconData   BeaconData
	defaultFreq  int
}

// NewXPlaneUdp creates a new XPlaneUdp client.
func NewXPlaneUdp() *XPlaneUdp {
	return &XPlaneUdp{
		datarefs:     make(map[int]string),
		xplaneValues: make(map[string]float32),
		defaultFreq:  UPDATE_FREQ,
	}
}

// InitSocket initializes the main UDP socket for communication.
func (xp *XPlaneUdp) InitSocket() error {
	if xp.BeaconData.IP == "" {
		return fmt.Errorf("beacon data not available, run FindIp first")
	}

	var err error
	xp.destAddr, err = net.ResolveUDPAddr("udp", fmt.Sprintf("%s:%d", xp.BeaconData.IP, xp.BeaconData.Port))
	if err != nil {
		return fmt.Errorf("could not resolve x-plane address: %w", err)
	}

	xp.socket, err = net.ListenUDP("udp", nil) // Listen on a random ephemeral port
	if err != nil {
		return fmt.Errorf("could not listen on udp port: %w", err)
	}

	return nil
}

// Close unsubscribes from all datarefs and closes the socket.
func (xp *XPlaneUdp) Close() {
	if xp.socket == nil {
		return
	}
	log.Println("Closing connection and unsubscribing from datarefs...")
	refsToUnsubscribe := make([]string, 0, len(xp.datarefs))
	for _, dataref := range xp.datarefs {
		refsToUnsubscribe = append(refsToUnsubscribe, dataref)
	}

	for _, dataref := range refsToUnsubscribe {
		if err := xp.AddDataRef(dataref, 0); err != nil {
			log.Printf("Warning: failed to unsubscribe from dataref %s: %v", dataref, err)
		}
	}
	xp.socket.Close()
	log.Println("Connection closed.")
}

// FindIp finds the IP of an X-Plane Host by replicating the Python script's low-level socket logic.
func (xp *XPlaneUdp) FindIp() (BeaconData, error) {
	lc := net.ListenConfig{
		Control: func(network, address string, c syscall.RawConn) error {
			var soErr error
			err := c.Control(func(fd uintptr) {
				soErr = syscall.SetsockoptInt(syscall.Handle(fd), syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
			})
			if err != nil {
				return err
			}
			return soErr
		},
	}

	conn, err := lc.ListenPacket(context.Background(), "udp4", fmt.Sprintf("0.0.0.0:%d", MCAST_PORT))
	if err != nil {
		return BeaconData{}, fmt.Errorf("failed to listen on udp port with ListenConfig: %w", err)
	}
	defer conn.Close()

	p := ipv4.NewPacketConn(conn)
	group := net.ParseIP(MCAST_GRP)

	ifaces, err := net.Interfaces()
	if err != nil {
		return BeaconData{}, fmt.Errorf("could not get interfaces: %w", err)
	}

	var joinedAny bool
	for _, ifi := range ifaces {
		if (ifi.Flags&net.FlagUp) == 0 || (ifi.Flags&net.FlagMulticast) == 0 {
			continue
		}
		if err := p.JoinGroup(&ifi, &net.UDPAddr{IP: group}); err != nil {
			// log.Printf("Warning: could not join multicast group on interface %s: %v", ifi.Name, err)
		} else {
			// log.Printf("Joined multicast group on interface: %s", ifi.Name)
			joinedAny = true
		}
	}

	if !joinedAny {
		return BeaconData{}, fmt.Errorf("failed to join multicast group on any viable interface")
	}

	if err := conn.SetReadDeadline(time.Now().Add(5 * time.Second)); err != nil {
		return BeaconData{}, err
	}

	buf := make([]byte, 2048)
	n, sender, err := conn.ReadFrom(buf)
	if err != nil {
		if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
			return BeaconData{}, ErrXPlaneIpNotFound
		}
		return BeaconData{}, fmt.Errorf("error reading beacon packet: %w", err)
	}

	packet := buf[:n]
	if !bytes.HasPrefix(packet, []byte("BECN\x00")) {
		return BeaconData{}, fmt.Errorf("received non-beacon packet from %v", sender)
	}

	type becnStruct struct {
		BeaconMajorVersion uint8
		BeaconMinorVersion uint8
		ApplicationHostID  int32
		VersionNumber      int32
		Role               uint32
		Port               uint16
	}

	var beacon becnStruct
	reader := bytes.NewReader(packet[5:21])
	if err := binary.Read(reader, binary.LittleEndian, &beacon); err != nil {
		return BeaconData{}, fmt.Errorf("failed to decode beacon packet: %w", err)
	}

	if beacon.BeaconMajorVersion != 1 || beacon.BeaconMinorVersion > 2 || beacon.ApplicationHostID != 1 {
		return BeaconData{}, ErrXPlaneVersionNotSupported
	}

	hostnameBytes := packet[21:]
	nullIdx := bytes.IndexByte(hostnameBytes, 0)
	if nullIdx == -1 {
		nullIdx = len(hostnameBytes)
	}
	hostname := string(hostnameBytes[:nullIdx])

	senderUDPAddr, ok := sender.(*net.UDPAddr)
	if !ok {
		return BeaconData{}, fmt.Errorf("sender address is not a UDP address")
	}

	xp.BeaconData = BeaconData{
		IP:            senderUDPAddr.IP.String(),
		Port:          int(beacon.Port),
		Hostname:      hostname,
		XPlaneVersion: int(beacon.VersionNumber),
		Role:          int(beacon.Role),
	}
	return xp.BeaconData, nil
}

// AddDataRef requests X-Plane to send a dataref at a given frequency.
func (xp *XPlaneUdp) AddDataRef(dataref string, freq int) error {
	if xp.socket == nil {
		return fmt.Errorf("socket not initialized, call InitSocket first")
	}

	idx := -9999
	for k, v := range xp.datarefs {
		if v == dataref {
			idx = k
			break
		}
	}

	if idx != -9999 {
		if freq == 0 {
			xp.valuesMutex.Lock()
			delete(xp.xplaneValues, dataref)
			xp.valuesMutex.Unlock()
			delete(xp.datarefs, idx)
		}
	} else if freq > 0 {
		idx = xp.datarefIdx
		xp.datarefs[xp.datarefIdx] = dataref
		xp.datarefIdx++
	}

	buf := new(bytes.Buffer)
	buf.Write([]byte("RREF\x00"))
	binary.Write(buf, binary.LittleEndian, int32(freq))
	binary.Write(buf, binary.LittleEndian, int32(idx))

	datarefBytes := make([]byte, 400)
	copy(datarefBytes, dataref)
	buf.Write(datarefBytes)

	if _, err := xp.socket.WriteTo(buf.Bytes(), xp.destAddr); err != nil {
		return fmt.Errorf("failed to send RREF command: %w", err)
	}
	return nil
}

// Listen starts listening for UDP packets in a background loop.
func (xp *XPlaneUdp) Listen() {
	buf := make([]byte, 2048)
	for {
		if xp.socket == nil {
			time.Sleep(1 * time.Second)
			continue
		}
		
		xp.socket.SetReadDeadline(time.Now().Add(5 * time.Second))
		n, _, err := xp.socket.ReadFromUDP(buf)
		if err != nil {
			if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
				// Timeout is expected if no data, just loop
				continue
			}
			log.Printf("Error reading from UDP: %v", err)
			continue
		}

		data := buf[:n]
		if bytes.HasPrefix(data, []byte("RREF,")) {
			valuesData := data[5:]
			lenValue := 8
			numValues := len(valuesData) / lenValue

			xp.valuesMutex.Lock()
			for i := 0; i < numValues; i++ {
				singleData := valuesData[i*lenValue : (i+1)*lenValue]
				var idx int32
				var value float32

				r := bytes.NewReader(singleData)
				binary.Read(r, binary.LittleEndian, &idx)
				binary.Read(r, binary.LittleEndian, &value)

				if name, ok := xp.datarefs[int(idx)]; ok {
					if value < 0.0 && value > -0.001 {
						value = 0.0
					}
					xp.xplaneValues[name] = value
				}
			}
			xp.valuesMutex.Unlock()
		}
	}
}

// GetValueSafe returns the current value for a dataref and whether it exists.
func (xp *XPlaneUdp) GetValueSafe(dataref string) (float32, bool) {
	xp.valuesMutex.RLock()
	defer xp.valuesMutex.RUnlock()
	val, ok := xp.xplaneValues[dataref]
	return val, ok
}

// GetString fetches a string dataref by subscribing to individual byte indices.
// It subscribes, waits for data, constructs the string, and unsubscribes.
func (xp *XPlaneUdp) GetString(datarefBase string, length int) string {
	// 1. Subscribe to all indices
	// To avoid UDP packet loss/overload, we batch the subscriptions slightly or just send them.
	// X-Plane handles bursts reasonably well.
	for i := 0; i < length; i++ {
		dr := fmt.Sprintf("%s[%d]", datarefBase, i)
		xp.AddDataRef(dr, 1) // 1 Hz is enough, we just need one value
	}

	// 2. Wait for data
	// We wait until we have a contiguous set of bytes up to a null terminator or length
	timeout := time.After(3 * time.Second)
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()

	var result []byte
	
	// Inner loop to check readiness
Loop:
	for {
		select {
		case <-timeout:
			break Loop
		case <-ticker.C:
			currentBytes := make([]byte, 0, length)
			complete := false
			for i := 0; i < length; i++ {
				dr := fmt.Sprintf("%s[%d]", datarefBase, i)
				val, ok := xp.GetValueSafe(dr)
				if !ok {
					// Missing a byte, keep waiting
					break
				}
				b := byte(val)
				if b == 0 {
					complete = true
					break // Null terminator found
				}
				currentBytes = append(currentBytes, b)
				if i == length-1 {
					complete = true // Reached max length
				}
			}
			if complete {
				result = currentBytes
				break Loop
			}
		}
	}

	// 3. Unsubscribe
	for i := 0; i < length; i++ {
		dr := fmt.Sprintf("%s[%d]", datarefBase, i)
		xp.AddDataRef(dr, 0)
	}

	return string(result)
}

// ManageAircraftUpdates monitors for aircraft changes and sends updates to Volanta.
func (xp *XPlaneUdp) ManageAircraftUpdates(volanta *VolantaClient) {
	// We monitor ICAO changes by keeping a persistent subscription to the first few chars of ICAO.
	// sim/aircraft/view/acf_ICAO is 40 chars max.
	
	// Initial subscription to ICAO trigger
	icaoTriggerRefs := []string{
		"sim/aircraft/view/acf_ICAO[0]",
		"sim/aircraft/view/acf_ICAO[1]",
		"sim/aircraft/view/acf_ICAO[2]",
		"sim/aircraft/view/acf_ICAO[3]",
	}
	for _, dr := range icaoTriggerRefs {
		xp.AddDataRef(dr, 1)
	}

	var lastICAO string
	
	// Regex for registration extraction (ported from C++)
	regRegex := regexp.MustCompile(`[A-Z]-[A-Z]{4}|([A-Z]|[1-9]){2}-[A-Z]{3}|N[0-9]{1,5}[A-Z]{0,2}`)

	ticker := time.NewTicker(2 * time.Second)
	defer ticker.Stop()

	for range ticker.C {
		// Reconstruct current ICAO prefix
		currentICAOBytes := make([]byte, 0, 4)
		for _, dr := range icaoTriggerRefs {
			val, ok := xp.GetValueSafe(dr)
			if ok && byte(val) != 0 {
				currentICAOBytes = append(currentICAOBytes, byte(val))
			}
		}
		currentICAOPrefix := string(currentICAOBytes)

		// If we have some data and it looks different (or we haven't sent anything yet)
		// Note: This is a fuzzy check. A robust check fetches the full string if the prefix changes.
		if currentICAOPrefix != "" && currentICAOPrefix != lastICAO {
			log.Printf("Aircraft change detected (ICAO prefix: %s). Fetching details...", currentICAOPrefix)
			
			// Fetch full details
			fullICAO := xp.GetString("sim/aircraft/view/acf_ICAO", 40)
			tailNum := xp.GetString("sim/aircraft/view/acf_tailnum", 40)
			liveryPath := xp.GetString("sim/aircraft/view/acf_livery_path", 255)
			
			// Extract Registration
			// Try Livery Path first
			reg := ""
			// Clean up livery path (sometimes comes with path separators)
			// The regex searches the whole string, so path separators are fine.
			match := regRegex.FindString(liveryPath)
			if match != "" {
				reg = match
			} else {
				// Fallback to tailnum
				reg = tailNum
			}

			// Send Update
			log.Printf("Sending Aircraft Update: ICAO=%s, Reg=%s", fullICAO, reg)
			
			msg := map[string]interface{}{
				"type": "STREAM",
				"name": "AIRCRAFT_UPDATE",
				"data": map[string]string{
					"title":        "",
					"type":         fullICAO,
					"model":        fullICAO,
					"registration": reg,
					"airline":      "",
				},
			}
			
			jsonBytes, _ := json.Marshal(msg)
			if err := volanta.Send(jsonBytes); err != nil {
				log.Printf("Failed to send aircraft update: %v", err)
			} else {
				lastICAO = currentICAOPrefix // Update last seen only on success? or always.
				// If fullICAO is short (e.g. 3 chars), lastICAO (prefix) might match fullICAO.
				// We update lastICAO to the prefix we monitor.
				if len(fullICAO) > 0 && len(fullICAO) < 4 {
					lastICAO = fullICAO
				} else if len(fullICAO) >= 4 {
					lastICAO = fullICAO[:4]
				} else {
					lastICAO = currentICAOPrefix
				}
			}
		}
	}
}

// VolantaClient handles sending data to Volanta.
type VolantaClient struct {
	conn net.Conn
}

func (vc *VolantaClient) Connect() error {
	var err error
	vc.conn, err = net.Dial("tcp", VOLANTA_TCP_ADDR)
	if err != nil {
		return err
	}
	return nil
}

func (vc *VolantaClient) Send(data []byte) error {
	if vc.conn == nil {
		if err := vc.Connect(); err != nil {
			return err
		}
	}
	_, err := vc.conn.Write(data)
	if err != nil {
		vc.conn.Close()
		vc.conn = nil
		return err
	}
	return nil
}

func main() {
	log.Println("OpenVolanta UDP Bridge starting...")
	xp := NewXPlaneUdp()

	// 1. Find X-Plane
	log.Println("Looking for X-Plane...")
	beacon, err := xp.FindIp()
	if err != nil {
		log.Fatalf("Failed to find X-Plane: %v", err)
	}
	log.Printf("Found X-Plane at %s:%d (Hostname: %s)", beacon.IP, beacon.Port, beacon.Hostname)

	// 2. Initialize UDP Socket
	if err := xp.InitSocket(); err != nil {
		log.Fatalf("Failed to initialize socket: %v", err)
	}
	defer xp.Close()

	// 3. Subscribe to DataRefs
	datarefs := []string{
		"sim/flightmodel/position/elevation", // alt_amsl
		"sim/flightmodel/position/y_agl",     // alt_agl
		"sim/flightmodel/position/latitude",
		"sim/flightmodel/position/longitude",
		"sim/flightmodel/position/theta", // pitch
		"sim/flightmodel/position/phi",   // bank
		"sim/flightmodel/position/psi",   // heading
		"sim/flightmodel/position/groundspeed",
		"sim/flightmodel/position/vh_ind_fpm", // vs
		"sim/flightmodel/weight/m_fuel_total", // fuel
		"sim/physics/gravity_normal",          // gravity
		"sim/cockpit/radios/transponder_code",
		"sim/flightmodel/failures/onground_any",
		"sim/operation/override/override_planepath", // slew
		"sim/time/paused",
		"sim/operation/prefs/replay_mode",
		"sim/graphics/view/framerate_period", // fps = 1/period
		"sim/time/time_accel",
		"sim/cockpit/autopilot/autopilot_mode",
		"sim/flightmodel/engine/ENGN_running",
		"sim/cockpit2/controls/parking_brake_ratio",
		"sim/weather/wind_speed_kt",
		"sim/weather/wind_direction_degt",
	}

	for _, dr := range datarefs {
		if err := xp.AddDataRef(dr, xp.defaultFreq); err != nil {
			log.Printf("Failed to subscribe to %s: %v", dr, err)
		}
	}

	// 4. Start Listening
	go xp.Listen()

	// 5. Setup Volanta Client
	volanta := &VolantaClient{}

	// Start Aircraft Update Manager
	go xp.ManageAircraftUpdates(volanta)
	
	// 6. Main Loop
	ticker := time.NewTicker(time.Second / time.Duration(UPDATE_FREQ))
	defer ticker.Stop()

	log.Printf("Bridge running. Forwarding data to %s", VOLANTA_TCP_ADDR)

	for range ticker.C {
		// Prepare data
		// Helper to get value or 0.0
		val := func(name string) float32 {
			v, _ := xp.GetValueSafe(name)
			return v
		}

		altA := float64(val("sim/flightmodel/position/elevation"))
		altG := float64(val("sim/flightmodel/position/y_agl"))
		lat := float64(val("sim/flightmodel/position/latitude"))
		lon := float64(val("sim/flightmodel/position/longitude"))
		pitch := val("sim/flightmodel/position/theta")
		bank := val("sim/flightmodel/position/phi")
		hdg := val("sim/flightmodel/position/psi")
		gs := val("sim/flightmodel/position/groundspeed")
		vs := val("sim/flightmodel/position/vh_ind_fpm")
		fuel := val("sim/flightmodel/weight/m_fuel_total")
		grav := val("sim/physics/gravity_normal")
		
		xpndr := int(val("sim/cockpit/radios/transponder_code"))
		og := val("sim/flightmodel/failures/onground_any") > 0.5
		slew := val("sim/operation/override/override_planepath") > 0.5
		paused := val("sim/time/paused") > 0.5
		replay := val("sim/operation/prefs/replay_mode") > 0.5
		
		// fpsPeriod := val("sim/graphics/view/framerate_period")
		// fps := float32(0)
		// if fpsPeriod > 0 {
		// 	fps = 1.0 / fpsPeriod
		// }
		fps := 144.0 // Hardcoded as in C++ version for now

		taccel := val("sim/time/time_accel")
		ap_eng := val("sim/cockpit/autopilot/autopilot_mode") > 0
		eng_run := val("sim/flightmodel/engine/ENGN_running") > 0.5
		brake := val("sim/cockpit2/controls/parking_brake_ratio") > 0.5
		windS := val("sim/weather/wind_speed_kt")
		windD := val("sim/weather/wind_direction_degt")

		// Create Map for JSON structure matching the C++ sprintf
		dataMap := map[string]interface{}{
			"altitude_amsl":     altA * METERS_TO_FT,
			"altitude_agl":      altG * METERS_TO_FT,
			"latitude":          lat,
			"longitude":         lon,
			"pitch":             pitch,
			"bank":              bank,
			"heading_true":      hdg,
			"ground_speed":      gs,
			"vertical_speed":    vs,
			"fuel_kg":           fuel,
			"gravity":           grav,
			"transponder":       fmt.Sprintf("%04d", xpndr),
			"on_ground":         og,
			"slew":              slew,
			"paused":            paused,
			"in_replay_mode":    replay,
			"fps":               fps,
			"time_acceleration": taccel,
			"autopilot_engaged": ap_eng,
			"engines_running":   eng_run,
			"parking_brake":     brake,
			"sim_abbreviation":  "xp12",
			"sim_version":       "12.320",
			"wind_speed":        windS,
			"wind_direction":    windD,
		}

		msg := map[string]interface{}{
			"type": "STREAM",
			"name": "POSITION_UPDATE",
			"data": dataMap,
		}

		jsonBytes, err := json.Marshal(msg)
		if err != nil {
			log.Printf("JSON Error: %v", err)
			continue
		}

		if err := volanta.Send(jsonBytes); err != nil {
			// Quietly fail connection errors (will retry next tick)
			// log.Printf("Volanta Send Error: %v", err)
		}
	}
}
