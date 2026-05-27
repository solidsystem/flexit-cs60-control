package main

import (
	"encoding/hex"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/godbus/dbus/v5"
	"github.com/godbus/dbus/v5/introspect"
	"tinygo.org/x/bluetooth"
)

const (
	deviceName   = "flexitMC3"
	fixedPasskey = uint32(444999)
	agentPath    = dbus.ObjectPath("/flexitmc/agent")
	maxReplies   = 10
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

func (a *pairingAgent) Cancel() *dbus.Error { return nil }
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

func cmdStream() {
	sysbus, err := dbus.SystemBus()
	must("system dbus", err)
	registerPairingAgent(sysbus)

	device := connectBLE()
	defer device.Disconnect()

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

	var txChar, rxChar bluetooth.DeviceCharacteristic
	for _, c := range chars {
		switch c.UUID() {
		case bluetooth.CharacteristicUUIDUARTTX:
			txChar = c
		case bluetooth.CharacteristicUUIDUARTRX:
			rxChar = c
		}
	}

	msgCh := make(chan string, maxReplies)
	must("subscribe TX", txChar.EnableNotifications(func(buf []byte) {
		select {
		case msgCh <- string(buf):
		default:
		}
	}))

	fmt.Println("Sending 'start'...")
	_, werr := rxChar.WriteWithoutResponse([]byte("start"))
	must("write start", werr)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)

	count := 0
	for count < maxReplies {
		select {
		case msg := <-msgCh:
			fmt.Print(msg)
			count++
		case <-sig:
			fmt.Println("\nInterrupted")
			return
		}
	}

	fmt.Println("Sending 'stop'...")
	_, werr2 := rxChar.WriteWithoutResponse([]byte("stop"))
	must("write stop", werr2)
	fmt.Printf("Done — received %d replies.\n", count)
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

	// Step 1: list current images
	fmt.Println("Reading image list...")
	list, err := smp.imageList()
	must("imageList", err)
	fmt.Println("Images:", list)

	// Step 2: upload
	fmt.Println("Uploading image...")
	must("imageUpload", smp.imageUpload(imagePath, 0))
	fmt.Println("Upload done.")

	// Step 3: read slot-1 hash from device (MCUboot TLV hash, not file SHA256)
	fmt.Println("Reading slot-1 hash...")
	hash, err := smp.imageSlot1Hash()
	must("imageSlot1Hash", err)
	fmt.Printf("Slot-1 hash: %x\n", hash)

	// Step 4: mark for test
	fmt.Println("Marking image for test...")
	must("imageTest", smp.imageTest(hash))

	// Step 4: reset
	fmt.Println("Resetting device...")
	_ = smp.osReset() // device disconnects immediately, ignore timeout error

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
  %s stream               — NUS ping/reply test (sends 'start', receives 10 replies, sends 'stop')
  %s flash <image.bin>    — Upload signed firmware image via SMP over BLE
  %s confirm <hash-hex>   — Confirm image after test-boot (run after 'flash')
  %s list                 — List firmware images via SMP over BLE
  %s scan                 — Scan and print RSSI for flexitMC3 (Ctrl-C to stop)
`, os.Args[0], os.Args[0], os.Args[0], os.Args[0], os.Args[0])
	os.Exit(1)
}

func main() {
	if len(os.Args) < 2 {
		usage()
	}

	switch os.Args[1] {
	case "stream":
		cmdStream()
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
