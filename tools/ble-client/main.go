package main

import (
	"encoding/hex"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/godbus/dbus/v5"
	"github.com/godbus/dbus/v5/introspect"
	"tinygo.org/x/bluetooth"
)

const (
	deviceName   = "flexitMC"
	fixedPasskey = uint32(444999)
	agentPath    = dbus.ObjectPath("/flexitmc/agent")
)

var adapter = bluetooth.DefaultAdapter

type pairingAgent struct{}

func (a *pairingAgent) RequestPasskey(device dbus.ObjectPath) (uint32, *dbus.Error) {
	fmt.Printf("Pairing: providing passkey to %s\n", device)
	return fixedPasskey, nil
}

func (a *pairingAgent) DisplayPasskey(device dbus.ObjectPath, passkey uint32, entered uint16) *dbus.Error {
	return nil
}

func (a *pairingAgent) RequestConfirmation(device dbus.ObjectPath, passkey uint32) *dbus.Error {
	return nil
}

func (a *pairingAgent) RequestPinCode(device dbus.ObjectPath) (string, *dbus.Error) {
	return "", nil
}

func (a *pairingAgent) AuthorizeService(device dbus.ObjectPath, uuid string) *dbus.Error {
	return nil
}

func (a *pairingAgent) Cancel() *dbus.Error  { return nil }
func (a *pairingAgent) Release() *dbus.Error { return nil }

func registerPairingAgent(conn *dbus.Conn) {
	agent := &pairingAgent{}
	conn.Export(agent, agentPath, "org.bluez.Agent1")
	conn.Export(introspect.Introspectable(""), agentPath, "org.freedesktop.DBus.Introspectable")
	mgr := conn.Object("org.bluez", "/org/bluez")
	must("register agent", mgr.Call("org.bluez.AgentManager1.RegisterAgent", 0, agentPath, "KeyboardOnly").Err)
	must("default agent", mgr.Call("org.bluez.AgentManager1.RequestDefaultAgent", 0, agentPath).Err)
}

func connectBLE() bluetooth.Device {
	must("enable adapter", adapter.Enable())
	fmt.Printf("Scanning for %s...\n", deviceName)
	addrCh := make(chan bluetooth.Address, 1)
	go func() {
		must("scan", adapter.Scan(func(a *bluetooth.Adapter, r bluetooth.ScanResult) {
			if r.LocalName() == deviceName {
				fmt.Printf("Found %s at %s\n", deviceName, r.Address)
				addrCh <- r.Address
				a.StopScan()
			}
		}))
	}()
	targetAddr := <-addrCh
	fmt.Println("Connecting...")
	device, err := adapter.Connect(targetAddr, bluetooth.ConnectionParams{})
	must("connect", err)
	fmt.Println("Connected")
	return device
}

// openNUS discovers the Nordic UART Service and returns (txChar, rxChar).
// txChar carries device→client notifications; rxChar is written by the client.
func openNUS(device bluetooth.Device) (tx, rx bluetooth.DeviceCharacteristic) {
	srvcs, err := device.DiscoverServices([]bluetooth.UUID{bluetooth.ServiceUUIDNordicUART})
	must("discover NUS service", err)
	if len(srvcs) == 0 {
		log.Fatal("NUS service not found")
	}

	chars, err := srvcs[0].DiscoverCharacteristics([]bluetooth.UUID{
		bluetooth.CharacteristicUUIDUARTTX,
		bluetooth.CharacteristicUUIDUARTRX,
	})
	must("discover NUS characteristics", err)

	for _, c := range chars {
		switch c.UUID() {
		case bluetooth.CharacteristicUUIDUARTTX:
			tx = c
		case bluetooth.CharacteristicUUIDUARTRX:
			rx = c
		}
	}
	return tx, rx
}

