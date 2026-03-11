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

#include <sndio.h>


static char battery_percent[32];
static char cpu_temp[32];
static char fan_speed[32];
static char cpu_base_speed[32];
static char cpu_avg_speed[32];
static char volume[32];
static char datetime[32];

static bool battery_onpower = false;

struct vol_ctx {
    unsigned int maxval;
    unsigned int val;
    int found;
};

static void
vol_ondesc(void *arg, struct sioctl_desc *desc, int curval)
{
    struct vol_ctx *ctx = arg;
    if (desc == NULL || ctx->found)
        return;
    if (desc->type == SIOCTL_NUM &&
        strcmp(desc->func, "level") == 0 &&
        strcmp(desc->node0.name, "output") == 0) {
        ctx->maxval = desc->maxval;
        ctx->val = (unsigned int)curval;
        ctx->found = 1;
    }
}

void update_volume() {
    struct sioctl_hdl *hdl;
    struct vol_ctx ctx = {0};

    hdl = sioctl_open(SIO_DEVANY, SIOCTL_READ, 0);
    if (hdl == NULL) {
        snprintf(volume, sizeof(volume), "N/A");
        return;
    }
    sioctl_ondesc(hdl, vol_ondesc, &ctx);
    sioctl_close(hdl);

    if (!ctx.found || ctx.maxval == 0) {
        snprintf(volume, sizeof(volume), "N/A");
        return;
    }
    snprintf(volume, sizeof(volume), "%.0f%%",
        (ctx.val * 100.0) / ctx.maxval);
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
    int count = 0;
    uint temp = 0;

    int i;
    for (i = 0; i < 24; i++) {

        int mib[5] = { CTL_HW, HW_SENSORS, i, SENSOR_FREQ, 0 };

        if (sysctl(mib, 5, &sensor, &templen, NULL, 0) != -1) {
            count++;
            temp += ( sensor.value / 1000000 / 1000000 );
        }
    }
    if (count == 0)
        snprintf(cpu_avg_speed, sizeof(cpu_avg_speed), "N/A");
    else
        snprintf(cpu_avg_speed,sizeof(cpu_avg_speed), "%4dMhz", temp / count);
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
    if (sysctl(mib, 5, &sensor, &templen, NULL, 0) != -1) {
        temp = (sensor.value  - 273150000) / 1000000.0;
    }

    snprintf(cpu_temp,sizeof(cpu_temp), "%d°C", temp);
}

void update_battery() {
    int fd;
    struct apm_power_info pi;

    if ((fd = open("/dev/apm", O_RDONLY)) == -1 ||
            ioctl(fd, APM_IOC_GETPOWER, &pi) == -1 ||
            close(fd) == -1) {
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
    snprintf(battery_percent,sizeof(battery_percent),
        "%d%%", pi.battery_life);
}
void update_datetime() {
    time_t rawtime;
    struct tm * timeinfo;
    time ( &rawtime );
    timeinfo = localtime ( &rawtime );
    strftime(datetime,sizeof(datetime),"%d %b %Y %H:%M", timeinfo);
}

int main(int argc, const char *argv[])
{

    setlocale(LC_CTYPE, "C");
    setlocale(LC_ALL, "en_US.UTF-8");

    //const wchar_t sep =  0xE621; // 
    //const char sep =  '|';
    const wchar_t ico_time = 0xF455; // 
    const wchar_t ico_fire = 0xF2DB; //  
    const wchar_t ico_tacho = 0xF0E4; // 
    const wchar_t ico_temp = 0xF2C7; // 
    const wchar_t ico_volume = 0xF028; // 

    wchar_t ico_battery;


    while(1) {

        update_battery();
        update_cpu_temp();
        update_cpu_avg_speed();
        update_cpu_base_speed();
        update_fan_speed();
        update_volume();
        update_datetime();

        if(battery_onpower) {
            ico_battery = 0xF1E6; // 
        } else {
            ico_battery = 0xF240; // 
        }

        printf(" %lc ", ico_battery);
        printf("%s ", battery_percent);

        printf(" %lc ", ico_temp);
        printf("%s ", cpu_temp);

        printf(" %lc ", ico_fire);
        printf("%s ", cpu_avg_speed);

        printf(" %lc ", ico_tacho);
        printf("%s ", fan_speed);

        printf(" %lc ", ico_volume);
        printf("%s ", volume);

        printf(" %lc ", ico_time);
        printf("%s", datetime);

        printf("\n");

        fflush(stdout);
        if(argc == 2)
           if(strcmp("-1", argv[1]) == 0)
                return 0;
        usleep(1000000);
    }
    return 0;
}
