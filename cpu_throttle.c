#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>

#define CPUFREQ_PATH "/sys/devices/system/cpu"

char temp_path[256] = "/sys/class/thermal/thermal_zone0/temp";
int dry_run = 0;
FILE *logfile = NULL;

int read_temp() {
    FILE *fp = fopen(temp_path, "r");
    if (!fp) return -1;
    int temp_raw;
    fscanf(fp, "%d", &temp_raw);
    fclose(fp);
    return temp_raw / 1000;
}

int read_freq_value(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int val;
    fscanf(fp, "%d", &val);
    fclose(fp);
    return val;
}

void set_max_freq_all_cpus(int freq) {
    DIR *dir = opendir(CPUFREQ_PATH);
    struct dirent *entry;
    if (!dir) return;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "cpu", 3) == 0 && isdigit(entry->d_name[3])) {
            char path[256];
            snprintf(path, sizeof(path),
                     "%s/%s/cpufreq/scaling_max_freq", CPUFREQ_PATH, entry->d_name);
            if (!dry_run) {
                FILE *fp = fopen(path, "w");
                if (fp) {
                    fprintf(fp, "%d", freq);
                    fclose(fp);
                }
            }
        }
    }
    closedir(dir);
}

int clamp(int val, int min, int max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

void print_help(const char *name) {
    printf("Usage: %s [OPTIONS]\n", name);
    printf("  --dry-run            Simulate frequency setting (no writes)\n");
    printf("  --log <path>         Append log messages to a file\n");
    printf("  --sensor <path>      Manually specify temp sensor file\n");
    printf("  --help               Show this help message\n");
}

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            logfile = fopen(argv[++i], "a");
            if (!logfile) {
                fprintf(stderr, "Error: Cannot open log file: %s\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--sensor") == 0 && i + 1 < argc) {
            strncpy(temp_path, argv[++i], sizeof(temp_path) - 1);
            temp_path[sizeof(temp_path) - 1] = '\0';
        } else if (strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_help(argv[0]);
            return 1;
        }
    }

    int last_freq = 0;

    char cpu0_path[256];
    snprintf(cpu0_path, sizeof(cpu0_path), "%s/cpu0/cpufreq", CPUFREQ_PATH);

    char min_path[256], max_path[256];
    snprintf(min_path, sizeof(min_path), "%s/cpuinfo_min_freq", cpu0_path);
    snprintf(max_path, sizeof(max_path), "%s/cpuinfo_max_freq", cpu0_path);

    int min_freq = read_freq_value(min_path);
    int max_freq = read_freq_value(max_path);

    if (min_freq <= 0 || max_freq <= 0) {
        fprintf(stderr, "Failed to read min/max CPU frequency\n");
        return 1;
    }

    while (1) {
        int temp = read_temp();
        if (temp < 0) {
            fprintf(stderr, "Failed to read CPU temperature\n");
            return 1;
        }

        temp = clamp(temp, 70, 95);
        int scale = 95 - temp;
        int freq = min_freq + (scale * (max_freq - min_freq)) / 25;

        if (freq != last_freq) {
            set_max_freq_all_cpus(freq);
            printf("Temp: %d°C → MaxFreq: %d kHz%s\n",
                   temp, freq, dry_run ? " [DRY-RUN]" : "");
            if (logfile) {
                fprintf(logfile, "Temp: %d°C → MaxFreq: %d kHz\n", temp, freq);
                fflush(logfile);
            }
            last_freq = freq;
        }

        sleep(1);
    }

    if (logfile) fclose(logfile);
    return 0;
}
