#include <stdio.h>
#include <unistd.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <stdbool.h>

#include <sys/time.h>
#include <sys/sysctl.h>
#include <sys/ioctl.h>
#include <sys/sensors.h>
#include <machine/apmvar.h>
#include <locale.h>
#include <poll.h>

#include <net/if.h>
#include <ifaddrs.h>

#include <sndio.h>


static char battery_percent[32];
static char battery_power[16];
static char cpu_temp[32];
static char fan_speed[32];
static char cpu_base_speed[32];
static char cpu_avg_speed[32];
static char volume[32];
static char net_rx[8];
static char net_tx[8];
static char datetime[32];

static bool battery_onpower = false;
static int  battery_life = -1;
static int  cpu_temp_val = -1;
static bool color_mode = true;

struct vol_ctx {
    unsigned int addr;
    unsigned int maxval;
    unsigned int val;
    int found;
    unsigned int mute_addr;
    int muted;
    int mute_found;
};

static struct sioctl_hdl *vol_hdl;
static struct vol_ctx vol_state;

static void
vol_ondesc(void *arg, struct sioctl_desc *desc, int curval)
{
    struct vol_ctx *ctx = arg;
    if (desc == NULL)
        return;
    if (desc->type == SIOCTL_NUM &&
        strcmp(desc->func, "level") == 0 &&
        strcmp(desc->node0.name, "output") == 0) {
        ctx->addr = desc->addr;
        ctx->maxval = desc->maxval;
        ctx->val = (unsigned int)curval;
        ctx->found = 1;
    }
    if (desc->type == SIOCTL_SW &&
        strcmp(desc->func, "mute") == 0 &&
        strcmp(desc->node0.name, "output") == 0) {
        ctx->mute_addr = desc->addr;
        ctx->muted = curval;
        ctx->mute_found = 1;
    }
}

static void
vol_onval(void *arg, unsigned int addr, unsigned int val)
{
    struct vol_ctx *ctx = arg;
    if (ctx->found && addr == ctx->addr) {
        ctx->val = val;
        snprintf(volume, sizeof(volume), "%.0f%%", (val * 100.0) / ctx->maxval);
    }
    if (ctx->mute_found && addr == ctx->mute_addr)
        ctx->muted = val;
}

void update_cpu_base_speed() {
    int temp;
    size_t templen = sizeof(temp);

    int mib[5] = { CTL_HW, HW_CPUSPEED };

    if (sysctl(mib, 2, &temp, &templen, NULL, 0) == -1)
        snprintf(cpu_base_speed,sizeof(cpu_base_speed), "no_freq");
    else
        snprintf(cpu_base_speed,sizeof(cpu_base_speed), "%4dMhz", temp);
}

void update_cpu_avg_speed() {
    struct sensor sensor;
    size_t templen = sizeof(sensor);
    static int freq_mibs[24];
    static int freq_count = -1;

    if (freq_count == -1) {
        freq_count = 0;
        for (int i = 0; i < 24; i++) {
            int mib[5] = { CTL_HW, HW_SENSORS, i, SENSOR_FREQ, 0 };
            if (sysctl(mib, 5, &sensor, &templen, NULL, 0) != -1)
                freq_mibs[freq_count++] = i;
        }
    }

    if (freq_count == 0) {
        snprintf(cpu_avg_speed, sizeof(cpu_avg_speed), "N/A");
        return;
    }

    uint temp = 0;
    for (int i = 0; i < freq_count; i++) {
        int mib[5] = { CTL_HW, HW_SENSORS, freq_mibs[i], SENSOR_FREQ, 0 };
        if (sysctl(mib, 5, &sensor, &templen, NULL, 0) != -1)
            temp += sensor.value / 1000000 / 1000000;
    }
    snprintf(cpu_avg_speed, sizeof(cpu_avg_speed), "%4dMhz", temp / freq_count);
}

void update_fan_speed() {
    struct sensor sensor;
    size_t templen = sizeof(sensor);
    int temp = -1;
    static int fan_mib = -1;

    // grab first sensor that provides SENSOR_FANRPM
    if (fan_mib == -1) {
        for (fan_mib=0; fan_mib<20; fan_mib++) {
            int mib[5] = { CTL_HW, HW_SENSORS, fan_mib, SENSOR_FANRPM, 0 };
            if (sysctl(mib, 5, &sensor, &templen, NULL, 0) != -1)
                break;
        }
    }

    int mib[5] = { CTL_HW, HW_SENSORS, fan_mib, SENSOR_FANRPM, 0 };
    if (sysctl(mib, 5, &sensor, &templen, NULL, 0) != -1)
        temp = sensor.value;

    snprintf(fan_speed,sizeof(fan_speed), "%dRPM", temp);
}

