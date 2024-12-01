#!/bin/bash
# © 2024 Alec Fessler
# MIT License
# See LICENSE file in the project root for full license information.

# rpi 1 = eth102, wifi79, ****5 ports
# rpi 2 = eth101, wifi78, ****6 ports
# rpi 3 = eth103, wifi104, ****7 ports

# Port configurations
TCP_PORTS=(12345 12346 12347)
UDP_PORTS=(22345 22346 22347)
# Network configurations
ETH_IPS=("192.168.1.102" "192.168.1.101" "192.168.1.103")
WIFI_IPS=("192.168.86.79" "192.168.86.78"  "192.168.86.104")

COUNTER_FILE=".video_counter.txt"
LOG_FILE="/var/log/ffmpeg_mgr.log"
VIDEO_BASE_PATH="/home/alecfessler/Documents/mimic/data"

log() {
    local level="$1"
    local message="$2"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [$level] $message" | tee -a "$LOG_FILE"
}

# Create counter file if it doesn't exist
if [ ! -f "$COUNTER_FILE" ]; then
    log "INFO" "Initializing counter file"
    echo "0" > "$COUNTER_FILE"
fi

next_counter() {
    counter=$(cat "$COUNTER_FILE")
    next_counter=$((counter + 1))
    echo "$next_counter" > "$COUNTER_FILE"
    echo "$next_counter"
}

write_config() {
    local port="$1"
    local counter="$2"
    local filename="port${port}_vid${counter}"
    {
        echo "VIDEO_BASE_PATH=${VIDEO_BASE_PATH}"
        echo "FILENAME=${VIDEO_BASE_PATH}/${filename}_%03d.mp4"
    } > /etc/default/ffmpeg-stream
}

send_udp_message() {
    local message="$1"
    local index="$2"
    local port="$3"

    # If this is a STOP message, send it as-is
    if [ "$message" = "STOP" ]; then
        printf "%s" "$message" | nc -u "${ETH_IPS[index]}" "$port"
    else
        # For timestamps, use perl to pack the number as a 64-bit big-endian integer
        perl -e "print pack('Q>', $message)" | nc -u "${ETH_IPS[index]}" "$port"
    fi

    if [ $? -eq 0 ]; then
        log "INFO" "Sent message via ethernet to ${ETH_IPS[index]} on port $port"
        return 0
    fi

    # Fall back to WiFi if ethernet fails, using the same message format
    if [ "$message" = "STOP" ]; then
        printf "%s" "$message" | nc -u "${WIFI_IPS[index]}" "$port"
    else
        perl -e "print pack('Q>', $message)" | nc -u "${WIFI_IPS[index]}" "$port"
    fi

    if [ $? -eq 0 ]; then
        log "INFO" "Sent message via WiFi to ${WIFI_IPS[index]} on port $port"
        return 0
    fi

    log "ERROR" "Failed to send message to camera $((index+1)) on both networks"
    return 1
}

send_timestamp() {
    timestamp=$(date +%s%N)  # Gets nanoseconds since epoch
    log "INFO" "Broadcasting start timestamp: $timestamp"
    for i in "${!TCP_PORTS[@]}"; do
        send_udp_message "$timestamp" "$i" "${UDP_PORTS[i]}"
    done
}

send_stop() {
    log "INFO" "Broadcasting stop signal"
    for i in "${!TCP_PORTS[@]}"; do
        send_udp_message "STOP" "$i" "${UDP_PORTS[i]}"
    done
}

case $1 in
    start)
        log "INFO" "Starting FFmpeg instances..."
        counter=$(next_counter)
        for port in "${TCP_PORTS[@]}"; do
            filename="port${port}_vid${counter}"
            write_config "$port" "$counter"
            if ! sudo systemctl start ffmpeg-stream@"$port".service; then
                log "ERROR" "Failed to start ffmpeg-stream@$port.service"
                continue
            fi
            log "INFO" "Started ffmpeg-stream@$port.service with filename $filename"
        done
        # Wait briefly for services to fully start
        sleep 1
        send_timestamp
        ;;

    stop)
        log "INFO" "Stopping recording..."
        send_stop
        # Wait for files to finalize using inotify
        counter=$(cat "$COUNTER_FILE")
        inotifywait -e close_write "${VIDEO_BASE_PATH}/port"*"_vid${counter}_"*".mp4"
        log "INFO" "Files finalized, stopping services..."
        for port in "${TCP_PORTS[@]}"; do
            if ! sudo systemctl stop ffmpeg-stream@"$port".service; then
                log "ERROR" "Failed to stop ffmpeg-stream@$port.service"
                continue
            fi
            log "INFO" "Stopped ffmpeg-stream@$port.service"
        done
        ;;

    status)
        log "INFO" "Checking status of FFmpeg instances..."
        for port in "${TCP_PORTS[@]}"; do
            log "DEBUG" "Checking status of ffmpeg-stream@$port.service"
            sudo systemctl status ffmpeg-stream@"$port".service
        done
        ;;

    restart)
        log "INFO" "Restarting recording..."
        # Stop current recording
        send_stop
        # Wait for files to finalize
        counter=$(cat "$COUNTER_FILE")
        inotifywait -e close_write "${VIDEO_BASE_PATH}/port"*"_vid${counter}_"*".mp4"
        log "INFO" "Files finalized, restarting services..."
        # Get new counter for new files
        counter=$(next_counter)
        for port in "${TCP_PORTS[@]}"; do
            filename="port${port}_vid${counter}"
            write_config "$port" "$counter"
            if ! sudo systemctl restart ffmpeg-stream@"$port".service; then
                log "ERROR" "Failed to restart ffmpeg-stream@$port.service"
                continue
            fi
            log "INFO" "Restarted ffmpeg-stream@$port.service with filename $filename"
        done
        # Wait briefly for services to fully start
        sleep 1
        send_timestamp
        ;;

    *)
        log "ERROR" "Invalid command: $1"
        echo "Usage: $0 {start|stop|status|restart}"
        exit 1
        ;;
esac
