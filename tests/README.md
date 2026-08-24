# TFTP Test Suite

This directory contains test scripts for the TFTP client and server implementation.

## test-tftp.sh

A comprehensive shell script that tests bidirectional file transfer between a TFTP client and an ephemeral TFTP server.

### Features

- Starts an ephemeral TFTP server on localhost with a nonstandard port (default: 6969)
- Creates multiple test files of varying sizes and types:
  - Small text file
  - Medium binary file (10 KB random data)
  - Multiline text file
  - Large ASCII file (100 lines of numbers)
- Tests **download** operations (client receives files from server)
- Tests **upload** operations (client sends files to server)
- Verifies data integrity by comparing original and transferred files
- Automatically terminates the server after tests complete
- Cleans up all temporary files

### Requirements

- `tftpd` - TFTP server daemon (system or built from this repository)
- `tftp` - TFTP client (system or built from this repository)
- Standard POSIX utilities: `bash`, `diff`, `dd`, `mktemp`, `seq`

### Usage

#### Using binaries from the current tree (default)
```bash
./test-tftp.sh
```

#### Using custom tftpd and tftp binaries
```bash
./test-tftp.sh $objtree/tftpd/tftpd $objtree/tftp/tftp
```

#### Using custom port (default: 6969)
```bash
./test-tftp.sh '' '' 6970
```

### Test Operations

The script performs the following sequence:

1. **Initialization**
   - Verifies binaries exist and are executable
   - Creates temporary directories for test files and server storage
   - Starts tftpd on localhost:PORT

2. **Download Tests** (Client → Receive)
   - `small.txt` - Basic text file transfer
   - `multiline.txt` - Multi-line text file
   - `numbers.txt` - Sequential numbered lines
   - `medium.bin` - Binary data transfer (10 KB)

3. **Upload Tests** (Client → Send)
   - Same files as download tests
   - Server receives and stores files
   - Integrity verified by diff

4. **Cleanup**
   - Terminates tftpd server
   - Removes temporary files and directories

### Output

The script provides colored output for easy reading:
- **[INFO]** - Information messages (yellow)
- **[PASS]** - Successful test results (green)
- **[FAIL]** - Test failures (red)

Example:
```
[INFO] ================================
[INFO] TFTP Client-Server Test Suite
[INFO] ================================
[PASS] Found tftpd at: /usr/sbin/tftpd
[PASS] Found tftp at: /usr/bin/tftp
[INFO] Starting tftpd on 127.0.0.1:6969...
[PASS] tftpd started with PID 12345
[INFO] Running download tests...
[PASS] Download test passed: small.txt
[PASS] Download test passed: multiline.txt
[PASS] All tests passed!
```

### Exit Codes

- `0` - All tests passed
- `1` - One or more tests failed, or setup error

### Notes

- The test server runs with limited privileges (user: nobody)
- Uses ephemeral ports and temporary directories—safe to run multiple instances
- No files are permanently created; all cleanup occurs automatically
- TFTP uses UDP (port 69 by default, or specified PORT)—ensure no firewall blocks the port

### Troubleshooting

#### "tftpd not found"
Build the tftpd from source:
```bash
cd ../
make
./tests/test-tftp.sh ./tftpd/tftpd ./tftp/tftp
```

Or specify the system tftpd:
```bash
./test-tftp.sh /usr/sbin/tftpd /usr/bin/tftp
```

#### Port already in use
Specify a different port:
```bash
./test-tftp.sh /usr/sbin/tftpd /usr/bin/tftp 6971
```

#### Permission denied errors
The script requires:
- Write access to create temporary directories
- Execute permission on tftp and tftpd binaries
- Ability to bind to localhost

#### Transfer timeouts
- Verify firewall doesn't block the UDP port
- Check that the server is actually running: `ps aux | grep tftpd`
- Try with a different port number

## Contributing

To add more tests:

1. Create test files in `create_test_files()` function
2. Add corresponding test functions (e.g., `test_download "myfile"`)
3. Call test functions from `main()`
4. Track pass/fail counts with `((tests_passed++))` / `((tests_failed++))`
