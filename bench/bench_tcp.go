// bench_tcp.go — RPC benchmark CLIENT over TCP (bench-tcp pod, baseline).
//
// TCP counterpart of bench/bench_sock.c: the SAME closed-loop RPC methodology,
// but driving kernel TCP through the Envoy sidecar instead of the DPUmesh DPU
// transport. This is the service-mesh baseline the DPUmesh side is compared
// against, measured identically so the numbers line up.
//
// Methodology (closed loop, fixed concurrency window, warmup, greeter SayHello
// with a fixed small reply, per-thread rate summed) — see bench_sock.c. Each
// client thread owns ONE TCP connection and keeps `concurrency` requests
// outstanding on it via a writer/reader goroutine pair.
//
// Control protocol (TCP :9092):
//   RUN <req_size> <reply_size> <concurrency> <duration_s> <warmup> <threads>
//        -> OK mrps=.. gbps=.. p50=.. p95=.. p99=.. avg=.. min=.. max=..
//              rcnt=.. fail=.. conc=.. threads=.. reqsz=.. repsz=.. durs=..
//   PING -> PONG
//
// env: BENCH_TARGET (upstream, default "sidecar:9091").
package main

import (
	"bufio"
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	ctrlPortDefault = 9092
	reqMagic        = 0x62526571 // "bReq"
	hdrLen          = 16
	stopGraceSec    = 15
	histBuckets     = 1 << 20 // 1us buckets up to ~1.05s
)

var target = "sidecar:9091"

func nowSec() float64 { return float64(time.Now().UnixNano()) / 1e9 }

// ------------------------------------------------------------ latency histogram
type hist struct {
	b        []uint32
	overflow int64
	count    int64
	sum      float64
	min, max float64
}

func newHist() *hist { return &hist{b: make([]uint32, histBuckets), min: 1e30} }
func (h *hist) record(us float64) {
	if us < 0 {
		us = 0
	}
	if us >= float64(histBuckets) {
		h.overflow++
	} else {
		h.b[int(us)]++
	}
	h.count++
	h.sum += us
	if us < h.min {
		h.min = us
	}
	if us > h.max {
		h.max = us
	}
}
func (h *hist) merge(o *hist) {
	for i := range h.b {
		h.b[i] += o.b[i]
	}
	h.overflow += o.overflow
	h.count += o.count
	h.sum += o.sum
	if o.count > 0 {
		if o.min < h.min {
			h.min = o.min
		}
		if o.max > h.max {
			h.max = o.max
		}
	}
}
func (h *hist) pct(p float64) float64 {
	if h.count == 0 {
		return 0
	}
	rank := int64(p / 100.0 * float64(h.count-1))
	var cum int64
	for i, c := range h.b {
		cum += int64(c)
		if cum > rank {
			return float64(i)
		}
	}
	return h.max
}
func (h *hist) avg() float64 {
	if h.count == 0 {
		return 0
	}
	return h.sum / float64(h.count)
}

// ------------------------------------------------------------ one connection
type connResult struct {
	rcnt, fail int64
	dura       float64
	h          *hist
	broken     bool
}

// One client thread: a single TCP conn kept at `W` outstanding requests via a
// writer goroutine (fed by a credit channel) and this goroutine as the reader.
func runConn(reqSize, replySize, W int, warmup int64, duration, startAt float64, res *connResult) {
	res.h = newHist()
	conn, err := net.DialTimeout("tcp", target, 5*time.Second)
	if err != nil {
		res.broken = true
		return
	}
	defer conn.Close()
	if tc, ok := conn.(*net.TCPConn); ok {
		_ = tc.SetNoDelay(true)
	}

	payload := make([]byte, reqSize)
	for i := range payload {
		payload[i] = 42
	}
	starts := make([]float64, W)
	credits := make(chan struct{}, W)
	for i := 0; i < W; i++ {
		credits <- struct{}{}
	}
	stopCh := make(chan struct{})

	for nowSec() < startAt { // barrier: all threads start together
		time.Sleep(50 * time.Microsecond)
	}
	startTime := nowSec()
	// Backstop so a blocked read/write unwinds if the peer stalls.
	_ = conn.SetDeadline(time.Now().Add(time.Duration((duration + stopGraceSec) * float64(time.Second))))

	var wseq uint32
	var wg sync.WaitGroup
	wg.Add(1)
	go func() { // writer
		defer wg.Done()
		w := bufio.NewWriterSize(conn, 64<<10)
		hdr := make([]byte, hdrLen)
		for {
			select {
			case <-stopCh:
				return
			case <-credits:
			}
			select { // stop may have fired while waiting
			case <-stopCh:
				return
			default:
			}
			binary.LittleEndian.PutUint32(hdr[0:4], reqMagic)
			binary.LittleEndian.PutUint32(hdr[4:8], wseq)
			binary.LittleEndian.PutUint32(hdr[8:12], uint32(reqSize))
			binary.LittleEndian.PutUint32(hdr[12:16], uint32(replySize))
			starts[wseq%uint32(W)] = nowSec()
			if _, err := w.Write(hdr); err != nil {
				return
			}
			if reqSize > 0 {
				if _, err := w.Write(payload); err != nil {
					return
				}
			}
			if err := w.Flush(); err != nil {
				return
			}
			wseq++
		}
	}()

	r := bufio.NewReaderSize(conn, 64<<10)
	hdr := make([]byte, hdrLen)
	var expSeq uint32
	var rcnt int64
	warmupEnd := startTime
	for {
		if _, err := io.ReadFull(r, hdr); err != nil {
			break
		}
		seq := binary.LittleEndian.Uint32(hdr[4:8])
		plen := binary.LittleEndian.Uint32(hdr[8:12])
		if plen > 0 {
			if _, err := io.CopyN(io.Discard, r, int64(plen)); err != nil {
				break
			}
		}
		now := nowSec()
		if seq != expSeq {
			res.fail++
		}
		t0 := starts[expSeq%uint32(W)]
		expSeq++
		if rcnt >= warmup {
			res.h.record((now - t0) * 1e6)
		}
		rcnt++
		if rcnt == warmup {
			warmupEnd = now
		}
		if now-startTime > duration {
			break
		}
		select { // let the writer send one more (non-blocking: never drops in steady state)
		case credits <- struct{}{}:
		default:
		}
	}
	close(stopCh)
	_ = conn.Close() // unblock a writer parked in Write
	wg.Wait()

	res.rcnt = rcnt
	if rcnt > warmup {
		res.dura = nowSec() - warmupEnd
	}
}

