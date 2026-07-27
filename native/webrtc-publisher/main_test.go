package main

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"os/exec"
	"strconv"
	"syscall"
	"testing"
	"time"
)

func TestReadAnnexBNALsMixedStartCodes(t *testing.T) {
	stream := []byte{
		0x00, 0x00, 0x00, 0x01, 0x67, 0x11, 0x22,
		0x00, 0x00, 0x01, 0x68, 0x33,
		0x00, 0x00, 0x00, 0x01, 0x65, 0x44, 0x55, 0x00,
	}
	ch := make(chan []byte, 8)
	errCh := make(chan error, 1)
	go func() {
		defer close(ch)
		errCh <- readAnnexBNALs(context.Background(), bytes.NewReader(stream), ch)
	}()
	err := <-errCh
	if err != nil {
		t.Fatalf("readAnnexBNALs failed: %v", err)
	}
	var got [][]byte
	for nal := range ch {
		got = append(got, nal)
	}
	want := [][]byte{
		{0x67, 0x11, 0x22},
		{0x68, 0x33},
		{0x65, 0x44, 0x55},
	}
	if len(got) != len(want) {
		t.Fatalf("got %d NALs, want %d: %#v", len(got), len(want), got)
	}
	for i := range want {
		if !bytes.Equal(got[i], want[i]) {
			t.Fatalf("NAL %d = %#v, want %#v", i, got[i], want[i])
		}
	}
}

func TestCommandGroupCancellationStopsChildren(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cmd := exec.CommandContext(ctx, "sh", "-c", "sleep 60 & echo $!; wait")
	configureCommandGroupCancellation(cmd)
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}

	scanner := bufio.NewScanner(stdout)
	if !scanner.Scan() {
		cancel()
		_ = cmd.Wait()
		t.Fatal("child pid was not reported")
	}
	childPID, err := strconv.Atoi(scanner.Text())
	if err != nil {
		cancel()
		_ = cmd.Wait()
		t.Fatalf("invalid child pid: %v", err)
	}

	started := time.Now()
	cancel()
	_ = cmd.Wait()
	if elapsed := time.Since(started); elapsed > 4*time.Second {
		t.Fatalf("process group cancellation took %s", elapsed)
	}

	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		err = syscall.Kill(childPID, 0)
		if errors.Is(err, syscall.ESRCH) {
			return
		}
		time.Sleep(20 * time.Millisecond)
	}
	_ = syscall.Kill(childPID, syscall.SIGKILL)
	t.Fatalf("child process %d survived cancellation", childPID)
}

func TestViewerSlotKeepsFirstConnection(t *testing.T) {
	s := &lanServer{sessions: map[string]*session{}}
	if !s.claimViewerSlot("first") {
		t.Fatal("first viewer should acquire the slot")
	}
	if s.claimViewerSlot("second") {
		t.Fatal("second viewer must not displace the active viewer")
	}

	// Closing a rejected/non-owner viewer must not release the current owner.
	s.closeSession("second")
	if s.claimViewerSlot("second") {
		t.Fatal("closing a non-owner must not release the active viewer")
	}

	s.closeSession("first")
	if !s.claimViewerSlot("second") {
		t.Fatal("the next viewer should acquire the slot after the owner closes")
	}
}
