package main

import (
	"testing"
	"time"

	"github.com/pion/rtcp"
)

func TestCongestionControllerReducesOnLoss(t *testing.T) {
	now := time.Unix(1700000000, 0)
	controller := newCongestionController(2000000, 8000000, 6000000, time.Second, now)
	controller.observeReceiverReport(rtcp.ReceptionReport{FractionLost: 26}, now.Add(time.Second))
	target, changed, metrics, evaluated := controller.maybeUpdate(now.Add(time.Second))
	if !evaluated || !changed || target != 4200000 {
		t.Fatalf("unexpected loss response: target=%d changed=%v evaluated=%v", target, changed, evaluated)
	}
	if metrics.Reason != "severe-congestion" {
		t.Fatalf("unexpected reason: %s", metrics.Reason)
	}
}

func TestCongestionControllerRequiresTwoCleanWindowsToIncrease(t *testing.T) {
	now := time.Unix(1700000000, 0)
	controller := newCongestionController(2000000, 8000000, 6000000, time.Second, now)
	controller.observeReceiverReport(rtcp.ReceptionReport{}, now.Add(time.Second))
	if target, changed, _, _ := controller.maybeUpdate(now.Add(time.Second)); changed || target != 6000000 {
		t.Fatalf("first clean window changed bitrate: target=%d changed=%v", target, changed)
	}
	controller.observeReceiverReport(rtcp.ReceptionReport{}, now.Add(2*time.Second))
	if target, changed, metrics, _ := controller.maybeUpdate(now.Add(2 * time.Second)); !changed || target != 6600000 || metrics.Reason != "clean-link" {
		t.Fatalf("second clean window did not increase bitrate: target=%d changed=%v reason=%s", target, changed, metrics.Reason)
	}
}

func TestCongestionControllerHonorsREMBCap(t *testing.T) {
	now := time.Unix(1700000000, 0)
	controller := newCongestionController(2000000, 8000000, 6000000, time.Second, now)
	controller.observeREMB(&rtcp.ReceiverEstimatedMaximumBitrate{Bitrate: 4000000}, now.Add(time.Second))
	controller.observeReceiverReport(rtcp.ReceptionReport{FractionLost: 6}, now.Add(time.Second))
	target, changed, metrics, _ := controller.maybeUpdate(now.Add(time.Second))
	if !changed || target != 3600000 || metrics.Reason != "congestion" {
		t.Fatalf("REMB cap not applied: target=%d changed=%v reason=%s", target, changed, metrics.Reason)
	}
}

func TestCongestionControllerIgnoresApplicationLimitedREMBOnCleanLink(t *testing.T) {
	now := time.Unix(1700000000, 0)
	controller := newCongestionController(2000000, 8000000, 6000000, time.Second, now)
	controller.observeREMB(&rtcp.ReceiverEstimatedMaximumBitrate{Bitrate: 500000}, now.Add(time.Second))
	controller.observeReceiverReport(rtcp.ReceptionReport{}, now.Add(time.Second))
	target, changed, _, _ := controller.maybeUpdate(now.Add(time.Second))
	if changed || target != 6000000 {
		t.Fatalf("clean application-limited stream was reduced: target=%d changed=%v", target, changed)
	}
}

func TestReceiverReportRTT(t *testing.T) {
	now := time.Unix(1700000000, 500000000)
	report := rtcp.ReceptionReport{
		LastSenderReport: compactNTP(now.Add(-200 * time.Millisecond)),
		Delay:            uint32(100 * 65536 / 1000),
	}
	rtt, ok := receiverReportRTT(report, now)
	if !ok || rtt < 99*time.Millisecond || rtt > 101*time.Millisecond {
		t.Fatalf("unexpected RTT: %s ok=%v", rtt, ok)
	}
}

func TestCongestionControllerClampsAtMinimum(t *testing.T) {
	now := time.Unix(1700000000, 0)
	controller := newCongestionController(2000000, 8000000, 2100000, time.Second, now)
	controller.observeNACK(&rtcp.TransportLayerNack{Nacks: []rtcp.NackPair{{PacketID: 1, LostPackets: 0xffff}}})
	target, changed, _, _ := controller.maybeUpdate(now.Add(time.Second))
	if !changed || target != 2000000 {
		t.Fatalf("minimum not enforced: target=%d changed=%v", target, changed)
	}
}
