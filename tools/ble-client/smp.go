package main

import (
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"sync"
	"time"

	"github.com/fxamacker/cbor/v2"
	"tinygo.org/x/bluetooth"
)

// SMP UUIDs
const (
	smpServiceUUID = "8d53dc1d-1db7-4cd3-868b-8a527460aa84"
	smpCharUUID    = "da2e7828-fbce-4e01-ae9e-261174997c48"
)

// SMP op codes
const (
	opReadReq  = 0
	opReadResp = 1
	opWriteReq = 2
	opWriteResp = 3
)

// Group / command IDs
const (
	groupOS    = 0
	groupImage = 1

	cmdOSReset    = 5
	cmdImageState = 0
	cmdImageUpload = 1
)

// smpHeader is the 8-byte SMP framing header.
type smpHeader struct {
	Op    uint8
	Flags uint8
	Len   uint16 // big-endian
	Group uint16 // big-endian
	Seq   uint8
	Id    uint8
}

func encodeSMPHeader(h smpHeader) []byte {
	b := make([]byte, 8)
	b[0] = h.Op
	b[1] = h.Flags
	binary.BigEndian.PutUint16(b[2:], h.Len)
	binary.BigEndian.PutUint16(b[4:], h.Group)
	b[6] = h.Seq
	b[7] = h.Id
	return b
}

func decodeSMPHeader(b []byte) (smpHeader, error) {
	if len(b) < 8 {
		return smpHeader{}, fmt.Errorf("SMP frame too short: %d bytes", len(b))
	}
	return smpHeader{
		Op:    b[0],
		Flags: b[1],
		Len:   binary.BigEndian.Uint16(b[2:]),
		Group: binary.BigEndian.Uint16(b[4:]),
		Seq:   b[6],
		Id:    b[7],
	}, nil
}

type smpTransport struct {
	char bluetooth.DeviceCharacteristic
	mu   sync.Mutex
	seq  uint8

	// current pending response assembly
	respMu   sync.Mutex
	respBuf  []byte
	respWant int
	respCh   chan []byte
}

func newSMPTransport(char bluetooth.DeviceCharacteristic) (*smpTransport, error) {
	t := &smpTransport{
		char:   char,
		respCh: make(chan []byte, 1),
	}
	err := char.EnableNotifications(func(buf []byte) {
		t.onNotify(buf)
	})
	if err != nil {
		return nil, fmt.Errorf("SMP enable notifications: %w", err)
	}
	return t, nil
}

func (t *smpTransport) onNotify(buf []byte) {
	t.respMu.Lock()
	defer t.respMu.Unlock()

	if len(t.respBuf) == 0 {
		// First fragment — parse header to know total length
		if len(buf) < 8 {
			return
		}
		h, err := decodeSMPHeader(buf)
		if err != nil {
			return
		}
		t.respWant = 8 + int(h.Len)
	}

	t.respBuf = append(t.respBuf, buf...)

	if len(t.respBuf) >= t.respWant {
		full := make([]byte, len(t.respBuf))
		copy(full, t.respBuf)
		t.respBuf = nil
		t.respWant = 0
		select {
		case t.respCh <- full:
		default:
		}
	}
}

func (t *smpTransport) call(op, group, id uint8, payload []byte, timeout time.Duration) ([]byte, error) {
	t.mu.Lock()
	defer t.mu.Unlock()

	seq := t.seq
	t.seq++

	hdr := encodeSMPHeader(smpHeader{
		Op:    op,
		Flags: 0,
		Len:   uint16(len(payload)),
		Group: uint16(group),
		Seq:   seq,
		Id:    id,
	})
	frame := append(hdr, payload...)

	// drain any stale response
	select {
	case <-t.respCh:
	default:
	}

	_, err := t.char.WriteWithoutResponse(frame)
	if err != nil {
		return nil, fmt.Errorf("SMP write: %w", err)
	}

	select {
	case resp := <-t.respCh:
		return resp[8:], nil // strip header, return CBOR payload
	case <-time.After(timeout):
		return nil, errors.New("SMP response timeout")
	}
}

// imageListRaw calls image state and returns the parsed map.
func (t *smpTransport) imageListRaw() (map[string]interface{}, error) {
	empty, _ := cbor.Marshal(map[string]interface{}{})
	resp, err := t.call(opReadReq, groupImage, cmdImageState, empty, 10*time.Second)
	if err != nil {
		return nil, err
	}
	var m map[string]interface{}
	if err := cbor.Unmarshal(resp, &m); err != nil {
		return nil, fmt.Errorf("decode imageList: %w", err)
	}
	return m, nil
}

// imageList returns a human-readable dump of the image slot state.
func (t *smpTransport) imageList() (string, error) {
	m, err := t.imageListRaw()
	if err != nil {
		return "", err
	}
	return fmt.Sprintf("%v", m), nil
}

