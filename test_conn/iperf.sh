#!/bin/bash

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo "Options:"
    echo "  -s , for server mode"
    echo "  -c SERVER_IP [SCHEDULER], for client mode, specify server IP and optionally scheduler"
    echo "  Available schedulers: default, minrtt, blest, xlayer"
}

mptcp_scheduler(){
    local scheduler="$1"
    if [ -z "$scheduler" ]; then
        echo "No scheduler specified, using default (default)"
        return 0
    fi

    if ! sysctl -a | grep -q "net.mptcp.scheduler = $scheduler"; then
        sysctl -w net.mptcp.scheduler="$scheduler"
    fi
}

iperf_client(){

    local IP_SERVER="$1"
    local MPTCP_SCHEDULER="${2:-default}"
    
    echo "Starting iperf3 client..."
    echo "Server IP: $IP_SERVER"
    echo "MPTCP Scheduler: $MPTCP_SCHEDULER"

    mptcp_scheduler "$MPTCP_SCHEDULER"

    # Create log and pcap files in the current directory
    echo "Logs and captures will be saved in: $PWD"
    touch "$PWD/iperf_client-$MPTCP_SCHEDULER.json"
    touch "$PWD/iperf_capture-$MPTCP_SCHEDULER.pcap"

    chmod 777 "$PWD/iperf_client-$MPTCP_SCHEDULER.json"
    chmod 777 "$PWD/iperf_capture-$MPTCP_SCHEDULER.pcap"

    # Start capturing packets with tshark
    tshark -i any -w "$PWD/iperf_capture-$MPTCP_SCHEDULER.pcap" &
    TSHARK_PID=$!

    sleep 2 # Give tshark a moment to start

    echo "tshark started with PID $TSHARK_PID, capturing packets..."

    mptcpize run iperf3 -c "$IP_SERVER" -t 10 --json > "$PWD/iperf_client-$MPTCP_SCHEDULER.json"

    # Stop tshark
    kill -9 $TSHARK_PID

}

iperf_server(){
    echo "Starting iperf3 server..."
    mptcpize run iperf3 -s
}


MODE="$1"
if [ "$MODE" != "-s" ] && [ "$MODE" != "-c" ]; then
    usage
    exit 1
fi  

if [ "$MODE" = "-s" ]; then
    # print the IP addresses of the machine
    echo "Machine IP addresses:"
    ip addr show | grep 'inet ' | awk '{print $2}'
    echo "Starting in server mode..."
    iperf_server
    exit 0
fi

if [ "$MODE" = "-c" ]; then
    if [ -z "$2" ]; then
        echo "Error: SERVER_IP is required in client mode."
        usage
        exit 1
    fi
    SERVER_IP="$2"
    SCHEDULER="$3"
    iperf_client "$SERVER_IP" "$SCHEDULER"
    exit 0
fi

exit 0

