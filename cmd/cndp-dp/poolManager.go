/*
 Copyright(c) 2021 Intel Corporation.
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
*/

package main

import (
	"fmt"
	"github.com/intel/cndp_device_plugin/pkg/bpf"
	"github.com/intel/cndp_device_plugin/pkg/cndp"
	"github.com/intel/cndp_device_plugin/pkg/logging"
	"golang.org/x/net/context"
	"google.golang.org/grpc"
	pluginapi "k8s.io/kubelet/pkg/apis/deviceplugin/v1beta1"
	"net"
	"os"
	"strconv"
	"time"
)

/*
PoolManager represents an manages the pool of devices.
Each PoolManager registers with Kubernetes as a different device type.
*/
type PoolManager struct {
	Name          string
	Devices       map[string]*pluginapi.Device
	UpdateSignal  chan bool
	DpAPISocket   string
	DpAPIEndpoint string
	DpAPIServer   *grpc.Server
	ServerFactory cndp.ServerFactory
}

/*
Init is called it initialise the PoolManager.
*/
func (pm *PoolManager) Init(config poolConfig) error {
	pm.ServerFactory = cndp.NewServerFactory()

	err := pm.registerWithKubelet()
	if err != nil {
		return err
	}
	logging.Infof(devicePrefix + "/" + pm.Name + " registered with Kubelet")

	err = pm.startGRPC()
	if err != nil {
		return err
	}
	logging.Infof(devicePrefix + "/" + pm.Name + " serving on " + pm.DpAPISocket)

	err = pm.discoverResources(config)
	if err != nil {
		return err
	}

	return nil
}

/*
Terminate is called it terminate the PoolManager.
*/
func (pm *PoolManager) Terminate() error {
	pm.stopGRPC()
	pm.cleanup()
	logging.Infof(devicePrefix + "/" + pm.Name + " terminated")

	return nil
}

/*
ListAndWatch is part of the device plugin API.
Returns a stream list of Devices. Whenever a device state changes,
ListAndWatch should return the new list.
*/
func (pm *PoolManager) ListAndWatch(emtpy *pluginapi.Empty,
	stream pluginapi.DevicePlugin_ListAndWatchServer) error {

	logging.Infof(devicePrefix + "/" + pm.Name + " ListAndWatch started")

	for {
		select {
		case <-pm.UpdateSignal:
			resp := new(pluginapi.ListAndWatchResponse)
			logging.Infof(devicePrefix + "/" + pm.Name + " device list:")

			for _, dev := range pm.Devices {
				logging.Infof("\t" + dev.ID + ", " + dev.Health)
				resp.Devices = append(resp.Devices, dev)
			}

			if err := stream.Send(resp); err != nil {
				logging.Errorf("Failed to send stream to kubelet: %v", err)
			}
		}
	}
}

/*
Allocate is part of the device plugin API.
Called during container creation so that the Device Plugin can run
device specific operations and instruct Kubelet of the steps to make
the Device available in the container.
*/
func (pm *PoolManager) Allocate(ctx context.Context,
	rqt *pluginapi.AllocateRequest) (*pluginapi.AllocateResponse, error) {
	response := pluginapi.AllocateResponse{}

	logging.Infof("New allocate request. Creating new UDS server.")
	cndpServer, udsPath := pm.ServerFactory.CreateServer(devicePrefix + "/" + pm.Name)
	logging.Infof("UDS socket path: " + udsPath)

	//loop each container
	for _, crqt := range rqt.ContainerRequests {
		cresp := new(pluginapi.ContainerAllocateResponse)

		cresp.Mounts = append(cresp.Mounts, &pluginapi.Mount{
			HostPath:      udsPath,
			ContainerPath: "/tmp/cndp.sock",
			ReadOnly:      false,
		})

		//loop each device request per container
		for _, dev := range crqt.DevicesIDs {
			logging.Infof("Loading BPF program on device: " + dev)
			fd := bpf.LoadBpfSendXskMap(dev) //TODO Load BPF should return an error
			logging.Infof("BPF program loaded on: " + dev + " File descriptor: " + strconv.Itoa(fd))
			cndpServer.AddDevice(dev, fd)
		}
		response.ContainerResponses = append(response.ContainerResponses, cresp)
	}

	cndpServer.Start()

	return &response, nil
}

/*
GetDevicePluginOptions is part of the device plugin API.
Unused.
*/
func (pm *PoolManager) GetDevicePluginOptions(context.Context, *pluginapi.Empty) (*pluginapi.DevicePluginOptions, error) {
	return &pluginapi.DevicePluginOptions{}, nil
}

/*
PreStartContainer is part of the device plugin API.
Unused.
*/
func (pm *PoolManager) PreStartContainer(context.Context, *pluginapi.PreStartContainerRequest) (*pluginapi.PreStartContainerResponse, error) {
	return &pluginapi.PreStartContainerResponse{}, nil
}

/*
GetPreferredAllocation is part of the device plugin API.
Unused.
*/
func (pm *PoolManager) GetPreferredAllocation(context.Context, *pluginapi.PreferredAllocationRequest) (*pluginapi.PreferredAllocationResponse, error) {
	return &pluginapi.PreferredAllocationResponse{}, nil
}

func (pm *PoolManager) discoverResources(config poolConfig) error {

	for _, device := range config.Devices {
		newdev := pluginapi.Device{ID: device, Health: pluginapi.Healthy}
		pm.Devices[device] = &newdev
	}

	if len(pm.Devices) > 0 {
		pm.UpdateSignal <- true
	}

	return nil
}

func (pm *PoolManager) registerWithKubelet() error {
	conn, err := grpc.Dial(pluginapi.KubeletSocket, grpc.WithInsecure(),
		grpc.WithDialer(func(addr string, timeout time.Duration) (net.Conn, error) {
			return net.DialTimeout("unix", addr, timeout)
		}))
	defer conn.Close()
	if err != nil {
		return fmt.Errorf("Error registering with Kubelet: %v", err)
	}
	client := pluginapi.NewRegistrationClient(conn)

	reqt := &pluginapi.RegisterRequest{
		Version:      pluginapi.Version,
		Endpoint:     pm.DpAPIEndpoint,
		ResourceName: devicePrefix + "/" + pm.Name,
	}

	_, err = client.Register(context.Background(), reqt)
	if err != nil {
		return fmt.Errorf("Error registering with Kubelet: %v", err)
	}

	return nil
}

func (pm *PoolManager) startGRPC() error {
	err := pm.cleanup()
	if err != nil {
		return err
	}

	sock, err := net.Listen("unix", pm.DpAPISocket)
	if err != nil {
		return err
	}

	pm.DpAPIServer = grpc.NewServer([]grpc.ServerOption{}...)
	pluginapi.RegisterDevicePluginServer(pm.DpAPIServer, pm)
	go pm.DpAPIServer.Serve(sock)

	conn, err := grpc.Dial(pm.DpAPISocket, grpc.WithInsecure(), grpc.WithBlock(),
		grpc.WithTimeout(5*time.Second),
		grpc.WithDialer(func(addr string, timeout time.Duration) (net.Conn, error) {
			return net.DialTimeout("unix", addr, timeout)
		}),
	)
	conn.Close()

	if err != nil {
		return err
	}

	return nil
}

func (pm *PoolManager) stopGRPC() {
	if pm.DpAPIServer != nil {
		pm.DpAPIServer.Stop()
		pm.DpAPIServer = nil
	}
}

func (pm *PoolManager) cleanup() error {
	if err := os.Remove(pm.DpAPISocket); err != nil && !os.IsNotExist(err) {
		return err
	}
	return nil
}