void update_cpu_temp() {
    struct sensor sensor;
    size_t templen = sizeof(sensor);
    int temp = -1;

    static int temp_mib = -1;

    // grab first sensor that provides SENSOR_TEMP
    if (temp_mib == -1) {
        for (temp_mib=0; temp_mib<20; temp_mib++) {
            int mib[5] = { CTL_HW, HW_SENSORS, temp_mib, SENSOR_TEMP, 0 }; // acpitz0.temp0 (x13)
            if (sysctl(mib, 5, &sensor, &templen, NULL, 0) != -1)
                break;
        }
    }

    int mib[5] = { CTL_HW, HW_SENSORS, temp_mib, SENSOR_TEMP, 0 };
    if (sysctl(mib, 5, &sensor, &templen, NULL, 0) != -1)
        temp = (sensor.value - 273150000) / 1000000.0;

    cpu_temp_val = temp;
    snprintf(cpu_temp, sizeof(cpu_temp), "%d°C", temp);
}

void update_battery() {
    static int fd = -1;
    struct apm_power_info pi;

    if (fd == -1)
        fd = open("/dev/apm", O_RDONLY);
    if (fd == -1 || ioctl(fd, APM_IOC_GETPOWER, &pi) == -1) {
        if (fd != -1) { close(fd); fd = -1; }
        strlcpy(battery_percent, "N/A", sizeof(battery_percent));
        return;
    }

    if (pi.battery_state == APM_BATT_UNKNOWN ||
            pi.battery_state == APM_BATTERY_ABSENT) {
        strlcpy(battery_percent, "N/A", sizeof(battery_percent));
        return;
    }
    if(pi.ac_state == APM_AC_ON) {
        battery_onpower = true;
    } else {
        battery_onpower = false;
    }
    battery_life = pi.battery_life;
    snprintf(battery_percent, sizeof(battery_percent), "%d%%", pi.battery_life);

    static int pow_dev = -1;
    if (pow_dev == -1) {
        struct sensor s;
        size_t slen = sizeof(s);
        for (pow_dev = 0; pow_dev < 20; pow_dev++) {
            int m[5] = { CTL_HW, HW_SENSORS, pow_dev, SENSOR_WATTS, 0 };
            if (sysctl(m, 5, &s, &slen, NULL, 0) != -1)
                break;
        }
    }
    struct sensor ps;
    size_t pslen = sizeof(ps);
    int pm[5] = { CTL_HW, HW_SENSORS, pow_dev, SENSOR_WATTS, 0 };
    if (sysctl(pm, 5, &ps, &pslen, NULL, 0) != -1 &&
        !(ps.flags & (SENSOR_FINVALID | SENSOR_FUNKNOWN))) {
        int watts = (int)(ps.value / 1000000.0 + 0.5);
        if (watts > 0)
            snprintf(battery_power, sizeof(battery_power), "%dW", watts);
        else
            battery_power[0] = '\0';
    } else {
        battery_power[0] = '\0';
    }
}

static void
fmt_rate(char *buf, size_t len, uint64_t bytes)
{
    if (bytes >= 1000000)
        snprintf(buf, len, "%.0fM", bytes / 1000000.0);
    else if (bytes >= 1000)
        snprintf(buf, len, "%.0fK", bytes / 1000.0);
    else
        snprintf(buf, len, "%lluB", (unsigned long long)bytes);
}

void update_net() {
    static uint64_t prev_rx, prev_tx;
    static int init;
    struct ifaddrs *ifas, *ifa;
    uint64_t rx = 0, tx = 0;

    if (getifaddrs(&ifas) == -1) {
        strlcpy(net_rx, "N/A", sizeof(net_rx));
        strlcpy(net_tx, "N/A", sizeof(net_tx));
        return;
    }
    for (ifa = ifas; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_LINK)
            continue;
        if (strcmp(ifa->ifa_name, "trunk0") != 0)
            continue;
        struct if_data *ifd = (struct if_data *)ifa->ifa_data;
        rx = ifd->ifi_ibytes;
        tx = ifd->ifi_obytes;
        break;
    }
    freeifaddrs(ifas);

    if (!init) {
        prev_rx = rx;
        prev_tx = tx;
        init = 1;
        strlcpy(net_rx, "0B", sizeof(net_rx));
        strlcpy(net_tx, "0B", sizeof(net_tx));
        return;
    }

    fmt_rate(net_rx, sizeof(net_rx), rx - prev_rx);
    fmt_rate(net_tx, sizeof(net_tx), tx - prev_tx);
    prev_rx = rx;
    prev_tx = tx;
}

void update_datetime() {
    time_t rawtime;
    struct tm * timeinfo;
    time ( &rawtime );
    timeinfo = localtime ( &rawtime );
    strftime(datetime,sizeof(datetime),"%d %b %Y %H:%M", timeinfo);
}

