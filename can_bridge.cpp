/*
 * CAN Bridge for Rexroth RCU4 / Owasys OWA5x
 * 
 * Simple CAN message router that reads from multiple CAN interfaces
 * and forwards messages between them.
 * 
 * Interfaces: canfd1, canfd2, canfd3 (using standard CAN, not CAN-FD)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <stdint.h>

// CAN ID for keypad messages
#define CAN_ID_KEYPAD 0x18FF0280

// CAN ID for J1939 TSC1 (Torque/Speed Control)
#define CAN_ID_TSC1 0x0C000003

// Button state tracking (for 8 buttons)
static bool buttonStates[8] = {false};
static bool buttonChanged[8] = {false};

// Global flag for clean shutdown
volatile sig_atomic_t running = 1;

void signal_handler(int signum) {
    running = 0;
}

// Decode keypad button data (J1939 format)
void decodeKeypadButtons(unsigned char* data) {
    // Combine first two bytes to get 16 bits for J1939 keypad format
    uint16_t buttonData = (data[1] << 8) | data[0];
    
    printf("  Keypad Buttons: ");
    
    // Check each button (2 bits each, starting from LSB)
    for (int i = 0; i < 8; i++) {
        uint8_t buttonBits = (buttonData >> (i * 2)) & 0x03;
        bool previousState = buttonStates[i];
        buttonStates[i] = (buttonBits == 0x01);
        buttonChanged[i] = (buttonStates[i] != previousState);
        
        // Print button state
        if (buttonStates[i]) {
            printf("[BTN%d:PRESSED]", i);
            if (buttonChanged[i]) {
                printf("*");  // Asterisk indicates state change
            }
            printf(" ");
        }
    }
    printf("\n");
}

// Decode J1939 TSC1 message (Torque/Speed Control)
void decodeTSC1(unsigned char* data) {
    // Byte 0: Override control modes
    uint8_t overrideCtrl = data[0];
    
    // Bytes 1-2: Requested speed/speed limit (little-endian, 0.125 rpm/bit)
    uint16_t rawSpeed = (data[2] << 8) | data[1];
    float requestedSpeed = rawSpeed * 0.125;  // RPM
    
    // Byte 3: Requested torque/torque limit (1% per bit, offset -125%)
    uint8_t rawTorque = data[3];
    int16_t requestedTorque = rawTorque - 125;  // Percent
    
    // Byte 4: Override control mode priority
    uint8_t priority = data[4] & 0x03;
    
    printf("  TSC1: Speed=%.1f RPM, Torque=%d%%, Priority=%d, CtrlMode=0x%02X\n", 
           requestedSpeed, requestedTorque, priority, overrideCtrl);
}

// Restart and configure a CAN interface
int restart_can_interface(const char* interface_name, int bitrate) {
    char command[256];
    int ret;
    
    printf("Configuring %s...\n", interface_name);
    
    // Bring interface down
    snprintf(command, sizeof(command), "ip link set %s down 2>/dev/null", interface_name);
    system(command);
    
    // Configure bitrate
    snprintf(command, sizeof(command), "ip link set %s type can bitrate %d", interface_name, bitrate);
    ret = system(command);
    if (ret != 0) {
        fprintf(stderr, "Warning: Failed to configure %s bitrate\n", interface_name);
    }
    
    // Bring interface up
    snprintf(command, sizeof(command), "ip link set %s up", interface_name);
    ret = system(command);
    if (ret != 0) {
        fprintf(stderr, "Error: Failed to bring up %s\n", interface_name);
        return -1;
    }
    
    printf("  %s configured at %d bps\n", interface_name, bitrate);
    return 0;
}

// Create and bind a CAN socket to the specified interface
int setup_can_socket(const char* interface_name) {
    int sock;
    struct sockaddr_can addr;
    struct ifreq ifr;
    
    // Create socket
    sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (sock < 0) {
        perror("Error creating socket");
        return -1;
    }
    
    // Get interface index
    strcpy(ifr.ifr_name, interface_name);
    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        perror("Error getting interface index");
        close(sock);
        return -1;
    }
    
    // Bind socket to CAN interface
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("Error binding socket");
        close(sock);
        return -1;
    }
    
    printf("Initialized CAN interface: %s\n", interface_name);
    return sock;
}

// Read and print CAN frame from socket
int read_and_print_frame(int src_sock, const char* src_name) {
    struct can_frame frame;
    int nbytes;
    
    // Read frame from source
    nbytes = read(src_sock, &frame, sizeof(struct can_frame));
    if (nbytes < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("Error reading from CAN");
        }
        return -1;
    }
    
    if (nbytes < (int)sizeof(struct can_frame)) {
        fprintf(stderr, "Incomplete CAN frame received\n");
        return -1;
    }
    
    // Print received CAN message
    printf("[RX %s] ID=0x%08X DLC=%d Data: ", src_name, frame.can_id & CAN_EFF_MASK, frame.can_dlc);
    for (int i = 0; i < frame.can_dlc; i++) {
        printf("%02X ", frame.data[i]);
    }
    printf("\n");
    
    // Decode specific messages
    if ((frame.can_id & CAN_EFF_MASK) == CAN_ID_KEYPAD && frame.can_dlc >= 2) {
        decodeKeypadButtons(frame.data);
    }
    else if ((frame.can_id & CAN_EFF_MASK) == CAN_ID_TSC1 && frame.can_dlc >= 4) {
        decodeTSC1(frame.data);
    }
    
    fflush(stdout);  // Ensure immediate output
    
    return 0;
}

int main(int argc, char *argv[]) {
    int sock_canfd1, sock_canfd2, sock_canfd3;
    fd_set read_fds;
    int max_fd;
    struct timeval timeout;
    //int bitrate = 250000;  // Default 250kbps, change as needed
    
    printf("CAN Bridge for RCU4 starting...\n");
    
    // Setup signal handler for clean shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Restart and configure CAN interfaces
    //printf("\nConfiguring CAN interfaces at %d bps...\n", bitrate);
    if (restart_can_interface("canfd1", 250000) < 0 ||
        restart_can_interface("canfd2", 500000) < 0 ||
        restart_can_interface("canfd3", 500000) < 0) {
        fprintf(stderr, "Failed to configure CAN interfaces\n");
        return 1;
    }
    
    // Small delay to let interfaces stabilize
    usleep(100000);  // 100ms
    
    printf("\nInitializing CAN sockets...\n");
    
    // Initialize CAN sockets
    sock_canfd1 = setup_can_socket("canfd1");
    sock_canfd2 = setup_can_socket("canfd2");
    sock_canfd3 = setup_can_socket("canfd3");
    
    if (sock_canfd1 < 0 || sock_canfd2 < 0 || sock_canfd3 < 0) {
        fprintf(stderr, "Failed to initialize CAN interfaces\n");
        return 1;
    }
    
    printf("All CAN interfaces initialized successfully\n");
    printf("Monitoring CAN messages (no forwarding)...\n");
    
    // Main loop - similar to Arduino loop()
    while (running) {
        // Prepare file descriptor set for select()
        FD_ZERO(&read_fds);
        FD_SET(sock_canfd1, &read_fds);
        FD_SET(sock_canfd2, &read_fds);
        FD_SET(sock_canfd3, &read_fds);
        
        // Find max file descriptor for select()
        max_fd = sock_canfd1;
        if (sock_canfd2 > max_fd) max_fd = sock_canfd2;
        if (sock_canfd3 > max_fd) max_fd = sock_canfd3;
        
        // Set timeout for select (allows periodic checking of running flag)
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        
        // Wait for activity on any socket
        int ret = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (ret < 0) {
            if (errno == EINTR) {
                continue;  // Interrupted by signal, check running flag
            }
            perror("select error");
            break;
        }
        
        if (ret == 0) {
            // Timeout - no activity
            continue;
        }
        
        // Check which socket has data and print received messages
        
        if (FD_ISSET(sock_canfd1, &read_fds)) {
            read_and_print_frame(sock_canfd1, "canfd1");
        }
        
        if (FD_ISSET(sock_canfd2, &read_fds)) {
            read_and_print_frame(sock_canfd2, "canfd2");
        }
        
        if (FD_ISSET(sock_canfd3, &read_fds)) {
            read_and_print_frame(sock_canfd3, "canfd3");
        }
    }
    
    // Cleanup
    printf("\nShutting down...\n");
    close(sock_canfd1);
    close(sock_canfd2);
    close(sock_canfd3);
    printf("CAN Bridge stopped\n");
    
    return 0;
}
