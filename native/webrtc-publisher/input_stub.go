//go:build !linux

package main

import "errors"

type inputDevice struct{}

func openInputDevice(width, height int) (*inputDevice, error) {
	return nil, errors.New("uinput input is only available on Linux")
}

func (d *inputDevice) Close() error {
	return nil
}

func (d *inputDevice) HandleJSON(data []byte) error {
	return errors.New("uinput input is unavailable on this platform")
}
