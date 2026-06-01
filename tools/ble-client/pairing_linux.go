//go:build linux

package main

import (
	"fmt"
	"log"
	"os"
	"strconv"

	"github.com/godbus/dbus/v5"
	"github.com/godbus/dbus/v5/introspect"
)

// On Linux, BLE pairing goes through BlueZ over D-Bus. BlueZ has no interactive
// passkey prompt of its own, so we register an agent that answers passkey
// requests with $FLEXIT_CS60_CONTROL_BLE_KEY — the same 6-digit number the
// firmware bakes in at build time.

const (
	agentPath     = dbus.ObjectPath("/flexit_cs60_control/agent")
	blePasskeyEnv = "FLEXIT_CS60_CONTROL_BLE_KEY"
)

// blePasskey is the pairing passkey, loaded from $FLEXIT_CS60_CONTROL_BLE_KEY by
// loadPasskey() before the pairing agent is registered.
var blePasskey uint32

// loadPasskey reads the BLE pairing passkey from the environment. It exits with
// a clear error if the variable is unset or not a 6-digit number.
func loadPasskey() uint32 {
	v := os.Getenv(blePasskeyEnv)
	if v == "" {
		log.Fatalf("%s is not set; export the 6-digit BLE pairing passkey to connect", blePasskeyEnv)
	}
	n, err := strconv.ParseUint(v, 10, 32)
	if err != nil || len(v) != 6 {
		log.Fatalf("%s=%q must be a 6-digit number", blePasskeyEnv, v)
	}
	return uint32(n)
}

type pairingAgent struct{}

func (a *pairingAgent) RequestPasskey(device dbus.ObjectPath) (uint32, *dbus.Error) {
	fmt.Printf("Pairing: providing passkey to %s\n", device)
	return blePasskey, nil
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
	blePasskey = loadPasskey()
	agent := &pairingAgent{}
	conn.Export(agent, agentPath, "org.bluez.Agent1")
	conn.Export(introspect.Introspectable(""), agentPath, "org.freedesktop.DBus.Introspectable")
	mgr := conn.Object("org.bluez", "/org/bluez")
	must("register agent", mgr.Call("org.bluez.AgentManager1.RegisterAgent", 0, agentPath, "KeyboardOnly").Err)
	must("default agent", mgr.Call("org.bluez.AgentManager1.RequestDefaultAgent", 0, agentPath).Err)
}

// setupPairing registers the BlueZ pairing agent so that pairing requests are
// answered automatically with the environment-supplied passkey.
func setupPairing() {
	sysbus, err := dbus.SystemBus()
	must("system dbus", err)
	registerPairingAgent(sysbus)
}

// removeBond deletes the BlueZ bond/keys for the xiao_ble, if BlueZ knows about
// it. It enumerates BlueZ's managed objects, finds any org.bluez.Device1 whose
// Name/Alias matches deviceName, and calls Adapter1.RemoveDevice on its owning
// adapter — which drops the device along with its stored pairing keys. The
// device's own half of the bond is unaffected (clear that on the firmware side
// for a full both-ends reset).
func removeBond() {
	sysbus, err := dbus.SystemBus()
	must("system dbus", err)

	var objects map[dbus.ObjectPath]map[string]map[string]dbus.Variant
	err = sysbus.Object("org.bluez", "/").
		Call("org.freedesktop.DBus.ObjectManager.GetManagedObjects", 0).Store(&objects)
	must("get managed objects", err)

	removed := 0
	for path, ifaces := range objects {
		props, ok := ifaces["org.bluez.Device1"]
		if !ok {
			continue
		}

		name, _ := props["Name"].Value().(string)
		alias, _ := props["Alias"].Value().(string)
		if name != deviceName && alias != deviceName {
			continue
		}

		adapterPath, ok := props["Adapter"].Value().(dbus.ObjectPath)
		if !ok {
			log.Printf("device %s has no Adapter property; skipping", path)
			continue
		}
		paired, _ := props["Paired"].Value().(bool)

		if callErr := sysbus.Object("org.bluez", adapterPath).
			Call("org.bluez.Adapter1.RemoveDevice", 0, path).Err; callErr != nil {
			log.Fatalf("remove device %s: %v", path, callErr)
		}

		if paired {
			fmt.Printf("Removed bond for %s (%s)\n", deviceName, path)
		} else {
			fmt.Printf("Removed %s (%s) — was known but not bonded\n", deviceName, path)
		}
		removed++
	}

	if removed == 0 {
		fmt.Printf("No %s device known to BlueZ — nothing to remove.\n", deviceName)
	}
}