// ------------------------------------------------------------ one benchmark run
func runBench(out net.Conn, reqSize, replySize, concurrency int, duration float64,
	warmup int64, threads int) {
	if reqSize < 0 || replySize < 1 || concurrency < 1 || duration <= 0 || threads < 1 {
		fmt.Fprintln(out, "ERR invalid args")
		return
	}
	fmt.Fprintf(os.Stderr, "[bench-tcp] RUN req=%d reply=%d conc=%d dur=%.1fs warmup=%d threads=%d target=%s\n",
		reqSize, replySize, concurrency, duration, warmup, threads, target)

	results := make([]connResult, threads)
	var wg sync.WaitGroup
	startAt := nowSec() + 0.1
	for i := 0; i < threads; i++ {
		wg.Add(1)
		go func(idx int) {
			defer wg.Done()
			runConn(reqSize, replySize, concurrency, warmup, duration, startAt, &results[idx])
		}(i)
	}
	wg.Wait()

	agg := newHist()
	var mrps, gbps float64
	var totalOk, totalFail int64
	for i := range results {
		r := &results[i]
		measured := r.rcnt - warmup
		if measured < 0 {
			measured = 0
		}
		totalFail += r.fail
		if !r.broken && r.dura > 1e-9 && measured > 0 {
			mrps += float64(measured) / r.dura * 1e-6
			gbps += 8e-9 * float64(measured) * float64(reqSize) / r.dura
			totalOk += measured
			agg.merge(r.h)
		}
	}
	p50, p95, p99 := agg.pct(50), agg.pct(95), agg.pct(99)

	reply := fmt.Sprintf("OK mrps=%.6f gbps=%.4f p50=%.2f p95=%.2f p99=%.2f avg=%.2f min=%.2f max=%.2f "+
		"rcnt=%d fail=%d conc=%d threads=%d reqsz=%d repsz=%d durs=%.3f\n",
		mrps, gbps, p50, p95, p99, agg.avg(), agg.min, agg.max,
		totalOk, totalFail, concurrency, threads, reqSize, replySize, duration)
	fmt.Fprint(out, reply)
	fmt.Fprintf(os.Stderr, "[bench-tcp] DONE %s", reply)
}

// ------------------------------------------------------------ control TCP
func atoiDef(s string, d int) int {
	if v, err := strconv.Atoi(s); err == nil {
		return v
	}
	return d
}
func atofDef(s string, d float64) float64 {
	if v, err := strconv.ParseFloat(s, 64); err == nil {
		return v
	}
	return d
}

func handleCtrl(c net.Conn) {
	defer c.Close()
	line, err := bufio.NewReader(c).ReadString('\n')
	if err != nil && line == "" {
		return
	}
	f := strings.Fields(strings.TrimSpace(line))
	if len(f) == 0 {
		return
	}
	switch f[0] {
	case "PING":
		fmt.Fprintln(c, "PONG")
	case "RUN":
		// RUN <req> <reply> <conc> <dur> <warmup> <threads>
		req, rep, conc, threads := 32, 8, 1, 1
		dur := 10.0
		var warm int64 = 1000
		if len(f) > 1 {
			req = atoiDef(f[1], req)
		}
		if len(f) > 2 {
			rep = atoiDef(f[2], rep)
		}
		if len(f) > 3 {
			conc = atoiDef(f[3], conc)
		}
		if len(f) > 4 {
			dur = atofDef(f[4], dur)
		}
		if len(f) > 5 {
			warm = int64(atoiDef(f[5], 1000))
		}
		if len(f) > 6 {
			threads = atoiDef(f[6], threads)
		}
		runBench(c, req, rep, conc, dur, warm, threads)
	default:
		fmt.Fprintln(c, "ERR use: RUN <req> <reply> <conc> <dur> <warmup> <threads> | PING")
	}
}

func main() {
	port := flag.Int("ctrl-port", ctrlPortDefault, "control TCP port")
	flag.Parse()
	if t := os.Getenv("BENCH_TARGET"); t != "" {
		target = t
	}
	fmt.Fprintf(os.Stderr, "[bench-tcp] target=%s\n", target)

	ln, err := net.Listen("tcp", fmt.Sprintf(":%d", *port))
	if err != nil {
		fmt.Fprintln(os.Stderr, "listen:", err)
		os.Exit(1)
	}
	fmt.Fprintf(os.Stderr, "[bench-tcp] control LISTEN on :%d\n", *port)
	for {
		c, err := ln.Accept()
		if err != nil {
			continue
		}
		handleCtrl(c)
	}
}