static void
print_status(wchar_t ico_time, wchar_t ico_fire, wchar_t ico_tacho,
    wchar_t ico_temp, wchar_t ico_volume, wchar_t ico_rx, wchar_t ico_tx)
{
    wchar_t ico_battery = battery_onpower ? (wchar_t)0xF1E6 : (wchar_t)0xF240;

    if (color_mode && battery_life >= 0 && battery_life < 10)
        printf("+@fg=1;");
    else if (color_mode && battery_life >= 0 && battery_life < 20)
        printf("+@fg=2;");
    printf(" %lc ", ico_battery);
    printf("%s ", battery_percent);
    if (battery_power[0])
        printf("(%s) ", battery_power);
    if (color_mode && battery_life >= 0 && battery_life < 20)
        printf("+@fg=0;");

    if (color_mode && cpu_temp_val >= 78)
        printf("+@fg=1;");
    else if (color_mode && cpu_temp_val >= 68)
        printf("+@fg=2;");
    printf("%lc ", ico_temp);
    printf("%s ", cpu_temp);
    if (color_mode && cpu_temp_val >= 68)
        printf("+@fg=0;");

    printf("%lc ", ico_fire);
    printf("%s ", cpu_avg_speed);

    printf("%lc ", ico_tacho);
    printf("%s ", fan_speed);

    printf("%lc %4s/s", ico_rx, net_rx);
    printf(" %lc %4s/s ", ico_tx, net_tx);

    wchar_t ico_vol = (vol_state.muted || (vol_state.found && vol_state.val == 0))
        ? (wchar_t)0xF026   /* volume-off */
        : ico_volume;
    printf("%lc ", ico_vol);
    printf("%s ", volume);

    printf("%lc ", ico_time);
    printf("%s", datetime);

    printf("\n");
    fflush(stdout);
}

int main(int argc, const char *argv[])
{
    setlocale(LC_CTYPE, "C");
    setlocale(LC_ALL, "en_US.UTF-8");

    const wchar_t ico_time   = 0xF455;
    const wchar_t ico_fire   = 0xF2DB;
    const wchar_t ico_tacho  = 0xF0E4;
    const wchar_t ico_temp   = 0xF2C7;
    const wchar_t ico_volume = 0xF028;
    const wchar_t ico_rx     = 0xF063;  /* fa-arrow-down */
    const wchar_t ico_tx     = 0xF062;  /* fa-arrow-up */

    int one_shot = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp("-1", argv[i]) == 0) one_shot = 1;
        if (strcmp("-m", argv[i]) == 0) color_mode = false;
    }

    strlcpy(volume, "N/A", sizeof(volume));
    vol_hdl = sioctl_open(SIO_DEVANY, SIOCTL_READ, 0);
    if (vol_hdl != NULL) {
        sioctl_ondesc(vol_hdl, vol_ondesc, &vol_state);
        sioctl_onval(vol_hdl, vol_onval, &vol_state);
        /* vol_ondesc sets val from curval synchronously; compute string now */
        if (vol_state.found && vol_state.maxval > 0)
            snprintf(volume, sizeof(volume), "%.0f%%",
                (vol_state.val * 100.0) / vol_state.maxval);
    }

    update_battery();
    update_cpu_temp();
    update_cpu_avg_speed();
    update_cpu_base_speed();
    update_fan_speed();
    update_net();
    update_datetime();
    print_status(ico_time, ico_fire, ico_tacho, ico_temp, ico_volume, ico_rx, ico_tx);

    if (one_shot) {
        if (vol_hdl != NULL)
            sioctl_close(vol_hdl);
        return 0;
    }

    for (;;) {
        struct pollfd pfds[8];
        int nfds = 0, revents, ret;

        if (vol_hdl != NULL)
            nfds = sioctl_pollfd(vol_hdl, pfds, POLLIN);
        ret = poll(pfds, nfds, 1000);

        if (ret > 0 && vol_hdl != NULL) {
            /* Early wakeup: volume changed — fire vol_onval, refresh time */
            revents = sioctl_revents(vol_hdl, pfds);
            if (revents & POLLHUP) {
                sioctl_close(vol_hdl);
                vol_hdl = NULL;
                memset(&vol_state, 0, sizeof(vol_state));
                strlcpy(volume, "N/A", sizeof(volume));
            }
            update_datetime();
        } else {
            /* Timeout: full update */
            if (vol_hdl == NULL) {
                vol_hdl = sioctl_open(SIO_DEVANY, SIOCTL_READ, 0);
                if (vol_hdl != NULL) {
                    sioctl_ondesc(vol_hdl, vol_ondesc, &vol_state);
                    sioctl_onval(vol_hdl, vol_onval, &vol_state);
                    if (vol_state.found && vol_state.maxval > 0)
                        snprintf(volume, sizeof(volume), "%.0f%%",
                            (vol_state.val * 100.0) / vol_state.maxval);
                }
            }
            update_battery();
            update_cpu_temp();
            update_cpu_avg_speed();
            update_cpu_base_speed();
            update_fan_speed();
            update_net();
            update_datetime();
        }

        print_status(ico_time, ico_fire, ico_tacho, ico_temp, ico_volume, ico_rx, ico_tx);
    }
    return 0;
}