// cmdStream connects, sends "stream", and appends every RS485 notification
// to outputPath until Ctrl-C. Sends "stop" before disconnecting.
func cmdStream(outputPath string) {
	sysbus, err := dbus.SystemBus()
	must("system dbus", err)
	registerPairingAgent(sysbus)

	device := connectBLE()
	defer device.Disconnect()

	txChar, rxChar := openNUS(device)

	f, err := os.OpenFile(outputPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	must("open output file", err)

	// Writer goroutine owns the file; closed once writeCh is drained.
	writeCh := make(chan []byte, 256)
	var wg sync.WaitGroup
	var written int64

	wg.Add(1)
	go func() {
		defer wg.Done()
		for data := range writeCh {
			n, werr := f.Write(data)
			if werr != nil {
				log.Printf("file write error: %v", werr)
			}
			written += int64(n)
		}
		f.Close()
	}()

	must("subscribe TX", txChar.EnableNotifications(func(buf []byte) {
		b := make([]byte, len(buf))
		copy(b, buf)
		select {
		case writeCh <- b:
		default:
			// channel full — notification dropped (very unlikely at 256 slots)
		}
	}))

	fmt.Println("Sending 'stream'...")
	_, werr := rxChar.WriteWithoutResponse([]byte("stream"))
	must("write stream", werr)

	fmt.Printf("Streaming RS485 data to %s  (Ctrl-C to stop)...\n", outputPath)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	<-sig

	fmt.Println("\nSending 'stop'...")
	rxChar.WriteWithoutResponse([]byte("stop"))        //nolint:errcheck
	time.Sleep(200 * time.Millisecond)                 // let last notifications arrive

	close(writeCh)
	wg.Wait()

	fmt.Printf("Done. Wrote %d bytes to %s\n", written, outputPath)
}

func cmdFlash(imagePath string) {
	sysbus, err := dbus.SystemBus()
	must("system dbus", err)
	registerPairingAgent(sysbus)

	device := connectBLE()
	defer device.Disconnect()

	smpSvcUUID, err := bluetooth.ParseUUID(smpServiceUUID)
	must("parse SMP service UUID", err)
	smpCharUUIDParsed, err := bluetooth.ParseUUID(smpCharUUID)
	must("parse SMP char UUID", err)

	srvcs, err := device.DiscoverServices([]bluetooth.UUID{smpSvcUUID})
	must("discover SMP service", err)
	if len(srvcs) == 0 {
		log.Fatal("SMP service not found — is CONFIG_MCUMGR_TRANSPORT_BT enabled in firmware?")
	}

	chars, err := srvcs[0].DiscoverCharacteristics([]bluetooth.UUID{smpCharUUIDParsed})
	must("discover SMP characteristic", err)
	if len(chars) == 0 {
		log.Fatal("SMP characteristic not found")
	}

	smp, err := newSMPTransport(chars[0])
	must("init SMP transport", err)

	fmt.Println("Reading image list...")
	list, err := smp.imageList()
	must("imageList", err)
	fmt.Println("Images:", list)

	fmt.Println("Uploading image...")
	// Size upload chunks off the negotiated ATT MTU (falls back to a safe
	// default if BlueZ doesn't expose it). With Data Length Extension and a
	// large MTU this is the difference between 128 B and ~400 B chunks.
	mtu, mErr := chars[0].GetMTU()
	if mErr != nil {
		fmt.Printf("(MTU query failed: %v — using default chunk size)\n", mErr)
		mtu = 0
	} else {
		fmt.Printf("Negotiated ATT MTU: %d\n", mtu)
	}
	must("imageUpload", smp.imageUpload(imagePath, int(mtu)))
	fmt.Println("Upload done.")

	fmt.Println("Reading slot-1 hash...")
	hash, err := smp.imageSlot1Hash()
	must("imageSlot1Hash", err)
	fmt.Printf("Slot-1 hash: %x\n", hash)

	fmt.Println("Marking image for test...")
	must("imageTest", smp.imageTest(hash))

	fmt.Println("Resetting device...")
	_ = smp.osReset()

	fmt.Printf("Device resetting. Slot-1 hash: %x\n", hash)
	fmt.Println("Wait ~10s for boot, then run:")
	fmt.Printf("  %s confirm %x\n", os.Args[0], hash)
}

func cmdConfirm(hashHex string) {
	hash, err := hex.DecodeString(hashHex)
	if err != nil {
		log.Fatalf("invalid hash: %v", err)
	}

	sysbus, err := dbus.SystemBus()
	must("system dbus", err)
	registerPairingAgent(sysbus)

	fmt.Println("Waiting 10s for device to boot...")
	time.Sleep(10 * time.Second)

	device := connectBLE()
	defer device.Disconnect()

	smpSvcUUID, _ := bluetooth.ParseUUID(smpServiceUUID)
	smpCharUUIDParsed, _ := bluetooth.ParseUUID(smpCharUUID)

	srvcs, err := device.DiscoverServices([]bluetooth.UUID{smpSvcUUID})
	must("discover SMP service", err)
	if len(srvcs) == 0 {
		log.Fatal("SMP service not found")
	}
	chars, err := srvcs[0].DiscoverCharacteristics([]bluetooth.UUID{smpCharUUIDParsed})
	must("discover SMP characteristic", err)
	if len(chars) == 0 {
		log.Fatal("SMP characteristic not found")
	}

	smp, err := newSMPTransport(chars[0])
	must("init SMP transport", err)

	fmt.Println("Confirming image...")
	must("imageConfirm", smp.imageConfirm(hash))
	fmt.Println("Image confirmed. Firmware update complete.")
}

// cmdState connects, sends "state", prints the snapshot returned by the
// firmware's panel_mirror module, then disconnects.
// If humanFriendly is true the compact firmware line is parsed and rendered
// as a multiline human-readable report; otherwise it is printed as-is.
func cmdState(humanFriendly bool) {
	sysbus, err := dbus.SystemBus()
	must("system dbus", err)
	registerPairingAgent(sysbus)

	device := connectBLE()
	defer device.Disconnect()

	txChar, rxChar := openNUS(device)

	notifyCh := make(chan []byte, 16)
	must("subscribe TX", txChar.EnableNotifications(func(buf []byte) {
		b := make([]byte, len(buf))
		copy(b, buf)
		notifyCh <- b
	}))

	_, werr := rxChar.WriteWithoutResponse([]byte("state"))
	must("write state", werr)

	// The firmware sends the full line in a single NUS notification, but
	// long ATT MTUs are not guaranteed on every BlueZ stack — keep
	// accumulating until we see a '\n' or hit the timeout.
	var line []byte
	deadline := time.After(3 * time.Second)
	for {
		select {
		case chunk := <-notifyCh:
			line = append(line, chunk...)
			if i := indexByte(line, '\n'); i >= 0 {
				s := string(line[:i])
				if humanFriendly {
					formatStateHuman(s)
				} else {
					fmt.Println(s)
				}
				return
			}
		case <-deadline:
			if len(line) > 0 {
				s := strings.TrimRight(string(line), "\r\n")
				if humanFriendly {
					formatStateHuman(s)
				} else {
					fmt.Println(s)
				}
			} else {
				log.Fatal("Timeout waiting for state response")
			}
			return
		}
	}
}

// cmdMode connects, sends "mode N" over NUS RX, prints the one-line ack
// the firmware sends back on NUS TX, then disconnects. N must be 0..3.
func cmdMode(modeArg string) {
	mode, err := strconv.Atoi(modeArg)
	if err != nil || mode < 0 || mode > 3 {
		log.Fatalf("mode: bad arg %q — must be 0, 1, 2 or 3", modeArg)
	}

	sysbus, err := dbus.SystemBus()
	must("system dbus", err)
	registerPairingAgent(sysbus)

	device := connectBLE()
	defer device.Disconnect()

	txChar, rxChar := openNUS(device)

	notifyCh := make(chan []byte, 16)
	must("subscribe TX", txChar.EnableNotifications(func(buf []byte) {
		b := make([]byte, len(buf))
		copy(b, buf)
		notifyCh <- b
	}))

	cmd := fmt.Sprintf("mode %d", mode)
	fmt.Printf("Sending %q...\n", cmd)
	_, werr := rxChar.WriteWithoutResponse([]byte(cmd))
	must("write mode", werr)

	var line []byte
	deadline := time.After(3 * time.Second)
	for {
		select {
		case chunk := <-notifyCh:
			line = append(line, chunk...)
			if i := indexByte(line, '\n'); i >= 0 {
				fmt.Print(string(line[:i+1]))
				return
			}
		case <-deadline:
			if len(line) > 0 {
				fmt.Println(strings.TrimRight(string(line), "\r\n"))
			} else {
				log.Fatal("Timeout waiting for mode ack")
			}
			return
		}
	}
}

// cmdZbReset connects, sends "zbreset" over NUS RX, prints the one-line ack,
// then disconnects. The firmware leaves its Zigbee network, clears NVRAM and
// reboots a second or so later, so on the next boot it scans for and joins an
// open coordinator (e.g. open ZHA's "Add device" first). The BLE link drops as
// the device reboots — that is expected and not an error.
func cmdZbReset() {
	sysbus, err := dbus.SystemBus()
	must("system dbus", err)
	registerPairingAgent(sysbus)

	device := connectBLE()
	defer device.Disconnect()

	txChar, rxChar := openNUS(device)

	notifyCh := make(chan []byte, 16)
	must("subscribe TX", txChar.EnableNotifications(func(buf []byte) {
		b := make([]byte, len(buf))
		copy(b, buf)
		notifyCh <- b
	}))

	fmt.Println("Sending \"zbreset\"...")
	_, werr := rxChar.WriteWithoutResponse([]byte("zbreset"))
	must("write zbreset", werr)

	var line []byte
	deadline := time.After(3 * time.Second)
	for {
		select {
		case chunk := <-notifyCh:
			line = append(line, chunk...)
			if i := indexByte(line, '\n'); i >= 0 {
				fmt.Print(string(line[:i+1]))
				fmt.Println("Device is rebooting to start fresh Zigbee commissioning.")
				fmt.Println("Make sure ZHA \"Add device\" (permit join) is open so it can pair.")
				return
			}
		case <-deadline:
			if len(line) > 0 {
				fmt.Println(strings.TrimRight(string(line), "\r\n"))
			} else {
				log.Fatal("Timeout waiting for zbreset ack")
			}
			return
		}
	}
}

// formatStateHuman parses the compact one-line firmware state string and
// prints a verbose, multiline human-readable report to stdout.
//
// Expected firmware format (space-separated key=value tokens):
//
//	MODE=1 SET=19.6 SET2=19.6 T_SUP=19.5 T_EXT=n/a T_OUT=12.4 T_RET=n/a
//	PCT_COOL=0 PCT_HX=13 PCT_HEAT=0 PCT_FAN=50
//	UNK1=0x0DA8 UNK2=0x0000
//	FC10=182 FC06=84 CRCERR=0 LAST_MS=20746
//	CNT[0x014F]=3447@0s CNT[0x0155]=12929@0s …
func formatStateHuman(line string) {
	kv := make(map[string]string)
	var cntKeys []string // ordered list of CNT[…] keys

	for _, f := range strings.Fields(line) {
		idx := strings.IndexByte(f, '=')
		if idx < 0 {
			continue
		}
		key := f[:idx]
		val := f[idx+1:]
		kv[key] = val
		if strings.HasPrefix(key, "CNT[") {
			cntKeys = append(cntKeys, key)
		}
	}

	modeNames := map[string]string{
		"0": "Stop",
		"1": "Minimum",
		"2": "Normal",
		"3": "Maximum",
	}
	modeName, ok := modeNames[kv["MODE"]]
	if !ok {
		modeName = "Unknown"
	}

	sep := strings.Repeat("═", 56)
	fmt.Println(sep)
	fmt.Println(" Flexit Panel State")
	fmt.Println(sep)
	fmt.Println()

	// --- Operating mode ---
	fmt.Printf("  Mode:              %s — %s\n", kv["MODE"], modeName)
	fmt.Println()

	// --- Temperatures ---
	fmt.Println("  Temperatures")
	fmt.Printf("    Setpoint:        %s\n", formatTempHuman(kv["SET"]))
	fmt.Printf("    Setpoint 2:      %s\n", formatTempHuman(kv["SET2"]))
	fmt.Printf("    Supply air:      %s\n", formatTempHuman(kv["T_SUP"]))
	fmt.Printf("    Extract air:     %s\n", formatTempHuman(kv["T_EXT"]))
	fmt.Printf("    Outdoor air:     %s\n", formatTempHuman(kv["T_OUT"]))
	fmt.Printf("    Return water:    %s\n", formatTempHuman(kv["T_RET"]))
	fmt.Println()

	// --- Actuators ---
	fmt.Println("  Actuators")
	fmt.Printf("    Supply fan:      %s %%\n", kv["PCT_FAN"])
	fmt.Printf("    Heat exchanger:  %s %%\n", kv["PCT_HX"])
	fmt.Printf("    Heating:         %s %%\n", kv["PCT_HEAT"])
	fmt.Printf("    Cooling:         %s %%\n", kv["PCT_COOL"])
	fmt.Println()

	// --- Pending command (Modbus slave cycle) ---
	if _, ok := kv["MODE_QUEUED"]; ok {
		fmt.Println("  Pending command")
		queued := kv["MODE_QUEUED"]
		if queued == "none" {
			fmt.Println("    Queued:          —")
		} else if name, ok := modeNames[queued]; ok {
			fmt.Printf("    Queued:          %s — %s\n", queued, name)
		} else {
			fmt.Printf("    Queued:          %s\n", queued)
		}

		acked := kv["MODE_ACKED"]
		if acked == "" || acked == "none" {
			fmt.Println("    Acked:           —")
		} else {
			// MODE_ACKED format: "<value>@<ms>"
			at := strings.IndexByte(acked, '@')
			val := acked
			ts := ""
			if at >= 0 {
				val = acked[:at]
				ts = acked[at+1:]
			}
			name := modeNames[val]
			if name == "" {
				name = "?"
			}
			if ts != "" {
				if ms, err := strconv.ParseInt(ts, 10, 64); err == nil {
					fmt.Printf("    Acked:           %s — %s  (@ %.3f s uptime)\n",
						val, name, float64(ms)/1000.0)
				} else {
					fmt.Printf("    Acked:           %s — %s  (@ %s)\n", val, name, ts)
				}
			} else {
				fmt.Printf("    Acked:           %s — %s\n", val, name)
			}
		}

		fmt.Printf("    Coil 0 pending:  %s\n", kv["SLV_COIL0"])
		fmt.Printf("    Slave traffic:   FC01=%s  FC03=%s  FC04=%s  FC65=%s\n",
			kv["SLV_FC01"], kv["SLV_FC03"], kv["SLV_FC04"], kv["SLV_FC65"])
		fmt.Println()
	}

	// --- Counters (FC06 runtime registers) ---
	if len(cntKeys) > 0 {
		fmt.Println("  Runtime counters  (operating-minute registers)")
		for _, k := range cntKeys {
			addr := k[4 : len(k)-1] // strip "CNT[" prefix and "]" suffix
			val := kv[k]
			atIdx := strings.LastIndexByte(val, '@')
			if atIdx >= 0 {
				countVal := val[:atIdx]
				age := val[atIdx+1:]
				fmt.Printf("    %-8s  %s  (updated %s ago)\n", addr, countVal, age)
			} else {
				fmt.Printf("    %-8s  %s\n", addr, val)
			}
		}
		fmt.Println()
	}

	// --- Diagnostics ---
	fmt.Println("  Diagnostics")
	fmt.Printf("    FC10 frames:     %s\n", kv["FC10"])
	fmt.Printf("    FC06 frames:     %s\n", kv["FC06"])
	fmt.Printf("    CRC errors:      %s\n", kv["CRCERR"])
	if lastMs, err := strconv.ParseInt(kv["LAST_MS"], 10, 64); err == nil {
		fmt.Printf("    Last FC10 uptime:  %.3f s\n", float64(lastMs)/1000.0)
	}
	fmt.Printf("    Unknown reg 1:   %s\n", kv["UNK1"])
	fmt.Printf("    Unknown reg 2:   %s\n", kv["UNK2"])
	fmt.Println()

	fmt.Println(sep)
}

// formatTempHuman returns a display string for a temperature value.
// "n/a" is expanded to indicate the sensor is not present.
func formatTempHuman(v string) string {
	if v == "n/a" || v == "" {
		return "n/a  (sensor not present)"
	}
	return v + " °C"
}

func indexByte(b []byte, c byte) int {
	for i, x := range b {
		if x == c {
			return i
		}
	}
	return -1
}

func cmdScan() {
	must("enable adapter", adapter.Enable())
	fmt.Printf("Scanning for %s (Ctrl-C to stop)...\n", deviceName)
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sig
		adapter.StopScan()
	}()
	adapter.Scan(func(a *bluetooth.Adapter, r bluetooth.ScanResult) {
		if r.LocalName() == deviceName {
			fmt.Printf("%-20s  RSSI: %d dBm\n", r.Address, r.RSSI)
		}
	})
}

