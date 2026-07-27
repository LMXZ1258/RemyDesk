//go:build linux

package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"strings"
	"sync"
	"syscall"
	"time"
)

const (
	evSyn = 0x00
	evKey = 0x01
	evRel = 0x02
	evAbs = 0x03

	synReport = 0x00

	btnLeft   = 0x110
	btnRight  = 0x111
	btnMiddle = 0x112

	relWheel = 0x08
	absX     = 0x00
	absY     = 0x01

	busUSB = 0x03

	uiSetEvbit   = 0x40045564
	uiSetKeybit  = 0x40045565
	uiSetRelbit  = 0x40045566
	uiSetAbsbit  = 0x40045567
	uiDevCreate  = 0x5501
	uiDevDestroy = 0x5502

	keyEsc        = 1
	key1          = 2
	key2          = 3
	key3          = 4
	key4          = 5
	key5          = 6
	key6          = 7
	key7          = 8
	key8          = 9
	key9          = 10
	key0          = 11
	keyMinus      = 12
	keyEqual      = 13
	keyBackspace  = 14
	keyTab        = 15
	keyQ          = 16
	keyW          = 17
	keyE          = 18
	keyR          = 19
	keyT          = 20
	keyY          = 21
	keyU          = 22
	keyI          = 23
	keyO          = 24
	keyP          = 25
	keyLeftBrace  = 26
	keyRightBrace = 27
	keyEnter      = 28
	keyLeftCtrl   = 29
	keyA          = 30
	keyS          = 31
	keyD          = 32
	keyF          = 33
	keyG          = 34
	keyH          = 35
	keyJ          = 36
	keyK          = 37
	keyL          = 38
	keySemicolon  = 39
	keyApostrophe = 40
	keyGrave      = 41
	keyLeftShift  = 42
	keyBackslash  = 43
	keyZ          = 44
	keyX          = 45
	keyC          = 46
	keyV          = 47
	keyB          = 48
	keyN          = 49
	keyM          = 50
	keyComma      = 51
	keyDot        = 52
	keySlash      = 53
	keyRightShift = 54
	keyLeftAlt    = 56
	keySpace      = 57
	keyCapsLock   = 58
	keyF1         = 59
	keyF2         = 60
	keyF3         = 61
	keyF4         = 62
	keyF5         = 63
	keyF6         = 64
	keyF7         = 65
	keyF8         = 66
	keyF9         = 67
	keyF10        = 68
	keyF11        = 87
	keyF12        = 88
	keyRightCtrl  = 97
	keyRightAlt   = 100
	keyHome       = 102
	keyUp         = 103
	keyPageUp     = 104
	keyLeft       = 105
	keyRight      = 106
	keyEnd        = 107
	keyDown       = 108
	keyPageDown   = 109
	keyInsert     = 110
	keyDelete     = 111
	keyLeftMeta   = 125
)

