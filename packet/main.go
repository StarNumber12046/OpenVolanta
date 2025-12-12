package main

import (
	"fmt"
	"log/slog"
	"net"
	"os"

	slogmulti "github.com/samber/slog-multi"
)

func handleClient(conn net.Conn, logger *slog.Logger) {
	defer conn.Close()

	buf := make([]byte, 1024)
	n, err := conn.Read(buf)
	if err != nil {
		logger.Error("Error reading from client", "err", err)
		return
	}

	message := string(buf[:n])
	logger.Info(message)
}

func main() {
	file, err := os.OpenFile("volanta.log", os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
	if err != nil {
		panic(err)
	}
	defer file.Close()
	logger := slog.New(slogmulti.Fanout(
		slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{}),
		// log.txt file
		slog.NewTextHandler(file, &slog.HandlerOptions{}),
	))

	listener, err := net.Listen("tcp", "localhost:6746")
	if err != nil {
		fmt.Println("Error:", err)
		return
	}
	defer listener.Close()

	for {
		// Accept incoming connections
		conn, err := listener.Accept()
		if err != nil {
			fmt.Println("Error:", err)
			continue
		}

		// Handle client connection in a goroutine
		go handleClient(conn, logger)
	}
}
