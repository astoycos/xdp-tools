package main

import (
	"github.com/intel/cndp_device_plugin/pkg/uds"
	"os"
	"strconv"
	"strings"
	"time"
)

const (
	udsProtocol    = "unixpacket"
	udsPath        = "/tmp/cndp.sock"
	udsMsgBufSize  = 64
	udsCtlBufSize  = 4
	udsIdleTimeout = 0 * time.Second
	requestDelay   = 100 * time.Millisecond // not required but keeps things in nice order when DP and this test app are both printing to screen
)

var udsHandler uds.Handler

func main() {
	//Get environment variable device values
	devicesVar, exists := os.LookupEnv("CNDP_DEVICES")
	if !exists {
		println("Test App Error: Devices env var does not exist")
		os.Exit(1)
	}
	devices := strings.Split(devicesVar, " ")

	hostname, exists := os.LookupEnv("HOSTNAME")
	if !exists {
		println("Test App Error: Hostname env var does not exist")
		os.Exit(1)
	}

	udsHandler = uds.NewHandler()

	// init
	if err := udsHandler.Init(udsPath, udsProtocol, udsMsgBufSize, udsCtlBufSize, udsIdleTimeout); err != nil {
		println("Test App Error: Error Initialising UDS server: ", err)
		os.Exit(1)
	}

	cleanup, err := udsHandler.Dial()
	if err != nil {
		println("Test App Error: UDS Dial error:: ", err)
		cleanup()
		os.Exit(1)
	}
	defer cleanup()

	// connect and verify pod hostname
	makeRequest("/connect, " + hostname)
	time.Sleep(requestDelay)

	// request version
	makeRequest("/version")
	time.Sleep(requestDelay)

	// request XSK map FD for all devices
	for _, dev := range devices {
		request := "/xsk_map_fd, " + dev
		makeRequest(request)
		time.Sleep(requestDelay)
	}

	// request an unknown device
	makeRequest("/xsk_map_fd, bad-device")
	time.Sleep(requestDelay)

	// send a bad request
	makeRequest("/bad-request")
	time.Sleep(requestDelay)

	// finish
	makeRequest("/fin")
	time.Sleep(requestDelay)
}

func makeRequest(request string) {
	println()
	println("Test App - Request: " + request)

	if err := udsHandler.Write(request, -1); err != nil {
		println("Test App - Write error: ", err)
	}

	response, fd, err := udsHandler.Read()
	if err != nil {
		println("Test App - Read error: ", err)
	}

	println("Test App - Response: " + response)
	if fd > 0 {
		println("Test App - File Descriptor:", strconv.Itoa(fd))
	}
	println()
}