var keyCodeMap = map[string]uint16{
	"Escape": keyEsc, "Digit1": key1, "Digit2": key2, "Digit3": key3, "Digit4": key4,
	"Digit5": key5, "Digit6": key6, "Digit7": key7, "Digit8": key8, "Digit9": key9,
	"Digit0": key0, "Minus": keyMinus, "Equal": keyEqual, "Backspace": keyBackspace,
	"Tab": keyTab, "KeyQ": keyQ, "KeyW": keyW, "KeyE": keyE, "KeyR": keyR,
	"KeyT": keyT, "KeyY": keyY, "KeyU": keyU, "KeyI": keyI, "KeyO": keyO,
	"KeyP": keyP, "BracketLeft": keyLeftBrace, "BracketRight": keyRightBrace,
	"Enter": keyEnter, "ControlLeft": keyLeftCtrl, "KeyA": keyA, "KeyS": keyS,
	"KeyD": keyD, "KeyF": keyF, "KeyG": keyG, "KeyH": keyH, "KeyJ": keyJ,
	"KeyK": keyK, "KeyL": keyL, "Semicolon": keySemicolon, "Quote": keyApostrophe,
	"Backquote": keyGrave, "ShiftLeft": keyLeftShift, "Backslash": keyBackslash,
	"KeyZ": keyZ, "KeyX": keyX, "KeyC": keyC, "KeyV": keyV, "KeyB": keyB,
	"KeyN": keyN, "KeyM": keyM, "Comma": keyComma, "Period": keyDot, "Slash": keySlash,
	"ShiftRight": keyRightShift, "AltLeft": keyLeftAlt, "Space": keySpace,
	"CapsLock": keyCapsLock, "F1": keyF1, "F2": keyF2, "F3": keyF3, "F4": keyF4,
	"F5": keyF5, "F6": keyF6, "F7": keyF7, "F8": keyF8, "F9": keyF9,
	"F10": keyF10, "F11": keyF11, "F12": keyF12, "ControlRight": keyRightCtrl,
	"AltRight": keyRightAlt, "Home": keyHome, "ArrowUp": keyUp, "PageUp": keyPageUp,
	"ArrowLeft": keyLeft, "ArrowRight": keyRight, "End": keyEnd, "ArrowDown": keyDown,
	"PageDown": keyPageDown, "Insert": keyInsert, "Delete": keyDelete,
	"MetaLeft": keyLeftMeta, "MetaRight": keyLeftMeta,
}

var keyNameMap = map[string]uint16{
	"Escape": keyEsc, "Backspace": keyBackspace, "Tab": keyTab, "Enter": keyEnter,
	" ": keySpace, "ArrowUp": keyUp, "ArrowDown": keyDown, "ArrowLeft": keyLeft,
	"ArrowRight": keyRight, "Delete": keyDelete, "Home": keyHome, "End": keyEnd,
	"PageUp": keyPageUp, "PageDown": keyPageDown,
	"a": keyA, "b": keyB, "c": keyC, "d": keyD, "e": keyE, "f": keyF, "g": keyG,
	"h": keyH, "i": keyI, "j": keyJ, "k": keyK, "l": keyL, "m": keyM, "n": keyN,
	"o": keyO, "p": keyP, "q": keyQ, "r": keyR, "s": keyS, "t": keyT, "u": keyU,
	"v": keyV, "w": keyW, "x": keyX, "y": keyY, "z": keyZ, "1": key1, "2": key2,
	"3": key3, "4": key4, "5": key5, "6": key6, "7": key7, "8": key8,
	"9": key9, "0": key0,
}

type charKey struct {
	code  uint16
	shift bool
}

