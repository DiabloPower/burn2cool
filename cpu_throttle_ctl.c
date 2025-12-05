#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <dirent.h>

#define SOCKET_PATH "/tmp/cpu_throttle.sock"

char* get_profile_dir() {
    static char profile_dir[512];
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    snprintf(profile_dir, sizeof(profile_dir), "%s/.config/cpu_throttle/profiles", home);
    return profile_dir;
}

void ensure_profile_dir() {
    char *dir = get_profile_dir();
    // Create ~/.config if needed
    char config_dir[512];
    const char *home = getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : "/tmp";
    }
    snprintf(config_dir, sizeof(config_dir), "%s/.config", home);
    mkdir(config_dir, 0755);
    
    // Create ~/.config/cpu_throttle if needed
    snprintf(config_dir, sizeof(config_dir), "%s/.config/cpu_throttle", home);
    mkdir(config_dir, 0755);
    
    // Create profiles directory
    mkdir(dir, 0755);
}

void print_help(const char *name) {
    printf("Usage: %s <command> [args]\n", name);
    printf("\nCommands:\n");
    printf("  set-safe-max <freq>    Set maximum frequency in kHz\n");
    printf("  set-safe-min <freq>    Set minimum frequency in kHz\n");
    printf("  set-temp-max <temp>    Set maximum temperature in °C\n");
    printf("  status                 Show current status\n");
    printf("  quit                   Shutdown cpu_throttle daemon\n");
    printf("\nProfile commands:\n");
    printf("  save-profile <name>    Save current settings to a profile\n");
    printf("  load-profile <name>    Load settings from a profile\n");
    printf("  list-profiles          List all saved profiles\n");
    printf("  delete-profile <name>  Delete a profile\n");
    printf("\nExamples:\n");
    printf("  %s set-safe-max 3000000\n", name);
    printf("  %s save-profile gaming\n", name);
    printf("  %s load-profile powersave\n", name);
    printf("  %s status\n", name);
    printf("\nProfiles are stored in: %s\n", get_profile_dir());
}

int send_command(const char *cmd) {
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: Cannot connect to cpu_throttle daemon.\n");
        fprintf(stderr, "Make sure cpu_throttle is running.\n");
        close(sock_fd);
        return -1;
    }

    if (send(sock_fd, cmd, strlen(cmd), 0) < 0) {
        perror("send");
        close(sock_fd);
        return -1;
    }

    char response[512];
    ssize_t n = recv(sock_fd, response, sizeof(response) - 1, 0);
    if (n > 0) {
        response[n] = '\0';
        printf("%s", response);
    }

    close(sock_fd);
    return 0;
}

void save_profile(const char *profile_name) {
    ensure_profile_dir();
    
    // Get current status from daemon
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Error: Cannot connect to daemon\n");
        close(sock_fd);
        return;
    }

    send(sock_fd, "status", 6, 0);
    char response[512];
    ssize_t n = recv(sock_fd, response, sizeof(response) - 1, 0);
    close(sock_fd);
    
    if (n <= 0) {
        fprintf(stderr, "Error: Failed to get status\n");
        return;
    }
    response[n] = '\0';
    
    // Parse status and save to profile
    char profile_path[512];
    snprintf(profile_path, sizeof(profile_path), "%s/%s.conf", get_profile_dir(), profile_name);
    
    FILE *fp = fopen(profile_path, "w");
    if (!fp) {
        fprintf(stderr, "Error: Cannot create profile file\n");
        return;
    }
    
    // Parse response and extract values
    int safe_min = 0, safe_max = 0, temp_max = 0;
    char *line = strtok(response, "\n");
    while (line) {
        if (sscanf(line, "safe_min: %d", &safe_min) == 1) {
            if (safe_min > 0) fprintf(fp, "safe_min=%d\n", safe_min);
        } else if (sscanf(line, "safe_max: %d", &safe_max) == 1) {
            if (safe_max > 0) fprintf(fp, "safe_max=%d\n", safe_max);
        } else if (sscanf(line, "temp_max: %d", &temp_max) == 1) {
            fprintf(fp, "temp_max=%d\n", temp_max);
        }
        line = strtok(NULL, "\n");
    }
    
    fclose(fp);
    printf("Profile '%s' saved to %s\n", profile_name, profile_path);
}

void load_profile(const char *profile_name) {
    char profile_path[512];
    snprintf(profile_path, sizeof(profile_path), "%s/%s.conf", get_profile_dir(), profile_name);
    
    FILE *fp = fopen(profile_path, "r");
    if (!fp) {
        fprintf(stderr, "Error: Profile '%s' not found\n", profile_name);
        return;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char key[64], value[64];
        if (sscanf(line, "%63[^=]=%63s", key, value) == 2) {
            char cmd[128];
            if (strcmp(key, "safe_min") == 0) {
                snprintf(cmd, sizeof(cmd), "set-safe-min %s", value);
                send_command(cmd);
            } else if (strcmp(key, "safe_max") == 0) {
                snprintf(cmd, sizeof(cmd), "set-safe-max %s", value);
                send_command(cmd);
            } else if (strcmp(key, "temp_max") == 0) {
                snprintf(cmd, sizeof(cmd), "set-temp-max %s", value);
                send_command(cmd);
            }
        }
    }
    fclose(fp);
    printf("Profile '%s' loaded\n", profile_name);
}

void list_profiles() {
    char *dir_path = get_profile_dir();
    DIR *dir = opendir(dir_path);
    if (!dir) {
        printf("No profiles found. Create one with 'save-profile <name>'\n");
        return;
    }
    
    printf("Available profiles:\n");
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        char *ext = strstr(entry->d_name, ".conf");
        if (ext && ext[5] == '\0') {
            *ext = '\0';  // Remove .conf extension
            printf("  - %s\n", entry->d_name);
            count++;
        }
    }
    closedir(dir);
    
    if (count == 0) {
        printf("  (none)\n");
    }
}

void delete_profile(const char *profile_name) {
    char profile_path[512];
    snprintf(profile_path, sizeof(profile_path), "%s/%s.conf", get_profile_dir(), profile_name);
    
    if (unlink(profile_path) == 0) {
        printf("Profile '%s' deleted\n", profile_name);
    } else {
        fprintf(stderr, "Error: Profile '%s' not found\n", profile_name);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_help(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help(argv[0]);
        return 0;
    }

    // Handle profile commands locally
    if (strcmp(argv[1], "save-profile") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Profile name required\n");
            return 1;
        }
        save_profile(argv[2]);
        return 0;
    } else if (strcmp(argv[1], "load-profile") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Profile name required\n");
            return 1;
        }
        load_profile(argv[2]);
        return 0;
    } else if (strcmp(argv[1], "list-profiles") == 0) {
        list_profiles();
        return 0;
    } else if (strcmp(argv[1], "delete-profile") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: Profile name required\n");
            return 1;
        }
        delete_profile(argv[2]);
        return 0;
    }

    // Build command string for daemon
    char cmd[256];
    if (argc == 2) {
        snprintf(cmd, sizeof(cmd), "%s", argv[1]);
    } else if (argc == 3) {
        snprintf(cmd, sizeof(cmd), "%s %s", argv[1], argv[2]);
    } else {
        fprintf(stderr, "Error: Too many arguments\n");
        print_help(argv[0]);
        return 1;
    }

    return send_command(cmd);
}
