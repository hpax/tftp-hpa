#!/bin/bash
#
# TFTP Client-Server Test Script
# Tests bidirectional file transfer with an ephemeral TFTP server
#
# Usage: ./test-tftp.sh [tftpd_path] [tftp_path] [port] [portrange]
#

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
TFTPD="${1:-$REPO_ROOT/tftpd/tftpd}"
TFTP="${2:-${REPO_ROOT}/tftp/tftp}"
PORT="${3:-6969}"
PORTRANGE="${4:-60969:60999}"
LOCALHOSTS="${LOCALHOSTS:-127.0.0.1 ::1}"
TESTROOT=$(mktemp -d)
TEST_DIR="$TESTROOT/client"
SERVER_DIR="$TESTROOT/server"
PCAP_LOG="$SCRIPT_DIR/test-tftp.pcap.gz"
mkdir -p "$TEST_DIR" "$SERVER_DIR"

trap 'cleanup' EXIT INT TERM

# Color output helpers
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_info() {
    echo -e "${YELLOW}[INFO]${NC} $*"
}

print_success() {
    echo -e "${GREEN}[PASS]${NC} $*"
}

print_error() {
    echo -e "${RED}[FAIL]${NC} $*"
}

# Cleanup function
cleanup() {
    print_info "Cleaning up..."

    # Kill tftpd if still running
    if [ -n "$TFTPD_PID" ] && kill -0 "$TFTPD_PID" 2>/dev/null; then
        print_info "Terminating tftpd (PID $TFTPD_PID)..."
        kill "$TFTPD_PID" 2>/dev/null
        sleep 1
        kill -9 "$TFTPD_PID" 2>/dev/null
    fi

    # Kill tshark if still running
    if [ -n "$TSHARK_PID" ] && kill -0 "$TSHARK_PID" 2>/dev/null; then
        print_info "Terminating tshark (PID $TSHARK_PID)..."
        kill "$TSHARK_PID" 2>/dev/null
        sleep 1
        kill -9 "$TSHARK_PID" 2>/dev/null
    fi

    # Remove temporary directories
    rm -rf "$TESTROOT"
}

# Check if binaries exist and are executable
check_binaries() {
    print_info "Checking for TFTP binaries..."

    if [ ! -x "$TFTPD" ]; then
        print_error "tftpd not found or not executable at: $TFTPD"
        echo "Usage: $0 [tftpd_path] [tftp_path]"
        echo "Example: $0 ./tftpd/tftpd /usr/bin/tftp"
        exit 1
    fi

    if [ ! -x "$TFTP" ]; then
        print_error "tftp not found or not executable at: $TFTP"
        exit 1
    fi

    print_success "Found tftpd at: $TFTPD"
    print_success "Found tftp at: $TFTP"
}

# Start the TFTP server
start_server() {
    local addrs=$(echo "$LOCALHOSTS" | \
		      sed -E -e 's/([^[:space:]]*:[^[:space:]]*)/[\1]/g' \
			  -e "s/([^[:space:]]+)/-a \\1:$PORT/g")

    print_info "Starting tftpd..."

    # Start tftpd in the background, listening on localhost
    # Run in standalone mode, serve from SERVER_DIR
    local -a TFTPD_CMD=("$TFTPD" --stderr -L -p --port-range $PORTRANGE
				 -c $addrs "$SERVER_DIR")
    print_info "${TFTPD_CMD[*]}"
    "${TFTPD_CMD[@]}" &
    TFTPD_PID=$!

    # Wait for server to start
    sleep 1

    # Check if server started successfully
    if ! kill -0 "$TFTPD_PID" 2>/dev/null; then
        print_error "tftpd failed to start"
        return 1
    fi

    print_success "tftpd started with PID $TFTPD_PID"

    me=$(whoami)

    # If the user has access to tshark, dump a packet trace
    rm -f "$PCAP_LOG"
    local dumphosts=$(echo "$LOCALHOSTS" | \
			  sed -E -e 's/([^[:space:]]*:[^[:space:]]*)/[\1]/g' \
			  -e "s/([^[:space:]]+)/or host \\1/g" \
			  -e 's/^or //')
	   tshark -q -Q -i lo -n -w "$PCAP_LOG" \
	   -f "udp port $PORT or portrange ${PORTRANGE/:/-}" \
	   1>&2 2>/dev/null &
    TSHARK_PID=$!
    if kill -0 "$TSHARK_PID" 2>/dev/null; then
	print_info "dumping packets to: $PCAP_LOG"
    else
	unset TSHARK_PID
    fi
}

declare -a testfiles