var textKeyMap = map[rune]charKey{
	'a': {keyA, false}, 'b': {keyB, false}, 'c': {keyC, false}, 'd': {keyD, false},
	'e': {keyE, false}, 'f': {keyF, false}, 'g': {keyG, false}, 'h': {keyH, false},
	'i': {keyI, false}, 'j': {keyJ, false}, 'k': {keyK, false}, 'l': {keyL, false},
	'm': {keyM, false}, 'n': {keyN, false}, 'o': {keyO, false}, 'p': {keyP, false},
	'q': {keyQ, false}, 'r': {keyR, false}, 's': {keyS, false}, 't': {keyT, false},
	'u': {keyU, false}, 'v': {keyV, false}, 'w': {keyW, false}, 'x': {keyX, false},
	'y': {keyY, false}, 'z': {keyZ, false},
	'A': {keyA, true}, 'B': {keyB, true}, 'C': {keyC, true}, 'D': {keyD, true},
	'E': {keyE, true}, 'F': {keyF, true}, 'G': {keyG, true}, 'H': {keyH, true},
	'I': {keyI, true}, 'J': {keyJ, true}, 'K': {keyK, true}, 'L': {keyL, true},
	'M': {keyM, true}, 'N': {keyN, true}, 'O': {keyO, true}, 'P': {keyP, true},
	'Q': {keyQ, true}, 'R': {keyR, true}, 'S': {keyS, true}, 'T': {keyT, true},
	'U': {keyU, true}, 'V': {keyV, true}, 'W': {keyW, true}, 'X': {keyX, true},
	'Y': {keyY, true}, 'Z': {keyZ, true},
	'1': {key1, false}, '2': {key2, false}, '3': {key3, false}, '4': {key4, false},
	'5': {key5, false}, '6': {key6, false}, '7': {key7, false}, '8': {key8, false},
	'9': {key9, false}, '0': {key0, false},
	'!': {key1, true}, '@': {key2, true}, '#': {key3, true}, '$': {key4, true},
	'%': {key5, true}, '^': {key6, true}, '&': {key7, true}, '*': {key8, true},
	'(': {key9, true}, ')': {key0, true},
	' ': {keySpace, false}, '-': {keyMinus, false}, '_': {keyMinus, true},
	'=': {keyEqual, false}, '+': {keyEqual, true}, '[': {keyLeftBrace, false},
	'{': {keyLeftBrace, true}, ']': {keyRightBrace, false}, '}': {keyRightBrace, true},
	';': {keySemicolon, false}, ':': {keySemicolon, true}, '\'': {keyApostrophe, false},
	'"': {keyApostrophe, true}, '`': {keyGrave, false}, '~': {keyGrave, true},
	'\\': {keyBackslash, false}, '|': {keyBackslash, true}, ',': {keyComma, false},
	'<': {keyComma, true}, '.': {keyDot, false}, '>': {keyDot, true},
	'/': {keySlash, false}, '?': {keySlash, true}, '\t': {keyTab, false},
	'\n': {keyEnter, false}, '\r': {keyEnter, false},
}

type inputDevice struct {
	fd       int
	width    int
	height   int
	mu       sync.Mutex
	downKeys map[uint16]bool
}

type inputMessage struct {
	Type   string   `json:"type"`
	X      *float64 `json:"x,omitempty"`
	Y      *float64 `json:"y,omitempty"`
	Button string   `json:"button,omitempty"`
	Steps  int      `json:"steps,omitempty"`
	Key    string   `json:"key,omitempty"`
	Code   string   `json:"code,omitempty"`
	Down   bool     `json:"down,omitempty"`
	Text   string   `json:"text,omitempty"`
}

func openInputDevice(width, height int) (*inputDevice, error) {
	if width < 2 {
		width = 1920
	}
	if height < 2 {
		height = 1080
	}
	fd, err := syscall.Open("/dev/uinput", syscall.O_WRONLY|syscall.O_NONBLOCK, 0)
	if err != nil {
		return nil, err
	}
	dev := &inputDevice{fd: fd, width: width, height: height, downKeys: map[uint16]bool{}}
	if err := dev.create(); err != nil {
		_ = syscall.Close(fd)
		return nil, err
	}
	return dev, nil
}

func (d *inputDevice) create() error {
	for _, ev := range []uint16{evKey, evAbs, evRel} {
		if err := ioctl(d.fd, uiSetEvbit, uintptr(ev)); err != nil {
			return err
		}
	}
	for _, key := range d.supportedKeys() {
		if err := ioctl(d.fd, uiSetKeybit, uintptr(key)); err != nil {
			return err
		}
	}
	if err := ioctl(d.fd, uiSetRelbit, relWheel); err != nil {
		return err
	}
	if err := ioctl(d.fd, uiSetAbsbit, absX); err != nil {
		return err
	}
	if err := ioctl(d.fd, uiSetAbsbit, absY); err != nil {
		return err
	}

	buf := make([]byte, 1116)
	copy(buf[:80], []byte("DRM WebRTC Input"))
	binary.LittleEndian.PutUint16(buf[80:82], busUSB)
	binary.LittleEndian.PutUint16(buf[82:84], 0x1209)
	binary.LittleEndian.PutUint16(buf[84:86], 0x3588)
	binary.LittleEndian.PutUint16(buf[86:88], 1)
	putInt32(buf, 92+absX*4, int32(d.width-1))
	putInt32(buf, 92+absY*4, int32(d.height-1))
	if _, err := syscall.Write(d.fd, buf); err != nil {
		return err
	}
	if err := ioctl(d.fd, uiDevCreate, 0); err != nil {
		return err
	}
	time.Sleep(200 * time.Millisecond)
	return nil
}