// imageSlot1Hash reads the image list and extracts the slot-1 hash bytes.
func (t *smpTransport) imageSlot1Hash() ([]byte, error) {
	m, err := t.imageListRaw()
	if err != nil {
		return nil, err
	}
	images, ok := m["images"]
	if !ok {
		return nil, fmt.Errorf("no 'images' in response")
	}
	list, ok := images.([]interface{})
	if !ok {
		return nil, fmt.Errorf("unexpected 'images' type")
	}
	for _, img := range list {
		imgMap, ok := img.(map[interface{}]interface{})
		if !ok {
			// cbor/v2 may decode as map[string]interface{}
			if imgMap2, ok2 := img.(map[string]interface{}); ok2 {
				slot, _ := imgMap2["slot"].(uint64)
				if slot == 1 {
					return toByteSlice(imgMap2["hash"])
				}
				continue
			}
			continue
		}
		slot, _ := imgMap["slot"].(uint64)
		if slot == 1 {
			return toByteSlice(imgMap["hash"])
		}
	}
	return nil, fmt.Errorf("slot 1 not found in image list")
}

func toByteSlice(v interface{}) ([]byte, error) {
	switch val := v.(type) {
	case []byte:
		return val, nil
	case []interface{}:
		b := make([]byte, len(val))
		for i, x := range val {
			n, ok := x.(uint64)
			if !ok {
				return nil, fmt.Errorf("non-integer in hash array")
			}
			b[i] = byte(n)
		}
		return b, nil
	default:
		return nil, fmt.Errorf("unexpected hash type %T", v)
	}
}

// imageUpload uploads a raw signed image binary.
func (t *smpTransport) imageUpload(path string, mtu int) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return fmt.Errorf("read image: %w", err)
	}
	h := sha256.Sum256(data)
	fmt.Printf("Image: %s (%d bytes), SHA256: %s\n", path, len(data), hex.EncodeToString(h[:]))

	chunkSize := 128
	if mtu > 0 {
		// SMP header=8, CBOR overhead ~50 bytes for upload fields
		if cs := mtu - 8 - 60; cs > 0 {
			chunkSize = cs
		}
	}

	off := 0
	for off < len(data) {
		end := off + chunkSize
		if end > len(data) {
			end = len(data)
		}
		chunk := data[off:end]

		m := map[string]interface{}{
			"image": 0,
			"len":   len(data),
			"off":   off,
			"data":  chunk,
		}
		if off == 0 {
			m["sha"] = h[:]
		}

		payload, err := cbor.Marshal(m)
		if err != nil {
			return fmt.Errorf("encode upload chunk: %w", err)
		}

		resp, err := t.call(opWriteReq, groupImage, cmdImageUpload, payload, 30*time.Second)
		if err != nil {
			return fmt.Errorf("upload chunk at %d: %w", off, err)
		}

		var rm map[string]interface{}
		if err := cbor.Unmarshal(resp, &rm); err != nil {
			return fmt.Errorf("decode upload response: %w", err)
		}
		if rc, ok := rm["rc"]; ok {
			rcInt, _ := rc.(uint64)
			if rcInt != 0 {
				return fmt.Errorf("upload error rc=%d at off=%d", rcInt, off)
			}
		}

		// server returns next expected offset
		if nextOff, ok := rm["off"]; ok {
			if n, ok := nextOff.(uint64); ok {
				if int(n) <= off {
					return fmt.Errorf("upload stalled at off=%d", off)
				}
				off = int(n)
			} else {
				off = end
			}
		} else {
			off = end
		}

		pct := off * 100 / len(data)
		fmt.Printf("\rUploading... %d%% (%d/%d bytes)", pct, off, len(data))
	}
	fmt.Println()
	return nil
}

// imageTest marks slot-1 hash for test-boot.
func (t *smpTransport) imageTest(hash []byte) error {
	m := map[string]interface{}{
		"confirm": false,
		"hash":    hash,
	}
	payload, _ := cbor.Marshal(m)
	resp, err := t.call(opWriteReq, groupImage, cmdImageState, payload, 10*time.Second)
	if err != nil {
		return err
	}
	var rm map[string]interface{}
	if err := cbor.Unmarshal(resp, &rm); err != nil {
		return fmt.Errorf("decode imageTest: %w", err)
	}
	if rc, ok := rm["rc"]; ok {
		rcInt, _ := rc.(uint64)
		if rcInt != 0 {
			return fmt.Errorf("imageTest rc=%d", rcInt)
		}
	}
	return nil
}

// imageConfirm permanently confirms slot-0 after test boot.
func (t *smpTransport) imageConfirm(hash []byte) error {
	m := map[string]interface{}{
		"confirm": true,
		"hash":    hash,
	}
	payload, _ := cbor.Marshal(m)
	resp, err := t.call(opWriteReq, groupImage, cmdImageState, payload, 10*time.Second)
	if err != nil {
		return err
	}
	var rm map[string]interface{}
	if err := cbor.Unmarshal(resp, &rm); err != nil {
		return fmt.Errorf("decode imageConfirm: %w", err)
	}
	if rc, ok := rm["rc"]; ok {
		rcInt, _ := rc.(uint64)
		if rcInt != 0 {
			return fmt.Errorf("imageConfirm rc=%d", rcInt)
		}
	}
	return nil
}

// osReset sends a reset command.
func (t *smpTransport) osReset() error {
	empty, _ := cbor.Marshal(map[string]interface{}{})
	_, err := t.call(opWriteReq, groupOS, cmdOSReset, empty, 5*time.Second)
	return err
}
