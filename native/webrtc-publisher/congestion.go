package main

import (
	"time"

	"github.com/pion/rtcp"
)

type congestionMetrics struct {
	Loss       float64
	NACKs      int
	RTT        time.Duration
	HasRTT     bool
	REMB       int
	Reason     string
	GoodWindow int
}

// congestionController is a deliberately conservative AIMD controller. Loss,
// NACK and excessive RTT reduce bitrate immediately; a clean connection must
// produce two consecutive receiver-report windows before bitrate can rise.
type congestionController struct {
	minBitrate int
	maxBitrate int
	target     int
	interval   time.Duration
	lastEval   time.Time

	hasRR     bool
	maxLoss   float64
	nacks     int
	rtt       time.Duration
	hasRTT    bool
	remb      int
	rembAt    time.Time
	goodCount int
}

func newCongestionController(minBitrate, maxBitrate, initialBitrate int, interval time.Duration, now time.Time) *congestionController {
	return &congestionController{
		minBitrate: minBitrate,
		maxBitrate: maxBitrate,
		target:     clampInt(initialBitrate, minBitrate, maxBitrate),
		interval:   interval,
		lastEval:   now,
	}
}

func (c *congestionController) observeReceiverReport(report rtcp.ReceptionReport, now time.Time) {
	c.hasRR = true
	loss := float64(report.FractionLost) / 256.0
	if loss > c.maxLoss {
		c.maxLoss = loss
	}
	if rtt, ok := receiverReportRTT(report, now); ok {
		c.rtt = rtt
		c.hasRTT = true
	}
}

func (c *congestionController) observeNACK(nack *rtcp.TransportLayerNack) {
	for i := range nack.Nacks {
		nack.Nacks[i].Range(func(uint16) bool {
			c.nacks++
			return true
		})
	}
}

func (c *congestionController) observeREMB(remb *rtcp.ReceiverEstimatedMaximumBitrate, now time.Time) {
	if remb.Bitrate <= 0 {
		return
	}
	bitrate := int(remb.Bitrate)
	if bitrate < 0 {
		return
	}
	c.remb = bitrate
	c.rembAt = now
}

func (c *congestionController) maybeUpdate(now time.Time) (int, bool, congestionMetrics, bool) {
	if now.Sub(c.lastEval) < c.interval {
		return c.target, false, congestionMetrics{}, false
	}
	c.lastEval = now
	metrics := congestionMetrics{
		Loss:       c.maxLoss,
		NACKs:      c.nacks,
		RTT:        c.rtt,
		HasRTT:     c.hasRTT,
		REMB:       c.remb,
		GoodWindow: c.goodCount,
	}
	hasFeedback := c.hasRR || c.nacks > 0 || c.remb > 0
	desired := c.target
	reason := "stable"

	severe := c.maxLoss >= 0.08 || c.nacks >= 20 || (c.hasRTT && c.rtt >= 450*time.Millisecond)
	moderate := c.maxLoss >= 0.02 || c.nacks >= 5 || (c.hasRTT && c.rtt >= 250*time.Millisecond)
	if severe {
		desired = c.target * 70 / 100
		reason = "severe-congestion"
		c.goodCount = 0
	} else if moderate {
		desired = c.target * 85 / 100
		reason = "congestion"
		c.goodCount = 0
	} else {
		if c.hasRR && c.maxLoss < 0.01 && c.nacks == 0 && (!c.hasRTT || c.rtt < 180*time.Millisecond) {
			c.goodCount++
			if c.goodCount >= 2 {
				step := c.target / 10
				if step < 250000 {
					step = 250000
				}
				desired = c.target + step
				reason = "clean-link"
				c.goodCount = 0
			}
		} else if c.hasRR {
			c.goodCount = 0
		}
	}

	// A screen stream is frequently application-limited: a static desktop may
	// emit only a few hundred kbit/s even when the path has ample capacity.
	// Chrome's REMB can mirror that low observed rate. Treat REMB as a cap only
	// when independent congestion evidence exists, otherwise it creates a
	// self-reinforcing low-bitrate state.
	if rembCap, ok := c.freshREMBCap(now); ok && (severe || moderate) && desired > rembCap {
		desired = rembCap
	}
	desired = clampInt(desired, c.minBitrate, c.maxBitrate)
	changed := desired != c.target
	if changed {
		c.target = desired
	}
	metrics.Reason = reason
	metrics.GoodWindow = c.goodCount

	c.hasRR = false
	c.maxLoss = 0
	c.nacks = 0
	c.rtt = 0
	c.hasRTT = false
	return c.target, changed, metrics, hasFeedback
}

func (c *congestionController) freshREMBCap(now time.Time) (int, bool) {
	if c.remb <= 0 || c.rembAt.IsZero() {
		return 0, false
	}
	freshFor := 3 * time.Second
	if candidate := 3 * c.interval; candidate > freshFor {
		freshFor = candidate
	}
	if now.Sub(c.rembAt) > freshFor {
		return 0, false
	}
	return clampInt(c.remb*9/10, c.minBitrate, c.maxBitrate), true
}

func receiverReportRTT(report rtcp.ReceptionReport, now time.Time) (time.Duration, bool) {
	if report.LastSenderReport == 0 {
		return 0, false
	}
	units := compactNTP(now) - report.LastSenderReport - report.Delay
	if units > 60*65536 {
		return 0, false
	}
	return time.Duration(uint64(units) * uint64(time.Second) / 65536), true
}

func compactNTP(now time.Time) uint32 {
	const ntpEpochOffset = int64(2208988800)
	seconds := uint64(now.Unix() + ntpEpochOffset)
	fraction := uint64(now.Nanosecond()) * (uint64(1) << 32) / uint64(time.Second)
	return uint32((seconds&0xffff)<<16 | fraction>>16)
}

func clampInt(value, minimum, maximum int) int {
	if value < minimum {
		return minimum
	}
	if value > maximum {
		return maximum
	}
	return value
}