func (d *inputDevice) supportedKeys() []uint16 {
	seen := map[uint16]bool{btnLeft: true, btnRight: true, btnMiddle: true}
	keys := []uint16{btnLeft, btnRight, btnMiddle}
	for _, code := range keyCodeMap {
		if !seen[code] {
			seen[code] = true
			keys = append(keys, code)
		}
	}
	for _, code := range keyNameMap {
		if !seen[code] {
			seen[code] = true
			keys = append(keys, code)
		}
	}
	return keys
}

func (d *inputDevice) Close() error {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.fd < 0 {
		return nil
	}
	_ = d.allUpLocked()
	_ = ioctl(d.fd, uiDevDestroy, 0)
	err := syscall.Close(d.fd)
	d.fd = -1
	return err
}

func (d *inputDevice) HandleJSON(data []byte) error {
	var msg inputMessage
	if err := json.Unmarshal(data, &msg); err != nil {
		return err
	}
	return d.handle(msg)
}

func (d *inputDevice) handle(msg inputMessage) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.fd < 0 {
		return fmt.Errorf("input device closed")
	}
	if msg.X != nil && msg.Y != nil {
		if err := d.moveNormLocked(*msg.X, *msg.Y); err != nil {
			return err
		}
	}
	switch msg.Type {
	case "move":
		return nil
	case "down":
		return d.buttonLocked(msg.Button, 1)
	case "up":
		return d.buttonLocked(msg.Button, 0)
	case "click":
		if err := d.buttonLocked(msg.Button, 1); err != nil {
			return err
		}
		time.Sleep(35 * time.Millisecond)
		return d.buttonLocked(msg.Button, 0)
	case "wheel":
		return d.emitSyncLocked(evRel, relWheel, int32(msg.Steps))
	case "key":
		code := lookupKey(msg.Key, msg.Code)
		if code == 0 {
			return nil
		}
		if msg.Down {
			d.downKeys[code] = true
		} else {
			delete(d.downKeys, code)
		}
		return d.emitSyncLocked(evKey, code, boolInt(msg.Down))
	case "text":
		return d.typeTextLocked(msg.Text)
	case "osk":
		if err := showOnScreenKeyboard(); err == nil {
			return nil
		}
		return d.pressChordLocked([]uint16{keyLeftMeta, keyLeftCtrl}, keyO)
	case "all_up":
		return d.allUpLocked()
	default:
		return fmt.Errorf("unsupported input event %q", msg.Type)
	}
}

func (d *inputDevice) moveNormLocked(x, y float64) error {
	x = clamp01(x)
	y = clamp01(y)
	if err := d.emitLocked(evAbs, absX, int32(x*float64(d.width-1))); err != nil {
		return err
	}
	if err := d.emitLocked(evAbs, absY, int32(y*float64(d.height-1))); err != nil {
		return err
	}
	return d.syncLocked()
}

func (d *inputDevice) buttonLocked(button string, value int32) error {
	return d.emitSyncLocked(evKey, buttonCode(button), value)
}

func (d *inputDevice) typeTextLocked(text string) error {
	const maxRunes = 512
	text = limitRunes(text, maxRunes)
	if text == "" {
		return nil
	}
	if containsNonASCII(text) {
		return d.pasteClipboardLocked(text)
	}
	for _, r := range text {
		if combo, ok := textKeyMap[r]; ok {
			if err := d.tapKeyLocked(combo); err != nil {
				return err
			}
			continue
		}
	}
	return nil
}

func limitRunes(text string, maxRunes int) string {
	if maxRunes <= 0 {
		return ""
	}
	count := 0
	for i := range text {
		if count >= maxRunes {
			return text[:i]
		}
		count++
	}
	return text
}

