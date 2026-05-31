// rs485-decode: analyse a raw RS485/Modbus capture (as produced by
// `ble-client stream`) to find the command a panel issued — in particular the
// FC65 "reset coil + stamp register" acks (which carry reg_addr + value) and
// the FC10 status-broadcast setpoint registers (0x00BE / 0x00C2) over time.
//
// Usage:  go run . <capture.bin>
package main

import (
	"fmt"
	"os"
	"strconv"
)

// Modbus RTU CRC16 (poly 0xA001, init 0xFFFF), little-endian on the wire.
func crc16(b []byte) uint16 {
	crc := uint16(0xFFFF)
	for _, x := range b {
		crc ^= uint16(x)
		for i := 0; i < 8; i++ {
			if crc&1 != 0 {
				crc = (crc >> 1) ^ 0xA001
			} else {
				crc >>= 1
			}
		}
	}
	return crc
}

// crcOK reports whether the last two bytes of frame are a valid CRC of the rest.
func crcOK(frame []byte) bool {
	if len(frame) < 4 {
		return false
	}
	n := len(frame)
	want := uint16(frame[n-2]) | uint16(frame[n-1])<<8
	return crc16(frame[:n-2]) == want
}

func be16(b []byte, off int) uint16 { return uint16(b[off])<<8 | uint16(b[off+1]) }

// candidateLengths returns plausible total frame lengths at buf[i:], by
// function code and embedded byte-count fields. CRC then disambiguates.
func candidateLengths(buf []byte, i int) []int {
	if i+2 > len(buf) {
		return nil
	}
	fc := buf[i+1]
	switch fc {
	case 0x01, 0x02, 0x03, 0x04: // read*: request=8B, response=3+bc+2
		out := []int{8}
		if i+2 < len(buf) {
			bc := int(buf[i+2])
			out = append(out, bc+5)
		}
		return out
	case 0x05, 0x06: // write single coil/reg: 8B
		return []int{8}
	case 0x0F, 0x10: // write multiple: addr fc AH AL QH QL BC data CRC
		if i+7 <= len(buf) {
			bc := int(buf[i+6])
			return []int{bc + 9}
		}
		return nil
	case 0x65: // Flexit proprietary reset coil+reg: 8B
		return []int{8}
	default:
		return nil
	}
}

type fc65 struct {
	reg, val uint16
}

func tempStr(raw uint16) string {
	// Panel temps are int16 ×10 °C.
	return fmt.Sprintf("%.1f", float64(int16(raw))/10.0)
}

