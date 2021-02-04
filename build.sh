#!/bin/bash

echo "***** Go Format *****"
echo "Runing Go Format on the following files:"
go fmt github.com/intel/cndp_device_plugin/...
echo 

echo "***** Go Lint *****"
#install golint if needed, suppress output to keep output clean
go get -u golang.org/x/lint/golint &> /dev/null
#where was golint installed?
golint=$(go list -f {{.Target}} golang.org/x/lint/golint)
#run golint
echo "The following files have linting issues:"
eval "$golint ./..."
echo

echo "***** Unit Tests *****"
echo "Running unit tests:"
go test github.com/intel/cndp_device_plugin/...
echo

echo "***** Build *****"
echo "Building Device Plugin"
go build -o ./bin/cndp-dp ./cmd/cndp-dp
echo "Building CNI"
go build -o ./bin/cndp ./cmd/cndp-cni
echo

echo "Build complete!"