# Create test files
create_test_files() {
    print_info "Creating test files..."

    # Small text file
    echo "This is a small test file for TFTP." > "$TEST_DIR/small.txt"

    # Medium binary-like file
    dd if=/dev/urandom of="$TEST_DIR/medium.bin" bs=1024 count=10 2>/dev/null

    # Exactly one four-block window; exercises the terminating empty block
    dd if=/dev/urandom of="$TEST_DIR/window-boundary.bin" bs=512 count=4 2>/dev/null

    # Text file with multiple lines
    printf "Line 1\nLine 2\nLine 3\nLine 4\nLine 5\n" > "$TEST_DIR/multiline.txt"

    # Sparse ASCII file
    seq 1 100 > "$TEST_DIR/numbers.txt"

    # Large file (> 65536 blocks)
    dd if=/dev/urandom of="$TEST_DIR/large.bin" bs=512 count=67890 2>/dev/null

    print_success "Created test files in $TEST_DIR"
}

# Test file download (client receives)
test_download() {
    local filename="$1"
    local -a tftp_options=(-w $WINSIZE "${@:2}")

    print_info "Testing download: $filename"

    # Copy file to server directory
    local server_file="$SERVER_DIR/$filename"
    cp "$TEST_DIR/$filename" "$server_file"

    # Download using tftp
    local download_file="$TEST_DIR/${filename}.downloaded"

    # Use non-interactive tftp with get command
    local -a TFTP_CMD=("$TFTP" "${tftp_options[@]}" "$LOCALHOST" "$PORT"
		       -c get "$server_file" "$download_file")
    print_info "${TFTP_CMD[*]}"
    ${TFTP_CMD[@]} 2>&1 | grep -v "^Connected"

    # Verify file was downloaded
    if [ ! -f "$download_file" ]; then
        print_error "Failed to download $filename"
        return 1
    fi

    # Compare files
    if ! diff -q "$TEST_DIR/$filename" "$download_file" > /dev/null 2>&1; then
        print_error "Downloaded file differs from original: $filename"
        return 1
    fi

    print_success "Download test passed: $filename"
}

# Test file upload (client sends)
test_upload() {
    local filename="$1"
    local -a tftp_options=(-w $WINSIZE "${@:2}")

    print_info "Testing upload: $filename"

    local server_file="$SERVER_DIR/$filename"

    # Use tftp to upload the file
    # The server will write it to SERVER_DIR
    local -a TFTP_CMD=("$TFTP" "${tftp_options[@]}" "$LOCALHOST" "$PORT"
		       -c put "$TEST_DIR/$filename" "$server_file")
    print_info "${TFTP_CMD[*]}"
    "${TFTP_CMD[@]}" 2>&1 | grep -v "^Connected"

    # Verify file was uploaded
    if [ ! -f "$SERVER_DIR/$filename" ]; then
        print_error "Failed to upload $filename"
        return 1
    fi

    # Compare files
    if ! diff -q "$TEST_DIR/$filename" "$SERVER_DIR/$filename" > /dev/null 2>&1; then
        print_error "Uploaded file differs from original: $filename"
        return 1
    fi

    print_success "Upload test passed: $filename"
}

# Main test execution
main() {
    umask 077

    print_info "================================"
    print_info "TFTP Client-Server Test Suite"
    print_info "================================"
    print_info "Test directory:   $TEST_DIR"
    print_info "Server directory: $SERVER_DIR"
    print_info "Server addresses: $LOCALHOSTS"
    print_info "Server port:      $PORT"
    print_info ""

    local tests_passed=0
    local tests_failed=0

    check_binaries
    create_test_files
    start_server

    # Give server a moment to start
    sleep 2

    local -a testfiles=(small.txt multiline.txt numbers.txt medium.bin
			window-boundary.bin large.bin)

    # The largest supported client window exercises the threaded packet ring.
    for WINSIZE in ${TFTP_TEST_WINSIZES:-1 4 64}; do
	for LOCALHOST in $LOCALHOSTS; do
	    # Clear test directory of any previously downloaded files
	    rm -f "$TEST_DIR"/*.downloaded

	    print_info "Running download tests, window size $WINSIZE, address $LOCALHOST..."
	    for testfile in "${testfiles[@]}"; do
		if test_download $testfile; then
		    ((tests_passed++))
		else
		    ((tests_failed++))
		fi
	    done

	    # Clear server directory of downloaded test files
	    rm -f "$SERVER_DIR"/*

	    print_info "Running upload tests, window size $WINSIZE, address $LOCALHOST..."

	    for testfile in "${testfiles[@]}"; do
		if test_upload $testfile; then
		    ((tests_passed++))
		else
		    ((tests_failed++))
		fi
	    done
	done
    done

    print_info ""
    print_info "================================"
    print_info "Test Results Summary"
    print_info "================================"
    print_success "Tests passed: $tests_passed"

    if [ $tests_failed -gt 0 ]; then
        print_error "Tests failed: $tests_failed"
        return 1
    else
        print_success "All tests passed!"
        return 0
    fi
}

# Run main function
main
exit $?
