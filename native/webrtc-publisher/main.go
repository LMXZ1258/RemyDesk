package main

import (
	"context"
	crand "crypto/rand"
	"encoding/binary"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/gorilla/websocket"
	"github.com/pion/rtcp"
	"github.com/pion/rtp"
	"github.com/pion/webrtc/v3"
)

const version = "0.1.0"

type config struct {
	SignalMode     string
	LANListen      string
	LANPassword    string
	Name           string
	SourceMode     string
	H264Command    string
	H264WorkDir    string
	FPS            int
	ICEServers     string
	ICEUsername    string
	ICECredential  string
	FFmpeg         string
	FFmpegLogLevel string
	PacketSize     int
	H264Fmtp       string
	H264NALQueue   int
	AudioEnabled   bool
	AudioBackend   string
	AudioSource    string
	AudioBitrate   string
	AudioQueueSize int
	PulseServer    string
	InputEnabled   bool
	InputWidth     int
	InputHeight    int
	ReconnectDelay time.Duration
	Verbose        bool
}

type signalMessage struct {
	Type      string                     `json:"type"`
	SDP       *webrtc.SessionDescription `json:"sdp,omitempty"`
	Candidate *webrtc.ICECandidateInit   `json:"candidate,omitempty"`
	Error     string                     `json:"error,omitempty"`
}

type lanConfigResponse struct {
	Name            string `json:"name"`
	PasswordEnabled bool   `json:"passwordEnabled"`
	InputWidth      int    `json:"inputWidth"`
	InputHeight     int    `json:"inputHeight"`
	AudioEnabled    bool   `json:"audioEnabled"`
}

type lanOfferRequest struct {
	Password string                    `json:"password,omitempty"`
	SDP      webrtc.SessionDescription `json:"sdp"`
}

type lanOfferResponse struct {
	SDP *webrtc.SessionDescription `json:"sdp"`
}

type lanServer struct {
	ctx          context.Context
	cfg          config
	input        *inputDevice
	sessMu       sync.Mutex
	sessions     map[string]*session
	activeViewer string
}

type session struct {
	viewerID   string
	cfg        config
	track      *webrtc.TrackLocalStaticRTP
	audioTrack *webrtc.TrackLocalStaticRTP
	pc         *webrtc.PeerConnection
	audioUDP   *net.UDPConn
	cmd        *exec.Cmd
	audioCmd   *exec.Cmd
	cancel     context.CancelFunc
	mediaMu    sync.Mutex
	mediaOn    bool
	lastIDRReq time.Time
	audioMu    sync.Mutex
	audioOn    bool
	closeMu    sync.Once
}

func main() {
	cfg := readConfig()
	if cfg.Verbose {
		log.Printf("version=%s mode=%s lan=%s name=%s source=%s", version, cfg.SignalMode, cfg.LANListen, cfg.Name, cfg.SourceMode)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	for {
		err := runOnce(ctx, cfg)
		if ctx.Err() != nil {
			return
		}
		log.Printf("publisher stopped: %v; reconnecting in %s", err, cfg.ReconnectDelay)
		select {
		case <-ctx.Done():
			return
		case <-time.After(cfg.ReconnectDelay):
		}
	}
}

func readConfig() config {
	var showVersion bool
	cfg := config{
		SignalMode:     getenv("SIGNAL_MODE", "lan"),
		LANListen:      getenv("LAN_LISTEN", ":8088"),
		LANPassword:    os.Getenv("LAN_PASSWORD"),
		Name:           getenv("SIGNAL_NAME", "rk3588-kms"),
		SourceMode:     getenv("SOURCE_MODE", "command"),
		H264Command:    getenv("H264_COMMAND", "exec ./portable_h264_stream.sh"),
		H264WorkDir:    getenv("H264_WORKDIR", "/opt/remydesk/libexec"),
		FPS:            getenvInt("FPS", 30),
		ICEServers:     getenv("ICE_SERVERS", ""),
		ICEUsername:    "",
		ICECredential:  "",
		FFmpeg:         getenv("FFMPEG", "ffmpeg"),
		FFmpegLogLevel: getenv("FFMPEG_LOGLEVEL", "warning"),
		PacketSize:     getenvInt("RTP_PACKET_SIZE", 1200),
		H264Fmtp:       getenv("H264_FMTP", "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42c028"),
		H264NALQueue:   getenvInt("H264_NAL_QUEUE", 4),
		AudioEnabled:   getenvBool("AUDIO_ENABLED", false),
		AudioBackend:   getenv("AUDIO_BACKEND", "pulse"),
		AudioSource:    getenv("AUDIO_SOURCE", "auto"),
		AudioBitrate:   getenv("AUDIO_BITRATE", "128k"),
		AudioQueueSize: getenvInt("AUDIO_THREAD_QUEUE_SIZE", 8),
		PulseServer:    getenv("PULSE_SERVER", ""),
		InputEnabled:   getenvBool("INPUT_ENABLED", true),
		InputWidth:     getenvInt("INPUT_WIDTH", 1920),
		InputHeight:    getenvInt("INPUT_HEIGHT", 1080),
		ReconnectDelay: time.Duration(getenvInt("RECONNECT_SECONDS", 3)) * time.Second,
	}
	flag.StringVar(&cfg.SignalMode, "signal-mode", cfg.SignalMode, "signaling mode (RemyDesk v0.1 supports lan only)")
	flag.StringVar(&cfg.LANListen, "lan-listen", cfg.LANListen, "LAN HTTP listen address for signal-mode=lan")
	flag.StringVar(&cfg.LANPassword, "lan-password", cfg.LANPassword, "optional LAN page/API password")
	flag.StringVar(&cfg.Name, "name", cfg.Name, "publisher name")
	flag.StringVar(&cfg.SourceMode, "source-mode", cfg.SourceMode, "source mode: command")
	flag.StringVar(&cfg.H264Command, "h264-command", cfg.H264Command, "command that writes Annex-B H264 to stdout")
	flag.StringVar(&cfg.H264WorkDir, "h264-workdir", cfg.H264WorkDir, "working directory for h264-command")
	flag.IntVar(&cfg.FPS, "fps", cfg.FPS, "input video frame rate")
	flag.StringVar(&cfg.ICEServers, "ice-servers", cfg.ICEServers, "comma-separated STUN/TURN URLs")
	flag.StringVar(&cfg.ICEUsername, "ice-username", cfg.ICEUsername, "TURN username for turn: ICE servers")
	flag.StringVar(&cfg.ICECredential, "ice-credential", cfg.ICECredential, "TURN credential for turn: ICE servers")
	flag.StringVar(&cfg.FFmpeg, "ffmpeg", cfg.FFmpeg, "ffmpeg binary")
	flag.StringVar(&cfg.FFmpegLogLevel, "ffmpeg-loglevel", cfg.FFmpegLogLevel, "ffmpeg loglevel")
	flag.IntVar(&cfg.PacketSize, "rtp-packet-size", cfg.PacketSize, "RTP packet size")
	flag.StringVar(&cfg.H264Fmtp, "h264-fmtp", cfg.H264Fmtp, "H264 SDP fmtp line")
	flag.IntVar(&cfg.H264NALQueue, "h264-nal-queue", cfg.H264NALQueue, "maximum queued Annex-B NAL units")
	flag.BoolVar(&cfg.AudioEnabled, "audio-enabled", cfg.AudioEnabled, "enable optional Opus audio publishing")
	flag.StringVar(&cfg.AudioBackend, "audio-backend", cfg.AudioBackend, "ffmpeg audio input backend: pulse or alsa")
	flag.StringVar(&cfg.AudioSource, "audio-source", cfg.AudioSource, "ffmpeg audio input source")
	flag.StringVar(&cfg.AudioBitrate, "audio-bitrate", cfg.AudioBitrate, "Opus audio bitrate")
	flag.IntVar(&cfg.AudioQueueSize, "audio-thread-queue-size", cfg.AudioQueueSize, "ffmpeg audio capture queue size")
	flag.StringVar(&cfg.PulseServer, "pulse-server", cfg.PulseServer, "PulseAudio server socket")
	flag.BoolVar(&cfg.InputEnabled, "input-enabled", cfg.InputEnabled, "enable WebRTC datachannel input injection through /dev/uinput")
	flag.IntVar(&cfg.InputWidth, "input-width", cfg.InputWidth, "absolute input device width")
	flag.IntVar(&cfg.InputHeight, "input-height", cfg.InputHeight, "absolute input device height")
	flag.BoolVar(&cfg.Verbose, "verbose", os.Getenv("VERBOSE") == "1", "verbose logs")
	flag.BoolVar(&showVersion, "version", false, "print version and exit")
	flag.Parse()
	if showVersion {
		fmt.Println(version)
		os.Exit(0)
	}
	if mode := strings.ToLower(strings.TrimSpace(cfg.SignalMode)); mode != "lan" && mode != "local" && mode != "standalone" {
		fmt.Fprintln(os.Stderr, "RemyDesk v0.1 only supports LAN signaling")
		os.Exit(2)
	}
	if mode := strings.ToLower(strings.TrimSpace(cfg.SourceMode)); mode != "command" && mode != "annexb" && mode != "stdout" {
		fmt.Fprintln(os.Stderr, "RemyDesk v0.1 only supports direct Annex-B command input")
		os.Exit(2)
	}
	if cfg.H264NALQueue < 2 {
		cfg.H264NALQueue = 2
	} else if cfg.H264NALQueue > 64 {
		cfg.H264NALQueue = 64
	}
	if cfg.AudioQueueSize < 8 {
		cfg.AudioQueueSize = 8
	} else if cfg.AudioQueueSize > 256 {
		cfg.AudioQueueSize = 256
	}
	return cfg
}

func getenv(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}

func getenvInt(key string, fallback int) int {
	value := strings.TrimSpace(os.Getenv(key))
	if value == "" {
		return fallback
	}
	n, err := strconv.Atoi(value)
	if err != nil {
		return fallback
	}
	return n
}

func getenvBool(key string, fallback bool) bool {
	value := strings.ToLower(strings.TrimSpace(os.Getenv(key)))
	if value == "" {
		return fallback
	}
	switch value {
	case "1", "true", "yes", "on":
		return true
	case "0", "false", "no", "off":
		return false
	default:
		return fallback
	}
}

func runOnce(ctx context.Context, cfg config) error {
	switch strings.ToLower(strings.TrimSpace(cfg.SignalMode)) {
	case "lan", "local", "standalone":
		return runLANServer(ctx, cfg)
	default:
		return fmt.Errorf("RemyDesk v0.1 only supports LAN signaling, got %q", cfg.SignalMode)
	}
}

func createPeerSession(
	parent context.Context,
	cfg config,
	input *inputDevice,
	viewerID string,
	offer webrtc.SessionDescription,
	waitICEComplete bool,
	closeSession func(string),
	storeSession func(string, *session),
	sendCandidate func(webrtc.ICECandidateInit) error,
) (*webrtc.SessionDescription, error) {
	peerConfig := webrtc.Configuration{ICEServers: parseICEServers(cfg.ICEServers, cfg.ICEUsername, cfg.ICECredential)}
	pc, err := webrtc.NewPeerConnection(peerConfig)
	if err != nil {
		return nil, err
	}

	ctx, cancel := context.WithCancel(parent)
	s := &session{viewerID: viewerID, cfg: cfg, pc: pc, cancel: cancel}
	storeSession(viewerID, s)

	pc.OnICECandidate(func(c *webrtc.ICECandidate) {
		if c == nil || sendCandidate == nil {
			return
		}
		if err := sendCandidate(c.ToJSON()); err != nil {
			log.Printf("viewer=%s send candidate failed: %v", viewerID, err)
		}
	})
	pc.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		log.Printf("viewer=%s pc=%s", viewerID, state)
		if state == webrtc.PeerConnectionStateConnected {
			if err := s.startMediaForwarderOnce(ctx); err != nil {
				log.Printf("viewer=%s start media failed: %v", viewerID, err)
				closeSession(viewerID)
			}
		}
		if state == webrtc.PeerConnectionStateFailed ||
			state == webrtc.PeerConnectionStateClosed ||
			state == webrtc.PeerConnectionStateDisconnected {
			closeSession(viewerID)
		}
	})
	pc.OnICEConnectionStateChange(func(state webrtc.ICEConnectionState) {
		log.Printf("viewer=%s ice=%s", viewerID, state)
	})
	pc.OnDataChannel(func(dc *webrtc.DataChannel) {
		log.Printf("viewer=%s datachannel=%s", viewerID, dc.Label())
		dc.OnMessage(func(msg webrtc.DataChannelMessage) {
			if cfg.Verbose {
				log.Printf("viewer=%s input bytes=%d text=%t", viewerID, len(msg.Data), msg.IsString)
			}
			if !msg.IsString {
				return
			}
			if handled, err := s.handleControlMessage(ctx, msg.Data); handled {
				if err != nil {
					log.Printf("viewer=%s control error: %v", viewerID, err)
				}
				return
			}
			if input == nil {
				return
			}
			if err := input.HandleJSON(msg.Data); err != nil {
				log.Printf("viewer=%s input error: %v", viewerID, err)
			}
		})
	})

	if err := pc.SetRemoteDescription(offer); err != nil {
		s.Close()
		return nil, err
	}

	track, err := webrtc.NewTrackLocalStaticRTP(webrtc.RTPCodecCapability{
		MimeType:     webrtc.MimeTypeH264,
		ClockRate:    90000,
		SDPFmtpLine:  cfg.H264Fmtp,
		RTCPFeedback: []webrtc.RTCPFeedback{{Type: "nack"}, {Type: "nack", Parameter: "pli"}, {Type: "goog-remb"}},
	}, "video", "rk3588-kms")
	if err != nil {
		s.Close()
		return nil, err
	}
	s.track = track

	sender, err := pc.AddTrack(track)
	if err != nil {
		s.Close()
		return nil, err
	}
	go s.drainVideoRTCP(sender)

	if cfg.AudioEnabled {
		audioTrack, err := webrtc.NewTrackLocalStaticRTP(webrtc.RTPCodecCapability{
			MimeType:  webrtc.MimeTypeOpus,
			ClockRate: 48000,
			Channels:  2,
		}, "audio", "rk3588-audio")
		if err != nil {
			s.Close()
			return nil, err
		}
		s.audioTrack = audioTrack
		audioSender, err := pc.AddTrack(audioTrack)
		if err != nil {
			s.Close()
			return nil, err
		}
		go drainRTCP(audioSender)
	}

	answer, err := pc.CreateAnswer(nil)
	if err != nil {
		s.Close()
		return nil, err
	}
	if err := pc.SetLocalDescription(answer); err != nil {
		s.Close()
		return nil, err
	}
	if waitICEComplete {
		select {
		case <-webrtc.GatheringCompletePromise(pc):
		case <-ctx.Done():
			s.Close()
			return nil, ctx.Err()
		}
	}
	return pc.LocalDescription(), nil
}