func containsNonASCII(text string) bool {
	for _, r := range text {
		if r > 0x7f {
			return true
		}
	}
	return false
}

func (d *inputDevice) pasteClipboardLocked(text string) error {
	if err := setXClipboard(text); err != nil {
		return err
	}
	time.Sleep(30 * time.Millisecond)
	return d.pressChordLocked([]uint16{keyLeftCtrl, keyLeftShift}, keyV)
}

func showOnScreenKeyboard() error {
	enabled := runDesktopUserCommand(2*time.Second, "gsettings", "set", "org.gnome.desktop.a11y.applications", "screen-keyboard-enabled", "true") == nil
	scripts := []string{
		`Main.keyboard._syncEnabled(); Main.keyboard.open(Main.layoutManager.focusIndex);`,
		`Main.keyboard._keyboard.show(Main.layoutManager.focusIndex);`,
	}
	var errors []string
	for _, script := range scripts {
		evalArg := "'" + strings.ReplaceAll(script, "'", "\\'") + "'"
		if err := runDesktopUserCommand(2*time.Second, "gdbus", "call", "--session", "--dest", "org.gnome.Shell", "--object-path", "/org/gnome/Shell", "--method", "org.gnome.Shell.Eval", evalArg); err == nil {
			return nil
		} else {
			errors = append(errors, err.Error())
		}
	}
	if enabled {
		return nil
	}
	return fmt.Errorf("show on-screen keyboard failed: %s", strings.Join(errors, "; "))
}

func runDesktopUserCommand(timeout time.Duration, args ...string) error {
	if len(args) == 0 {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	env := []string{
		"DISPLAY=:0",
		"XDG_RUNTIME_DIR=/run/user/1000",
		"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus",
		"XAUTHORITY=/run/user/1000/gdm/Xauthority",
	}
	var cmd *exec.Cmd
	if _, err := os.Stat("/usr/sbin/runuser"); err == nil {
		runuserArgs := []string{"-u", "lmxz", "--", "env"}
		runuserArgs = append(runuserArgs, env...)
		runuserArgs = append(runuserArgs, args...)
		cmd = exec.CommandContext(ctx, "runuser", runuserArgs...)
	} else {
		cmd = exec.CommandContext(ctx, args[0], args[1:]...)
		cmd.Env = append(os.Environ(), env...)
	}
	var stderr bytes.Buffer
	cmd.Stdout = &stderr
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		if ctx.Err() == context.DeadlineExceeded {
			return fmt.Errorf("%s timed out", args[0])
		}
		if detail := strings.TrimSpace(stderr.String()); detail != "" {
			return fmt.Errorf("%s: %w: %s", args[0], err, detail)
		}
		return fmt.Errorf("%s: %w", args[0], err)
	}
	return nil
}

func setXClipboard(text string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()

	cmd := exec.CommandContext(ctx, "xclip", "-selection", "clipboard", "-in")
	cmd.Env = append(os.Environ(), "DISPLAY=:0", "XAUTHORITY=/run/user/1000/gdm/Xauthority")
	cmd.Stdin = strings.NewReader(text)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		if ctx.Err() == context.DeadlineExceeded {
			return fmt.Errorf("set x clipboard timed out")
		}
		if detail := strings.TrimSpace(stderr.String()); detail != "" {
			return fmt.Errorf("set x clipboard: %w: %s", err, detail)
		}
		return fmt.Errorf("set x clipboard: %w", err)
	}
	return nil
}

func (d *inputDevice) tapKeyLocked(combo charKey) error {
	if combo.shift {
		if err := d.emitLocked(evKey, keyLeftShift, 1); err != nil {
			return err
		}
	}
	if err := d.emitLocked(evKey, combo.code, 1); err != nil {
		return err
	}
	if err := d.syncLocked(); err != nil {
		return err
	}
	if err := d.emitLocked(evKey, combo.code, 0); err != nil {
		return err
	}
	if combo.shift {
		if err := d.emitLocked(evKey, keyLeftShift, 0); err != nil {
			return err
		}
	}
	if err := d.syncLocked(); err != nil {
		return err
	}
	time.Sleep(2 * time.Millisecond)
	return nil
}