func main() {
	if len(os.Args) != 2 && len(os.Args) != 4 {
		fmt.Fprintln(os.Stderr, "usage: go run . <capture.bin> [winStart winEnd]")
		fmt.Fprintln(os.Stderr, "  winStart/winEnd: optional valid-frame range to raw-hex dump")
		os.Exit(2)
	}
	winStart, winEnd := 0, 0
	if len(os.Args) == 4 {
		winStart, _ = strconv.Atoi(os.Args[2])
		winEnd, _ = strconv.Atoi(os.Args[3])
	}
	buf, err := os.ReadFile(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	var (
		total, valid int
		fcCount      = map[byte]int{}
		fc65s        []fc65
		// FC03 req->resp pairing: remember last request's (slave,reg).
		pendSlave, pendReg = -1, -1
		fc03Trans          []string // value transitions read at (slave,reg)
		fc03Last           = map[string]int{}
		// FC06 single-reg writes: distinct (reg->val) transitions.
		fc06Last  = map[uint16]int{}
		fc06Trans []string
		// FC01 responses with a non-zero coil bitmap.
		coilEvents  []string
		lastCoilSig string
		// FC10 status setpoint timeline: only record on change.
		lastBE, lastC2 = -1, -1
		beTimeline     []string
		windowDump     []string
	)

	i := 0
	for i < len(buf)-3 {
		matched := false
		for _, L := range candidateLengths(buf, i) {
			if i+L > len(buf) || L < 4 {
				continue
			}
			f := buf[i : i+L]
			if !crcOK(f) {
				continue
			}
			total++
			valid++
			fc := f[1]
			fcCount[fc]++

			switch fc {
			case 0x65: // 00 65 AH AL VH VL CRC
				reg, val := be16(f, 2), be16(f, 4)
				fc65s = append(fc65s, fc65{reg, val})
			case 0x01: // ReadCoils response: addr 01 BC <bitmap> CRC
				if L > 8 { // response (has byte count), not the 8B request
					bc := int(f[2])
					bitmap := f[3 : 3+bc]
					var set []int
					for bit := 0; bit < bc*8; bit++ {
						if bitmap[bit/8]&(1<<(bit%8)) != 0 {
							set = append(set, bit)
						}
					}
					if len(set) > 0 {
						sig := fmt.Sprintf("addr0x%02X%v", f[0], set)
						if sig != lastCoilSig {
							coilEvents = append(coilEvents, fmt.Sprintf(
								"  frame %d: slave 0x%02X coils set: %v", valid, f[0], set))
							lastCoilSig = sig
						}
					}
				}
			case 0x03:
				if L == 8 { // request: addr 03 AH AL QH QL CRC
					pendSlave, pendReg = int(f[0]), int(be16(f, 2))
				} else if L >= 7 && pendReg >= 0 { // response: addr 03 BC <data> CRC
					bc := int(f[2])
					if bc >= 2 {
						val := int(be16(f, 3))
						key := fmt.Sprintf("0x%02X@0x%04X", pendSlave, pendReg)
						if fc03Last[key] != val || len(fc03Trans) == 0 {
							fc03Trans = append(fc03Trans, fmt.Sprintf(
								"  frame %d: read %s = 0x%04X (%d)  [as temp×10: %s°C]",
								valid, key, val, val, tempStr(uint16(val))))
							fc03Last[key] = val
						}
					}
					pendReg = -1
				}
			case 0x06: // WriteSingleReg (broadcast): 00 06 AH AL VH VL CRC
				reg, val := be16(f, 2), int(be16(f, 4))
				if v, ok := fc06Last[reg]; !ok || v != val {
					fc06Trans = append(fc06Trans, fmt.Sprintf(
						"  frame %d: FC06 reg 0x%04X = 0x%04X (%d)", valid, reg, val, val))
					fc06Last[reg] = val
				}
			}
			if fc == 0x10 { // status broadcast: 00 10 00 BE <qty> <bc> <data...>
				if f[0] == 0x00 && be16(f, 2) == 0x00BE {
					// data starts at offset 7; reg0 = 0x00BE.
					data := f[7 : len(f)-2]
					regAt := func(addr int) (uint16, bool) {
						off := (addr - 0x00BE) * 2
						if off+2 <= len(data) {
							return be16(data, off), true
						}
						return 0, false
					}
					be, okBE := regAt(0x00BE)
					c2, okC2 := regAt(0x00C2)
					if okBE && okC2 && (int(be) != lastBE || int(c2) != lastC2) {
						beTimeline = append(beTimeline, fmt.Sprintf(
							"  frame %d: TEMP_SETPOINT(0x00BE)=%s°C  TEMP_SETPOINT_2(0x00C2)=%s°C",
							valid, tempStr(be), tempStr(c2)))
						lastBE, lastC2 = int(be), int(c2)
					}
				}
			}
			// Optional windowed raw dump (valid-frame range from argv) to
			// inspect a command's FC01/FC03/FC65 frames in context.
			if winStart > 0 && valid >= winStart && valid <= winEnd {
				hexs := ""
				for _, b := range f {
					hexs += fmt.Sprintf("%02X ", b)
				}
				windowDump = append(windowDump,
					fmt.Sprintf("  f%-4d FC%02X len%-2d  %s", valid, fc, L, hexs))
			}

			i += L
			matched = true
			break
		}
		if !matched {
			i++
		}
	}

	fmt.Printf("=== %s (%d bytes) ===\n", os.Args[1], len(buf))
	fmt.Printf("valid frames: %d\n\n", valid)

	fmt.Println("frame counts by function code:")
	for _, fc := range []byte{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0F, 0x10, 0x65} {
		if n := fcCount[fc]; n > 0 {
			fmt.Printf("  FC%02X: %d\n", fc, n)
		}
	}

	fmt.Println("\nFC65 acks (reg <- value) — distinct register/value transitions:")
	var lastReg, lastVal = -1, -1
	for _, a := range fc65s {
		if int(a.reg) == lastReg && int(a.val) == lastVal {
			continue
		}
		note := ""
		switch a.reg {
		case 0x0000:
			note = "  (CMD_MODE)"
		case 0x00BE, 0x00C2:
			note = fmt.Sprintf("  (TEMP_SETPOINT reg, value=%s°C)", tempStr(a.val))
		default:
			note = fmt.Sprintf("  (value as temp ×10 = %s°C)", tempStr(a.val))
		}
		fmt.Printf("  reg 0x%04X <- 0x%04X (%d)%s\n", a.reg, a.val, a.val, note)
		lastReg, lastVal = int(a.reg), int(a.val)
	}
	if len(fc65s) == 0 {
		fmt.Println("  (none)")
	}

	fmt.Println("\nFC03 reads (CS60 -> slave) — value transitions:")
	if len(fc03Trans) == 0 {
		fmt.Println("  (none)")
	}
	for _, l := range fc03Trans {
		fmt.Println(l)
	}

	fmt.Println("\nFC06 single-reg writes — value transitions:")
	if len(fc06Trans) == 0 {
		fmt.Println("  (none)")
	}
	for _, l := range fc06Trans {
		fmt.Println(l)
	}

	fmt.Println("\nFC01 coil bitmaps with bits set (command flags):")
	if len(coilEvents) == 0 {
		fmt.Println("  (all-zero throughout — no slave raised a coil)")
	}
	for _, l := range coilEvents {
		fmt.Println(l)
	}

	fmt.Println("\nFC10 status-broadcast setpoint timeline (on change):")
	if len(beTimeline) == 0 {
		fmt.Println("  (no FC10 0x00BE status frames decoded)")
	}
	for _, l := range beTimeline {
		fmt.Println(l)
	}

	if winStart > 0 {
		fmt.Printf("\nRaw frame dump, valid frames %d-%d:\n", winStart, winEnd)
		for _, l := range windowDump {
			fmt.Println(l)
		}
	}
}