func ptr[T any](v T) *T {
	return &v
}

func parseICEServers(raw string, username string, credential string) []webrtc.ICEServer {
	var servers []webrtc.ICEServer
	for _, value := range strings.Split(raw, ",") {
		value = strings.TrimSpace(value)
		if value == "" {
			continue
		}
		server := webrtc.ICEServer{URLs: []string{value}}
		if strings.HasPrefix(value, "turn:") || strings.HasPrefix(value, "turns:") {
			server.Username = username
			server.Credential = credential
		}
		servers = append(servers, server)
	}
	return servers
}

func runLANServer(ctx context.Context, cfg config) error {
	s := &lanServer{
		ctx:      ctx,
		cfg:      cfg,
		sessions: map[string]*session{},
	}
	if cfg.InputEnabled {
		input, err := openInputDevice(cfg.InputWidth, cfg.InputHeight)
		if err != nil {
			log.Printf("input disabled: %v", err)
		} else {
			s.input = input
			defer input.Close()
			log.Printf("input enabled via /dev/uinput size=%dx%d", cfg.InputWidth, cfg.InputHeight)
		}
	}
	defer s.closeSessions()

	mux := http.NewServeMux()
	mux.HandleFunc("/", s.handleIndex)
	mux.HandleFunc("/api/config", s.handleConfig)
	mux.HandleFunc("/api/offer", s.handleOffer)
	mux.HandleFunc("/ws", s.handleWS)

	server := &http.Server{
		Addr:              cfg.LANListen,
		Handler:           mux,
		ReadHeaderTimeout: 5 * time.Second,
	}
	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		_ = server.Shutdown(shutdownCtx)
	}()

	log.Printf("LAN WebRTC server listening on http://0.0.0.0%s", normalizeListenForLog(cfg.LANListen))
	err := server.ListenAndServe()
	if errors.Is(err, http.ErrServerClosed) {
		return nil
	}
	return err
}

func normalizeListenForLog(addr string) string {
	if strings.HasPrefix(addr, ":") {
		return addr
	}
	if host, port, err := net.SplitHostPort(addr); err == nil && (host == "" || host == "0.0.0.0" || host == "::") {
		return ":" + port
	}
	return " (" + addr + ")"
}

func (s *lanServer) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	_, _ = io.WriteString(w, lanIndexHTML)
}

func (s *lanServer) handleConfig(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	writeJSON(w, http.StatusOK, lanConfigResponse{
		Name:            s.cfg.Name,
		PasswordEnabled: s.cfg.LANPassword != "",
		InputWidth:      s.cfg.InputWidth,
		InputHeight:     s.cfg.InputHeight,
		AudioEnabled:    s.cfg.AudioEnabled,
	})
}