func (d *inputDevice) pressChordLocked(modifiers []uint16, key uint16) error {
	for _, code := range modifiers {
		if err := d.emitLocked(evKey, code, 1); err != nil {
			return err
		}
	}
	if err := d.emitLocked(evKey, key, 1); err != nil {
		d.releaseKeysBestEffort(append(modifiers, key))
		return err
	}
	if err := d.syncLocked(); err != nil {
		d.releaseKeysBestEffort(append(modifiers, key))
		return err
	}
	if err := d.emitLocked(evKey, key, 0); err != nil {
		d.releaseKeysBestEffort(append(modifiers, key))
		return err
	}
	for i := len(modifiers) - 1; i >= 0; i-- {
		if err := d.emitLocked(evKey, modifiers[i], 0); err != nil {
			d.releaseKeysBestEffort(modifiers[:i])
			return err
		}
	}
	if err := d.syncLocked(); err != nil {
		return err
	}
	time.Sleep(5 * time.Millisecond)
	return nil
}

func (d *inputDevice) releaseKeysBestEffort(keys []uint16) {
	for i := len(keys) - 1; i >= 0; i-- {
		_ = d.emitLocked(evKey, keys[i], 0)
	}
	_ = d.syncLocked()
}

func (d *inputDevice) allUpLocked() error {
	for _, code := range []uint16{btnLeft, btnRight, btnMiddle} {
		if err := d.emitLocked(evKey, code, 0); err != nil {
			return err
		}
	}
	for code := range d.downKeys {
		if err := d.emitLocked(evKey, code, 0); err != nil {
			return err
		}
	}
	d.downKeys = map[uint16]bool{}
	return d.syncLocked()
}

func (d *inputDevice) emitSyncLocked(evType, code uint16, value int32) error {
	if err := d.emitLocked(evType, code, value); err != nil {
		return err
	}
	return d.syncLocked()
}

func (d *inputDevice) syncLocked() error {
	return d.emitLocked(evSyn, synReport, 0)
}

func (d *inputDevice) emitLocked(evType, code uint16, value int32) error {
	now := time.Now()
	buf := make([]byte, 24)
	binary.LittleEndian.PutUint64(buf[0:8], uint64(now.Unix()))
	binary.LittleEndian.PutUint64(buf[8:16], uint64(now.Nanosecond()/1000))
	binary.LittleEndian.PutUint16(buf[16:18], evType)
	binary.LittleEndian.PutUint16(buf[18:20], code)
	binary.LittleEndian.PutUint32(buf[20:24], uint32(value))
	_, err := syscall.Write(d.fd, buf)
	return err
}

func ioctl(fd int, request uintptr, value uintptr) error {
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), request, value)
	if errno != 0 {
		return errno
	}
	return nil
}

func putInt32(buf []byte, off int, value int32) {
	binary.LittleEndian.PutUint32(buf[off:off+4], uint32(value))
}

func buttonCode(button string) uint16 {
	switch button {
	case "right":
		return btnRight
	case "middle":
		return btnMiddle
	default:
		return btnLeft
	}
}

func lookupKey(key, code string) uint16 {
	if value, ok := keyCodeMap[code]; ok {
		return value
	}
	if value, ok := keyNameMap[key]; ok {
		return value
	}
	if len(key) == 1 {
		if value, ok := keyNameMap[strings.ToLower(key)]; ok {
			return value
		}
	}
	return 0
}

func clamp01(value float64) float64 {
	if value < 0 {
		return 0
	}
	if value > 1 {
		return 1
	}
	return value
}

func boolInt(value bool) int32 {
	if value {
		return 1
	}
	return 0
}
