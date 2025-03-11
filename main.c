#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <linux/ioctl.h>
#include <sys/ioctl.h>
#include <pthread.h>

#define DEVICE_PATH "/dev/adxl345-0"
#define READ_COUNT 10
#define ADXL345_TYPE 'A'
#define ADXL345_READ  _IOR(ADXL345_TYPE, 2, int)
#define ADXL345_WRITE _IOW(ADXL345_TYPE, 3, int)

typedef struct {
    int fd;
    char axis;
} AxisThreadData;

static void* affiche_axe(void* arg) {
    for (int i = 0 ; i < READ_COUNT ; i++) {
        int fd = ((AxisThreadData *) arg)->fd;
        char axe = ((AxisThreadData *) arg)->axis;
        unsigned long arg_ioctl = axe;
        __int16_t nombre;
        ioctl(fd, ADXL345_WRITE, &arg_ioctl);
        read(fd, &nombre, sizeof(nombre));
        arg_ioctl = 0;
        ioctl(fd, ADXL345_READ, &arg_ioctl);
        axe = (char) arg_ioctl;
        printf("Lecture %c : %" PRId16 "\n", axe, nombre);
    }
    return NULL;
}

int main() {
    int fd1 = open(DEVICE_PATH, O_RDONLY);
    int fd2 = open(DEVICE_PATH, O_RDONLY);
    int fd3 = open(DEVICE_PATH, O_RDONLY);

    AxisThreadData dataX = { fd1, 'X' };
    AxisThreadData dataY = { fd2, 'Y' };
    AxisThreadData dataZ = { fd3, 'Z' };

    pthread_t thX, thY, thZ;
    pthread_create(&thX, NULL, affiche_axe, &dataX);
    pthread_create(&thY, NULL, affiche_axe, &dataY);
    pthread_create(&thZ, NULL, affiche_axe, &dataZ);
    pthread_join(thX, NULL);
    pthread_join(thY, NULL);
    pthread_join(thZ, NULL);

    close(fd1);
    close(fd2);
    close(fd3);

    return 0;
}