func (s *lanServer) handleOffer(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	defer r.Body.Close()

	var req lanOfferRequest
	dec := json.NewDecoder(http.MaxBytesReader(w, r.Body, 2<<20))
	if err := dec.Decode(&req); err != nil {
		http.Error(w, "bad json", http.StatusBadRequest)
		return
	}
	if s.cfg.LANPassword != "" && req.Password != s.cfg.LANPassword {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	if req.SDP.Type != webrtc.SDPTypeOffer || strings.TrimSpace(req.SDP.SDP) == "" {
		http.Error(w, "missing offer", http.StatusBadRequest)
		return
	}

	viewerID := "lan-" + randomID(8)
	if !s.claimViewerSlot(viewerID) {
		log.Printf("viewer=%s rejected: desktop already in use", viewerID)
		http.Error(w, "desktop is already in use", http.StatusLocked)
		return
	}
	answer, err := createPeerSession(s.ctx, s.cfg, s.input, viewerID, req.SDP, true,
		s.closeSession,
		s.storeSession,
		nil,
	)
	if err != nil {
		s.closeSession(viewerID)
		log.Printf("viewer=%s lan offer failed: %v", viewerID, err)
		http.Error(w, "offer failed", http.StatusInternalServerError)
		return
	}
	log.Printf("viewer=%s LAN answer ready; media will start when pc=connected source=%s", viewerID, s.cfg.SourceMode)
	writeJSON(w, http.StatusOK, lanOfferResponse{SDP: answer})
}

var lanWSUpgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool {
		return true
	},
}

func (s *lanServer) handleWS(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if s.cfg.LANPassword != "" && r.URL.Query().Get("password") != s.cfg.LANPassword {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	conn, err := lanWSUpgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	defer conn.Close()

	viewerID := "lan-" + randomID(8)

	var writeMu sync.Mutex
	send := func(v any) error {
		writeMu.Lock()
		defer writeMu.Unlock()
		return conn.WriteJSON(v)
	}

	wsDone := make(chan struct{})
	go func() {
		ticker := time.NewTicker(20 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-s.ctx.Done():
				return
			case <-wsDone:
				return
			case <-ticker.C:
				writeMu.Lock()
				err := conn.WriteControl(websocket.PingMessage, []byte("ping"), time.Now().Add(5*time.Second))
				writeMu.Unlock()
				if err != nil {
					return
				}
			}
		}
	}()
	defer close(wsDone)

	log.Printf("viewer=%s LAN websocket connected", viewerID)
	for {
		var msg signalMessage
		if err := conn.ReadJSON(&msg); err != nil {
			log.Printf("viewer=%s LAN websocket closed: %v", viewerID, err)
			return
		}
		switch msg.Type {
		case "offer":
			if msg.SDP == nil {
				_ = send(signalMessage{Type: "error", Error: "offer missing sdp"})
				continue
			}
			s.closeSession(viewerID)
			if !s.claimViewerSlot(viewerID) {
				log.Printf("viewer=%s rejected: desktop already in use", viewerID)
				_ = send(signalMessage{Type: "error", Error: "桌面正在被其他连接使用，请关闭另一处桌面推流后重试"})
				continue
			}
			answer, err := createPeerSession(s.ctx, s.cfg, s.input, viewerID, *msg.SDP, false,
				s.closeSession,
				s.storeSession,
				func(candidate webrtc.ICECandidateInit) error {
					return send(signalMessage{Type: "candidate", Candidate: ptr(candidate)})
				},
			)
			if err != nil {
				s.closeSession(viewerID)
				log.Printf("viewer=%s LAN offer failed: %v", viewerID, err)
				_ = send(signalMessage{Type: "error", Error: "offer failed"})
				continue
			}
			if err := send(signalMessage{Type: "answer", SDP: answer}); err != nil {
				log.Printf("viewer=%s send LAN answer failed: %v", viewerID, err)
				return
			}
			log.Printf("viewer=%s LAN answer sent; media will start when pc=connected source=%s", viewerID, s.cfg.SourceMode)
		case "candidate":
			if msg.Candidate == nil {
				continue
			}
			if err := s.addCandidate(viewerID, *msg.Candidate); err != nil {
				log.Printf("viewer=%s add LAN candidate failed: %v", viewerID, err)
			}
		case "ping", "keepalive":
		default:
			if s.cfg.Verbose {
				log.Printf("viewer=%s ignored LAN signal: %s", viewerID, msg.Type)
			}
		}
	}
}

func (s *lanServer) storeSession(viewerID string, sess *session) {
	s.sessMu.Lock()
	defer s.sessMu.Unlock()
	s.sessions[viewerID] = sess
}

func (s *lanServer) claimViewerSlot(viewerID string) bool {
	s.sessMu.Lock()
	defer s.sessMu.Unlock()
	if s.activeViewer != "" && s.activeViewer != viewerID {
		return false
	}
	s.activeViewer = viewerID
	return true
}

func (s *lanServer) addCandidate(viewerID string, candidate webrtc.ICECandidateInit) error {
	s.sessMu.Lock()
	sess := s.sessions[viewerID]
	s.sessMu.Unlock()
	if sess == nil {
		return nil
	}
	return sess.pc.AddICECandidate(candidate)
}

func (s *lanServer) closeSession(viewerID string) {
	s.sessMu.Lock()
	sess := s.sessions[viewerID]
	delete(s.sessions, viewerID)
	if s.activeViewer == viewerID {
		s.activeViewer = ""
	}
	s.sessMu.Unlock()
	if sess != nil {
		sess.Close()
	}
}

