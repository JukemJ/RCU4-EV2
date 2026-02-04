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

// Global flag for clean shutdown
volatile sig_atomic_t running = 1;

void signal_handler(int signum) {
    running = 0;
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

// Forward a CAN frame from source to destination socket
int forward_frame(int src_sock, int dst_sock, const char* src_name, const char* dst_name) {
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
    printf("[RX %s] ID=0x%03X DLC=%d Data: ", src_name, frame.can_id & CAN_EFF_MASK, frame.can_dlc);
    for (int i = 0; i < frame.can_dlc; i++) {
        printf("%02X ", frame.data[i]);
    }
    printf("\n");
    fflush(stdout);  // Ensure immediate output
    
    // Write frame to destination (if destination socket is valid)
    if (dst_sock >= 0) {
        nbytes = write(dst_sock, &frame, sizeof(struct can_frame));
        if (nbytes < 0) {
            // Don't exit on write error - destination interface may be down
            fprintf(stderr, "    -> Error forwarding to %s: %s\n", dst_name, strerror(errno));
            return -1;
        }
        
        if (nbytes < (int)sizeof(struct can_frame)) {
            fprintf(stderr, "    -> Incomplete frame sent to %s\n", dst_name);
            return -1;
        }
        
        printf("    -> Forwarded to %s\n", dst_name);
    }
    
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
    printf("\nConfiguring CAN interfaces at %d bps...\n", bitrate);
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
    printf("Starting message routing...\n");
    
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
        
        // Check which socket has data and forward accordingly
        // TODO: Add your specific routing logic here
        // For now, this is a simple example routing pattern
        
        if (FD_ISSET(sock_canfd1, &read_fds)) {
            // Example: Forward from canfd1 to canfd2
            forward_frame(sock_canfd1, sock_canfd2, "canfd1", "canfd2");
        }
        
        if (FD_ISSET(sock_canfd2, &read_fds)) {
            // Example: Forward from canfd2 to canfd3
            forward_frame(sock_canfd2, sock_canfd3, "canfd2", "canfd3");
        }
        
        if (FD_ISSET(sock_canfd3, &read_fds)) {
            // Example: Forward from canfd3 to canfd1
            forward_frame(sock_canfd3, sock_canfd1, "canfd3", "canfd1");
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