func cmdList() {
	sysbus, err := dbus.SystemBus()
	must("system dbus", err)
	registerPairingAgent(sysbus)

	device := connectBLE()
	defer device.Disconnect()

	smpSvcUUID, _ := bluetooth.ParseUUID(smpServiceUUID)
	smpCharUUIDParsed, _ := bluetooth.ParseUUID(smpCharUUID)

	srvcs, err := device.DiscoverServices([]bluetooth.UUID{smpSvcUUID})
	must("discover SMP service", err)
	if len(srvcs) == 0 {
		log.Fatal("SMP service not found")
	}
	chars, err := srvcs[0].DiscoverCharacteristics([]bluetooth.UUID{smpCharUUIDParsed})
	must("discover SMP characteristic", err)
	if len(chars) == 0 {
		log.Fatal("SMP characteristic not found")
	}

	smp, err := newSMPTransport(chars[0])
	must("init SMP transport", err)

	list, err := smp.imageList()
	must("imageList", err)
	fmt.Println(list)
}

func usage() {
	fmt.Fprintf(os.Stderr, `Usage:
  %s stream <output.bin>   — Stream live RS485 traffic to file (Ctrl-C to stop)
  %s state [--human-friendly]  — Print decoded panel state; --human-friendly for verbose multiline output
  %s mode <0|1|2|3>        — Queue a CMD_MODE change (Stop/Min/Normal/Max)
  %s zbreset               — Zigbee factory reset: leave network, clear NVRAM, reboot to re-pair
  %s flash <image.bin>     — Upload signed firmware image via SMP over BLE
  %s confirm <hash-hex>    — Confirm image after test-boot (run after 'flash')
  %s list                  — List firmware images via SMP over BLE
  %s scan                  — Scan and print RSSI for flexitMC (Ctrl-C to stop)
`, os.Args[0], os.Args[0], os.Args[0], os.Args[0], os.Args[0], os.Args[0], os.Args[0], os.Args[0])
	os.Exit(1)
}

func main() {
	if len(os.Args) < 2 {
		usage()
	}

	switch os.Args[1] {
	case "stream":
		if len(os.Args) < 3 {
			usage()
		}
		cmdStream(os.Args[2])
	case "flash":
		if len(os.Args) < 3 {
			usage()
		}
		cmdFlash(os.Args[2])
	case "confirm":
		if len(os.Args) < 3 {
			usage()
		}
		cmdConfirm(os.Args[2])
	case "state":
		humanFriendly := false
		for _, arg := range os.Args[2:] {
			if arg == "--human-friendly" {
				humanFriendly = true
			}
		}
		cmdState(humanFriendly)
	case "mode":
		if len(os.Args) < 3 {
			usage()
		}
		cmdMode(os.Args[2])
	case "zbreset":
		cmdZbReset()
	case "list":
		cmdList()
	case "scan":
		cmdScan()
	default:
		usage()
	}
}

func must(action string, err error) {
	if err != nil {
		log.Fatalf("%s: %v", action, err)
	}
}