func (s *lanServer) closeSessions() {
	s.sessMu.Lock()
	sessions := s.sessions
	s.sessions = map[string]*session{}
	s.activeViewer = ""
	s.sessMu.Unlock()
	for _, sess := range sessions {
		sess.Close()
	}
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func randomID(n int) string {
	if n <= 0 {
		n = 8
	}
	b := make([]byte, n)
	if _, err := crand.Read(b); err != nil {
		return strconv.FormatInt(time.Now().UnixNano(), 16)
	}
	const hex = "0123456789abcdef"
	out := make([]byte, len(b)*2)
	for i, v := range b {
		out[i*2] = hex[v>>4]
		out[i*2+1] = hex[v&0x0f]
	}
	return string(out)
}

const lanIndexHTML = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>KMS LAN Desktop</title>
<style>
:root { color-scheme: dark; --bg:#05070a; --panel:rgba(12,17,23,.78); --line:rgba(148,163,184,.35); --text:#eef2f7; --muted:#cbd5e1; --ok:#86efac; --bad:#fca5a5; }
* { box-sizing:border-box; }
html, body { margin:0; width:100%; height:100%; overflow:hidden; background:#05070a; color:var(--text); font:14px/1.35 system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif; }
button, input { font:inherit; }
#stage { position:fixed; inset:0; overflow:hidden; background:#000; }
#video { position:absolute; inset:0; width:100%; height:100%; object-fit:contain; background:#000; }
#audio { display:none; }
#inputLayer { position:absolute; inset:0; z-index:5; pointer-events:none; touch-action:none; outline:none; user-select:none; }
#inputLayer.enabled { pointer-events:auto; cursor:crosshair; }
.controlOverlay { position:fixed; right:12px; bottom:12px; z-index:10; display:flex; flex-direction:column; gap:8px; align-items:stretch; width:max-content; max-width:calc(100vw - 24px); padding:8px; border:1px solid var(--line); border-radius:8px; background:var(--panel); backdrop-filter:blur(10px); box-shadow:0 10px 40px rgba(0,0,0,.35); transition:transform .2s ease,opacity .2s ease; }
.controlOverlay.idle { transform:translateX(calc(100% - 34px)); opacity:.82; }
.controlRow { display:flex; gap:8px; align-items:center; }
button { min-height:34px; border:1px solid #344255; border-radius:7px; background:#182230; color:var(--text); padding:8px 11px; cursor:pointer; font-weight:650; }
button:active { transform:translateY(1px); }
.controlHandle { width:26px; min-height:34px; padding:0; position:relative; flex:0 0 auto; }
.controlHandle::before { content:""; position:absolute; left:6px; top:50%; width:8px; height:8px; border-right:2px solid currentColor; border-bottom:2px solid currentColor; transform:translateY(-50%) rotate(-45deg); }
.controlOverlay.idle .controlHandle::before { left:9px; border-right:0; border-left:2px solid currentColor; transform:translateY(-50%) rotate(45deg); }
.clipboardRow { display:flex; gap:8px; align-items:stretch; }
.clipboardBtn { flex:1 1 auto; width:auto; min-width:0; }
.backspaceBtn { flex:0 0 auto; min-width:58px; white-space:nowrap; }
.keyboardInput { display:none; width:100%; min-height:38px; max-height:96px; resize:vertical; border:1px solid #344255; border-radius:7px; background:#0c1117; color:var(--text); padding:8px 10px; outline:none; }
.keyboardInput.active { display:block; }
.badge { display:inline-flex; align-items:center; min-height:34px; padding:3px 8px; border-radius:999px; background:#223045; color:var(--muted); font-size:12px; white-space:nowrap; }
.badge.active { background:rgba(34,197,94,.18); color:var(--ok); }
.badge.failed { background:rgba(239,68,68,.18); color:var(--bad); }
#statsPanel { position:fixed; left:12px; bottom:12px; z-index:9; min-width:190px; max-width:calc(100vw - 24px); padding:8px 10px; border:1px solid var(--line); border-radius:8px; background:rgba(12,17,23,.72); color:#d7dee8; font:12px/1.45 ui-monospace,SFMono-Regular,Consolas,"Liberation Mono",monospace; white-space:pre; pointer-events:none; backdrop-filter:blur(8px); }
#lock { position:fixed; inset:0; z-index:50; display:none; align-items:center; justify-content:center; padding:20px; background:rgba(0,0,0,.72); }
#lock.active { display:flex; }
.pin { width:min(360px,100%); padding:18px; border:1px solid #344255; border-radius:8px; background:#111820; box-shadow:0 20px 60px rgba(0,0,0,.36); }
.pin label { display:block; margin:0 0 10px; color:#9da9b7; font-size:12px; text-transform:uppercase; letter-spacing:.08em; }
.pin input { width:100%; height:44px; border:1px solid #344255; border-radius:7px; background:#0c1117; color:var(--text); padding:0 11px; outline:none; font-size:22px; text-align:center; }
#pinError { min-height:18px; margin-top:9px; color:var(--bad); font-size:13px; }
@media (max-width:640px) {
  .controlOverlay { left:8px; right:8px; bottom:8px; width:auto; max-width:none; gap:6px; padding:7px; }
  .controlOverlay.idle { left:auto; width:min(260px,calc(100vw - 16px)); }
  .controlRow { flex-wrap:wrap; gap:6px; }
  .controlRow > button:not(.controlHandle), .controlRow > .badge, .clipboardBtn, .backspaceBtn { min-height:32px; padding:7px 8px; font-size:13px; }
  .controlRow > .badge { flex:1 1 auto; min-width:92px; }
  .controlRow > button:not(.controlHandle) { flex:1 1 auto; min-width:72px; }
  #statsPanel { bottom:86px; left:8px; max-width:calc(100vw - 16px); }
}
</style>
</head>
<body>
<div id="stage">
  <video id="video" autoplay playsinline controls></video>
  <audio id="audio" autoplay playsinline></audio>
  <div id="inputLayer" tabindex="-1"></div>
</div>
<div class="controlOverlay" id="controls">
  <div class="controlRow">
    <button id="btnHide" class="controlHandle" type="button" aria-label="toggle controls"></button>
    <span class="badge" id="inputState">input: waiting</span>
    <button id="btnAudio" type="button">开启声音</button>
    <button id="btnInput" type="button">Enable Input</button>
    <button id="btnFull" type="button" title="向设备发送 F11">F11 全屏</button>
  </div>
  <div class="clipboardRow">
    <button id="btnKeyboardDialog" class="clipboardBtn" type="button">键盘 / 剪贴板</button>
    <button id="btnBackspace" class="backspaceBtn" type="button" title="发送一次 Backspace">退格</button>
  </div>
  <textarea id="keyboardInput" class="keyboardInput" rows="2" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="在这里输入或粘贴，内容会发送到设备"></textarea>
</div>
<div id="statsPanel">stats: waiting</div>
<div id="lock">
  <div class="pin">
    <label for="pinInput">PIN</label>
    <input id="pinInput" type="password" inputmode="numeric" autocomplete="off" maxlength="4">
    <div id="pinError"></div>
  </div>
</div>
<script>
(function () {
  "use strict";
  var cfg = null;
  var password = "";
  var pc = null;
  var dc = null;
  var ws = null;
  var remoteStream = null;
  var pendingRemoteCandidates = [];
  var keepalive = 0;
  var controlIdleTimer = 0;
  var statsTimer = 0;
  var inputEnabled = false;
  var audioOn = false;
  var lastMove = 0;
  var pressedKeys = {};
  var activePointer = null;
  var touchStart = null;
  var touchMouseDown = false;
  var touchLongPressed = false;
  var lastTouch = null;
  var longPressTimer = 0;
  var wheelRemainder = 0;
  var lastStatsFrames = 0;
  var lastStatsTime = 0;
  var video = document.getElementById("video");
  var audio = document.getElementById("audio");
  var controls = document.getElementById("controls");
  var inputLayer = document.getElementById("inputLayer");
  var statsPanel = document.getElementById("statsPanel");
  var lock = document.getElementById("lock");
  var pinInput = document.getElementById("pinInput");
  var pinError = document.getElementById("pinError");
  var inputState = document.getElementById("inputState");
  var btnAudio = document.getElementById("btnAudio");
  var btnInput = document.getElementById("btnInput");
  var btnKeyboardDialog = document.getElementById("btnKeyboardDialog");
  var btnBackspace = document.getElementById("btnBackspace");
  var btnFull = document.getElementById("btnFull");
  var btnHide = document.getElementById("btnHide");
  var keyboardInput = document.getElementById("keyboardInput");
  var keyboardComposing = false;

  video.muted = true;
  audio.muted = true;

  function setStatus(text, bad) {
    inputState.textContent = text;
    inputState.classList.toggle("active", !bad && (text.indexOf("ready") !== -1 || text.indexOf("active") !== -1 || text.indexOf("connected") !== -1));
    inputState.classList.toggle("failed", !!bad);
  }
  function setStatsText(text) {
    statsPanel.textContent = text;
  }
  function send(obj) {
    if (dc && dc.readyState === "open") dc.send(JSON.stringify(obj));
  }
  function scheduleControlAutoHide() {
    if (controlIdleTimer) clearTimeout(controlIdleTimer);
    controlIdleTimer = setTimeout(function () {
      if (lock.classList.contains("active")) return;
      if (keyboardInput && (keyboardInput.classList.contains("active") || document.activeElement === keyboardInput)) return;
      controls.classList.add("idle");
      refocusInputLayer();
    }, 30000);
  }
  function showControls() {
    controls.classList.remove("idle");
    scheduleControlAutoHide();
  }
  function hideControls() {
    if (controlIdleTimer) clearTimeout(controlIdleTimer);
    controls.classList.add("idle");
    refocusInputLayer();
  }
  function toggleControls() {
    if (controls.classList.contains("idle")) showControls();
    else hideControls();
  }
  function refocusInputLayer() {
    if (!inputEnabled) return;
    try { inputLayer.focus({ preventScroll: true }); } catch (e) {}
  }
  function openKeyboardInput(statusText) {
    if (!inputReady()) {
      setStatus("input: not ready", true);
      return;
    }
    controls.classList.remove("idle");
    keyboardInput.classList.add("active");
    if (btnKeyboardDialog) btnKeyboardDialog.textContent = "关闭输入框";
    keyboardInput.value = "";
    if (statusText) setStatus(statusText);
    try { keyboardInput.focus({ preventScroll: true }); } catch (e) { keyboardInput.focus(); }
    scheduleControlAutoHide();
  }
  function closeKeyboardInput(statusText) {
    if (!keyboardInput) return;
    keyboardInput.classList.remove("active");
    keyboardInput.value = "";
    if (btnKeyboardDialog) btnKeyboardDialog.textContent = "键盘 / 剪贴板";
    if (statusText) setStatus(statusText);
    refocusInputLayer();
    scheduleControlAutoHide();
  }
  function toggleKeyboardInput() {
    if (!inputReady()) {
      setStatus("input: not ready", true);
      return;
    }
    if (keyboardInput.classList.contains("active")) {
      closeKeyboardInput("input: closed");
      return;
    }
    send({ type: "osk" });
    openKeyboardInput("input: keyboard ready");
  }
  function sendKeyboardInputBuffer() {
    if (keyboardComposing) return;
    var text = keyboardInput.value;
    if (!text) return;
    keyboardInput.value = "";
    send({ type: "text", text: text });
    setStatus("input: keyboard typed");
  }
  function sendBackspaceOnce() {
    if (!inputReady()) {
      setStatus("input: not ready", true);
      return;
    }
    send({ type: "key", key: "Backspace", code: "Backspace", down: true });
    setTimeout(function () {
      send({ type: "key", key: "Backspace", code: "Backspace", down: false });
    }, 35);
    setStatus("input: backspace sent");
    refocusInputLayer();
    scheduleControlAutoHide();
  }
  function sendF11Once() {
    if (!inputReady()) {
      setStatus("input: not ready", true);
      return;
    }
    send({ type: "key", key: "F11", code: "F11", down: true });
    setTimeout(function () {
      send({ type: "key", key: "F11", code: "F11", down: false });
    }, 45);
    setStatus("input: F11 sent");
    refocusInputLayer();
    scheduleControlAutoHide();
  }
  function preferH264(transceiver) {
    if (!transceiver || !transceiver.setCodecPreferences || !window.RTCRtpReceiver) return;
    var caps = RTCRtpReceiver.getCapabilities && RTCRtpReceiver.getCapabilities("video");
    if (!caps || !caps.codecs) return;
    var h264 = caps.codecs.filter(function (codec) {
      var mime = (codec.mimeType || "").toLowerCase();
      var fmtp = codec.sdpFmtpLine || "";
      return mime === "video/h264" && (!fmtp || fmtp.indexOf("packetization-mode=1") !== -1);
    });
    if (h264.length) transceiver.setCodecPreferences(h264);
  }
  function stopStats() {
    if (statsTimer) clearInterval(statsTimer);
    statsTimer = 0;
  }
  function startStats() {
    stopStats();
    lastStatsFrames = 0;
    lastStatsTime = performance.now();
    statsTimer = setInterval(async function () {
      if (!pc) return;
      try {
        var report = await pc.getStats();
        var rtt = null;
        var localType = "";
        var remoteType = "";
        report.forEach(function (item) {
          if (item.type === "candidate-pair" && item.state === "succeeded" && item.nominated) {
            rtt = Math.round((item.currentRoundTripTime || 0) * 1000);
            var local = report.get(item.localCandidateId);
            var remote = report.get(item.remoteCandidateId);
            localType = local && local.candidateType ? local.candidateType : "";
            remoteType = remote && remote.candidateType ? remote.candidateType : "";
          }
        });
        report.forEach(function (item) {
          if (item.type === "inbound-rtp" && item.kind === "video" && !item.isRemote) {
            var now = performance.now();
            var frames = item.framesDecoded || 0;
            var fps = 0;
            if (lastStatsTime > 0) {
              fps = Math.max(0, Math.round((frames - lastStatsFrames) * 1000 / Math.max(1, now - lastStatsTime)));
            }
            lastStatsFrames = frames;
            lastStatsTime = now;
            window.__kmsStats = {
              packets: item.packetsReceived || 0,
              frames: frames,
              dropped: item.framesDropped || 0,
              lost: item.packetsLost || 0
            };
            setStatsText(
              "fps: " + fps + "\n" +
              "packets: " + (item.packetsReceived || 0) + "\n" +
              "frames: " + frames + "\n" +
              "dropped: " + (item.framesDropped || 0) + "\n" +
              "lost: " + (item.packetsLost || 0) + "\n" +
              "rtt: " + (rtt === null ? "?" : rtt + " ms") + "\n" +
              "pair: " + (localType || "?") + " -> " + (remoteType || "?")
            );
          }
        });
      } catch (e) {}
    }, 1000);
  }
  function inputReady() {
    return dc && dc.readyState === "open";
  }
  function setInputEnabled(enabled) {
    if (enabled && !inputReady()) {
      setStatus("input: not ready", true);
      return;
    }
    if (!enabled && inputEnabled) send({ type: "all_up" });
    inputEnabled = !!enabled;
    inputLayer.classList.toggle("enabled", inputEnabled);
    btnInput.textContent = inputEnabled ? "Disable Input" : "Enable Input";
    if (inputEnabled) {
      video.controls = false;
      pressedKeys = {};
      try { inputLayer.focus({ preventScroll: true }); } catch (e) {}
      setStatus("input: active");
    } else {
      video.controls = true;
      activePointer = null;
      pressedKeys = {};
      setStatus(inputReady() ? "input: ready" : "input: waiting", !inputReady());
    }
  }
  function closePeer() {
    stopStats();
    setInputEnabled(false);
    if (keepalive) clearInterval(keepalive);
    keepalive = 0;
    if (ws) { try { ws.close(); } catch (e) {} ws = null; }
    if (dc) { try { dc.close(); } catch (e) {} dc = null; }
    if (pc) { try { pc.close(); } catch (e) {} pc = null; }
    remoteStream = null;
    video.srcObject = null;
    audio.srcObject = null;
    setStatsText("stats: waiting");
  }
  async function connect() {
    closePeer();
    setStatus("connecting");
    audioOn = false;
    pendingRemoteCandidates = [];
    btnAudio.textContent = "开启声音";
    try {
      var wsScheme = location.protocol === "https:" ? "wss:" : "ws:";
      ws = new WebSocket(wsScheme + "//" + location.host + "/ws?password=" + encodeURIComponent(password));
      pc = new RTCPeerConnection({ iceServers: [] });
      preferH264(pc.addTransceiver("video", { direction: "recvonly" }));
      if (cfg.audioEnabled) pc.addTransceiver("audio", { direction: "recvonly" });
      dc = pc.createDataChannel("input", { ordered: true });
      dc.onopen = function () {
        setStatus("input: ready");
        scheduleControlAutoHide();
        keepalive = setInterval(function () { send({ type: "keepalive" }); }, 5000);
      };
      dc.onclose = function () {
        setInputEnabled(false);
        setStatus("input: closed", true);
      };
      pc.ontrack = function (event) {
        // Keep audio and video in one HTMLMediaElement. Separate <video> and
        // <audio> elements have independent playback clocks and gradually
        // drift apart when the encoder or network drops a frame.
        if (!remoteStream) {
          remoteStream = new MediaStream();
          video.srcObject = remoteStream;
        }
        var exists = remoteStream.getTracks().some(function (track) {
          return track.id === event.track.id;
        });
        if (!exists) remoteStream.addTrack(event.track);
        if (event.track.kind === "audio") {
          video.muted = !audioOn;
        }
        event.track.onunmute = function () { video.play().catch(function () {}); };
        video.play().catch(function () {});
      };
      pc.onconnectionstatechange = function () {
        if (pc.connectionState === "failed" || pc.connectionState === "closed") setStatus("connection: " + pc.connectionState, true);
      };
      pc.oniceconnectionstatechange = function () {
        if (pc.iceConnectionState === "connected" || pc.iceConnectionState === "completed") {
          setStatus("input: connected");
          startStats();
        }
      };
      pc.onicecandidate = function (event) {
        if (event.candidate && ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ type: "candidate", candidate: event.candidate.toJSON() }));
      };
      ws.onopen = async function () {
        try {
          var offer = await pc.createOffer();
          await pc.setLocalDescription(offer);
          ws.send(JSON.stringify({ type: "offer", sdp: pc.localDescription }));
        } catch (err) {
          closePeer();
          setStatus(err.message || String(err), true);
        }
      };
      ws.onmessage = async function (event) {
        try {
          var msg = JSON.parse(event.data);
          if (msg.type === "answer" && msg.sdp) {
            await pc.setRemoteDescription(msg.sdp);
            while (pendingRemoteCandidates.length) {
              try { await pc.addIceCandidate(pendingRemoteCandidates.shift()); } catch (e) {}
            }
            return;
          }
          if (msg.type === "candidate" && msg.candidate) {
            if (pc.remoteDescription && pc.remoteDescription.type) {
              try { await pc.addIceCandidate(msg.candidate); } catch (e) {}
            } else {
              pendingRemoteCandidates.push(msg.candidate);
            }
            return;
          }
          if (msg.type === "error") {
            closePeer();
            setStatus(msg.error || "signaling error", true);
          }
        } catch (err) {
          setStatus(err.message || String(err), true);
        }
      };
      ws.onclose = function () {
        if (pc && pc.connectionState !== "connected" && pc.connectionState !== "closed") setStatus("signaling closed", true);
      };
      ws.onerror = function () { setStatus("signaling error", true); };
    } catch (err) {
      closePeer();
      setStatus(err.message || String(err), true);
      if (cfg.passwordEnabled) {
        lock.classList.add("active");
        pinInput.value = "";
        pinInput.focus();
      }
    }
  }
  function normFromEvent(event) {
    var rect = inputLayer.getBoundingClientRect();
    var vw = video.videoWidth || cfg.inputWidth || 1920;
    var vh = video.videoHeight || cfg.inputHeight || 1080;
    var contentW = rect.width;
    var contentH = rect.height;
    var contentX = rect.left;
    var contentY = rect.top;
    var rectAR = rect.width / rect.height;
    var videoAR = vw / vh;
    if (rectAR > videoAR) {
      contentW = rect.height * videoAR;
      contentX = rect.left + (rect.width - contentW) / 2;
    } else {
      contentH = rect.width / videoAR;
      contentY = rect.top + (rect.height - contentH) / 2;
    }
    return {
      x: Math.max(0, Math.min(1, (event.clientX - contentX) / contentW)),
      y: Math.max(0, Math.min(1, (event.clientY - contentY) / contentH))
    };
  }
  function buttonName(event) {
    if (event.button === 2) return "right";
    if (event.button === 1) return "middle";
    return "left";
  }
  function clearLongPress() {
    if (longPressTimer) clearTimeout(longPressTimer);
    longPressTimer = 0;
  }
  function isLocalTextInput(element) {
    if (!element) return false;
    var tag = element.tagName;
    return tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT" || element.isContentEditable;
  }
  inputLayer.addEventListener("pointerdown", function (event) {
    if (!inputEnabled) return;
    event.preventDefault();
    try { inputLayer.setPointerCapture(event.pointerId); } catch (e) {}
    activePointer = event.pointerId;
    var p = normFromEvent(event);
    send({ type: "move", x: p.x, y: p.y });
    if (event.pointerType === "touch") {
      touchStart = p;
      touchMouseDown = false;
      touchLongPressed = false;
      lastTouch = p;
      clearLongPress();
      longPressTimer = setTimeout(function () {
        touchLongPressed = true;
        send({ type: "click", x: lastTouch.x, y: lastTouch.y, button: "right" });
        activePointer = null;
      }, 650);
    } else {
      send({ type: "down", x: p.x, y: p.y, button: buttonName(event) });
    }
  });
  inputLayer.addEventListener("pointermove", function (event) {
    if (!inputEnabled || activePointer !== event.pointerId) return;
    var now = Date.now();
    if (now - lastMove < 16) return;
    lastMove = now;
    event.preventDefault();
    var p = normFromEvent(event);
    if (event.pointerType === "touch") {
      lastTouch = p;
      var dx = Math.abs(p.x - (touchStart ? touchStart.x : p.x));
      var dy = Math.abs(p.y - (touchStart ? touchStart.y : p.y));
      if ((dx > 0.015 || dy > 0.015) && !touchMouseDown && !touchLongPressed) {
        clearLongPress();
        send({ type: "down", x: (touchStart || p).x, y: (touchStart || p).y, button: "left" });
        touchMouseDown = true;
      }
    }
    send({ type: "move", x: p.x, y: p.y });
  });
  inputLayer.addEventListener("pointerup", function (event) {
    if (!inputEnabled || activePointer !== event.pointerId) return;
    event.preventDefault();
    var p = normFromEvent(event);
    clearLongPress();
    if (event.pointerType === "touch" && touchMouseDown) {
      send({ type: "up", x: p.x, y: p.y, button: "left" });
    } else if (event.pointerType === "touch" && !touchLongPressed) {
      send({ type: "click", x: p.x, y: p.y, button: "left" });
    } else {
      send({ type: "up", x: p.x, y: p.y, button: buttonName(event) });
    }
    activePointer = null;
    touchStart = null;
    touchMouseDown = false;
    touchLongPressed = false;
  });
  inputLayer.addEventListener("pointercancel", function () {
    clearLongPress();
    activePointer = null;
    touchStart = null;
    touchMouseDown = false;
    touchLongPressed = false;
    send({ type: "all_up" });
  });
  inputLayer.addEventListener("contextmenu", function (event) { event.preventDefault(); });
  inputLayer.addEventListener("wheel", function (event) {
    if (!inputEnabled) return;
    event.preventDefault();
    var p = normFromEvent(event);
    wheelRemainder += -event.deltaY / 240;
    var steps = wheelRemainder > 0 ? Math.floor(wheelRemainder) : Math.ceil(wheelRemainder);
    if (steps !== 0) {
      wheelRemainder -= steps;
      send({ type: "wheel", x: p.x, y: p.y, steps: Math.max(-1, Math.min(1, steps)) });
    }
  }, { passive: false });
  document.addEventListener("keydown", function (event) {
    if (!inputEnabled || isLocalTextInput(document.activeElement)) return;
    var id = event.code || event.key;
    if (event.repeat && pressedKeys[id]) return;
    pressedKeys[id] = true;
    send({ type: "key", key: event.key, code: event.code, down: true });
    event.preventDefault();
  });
  document.addEventListener("keyup", function (event) {
    if (!inputEnabled || isLocalTextInput(document.activeElement)) return;
    var id = event.code || event.key;
    delete pressedKeys[id];
    send({ type: "key", key: event.key, code: event.code, down: false });
    event.preventDefault();
  });
  window.addEventListener("blur", function () { send({ type: "all_up" }); });
  window.addEventListener("beforeunload", function () {
    send({ type: "all_up" });
    closePeer();
  });
  btnAudio.addEventListener("click", function () {
    audioOn = !audioOn;
    video.muted = !audioOn;
    btnAudio.textContent = audioOn ? "关闭声音" : "开启声音";
    send({ type: "audio", enabled: audioOn });
    video.play().catch(function () {});
    scheduleControlAutoHide();
  });
  btnInput.addEventListener("click", function () {
    setInputEnabled(!inputEnabled);
    scheduleControlAutoHide();
  });
  btnKeyboardDialog.addEventListener("click", toggleKeyboardInput);
  btnBackspace.addEventListener("click", sendBackspaceOnce);
  keyboardInput.addEventListener("compositionstart", function () {
    keyboardComposing = true;
  });
  keyboardInput.addEventListener("compositionend", function () {
    keyboardComposing = false;
    sendKeyboardInputBuffer();
  });
  keyboardInput.addEventListener("input", sendKeyboardInputBuffer);
  keyboardInput.addEventListener("keydown", function (event) {
    event.stopPropagation();
  });
  keyboardInput.addEventListener("keyup", function (event) {
    event.stopPropagation();
  });
  btnFull.addEventListener("click", function () {
    sendF11Once();
  });
  btnHide.addEventListener("click", toggleControls);
  pinInput.addEventListener("input", function () {
    pinInput.value = pinInput.value.replace(/\D/g, "").slice(0, 4);
    pinError.textContent = "";
    if (pinInput.value.length === 4) {
      password = pinInput.value;
      lock.classList.remove("active");
      connect();
    }
  });
  pinInput.addEventListener("keydown", function (event) {
    if (event.key === "Enter") {
      if (pinInput.value.length < 4) {
        pinError.textContent = "Please enter 4 digits";
        return;
      }
      password = pinInput.value;
      lock.classList.remove("active");
      connect();
    }
  });
  fetch("/api/config", { cache: "no-store" })
    .then(function (resp) { return resp.json(); })
    .then(function (data) {
      cfg = data;
      if (cfg.passwordEnabled) {
        lock.classList.add("active");
        pinInput.focus();
      } else {
        connect();
      }
      showControls();
    })
    .catch(function (err) { setStatus(err.message || String(err), true); });
})();
</script>
</body>
</html>
`

func drainRTCP(sender *webrtc.RTPSender) {
	buf := make([]byte, 1500)
	for {
		if _, _, err := sender.Read(buf); err != nil {
			return
		}
	}
}

func (s *session) drainVideoRTCP(sender *webrtc.RTPSender) {
	buf := make([]byte, 1500)
	for {
		n, _, err := sender.Read(buf)
		if err != nil {
			return
		}
		packets, err := rtcp.Unmarshal(buf[:n])
		if err != nil {
			continue
		}
		for _, pkt := range packets {
			switch pkt.(type) {
			case *rtcp.PictureLossIndication, *rtcp.FullIntraRequest:
				s.requestIDR()
			}
		}
	}
}

func (s *session) requestIDR() {
	// The RK3588 encoder is configured with a one-second GOP, so a fresh IDR
	// arrives naturally. Some vendor MPP builds emit a broken sequence after
	// MPP_ENC_SET_IDR_FRAME while streaming; forwarding PLI/FIR as SIGUSR1 then
	// freezes browser decoding even though RTP packets continue. Let the regular
	// GOP recover instead of disrupting a healthy stream.
	if s.cfg.Verbose {
		log.Printf("viewer=%s PLI/FIR received; waiting for periodic IDR", s.viewerID)
	}
}

type dataChannelControl struct {
	Type    string `json:"type"`
	Enabled bool   `json:"enabled"`
}

func (s *session) handleControlMessage(ctx context.Context, data []byte) (bool, error) {
	var msg dataChannelControl
	if err := json.Unmarshal(data, &msg); err != nil {
		return false, nil
	}
	switch msg.Type {
	case "audio":
		return true, s.setAudioEnabled(ctx, msg.Enabled)
	case "keepalive", "ping":
		return true, nil
	default:
		return false, nil
	}
}

func (s *session) startMediaForwarder(ctx context.Context) error {
	switch strings.ToLower(strings.TrimSpace(s.cfg.SourceMode)) {
	case "", "command", "annexb", "stdout":
		return s.startAnnexBCommandForwarder(ctx)
	default:
		return fmt.Errorf("unknown source mode %q", s.cfg.SourceMode)
	}
}

func (s *session) startMediaForwarderOnce(ctx context.Context) error {
	s.mediaMu.Lock()
	defer s.mediaMu.Unlock()
	if s.mediaOn {
		return nil
	}
	if err := s.startMediaForwarder(ctx); err != nil {
		return err
	}
	s.mediaOn = true
	log.Printf("viewer=%s media forwarder started source=%s", s.viewerID, s.cfg.SourceMode)
	return nil
}

func (s *session) setAudioEnabled(ctx context.Context, enabled bool) error {
	s.audioMu.Lock()
	defer s.audioMu.Unlock()
	if !s.cfg.AudioEnabled || s.audioTrack == nil {
		return errors.New("audio is disabled")
	}
	if enabled {
		if s.audioOn {
			return nil
		}
		if err := s.startAudioRTPForwarderLocked(ctx); err != nil {
			return err
		}
		s.audioOn = true
		log.Printf("viewer=%s audio forwarder started source=%s:%s", s.viewerID, s.cfg.AudioBackend, s.cfg.AudioSource)
		return nil
	}
	s.stopAudioLocked()
	log.Printf("viewer=%s audio forwarder stopped", s.viewerID)
	return nil
}

func pulseInfo(server string) (string, bool) {
	cmd := exec.Command("pactl", "info")
	cmd.Env = os.Environ()
	if server != "" {
		cmd.Env = append(cmd.Env, "PULSE_SERVER="+server)
	}
	out, err := cmd.Output()
	return string(out), err == nil
}

func resolvePulseInput(server, source string) (string, string) {
	if strings.EqualFold(server, "auto") {
		server = ""
		candidates := []string{"unix:/tmp/pulse-socket"}
		if entries, err := os.ReadDir("/run/user"); err == nil {
			for _, entry := range entries {
				socketPath := "/run/user/" + entry.Name() + "/pulse/native"
				if info, statErr := os.Stat(socketPath); statErr == nil && info.Mode()&os.ModeSocket != 0 {
					candidates = append(candidates, "unix:"+socketPath)
				}
			}
		}
		for _, candidate := range candidates {
			if _, ok := pulseInfo(candidate); ok {
				server = candidate
				break
			}
		}
	}

	if !strings.EqualFold(source, "auto") && source != "" {
		return server, source
	}
	source = "default"
	if info, ok := pulseInfo(server); ok {
		for _, line := range strings.Split(info, "\n") {
			line = strings.TrimSpace(line)
			if strings.HasPrefix(line, "Default Sink:") {
				sink := strings.TrimSpace(strings.TrimPrefix(line, "Default Sink:"))
				if sink != "" {
					source = sink + ".monitor"
				}
				break
			}
		}
	}
	return server, source
}

func (s *session) startAudioRTPForwarderLocked(ctx context.Context) error {
	addr, err := net.ResolveUDPAddr("udp4", "127.0.0.1:0")
	if err != nil {
		return err
	}
	conn, err := net.ListenUDP("udp4", addr)
	if err != nil {
		return err
	}
	s.audioUDP = conn
	port := conn.LocalAddr().(*net.UDPAddr).Port
	pulseServer := strings.TrimSpace(s.cfg.PulseServer)
	audioSource := strings.TrimSpace(s.cfg.AudioSource)
	if strings.EqualFold(strings.TrimSpace(s.cfg.AudioBackend), "pulse") {
		pulseServer, audioSource = resolvePulseInput(pulseServer, audioSource)
	}

	args := []string{
		"-hide_banner",
		"-loglevel", s.cfg.FFmpegLogLevel,
		"-fflags", "nobuffer",
		"-flags", "low_delay",
		"-thread_queue_size", strconv.Itoa(s.cfg.AudioQueueSize),
		"-f", s.cfg.AudioBackend,
	}
	if strings.ToLower(strings.TrimSpace(s.cfg.AudioBackend)) == "pulse" {
		if pulseServer != "" {
			args = append(args, "-server", pulseServer)
		}
		args = append(args,
			"-sample_rate", "48000",
			"-channels", "2",
			"-frame_size", "960",
			"-fragment_size", "960",
			"-wallclock", "1",
		)
	}
	args = append(args,
		"-i", audioSource,
		"-vn",
		"-af", "aresample=async=1:first_pts=0",
		"-c:a", "libopus",
		"-application", "lowdelay",
		"-frame_duration", "10",
		"-b:a", s.cfg.AudioBitrate,
		"-ar", "48000",
		"-ac", "2",
		"-muxdelay", "0",
		"-f", "rtp",
		"-payload_type", "111",
		"-flush_packets", "1",
		fmt.Sprintf("rtp://127.0.0.1:%d?pkt_size=%d", port, s.cfg.PacketSize),
	)

	cmd := exec.CommandContext(ctx, s.cfg.FFmpeg, args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	s.audioCmd = cmd
	if err := cmd.Start(); err != nil {
		_ = conn.Close()
		s.audioUDP = nil
		return err
	}
	log.Printf("viewer=%s audio ffmpeg pid=%d source=%s server=%s udp=127.0.0.1:%d",
		s.viewerID, cmd.Process.Pid, audioSource, pulseServer, port)

	go func() {
		err := cmd.Wait()
		if err != nil && ctx.Err() == nil {
			log.Printf("viewer=%s audio ffmpeg exited: %v", s.viewerID, err)
		}
		s.audioMu.Lock()
		if s.audioCmd == cmd {
			s.audioCmd = nil
			s.audioOn = false
		}
		s.audioMu.Unlock()
	}()
	go s.forwardAudioRTP(ctx, conn)
	return nil
}

func (s *session) stopAudioLocked() {
	if s.audioUDP != nil {
		_ = s.audioUDP.Close()
		s.audioUDP = nil
	}
	if s.audioCmd != nil && s.audioCmd.Process != nil {
		_ = s.audioCmd.Process.Signal(syscall.SIGTERM)
	}
	s.audioCmd = nil
	s.audioOn = false
}

func (s *session) forwardAudioRTP(ctx context.Context, conn *net.UDPConn) {
	buf := make([]byte, 2048)
	for {
		_ = conn.SetReadDeadline(time.Now().Add(time.Second))
		n, _, err := conn.ReadFromUDP(buf)
		if err != nil {
			if ctx.Err() != nil || strings.Contains(err.Error(), "use of closed network connection") {
				return
			}
			if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
				continue
			}
			log.Printf("viewer=%s audio udp read failed: %v", s.viewerID, err)
			return
		}
		var pkt rtp.Packet
		if err := pkt.Unmarshal(buf[:n]); err != nil {
			if s.cfg.Verbose {
				log.Printf("viewer=%s bad audio rtp packet: %v", s.viewerID, err)
			}
			continue
		}
		if err := s.audioTrack.WriteRTP(&pkt); err != nil && s.cfg.Verbose {
			log.Printf("viewer=%s write audio rtp failed: %v", s.viewerID, err)
		}
	}
}

func (s *session) startAnnexBCommandForwarder(ctx context.Context) error {
	if strings.TrimSpace(s.cfg.H264Command) == "" {
		return errors.New("H264_COMMAND is empty")
	}

	cmd := exec.CommandContext(ctx, "sh", "-c", s.cfg.H264Command)
	configureCommandGroupCancellation(cmd)
	if strings.TrimSpace(s.cfg.H264WorkDir) != "" {
		cmd.Dir = s.cfg.H264WorkDir
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return err
	}
	cmd.Stderr = os.Stderr
	s.cmd = cmd

	if err := cmd.Start(); err != nil {
		return err
	}
	log.Printf("viewer=%s h264 command pid=%d mode=annexb", s.viewerID, cmd.Process.Pid)

	go func() {
		err := cmd.Wait()
		if err != nil && ctx.Err() == nil {
			log.Printf("viewer=%s h264 command exited: %v", s.viewerID, err)
		}
	}()
	go s.forwardAnnexB(ctx, stdout)
	return nil
}

func configureCommandGroupCancellation(cmd *exec.Cmd) {
	// The selected video backend can be a small process tree (capture, FIFO and
	// MPP encoder). Put it in its own process group and make context cancellation
	// terminate the whole group, otherwise a blocked FFmpeg child can outlive the
	// viewer and make systemd stop time out.
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	cmd.Cancel = func() error {
		if cmd.Process == nil {
			return os.ErrProcessDone
		}
		err := syscall.Kill(-cmd.Process.Pid, syscall.SIGTERM)
		if errors.Is(err, syscall.ESRCH) {
			return os.ErrProcessDone
		}
		return err
	}
	cmd.WaitDelay = 3 * time.Second
}

func (s *session) forwardAnnexB(ctx context.Context, r io.Reader) {
	// Keep only a small parser queue so transient network backpressure cannot
	// turn into seconds of stale desktop video.
	nals := make(chan []byte, s.cfg.H264NALQueue)
	go func() {
		defer close(nals)
		if err := readAnnexBNALs(ctx, r, nals); err != nil && ctx.Err() == nil {
			log.Printf("viewer=%s annexb reader stopped: %v", s.viewerID, err)
		}
	}()

	writer := newH264RTPWriter(s.track, s.cfg.PacketSize, s.cfg.FPS)
	var au [][]byte
	hasVCL := false
	var startupSPS []byte
	var startupPPS []byte
	startupReady := false
	startupDropped := uint64(0)
	var accessUnits uint64
	var nalUnits uint64
	lastLog := time.Now()

	flush := func() {
		if len(au) == 0 {
			return
		}
		nalCount := len(au)
		if err := writer.writeAccessUnit(au); err != nil {
			log.Printf("viewer=%s write access unit failed: %v", s.viewerID, err)
		} else {
			accessUnits++
			nalUnits += uint64(nalCount)
			if time.Since(lastLog) >= 5*time.Second {
				log.Printf("viewer=%s h264 forwarded access_units=%d nals=%d", s.viewerID, accessUnits, nalUnits)
				lastLog = time.Now()
			}
		}
		au = nil
		hasVCL = false
	}

	for {
		select {
		case <-ctx.Done():
			return
		case nal, ok := <-nals:
			if !ok {
				flush()
				return
			}
			if len(nal) == 0 {
				continue
			}
			nalType := nal[0] & 0x1f
			if nalType == 9 || nalType == 12 {
				continue
			}

			// The vendor MPP build can occasionally corrupt the first output
			// packet while its internal hardware buffers are warming up.  The
			// packet then contains hundreds of repeated or over-sized parameter
			// sets, and Chromium never obtains a decodable first frame.  Cache
			// only plausible SPS/PPS NALs and begin RTP output at the first clean
			// IDR.  Later parameter sets are intentionally ignored because the
			// encoder configuration is fixed for the lifetime of the session.
			if nalType == 7 {
				if !startupReady && len(nal) <= 256 {
					startupSPS = append(startupSPS[:0], nal...)
				} else if !startupReady {
					startupDropped++
				}
				continue
			}
			if nalType == 8 {
				if !startupReady && len(nal) <= 128 {
					startupPPS = append(startupPPS[:0], nal...)
				} else if !startupReady {
					startupDropped++
				}
				continue
			}
			isVCL := nalType >= 1 && nalType <= 5
			if !startupReady {
				if nalType != 5 || len(startupSPS) == 0 || len(startupPPS) == 0 {
					startupDropped++
					continue
				}
				au = append(au, startupSPS, startupPPS, nal)
				hasVCL = true
				startupReady = true
				log.Printf("viewer=%s h264 startup synchronized dropped_nals=%d sps=%d pps=%d", s.viewerID, startupDropped, len(startupSPS), len(startupPPS))
				continue
			}
			if !isVCL && hasVCL {
				flush()
			}
			if isVCL && hasVCL {
				flush()
			}
			if isVCL {
				hasVCL = true
			}
			au = append(au, nal)
		}
	}
}

func readAnnexBNALs(ctx context.Context, r io.Reader, out chan<- []byte) error {
	buf := make([]byte, 0, 256*1024)
	tmp := make([]byte, 32*1024)
	for {
		n, err := r.Read(tmp)
		if n > 0 {
			buf = append(buf, tmp[:n]...)
			var nal []byte
			for {
				nal, buf = popAnnexBNAL(buf, false)
				if nal == nil {
					break
				}
				select {
				case <-ctx.Done():
					return nil
				case out <- nal:
				}
			}
		}
		if err != nil {
			if errors.Is(err, io.EOF) {
				for {
					nal, rest := popAnnexBNAL(buf, true)
					buf = rest
					if nal == nil {
						return nil
					}
					select {
					case <-ctx.Done():
						return nil
					case out <- nal:
					}
				}
			}
			return err
		}
	}
}

func popAnnexBNAL(buf []byte, eof bool) ([]byte, []byte) {
	start, startLen, ok := findStartCode(buf, 0)
	if !ok {
		if len(buf) > 4 {
			return nil, append([]byte(nil), buf[len(buf)-4:]...)
		}
		return nil, buf
	}
	if start > 0 {
		buf = buf[start:]
	}
	nalStart := startLen
	next, _, hasNext := findStartCode(buf, nalStart)
	if !hasNext {
		if !eof {
			return nil, buf
		}
		nal := trimNAL(buf[nalStart:])
		if len(nal) == 0 {
			return nil, nil
		}
		return nal, nil
	}
	nal := trimNAL(buf[nalStart:next])
	rest := buf[next:]
	if len(nal) == 0 {
		return nil, rest
	}
	return nal, rest
}

func findStartCode(buf []byte, from int) (int, int, bool) {
	for i := from; i+3 <= len(buf); i++ {
		if buf[i] != 0 || buf[i+1] != 0 {
			continue
		}
		if buf[i+2] == 1 {
			return i, 3, true
		}
		if i+4 <= len(buf) && buf[i+2] == 0 && buf[i+3] == 1 {
			return i, 4, true
		}
	}
	return 0, 0, false
}

func trimNAL(nal []byte) []byte {
	for len(nal) > 0 && nal[len(nal)-1] == 0 {
		nal = nal[:len(nal)-1]
	}
	if len(nal) == 0 {
		return nil
	}
	out := make([]byte, len(nal))
	copy(out, nal)
	return out
}

type h264RTPWriter struct {
	track       *webrtc.TrackLocalStaticRTP
	payloadType uint8
	sequence    uint16
	timestamp   uint32
	baseTS      uint32
	lastTS      uint32
	startedAt   time.Time
	frameTicks  uint32
	ssrc        uint32
	mtu         int
}

func newH264RTPWriter(track *webrtc.TrackLocalStaticRTP, mtu int, fps int) *h264RTPWriter {
	if mtu < 256 {
		mtu = 1200
	}
	if fps <= 0 {
		fps = 30
	}
	frameTicks := uint32(90000 / fps)
	if frameTicks == 0 {
		frameTicks = 1
	}
	initialTS := randomUint32()
	return &h264RTPWriter{
		track:       track,
		payloadType: 96,
		sequence:    randomUint16(),
		timestamp:   initialTS,
		baseTS:      initialTS,
		lastTS:      initialTS,
		frameTicks:  frameTicks,
		ssrc:        randomUint32(),
		mtu:         mtu,
	}
}

func (w *h264RTPWriter) writeAccessUnit(nals [][]byte) error {
	if len(nals) == 0 {
		return nil
	}
	now := time.Now()
	if w.startedAt.IsZero() {
		w.startedAt = now
		w.timestamp = w.baseTS
	} else {
		// The encoder pipe can be read in bursts even though DRM capture is paced
		// by vblank. Wall-clock timestamps therefore produce one large jump and
		// then many one-tick frames, which eventually stalls Chromium's jitter
		// buffer. Keep the RTP clock at the negotiated capture cadence instead.
		w.timestamp = w.lastTS + w.frameTicks
	}
	for i, nal := range nals {
		if len(nal) == 0 {
			continue
		}
		marker := i == len(nals)-1
		if err := w.writeNAL(nal, marker); err != nil {
			return err
		}
	}
	w.lastTS = w.timestamp
	return nil
}

func (w *h264RTPWriter) writeNAL(nal []byte, marker bool) error {
	if len(nal) <= w.mtu {
		return w.writePacket(nal, marker)
	}
	if len(nal) < 2 {
		return nil
	}
	maxFragment := w.mtu - 2
	if maxFragment <= 0 {
		return errors.New("rtp packet size too small for h264 fu-a")
	}
	nalHeader := nal[0]
	fuIndicator := (nalHeader & 0xe0) | 28
	nalType := nalHeader & 0x1f
	payload := nal[1:]
	for offset := 0; offset < len(payload); {
		end := offset + maxFragment
		if end > len(payload) {
			end = len(payload)
		}
		startBit := byte(0)
		if offset == 0 {
			startBit = 0x80
		}
		endBit := byte(0)
		last := end == len(payload)
		if last {
			endBit = 0x40
		}
		fragment := make([]byte, 2+end-offset)
		fragment[0] = fuIndicator
		fragment[1] = startBit | endBit | nalType
		copy(fragment[2:], payload[offset:end])
		if err := w.writePacket(fragment, marker && last); err != nil {
			return err
		}
		offset = end
	}
	return nil
}

func (w *h264RTPWriter) writePacket(payload []byte, marker bool) error {
	pkt := &rtp.Packet{
		Header: rtp.Header{
			Version:        2,
			PayloadType:    w.payloadType,
			SequenceNumber: w.sequence,
			Timestamp:      w.timestamp,
			SSRC:           w.ssrc,
			Marker:         marker,
		},
		Payload: payload,
	}
	w.sequence++
	return w.track.WriteRTP(pkt)
}

func randomUint16() uint16 {
	var b [2]byte
	if _, err := crand.Read(b[:]); err != nil {
		return uint16(time.Now().UnixNano())
	}
	return binary.BigEndian.Uint16(b[:])
}

func randomUint32() uint32 {
	var b [4]byte
	if _, err := crand.Read(b[:]); err != nil {
		return uint32(time.Now().UnixNano())
	}
	return binary.BigEndian.Uint32(b[:])
}

func (s *session) Close() {
	s.closeMu.Do(func() {
		if s.cancel != nil {
			s.cancel()
		}
		s.audioMu.Lock()
		s.stopAudioLocked()
		s.audioMu.Unlock()
		if s.pc != nil {
			_ = s.pc.Close()
		}
	})
}
