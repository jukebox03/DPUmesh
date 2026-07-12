// echo_tcp.go — Greeter SERVER over TCP (echo-tcp pod, TCP baseline).
//
// The TCP baseline mirrors the DPUmesh greeter (bench/echo_sock.c) so both
// transports are measured with the identical methodology. It is a Greeter, not a
// raw echo: it answers each framed SayHello request with a fixed reply_size-byte
// reply (chosen by the client, default 8), reproducing the asymmetric
// big-request/tiny-reply RPC.
//
// Wire frame (matches bench/bench.h, little-endian):
//   [u32 magic | u32 seq | u32 payload_len | u32 aux] + payload_len bytes
// request aux = reply_size; the server echoes seq and replies reply_size bytes.
//
// In the deployment this server sits behind its Envoy sidecar (sidecar2), so the
// path is client -> sidecar1 -> sidecar2 -> here, i.e. the realistic service-mesh
// sidecar tax that the DPUmesh side offloads to the DPU.
//
// Listens on :9092 internally by default (sidecar2 forwards 9091 -> 127.0.0.1:9092).
package main

import (
	"bufio"
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
)

const (
	repMagic = 0x62526570 // "bRep"
	hdrLen   = 16
)

func handleConn(c net.Conn) {
	defer c.Close()
	if tc, ok := c.(*net.TCPConn); ok {
		_ = tc.SetNoDelay(true)
	}
	r := bufio.NewReaderSize(c, 64<<10)
	w := bufio.NewWriterSize(c, 64<<10)

	hdr := make([]byte, hdrLen)
	fill := make([]byte, 8192)
	for i := range fill {
		fill[i] = 43
	}

	for {
		// Read a whole request header, then consume its payload (reassembly:
		// a large request may span many TCP segments).
		if _, err := io.ReadFull(r, hdr); err != nil {
			return
		}
		seq := binary.LittleEndian.Uint32(hdr[4:8])
		reqLen := binary.LittleEndian.Uint32(hdr[8:12])
		replySize := binary.LittleEndian.Uint32(hdr[12:16])
		if reqLen > 0 {
			if _, err := io.CopyN(io.Discard, r, int64(reqLen)); err != nil {
				return
			}
		}

		// Reply frame: [magic|seq|reply_size|0] + reply_size bytes.
		binary.LittleEndian.PutUint32(hdr[0:4], repMagic)
		binary.LittleEndian.PutUint32(hdr[4:8], seq)
		binary.LittleEndian.PutUint32(hdr[8:12], replySize)
		binary.LittleEndian.PutUint32(hdr[12:16], 0)
		if _, err := w.Write(hdr); err != nil {
			return
		}
		for left := replySize; left > 0; {
			n := left
			if n > uint32(len(fill)) {
				n = uint32(len(fill))
			}
			if _, err := w.Write(fill[:n]); err != nil {
				return
			}
			left -= n
		}
		if err := w.Flush(); err != nil {
			return
		}
	}
}

func main() {
	port := flag.Int("port", 9092, "greeter TCP port")
	flag.Parse()

	ln, err := net.Listen("tcp", fmt.Sprintf(":%d", *port))
	if err != nil {
		fmt.Fprintln(os.Stderr, "listen:", err)
		os.Exit(1)
	}
	fmt.Fprintf(os.Stderr, "[greeter-tcp] SayHello LISTEN on :%d\n", *port)
	for {
		c, err := ln.Accept()
		if err != nil {
			continue
		}
		go handleConn(c)
	}
}
