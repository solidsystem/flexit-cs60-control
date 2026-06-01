//go:build !linux

package main

import (
	"fmt"
	"runtime"
)

// setupPairing is a no-op on non-Linux platforms.
//
// On macOS (CoreBluetooth) and Windows (WinRT) the operating system owns BLE
// pairing: when the firmware requires an encrypted link, the OS pops up its own
// pairing dialog and the user types the 6-digit passkey by hand. tinygo's
// bluetooth package does not expose a passkey callback on these platforms, so
// there is nothing for us to register — and the $FLEXIT_CS60_CONTROL_BLE_KEY
// environment variable is not consulted here.
func setupPairing() {}

// removeBond cannot remove a BLE bond programmatically on these platforms:
// CoreBluetooth (macOS) and WinRT (Windows) do not expose an API to delete a
// stored pairing, and tinygo's bluetooth package surfaces none either. Print
// the manual steps instead.
func removeBond() {
	fmt.Println("Removing a BLE bond programmatically is not supported on this platform.")
	switch runtime.GOOS {
	case "darwin":
		fmt.Println("On macOS: open System Settings > Bluetooth, find \"" + deviceName + "\",")
		fmt.Println("click the (i) button, choose \"Forget This Device\", then confirm.")
	case "windows":
		fmt.Println("On Windows: open Settings > Bluetooth & devices, find \"" + deviceName + "\",")
		fmt.Println("click the \"...\" menu, choose \"Remove device\", then confirm.")
	default:
		fmt.Println("Remove the \"" + deviceName + "\" pairing via your OS Bluetooth settings.")
	}
}
