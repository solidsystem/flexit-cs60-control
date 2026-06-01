//go:build !linux

package main

// setupPairing is a no-op on non-Linux platforms.
//
// On macOS (CoreBluetooth) and Windows (WinRT) the operating system owns BLE
// pairing: when the firmware requires an encrypted link, the OS pops up its own
// pairing dialog and the user types the 6-digit passkey by hand. tinygo's
// bluetooth package does not expose a passkey callback on these platforms, so
// there is nothing for us to register — and the $FLEXIT_CS60_CONTROL_BLE_KEY
// environment variable is not consulted here.
func setupPairing() {}
